#include "CssParser.h"

#include <Arduino.h>
#include <Arena.h>
#include <ArenaVector.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <string_view>

#include "../../../EpdFont/GenericFontFamily.h"
#include "CssDeclarationExtras.h"
#include "CssMedia.h"
#include "CssStyleExtrasCodec.h"

namespace {

// Cold, bounded parsing scratch lives off the 4KiB firmware task stack. Both
// allocations are fallible and released together before layout begins.
struct StackBuffer {
  std::unique_ptr<char[]> data;
  size_t capacity, len = 0;
  bool truncated = false;
  explicit StackBuffer(size_t bytes = 1024) : data(makeUniqueNoThrow<char[]>(bytes)), capacity(bytes) {}
  void push_back(char c) {
    if (len < capacity)
      data[len++] = c;
    else
      truncated = true;
  }
  void clear() {
    len = 0;
    truncated = false;
  }
  bool empty() const { return len == 0; }
  size_t size() const { return len; }
  std::string_view view() const { return {data.get(), len}; }
  operator std::string_view() const noexcept { return view(); }
};

// Buffer size for reading CSS files
constexpr size_t READ_BUFFER_SIZE = 512;

// Maximum number of CSS rules to store in the selector map
// Prevents unbounded memory growth from pathological CSS files
constexpr size_t MAX_RULES = 1500;
constexpr size_t MAX_FONT_FACES = 32;
constexpr size_t MAX_FONT_FACE_HREF = 255;

// Maximum number of two-part descendant rules (ancestor subject) to store
constexpr size_t MAX_DESCENDANT_RULES = CssParser::MAX_DESCENDANT_RULES;

// Growing the selector containers uses throwing STL allocators. With firmware
// exceptions disabled, allocation failure aborts instead of returning an
// error, so stop early and let the caller persist a usable partial CSS cache.
constexpr size_t MIN_FREE_HEAP_FOR_RULE_GROWTH = 64 * 1024;
constexpr size_t MIN_LARGEST_BLOCK_FOR_RULE_GROWTH = 8 * 1024;

// Minimum free heap required to apply CSS during rendering
// If below this threshold, we skip CSS to avoid display artifacts.
constexpr size_t MIN_FREE_HEAP_FOR_CSS = 48 * 1024;

// Hydrating cached CSS rules is an optimization, not a requirement. Keep a
// larger heap floor than basic CSS application so low-memory books can still
// fall back to the disk-backed selector index.
constexpr size_t MIN_FREE_HEAP_FOR_CSS_RULE_ARENA = 96 * 1024;
constexpr size_t CSS_RULE_ARENA_MIN_FREE_AFTER_ALLOC = 80 * 1024;
constexpr size_t CSS_RULE_ARENA_EXTRA_BYTES = 1024;

// Maximum length for a single selector string
// Prevents parsing of extremely long or malformed selectors
constexpr size_t MAX_SELECTOR_LENGTH = 256;
constexpr size_t CSS_LENGTH_FIELD_COUNT = 13;
constexpr size_t CSS_LENGTH_BYTES = sizeof(float) + sizeof(uint8_t);
constexpr size_t CSS_FIXED_STYLE_BYTES = 6 * sizeof(uint8_t) + (CSS_LENGTH_FIELD_COUNT * CSS_LENGTH_BYTES) +
                                         4 * sizeof(uint8_t) + 19 * sizeof(uint8_t) + 2 * sizeof(uint32_t) +
                                         cssExtrasCodec::Bytes;
static_assert(CSS_FIXED_STYLE_BYTES == 149,
              "CssStyle cache payload changed; update read/writeCssStylePayload and bump CSS_CACHE_VERSION");

// Check if character is CSS whitespace
constexpr bool isCssWhitespace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

constexpr std::string_view trimCssWhitespace(std::string_view s) {
  while (!s.empty() && isCssWhitespace(s.front())) s.remove_prefix(1);
  while (!s.empty() && isCssWhitespace(s.back())) s.remove_suffix(1);
  return s;
}

constexpr char asciiToLower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// Case-insensitive equality on ASCII. lowercaseKeyword MUST already be
// lowercase; CSS keywords are ASCII by spec so byte-wise tolower is safe.
constexpr bool iequalsAscii(std::string_view value, std::string_view lowercaseKeyword) {
  return std::equal(value.begin(), value.end(), lowercaseKeyword.begin(), lowercaseKeyword.end(),
                    [](char a, char b) { return asciiToLower(a) == b; });
}

// Walk s and invoke fn(token) for each non-empty run between delimiters.
// Tokens are boundary-trimmed and yielded as string_views into s; no
// allocation. Runs of consecutive delimiters coalesce — no empty tokens are
// emitted. `isDelimiter` is invoked once per character.
template <typename Pred, typename F>
void forEachDelimitedToken(std::string_view s, Pred isDelimiter, F&& fn) {
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || isDelimiter(s[i])) {
      const std::string_view trimmed = trimCssWhitespace(s.substr(start, i - start));
      if (!trimmed.empty()) {
        fn(trimmed);
      }
      start = i + 1;
    }
  }
}

constexpr std::string_view unquoteCss(std::string_view s) {
  s = trimCssWhitespace(s);
  if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) {
    s = trimCssWhitespace(s.substr(1, s.size() - 2));
  }
  return s;
}

// Named publisher families retain their existing lookup. Unquoted generic
// families carry explicit tokens so they can select native reader families.
uint32_t firstFontFamily(const std::string_view value) {
  const size_t comma = value.find(',');
  const auto token = trimCssWhitespace(comma == std::string_view::npos ? value : value.substr(0, comma));
  const bool quoted = !token.empty() && (token.front() == '\'' || token.front() == '"');
  const std::string_view name = unquoteCss(token);
  if (name.empty()) return 0;
  if (!quoted) {
    if (iequalsAscii(name, "serif")) return genericFont::Serif;
    if (iequalsAscii(name, "sans-serif") || iequalsAscii(name, "system-ui")) return genericFont::SansSerif;
    constexpr std::string_view ignored[] = {"monospace", "cursive", "fantasy", "inherit", "initial", "unset"};
    for (const auto keyword : ignored)
      if (iequalsAscii(name, keyword)) return 0;
  }
  return CssParser::hashFontFamily(name);
}

// FNV-1a per Fowler/Noll/Vo, sized to match size_t on the target. The firmware
// runs on a 32-bit core where size_t is 32 bits, so naively using the 64-bit
// constants would silently truncate FNV_PRIME to a non-prime and wreck hash
// distribution. The selection below picks the canonical 32- or 64-bit
// constants at compile time so the same source works in a 64-bit host
// simulator. `fnv1aMix` is the per-byte mix step; callers apply any
// byte-level transform (e.g. asciiToLower) first.
static_assert(sizeof(size_t) == 4 || sizeof(size_t) == 8, "FNV constants are only defined for 32- or 64-bit size_t");
constexpr size_t FNV_OFFSET_BASIS =
    sizeof(size_t) == 8 ? static_cast<size_t>(14695981039346656037ULL) : static_cast<size_t>(2166136261U);
constexpr size_t FNV_PRIME =
    sizeof(size_t) == 8 ? static_cast<size_t>(1099511628211ULL) : static_cast<size_t>(16777619U);

constexpr size_t fnv1aMix(size_t hash, unsigned char byte) { return (hash ^ byte) * FNV_PRIME; }

// Parse the entirety of s as a number into `out`. Accepts an optional leading
// '+' (which std::from_chars rejects by spec) so callers can pass CSS-style
// signed numbers without manual trimming. Returns false on empty input, a
// non-numeric suffix, or any from_chars error.
template <typename T>
bool tryParseNumber(std::string_view s, T& out) {
  const char* begin = s.data();
  const char* end = s.data() + s.size();
  if (begin < end && *begin == '+') ++begin;
  const auto r = std::from_chars(begin, end, out);
  return r.ec == std::errc{} && r.ptr == end;
}

// Collect up to 4 whitespace-separated tokens for a CSS edge-value shorthand
// (margin, padding, and the border-* family). Returns the number of tokens
// written; extras are silently dropped. Callers apply the 1/2/3/4-value
// fallback rule using the returned count.
size_t collectEdgeValueTokens(std::string_view s, std::string_view (&out)[4]) {
  size_t count = 0;
  forEachDelimitedToken(s, isCssWhitespace, [&](std::string_view tok) {
    if (count < 4) out[count++] = tok;
  });
  return count;
}

std::string_view stripTrailingImportant(std::string_view value) {
  constexpr std::string_view IMPORTANT = "!important";

  while (!value.empty() && isCssWhitespace(value.back())) {
    value.remove_suffix(1);
  }

  if (value.size() < IMPORTANT.size()) {
    return value;
  }

  const size_t suffixPos = value.size() - IMPORTANT.size();
  if (!iequalsAscii(value.substr(suffixPos), IMPORTANT)) {
    return value;
  }

  value.remove_suffix(IMPORTANT.size());
  while (!value.empty() && isCssWhitespace(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

bool tryInterpretCssPageBreak(std::string_view value, bool& out) {
  value = trimCssWhitespace(stripTrailingImportant(value));
  if (iequalsAscii(value, "always") || iequalsAscii(value, "page") || iequalsAscii(value, "left") ||
      iequalsAscii(value, "right")) {
    out = true;
    return true;
  }
  if (iequalsAscii(value, "auto") || iequalsAscii(value, "avoid") || iequalsAscii(value, "avoid-page")) {
    out = false;
    return true;
  }
  return false;
}

bool tryInterpretBackgroundBlack(std::string_view value, bool& out) {
  value = stripTrailingImportant(value);
  value = trimCssWhitespace(value);

  if (value.empty()) {
    return false;
  }

  bool sawExplicitNonBlack = false;
  size_t tokenStart = 0;
  for (size_t i = 0; i <= value.size(); ++i) {
    if (i == value.size() || isCssWhitespace(value[i])) {
      if (i > tokenStart) {
        const std::string_view token = value.substr(tokenStart, i - tokenStart);
        if (iequalsAscii(token, "black") || token == "#000" || token == "#000000") {
          out = true;
          return true;
        }
        if (iequalsAscii(token, "white") || token == "#fff" || token == "#ffffff" ||
            iequalsAscii(token, "transparent") || iequalsAscii(token, "none")) {
          sawExplicitNonBlack = true;
        }
      }
      tokenStart = i + 1;
    }
  }

  std::string compact;
  compact.reserve(value.size());
  for (const char c : value) {
    if (!isCssWhitespace(c)) {
      compact.push_back(asciiToLower(c));
    }
  }

  if (compact == "rgb(0,0,0)" || compact == "rgba(0,0,0,1)" || compact == "rgba(0,0,0,1.0)" ||
      compact.find("rgb(0,0,0)") != std::string::npos || compact.find("rgba(0,0,0,1)") != std::string::npos ||
      compact.find("rgba(0,0,0,1.0)") != std::string::npos) {
    out = true;
    return true;
  }

  if (sawExplicitNonBlack || compact == "transparent" || compact == "none") {
    out = false;
    return true;
  }

  return false;
}

}  // anonymous namespace

// Transparent case-insensitive hash/equal. Bodies live here (rather than
// inline in the header) so they can share the anonymous-namespace asciiToLower
// with the other ASCII helpers in this translation unit.

size_t CssParser::SvHash::operator()(std::string_view sv) const noexcept {
  size_t h = FNV_OFFSET_BASIS;
  for (char c : sv) h = fnv1aMix(h, asciiToLower(c));
  return h;
}

size_t CssParser::SvHash::operator()(const std::string& s) const noexcept { return operator()(std::string_view(s)); }

size_t CssParser::SvHash::operator()(CompositeKey k) const noexcept {
  // Hash the case-folded concatenation of every piece without materializing
  // it — the running hash continues across pieces as if they were one buffer.
  size_t h = FNV_OFFSET_BASIS;
  for (std::string_view piece : k.pieces) {
    for (char c : piece) h = fnv1aMix(h, asciiToLower(c));
  }
  return h;
}

bool CssParser::SvEqual::operator()(std::string_view a, std::string_view b) const noexcept {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (asciiToLower(a[i]) != asciiToLower(b[i])) return false;
  }
  return true;
}

bool CssParser::SvEqual::operator()(const std::string& a, std::string_view b) const noexcept {
  return operator()(std::string_view(a), b);
}

bool CssParser::SvEqual::operator()(std::string_view a, const std::string& b) const noexcept {
  return operator()(a, std::string_view(b));
}

bool CssParser::SvEqual::operator()(const std::string& a, const std::string& b) const noexcept {
  return operator()(std::string_view(a), std::string_view(b));
}

bool CssParser::SvEqual::operator()(CompositeKey k, std::string_view sv) const noexcept {
  size_t total = 0;
  for (std::string_view piece : k.pieces) total += piece.size();
  if (total != sv.size()) return false;
  size_t i = 0;
  for (std::string_view piece : k.pieces) {
    for (char c : piece) {
      if (asciiToLower(c) != asciiToLower(sv[i++])) return false;
    }
  }
  return true;
}

bool CssParser::SvEqual::operator()(std::string_view sv, CompositeKey k) const noexcept { return operator()(k, sv); }

// Property value interpreters

CssTextAlign CssParser::interpretAlignment(std::string_view val) {
  val = trimCssWhitespace(val);

  if (iequalsAscii(val, "left") || iequalsAscii(val, "start")) return CssTextAlign::Left;
  if (iequalsAscii(val, "right") || iequalsAscii(val, "end")) return CssTextAlign::Right;
  if (iequalsAscii(val, "center")) return CssTextAlign::Center;
  if (iequalsAscii(val, "justify")) return CssTextAlign::Justify;

  return CssTextAlign::Left;
}

CssFontStyle CssParser::interpretFontStyle(std::string_view val) {
  val = trimCssWhitespace(val);

  if (iequalsAscii(val, "italic") || iequalsAscii(val, "oblique")) return CssFontStyle::Italic;
  return CssFontStyle::Normal;
}

CssFontWeight CssParser::interpretFontWeight(std::string_view val) {
  val = trimCssWhitespace(val);

  // Named values
  if (iequalsAscii(val, "bold") || iequalsAscii(val, "bolder")) return CssFontWeight::Bold;
  if (iequalsAscii(val, "normal") || iequalsAscii(val, "lighter")) return CssFontWeight::Normal;

  // Numeric values: 100-900
  // CSS spec: 400 = normal, 700 = bold
  // We use: 0-400 = normal, 700+ = bold, 500-600 = normal (conservative)
  long numericWeight = 0;
  if (tryParseNumber(val, numericWeight)) {
    return numericWeight >= 700 ? CssFontWeight::Bold : CssFontWeight::Normal;
  }
  return CssFontWeight::Normal;
}

CssFontVariantCaps CssParser::interpretFontVariantCaps(std::string_view value) {
  value = trimCssWhitespace(stripTrailingImportant(value));
  if (iequalsAscii(value, "all-small-caps")) return CssFontVariantCaps::AllSmallCaps;
  if (iequalsAscii(value, "small-caps")) return CssFontVariantCaps::SmallCaps;
  if (iequalsAscii(value, "inherit") || iequalsAscii(value, "unset")) return CssFontVariantCaps::Inherit;
  return CssFontVariantCaps::Normal;
}

CssTextDecoration CssParser::interpretDecoration(std::string_view val) {
  // text-decoration can have multiple space-separated values. Compare whole tokens
  // so malformed values like "notunderline" do not accidentally enable a line.
  CssTextDecoration result = CssTextDecoration::None;
  bool explicitNone = false;
  forEachDelimitedToken(val, isCssWhitespace, [&](const std::string_view token) {
    if (iequalsAscii(token, "none")) {
      explicitNone = true;
    } else if (iequalsAscii(token, "underline")) {
      result = result | CssTextDecoration::Underline;
    } else if (iequalsAscii(token, "line-through")) {
      result = result | CssTextDecoration::LineThrough;
    }
  });
  return explicitNone ? CssTextDecoration::None : result;
}

CssLength CssParser::interpretLength(std::string_view val) {
  CssLength result;
  tryInterpretLength(val, result);
  return result;
}

bool CssParser::tryInterpretLength(std::string_view val, CssLength& out) {
  val = trimCssWhitespace(val);
  if (val.empty()) {
    out = CssLength{};
    return false;
  }

  size_t unitStart = val.size();
  for (size_t i = 0; i < val.size(); ++i) {
    const char c = val[i];
    if (!std::isdigit(c) && c != '.' && c != '-' && c != '+') {
      unitStart = i;
      break;
    }
  }

  float numericValue;
  if (!tryParseNumber(val.substr(0, unitStart), numericValue)) {
    out = CssLength{};
    return false;  // No number parsed (e.g. auto, inherit, initial)
  }

  const std::string_view unitPart = val.substr(unitStart);
  auto unit = CssUnit::Pixels;
  if (iequalsAscii(unitPart, "em")) {
    unit = CssUnit::Em;
  } else if (iequalsAscii(unitPart, "rem")) {
    unit = CssUnit::Rem;
  } else if (iequalsAscii(unitPart, "pt")) {
    unit = CssUnit::Points;
  } else if (unitPart == "%") {
    unit = CssUnit::Percent;
  }

  out = CssLength{numericValue, unit};
  return true;
}

// Only border visibility is needed for <hr>; keep publisher dimensions and
// colors out of layout. These helpers allocate no memory.
bool tryBorderStyleSuppression(std::string_view value, bool& suppressed) {
  static constexpr const char* styles[] = {"none",   "hidden", "dotted", "dashed", "solid",
                                           "double", "groove", "ridge",  "inset",  "outset"};
  for (const char* keyword : styles) {
    if (iequalsAscii(value, keyword)) {
      suppressed = iequalsAscii(value, "none") || iequalsAscii(value, "hidden");
      return true;
    }
  }
  return false;
}

bool tryBorderWidthSuppression(std::string_view value, bool& suppressed) {
  if (iequalsAscii(value, "thin") || iequalsAscii(value, "medium") || iequalsAscii(value, "thick")) {
    suppressed = false;
    return true;
  }
  // Reject unsupported units rather than interpreting them as pixels.
  for (const auto unit :
       {std::string_view("px"), std::string_view("pt"), std::string_view("rem"), std::string_view("em")}) {
    if (value.size() > unit.size() && iequalsAscii(value.substr(value.size() - unit.size()), unit)) {
      float width = 0;
      if (!tryParseNumber(value.substr(0, value.size() - unit.size()), width) || !std::isfinite(width) || width < 0)
        return false;
      suppressed = width == 0;
      return true;
    }
  }
  float width = 0;
  if (!tryParseNumber(value, width) || width != 0) return false;
  suppressed = true;
  return true;
}

CssBorderStyle borderStyleValue(std::string_view value) {
  if (iequalsAscii(value, "none") || iequalsAscii(value, "hidden")) return CssBorderStyle::None;
  if (iequalsAscii(value, "double")) return CssBorderStyle::Double;
  if (iequalsAscii(value, "dotted")) return CssBorderStyle::Dotted;
  if (iequalsAscii(value, "dashed")) return CssBorderStyle::Dashed;
  return CssBorderStyle::Solid;
}

uint8_t borderWidthValue(std::string_view value) {
  if (iequalsAscii(value, "thin")) return 1;
  if (iequalsAscii(value, "medium")) return 3;
  if (iequalsAscii(value, "thick")) return 5;
  float scale = 1;
  for (const auto unit :
       {std::string_view("rem"), std::string_view("em"), std::string_view("pt"), std::string_view("px")}) {
    if (value.size() > unit.size() && iequalsAscii(value.substr(value.size() - unit.size()), unit)) {
      if (unit == "em" || unit == "rem")
        scale = 16;
      else if (unit == "pt")
        scale = 4.0f / 3.0f;
      value.remove_suffix(unit.size());
      break;
    }
  }
  float width = 0;
  if (!tryParseNumber(value, width)) return 0;
  return static_cast<uint8_t>(std::clamp(width * scale + 0.5f, 0.0f, 8.0f));
}

bool parseBorderSuppression(std::string_view name, std::string_view value, CssStyle& style) {
  uint8_t edges = 0x0F;
  std::string_view suffix;
  if (iequalsAscii(name, "border")) {
    suffix = {};
  } else if (name.size() > 7 && iequalsAscii(name.substr(0, 7), "border-")) {
    suffix = name.substr(7);
    static constexpr const char* edgeNames[] = {"top", "right", "bottom", "left"};
    for (size_t edge = 0; edge < 4; ++edge) {
      const std::string_view edgeName = edgeNames[edge];
      if (suffix.size() >= edgeName.size() && iequalsAscii(suffix.substr(0, edgeName.size()), edgeName) &&
          (suffix.size() == edgeName.size() || suffix[edgeName.size()] == '-')) {
        edges = 1 << edge;
        suffix.remove_prefix(edgeName.size());
        if (!suffix.empty()) suffix.remove_prefix(1);
        break;
      }
    }
  } else {
    return false;
  }

  value = trimCssWhitespace(stripTrailingImportant(value));
  uint8_t styleMask = 0, widthMask = 0;
  if (iequalsAscii(suffix, "style") || iequalsAscii(suffix, "width")) {
    std::string_view tokens[4];
    size_t count = 0;
    forEachDelimitedToken(value, isCssWhitespace, [&](std::string_view token) {
      if (count < 4) tokens[count] = token;
      ++count;
    });
    if (count == 0 || count > 4 || (edges != 0x0F && count != 1)) return true;
    CssBorderSide parsed[4];
    for (size_t edge = 0; edge < 4; ++edge) {
      // CSS one/two/three/four-value shorthand order: top/right/bottom/left.
      const size_t index = edge < count ? edge : (edge == 2 ? 0 : (count > 1 ? 1 : 0));
      bool suppressed = false;
      const bool valid = iequalsAscii(suffix, "style") ? tryBorderStyleSuppression(tokens[index], suppressed)
                                                       : tryBorderWidthSuppression(tokens[index], suppressed);
      if (!valid) return true;
      if (iequalsAscii(suffix, "style"))
        parsed[edge].style = borderStyleValue(tokens[index]);
      else
        parsed[edge].width = borderWidthValue(tokens[index]);
      if (suppressed) styleMask |= 1 << edge;
    }
    for (size_t edge = 0; edge < 4; ++edge) {
      if (!(edges & (1u << edge))) continue;
      if (iequalsAscii(suffix, "style"))
        style.borders[edge].style = parsed[edge].style;
      else
        style.borders[edge].width = parsed[edge].width;
    }
    if (iequalsAscii(suffix, "style")) {
      style.borderStyleSuppressed = (style.borderStyleSuppressed & ~edges) | (styleMask & edges);
      style.borderStyleDefined |= edges;
    } else {
      style.borderWidthSuppressed = (style.borderWidthSuppressed & ~edges) | (styleMask & edges);
      style.borderWidthDefined |= edges;
    }
  } else if (suffix.empty()) {
    // A border shorthand resets omitted style to none and width to medium.
    bool styleSuppressed = true, widthSuppressed = false, recognized = false;
    CssBorderSide parsed;
    forEachDelimitedToken(value, isCssWhitespace, [&](std::string_view token) {
      if (tryBorderStyleSuppression(token, styleSuppressed)) {
        parsed.style = borderStyleValue(token);
        recognized = true;
      } else if (tryBorderWidthSuppression(token, widthSuppressed)) {
        parsed.width = borderWidthValue(token);
        recognized = true;
      }
    });
    if (!recognized) return true;
    for (size_t edge = 0; edge < 4; ++edge)
      if (edges & (1u << edge)) style.borders[edge] = parsed;
    styleMask = styleSuppressed ? edges : 0;
    widthMask = widthSuppressed ? edges : 0;
    style.borderStyleSuppressed = (style.borderStyleSuppressed & ~edges) | styleMask;
    style.borderWidthSuppressed = (style.borderWidthSuppressed & ~edges) | widthMask;
    style.borderStyleDefined |= edges;
    style.borderWidthDefined |= edges;
  } else {
    return false;
  }
  style.defined.border = 1;
  return true;
}

// Declaration parsing

void CssParser::parseDeclarationIntoStyle(std::string_view decl, CssStyle& style, const CssParser* context,
                                          std::optional<std::string_view> base) {
  const size_t colonPos = decl.find(':');
  if (colonPos == std::string_view::npos || colonPos == 0) return;

  const std::string_view name = trimCssWhitespace(decl.substr(0, colonPos));
  const std::string_view value = trimCssWhitespace(stripTrailingImportant(decl.substr(colonPos + 1)));
  if (context) context->parseResourceDeclaration(name, value, style, base);
  if (cssExtras::parse(name, value, style)) return;

  if (name.empty() || value.empty()) return;

  if (iequalsAscii(name, "float")) {
    const auto mode = trimCssWhitespace(stripTrailingImportant(value));
    if (iequalsAscii(mode, "left") || iequalsAscii(mode, "none")) {
      style.floatLeft = iequalsAscii(mode, "left");
      style.defined.floatLeft = 1;
    }
  } else if (iequalsAscii(name, "initial-letter") || iequalsAscii(name, "-webkit-initial-letter")) {
    float lines = 0;
    const auto raw = trimCssWhitespace(stripTrailingImportant(value));
    if (iequalsAscii(raw, "normal")) {
      style.initialLetter = 0;
      style.defined.initialLetter = 1;
    } else if (tryParseNumber(raw.substr(0, raw.find_first_of(" \t")), lines) && std::isfinite(lines) && lines >= 1) {
      style.initialLetter = static_cast<uint8_t>(std::clamp(lines, 1.0f, 4.0f));
      style.defined.initialLetter = 1;
    }
  } else if (iequalsAscii(name, "white-space")) {
    const auto mode = trimCssWhitespace(stripTrailingImportant(value));
    if (iequalsAscii(mode, "pre") || iequalsAscii(mode, "pre-wrap") || iequalsAscii(mode, "break-spaces")) {
      style.preserveWhitespace = true;
      style.defined.whiteSpace = 1;
    } else if (iequalsAscii(mode, "normal") || iequalsAscii(mode, "nowrap")) {
      style.preserveWhitespace = false;
      style.defined.whiteSpace = 1;
    }
  } else if (parseBorderSuppression(name, value, style)) {
    return;
  }

  if (iequalsAscii(name, "text-align")) {
    style.textAlign = interpretAlignment(value);
    style.defined.textAlign = 1;
  } else if (iequalsAscii(name, "font-family")) {
    const auto familyValue = trimCssWhitespace(stripTrailingImportant(value));
    if (!context && (iequalsAscii(familyValue, "inherit") || iequalsAscii(familyValue, "unset"))) return;
    uint32_t token = context ? context->mutableFontCatalog().intern(familyValue) : firstFontFamily(familyValue);
    if (token != CssFontCatalog::INVALID_ID) {
      if (context && !context->fontScope_.empty())
        token = context->mutableFontCatalog().scopeStack(token, context->fontScope_);
      style.fontFamily = token;
      style.defined.fontFamily = 1;
    }
  } else if (iequalsAscii(name, "font-size")) {
    const auto sizeValue = trimCssWhitespace(stripTrailingImportant(value));
    CssLength size;
    bool valid = false;
    if (iequalsAscii(sizeValue, "inherit") || iequalsAscii(sizeValue, "unset")) {
      size = CssLength{1.0f, CssUnit::Em};
      valid = true;
    } else if (iequalsAscii(sizeValue, "larger") || iequalsAscii(sizeValue, "smaller")) {
      size = CssLength{iequalsAscii(sizeValue, "larger") ? 1.2f : 1.0f / 1.2f, CssUnit::Em};
      valid = true;
    } else {
      static constexpr const char* keywords[] = {"xx-small", "x-small", "small",    "medium",
                                                 "large",    "x-large", "xx-large", "initial"};
      static constexpr float factors[] = {0.6f, 0.75f, 0.889f, 1.0f, 1.2f, 1.5f, 2.0f, 1.0f};
      for (size_t i = 0; i < std::size(keywords); ++i) {
        if (iequalsAscii(sizeValue, keywords[i])) {
          size = CssLength{16.0f * factors[i], CssUnit::Pixels};
          valid = true;
          break;
        }
      }
      if (!valid) {
        // Unlike spacing lengths, unsupported units and unitless sizes are
        // ignored instead of silently becoming pixels.
        static constexpr const char* units[] = {"rem", "em", "px", "pt", "%"};
        static constexpr CssUnit unitKinds[] = {CssUnit::Rem, CssUnit::Em, CssUnit::Pixels, CssUnit::Points,
                                                CssUnit::Percent};
        for (size_t i = 0; i < std::size(units); ++i) {
          const size_t unitLength = std::strlen(units[i]);
          if (sizeValue.size() > unitLength &&
              iequalsAscii(sizeValue.substr(sizeValue.size() - unitLength), units[i])) {
            valid = tryParseNumber(sizeValue.substr(0, sizeValue.size() - unitLength), size.value);
            size.unit = unitKinds[i];
            break;
          }
        }
      }
    }
    if (valid && std::isfinite(size.value) && size.value > 0.0f) {
      style.fontSize = size;
      style.defined.fontSize = 1;
    }
  } else if (iequalsAscii(name, "text-transform")) {
    const auto transform = trimCssWhitespace(stripTrailingImportant(value));
    if (iequalsAscii(transform, "none") || iequalsAscii(transform, "initial") || iequalsAscii(transform, "uppercase") ||
        iequalsAscii(transform, "lowercase") || iequalsAscii(transform, "inherit") ||
        iequalsAscii(transform, "unset")) {
      style.textTransform = iequalsAscii(transform, "uppercase")   ? CssTextTransform::Uppercase
                            : iequalsAscii(transform, "lowercase") ? CssTextTransform::Lowercase
                            : (iequalsAscii(transform, "inherit") || iequalsAscii(transform, "unset"))
                                ? CssTextTransform::Inherit
                                : CssTextTransform::None;
      style.defined.textTransform = 1;
    }
  } else if (iequalsAscii(name, "letter-spacing")) {
    const auto spacing = trimCssWhitespace(stripTrailingImportant(value));
    if (iequalsAscii(spacing, "normal") || iequalsAscii(spacing, "initial")) {
      style.letterSpacing = CssLength{};
      style.defined.letterSpacing = 1;
    } else if (iequalsAscii(spacing, "inherit") || iequalsAscii(spacing, "unset")) {
      style.letterSpacing = CssLength{0, CssUnit::Inherit};
      style.defined.letterSpacing = 1;
    } else {
      CssLength length;
      bool valid = false;
      static constexpr const char* units[] = {"rem", "em", "px", "pt"};
      static constexpr CssUnit kinds[] = {CssUnit::Rem, CssUnit::Em, CssUnit::Pixels, CssUnit::Points};
      for (size_t i = 0; i < std::size(units); ++i) {
        const size_t bytes = std::strlen(units[i]);
        if (spacing.size() > bytes && iequalsAscii(spacing.substr(spacing.size() - bytes), units[i])) {
          valid = tryParseNumber(spacing.substr(0, spacing.size() - bytes), length.value);
          length.unit = kinds[i];
          break;
        }
      }
      if (!valid) valid = tryParseNumber(spacing, length.value) && length.value == 0;
      if (valid && std::isfinite(length.value)) {
        style.letterSpacing = length;
        style.defined.letterSpacing = 1;
      }
    }
  } else if (iequalsAscii(name, "font-style")) {
    style.fontStyle = interpretFontStyle(value);
    style.defined.fontStyle = 1;
  } else if (iequalsAscii(name, "font-weight")) {
    style.fontWeight = interpretFontWeight(value);
    style.defined.fontWeight = 1;
  } else if (iequalsAscii(name, "font-variant") || iequalsAscii(name, "font-variant-caps")) {
    style.fontVariantCaps = interpretFontVariantCaps(value);
    style.defined.fontVariantCaps = 1;
    if (iequalsAscii(name, "font-variant") &&
        (iequalsAscii(value, "none") || iequalsAscii(value, "normal") || iequalsAscii(value, "initial") ||
         iequalsAscii(value, "inherit") || iequalsAscii(value, "unset"))) {
      style.ligatures = iequalsAscii(value, "none")                                        ? CssLigatures::None
                        : (iequalsAscii(value, "inherit") || iequalsAscii(value, "unset")) ? CssLigatures::Inherit
                                                                                           : CssLigatures::Normal;
      style.defined.ligatures = 1;
    }
  } else if (iequalsAscii(name, "text-decoration") || iequalsAscii(name, "text-decoration-line")) {
    style.textDecoration = interpretDecoration(value);
    style.defined.textDecoration = 1;
  } else if (iequalsAscii(name, "text-indent")) {
    style.textIndent = interpretLength(value);
    style.defined.textIndent = 1;
  } else if (iequalsAscii(name, "margin-top")) {
    style.marginTop = interpretLength(value);
    style.defined.marginTop = 1;
  } else if (iequalsAscii(name, "margin-bottom")) {
    style.marginBottom = interpretLength(value);
    style.defined.marginBottom = 1;
  } else if (iequalsAscii(name, "margin-left")) {
    style.marginLeft = interpretLength(value);
    style.defined.marginLeft = 1;
  } else if (iequalsAscii(name, "margin-right")) {
    style.marginRight = interpretLength(value);
    style.defined.marginRight = 1;
  } else if (iequalsAscii(name, "margin")) {
    std::string_view margins[4];
    const size_t count = collectEdgeValueTokens(value, margins);
    if (count > 0) {
      style.marginTop = interpretLength(margins[0]);
      style.marginRight = count >= 2 ? interpretLength(margins[1]) : style.marginTop;
      style.marginBottom = count >= 3 ? interpretLength(margins[2]) : style.marginTop;
      style.marginLeft = count >= 4 ? interpretLength(margins[3]) : style.marginRight;
      style.defined.marginTop = style.defined.marginRight = style.defined.marginBottom = style.defined.marginLeft = 1;
    }
  } else if (iequalsAscii(name, "padding-top")) {
    style.paddingTop = interpretLength(value);
    style.defined.paddingTop = 1;
  } else if (iequalsAscii(name, "padding-bottom")) {
    style.paddingBottom = interpretLength(value);
    style.defined.paddingBottom = 1;
  } else if (iequalsAscii(name, "padding-left")) {
    style.paddingLeft = interpretLength(value);
    style.defined.paddingLeft = 1;
  } else if (iequalsAscii(name, "padding-right")) {
    style.paddingRight = interpretLength(value);
    style.defined.paddingRight = 1;
  } else if (iequalsAscii(name, "padding")) {
    std::string_view paddings[4];
    const size_t count = collectEdgeValueTokens(value, paddings);
    if (count > 0) {
      style.paddingTop = interpretLength(paddings[0]);
      style.paddingRight = count >= 2 ? interpretLength(paddings[1]) : style.paddingTop;
      style.paddingBottom = count >= 3 ? interpretLength(paddings[2]) : style.paddingTop;
      style.paddingLeft = count >= 4 ? interpretLength(paddings[3]) : style.paddingRight;
      style.defined.paddingTop = style.defined.paddingRight = style.defined.paddingBottom = style.defined.paddingLeft =
          1;
    }
  } else if (iequalsAscii(name, "height")) {
    CssLength len;
    if (tryInterpretLength(value, len)) {
      style.imageHeight = len;
      style.defined.imageHeight = 1;
    }
  } else if (iequalsAscii(name, "width")) {
    CssLength len;
    if (tryInterpretLength(value, len)) {
      style.imageWidth = len;
      style.defined.imageWidth = 1;
    }
  } else if (iequalsAscii(name, "display")) {
    const std::string_view displayValue = stripTrailingImportant(value);
    style.display = iequalsAscii(displayValue, "inline-block") ? CssDisplay::InlineBlock
                    : iequalsAscii(displayValue, "none")       ? CssDisplay::None
                    : iequalsAscii(displayValue, "inline")     ? CssDisplay::Inline
                                                               : CssDisplay::Block;
    style.defined.display = 1;
  } else if (iequalsAscii(name, "background") || iequalsAscii(name, "background-color")) {
    const auto color = trimCssWhitespace(stripTrailingImportant(value));
    if (iequalsAscii(color, "gray") || iequalsAscii(color, "grey") || iequalsAscii(color, "lightgray") ||
        iequalsAscii(color, "lightgrey") || iequalsAscii(color, "silver") ||
        ((color.size() == 4 || color.size() == 7) && color[0] == '#' && color != "#000" && color != "#000000" &&
         !iequalsAscii(color, "#fff") && !iequalsAscii(color, "#ffffff"))) {
      style.shaded = true;
      style.defined.shaded = 1;
    } else if (iequalsAscii(color, "transparent") || iequalsAscii(color, "white") || iequalsAscii(color, "#fff") ||
               iequalsAscii(color, "#ffffff")) {
      style.shaded = false;
      style.defined.shaded = 1;
    }
    bool backgroundBlack = false;
    if (tryInterpretBackgroundBlack(value, backgroundBlack)) {
      style.backgroundBlack = backgroundBlack;
      style.defined.backgroundBlack = 1;
      style.shaded = false;
      style.defined.shaded = 1;
    }
  } else if (iequalsAscii(name, "direction")) {
    const std::string_view directionValue = stripTrailingImportant(value);
    if (iequalsAscii(directionValue, "rtl")) {
      style.direction = CssTextDirection::Rtl;
      style.defined.direction = 1;
    } else if (iequalsAscii(directionValue, "ltr")) {
      style.direction = CssTextDirection::Ltr;
      style.defined.direction = 1;
    }
  } else if (iequalsAscii(name, "vertical-align")) {
    if (iequalsAscii(value, "super")) {
      style.verticalAlign = CssVerticalAlign::Super;
      style.defined.verticalAlign = 1;
    } else if (iequalsAscii(value, "sub")) {
      style.verticalAlign = CssVerticalAlign::Sub;
      style.defined.verticalAlign = 1;
    }
  } else if (iequalsAscii(name, "list-style-type")) {
    const std::string_view listStyleValue = stripTrailingImportant(value);
    style.listStyleType = iequalsAscii(listStyleValue, "none") ? CssListStyleType::None : CssListStyleType::Disc;
    style.defined.listStyleType = 1;
  } else if (iequalsAscii(name, "page-break-before") || iequalsAscii(name, "break-before")) {
    bool pageBreakBefore = false;
    if (tryInterpretCssPageBreak(value, pageBreakBefore)) {
      style.pageBreakBefore = pageBreakBefore;
      style.defined.pageBreakBefore = 1;
    }
  } else if (iequalsAscii(name, "page-break-after") || iequalsAscii(name, "break-after")) {
    bool pageBreakAfter = false;
    if (tryInterpretCssPageBreak(value, pageBreakAfter)) {
      style.pageBreakAfter = pageBreakAfter;
      style.defined.pageBreakAfter = 1;
    }
  }
}

CssStyle CssParser::parseDeclarations(std::string_view declBlock, const CssParser* context,
                                      std::optional<std::string_view> base) {
  CssStyle style;

  size_t start = 0;
  char quote = 0;
  unsigned depth = 0;
  bool escaped = false;
  for (size_t i = 0; i <= declBlock.size(); ++i) {
    const char c = i < declBlock.size() ? declBlock[i] : ';';
    if (escaped) {
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    if (quote) {
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
      continue;
    }
    if (c == '(')
      ++depth;
    else if (c == ')' && depth)
      --depth;
    if (c == ';' && !depth) {
      if (i > start) parseDeclarationIntoStyle(declBlock.substr(start, i - start), style, context, base);
      start = i + 1;
    }
  }

  return style;
}

// Returns true if a simple selector (tag, .class, or tag.class) matches the element.
// Matching is ASCII case-insensitive; class tokens are read without allocation.
bool CssParser::selectorMatchesElement(std::string_view selector, std::string_view tag, std::string_view classAttr) {
  if (selector.empty()) return false;

  const size_t dotPos = selector.find('.');
  if (dotPos == std::string::npos) {
    return iequalsAscii(selector, tag);
  }

  const std::string_view selectorTag(selector.data(), dotPos);
  const std::string_view selectorClass(selector.data() + dotPos + 1, selector.size() - dotPos - 1);

  if (!selectorTag.empty() && !iequalsAscii(selectorTag, tag)) return false;

  if (classAttr.empty()) return false;
  bool matched = false;
  forEachDelimitedToken(classAttr, isCssWhitespace, [&](std::string_view cls) {
    if (iequalsAscii(cls, selectorClass)) matched = true;
  });
  return matched;
}

// Bounded compound/context matching. The existing guarded rule/cache storage owns strings;
// matching only borrows views and uses a 100-byte index on the stack.
namespace {
bool compoundMatches(std::string_view selector, std::string_view tag, std::string_view classes, std::string_view id) {
  size_t pos = selector.find_first_of(".#");
  auto element = selector.substr(0, pos);
  if (!element.empty() && element != "*" && !iequalsAscii(element, tag)) return false;
  if (pos == std::string_view::npos) return !selector.empty();
  while (pos < selector.size()) {
    const char kind = selector[pos++];
    const size_t end = selector.find_first_of(".#", pos);
    const auto name = selector.substr(pos, end == std::string_view::npos ? end : end - pos);
    if (name.empty()) return false;
    if (kind == '#') {
      if (!iequalsAscii(name, id)) return false;
    } else {
      bool found = false;
      forEachDelimitedToken(classes, isCssWhitespace, [&](std::string_view value) {
        if (iequalsAscii(name, value)) found = true;
      });
      if (!found) return false;
    }
    if (end == std::string_view::npos) break;
    pos = end;
  }
  return true;
}

uint32_t selectorSpecificity(std::string_view selector) {
  uint32_t score = 0;
  bool beginsElement = true;
  for (const char c : selector) {
    if (c == '#') {
      score += 65536;
      beginsElement = false;
    } else if (c == '.') {
      score += 256;
      beginsElement = false;
    } else if (isCssWhitespace(c) || c == '>')
      beginsElement = true;
    else if (beginsElement) {
      if (c != '*' && c != ':') ++score;
      beginsElement = false;
    }
  }
  return score;
}

bool contextMatches(std::string_view prefix, const std::vector<CssAncestorEntry>& ancestors) {
  // Each bit represents a possible remaining ancestor count. Keeping all
  // matches avoids greedily choosing the wrong nested div before a child rule.
  if (ancestors.size() >= 32) return false;
  uint32_t candidates = uint32_t{1} << ancestors.size();
  while (!(prefix = trimCssWhitespace(prefix)).empty()) {
    const bool child = prefix.back() == '>';
    if (child) {
      prefix.remove_suffix(1);
      prefix = trimCssWhitespace(prefix);
    }
    const size_t split = prefix.find_last_of(" \t\r\n\f>");
    const auto compound = prefix.substr(split == std::string_view::npos ? 0 : split + 1);
    uint32_t matches = 0;
    for (size_t remaining = 1; remaining <= ancestors.size(); ++remaining) {
      if (!(candidates & (uint32_t{1} << remaining))) continue;
      for (int index = static_cast<int>(remaining) - 1; index >= 0; --index) {
        const auto& ancestor = ancestors[index];
        if (compoundMatches(compound, ancestor.tag, ancestor.classAttr, ancestor.idAttr))
          matches |= uint32_t{1} << index;
        if (child) break;
      }
    }
    if (!matches) return false;
    if (split == std::string_view::npos) return true;
    candidates = matches;
    prefix = prefix.substr(0, split + (prefix[split] == '>' ? 1 : 0));
  }
  return true;
}
}  // namespace

// Rule processing

CssParser::ParsedRule* CssParser::findPsramParsedRule(const std::string_view selector) const {
  if (!parsedRuleBuckets_) return nullptr;
  const size_t bucket = SvHash{}(selector) % PARSED_RULE_BUCKETS;
  for (auto* rule = parsedRuleBuckets_[bucket]; rule; rule = rule->bucketNext) {
    if (SvEqual{}(rule->selector, selector)) return rule;
  }
  return nullptr;
}

bool CssParser::addPsramParsedRule(const std::string_view selector, const CssStyle& style) {
  if (!parsedRuleBuckets_) {
    if (!parsedRuleArena_.init(4096)) return false;  // Arena logs fallible allocation failures.
    parsedRuleBuckets_ = arenaNewArray<ParsedRule*>(parsedRuleArena_, PARSED_RULE_BUCKETS);
    if (!parsedRuleBuckets_) {
      LOG_ERR("CSS", "Failed to allocate PSRAM rule buckets");
      return false;
    }
  }
  const auto checkpoint = parsedRuleArena_.save();
  auto* key = static_cast<char*>(parsedRuleArena_.alloc(selector.size(), alignof(char)));
  auto* rule = arenaNew<ParsedRule>(parsedRuleArena_);
  if (!key || !rule) {
    parsedRuleArena_.restore(checkpoint);
    LOG_ERR("CSS", "Failed to allocate PSRAM CSS rule (%u existing)", static_cast<unsigned>(psramParsedRuleCount_));
    return false;
  }
  memcpy(key, selector.data(), selector.size());
  rule->selector = {key, selector.size()};
  rule->style = style;
  const size_t bucket = SvHash{}(selector) % PARSED_RULE_BUCKETS;
  rule->bucketNext = parsedRuleBuckets_[bucket];
  rule->next = parsedRuleHead_;
  parsedRuleBuckets_[bucket] = parsedRuleHead_ = rule;
  ++psramParsedRuleCount_;
  return true;
}

bool CssParser::processRuleBlockWithStyle(std::string_view selectorGroup, const CssStyle& style) {
  // Skip rules that don't define any supported properties to save RAM.
  if (!style.defined.anySet()) {
    return true;
  }

  // Check if we've reached the rule limit before processing
  if (parsedRuleCount() >= MAX_RULES) {
    LOG_ERR("CSS", "Reached max rules limit (%zu), treating CSS parse as incomplete", MAX_RULES);
    return false;
  }

  // Walk comma-separated selectors in place — no vector allocation. Selectors
  // with unsupported syntax (combinators, attributes, pseudo, etc.) are skipped
  // silently; the only heap allocation per kept selector is the std::string
  // map key, which is unavoidable since the map owns its keys.
  bool limitReached = false;
  auto hasHeapForRuleGrowth = [&]() {
    const size_t freeHeap = ESP.getFreeHeap();
    const size_t largestBlock = ESP.getMaxAllocHeap();
    if (freeHeap >= MIN_FREE_HEAP_FOR_RULE_GROWTH && largestBlock >= MIN_LARGEST_BLOCK_FOR_RULE_GROWTH) {
      return true;
    }
    LOG_ERR("CSS", "Stopping CSS parse before rule allocation (free=%u maxAlloc=%u rules=%u)",
            static_cast<unsigned>(freeHeap), static_cast<unsigned>(largestBlock),
            static_cast<unsigned>(parsedRuleCount()));
    return false;
  };
  forEachDelimitedToken(
      selectorGroup, [](char c) { return c == ','; },
      [&](std::string_view sel) {
        if (limitReached) return;

        if (sel.size() > MAX_SELECTOR_LENGTH) {
          LOG_DBG("CSS", "Selector too long (%zu > %zu), skipping", sel.size(), MAX_SELECTOR_LENGTH);
          return;
        }

        // Keep the optimized simple-rule path; bounded complex rules include child/
        // descendant compounds, ids, multiple classes, and ::first-letter.
        std::string_view subject = sel;
        bool firstLetter = false;
        size_t pseudoBytes = 0;
        for (const auto suffix :
             {std::string_view("::first-letter"), std::string_view(":first-letter"), std::string_view("::before"),
              std::string_view(":before"), std::string_view("::after"), std::string_view(":after"),
              std::string_view("::first-line"), std::string_view(":first-line")}) {
          if (subject.size() >= suffix.size() && iequalsAscii(subject.substr(subject.size() - suffix.size()), suffix)) {
            firstLetter = true;
            pseudoBytes = suffix.size();
            subject.remove_suffix(suffix.size());
            break;
          }
        }
        if (subject.empty() || subject.find_first_of("+[:~") != std::string_view::npos) return;
        const size_t split = subject.find_last_of(" \t\n\r\f>");
        const bool complex =
            firstLetter || split != std::string_view::npos || subject.find('#') != std::string_view::npos ||
            std::count(subject.begin(), subject.end(), '.') > 1 || subject.find('*') != std::string_view::npos;
        if (complex) {
          std::string_view prefix;
          if (split != std::string_view::npos) {
            prefix = trimCssWhitespace(subject.substr(0, split + (subject[split] == '>' ? 1 : 0)));
            subject = subject.substr(split + 1);
          }
          if (subject.empty()) return;
          // The sentinel denotes no ancestor constraint and keeps the on-disk fields nonempty.
          if (prefix.empty()) prefix = "@";
          const auto storedSubject = firstLetter ? sel.substr(sel.size() - subject.size() - pseudoBytes) : subject;
          auto it = std::find_if(descendantRules_.begin(), descendantRules_.end(), [&](const DescendantRule& rule) {
            return iequalsAscii(rule.ancestorSelector, prefix) && iequalsAscii(rule.subjectSelector, storedSubject);
          });
          if (it != descendantRules_.end())
            it->style.applyOver(style);
          else if (descendantRules_.size() < MAX_DESCENDANT_RULES) {
            if (!hasHeapForRuleGrowth()) {
              limitReached = true;
              return;
            }
            descendantRules_.push_back({std::string(prefix), std::string(storedSubject), style});
          }
          return;
        }

        // Skip if this would exceed the rule limit
        if (parsedRuleCount() >= MAX_RULES) {
          LOG_ERR("CSS", "Reached max rules limit, treating CSS parse as incomplete");
          limitReached = true;
          return;
        }

        // Store or merge with existing. Hash/equal are case-insensitive, so two
        // selectors that differ only in ASCII case collide on insert and merge.
        if (usePsramParsedRules_) {
          if (auto* existing = findPsramParsedRule(sel)) {
            existing->style.applyOver(style);
          } else if (!hasHeapForRuleGrowth() || !addPsramParsedRule(sel, style)) {
            limitReached = true;
          }
          return;
        }
        auto it = rulesBySelector_.find(sel);
        if (it != rulesBySelector_.end()) {
          it->second.applyOver(style);
        } else {
          if (!hasHeapForRuleGrowth()) {
            limitReached = true;
            return;
          }
          rulesBySelector_.emplace(std::string(sel), style);
        }
      });
  return !limitReached;
}

// Main parsing entry point

bool CssParser::loadFromStream(FsFile& source, const std::string_view baseDir, const bool fontsOnly,
                               const bool includeFaces) {
  if (!source) {
    LOG_ERR("CSS", "Cannot read from invalid file");
    return false;
  }

  size_t totalRead = 0;

  // Use stack-allocated buffers for parsing to avoid heap reallocations
  StackBuffer selector;
  StackBuffer declBuffer(4096);
  if (!selector.data || !declBuffer.data) {
    LOG_ERR("CSS", "Cannot allocate bounded CSS parse scratch");
    return false;
  }
  stylesheetBase_.assign(baseDir);

  bool inComment = false;
  bool maybeSlash = false;
  bool prevStar = false;

  bool inAtRule = false;
  int atDepth = 0;
  char atKeyword[16] = {};
  size_t atKeywordLen = 0;
  bool atKeywordDone = false, inFontFace = false, faceTruncated = false;

  int bodyDepth = 0;
  bool skippingRule = false;
  bool stopParsing = false;
  CssStyle currentStyle;
  char quote = 0;
  bool escaped = false;
  unsigned functionDepth = 0;
  bool truncated = false;
  const auto append = [&](char c) {
    if (inFontFace || bodyDepth)
      declBuffer.push_back(c);
    else
      selector.push_back(c);
  };

  auto handleChar = [&](const char c) {
    if (escaped) {
      append(c);
      escaped = false;
      return;
    }
    if (quote) {
      append(c);
      if (c == '\\')
        escaped = true;
      else if (c == quote)
        quote = 0;
      return;
    }
    if (c == '\'' || c == '"') {
      append(c);
      quote = c;
      return;
    }
    if (bodyDepth == 1 && c == '(') {
      ++functionDepth;
      append(c);
      return;
    }
    if (bodyDepth == 1 && functionDepth) {
      if (c == ')') --functionDepth;
      append(c);
      return;
    }
    if (inAtRule) {
      if (c == '{') {
        ++atDepth;
        if (atDepth == 1 && std::string_view(atKeyword, atKeywordLen) == "font-face") {
          inFontFace = true;
          declBuffer.clear();
          faceTruncated = false;
        }
      } else if (c == '}') {
        if (atDepth > 0) --atDepth;
        if (atDepth == 0) {
          if (includeFaces && inFontFace && !faceTruncated) stopParsing = !parseFontFace(declBuffer, baseDir);
          if (faceTruncated) {
            mutableFontCatalog().markTruncated();
            truncated = true;
          }
          inFontFace = false;
          declBuffer.clear();
          inAtRule = false;
        }
      } else if (c == ';' && atDepth == 0) {
        inAtRule = false;
      } else if (atDepth == 0 && !atKeywordDone) {
        if (!isCssWhitespace(c) && atKeywordLen < sizeof(atKeyword))
          atKeyword[atKeywordLen++] = asciiToLower(c);
        else
          atKeywordDone = true;
      } else if (inFontFace && atDepth == 1) {
        if (declBuffer.size() == declBuffer.capacity) faceTruncated = true;
        declBuffer.push_back(c);
      }
      return;
    }

    if (bodyDepth == 0) {
      if (selector.empty() && isCssWhitespace(c)) {
        return;
      }
      if (c == '@' && selector.empty()) {
        inAtRule = true;
        atDepth = 0;
        atKeywordLen = 0;
        atKeywordDone = false;
        return;
      }
      if (c == '{') {
        bodyDepth = 1;
        currentStyle = CssStyle{};
        declBuffer.clear();
        if (selector.truncated || selector.size() > MAX_SELECTOR_LENGTH * 4) {
          skippingRule = true;
        }
        return;
      }
      selector.push_back(c);
      return;
    }

    // bodyDepth > 0
    if (c == '{') {
      ++bodyDepth;
      return;
    }
    if (c == '}') {
      --bodyDepth;
      if (bodyDepth == 0) {
        if (!fontsOnly && !skippingRule && !declBuffer.empty()) {
          if (!declBuffer.truncated)
            parseDeclarationIntoStyle(declBuffer, currentStyle, this);
          else
            truncated = true;
        }
        if (!skippingRule) {
          stopParsing = !fontsOnly && !processRuleBlockWithStyle(selector, currentStyle);
        }
        selector.clear();
        declBuffer.clear();
        skippingRule = false;
        return;
      }
      return;
    }
    if (bodyDepth > 1) {
      return;
    }
    if (!fontsOnly && !skippingRule) {
      if (c == ';') {
        if (!declBuffer.empty()) {
          if (!declBuffer.truncated)
            parseDeclarationIntoStyle(declBuffer, currentStyle, this);
          else
            truncated = true;
          declBuffer.clear();
        }
      } else {
        declBuffer.push_back(c);
      }
    }
  };

  char buffer[READ_BUFFER_SIZE];
  while (!stopParsing && source.available()) {
    int bytesRead = source.read(buffer, sizeof(buffer));
    if (bytesRead <= 0) return false;

    totalRead += static_cast<size_t>(bytesRead);
    if (totalRead > maxSourceBytes()) return false;

    for (int i = 0; i < bytesRead && !stopParsing; ++i) {
      const char c = buffer[i];

      if (inComment) {
        if (prevStar && c == '/') {
          inComment = false;
          prevStar = false;
          continue;
        }
        prevStar = c == '*';
        continue;
      }

      if (maybeSlash) {
        if (c == '*') {
          inComment = true;
          maybeSlash = false;
          prevStar = false;
          continue;
        }
        handleChar('/');
        maybeSlash = false;
        // fall through to process current char
      }

      if (c == '/' && !quote) {
        maybeSlash = true;
        continue;
      }

      handleChar(c);
    }
  }

  if (!stopParsing && maybeSlash) {
    handleChar('/');
  }

  if (stopParsing || truncated || quote || bodyDepth || (inAtRule && atDepth)) {
    LOG_ERR("CSS", "CSS parse stopped after %zu bytes with %zu selector rules and %zu descendant rules loaded",
            totalRead, parsedRuleCount(), descendantRules_.size());
    return false;
  }

  return true;
}

// Style resolution

CssStyle CssParser::resolveStyle(std::string_view tag, std::string_view classes,
                                 const std::vector<CssAncestorEntry>& ancestors, std::string_view id, bool firstLetter,
                                 const CssParser* overlay) const {
  return resolveStyleImpl(tag, classes, ancestors, id, firstLetter ? "first-letter" : "", overlay);
}
CssStyle CssParser::resolvePseudoStyle(std::string_view tag, std::string_view classes, std::string_view pseudo,
                                       const std::vector<CssAncestorEntry>& ancestors, std::string_view id,
                                       const CssParser* overlay) const {
  if (pseudo != "before" && pseudo != "after" && pseudo != "first-line" && pseudo != "first-letter") return {};
  return resolveStyleImpl(tag, classes, ancestors, id, pseudo, overlay);
}
CssStyle CssParser::resolveStyleImpl(std::string_view tag, std::string_view classes,
                                     const std::vector<CssAncestorEntry>& ancestors, std::string_view id,
                                     std::string_view pseudo, const CssParser* overlay) const {
  if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_CSS) return {};
  CssStyle result;
  const CssParser* sheets[] = {this, overlay};
  // At most 100 bounded context rules per sheet: 400 bytes, no selector/string allocation.
  // The native parser task has a 4KiB stack; styles are never duplicated into this array.
  std::array<uint16_t, MAX_DESCENDANT_RULES * 2> matches{};
  size_t count = 0;
  const auto ruleFor = [&](uint16_t index) -> const DescendantRule& {
    return sheets[index / MAX_DESCENDANT_RULES]->descendantRules_[index % MAX_DESCENDANT_RULES];
  };
  const auto specificity = [&](uint16_t index) {
    const auto& r = ruleFor(index);
    return selectorSpecificity(r.subjectSelector) +
           (r.ancestorSelector == "@" ? 0 : selectorSpecificity(r.ancestorSelector));
  };
  for (unsigned sheet = 0; sheet < 2; ++sheet) {
    if (!sheets[sheet]) continue;
    for (size_t i = 0; i < sheets[sheet]->descendantRules_.size(); ++i) {
      const auto& r = sheets[sheet]->descendantRules_[i];
      std::string_view subject = r.subjectSelector;
      auto colon = subject.find(':');
      std::string_view found;
      if (colon != std::string_view::npos) {
        found = subject.substr(colon);
        while (!found.empty() && found.front() == ':') found.remove_prefix(1);
        subject = subject.substr(0, colon);
      }
      if (!iequalsAscii(found, pseudo) || !compoundMatches(subject, tag, classes, id)) continue;
      if (r.ancestorSelector != "@" && !contextMatches(r.ancestorSelector, ancestors)) continue;
      matches[count++] = sheet * MAX_DESCENDANT_RULES + i;
    }
  }
  std::sort(matches.begin(), matches.begin() + count, [&](uint16_t a, uint16_t b) {
    auto sa = specificity(a), sb = specificity(b);
    return sa == sb ? a < b : sa < sb;
  });
  const auto applySimple = [&](const CssParser& sheet, uint32_t level) {
    if (!pseudo.empty()) return;
    CssStyle style;
    if (level == 1) {
      if (sheet.lookupRule(tag, style)) result.applyOver(style);
      return;
    }
    if (level != 256 && level != 257) return;
    forEachDelimitedToken(classes, isCssWhitespace, [&](std::string_view cls) {
      const size_t tagBytes = level == 257 ? tag.size() : 0;
      if (tagBytes + 1 + cls.size() > MAX_SELECTOR_LENGTH) return;
      std::array<char, MAX_SELECTOR_LENGTH> key{};
      if (tagBytes) memcpy(key.data(), tag.data(), tagBytes);
      key[tagBytes] = '.';
      memcpy(key.data() + tagBytes + 1, cls.data(), cls.size());
      if (sheet.lookupRule(std::string_view(key.data(), tagBytes + 1 + cls.size()), style)) result.applyOver(style);
    });
  };
  static constexpr uint32_t levels[] = {1, 256, 257};
  size_t simple = 0, next = 0;
  if (!pseudo.empty()) simple = 3;
  while (next < count || simple < 3) {
    const uint32_t contextLevel = next < count ? specificity(matches[next]) : UINT32_MAX;
    const uint32_t simpleLevel = simple < 3 ? levels[simple] : UINT32_MAX;
    const uint32_t level = std::min(contextLevel, simpleLevel);
    size_t end = next;
    while (end < count && specificity(matches[end]) == level) ++end;
    for (unsigned sheet = 0; sheet < 2; ++sheet) {
      if (!sheets[sheet]) continue;
      for (size_t i = next; i < end; ++i)
        if (matches[i] / MAX_DESCENDANT_RULES == sheet) result.applyOver(ruleFor(matches[i]).style);
      applySimple(*sheets[sheet], level);
    }
    next = end;
    if (simpleLevel == level) ++simple;
  }
  return result;
}
std::optional<std::string> CssParser::pseudoContentFor(std::string_view tag, std::string_view classes,
                                                       std::string_view pseudo,
                                                       const std::vector<CssAncestorEntry>& ancestors,
                                                       std::string_view id, const CssParser* overlay) const {
  const auto style = resolvePseudoStyle(tag, classes, pseudo, ancestors, id, overlay);
  if (!style.defined.pseudoContent) return {};
  if (!style.pseudoContent) return std::string{};
  const auto* text = overlay ? overlay->resource(style.pseudoContent, 1) : nullptr;
  if (!text) text = resource(style.pseudoContent, 1);
  return text ? std::optional<std::string>(*text) : std::nullopt;
}
std::optional<std::string> CssParser::backgroundImageFor(std::string_view tag, std::string_view classes,
                                                         const std::vector<CssAncestorEntry>& ancestors,
                                                         std::string_view id, const CssParser* overlay) const {
  const auto style = resolveStyle(tag, classes, ancestors, id, false, overlay);
  if (!style.defined.backgroundImage) return {};
  if (!style.backgroundImage) return std::string{};
  const auto* text = overlay ? overlay->resource(style.backgroundImage, 2) : nullptr;
  if (!text) text = resource(style.backgroundImage, 2);
  return text ? std::optional<std::string>(*text) : std::nullopt;
}

// Inline style parsing (static - doesn't need rule database)

CssStyle CssParser::parseInlineStyle(std::string_view styleValue) { return parseDeclarations(styleValue); }

// Cache serialization

// Cache file name (magic + version identify Crossink-owned CSS rule caches)
constexpr char rulesCache[] = "/css_rules.cache";
constexpr char rulesCacheTmp[] = "/css_rules.cache.tmp";
constexpr char rulesCacheBackup[] = "/css_rules.cache.bak";

uint32_t CssParser::selectorHash(std::string_view selector) {
  uint32_t h = 2166136261U;
  for (char c : selector) {
    h ^= static_cast<uint8_t>(asciiToLower(c));
    h *= 16777619U;
  }
  return h;
}

uint32_t CssParser::selectorSecondaryHash(std::string_view selector) {
  uint32_t h = 5381U;
  for (char c : selector) {
    h = ((h << 5U) + h) ^ static_cast<uint8_t>(asciiToLower(c));
  }
  return h;
}

bool CssParser::writeCssStylePayload(FsFile& file, const CssStyle& style) {
  auto writeBytes = [&file](const void* data, const size_t len) -> bool {
    return len == 0 || file.write(reinterpret_cast<const uint8_t*>(data), len) == len;
  };
  auto writeByte = [&writeBytes](const uint8_t value) -> bool { return writeBytes(&value, sizeof(value)); };
  auto writeLength = [&writeBytes, &writeByte](const CssLength& len) -> bool {
    return writeBytes(&len.value, sizeof(len.value)) && writeByte(static_cast<uint8_t>(len.unit));
  };

  if (!writeByte(static_cast<uint8_t>(style.textAlign)) || !writeByte(static_cast<uint8_t>(style.fontStyle)) ||
      !writeByte(static_cast<uint8_t>(style.fontWeight)) || !writeByte(static_cast<uint8_t>(style.textDecoration)) ||
      !writeByte(static_cast<uint8_t>(style.fontVariantCaps)) || !writeLength(style.textIndent) ||
      !writeLength(style.marginTop) || !writeLength(style.marginBottom) || !writeLength(style.marginLeft) ||
      !writeLength(style.marginRight) || !writeLength(style.paddingTop) || !writeLength(style.paddingBottom) ||
      !writeLength(style.paddingLeft) || !writeLength(style.paddingRight) || !writeLength(style.imageHeight) ||
      !writeLength(style.imageWidth) || !writeLength(style.fontSize) ||
      !writeByte(static_cast<uint8_t>(style.display)) ||
      !writeByte(static_cast<uint8_t>(style.backgroundBlack ? 1 : 0)) ||
      !writeByte(static_cast<uint8_t>(style.verticalAlign)) || !writeByte(static_cast<uint8_t>(style.direction)) ||
      !writeByte(static_cast<uint8_t>(style.pageBreakBefore ? 1 : 0)) ||
      !writeByte(static_cast<uint8_t>(style.pageBreakAfter ? 1 : 0)) ||
      !writeByte(static_cast<uint8_t>(style.listStyleType)) || !writeByte(style.borderStyleSuppressed) ||
      !writeByte(style.borderWidthSuppressed) || !writeByte(style.borderStyleDefined) ||
      !writeByte(style.borderWidthDefined) || !writeByte(style.preserveWhitespace)) {
    return false;
  }

  for (const auto& side : style.borders) {
    if (!writeByte(side.width) || !writeByte(static_cast<uint8_t>(side.style))) return false;
  }
  if (!writeByte(style.shaded) || !writeByte(style.floatLeft) || !writeByte(style.initialLetter)) return false;
  uint32_t definedBits = 0;
  if (style.defined.textAlign) definedBits |= 1 << 0;
  if (style.defined.fontStyle) definedBits |= 1 << 1;
  if (style.defined.fontWeight) definedBits |= 1 << 2;
  if (style.defined.textDecoration) definedBits |= 1 << 3;
  if (style.defined.textIndent) definedBits |= 1 << 4;
  if (style.defined.marginTop) definedBits |= 1 << 5;
  if (style.defined.marginBottom) definedBits |= 1 << 6;
  if (style.defined.marginLeft) definedBits |= 1 << 7;
  if (style.defined.marginRight) definedBits |= 1 << 8;
  if (style.defined.paddingTop) definedBits |= 1 << 9;
  if (style.defined.paddingBottom) definedBits |= 1 << 10;
  if (style.defined.paddingLeft) definedBits |= 1 << 11;
  if (style.defined.paddingRight) definedBits |= 1 << 12;
  if (style.defined.imageHeight) definedBits |= 1 << 13;
  if (style.defined.imageWidth) definedBits |= 1 << 14;
  if (style.defined.display) definedBits |= 1 << 15;
  if (style.defined.backgroundBlack) definedBits |= 1 << 16;
  if (style.defined.verticalAlign) definedBits |= 1 << 17;
  if (style.defined.direction) definedBits |= 1 << 18;
  if (style.defined.listStyleType) definedBits |= 1 << 19;
  if (style.defined.pageBreakBefore) definedBits |= 1 << 20;
  if (style.defined.pageBreakAfter) definedBits |= 1 << 21;
  if (style.defined.fontVariantCaps) definedBits |= 1 << 22;
  if (style.defined.fontSize) definedBits |= 1 << 23;
  if (style.defined.border) definedBits |= 1 << 24;
  if (style.defined.whiteSpace) definedBits |= 1u << 25;
  if (style.defined.shaded) definedBits |= 1u << 26;
  if (style.defined.floatLeft) definedBits |= 1u << 27;
  if (style.defined.initialLetter) definedBits |= 1u << 28;
  if (style.defined.fontFamily) definedBits |= 1u << 29;
  if (style.defined.textTransform) definedBits |= 1u << 30;
  if (style.defined.letterSpacing) definedBits |= 1u << 31;
  return writeBytes(&definedBits, sizeof(definedBits)) && writeBytes(&style.fontFamily, sizeof(style.fontFamily)) &&
         writeByte(static_cast<uint8_t>(style.textTransform)) && writeLength(style.letterSpacing) &&
         cssExtrasCodec::write(file, style);
}

bool CssParser::readCssStylePayload(FsFile& file, CssStyle& style) {
  auto readLength = [&file](CssLength& len) -> bool {
    if (file.read(&len.value, sizeof(len.value)) != sizeof(len.value)) return false;
    uint8_t unitVal;
    if (file.read(&unitVal, 1) != 1) return false;
    len.unit = static_cast<CssUnit>(unitVal);
    return true;
  };

  uint8_t enumVal;
  if (file.read(&enumVal, 1) != 1) return false;
  style.textAlign = static_cast<CssTextAlign>(enumVal);
  if (file.read(&enumVal, 1) != 1) return false;
  style.fontStyle = static_cast<CssFontStyle>(enumVal);
  if (file.read(&enumVal, 1) != 1) return false;
  style.fontWeight = static_cast<CssFontWeight>(enumVal);
  if (file.read(&enumVal, 1) != 1) return false;
  style.textDecoration = static_cast<CssTextDecoration>(enumVal & CSS_TEXT_DECORATION_MASK);
  if (file.read(&enumVal, 1) != 1) return false;
  style.fontVariantCaps = static_cast<CssFontVariantCaps>(enumVal);
  if (!readLength(style.textIndent) || !readLength(style.marginTop) || !readLength(style.marginBottom) ||
      !readLength(style.marginLeft) || !readLength(style.marginRight) || !readLength(style.paddingTop) ||
      !readLength(style.paddingBottom) || !readLength(style.paddingLeft) || !readLength(style.paddingRight) ||
      !readLength(style.imageHeight) || !readLength(style.imageWidth) || !readLength(style.fontSize)) {
    return false;
  }
  uint8_t displayVal;
  if (file.read(&displayVal, 1) != 1) return false;
  style.display = static_cast<CssDisplay>(displayVal);
  uint8_t backgroundBlackVal = 0;
  if (file.read(&backgroundBlackVal, 1) != 1) return false;
  style.backgroundBlack = backgroundBlackVal != 0;
  uint8_t verticalAlignVal = 0;
  if (file.read(&verticalAlignVal, 1) != 1) return false;
  style.verticalAlign = static_cast<CssVerticalAlign>(verticalAlignVal);
  uint8_t directionVal = 0;
  if (file.read(&directionVal, 1) != 1) return false;
  style.direction = static_cast<CssTextDirection>(directionVal);
  uint8_t pageBreakVal = 0;
  if (file.read(&pageBreakVal, 1) != 1) return false;
  style.pageBreakBefore = pageBreakVal != 0;
  if (file.read(&pageBreakVal, 1) != 1) return false;
  style.pageBreakAfter = pageBreakVal != 0;
  uint8_t listStyleTypeVal = 0;
  if (file.read(&listStyleTypeVal, 1) != 1 || listStyleTypeVal > static_cast<uint8_t>(CssListStyleType::None)) {
    return false;
  }
  style.listStyleType = static_cast<CssListStyleType>(listStyleTypeVal);
  if (file.read(&style.borderStyleSuppressed, 1) != 1 || file.read(&style.borderWidthSuppressed, 1) != 1 ||
      file.read(&style.borderStyleDefined, 1) != 1 || file.read(&style.borderWidthDefined, 1) != 1 ||
      ((style.borderStyleDefined | style.borderWidthDefined) & ~0x0F) != 0 ||
      (style.borderStyleSuppressed & ~style.borderStyleDefined) != 0 ||
      (style.borderWidthSuppressed & ~style.borderWidthDefined) != 0)
    return false;

  uint8_t whitespace = 0;
  if (file.read(&whitespace, 1) != 1 || whitespace > 1) return false;
  style.preserveWhitespace = whitespace != 0;
  for (auto& side : style.borders) {
    uint8_t raw = 0;
    if (file.read(&side.width, 1) != 1 || side.width > 8 || file.read(&raw, 1) != 1 || raw > 4) return false;
    side.style = static_cast<CssBorderStyle>(raw);
  }
  uint8_t shaded = 0, floated = 0;
  if (file.read(&shaded, 1) != 1 || shaded > 1 || file.read(&floated, 1) != 1 || floated > 1 ||
      file.read(&style.initialLetter, 1) != 1 || style.initialLetter > 4)
    return false;
  style.shaded = shaded;
  style.floatLeft = floated;
  uint32_t definedBits = 0;
  if (file.read(&definedBits, sizeof(definedBits)) != sizeof(definedBits)) return false;
  style.defined.textAlign = (definedBits & 1 << 0) != 0;
  style.defined.fontStyle = (definedBits & 1 << 1) != 0;
  style.defined.fontWeight = (definedBits & 1 << 2) != 0;
  style.defined.textDecoration = (definedBits & 1 << 3) != 0;
  style.defined.textIndent = (definedBits & 1 << 4) != 0;
  style.defined.marginTop = (definedBits & 1 << 5) != 0;
  style.defined.marginBottom = (definedBits & 1 << 6) != 0;
  style.defined.marginLeft = (definedBits & 1 << 7) != 0;
  style.defined.marginRight = (definedBits & 1 << 8) != 0;
  style.defined.paddingTop = (definedBits & 1 << 9) != 0;
  style.defined.paddingBottom = (definedBits & 1 << 10) != 0;
  style.defined.paddingLeft = (definedBits & 1 << 11) != 0;
  style.defined.paddingRight = (definedBits & 1 << 12) != 0;
  style.defined.imageHeight = (definedBits & 1 << 13) != 0;
  style.defined.imageWidth = (definedBits & 1 << 14) != 0;
  style.defined.display = (definedBits & 1 << 15) != 0;
  style.defined.backgroundBlack = (definedBits & 1 << 16) != 0;
  style.defined.verticalAlign = (definedBits & 1 << 17) != 0;
  style.defined.direction = (definedBits & 1 << 18) != 0;
  style.defined.listStyleType = (definedBits & 1 << 19) != 0;
  style.defined.pageBreakBefore = (definedBits & 1 << 20) != 0;
  style.defined.pageBreakAfter = (definedBits & 1 << 21) != 0;
  style.defined.fontVariantCaps = (definedBits & 1 << 22) != 0;
  style.defined.fontSize = (definedBits & 1 << 23) != 0;
  style.defined.border = (definedBits & 1 << 24) != 0;
  style.defined.whiteSpace = (definedBits & 1u << 25) != 0;
  style.defined.shaded = (definedBits & 1u << 26) != 0;
  style.defined.floatLeft = (definedBits & 1u << 27) != 0;
  style.defined.initialLetter = (definedBits & 1u << 28) != 0;
  style.defined.fontFamily = (definedBits & 1u << 29) != 0;
  style.defined.textTransform = (definedBits & 1u << 30) != 0;
  style.defined.letterSpacing = (definedBits & 1u << 31) != 0;
  if (file.read(&style.fontFamily, sizeof(style.fontFamily)) != sizeof(style.fontFamily)) return false;
  uint8_t transform;
  if (file.read(&transform, 1) != 1 || transform > static_cast<uint8_t>(CssTextTransform::Inherit) ||
      !readLength(style.letterSpacing) || !std::isfinite(style.letterSpacing.value) ||
      static_cast<uint8_t>(style.letterSpacing.unit) > static_cast<uint8_t>(CssUnit::Inherit) ||
      style.letterSpacing.unit == CssUnit::Percent)
    return false;
  style.textTransform = static_cast<CssTextTransform>(transform);
  if (style.hasFontSize() && (!std::isfinite(style.fontSize.value) || style.fontSize.value <= 0 ||
                              static_cast<uint8_t>(style.fontSize.unit) > static_cast<uint8_t>(CssUnit::Percent)))
    return false;
  return cssExtrasCodec::read(file, style);
}

bool CssParser::readRuleFromDiskAtOffset(const uint32_t ruleOffset, std::string_view selector,
                                         CssStyle& outStyle) const {
  FsFile file;
  if (!Storage.openFileForRead("CSS", cachePath + rulesCache, file)) {
    return false;
  }

  char selectorBuf[MAX_SELECTOR_LENGTH];
  uint16_t selectorLen = 0;
  bool ok = file.seek(ruleOffset) && file.read(&selectorLen, sizeof(selectorLen)) == sizeof(selectorLen) &&
            selectorLen == selector.size() && selectorLen <= MAX_SELECTOR_LENGTH &&
            file.read(selectorBuf, selectorLen) == selectorLen &&
            SvEqual{}(std::string_view(selectorBuf, selectorLen), selector) && readCssStylePayload(file, outStyle);
  file.close();
  return ok;
}

bool CssParser::lookupArenaRule(std::string_view selector, CssStyle& outStyle) const {
  if (!cachedRules_ || cachedRuleTableCount_ == 0 || selector.empty() || selector.size() > MAX_SELECTOR_LENGTH) {
    return false;
  }

  const uint32_t h = selectorHash(selector);
  const uint32_t secondaryHash = selectorSecondaryHash(selector);
  auto* begin = cachedRules_;
  auto* end = cachedRules_ + cachedRuleTableCount_;
  auto* it =
      std::lower_bound(begin, end, h, [](const CachedRule& rule, const uint32_t key) { return rule.hash < key; });
  for (; it != end && it->hash == h; ++it) {
    if (it->secondaryHash == secondaryHash && it->selectorLen == selector.size()) {
      outStyle = it->style;
      return true;
    }
  }
  return false;
}

bool CssParser::lookupRule(std::string_view selector, CssStyle& outStyle) const {
  if (const auto* rule = findPsramParsedRule(selector)) {
    outStyle = rule->style;
    return true;
  }
  if (auto it = rulesBySelector_.find(selector); it != rulesBySelector_.end()) {
    outStyle = it->second;
    return true;
  }
  if (selector.empty() || selector.size() > MAX_SELECTOR_LENGTH || !cacheIndexLoaded_) {
    return false;
  }

  if (lookupArenaRule(selector, outStyle)) {
    return true;
  }
  if (cachedRuleTableCount_ == cachedRuleCount_) {
    return false;
  }

  const uint32_t h = selectorHash(selector);
  auto it = std::lower_bound(cacheRuleOffsets_.begin(), cacheRuleOffsets_.end(), h,
                             [](const SelectorEntry& e, const uint32_t key) { return e.hash < key; });
  for (; it != cacheRuleOffsets_.end() && it->hash == h; ++it) {
    if (readRuleFromDiskAtOffset(it->offset, selector, outStyle)) {
      return true;
    }
  }
  return false;
}

bool CssParser::hasCache() const { return Storage.exists((cachePath + rulesCache).c_str()); }

CssParser::CacheStatus CssParser::inspectCache() const {
  if (cachePath.empty() || !hasCache()) {
    return CacheStatus::Missing;
  }

  FsFile file;
  if (!Storage.openFileForRead("CSS", cachePath + rulesCache, file)) {
    return CacheStatus::Invalid;
  }
  struct FileGuard {
    FsFile& file;
    ~FileGuard() {
      if (file.isOpen()) file.close();
    }
  } fileGuard{file};

  const auto readExact = [&file](void* out, const size_t size) { return file.read(out, size) == size; };
  const auto skipBytes = [&file](const size_t size) {
    if (static_cast<size_t>(file.available()) < size) return false;
    return file.seek(file.position() + size);
  };

  uint32_t magic = 0;
  uint8_t version = 0;
  uint8_t flags = 0;
  uint16_t ruleCount = 0;
  if (!readExact(&magic, sizeof(magic)) || magic != CSS_CACHE_MAGIC || !readExact(&version, sizeof(version)) ||
      version != CSS_CACHE_VERSION || !readExact(&flags, sizeof(flags)) || (flags & ~CSS_CACHE_FLAG_PARTIAL) != 0 ||
      !readExact(&ruleCount, sizeof(ruleCount)) || ruleCount > MAX_RULES) {
    return CacheStatus::Invalid;
  }

  if (!skipBytes(static_cast<size_t>(ruleCount) * sizeof(SelectorEntry))) {
    return CacheStatus::Invalid;
  }
  for (uint16_t i = 0; i < ruleCount; ++i) {
    uint16_t selectorLen = 0;
    if (!readExact(&selectorLen, sizeof(selectorLen)) || selectorLen == 0 || selectorLen > MAX_SELECTOR_LENGTH ||
        !skipBytes(static_cast<size_t>(selectorLen) + CSS_FIXED_STYLE_BYTES)) {
      return CacheStatus::Invalid;
    }
  }

  uint16_t descendantCount = 0;
  if (!readExact(&descendantCount, sizeof(descendantCount)) || descendantCount > MAX_DESCENDANT_RULES) {
    return CacheStatus::Invalid;
  }
  for (uint16_t i = 0; i < descendantCount; ++i) {
    for (uint8_t selectorIndex = 0; selectorIndex < 2; ++selectorIndex) {
      uint16_t selectorLen = 0;
      if (!readExact(&selectorLen, sizeof(selectorLen)) || selectorLen == 0 || selectorLen > MAX_SELECTOR_LENGTH ||
          !skipBytes(selectorLen)) {
        return CacheStatus::Invalid;
      }
    }
    if (!skipBytes(CSS_FIXED_STYLE_BYTES)) {
      return CacheStatus::Invalid;
    }
  }

  if (!readFontFaces(file)) return CacheStatus::Invalid;
  return (flags & CSS_CACHE_FLAG_PARTIAL) != 0 ? CacheStatus::Partial : CacheStatus::Complete;
}

std::array<uint16_t, 2> CssParser::contentFontSizeScales() const {
  constexpr std::array<uint16_t, 2> defaults{512, 192};
  if (cachePath.empty()) return defaults;
  FsFile file;
  if (!Storage.openFileForRead("CSS", cachePath + rulesCache, file)) return defaults;

  // A fixed 148-byte summary replaces a hydrated stylesheet. Nearby scales
  // share a nominal 12pt bucket; retain the smaller exact scale for stable ties.
  struct Hint {
    uint16_t scale = 0;
    uint16_t votes = 0;
  };
  std::array<Hint, 37> hints{};
  const auto skip = [&file](const size_t bytes) {
    return static_cast<size_t>(file.available()) >= bytes && file.seek(file.position() + bytes);
  };
  const auto readSelector = [&file, &skip](bool& heading) {
    uint16_t length = 0;
    if (file.read(&length, sizeof(length)) != sizeof(length) || !length || length > MAX_SELECTOR_LENGTH) return false;
    char prefix[3]{};
    const size_t count = std::min<size_t>(length, sizeof(prefix));
    if (file.read(prefix, count) != static_cast<int>(count) || !skip(length - count)) return false;
    heading = asciiToLower(prefix[0]) == 'h' && prefix[1] >= '1' && prefix[1] <= '6' &&
              (length == 2 || prefix[2] == '.' || prefix[2] == '#');
    return true;
  };
  const auto readStyle = [&file, &hints](const bool heading) {
    CssStyle style;
    if (!readCssStylePayload(file, style)) return false;
    if (!style.hasFontSize()) return true;
    float scale = style.fontSize.value;
    switch (style.fontSize.unit) {
      case CssUnit::Em:
      case CssUnit::Rem:
        break;
      case CssUnit::Percent:
        scale /= 100.0f;
        break;
      case CssUnit::Points:
        scale /= 12.0f;
        break;
      case CssUnit::Pixels:
        scale /= 16.0f;
        break;
      default:
        return true;
    }
    if (!std::isfinite(scale) || scale <= 0 || scale == 1.0f) return true;
    const auto fixed = static_cast<uint16_t>(std::clamp(scale * 256.0f + 0.5f, 46.0f, 1408.0f));
    const unsigned bucket = std::clamp<unsigned>((unsigned(fixed) * 12 + 128) / 256, 8, 44) - 8;
    auto& hint = hints[bucket];
    // The high bit ranks explicit heading selectors ahead of generic classes,
    // even in a stylesheet containing hundreds of footnote/body declarations.
    if (heading && !(hint.votes & 0x8000)) hint = {fixed, 0x8000};
    if (!heading && (hint.votes & 0x8000)) return true;
    if (!hint.scale || fixed < hint.scale) hint.scale = fixed;
    ++hint.votes;  // At most MAX_RULES + MAX_DESCENDANT_RULES declarations.
    return true;
  };
  const bool valid = [&]() {
    uint32_t magic = 0;
    uint8_t version = 0, flags = 0;
    uint16_t count = 0;
    if (file.read(&magic, sizeof(magic)) != sizeof(magic) || magic != CSS_CACHE_MAGIC || file.read(&version, 1) != 1 ||
        version != CSS_CACHE_VERSION || file.read(&flags, 1) != 1 || (flags & ~CSS_CACHE_FLAG_PARTIAL) ||
        file.read(&count, sizeof(count)) != sizeof(count) || count > MAX_RULES ||
        !skip(size_t(count) * sizeof(SelectorEntry)))
      return false;
    for (uint16_t i = 0; i < count; ++i) {
      bool heading = false;
      if (!readSelector(heading) || !readStyle(heading)) return false;
    }
    if (file.read(&count, sizeof(count)) != sizeof(count) || count > MAX_DESCENDANT_RULES) return false;
    for (uint16_t i = 0; i < count; ++i) {
      bool heading = false;
      if (!readSelector(heading) || !readSelector(heading) || !readStyle(heading)) return false;
    }
    return readFontFaces(file);
  }();
  file.close();  // Close the CSS cache before any .cpfont is opened.
  if (!valid) {
    LOG_DBG("CSS", "Ignoring invalid font-size hints cache");
    return defaults;
  }
  std::array<uint16_t, 2> result = defaults;
  for (size_t slot = 0; slot < result.size(); ++slot) {
    Hint* best = nullptr;
    for (auto& hint : hints) {
      // Small-print declarations must not consume both optional slots: a plain
      // h1 still needs its default enlargement when CSS only sizes footnotes.
      if (!hint.votes || (slot == 1 && result[0] < 256 && hint.scale < 256)) continue;
      if (!best || hint.votes > best->votes || (hint.votes == best->votes && hint.scale > best->scale)) best = &hint;
    }
    if (!best) break;
    result[slot] = best->scale;
    best->votes = 0;
    if (slot == 0) result[1] = result[0] < 256 ? defaults[0] : defaults[1];
  }
  return result;
}

void CssParser::deleteCache() const {
  if (hasCache()) Storage.remove((cachePath + rulesCache).c_str());
  Storage.remove((cachePath + rulesCacheTmp).c_str());
  Storage.remove((cachePath + rulesCacheBackup).c_str());
}

bool CssParser::saveToCache(const bool complete) const {
  if (cachePath.empty()) {
    return false;
  }

  const std::string finalPath = cachePath + rulesCache;
  const std::string tmpPath = cachePath + rulesCacheTmp;
  const std::string backupPath = cachePath + rulesCacheBackup;

  Storage.remove(tmpPath.c_str());

  FsFile file;
  if (!Storage.openFileForWrite("CSS", tmpPath, file)) {
    return false;
  }

  bool writeOk = true;
  auto writeBytes = [&file, &writeOk](const void* data, const size_t len) -> bool {
    if (!writeOk) return false;
    if (len == 0) return true;
    if (file.write(reinterpret_cast<const uint8_t*>(data), len) != len) {
      writeOk = false;
    }
    return writeOk;
  };
  auto writeByte = [&writeBytes](const uint8_t value) -> bool { return writeBytes(&value, sizeof(value)); };

  // Write header
  const uint32_t magic = CssParser::CSS_CACHE_MAGIC;
  writeBytes(&magic, sizeof(magic));
  writeByte(CssParser::CSS_CACHE_VERSION);
  writeByte(static_cast<uint8_t>(complete ? 0 : CSS_CACHE_FLAG_PARTIAL));

  // Write rule count
  const auto ruleCount = static_cast<uint16_t>(parsedRuleCount());
  writeBytes(&ruleCount, sizeof(ruleCount));

  Arena indexArena;
  if (!indexArena.init(4096)) {
    LOG_ERR("CSS", "Failed to allocate selector index arena");
    file.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }
  ArenaVector<SelectorEntry> indexEntries(indexArena);
  if (!indexEntries.reserve(ruleCount)) {
    LOG_ERR("CSS", "Failed to reserve selector index (%u rules)", ruleCount);
    file.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }
  const SelectorEntry zeroEntry{0, 0};
  for (uint16_t i = 0; i < ruleCount; ++i) {
    writeBytes(&zeroEntry, sizeof(zeroEntry));
  }

  // Write each simple rule: selector string + CssStyle fields
  auto writeRule = [&](const std::string_view selector, const CssStyle& style) {
    const uint32_t ruleOffset = file.position();
    const auto selectorLen = static_cast<uint16_t>(selector.size());
    if (!writeBytes(&selectorLen, sizeof(selectorLen)) || !writeBytes(selector.data(), selectorLen) ||
        !writeCssStylePayload(file, style)) {
      writeOk = false;
      return;
    }
    if (!indexEntries.push_back({selectorHash(selector), ruleOffset})) {
      writeOk = false;
    }
  };
  for (const auto& pair : rulesBySelector_) {
    writeRule(pair.first, pair.second);
    if (!writeOk) break;
  }
  for (const auto* rule = parsedRuleHead_; rule && writeOk; rule = rule->next) {
    writeRule(rule->selector, rule->style);
  }

  // Write descendant rules: count, then (ancestorSelector, subjectSelector, CssStyle) per entry
  const auto descendantCount = static_cast<uint16_t>(descendantRules_.size());
  writeBytes(&descendantCount, sizeof(descendantCount));
  for (const auto& rule : descendantRules_) {
    const auto ancLen = static_cast<uint16_t>(rule.ancestorSelector.size());
    if (!writeBytes(&ancLen, sizeof(ancLen)) || !writeBytes(rule.ancestorSelector.data(), ancLen)) {
      writeOk = false;
      break;
    }
    const auto subLen = static_cast<uint16_t>(rule.subjectSelector.size());
    if (!writeBytes(&subLen, sizeof(subLen)) || !writeBytes(rule.subjectSelector.data(), subLen) ||
        !writeCssStylePayload(file, rule.style)) {
      writeOk = false;
      break;
    }
  }

  writeOk = writeOk && writeFontFaces(file);
  if (!writeOk) {
    LOG_ERR("CSS", "Failed to write temporary CSS cache");
    file.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }

  std::sort(indexEntries.begin(), indexEntries.end(),
            [](const SelectorEntry& a, const SelectorEntry& b) { return a.hash < b.hash; });
  if (!file.seek(sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint16_t))) {
    LOG_ERR("CSS", "Failed to seek CSS index placeholder");
    file.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }
  for (const auto& entry : indexEntries) {
    if (!writeBytes(&entry, sizeof(entry))) {
      break;
    }
  }
  if (!writeOk) {
    LOG_ERR("CSS", "Failed to patch CSS index");
    file.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (!file.close()) {
    Storage.remove(tmpPath.c_str());
    return false;
  }

  Storage.remove(backupPath.c_str());
  const bool hadExistingCache = Storage.exists(finalPath.c_str());
  if (hadExistingCache && !Storage.rename(finalPath.c_str(), backupPath.c_str())) {
    LOG_ERR("CSS", "Failed to backup existing CSS cache before replace");
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
    LOG_ERR("CSS", "Failed to promote temporary CSS cache");
    Storage.remove(tmpPath.c_str());
    if (hadExistingCache) {
      Storage.rename(backupPath.c_str(), finalPath.c_str());
    }
    return false;
  }

  if (hadExistingCache) {
    Storage.remove(backupPath.c_str());
  }

  return true;
}

bool CssParser::loadFromCache() {
  if (cachePath.empty()) {
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("CSS", cachePath + rulesCache, file)) {
    return false;
  }
  struct FileGuard {
    FsFile& f;
    explicit FileGuard(FsFile& f) : f(f) {}
    // Ensure we only close an open file.
    ~FileGuard() {
      if (f.isOpen()) f.close();
    }
  } fileGuard(file);

  // Clear existing rules
  clear();
  cachePartial_ = false;

  // Read and verify header
  uint32_t magic = 0;
  if (file.read(&magic, sizeof(magic)) != sizeof(magic) || magic != CssParser::CSS_CACHE_MAGIC) {
    LOG_DBG("CSS", "Cache magic mismatch, removing stale cache for rebuild");
    file.close();
    Storage.remove((cachePath + rulesCache).c_str());
    return false;
  }

  uint8_t version = 0;
  if (file.read(&version, 1) != 1 || version != CssParser::CSS_CACHE_VERSION) {
    LOG_DBG("CSS", "Cache version mismatch (got %u, expected %u), removing stale cache for rebuild", version,
            CssParser::CSS_CACHE_VERSION);
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove((cachePath + rulesCache).c_str());
    return false;
  }

  uint8_t flags = 0;
  if (file.read(&flags, 1) != 1) {
    LOG_DBG("CSS", "Cache flags missing, removing stale cache for rebuild");
    file.close();
    Storage.remove((cachePath + rulesCache).c_str());
    return false;
  }
  if ((flags & ~CSS_CACHE_FLAG_PARTIAL) != 0) {
    LOG_DBG("CSS", "Unsupported CSS cache flags 0x%02X, removing stale cache for rebuild", flags);
    file.close();
    Storage.remove((cachePath + rulesCache).c_str());
    return false;
  }
  cachePartial_ = (flags & CSS_CACHE_FLAG_PARTIAL) != 0;

  // Read rule count
  uint16_t ruleCount = 0;
  if (file.read(&ruleCount, sizeof(ruleCount)) != sizeof(ruleCount)) {
    return false;
  }

  if (ruleCount > MAX_RULES) {
    LOG_DBG("CSS", "Invalid cache rule count (%u > %zu)", ruleCount, MAX_RULES);
    rulesBySelector_.clear();
    return false;
  }

  // Size the bucket array up front to avoid incremental rehashes while loading rules.
  rulesBySelector_.reserve(ruleCount);

  auto hasRemainingBytes = [&file](const size_t neededBytes) -> bool {
    return static_cast<size_t>(file.available()) >= neededBytes;
  };

  cacheRuleOffsets_.reserve(ruleCount);
  for (uint16_t i = 0; i < ruleCount; ++i) {
    SelectorEntry entry{};
    if (file.read(&entry, sizeof(entry)) != sizeof(entry)) {
      LOG_DBG("CSS", "Truncated CSS cache while reading selector index");
      cacheRuleOffsets_.clear();
      return false;
    }
    cacheRuleOffsets_.push_back(entry);
  }
  cacheIndexLoaded_ = true;
  cachedRuleCount_ = cacheRuleOffsets_.size();

  bool hydrateSimpleRules = false;
  size_t hydratedRuleCount = 0;
  const size_t freeHeapBeforeHydrate = ESP.getFreeHeap();
  const size_t arenaBytes = (static_cast<size_t>(ruleCount) * sizeof(CachedRule)) + CSS_RULE_ARENA_EXTRA_BYTES;
  const bool external = psramHeapAvailable();
  // The index and descendant/STL containers remain internal. Only remove the
  // arena payload from their budget, never their existing 80 KiB reserve.
  const bool admitted = external ? MemoryBudget::canAllocateInternal(0, CSS_RULE_ARENA_MIN_FREE_AFTER_ALLOC,
                                                                     MIN_LARGEST_BLOCK_FOR_RULE_GROWTH)
                                 : freeHeapBeforeHydrate >= MIN_FREE_HEAP_FOR_CSS_RULE_ARENA &&
                                       freeHeapBeforeHydrate >= arenaBytes + CSS_RULE_ARENA_MIN_FREE_AFTER_ALLOC;
  if (ruleCount > 0 && admitted) {
    if (cachedRuleArena_.init(arenaBytes)) {
      cachedRules_ = arenaNewArray<CachedRule>(cachedRuleArena_, ruleCount);
      hydrateSimpleRules = cachedRules_ != nullptr;
      if (!hydrateSimpleRules) {
        cachedRuleArena_.release();
        cachedRules_ = nullptr;
      }
    }
  }
  if (ruleCount > 0) {
    LOG_DBG("CSS", "Rule arena: bytes=%u pool=%s psramReserve=%u; %s", unsigned(arenaBytes),
            memoryPoolName(cachedRuleArena_.head ? cachedRuleArena_.head->pool : MemoryPool::None),
            unsigned(MemoryBudget::EPUB_PSRAM_RESERVE), hydrateSimpleRules ? "hydrated" : "disk index");
  }

  // Read each simple rule payload. When heap allows, hydrate into an arena-backed
  // table so resolveStyle() can stay in RAM instead of seeking the SD cache for
  // every selector lookup during page building. Selector text is only needed
  // while computing the compact lookup fingerprints, so it stays on the stack.
  char selectorBuf[MAX_SELECTOR_LENGTH];
  for (uint16_t i = 0; i < ruleCount; ++i) {
    const uint32_t recordStart = file.position();
    uint16_t selectorLen = 0;
    if (!hasRemainingBytes(sizeof(selectorLen)) ||
        file.read(&selectorLen, sizeof(selectorLen)) != sizeof(selectorLen)) {
      cacheRuleOffsets_.clear();
      return false;
    }
    if (selectorLen == 0 || selectorLen > MAX_SELECTOR_LENGTH ||
        !hasRemainingBytes(static_cast<size_t>(selectorLen) + CSS_FIXED_STYLE_BYTES)) {
      LOG_DBG("CSS", "Invalid selector length in cache: %u", selectorLen);
      cacheRuleOffsets_.clear();
      return false;
    }
    const uint32_t nextRecord = recordStart + sizeof(selectorLen) + selectorLen + CSS_FIXED_STYLE_BYTES;

    if (hydrateSimpleRules) {
      CssStyle style;
      if (file.read(selectorBuf, selectorLen) != selectorLen || !readCssStylePayload(file, style)) {
        LOG_DBG("CSS", "Truncated CSS cache while hydrating selector rule");
        cacheRuleOffsets_.clear();
        cachedRuleArena_.release();
        cachedRules_ = nullptr;
        cachedRuleTableCount_ = 0;
        return false;
      }
      const std::string_view selectorView(selectorBuf, selectorLen);
      cachedRules_[hydratedRuleCount++] = {selectorHash(selectorView), selectorSecondaryHash(selectorView), selectorLen,
                                           style};
    } else {
      if (!file.seek(nextRecord)) {
        cacheRuleOffsets_.clear();
        return false;
      }
    }
  }
  if (hydrateSimpleRules) {
    cachedRuleTableCount_ = hydratedRuleCount;
    std::sort(cachedRules_, cachedRules_ + cachedRuleTableCount_, [](const CachedRule& a, const CachedRule& b) {
      if (a.hash != b.hash) return a.hash < b.hash;
      if (a.secondaryHash != b.secondaryHash) return a.secondaryHash < b.secondaryHash;
      return a.selectorLen < b.selectorLen;
    });
    for (size_t i = 1; i < cachedRuleTableCount_; ++i) {
      const auto& prev = cachedRules_[i - 1];
      const auto& current = cachedRules_[i];
      if (prev.hash == current.hash && prev.secondaryHash == current.secondaryHash &&
          prev.selectorLen == current.selectorLen) {
        LOG_DBG("CSS", "CSS rule fingerprint collision; using disk-backed selector lookup");
        cachedRuleArena_.release();
        cachedRules_ = nullptr;
        cachedRuleTableCount_ = 0;
        break;
      }
    }
  }

  // Read descendant rules
  uint16_t descendantCount = 0;
  if (file.available() > 0) {
    if (file.read(&descendantCount, sizeof(descendantCount)) != sizeof(descendantCount)) {
      LOG_DBG("CSS", "Truncated CSS cache reading descendant count");
      rulesBySelector_.clear();
      return false;
    }
    if (descendantCount > MAX_DESCENDANT_RULES) {
      LOG_DBG("CSS", "Invalid descendant rule count (%u > %zu)", descendantCount, MAX_DESCENDANT_RULES);
      rulesBySelector_.clear();
      return false;
    }
    descendantRules_.reserve(descendantCount);
    for (uint16_t i = 0; i < descendantCount; ++i) {
      auto readStr = [&](std::string& out) -> bool {
        uint16_t len = 0;
        if (file.read(&len, sizeof(len)) != sizeof(len)) return false;
        if (len == 0 || len > MAX_SELECTOR_LENGTH || !hasRemainingBytes(len)) return false;
        out.resize(len);
        return file.read(&out[0], len) == len;
      };
      DescendantRule rule;
      if (!readStr(rule.ancestorSelector) || !readStr(rule.subjectSelector)) {
        LOG_DBG("CSS", "Truncated CSS cache reading descendant rule selectors");
        rulesBySelector_.clear();
        descendantRules_.clear();
        return false;
      }
      if (!hasRemainingBytes(CSS_FIXED_STYLE_BYTES) || !readCssStylePayload(file, rule.style)) {
        LOG_DBG("CSS", "Truncated CSS cache reading descendant rule style");
        rulesBySelector_.clear();
        descendantRules_.clear();
        return false;
      }
      descendantRules_.push_back(std::move(rule));
    }
  }

  if (!readFontFaces(file, nullptr, &resources_)) return false;
  LOG_DBG("CSS", "Loaded %u indexed rules + %u descendant rules from %s cache", static_cast<unsigned>(cachedRuleCount_),
          descendantCount, cachePartial_ ? "partial" : "complete");
  return true;
}

uint32_t CssParser::hashFontFamily(std::string_view name) { return CssFontCatalog::namedToken(name); }
CssFontCatalog& CssParser::mutableFontCatalog() const {
  if (fontCatalogOwner_) return fontCatalogOwner_->mutableFontCatalog();
  if (!fontCatalogLoaded_) {
    if (!hasCache())
      fontCatalogLoaded_ = true;
    else if (MemoryBudget::canAllocateInternal(CssFontCatalog::MAX_TEXT_BYTES + 8192, 64 * 1024, 8 * 1024))
      fontCatalogLoaded_ = loadFontCatalog(cachePath, fontCatalog_);
    // Failed hydration remains retryable when memory or SD access recovers.
  }
  return fontCatalog_;
}
const CssFontCatalog& CssParser::fontCatalog() const { return mutableFontCatalog(); }
void CssParser::resetFontCatalog() const {
  if (fontCatalogOwner_) return;
  fontCatalog_.reset();
  fontCatalogLoaded_ = true;
  CssFontCatalog::removeFiles(cachePath);
}
bool CssParser::saveFontCatalog() const {
  return fontCatalogOwner_ ? fontCatalogOwner_->saveFontCatalog() : mutableFontCatalog().save(cachePath);
}
CssStyle CssParser::parseInlineStyleWithFonts(std::string_view text, std::optional<std::string_view> base) const {
  auto& catalog = mutableFontCatalog();
  const size_t previous = catalog.stackCount();
  CssStyle style = parseDeclarations(text, this, base);
  if (catalog.stackCount() != previous && !saveFontCatalog()) {
    while (catalog.stackCount() > previous) catalog.discardLastStack();
    if (style.defined.fontFamily && !catalog.stack(style.fontFamily) && !genericFont::isGeneric(style.fontFamily))
      style.fontFamily = 0;
  }
  return style;
}
bool CssParser::parseFontFace(std::string_view declarations, std::string_view baseDir) {
  if (!MemoryBudget::canAllocateInternal(8192, MIN_FREE_HEAP_FOR_RULE_GROWTH, MIN_LARGEST_BLOCK_FOR_RULE_GROWTH))
    return false;
  CssFontCatalog::Face face;
  // Delimiters inside CSS strings/functions are not declaration separators.
  size_t start = 0;
  char quote = 0;
  unsigned depth = 0;
  bool escaped = false;
  for (size_t i = 0; i <= declarations.size(); ++i) {
    const char c = i < declarations.size() ? declarations[i] : ';';
    if (escaped) {
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    if (quote) {
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
      continue;
    }
    if (c == '(')
      ++depth;
    else if (c == ')' && depth)
      --depth;
    if (c == ';' && !depth) {
      CssFontCatalog::parseFaceDeclaration(face, declarations.substr(start, i - start), baseDir);
      start = i + 1;
    }
  }
  if (!fontScope_.empty() && !face.familyName.empty()) face.familyName = fontScope_ + face.familyName;
  mutableFontCatalog().addFace(std::move(face));
  return true;
}
bool CssParser::writeFontFaces(HalFile& file) const {
  const uint32_t offset = file.position();
  return writeResources(file) && mutableFontCatalog().write(file) &&
         file.write(reinterpret_cast<const uint8_t*>(&offset), sizeof(offset)) == sizeof(offset);
}
bool CssParser::readFontFaces(HalFile& file, std::vector<CssFontFace>* out, std::vector<Resource>* resources) {
  const uint32_t start = file.position();
  if (!readResources(file, resources)) return false;
  CssFontCatalog catalog;
  if (out ? !catalog.read(file) : !CssFontCatalog::inspect(file)) return false;
  uint32_t offset = 0;
  if (file.read(&offset, sizeof(offset)) != sizeof(offset) || offset != start || file.available()) return false;
  if (out) {
    out->reserve(catalog.faces().size());
    for (const auto& face : catalog.faces())
      out->push_back(
          {face.family, face.weight >= 600, face.style != CssFontCatalog::Style::Normal, face.sources.front().path});
  }
  return true;
}
bool CssParser::loadFontCatalog(const std::string& cachePath, CssFontCatalog& out) {
  out.reset();
  HalFile file;
  if (!Storage.openFileForRead("CSS", cachePath + rulesCache, file)) return false;
  uint32_t magic = 0, offset = 0, tail = 0;
  uint8_t version = 0;
  const bool ok = file.read(&magic, 4) == 4 && magic == CSS_CACHE_MAGIC && file.read(&version, 1) == 1 &&
                  version == CSS_CACHE_VERSION && file.size() >= 15 && file.seek(file.size() - 4) &&
                  file.read(&offset, 4) == 4 && offset >= 10 && offset < file.size() - 4 && file.seek(offset) &&
                  readResources(file) && out.read(file) && file.read(&tail, 4) == 4 && tail == offset &&
                  !file.available();
  file.close();
  if (!ok) {
    out.reset();
    return false;
  }
  CssFontCatalog extension;
  if (extension.load(cachePath, out.residentBytes()) && extension.isExtensionOf(out)) out = std::move(extension);
  return true;
}
bool CssParser::loadFontFaces(const std::string& cachePath, std::vector<CssFontFace>& out) {
  out.clear();
  CssFontCatalog catalog;
  if (!loadFontCatalog(cachePath, catalog)) return false;
  out.reserve(catalog.faces().size());
  for (const auto& face : catalog.faces())
    out.push_back(
        {face.family, face.weight >= 600, face.style != CssFontCatalog::Style::Normal, face.sources.front().path});
  return true;
}

namespace {
int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
bool appendCodepoint(std::string& out, uint32_t cp) {
  if (cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
  if (cp < 0x80)
    out.push_back(cp);
  else if (cp < 0x800) {
    out.push_back(0xc0 | (cp >> 6));
    out.push_back(0x80 | (cp & 63));
  } else if (cp < 0x10000) {
    out.push_back(0xe0 | (cp >> 12));
    out.push_back(0x80 | ((cp >> 6) & 63));
    out.push_back(0x80 | (cp & 63));
  } else {
    out.push_back(0xf0 | (cp >> 18));
    out.push_back(0x80 | ((cp >> 12) & 63));
    out.push_back(0x80 | ((cp >> 6) & 63));
    out.push_back(0x80 | (cp & 63));
  }
  return out.size() <= 128;
}
std::optional<std::string> parseLiteralContent(std::string_view value) {
  // Keywords are case-insensitive; quoted content is never lowercased or whitespace-normalized.
  std::string keyword(value);
  for (char& c : keyword) c = std::tolower(static_cast<unsigned char>(c));
  keyword.resize(stripTrailingImportant(keyword).size());
  if (keyword == "none" || keyword == "normal" || keyword == "initial" || keyword == "unset") return std::string();
  std::string out;
  out.reserve(std::min<size_t>(128, value.size()));
  bool hadString = false;
  while (!value.empty()) {
    value = trimCssWhitespace(value);
    if (value.size() == 10) {
      bool important = true;
      constexpr char marker[] = "!important";
      for (size_t i = 0; i < 10; ++i)
        if (std::tolower(static_cast<unsigned char>(value[i])) != marker[i]) important = false;
      if (important) break;
    }
    if (value.empty()) break;
    const char quote = value.front();
    if (quote != '\'' && quote != '"') return std::nullopt;
    value.remove_prefix(1);
    hadString = true;
    bool closed = false;
    while (!value.empty()) {
      char c = value.front();
      value.remove_prefix(1);
      if (c == quote) {
        closed = true;
        break;
      }
      if (c == '\n' || c == '\r' || c == '\f') return std::nullopt;
      if (c == '\\') {
        if (value.empty()) return std::nullopt;
        if (value.front() == '\n' || value.front() == '\r' || value.front() == '\f') {
          char line = value.front();
          value.remove_prefix(1);
          if (line == '\r' && !value.empty() && value.front() == '\n') value.remove_prefix(1);
          continue;
        }
        if (hexDigit(value.front()) >= 0) {
          uint32_t cp = 0;
          int n = 0;
          while (!value.empty() && n < 6 && hexDigit(value.front()) >= 0) {
            cp = (cp << 4) | hexDigit(value.front());
            value.remove_prefix(1);
            ++n;
          }
          if (!value.empty() && isCssWhitespace(value.front())) {
            char ws = value.front();
            value.remove_prefix(1);
            if (ws == '\r' && !value.empty() && value.front() == '\n') value.remove_prefix(1);
          }
          if (!appendCodepoint(out, cp)) return std::nullopt;
          continue;
        }
        c = value.front();
        value.remove_prefix(1);
      }
      out.push_back(c);
      if (out.size() > 128) return std::nullopt;
    }
    if (!closed) return std::nullopt;
  }
  return hadString ? std::optional<std::string>(std::move(out)) : std::nullopt;
}

uint32_t resourceToken(uint8_t kind, std::string_view value) {
  uint32_t h = (2166136261u ^ kind) * 16777619u;
  for (unsigned char c : value) h = (h ^ c) * 16777619u;
  return h ? h : 1;
}
}  // namespace
uint32_t CssParser::internResource(uint8_t kind, std::string text) const {
  if (text.empty()) return 0;
  const auto token = resourceToken(kind, text);
  size_t total = text.size();
  for (const auto& r : resources_) {
    if (r.token == token) {
      if (r.kind == kind && r.text == text) return token;
      return UINT32_MAX;
    }
    total += r.text.size();
  }
  if (resources_.size() >= 128 || total > 16384 ||
      !MemoryBudget::canAllocateInternal(text.size() + sizeof(Resource) * (resources_.size() + 1),
                                         MIN_FREE_HEAP_FOR_RULE_GROWTH, MIN_LARGEST_BLOCK_FOR_RULE_GROWTH))
    return UINT32_MAX;
  resources_.reserve(resources_.size() + 1);
  resources_.push_back({token, kind, std::move(text)});
  return token;
}
const std::string* CssParser::resource(uint32_t token, uint8_t kind) const {
  for (const auto& r : resources_)
    if (r.token == token && r.kind == kind) return &r.text;
  return nullptr;
}
void CssParser::parseResourceDeclaration(std::string_view name, std::string_view value, CssStyle& style,
                                         std::optional<std::string_view> base) const {
  if (iequalsAscii(name, "content")) {
    if (auto literal = parseLiteralContent(value)) {
      const auto token = internResource(1, std::move(*literal));
      if (token != UINT32_MAX) {
        style.pseudoContent = token;
        style.defined.pseudoContent = 1;
      }
    }
    return;
  }
  if (!iequalsAscii(name, "background") && !iequalsAscii(name, "background-image")) return;
  bool reset = iequalsAscii(value, "none") || iequalsAscii(value, "initial") || iequalsAscii(value, "unset") ||
               iequalsAscii(name, "background");
  for (size_t i = 0; i + 4 <= value.size(); ++i) {
    if (!iequalsAscii(value.substr(i, 4), "url(")) continue;
    size_t end = i + 4;
    char quote = 0;
    bool escaped = false;
    for (; end < value.size(); ++end) {
      char c = value[end];
      if (escaped) {
        escaped = false;
        continue;
      }
      if (c == '\\') {
        escaped = true;
        continue;
      }
      if (quote) {
        if (c == quote) quote = 0;
        continue;
      }
      if (c == '\'' || c == '"') {
        quote = c;
        continue;
      }
      if (c == ')') break;
    }
    if (end == value.size()) return;
    std::string path = CssFontCatalog::resolvePath(base.value_or(stylesheetBase_), value.substr(i + 4, end - i - 4));
    if (path.empty()) return;
    const auto token = internResource(2, std::move(path));
    if (token == UINT32_MAX) return;
    style.backgroundImage = token;
    style.defined.backgroundImage = 1;
    reset = false;
    if (iequalsAscii(name, "background")) {
      style.backgroundRepeat = CssBackgroundRepeat::Repeat;
      style.defined.backgroundRepeat = 1;
      forEachDelimitedToken(value.substr(end + 1), isCssWhitespace,
                            [&](std::string_view token) { cssExtras::parse("background-repeat", token, style); });
    }
    break;
  }
  if (reset) {
    style.backgroundImage = 0;
    style.defined.backgroundImage = 1;
  }
}
bool CssParser::writeResources(HalFile& file) const {
  const uint16_t count = resources_.size();
  if (file.write(reinterpret_cast<const uint8_t*>(&count), 2) != 2) return false;
  for (const auto& r : resources_) {
    const uint16_t n = r.text.size();
    if (file.write(reinterpret_cast<const uint8_t*>(&r.token), 4) != 4 || file.write(r.kind) != 1 ||
        file.write(reinterpret_cast<const uint8_t*>(&n), 2) != 2 ||
        file.write(reinterpret_cast<const uint8_t*>(r.text.data()), n) != n)
      return false;
  }
  return true;
}
bool CssParser::readResources(HalFile& file, std::vector<Resource>* out) {
  uint16_t count = 0;
  if (file.read(&count, 2) != 2 || count > 128) return false;
  if (out) {
    out->clear();
    if (!MemoryBudget::canAllocateInternal(count * sizeof(Resource) + 16384, MIN_FREE_HEAP_FOR_CSS, 8192)) return false;
    out->reserve(count);
  }
  size_t total = 0;
  for (size_t i = 0; i < count; ++i) {
    uint32_t token = 0;
    uint16_t n = 0;
    uint8_t kind = 0;
    if (file.read(&token, 4) != 4 || !token || file.read(&kind, 1) != 1 || kind < 1 || kind > 2 ||
        file.read(&n, 2) != 2 || !n || n > (kind == 1 ? 128 : 512) || total + n > 16384)
      return false;
    total += n;
    uint32_t hash = (2166136261u ^ kind) * 16777619u;
    std::string text;
    if (out) text.reserve(n);
    uint8_t buffer[128];
    size_t remaining = n;
    while (remaining) {
      size_t chunk = std::min(remaining, sizeof(buffer));
      if (file.read(buffer, chunk) != static_cast<int>(chunk) || std::memchr(buffer, 0, chunk)) return false;
      for (size_t j = 0; j < chunk; ++j) hash = (hash ^ buffer[j]) * 16777619u;
      if (out) text.append(reinterpret_cast<const char*>(buffer), chunk);
      remaining -= chunk;
    }
    if (token != (hash ? hash : 1)) return false;
    if (out) {
      for (const auto& r : *out)
        if (r.token == token) return false;
      out->push_back({token, kind, std::move(text)});
    }
  }
  return true;
}

std::optional<std::string> CssParser::backgroundImageForStyle(const CssStyle& style) const {
  if (!style.defined.backgroundImage) return {};
  if (!style.backgroundImage) return std::string{};
  if (const auto* found = resource(style.backgroundImage, 2)) return *found;
  return fontCatalogOwner_ ? fontCatalogOwner_->backgroundImageForStyle(style) : std::nullopt;
}
