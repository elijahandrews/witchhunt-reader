#include "ChapterHtmlSlimParser.h"

#include <Arduino.h>
#include <Epub/LineHeight.h>
#include <FontTypography.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <UniqueCodepointSet.h>
#include <Utf8.h>
#include <expat.h>
#include <strings.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <new>
#include <string_view>

#include "../../../ScalableFont/ScalableFontSizing.h"
#include "../SourceTextTransform.h"
#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/ImageDimsProbe.h"
#include "Epub/converters/ImageToFramebufferDecoder.h"
#include "Epub/htmlEntities.h"
#include "Epub/tables/TableColumnLayout.h"
#include "PreviewBlockLocator.h"

// Minimum file size (in bytes) to show indexing popup - smaller chapters don't benefit from it
constexpr size_t MIN_SIZE_FOR_POPUP = 10 * 1024;  // 10KB
constexpr size_t PARSE_BUFFER_SIZE = 1024;
// Initial slab for the bounded style stacks. The fallible arena adds a slab if their layout grows.
constexpr size_t PARSE_ARENA_SLAB_SIZE = 4 * 1024;
constexpr size_t DEFAULT_BUFFERED_WORDS_BEFORE_LAYOUT = 350;
constexpr size_t CSS_BUFFERED_WORDS_BEFORE_LAYOUT = 320;
constexpr uint16_t DEFAULT_TEXT_RUN_BYTES_BEFORE_LAYOUT = 2048;
constexpr size_t SINGLE_READING_AID_BUFFERED_WORDS_BEFORE_LAYOUT = 240;
constexpr uint16_t SINGLE_READING_AID_TEXT_RUN_BYTES_BEFORE_LAYOUT = 1536;
constexpr size_t COMBINED_READING_AID_BUFFERED_WORDS_BEFORE_LAYOUT = 175;
constexpr uint16_t COMBINED_READING_AID_TEXT_RUN_BYTES_BEFORE_LAYOUT = 1024;
constexpr size_t SECTION_ADVANCE_PREWARM_READ_BUFFER_SIZE = 512;
constexpr uint32_t SECTION_ADVANCE_PREWARM_MAX_CODEPOINTS = 4096;
// Deferred prewarm slices: scan HTML, or load advances in small batches, for up to
// this long per background build tick, so a tick never blocks input for long.
constexpr uint32_t SECTION_ADVANCE_PREWARM_STEP_MS = 8;
constexpr uint32_t SECTION_ADVANCE_PREWARM_FEED_BATCH = 8;
// SdCardFont::ADVANCE_CACHE_LIMIT: the per-style table holds this many, and a miss on a
// full table clears it, so feeding more in batches would only churn it.
constexpr uint32_t SECTION_ADVANCE_PREWARM_FEED_LIMIT = 254;  // Leave room for space and hyphen.
// The whole-section advance prewarm is a batch-I/O optimization, not a requirement:
// layout falls back to per-paragraph advance loads (ParsedText loads glyph metrics per
// block, masked to the styles that block uses). Running it costs a 16KB contiguous
// codepoint scratch array plus the resident per-style advance tables (~30KB total
// observed), on top of the 44KB layout floor. 80KB keeps the prewarm for ordinary
// chapter opens (observed surviving from 81KB free) and skips it for low-heap
// extension starts (observed aborting the build when run from 70KB free).
constexpr uint32_t MIN_FREE_HEAP_FOR_SECTION_PREWARM = 80 * 1024;
constexpr uint32_t MIN_MAX_ALLOC_FOR_SECTION_PREWARM = 24 * 1024;
constexpr uint8_t INITIAL_PAGE_ELEMENT_RESERVE = 8;
constexpr uint8_t INITIAL_TABLE_FRAGMENT_ROW_RESERVE = 8;
constexpr uint32_t PAGE_ELEMENT_RESERVE_MIN_MAX_ALLOC = 1024;
constexpr uint16_t WARNING_PAGE_SIDE_MARGIN = 24;
constexpr uint8_t WARNING_TITLE_MAX_LINES = 2;
constexpr uint8_t WARNING_BODY_MAX_LINES = 6;
// Cap chapter anchors so converter-generated IDs do not grow memory without bound.
constexpr size_t MAX_ANCHORS_PER_CHAPTER = 1024;
constexpr size_t MAX_PENDING_FOOTNOTES_BEFORE_LAYOUT = Page::MAX_FOOTNOTES_PER_PAGE * 3;
// Rich tables retain ParsedText and per-cell vectors until the closing tag. The
// C3 SD-font resume path normally has only about 85-90 KiB free and a 49 KiB
// largest block, so leave that baseline to the compact row model as well.
// Select before the first cell is captured; never build both representations
// and retry after an OOM.
constexpr uint32_t MIN_FREE_HEAP_FOR_RICH_TABLE = 96U * 1024U;
constexpr uint32_t MIN_MAX_ALLOC_FOR_RICH_TABLE = 56U * 1024U;

static constexpr const char* const HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
static constexpr const char* const BLOCK_TAGS[] = {"p",       "li",      "div",    "br",       "blockquote",
                                                   "ul",      "ol",      "figure", "fieldset", "pre",
                                                   "section", "article", "aside",  "main",     "body"};
static constexpr const char* const BOLD_TAGS[] = {"b", "strong"};
static constexpr const char* const ITALIC_TAGS[] = {"i", "em"};
static constexpr const char* const UNDERLINE_TAGS[] = {"u", "ins"};
static constexpr const char* const STRIKETHROUGH_TAGS[] = {"s", "strike", "del"};
static constexpr const char* const IMAGE_TAGS[] = {"img", "image"};
static constexpr const char* const SKIP_TAGS[] = {"head", "style", "script", "title", "rp", "rt"};

constexpr bool isReferenceWhitespaceCodepoint(const uint32_t codepoint) {
  return codepoint == 0x0009 || codepoint == 0x000A || codepoint == 0x000B || codepoint == 0x000C ||
         codepoint == 0x000D || codepoint == 0x0020 || codepoint == 0x00A0 || codepoint == 0x1680 ||
         (codepoint >= 0x2000 && codepoint <= 0x200A) || codepoint == 0x2028 || codepoint == 0x2029 ||
         codepoint == 0x202F || codepoint == 0x205F || codepoint == 0x3000 || codepoint == 0xFEFF;
}

uint32_t ChapterHtmlSlimParser::consumeReferenceCodepoint(const uint32_t codepoint) {
  if (isReferenceWhitespaceCodepoint(codepoint)) {
    if (referenceTextStarted) referenceWhitespacePending = true;
    return referenceTextOffset;
  }
  if (referenceWhitespacePending) {
    referenceTextOffset++;
    referenceWhitespacePending = false;
  }
  const uint32_t offset = referenceTextOffset;
  referenceTextOffset++;
  referenceTextStarted = true;
  return offset;
}

void ChapterHtmlSlimParser::consumeReferenceCharacters(const XML_Char* text, const int length) {
  const auto* cursor = reinterpret_cast<const unsigned char*>(text);
  const auto* const end = cursor + length;
  while (cursor < end) consumeReferenceCodepoint(utf8NextCodepoint(&cursor));
}

void ChapterHtmlSlimParser::clearReferenceExclusionIfClosed() {
  while (layoutScopeCount_ && layoutScopes_[layoutScopeCount_ - 1].depth >= depth)
    inheritedLayout_ = layoutScopes_[--layoutScopeCount_].parent;
  while (floatScopeCount_ && floatScopes_[floatScopeCount_ - 1].depth >= depth) --floatScopeCount_;
  while (containerWidthCount_ && containerWidths_[containerWidthCount_ - 1].depth >= depth) --containerWidthCount_;
  if (skipTextUntilDepth == depth) skipTextUntilDepth = INT_MAX;
  if (trackReferenceCharacters && referenceExcludedUntilDepth == depth) {
    referenceExcludedUntilDepth = INT_MAX;
  }
}

bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

static char asciiLower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

const char* localTagName(const char* name) {
  const auto* colon = strrchr(name, ':');
  return colon ? colon + 1 : name;
}

void resetPrewarmUtf8(uint32_t& accumulator, uint8_t& remaining) {
  accumulator = 0;
  remaining = 0;
}

bool feedPrewarmUtf8Byte(const uint8_t byte, UniqueCodepointSet& codepoints, uint32_t& accumulator,
                         uint8_t& remaining) {
  if (remaining == 0) {
    if (byte < 0x80) {
      return codepoints.add(byte);
    }
    if ((byte & 0xE0) == 0xC0) {
      accumulator = byte & 0x1F;
      remaining = 1;
    } else if ((byte & 0xF0) == 0xE0) {
      accumulator = byte & 0x0F;
      remaining = 2;
    } else if ((byte & 0xF8) == 0xF0) {
      accumulator = byte & 0x07;
      remaining = 3;
    } else {
      return codepoints.add(REPLACEMENT_GLYPH);
    }
    return false;
  }

  if ((byte & 0xC0) != 0x80) {
    resetPrewarmUtf8(accumulator, remaining);
    return codepoints.add(REPLACEMENT_GLYPH);
  }

  accumulator = (accumulator << 6U) | (byte & 0x3FU);
  --remaining;
  if (remaining == 0) {
    const uint32_t cp = accumulator;
    accumulator = 0;
    return codepoints.add(cp);
  }
  return false;
}

static bool tokenEqualsIgnoreCase(const char* value, const char* token, const size_t tokenLen) {
  for (size_t i = 0; i < tokenLen; ++i) {
    if (asciiLower(value[i]) != asciiLower(token[i])) return false;
  }
  return true;
}

std::string trimAndNormalize(const std::string& str) {
  if (str.empty()) return "";
  size_t start = 0;
  while (start < str.size() && isWhitespace(str[start])) {
    start++;
  }
  if (start == str.size()) return "";
  size_t end = str.size() - 1;
  while (end > start && isWhitespace(str[end])) {
    end--;
  }
  std::string result;
  result.reserve(end - start + 1);
  bool inSpace = false;
  for (size_t i = start; i <= end; i++) {
    if (isWhitespace(str[i])) {
      if (!inSpace) {
        result.push_back(' ');
        inSpace = true;
      }
    } else {
      result.push_back(str[i]);
      inSpace = false;
    }
  }
  return result;
}

bool matches(const char* tag_name, const char* const* possible_tags, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(tag_name, possible_tags[i]) == 0) {
      return true;
    }
  }
  return false;
}

const char* getAttribute(const XML_Char** atts, const char* attrName) {
  if (!atts) return nullptr;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], attrName) == 0) return atts[i + 1];
  }
  return nullptr;
}

bool parseListValue(const char* value, int32_t& parsed) {
  if (!value || value[0] == '\0') return false;
  errno = 0;
  char* end = nullptr;
  const long candidate = std::strtol(value, &end, 10);
  while (end && isWhitespace(*end)) ++end;
  if (errno == ERANGE || end == value || (end && *end != '\0') || candidate < INT32_MIN || candidate > INT32_MAX) {
    return false;
  }
  parsed = static_cast<int32_t>(candidate);
  return true;
}

bool isNonNavigableInlineElement(const char* name) { return strcmp(name, "span") == 0; }

bool isInternalEpubLink(const char* href) {
  if (!href || href[0] == '\0') return false;
  if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) return false;
  if (strncmp(href, "mailto:", 7) == 0) return false;
  if (strncmp(href, "ftp://", 6) == 0) return false;
  if (strncmp(href, "tel:", 4) == 0) return false;
  if (strncmp(href, "javascript:", 11) == 0) return false;
  return true;
}

bool attributeContainsToken(const char* value, const char* token) {
  if (!value || !token || token[0] == '\0') return false;

  const size_t tokenLen = strlen(token);
  const char* pos = value;
  while (*pos != '\0') {
    while (isWhitespace(*pos)) {
      pos++;
    }
    const char* end = pos;
    while (*end != '\0' && !isWhitespace(*end)) {
      end++;
    }
    if (static_cast<size_t>(end - pos) == tokenLen && tokenEqualsIgnoreCase(pos, token, tokenLen)) {
      return true;
    }
    pos = end;
  }

  return false;
}

namespace {

// A prefix scan identifies paragraph-defined body fonts before any pages are laid out.
constexpr size_t BODY_FONT_SCAN_BYTES = 64 * 1024;

class BodyFontProbe {
  static constexpr size_t MAX_DEPTH = 16;
  static constexpr size_t MAX_FAMILIES = 33;  // 32 embedded faces plus the reader font
  struct Frame {
    uint32_t family = 0;
    uint32_t paragraphFamily = 0;
    CssTextAlign alignment = CssTextAlign::Justify;
    bool inBody = false;
    bool inParagraph = false;
    bool ignored = false;
  };
  struct Usage {
    uint32_t family = 0;
    uint32_t bytes = 0;
  };
  const CssParser& css;
  const CssParser* overlay;
  const bool useTextFamily;
  CssStyle resolvedStyle;
  std::vector<CssAncestorEntry> ancestors;
  std::array<Frame, MAX_DEPTH> frames{};
  std::array<Usage, MAX_FAMILIES> usage{};
  size_t depth = 0;
  uint32_t rootFamily = 0;

  static void XMLCALL start(void* context, const XML_Char* name, const XML_Char** attrs) {
    auto& self = *static_cast<BodyFontProbe*>(context);
    const size_t index = self.depth++;
    if (index >= MAX_DEPTH) return;
    std::string_view classes, id, inlineStyle;
    bool hidden = false;
    if (attrs) {
      for (size_t i = 0; attrs[i]; i += 2) {
        if (strcmp(attrs[i], "class") == 0) classes = attrs[i + 1];
        if (strcmp(attrs[i], "id") == 0) id = attrs[i + 1];
        if (strcmp(attrs[i], "style") == 0) inlineStyle = attrs[i + 1];
        if (strcmp(attrs[i], "hidden") == 0) hidden = true;
      }
    }
    auto& style = self.resolvedStyle;
    self.ancestors.resize(index);
    style = self.css.resolveStyle(name, classes, self.ancestors, id, false, self.overlay);
    self.ancestors.push_back({static_cast<int>(index), std::string(name), std::string(classes.substr(0, 255)),
                              std::string(id.substr(0, 255))});
    if (!inlineStyle.empty())
      style.applyOver((self.overlay ? self.overlay : &self.css)->parseInlineStyleWithFonts(inlineStyle));
    auto& frame = self.frames[index];
    frame = index == 0 ? Frame{} : self.frames[index - 1];
    if (style.defined.fontFamily && style.fontFamily != CssFontCatalog::Inherit) frame.family = style.fontFamily;
    if (style.hasTextAlign()) frame.alignment = style.textAlign;
    if (strcmp(name, "html") == 0 || strcmp(name, "body") == 0) {
      if (style.defined.fontFamily) self.rootFamily = style.fontFamily;
    }
    if (strcmp(name, "body") == 0) frame.inBody = true;
    frame.ignored = frame.ignored || hidden || style.display == CssDisplay::None || style.isElementHidden() ||
                    style.isTextTransparent() || style.isFontSizeInvisible() ||
                    matches(name, SKIP_TAGS, std::size(SKIP_TAGS)) || strcmp(name, "template") == 0 ||
                    matches(name, HEADER_TAGS, std::size(HEADER_TAGS)) || strcmp(name, "blockquote") == 0 ||
                    strcmp(name, "table") == 0 || strcmp(name, "nav") == 0 || strcmp(name, "aside") == 0 ||
                    strcmp(name, "figure") == 0 || strcmp(name, "pre") == 0;
    if (strcmp(name, "p") == 0) {
      frame.inParagraph = true;
      frame.paragraphFamily = frame.family;
      frame.ignored =
          frame.ignored || frame.alignment == CssTextAlign::Center || frame.alignment == CssTextAlign::Right;
    }
  }

  static void XMLCALL end(void* context, const XML_Char*) {
    auto& self = *static_cast<BodyFontProbe*>(context);
    if (self.depth > 0) --self.depth;
  }

  static void XMLCALL text(void* context, const XML_Char* text, const int length) {
    auto& self = *static_cast<BodyFontProbe*>(context);
    if (self.depth == 0 || self.depth > MAX_DEPTH) return;
    const auto& frame = self.frames[self.depth - 1];
    if (!frame.inBody || (!frame.inParagraph && !self.useTextFamily) || frame.ignored) return;
    uint32_t bytes = 0;
    for (int i = 0; i < length; ++i) {
      if (!isWhitespace(text[i])) ++bytes;
    }
    if (bytes == 0) return;
    const uint32_t family = self.useTextFamily ? frame.family : frame.paragraphFamily;
    for (auto& candidate : self.usage) {
      if (candidate.bytes == 0 || candidate.family == family) {
        candidate.family = family;
        candidate.bytes += bytes;
        return;
      }
    }
  }

  static void XMLCALL entity(void* context, const XML_Char* text, const int length) {
    if (length >= 3 && text[0] == '&' && text[length - 1] == ';') BodyFontProbe::text(context, "x", 1);
  }

 public:
  explicit BodyFontProbe(const CssParser& css, const bool useTextFamily = false, const CssParser* overlay = nullptr)
      : css(css), overlay(overlay), useTextFamily(useTextFamily) {
    ancestors.reserve(MAX_DEPTH);
  }

  bool attach(SaxParser& parser) { return parser.init(this, start, end, text, entity, true); }

  uint32_t family(const bool preferParagraph = false) const {
    if (!preferParagraph && rootFamily != 0) return rootFamily;
    const auto best =
        std::max_element(usage.begin(), usage.end(), [](const Usage& a, const Usage& b) { return a.bytes < b.bytes; });
    return best->bytes ? best->family : (preferParagraph ? 0 : rootFamily);
  }
};

}  // namespace

bool isHeaderOrBlock(const char* name) {
  return matches(name, HEADER_TAGS, std::size(HEADER_TAGS)) || matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS)) ||
         strcmp(name, "caption") == 0;
}

bool isTableStructuralTag(const char* name) {
  return strcmp(name, "table") == 0 || strcmp(name, "tr") == 0 || strcmp(name, "td") == 0 || strcmp(name, "th") == 0;
}

void stripPublisherSpacing(BlockStyle& blockStyle) {
  blockStyle.marginLeft = 0;
  blockStyle.marginRight = 0;
  blockStyle.marginTop = 0;
  blockStyle.marginBottom = 0;
  blockStyle.paddingLeft = 0;
  blockStyle.paddingRight = 0;
  blockStyle.paddingTop = 0;
  blockStyle.paddingBottom = 0;
  blockStyle.textIndentDefined = false;
  blockStyle.textIndent = 0;
}

void ChapterHtmlSlimParser::skipCurrentElement() {
  skipUntilDepth = depth;
  skipEndElementStateUntilDepth = depth;
  depth += 1;
}

void ChapterHtmlSlimParser::skipDescendantsOfCurrentElement() {
  skipUntilDepth = depth - 1;
  skipEndElementStateUntilDepth = depth;
}

void ChapterHtmlSlimParser::applyDirectionToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (css.hasDirection()) {
    entry.hasDirection = true;
    entry.direction = css.direction;
  }
}

void ChapterHtmlSlimParser::applyInlineFontSize(StyleStackEntry& entry, const CssStyle& css, const char* tag) {
  if (!embeddedStyle || isLightMode() || tableDepth || isHeaderOrBlock(tag) || !css.hasFontSize()) return;
  const auto base = renderer.getFontPointSize(fontId);
  if (!base) return;
  const float parent = effectiveInlinePoints64
                           ? effectiveInlinePoints64 / 64.0f
                           : (effectiveInlineFontSize ? effectiveInlineFontSize
                                                      : renderer.getFontPointSize64(currentTextFontId()) / 64.0f);
  float size = css.fontSize.value;
  switch (css.fontSize.unit) {
    case CssUnit::Em:
      size *= parent;
      break;
    case CssUnit::Percent:
      size *= parent / 100;
      break;
    case CssUnit::Rem:
      size *= base * rootFontScaleExact_;
      break;
    case CssUnit::Points:
      size *= base / 12.0f;
      break;
    case CssUnit::Pixels:
      size *= base / 16.0f;
      break;
    case CssUnit::Inherit:
    case CssUnit::Auto:
      size = parent;
      break;
  }
  if (!std::isfinite(size)) return;
  entry.points64 = static_cast<uint16_t>(std::round(std::clamp(size, 1.0f / 64, 128.0f) * 64));
  entry.fontSize = static_cast<uint8_t>((entry.points64 + 32) / 64);
}

void ChapterHtmlSlimParser::applyScriptSizing(StyleStackEntry& entry, const CssStyle& css) {
  if (!(entry.hasSup && entry.sup) && !(entry.hasSub && entry.sub)) return;
  const float base = renderer.getFontPointSize64(fontId) / 64.0f;
  if (!base) return;
  const float parent = effectiveInlinePoints64 ? effectiveInlinePoints64 / 64.0f
                                               : renderer.getFontPointSize64(currentTextFontId()) / 64.0f;
  float size = parent * 0.5f;
  if (embeddedStyle && !isLightMode() && css.hasFontSize()) {
    size = css.fontSize.value;
    switch (css.fontSize.unit) {
      case CssUnit::Em:
        size *= parent;
        break;
      case CssUnit::Rem:
        size *= base * rootFontScaleExact_;
        break;
      case CssUnit::Percent:
        size *= parent / 100.0f;
        break;
      case CssUnit::Points:
        size *= base / 12.0f;
        break;
      case CssUnit::Pixels:
        size *= base / 16.0f;
        break;
      default:
        size = parent;
        break;
    }
  }
  if (!std::isfinite(size)) return;
  entry.points64 = static_cast<uint16_t>(std::round(std::clamp(size, 1.0f / 64, 128.0f) * 64));
  entry.fontSize = static_cast<uint8_t>((entry.points64 + 32) / 64);
  entry.typography =
      (entry.hasTypography ? entry.typography : effectiveTypography) | fontTypography::ScriptSizeResolved;
  entry.hasTypography = true;
  if (embeddedStyle && !isLightMode() && css.hasLetterSpacing()) {
    entry.hasLetterSpacing = true;
    entry.letterSpacing = resolveLetterSpacing(css.letterSpacing, renderer.getFontIdForSize64(fontId, entry.points64));
  }
}

void ChapterHtmlSlimParser::applySmallCapsToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (css.hasLigatures()) {
    entry.hasTypography = true;
    entry.typography = effectiveTypography;
    if (css.ligatures == CssLigatures::None)
      entry.typography |= fontTypography::NoLigatures;
    else if (css.ligatures == CssLigatures::Normal)
      entry.typography &= ~fontTypography::NoLigatures;
  }
  if (css.hasTextTransform() && embeddedStyle && !isLightMode()) {
    entry.hasTextTransform = true;
    entry.textTransform = css.textTransform == CssTextTransform::Inherit ? effectiveTextTransform : css.textTransform;
  }
  if (css.hasLetterSpacing() && embeddedStyle && !isLightMode()) {
    entry.hasLetterSpacing = true;
    const int selectedFont = entry.points64 ? renderer.getFontIdForSize64(fontId, entry.points64) : currentTextFontId();
    entry.letterSpacing = resolveLetterSpacing(css.letterSpacing, selectedFont);
  }
  if (css.defined.fontFamily) {
    entry.hasFontFamily = true;
    entry.fontFamily = css.fontFamily == CssFontCatalog::Inherit ? effectiveFontFamily : css.fontFamily;
  }
  if (css.hasWhiteSpace()) {
    entry.hasWhiteSpace = true;
    entry.whiteSpace = css.whiteSpace == CssWhiteSpace::Inherit ? effectiveWhiteSpace : css.whiteSpace;
    entry.preserveWhitespace = entry.whiteSpace == CssWhiteSpace::Pre || entry.whiteSpace == CssWhiteSpace::PreWrap;
  }
  if (css.hasFontVariantCaps()) {
    entry.hasSmallCaps = true;
    entry.smallCaps = css.fontVariantCaps == CssFontVariantCaps::Inherit
                          ? effectiveSmallCaps
                          : css.fontVariantCaps == CssFontVariantCaps::SmallCaps ||
                                css.fontVariantCaps == CssFontVariantCaps::AllSmallCaps;
    entry.typography = (entry.hasTypography ? entry.typography : effectiveTypography) & ~fontTypography::AllSmallCaps;
    entry.hasTypography = true;
    if (css.fontVariantCaps == CssFontVariantCaps::AllSmallCaps ||
        (css.fontVariantCaps == CssFontVariantCaps::Inherit && (effectiveTypography & fontTypography::AllSmallCaps)))
      entry.typography |= fontTypography::AllSmallCaps;
  }
}

void ChapterHtmlSlimParser::startTextFloat(const CssStyle& css) {
  if (textFloatParent_ || !currentTextBlock || tableDepth || inRuby || activeFloatBottom_ || pendingFloatImage_ ||
      !pendingInlineImages.empty() || !MemoryBudget::canAllocateInternal(sizeof(ParsedText), 48 * 1024, 8192))
    return;
  if (partWordBufferIndex) flushPartWordBuffer();
  if (lowMemoryAbort || previewStopRequested) return;
  auto style = currentTextBlock->getBlockStyle();
  style.marginTop = style.marginBottom = style.marginLeft = style.marginRight = 0;
  style.paddingTop = style.paddingBottom = style.paddingLeft = style.paddingRight = 0;
  style.textIndent = 0;
  style.textIndentDefined = true;
  style.alignment = CssTextAlign::Right;
  style.textAlignDefined = true;
  style.lineHeightMultiplier = css.lineHeightMultiplier;
  auto floating = makeUniqueNoThrow<ParsedText>(extraParagraphSpacing, forceParagraphIndents, hyphenationEnabled,
                                                focusReadingEnabled, guideReadingEnabled, wordSpacing, style,
                                                trackReferenceCharacters, characterSpacing);
  if (!floating) {
    LOG_ERR("EHP", "Could not allocate text float; retaining normal flow");
    return;
  }
  textFloatParent_ = std::move(currentTextBlock);
  currentTextBlock = std::move(floating);
  textFloatDepth_ = depth;
  textFloatParagraph_ = currentTextBlockParagraphIndex;
  textFloatListItem_ = currentTextBlockListItemIndex;
  textFloatParentRunBytes_ = currentTextRunBytes;
  currentTextRunBytes = 0;
  textFloatParentWords_ = wordsExtractedInBlock;
  wordsExtractedInBlock = 0;
  textFloatParentMarkers_ = std::move(pendingPublisherPageMarkers);
  pendingPublisherPageMarkers.clear();
  textFloatParentFootnotes_ = std::move(pendingFootnotes);
  pendingFootnotes.clear();
}

void ChapterHtmlSlimParser::finishTextFloat() {
  if (previewStopRequested) return;
  if (!textFloatParent_ || !currentTextBlock) return;
  const auto parentStyle = textFloatParent_->getBlockStyle();
  const int available = std::max(1, int(viewportWidth) - parentStyle.totalHorizontalInset());
  const int column = std::max(1, available * 2 / 3);
  int totalHeight = 0, left = column;
  const auto lineHeight = [&](const TextBlock& line) {
    return epubLineHeightPixels(line.maxLineHeight(renderer, fontId), lineCompression,
                                ignorePublisherLineSpacing ? 0 : line.getBlockStyle().lineHeightMultiplier) +
           line.getRubyShift(line.maxAscender(renderer, fontId));
  };
  // The reference captures at most 64 words. A probe clones that bounded state;
  // reserve for strings, transformed-source maps and metadata before copying.
  const size_t cloneBudget =
      sizeof(ParsedText) + size_t(currentTextRunBytes) * 12 + currentTextBlock->size() * 160 + 8192;
  bool canFloat = MemoryBudget::canAllocateInternal(cloneBudget, 32 * 1024, 32768);
  if (canFloat)
    canFloat = currentTextBlock->layoutAndExtractLinesPreservingSource(
        renderer, fontId, column, [&](std::shared_ptr<TextBlock> line) {
          totalHeight += lineHeight(*line);
          for (uint16_t i = 0; i < line->wordCount(); ++i) left = std::min(left, int(line->wordXpos(i)));
        });
  auto floating = std::move(currentTextBlock);
  auto floatingMarkers = std::move(pendingPublisherPageMarkers);
  auto floatingFootnotes = std::move(pendingFootnotes);
  currentTextBlock = std::move(textFloatParent_);
  pendingFootnotes = std::move(textFloatParentFootnotes_);
  pendingPublisherPageMarkers = std::move(textFloatParentMarkers_);
  currentTextRunBytes = textFloatParentRunBytes_;
  wordsExtractedInBlock = textFloatParentWords_;
  textFloatDepth_ = -1;
  if (floating->isEmpty()) {
    if (floatingMarkers.empty()) return;
    if (!MemoryBudget::canAllocateInternal(
            (pendingPublisherPageMarkers.size() + floatingMarkers.size()) * sizeof(PendingPublisherPageMarker),
            32 * 1024)) {
      LOG_ERR("EHP", "Could not retain empty-float page markers");
      lowMemoryAbort = true;
      return;
    }
    for (auto marker : floatingMarkers) {
      marker.wordIndex = wordsExtractedInBlock + currentTextBlock->size();
      pendingPublisherPageMarkers.push_back(marker);
    }
    return;
  }
  if (!canFloat || totalHeight <= 0 || totalHeight > viewportHeight) {
    if (!currentTextBlock->isEmpty()) makePages();
    if (lowMemoryAbort || previewStopRequested) return;
    const int parentWordsAfterLayout = wordsExtractedInBlock;
    auto parent = std::move(currentTextBlock);
    auto parentMarkers = std::move(pendingPublisherPageMarkers);
    auto parentFootnotes = std::move(pendingFootnotes);
    currentTextBlock = std::move(floating);
    pendingFootnotes = std::move(floatingFootnotes);
    pendingPublisherPageMarkers = std::move(floatingMarkers);
    wordsExtractedInBlock = 0;
    makePages();
    currentTextBlock = std::move(parent);
    pendingFootnotes = std::move(parentFootnotes);
    pendingPublisherPageMarkers = std::move(parentMarkers);
    wordsExtractedInBlock = parentWordsAfterLayout;
    currentTextRunBytes = 0;
    auto& style = currentTextBlock->getBlockStyle();
    style.marginTop = style.paddingTop = style.textIndent = 0;
    return;
  }
  if (!currentPage && !startNewPage("text float")) return;
  int y = currentPageNextY + (!wordsExtractedInBlock ? std::max(0, int(parentStyle.topInset())) : 0);
  if (y + totalHeight > viewportHeight && !currentPage->elements.empty()) {
    completeCurrentPage();
    ++completedPageCount;
    stopPreviewIfPageLimitReached();
    if (previewStopRequested) return;
    if (!startNewPage("text float break")) return;
    y = 0;
    currentTextBlock->getBlockStyle().marginTop = currentTextBlock->getBlockStyle().paddingTop = 0;
  }
  y = std::min(y, std::max(0, int(viewportHeight) - totalHeight));
  const int top = y;
  int floatedWords = 0;
  size_t nextMarker = 0;
  const bool ok = floating->layoutAndExtractLines(
      renderer, fontId, column, [&](std::shared_ptr<TextBlock> line, uint32_t visible, uint32_t reference) {
        if (lowMemoryAbort) return;
        const int height = lineHeight(*line);
        auto style = line->getBlockStyle();
        style.lineHeight = height;
        line->setBlockStyle(style);
        auto element = makeUniqueNoThrow<PageLine>(line, parentStyle.leftInset() + available - column, y);
        if (!element) {
          LOG_ERR("EHP", "Could not allocate text-float line");
          lowMemoryAbort = true;
          return;
        }
        currentPageParagraphIndex = textFloatParagraph_;
        currentPageListItemIndex = textFloatListItem_;
        setCurrentPageVisibleOffset(std::min(visible, currentTextBlock->firstVisibleOffset()),
                                    std::min(reference, currentTextBlock->firstReferenceOffset()));
        currentPage->noteSourceLine(*line);
        resolveSourceAnchors(line->sourceMin(), line->sourceEnd());
        for (uint16_t i = 0; i < line->wordCount(); ++i) {
          const auto id = line->wordLinkId(i);
          if (!id) continue;
          if (insideFootnoteLink && currentFootnote.linkId == id)
            currentPage->addFootnote(currentFootnote.number, currentFootnote.href, id);
          else
            for (const auto& entry : floatingFootnotes)
              if (entry.second.linkId == id) currentPage->addFootnote(entry.second.number, entry.second.href, id);
        }
        floatedWords += line->wordCount();
        while (nextMarker < floatingMarkers.size() && floatingMarkers[nextMarker].wordIndex <= floatedWords) {
          if (currentPage->publisherPageMarkers.size() >= Page::MAX_PUBLISHER_PAGE_MARKERS_PER_PAGE)
            LOG_DBG("EHP", "Text-float page marker limit reached");
          else
            currentPage->addPublisherPageMarker(floatingMarkers[nextMarker].label, y);
          ++nextMarker;
        }
        currentPage->elements.push_back(std::move(element));
        y += height;
      });
  if (!ok) {
    LOG_ERR("EHP", "Failed to lay out text float");
    lowMemoryAbort = true;
    return;
  }
  noteContent(top, y);
  activeFloatTop_ = top;
  activeFloatBottom_ = y;
  activeFloatWidth_ = std::min(available - 1, column - left + 4);
  activeFloatIsRight_ = true;
}

void ChapterHtmlSlimParser::inheritLayoutProperties(CssStyle& style, const bool block) {
  if (style.hasLineHeight() || style.hasWhiteSpace() || style.hasHyphens() || style.hasHyphenateLimitBefore() ||
      style.hasHyphenateLimitAfter() || style.hasHyphenateLimitLines()) {
    if (layoutScopeCount_ == layoutScopes_.size()) {
      lowMemoryAbort = true;
      return;
    }
    layoutScopes_[layoutScopeCount_++] = {depth, inheritedLayout_};
    if (style.hasLineHeight() && style.lineHeightMultiplier > 0)
      inheritedLayout_.lineHeight = style.lineHeightMultiplier;
    if (style.hasWhiteSpace() && style.whiteSpace != CssWhiteSpace::Inherit)
      inheritedLayout_.whiteSpace = style.whiteSpace;
    if (style.hasHyphens() && style.hyphens != CssHyphens::Inherit) inheritedLayout_.hyphens = style.hyphens;
    if (style.hasHyphenateLimitBefore() && style.hyphenateLimitBefore != 255)
      inheritedLayout_.before = style.hyphenateLimitBefore;
    if (style.hasHyphenateLimitAfter() && style.hyphenateLimitAfter != 255)
      inheritedLayout_.after = style.hyphenateLimitAfter;
    if (style.hasHyphenateLimitLines() && style.hyphenateLimitLines != 255)
      inheritedLayout_.lines = style.hyphenateLimitLines;
  }
  style.lineHeightMultiplier = inheritedLayout_.lineHeight;
  style.defined.lineHeight = 1;
  style.whiteSpace = inheritedLayout_.whiteSpace;
  style.defined.whiteSpace = block || style.defined.whiteSpace;
  style.hyphens = inheritedLayout_.hyphens;
  style.defined.hyphens = 1;
  style.hyphenateLimitBefore = inheritedLayout_.before;
  style.defined.hyphenateLimitBefore = 1;
  style.hyphenateLimitAfter = inheritedLayout_.after;
  style.defined.hyphenateLimitAfter = 1;
  style.hyphenateLimitLines = inheritedLayout_.lines;
  style.defined.hyphenateLimitLines = 1;
}

void ChapterHtmlSlimParser::noteFloatStyle(const CssStyle& style) {
  if (!embeddedStyle || isLightMode() || tableDepth) return;
  if (style.hasCssClear() && style.cssClear != CssClear::None && style.cssClear != CssClear::Inherit &&
      (activeFloatBottom_ || pendingFloatImage_) &&
      (style.cssClear == CssClear::Both ||
       (style.cssClear == CssClear::Right) == (pendingFloatImage_ ? pendingFloatIsRight_ : activeFloatIsRight_))) {
    if (partWordBufferIndex) flushPartWordBuffer();
    if (currentTextBlock && !currentTextBlock->isEmpty()) makePages();
    if (pendingFloatImage_) attachPendingFloatImage();
    currentPageNextY = std::max(currentPageNextY, activeFloatBottom_);
    activeFloatBottom_ = 0;
  }
  if (style.hasCssFloat() && (style.cssFloat == CssFloat::Left || style.cssFloat == CssFloat::Right) &&
      floatScopeCount_ < floatScopes_.size())
    floatScopes_[floatScopeCount_++] = {depth, style.cssFloat == CssFloat::Right};
}

bool ChapterHtmlSlimParser::attachPendingFloatImage() {
  if (previewStopRequested) return true;
  if (!pendingFloatImage_) return true;
  if (!currentPage && !startNewPage("float image")) return false;
  const int height = pendingFloatImage_->getHeight(), width = pendingFloatImage_->getWidth();
  const int inset =
      currentTextBlock && !wordsExtractedInBlock ? std::max(0, int(currentTextBlock->getBlockStyle().topInset())) : 0;
  int top = currentPageNextY + inset;
  if (top + height > viewportHeight && !currentPage->elements.empty()) {
    completeCurrentPage();
    ++completedPageCount;
    stopPreviewIfPageLimitReached();
    if (previewStopRequested) return true;
    if (!startNewPage("float image break")) return false;
    top = 0;
    if (currentTextBlock)
      currentTextBlock->getBlockStyle().marginTop = currentTextBlock->getBlockStyle().paddingTop = 0;
  }
  top = std::min(top, std::max(0, int(viewportHeight) - height));
  const int left = currentTextBlock ? currentTextBlock->getBlockStyle().leftInset() : 0;
  const int right = currentTextBlock ? currentTextBlock->getBlockStyle().rightInset() : 0;
  auto image = makeUniqueNoThrow<PageImage>(std::move(pendingFloatImage_),
                                            pendingFloatIsRight_ ? viewportWidth - right - width : left, top);
  if (!image) {
    LOG_ERR("EHP", "Could not allocate float image");
    lowMemoryAbort = true;
    return false;
  }
  setCurrentPageVisibleOffset(pendingFloatVisible_, pendingFloatReference_);
  currentPageParagraphIndex = pendingFloatParagraph_;
  currentPageListItemIndex = pendingFloatListItem_;
  currentPage->noteSourcePoint(pendingFloatSource_);
  bindSourceAnchors(pendingFloatSource_);
  resolveSourceAnchors(pendingFloatSource_, pendingFloatSource_ == UINT32_MAX ? 0 : pendingFloatSource_ + 1);
  currentPage->elements.push_back(std::move(image));
  noteContent(top, top + height);
  activeFloatTop_ = top;
  activeFloatBottom_ = top + height;
  activeFloatWidth_ = width + 4;
  activeFloatIsRight_ = pendingFloatIsRight_;
  return true;
}

void ChapterHtmlSlimParser::noteContainerWidth(const char* tag, const CssStyle& style) {
  if (!embeddedStyle || isLightMode() || !style.hasImageWidth() || matches(tag, IMAGE_TAGS, std::size(IMAGE_TAGS)) ||
      containerWidthCount_ == containerWidths_.size())
    return;
  const auto* parent = containerWidthCount_ ? &containerWidths_[containerWidthCount_ - 1] : nullptr;
  const int parentWidth = parent ? parent->width : viewportWidth;
  const float em = renderer.getFontAscenderSize(currentTextFontId());
  const int width = style.imageWidth.toPixelsInt16(em, parentWidth);
  if (width < 1 || width >= parentWidth) return;
  const bool relative = style.imageWidth.unit == CssUnit::Percent;
  containerWidths_[containerWidthCount_++] = {
      depth, static_cast<int16_t>(width),
      relative ? (parent ? parent->fixedWidth : int16_t(0)) : static_cast<int16_t>(width),
      style.display == CssDisplay::InlineBlock || (parent && parent->inlineBlock)};
}

void ChapterHtmlSlimParser::prepareBackgroundPicture(const char* tag, const CssStyle& style) {
  static constexpr const char* tags[] = {"div",   "p",          "table",   "section", "figure",
                                         "aside", "blockquote", "article", "center"};
  if (!embeddedStyle || isLightMode() || !cssParser || imageRendering != 0 || tableDepth ||
      backgroundPictures_.size() >= 4 || !matches(tag, tags, std::size(tags)) ||
      style.backgroundRepeat != CssBackgroundRepeat::NoRepeat)
    return;
  const auto path = (documentCss_ ? documentCss_.get() : cssParser)->backgroundImageForStyle(style);
  if (!path || path->empty()) return;
  if (!MemoryBudget::canAllocateInternal(path->size() + 512, 32 * 1024, 4096)) {
    lowMemoryAbort = true;
    return;
  }
  backgroundPictures_.push_back({depth, *path, activeParser ? activeParser->sourceByteOffset() : UINT32_MAX,
                                 visibleTextOffset, referenceTextOffset, xpathParagraphIndex, xpathListItemIndex});
}

bool ChapterHtmlSlimParser::probeImageDimensions(const std::string& member, const std::string& cachedPath,
                                                 ImageDimensions& dimensions, std::string& sourcePath) {
  ImageDimsProbe probe;
  bool available = epub->readItemContentsToStream(member, probe, 1024, true) && probe.getDimensions(dimensions);
  if (!available) {
    // A deflated header needs a 32 KiB window. Borrow the existing framebuffer
    // before falling back to extraction for SVG or an uncommon raster header.
    GfxRenderer::FrameBufferLoan loan(renderer);
    ImageDimsProbe retry;
    available = epub->readItemContentsToStream(member, retry, 1024, true) && retry.getDimensions(dimensions);
  }
  sourcePath.clear();
  if (available) {
    sourcePath = member;
    return true;
  }
  {
    GfxRenderer::FrameBufferLoan loan(renderer);
    if (!epub->extractItemToFile(member, cachedPath)) return false;
  }
  auto* decoder = ImageDecoderFactory::getDecoder(cachedPath);
  for (int attempt = 0; decoder && attempt < 3; ++attempt) {
    if (attempt) delay(50);
    if (decoder->getDimensions(cachedPath, dimensions)) return true;
  }
  return false;
}

std::shared_ptr<ImageBlock> ChapterHtmlSlimParser::admitTableImage(const char* reference) {
  if (!reference || !*reference || imageRendering != 0) return nullptr;
  std::string member = reference;
  if (const auto fragment = member.find('#'); fragment != std::string::npos) member.resize(fragment);
  if (member.empty()) return nullptr;
  member = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(contentBase + member));
  const std::string cached = imageBasePath + "table_" + std::to_string(imageCounter++) + ".img";
  ImageDimensions dimensions{};
  uint16_t width = 0, height = 0;
  const bool optimized = epub->getOptimizerImageDimensions(member, width, height) && width > 0 && height > 0 &&
                         width <= INT16_MAX && height <= INT16_MAX;
  if (!(optimized ? MemoryBudget::hasHeapForOptimizerPxcImage("EHP", member.c_str())
                  : MemoryBudget::hasHeapForEpubInlineImage("EHP", member.c_str()))) {
    lowMemoryImageFallback = true;
    return nullptr;
  }
  std::string source = member;
  if (optimized)
    dimensions = {static_cast<int16_t>(width), static_cast<int16_t>(height)};
  else if (!probeImageDimensions(member, cached, dimensions, source))
    return nullptr;
  if (dimensions.width <= 0 || dimensions.height <= 0) return nullptr;
  auto image = makeUniqueNoThrow<ImageBlock>(cached, source, dimensions.width, dimensions.height);
  if (!image) {
    lowMemoryAbort = true;
    return nullptr;
  }
  return image;
}

bool ChapterHtmlSlimParser::placeTableImage(const std::shared_ptr<ImageBlock>& image, const uint32_t source) {
  if (previewStopRequested) return true;
  if (lowMemoryAbort) return false;
  if (!image) return true;
  auto fitted = image->fittedTo(viewportWidth, viewportHeight);
  if (!fitted) {
    lowMemoryAbort = true;
    return false;
  }
  if (!currentPage && !startNewPage("table image fallback")) return false;
  if (!currentPage->elements.empty() && currentPageNextY + fitted->getHeight() > viewportHeight) {
    completeCurrentPage();
    ++completedPageCount;
    stopPreviewIfPageLimitReached();
    if (previewStopRequested) return true;
    if (!startNewPage("table image fallback break")) return false;
  }
  const int height = fitted->getHeight();
  const int x = (viewportWidth - fitted->getWidth()) / 2;
  auto placed = makeUniqueNoThrow<PageImage>(std::move(fitted), x, currentPageNextY);
  if (!placed) {
    lowMemoryAbort = true;
    return false;
  }
  currentPage->noteSourcePoint(source);
  resolveSourceAnchors(source, source == UINT32_MAX ? 0 : source + 1);
  currentPage->elements.push_back(std::move(placed));
  setCurrentPageVisibleOffset(visibleTextOffset);
  markCurrentPageFromCurrentElement();
  noteContent(currentPageNextY, currentPageNextY + height);
  currentPageNextY += height;
  return true;
}

void ChapterHtmlSlimParser::finishBackgroundPicture(const int closingDepth) {
  if (previewStopRequested) return;
  if (backgroundPictures_.empty() || backgroundPictures_.back().depth != closingDepth || lowMemoryAbort) return;
  auto picture = std::move(backgroundPictures_.back());
  backgroundPictures_.pop_back();
  if (partWordBufferIndex) flushPartWordBuffer();
  if (currentTextBlock && !currentTextBlock->isEmpty()) makePages();
  if (lowMemoryAbort || previewStopRequested) return;
  if (!attachPendingFloatImage() || previewStopRequested) return;
  currentPageNextY = std::max(currentPageNextY, activeFloatBottom_);
  activeFloatBottom_ = 0;
  const std::string cached = imageBasePath + "background_" + std::to_string(imageCounter++) + ".img";
  if (!MemoryBudget::hasHeapForEpubInlineImage("EHP", cached.c_str())) {
    lowMemoryImageFallback = true;
    return;
  }
  ImageDimensions dims{};
  uint16_t width = 0, height = 0;
  bool available = epub->getOptimizerImageDimensions(picture.path, width, height);
  available = available && width <= INT16_MAX && height <= INT16_MAX;
  std::string sourcePath = picture.path;
  if (available) dims = {static_cast<int16_t>(width), static_cast<int16_t>(height)};
  if (!available) {
    available = probeImageDimensions(picture.path, cached, dims, sourcePath);
  }
  // The reference treats a no-repeat illustration as a block after its text;
  // smaller ornaments and repeating page textures stay unplaced.
  if (!available || dims.width < 96 || dims.height < 96) return;
  const float scale = std::min({1.0f, float(viewportWidth) / dims.width, float(viewportHeight) / dims.height});
  const int16_t w = std::max(1, int(dims.width * scale));
  const int16_t h = std::max(1, int(dims.height * scale));
  auto image = makeUniqueNoThrow<ImageBlock>(cached, sourcePath, w, h);
  if (!image) {
    lowMemoryAbort = true;
    return;
  }
  if (!currentPage && !startNewPage("background picture")) return;
  if (!currentPage->elements.empty() && currentPageNextY + h > viewportHeight) {
    completeCurrentPage();
    ++completedPageCount;
    stopPreviewIfPageLimitReached();
    if (previewStopRequested) return;
    if (!startNewPage("background picture break")) return;
  }
  auto placed = makeUniqueNoThrow<PageImage>(std::move(image), (viewportWidth - w) / 2, currentPageNextY);
  if (!placed) {
    lowMemoryAbort = true;
    return;
  }
  setCurrentPageVisibleOffset(picture.visible, picture.reference);
  currentPageParagraphIndex = picture.paragraph;
  currentPageListItemIndex = picture.listItem;
  currentPage->noteSourcePoint(picture.source);
  bindSourceAnchors(picture.source);
  resolveSourceAnchors(picture.source, picture.source == UINT32_MAX ? 0 : picture.source + 1);
  currentPage->elements.push_back(std::move(placed));
  noteContent(currentPageNextY, currentPageNextY + h);
  currentPageNextY += h;
}

void ChapterHtmlSlimParser::prepareGeneratedPseudo(const char* tag, std::string_view classes, std::string_view id) {
  if (!embeddedStyle || isLightMode() || !cssParser || tableDepth || generatedPseudos_.size() >= 8) return;
  const auto before = cssParser->pseudoContentFor(tag, classes, "before", ancestorStack_, id, documentCss_.get());
  const auto after = cssParser->pseudoContentFor(tag, classes, "after", ancestorStack_, id, documentCss_.get());
  if ((!before || before->empty()) && (!after || after->empty())) return;
  if (!MemoryBudget::canAllocateInternal(sizeof(GeneratedPseudo) + 1024, 32 * 1024, 4096)) {
    lowMemoryAbort = true;
    return;
  }
  auto entry = makeUniqueNoThrow<GeneratedPseudo>();
  if (!entry) {
    lowMemoryAbort = true;
    return;
  }
  entry->depth = depth;
  if (before) {
    entry->before = *before;
    entry->beforeStyle = cssParser->resolvePseudoStyle(tag, classes, "before", ancestorStack_, id, documentCss_.get());
  }
  if (after) {
    entry->after = *after;
    entry->afterStyle = cssParser->resolvePseudoStyle(tag, classes, "after", ancestorStack_, id, documentCss_.get());
  }
  generatedPseudos_.push_back(std::move(entry));
}

void ChapterHtmlSlimParser::emitGeneratedPseudo(std::string_view text, const CssStyle& css) {
  if (text.empty() || !currentTextBlock || css.isElementHidden() || css.isTextTransparent() ||
      (css.hasDisplay() && css.display == CssDisplay::None) || css.isFontSizeInvisible() ||
      inlineStyleCount_ >= MAX_INLINE_STYLE_DEPTH)
    return;
  if (partWordBufferIndex) {
    flushPartWordBuffer();
    nextWordContinues = true;
  }
  StyleStackEntry entry;
  entry.depth = depth;
  entry.hasBold = css.hasFontWeight();
  entry.bold = css.fontWeight == CssFontWeight::Bold;
  entry.hasItalic = css.hasFontStyle();
  entry.italic = css.fontStyle == CssFontStyle::Italic;
  applyInlineFontSize(entry, css, "span");
  applySmallCapsToEntry(entry, css);
  applyVerticalAlignToEntry(entry, css);
  applyScriptSizing(entry, css);
  inlineStyleBuf_[inlineStyleCount_++] = entry;
  updateEffectiveInlineStyle();
  const bool synthetic = syntheticCharacterData;
  emittingPseudo_ = syntheticCharacterData = true;
  characterData(this, text.data(), text.size());
  if (partWordBufferIndex) {
    flushPartWordBuffer();
    nextWordContinues = true;
  }
  emittingPseudo_ = false;
  syntheticCharacterData = synthetic;
  --inlineStyleCount_;
  updateEffectiveInlineStyle();
}

void ChapterHtmlSlimParser::emitPendingGeneratedBefore() {
  if (emittingPseudo_) return;
  for (auto& entry : generatedPseudos_) {
    if (entry->beforeEmitted) continue;
    entry->beforeEmitted = true;
    emitGeneratedPseudo(entry->before, entry->beforeStyle);
  }
}

void ChapterHtmlSlimParser::applyFirstLineStyle(BlockStyle& block, const char* tag, std::string_view classes,
                                                std::string_view id) {
  if (!embeddedStyle || isLightMode() || !cssParser) return;
  const auto css = cssParser->resolvePseudoStyle(tag, classes, "first-line", ancestorStack_, id, documentCss_.get());
  if (css.hasFontWeight()) {
    block.firstLineStyleMask |= EpdFontFamily::BOLD;
    if (css.fontWeight == CssFontWeight::Bold) block.firstLineStyle |= EpdFontFamily::BOLD;
  }
  if (css.hasFontStyle()) {
    block.firstLineStyleMask |= EpdFontFamily::ITALIC;
    if (css.fontStyle == CssFontStyle::Italic) block.firstLineStyle |= EpdFontFamily::ITALIC;
  }
  if (css.hasFontVariantCaps() && css.fontVariantCaps != CssFontVariantCaps::Inherit) {
    block.firstLineStyleMask |= EpdFontFamily::SMALL_CAPS;
    block.firstLineTypographyMask |= fontTypography::AllSmallCaps;
    if (css.fontVariantCaps != CssFontVariantCaps::Normal) block.firstLineStyle |= EpdFontFamily::SMALL_CAPS;
    if (css.fontVariantCaps == CssFontVariantCaps::AllSmallCaps)
      block.firstLineTypography |= fontTypography::AllSmallCaps;
  }
  if (css.hasLigatures() && css.ligatures != CssLigatures::Inherit) {
    block.firstLineTypographyMask |= fontTypography::NoLigatures;
    if (css.ligatures == CssLigatures::None) block.firstLineTypography |= fontTypography::NoLigatures;
  }
  if (css.defined.fontFamily && css.fontFamily != CssFontCatalog::Inherit) {
    block.firstLineFontFamilyDefined = true;
    block.firstLineFontFamily = publisherFamily(css.fontFamily);
  }
  if (css.hasFontSize() && css.fontSize.unit != CssUnit::Inherit) {
    const float base = renderer.getFontPointSize(fontId);
    const float parent = block.fontSize64 ? block.fontSize64 / 64.0f : base;
    float points = css.fontSize.value;
    switch (css.fontSize.unit) {
      case CssUnit::Em:
        points *= parent;
        break;
      case CssUnit::Rem:
        points *= base * rootFontScaleExact_;
        break;
      case CssUnit::Percent:
        points *= parent / 100;
        break;
      case CssUnit::Points:
        points *= base / 12;
        break;
      case CssUnit::Pixels:
        points *= base / 16;
        break;
      default:
        points = parent;
        break;
    }
    if (std::isfinite(points))
      block.firstLinePoints64 = static_cast<uint16_t>(std::round(std::clamp(points, 1.0f / 64, 128.0f) * 64));
  }
  if (css.hasLetterSpacing()) {
    block.firstLineLetterSpacingDefined = true;
    block.firstLineLetterSpacing = resolveLetterSpacing(
        css.letterSpacing,
        renderer.getFontIdForSize64(fontId, block.firstLinePoints64 ? block.firstLinePoints64 : block.fontSize64));
  }
}

int16_t ChapterHtmlSlimParser::resolveLetterSpacing(const CssLength& length, const int textFontId) const {
  if (length.unit == CssUnit::Inherit) return effectiveLetterSpacing;
  const int selectedFont =
      length.unit == CssUnit::Rem
          ? renderer.getFontIdForSize64(
                fontId, static_cast<uint16_t>(std::clamp(
                            std::round(renderer.getFontPointSize64(fontId) * rootFontScaleExact_), 1.0f, 8192.0f)))
          : textFontId;
  const float em = renderer.getFontAscenderSize(selectedFont);
  const float pixels = length.toPixels(em);
  if (!std::isfinite(pixels)) return 0;
  return static_cast<int16_t>(std::round(std::clamp(pixels * 16.0f, -512.0f, 512.0f)));
}

void ChapterHtmlSlimParser::applyVerticalAlignToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (!css.hasVerticalAlign()) return;
  if (css.verticalAlign == CssVerticalAlign::Super) {
    entry.hasSup = true;
    entry.sup = true;
  } else if (css.verticalAlign == CssVerticalAlign::Sub) {
    entry.hasSub = true;
    entry.sub = true;
  }
}

// Reuse the depth-scoped inline stack for inherited block font styles. Only
// explicit properties need entries; unstyled children keep the parent values.
void ChapterHtmlSlimParser::pushBlockFontStyle(const CssStyle& cssStyle) {
  if (!cssStyle.hasFontWeight() && !cssStyle.hasFontStyle() && !cssStyle.hasWhiteSpace() &&
      !cssStyle.defined.fontFamily && !cssStyle.hasTextTransform() && !cssStyle.hasLetterSpacing() &&
      !cssStyle.hasFontVariantCaps() && !cssStyle.hasLigatures())
    return;
  if (partWordBufferIndex > 0) {
    flushPartWordBuffer();
  }
  StyleStackEntry entry;
  entry.depth = depth;
  applySmallCapsToEntry(entry, cssStyle);
  entry.hasBold = cssStyle.hasFontWeight();
  entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
  entry.hasItalic = cssStyle.hasFontStyle();
  entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
  applyScriptSizing(entry, cssStyle);
  if (inlineStyleCount_ < MAX_INLINE_STYLE_DEPTH) {
    inlineStyleBuf_[inlineStyleCount_++] = entry;
  } else {
    LOG_ERR("EHP", "inline style stack overflow (block font)");
  }
}

// Update effective bold/italic/underline based on block style and inline style stack
uint32_t ChapterHtmlSlimParser::publisherFamily(const uint32_t family) const {
  if (!embeddedStyle || isLightMode() || publisherFonts == PublisherFonts::Off ||
      (publisherFonts == PublisherFonts::Accents && family == bodyFontFamily))
    return 0;
  return family;
}

void ChapterHtmlSlimParser::updateEffectiveInlineStyle() {
  // Start with block-level styles
  effectiveBold = currentCssStyle.hasFontWeight() && currentCssStyle.fontWeight == CssFontWeight::Bold;
  effectiveItalic = currentCssStyle.hasFontStyle() && currentCssStyle.fontStyle == CssFontStyle::Italic;
  effectiveUnderline = currentCssStyle.hasTextDecoration() &&
                       (currentCssStyle.textDecoration & CssTextDecoration::Underline) != CssTextDecoration::None;
  effectiveStrikethrough = currentCssStyle.hasTextDecoration() &&
                           (currentCssStyle.textDecoration & CssTextDecoration::LineThrough) != CssTextDecoration::None;
  effectiveBackgroundBlack =
      honorsPublisherDecorations() && currentCssStyle.hasBackgroundBlack() && currentCssStyle.backgroundBlack;
  bool paragraphDirectionDefined = false;
  bool paragraphIsRtl = false;
  if (blockStyleCount_ > 0) {
    const auto& blockStyle = blockStyleBuf_[blockStyleCount_ - 1];
    paragraphDirectionDefined = blockStyle.directionDefined;
    paragraphIsRtl = blockStyle.isRtl;
  }
  effectiveDirectionDefined = paragraphDirectionDefined;
  effectiveDirection = paragraphIsRtl ? CssTextDirection::Rtl : CssTextDirection::Ltr;
  effectiveSup = currentCssStyle.hasVerticalAlign() && currentCssStyle.verticalAlign == CssVerticalAlign::Super;
  effectiveSub = currentCssStyle.hasVerticalAlign() && currentCssStyle.verticalAlign == CssVerticalAlign::Sub;
  effectiveSmallCaps =
      currentCssStyle.hasFontVariantCaps() && (currentCssStyle.fontVariantCaps == CssFontVariantCaps::SmallCaps ||
                                               currentCssStyle.fontVariantCaps == CssFontVariantCaps::AllSmallCaps);
  effectiveTypography =
      (currentCssStyle.fontVariantCaps == CssFontVariantCaps::AllSmallCaps ? fontTypography::AllSmallCaps : 0) |
      (currentCssStyle.ligatures == CssLigatures::None ? fontTypography::NoLigatures : 0);
  effectiveTextTransform = embeddedStyle && !isLightMode() && currentCssStyle.hasTextTransform()
                               ? currentCssStyle.textTransform
                               : CssTextTransform::None;
  effectiveLetterSpacing = 0;

  uint32_t family = currentCssStyle.defined.fontFamily ? currentCssStyle.fontFamily : 0;
  effectiveInlineFontSize = 0;
  effectiveInlinePoints64 = 0;
  effectiveWhiteSpace = currentTextBlock ? currentTextBlock->getBlockStyle().whiteSpace : CssWhiteSpace::Normal;
  if (currentCssStyle.hasWhiteSpace() && currentCssStyle.whiteSpace != CssWhiteSpace::Inherit)
    effectiveWhiteSpace = currentCssStyle.whiteSpace;
  effectivePreserveWhitespace =
      effectiveWhiteSpace == CssWhiteSpace::Pre || effectiveWhiteSpace == CssWhiteSpace::PreWrap;

  // Apply inline style stack in order
  for (size_t i = 0; i < inlineStyleCount_; ++i) {
    const auto& entry = inlineStyleBuf_[i];
    if (entry.hasFontFamily) family = entry.fontFamily;
    if (entry.hasBold) {
      effectiveBold = entry.bold;
    }
    if (entry.hasItalic) {
      effectiveItalic = entry.italic;
    }
    if (entry.hasUnderline) {
      effectiveUnderline = entry.underline;
    }
    if (entry.hasStrikethrough) {
      effectiveStrikethrough = entry.strikethrough;
    }
    if (honorsPublisherDecorations() && entry.hasBackgroundBlack) {
      effectiveBackgroundBlack = entry.backgroundBlack;
    }
    if (entry.hasDirection) {
      effectiveDirectionDefined = true;
      effectiveDirection = entry.direction;
      if (entry.setsParagraphDirection) {
        paragraphDirectionDefined = true;
        paragraphIsRtl = entry.direction == CssTextDirection::Rtl;
      }
    }
    if (entry.hasSup) {
      effectiveSup = entry.sup;
      if (entry.sup) effectiveSub = false;
    }
    if (entry.hasSub) {
      effectiveSub = entry.sub;
      if (entry.sub) effectiveSup = false;
    }
    if ((entry.points64 || entry.fontSize) &&
        (blockStyleCount_ == 0 || entry.depth > blockStyleDepths_[blockStyleCount_ - 1])) {
      effectiveInlineFontSize = entry.fontSize;
      effectiveInlinePoints64 = entry.points64;
    }
    if (entry.hasTypography) effectiveTypography = entry.typography;
    if (entry.hasWhiteSpace) {
      effectivePreserveWhitespace = entry.preserveWhitespace;
      effectiveWhiteSpace = entry.whiteSpace;
    }
    if (entry.hasTextTransform) effectiveTextTransform = entry.textTransform;
    if (entry.hasLetterSpacing) effectiveLetterSpacing = entry.letterSpacing;
    if (entry.hasSmallCaps) {
      effectiveSmallCaps = entry.smallCaps;
    }
  }

  effectiveFontFamily = publisherFamily(family);

  if (currentTextBlock && currentTextBlock->isEmpty()) {
    auto& style = currentTextBlock->getBlockStyle();
    style.directionDefined = paragraphDirectionDefined;
    style.isRtl = paragraphIsRtl;
  }
}

bool ChapterHtmlSlimParser::shouldAbortForLowMemory(const char* stage) {
  if (lowMemoryAbort) {
    return true;
  }

  auto heap = MemoryBudget::snapshot();
  if (MemoryBudget::hasHeapForEpubTextLayoutStart(heap)) {
    return false;
  }

  // Optional keep-together lookahead must not make otherwise readable content
  // fail under pressure. Publishing the retained prior page releases its text.
  if (avoidGroup_.depth >= 0) {
    finishAvoidGroup(true);
    heap = MemoryBudget::snapshot();
    if (MemoryBudget::hasHeapForEpubTextLayoutStart(heap)) return false;
  }

  if (!attemptedTextLayoutFontCacheRelease) {
    attemptedTextLayoutFontCacheRelease = true;
    if (renderer.releaseSdCardFontForLowMemory(fontId)) {
      const auto afterRelease = MemoryBudget::snapshot();
      LOG_DBG("EHP", "Released SD font caches before %s: free=%u->%u maxAlloc=%u->%u", stage, heap.freeHeap,
              afterRelease.freeHeap, heap.maxAllocHeap, afterRelease.maxAllocHeap);
      heap = afterRelease;
      if (MemoryBudget::hasHeapForEpubTextLayoutStart(heap)) {
        return false;
      }
    }
  }

  LOG_ERR("EHP", "Low heap during %s (%u free, %u max alloc); aborting section build", stage, heap.freeHeap,
          heap.maxAllocHeap);
  lowMemoryAbort = true;
  return true;
}

bool ChapterHtmlSlimParser::startNewPage(const char* reason) {
  activeFloatBottom_ = 0;
  currentPage.reset(new (std::nothrow) Page());
  if (!currentPage) {
    const auto heap = MemoryBudget::snapshot();
    LOG_ERR("EHP", "Failed to create page during %s (%u free, %u max alloc)", reason, heap.freeHeap, heap.maxAllocHeap);
    lowMemoryAbort = true;
    return false;
  }

  const auto heap = MemoryBudget::snapshot();
  if (heap.freeHeap >= MemoryBudget::EPUB_TEXT_LAYOUT_MIN_FREE &&
      heap.maxAllocHeap >= PAGE_ELEMENT_RESERVE_MIN_MAX_ALLOC) {
    currentPage->elements.reserve(INITIAL_PAGE_ELEMENT_RESERVE);
  }
  currentPageNextY = 0;
  currentPageParagraphIndex = 0;
  currentPageListItemIndex = 0;
  currentPageVisibleOffsetSet = false;
  return true;
}

void ChapterHtmlSlimParser::markCurrentPageFromCurrentTextBlock() {
  currentPageParagraphIndex = currentTextBlockParagraphIndex;
  currentPageListItemIndex = currentTextBlockListItemIndex;
}

void ChapterHtmlSlimParser::markCurrentPageFromCurrentElement() {
  currentPageParagraphIndex = xpathParagraphIndex;
  currentPageListItemIndex = xpathListItemIndex;
}

void ChapterHtmlSlimParser::noteContent(const int top, const int bottom) {
  for (size_t i = 0; i < boxScopeCount; ++i) {
    auto& box = boxScopes[i];
    if (box.top < 0) box.top = static_cast<int16_t>(top);
    box.bottom = static_cast<int16_t>(std::max<int>(box.bottom, bottom));
  }
}

void ChapterHtmlSlimParser::openBoxScope(BlockStyle& ownStyle, const CssStyle& cssStyle) {
  if (!embeddedStyle || !honorsPublisherDecorations()) return;
  const bool shaded = cssStyle.defined.shaded && cssStyle.shaded;
  uint16_t gray = cssStyle.hasBackgroundGray() ? cssStyle.backgroundGray : uint16_t(256);
  if (gray == 257)
    gray = boxScopeCount && boxScopes[boxScopeCount - 1].depth == depth - 1 ? boxScopes[boxScopeCount - 1].gray : 256;
  const bool sized = cssStyle.hasMinHeight() || cssStyle.hasImageHeight();
  if (tableDepth > 0 || boxScopeCount >= MAX_BOX_SCOPES ||
      (!cssStyle.hasVisibleBorder() && !shaded && gray == 256 && !sized))
    return;

  if (partWordBufferIndex) flushPartWordBuffer();
  if (currentTextBlock && !currentTextBlock->isEmpty()) {
    makePages();
    currentTextBlock.reset();
  }
  if (lowMemoryAbort) return;
  BoxScope box;
  box.depth = depth;
  box.shaded = shaded;
  box.gray = gray;
  box.layer = boxScopeCount;

  const float em = renderer.getFontAscenderSize(currentTextFontId());
  const auto pixels = [&](const CssLength& length) { return length.toPixelsInt16(em, viewportHeight); };
  int minimum = cssStyle.hasMinHeight() ? pixels(cssStyle.minHeight) : 0;
  if (cssStyle.hasMinHeight() && cssStyle.minHeight.unit == CssUnit::Inherit)
    minimum =
        boxScopeCount && boxScopes[boxScopeCount - 1].depth == depth - 1 ? boxScopes[boxScopeCount - 1].minimum : 0;
  if (cssStyle.hasImageHeight()) minimum = std::max(minimum, int(pixels(cssStyle.imageHeight)));
  if (cssStyle.hasMaxHeight() && cssStyle.maxHeight.unit != CssUnit::Auto &&
      cssStyle.maxHeight.unit != CssUnit::Inherit)
    minimum = std::min(minimum, std::max(0, int(pixels(cssStyle.maxHeight))));
  box.minimum = std::clamp(minimum, 0, int(viewportHeight));
  const CssBorderSide* sides[4] = {&cssStyle.borders[0], &cssStyle.borders[1], &cssStyle.borders[2],
                                   &cssStyle.borders[3]};
  for (size_t i = 0; i < 4; ++i) {
    if (sides[i]->visible()) box.sides[i] = *sides[i];
  }
  const BlockStyle& parent = blockStyleBuf_[blockStyleCount_ - 1];
  box.left = static_cast<int16_t>(parent.leftInset() + ownStyle.marginLeft);
  box.right = static_cast<int16_t>(viewportWidth - parent.rightInset() - ownStyle.marginRight);
  if (box.right - box.left < 8) return;

  // The border sits between margin and padding, so it joins the padding. Text also
  // keeps a small gap from the frame even when the book sets no padding.
  constexpr int16_t MIN_FRAME_GAP = 4;
  const auto pad = [&](int16_t& padding, const CssBorderSide& side) {
    if (!side.visible() && !box.shaded) return;
    padding = static_cast<int16_t>(std::max(padding, MIN_FRAME_GAP) + side.width);
  };
  pad(ownStyle.paddingTop, box.sides[0]);
  pad(ownStyle.paddingRight, box.sides[1]);
  pad(ownStyle.paddingBottom, box.sides[2]);
  pad(ownStyle.paddingLeft, box.sides[3]);
  box.start = completedPageCount * int(viewportHeight) + currentPageNextY + std::max(0, int(ownStyle.marginTop)) +
              std::max(0, int(ownStyle.paddingTop));
  box.padTop = ownStyle.paddingTop;
  box.padBottom = ownStyle.paddingBottom;
  boxScopes[boxScopeCount++] = box;
}

void ChapterHtmlSlimParser::closeBoxScope() {
  BoxScope box = boxScopes[--boxScopeCount];
  if (previewStopRequested || lowMemoryAbort) return;
  const int used = completedPageCount * int(viewportHeight) + currentPageNextY - box.start;
  bool painted = box.gray < 256 || box.shaded;
  for (const auto& side : box.sides) painted = painted || side.visible();
  if (!(box.hiddenContent && used <= 0 && !painted) && box.minimum > used && box.top >= 0) {
    currentPageNextY = std::min<int>(viewportHeight, currentPageNextY + box.minimum - used);
    box.bottom = std::max<int>(box.bottom, currentPageNextY - box.padBottom);
  }
  if (box.top < 0 && !box.continued) {
    // An element without content (a rule drawn with borders) gets its own height.
    // Its spacing still sits on the empty text block; consume it here instead.
    const int height =
        std::min<int>(viewportHeight, box.padTop + box.padBottom + ((box.hiddenContent && !painted) ? 0 : box.minimum));
    if (height <= 0) return;
    int above = 0;
    int below = 0;
    if (currentTextBlock && currentTextBlock->isEmpty()) {
      BlockStyle& pending = currentTextBlock->getBlockStyle();
      above = pending.marginTop;
      below = pending.marginBottom;
      pending.marginTop = pending.paddingTop = pending.marginBottom = pending.paddingBottom = 0;
    }
    if (!currentPage) {
      if (!startNewPage("empty border box")) return;
    }
    if (!currentPage->elements.empty() && currentPageNextY + above + height > viewportHeight) {
      completeCurrentPage();
      ++completedPageCount;
      stopPreviewIfPageLimitReached();
      if (previewStopRequested) return;
      if (!startNewPage("border box break")) return;
      above = 0;
    }
    const int top = currentPageNextY + above;
    box.top = static_cast<int16_t>(top + box.padTop);
    box.bottom = top + height - box.padBottom;
    currentPageNextY = static_cast<int16_t>(top + height + below);
    noteContent(top, top + height);
  }
  if (box.top >= 0) noteContent(box.top - box.padTop, box.bottom + box.padBottom);
  emitBoxSegment(box, true);
}

void ChapterHtmlSlimParser::emitBoxSegment(const BoxScope& box, const bool closing) {
  if (box.top < 0 || !currentPage) return;
  const int top = std::max(0, box.top - box.padTop);
  const int bottom = std::min<int>(viewportHeight, box.bottom + (closing ? box.padBottom : 0));
  if (bottom <= top) return;
  // A page break slices the box: the continuing edges are left open.
  const CssBorderSide sides[4] = {box.continued ? CssBorderSide{} : box.sides[0], box.sides[1],
                                  closing ? box.sides[2] : CssBorderSide{}, box.sides[3]};
  auto element =
      makeUniqueNoThrow<PageBorderBox>(static_cast<uint16_t>(box.right - box.left), static_cast<uint16_t>(bottom - top),
                                       sides, box.shaded, box.left, static_cast<int16_t>(top));
  if (!element) {
    LOG_ERR("EHP", "OOM: PageBorderBox");
    lowMemoryAbort = true;
    return;
  }
  element->configureBackground(box.gray, box.layer);
  currentPage->elements.push_back(std::move(element));
}

uint8_t ChapterHtmlSlimParser::dropCapLines(const CssStyle& style) {
  static_assert(MAX_DROP_CAP_BYTES == PageDropCap::MAX_TEXT_BYTES, "captured drop-cap text must fit a PageDropCap");
  if (style.defined.initialLetter && style.initialLetter > 0) {
    return style.initialLetter >= 2 ? style.initialLetter : 0;
  }
  float em = style.fontSize.value;
  if (style.fontSize.unit == CssUnit::Pixels)
    em /= 16;
  else if (style.fontSize.unit == CssUnit::Points)
    em /= 12;
  else if (style.fontSize.unit == CssUnit::Percent)
    em /= 100;
  const bool large = style.hasFontSize() && em >= 1.5f;
  if (!large && !(style.defined.floatLeft && style.floatLeft)) return 0;
  // A floated letter without a size spans three lines; a sized one about its size in ems.
  return large ? static_cast<uint8_t>(std::clamp(em + 0.5f, 2.0f, 4.0f)) : 3;
}

namespace {
// Opening quotes and other punctuation that lead into a drop cap letter.
bool isDropCapPunctuation(const uint32_t cp) {
  if (cp < 0x80) return std::ispunct(static_cast<int>(cp)) != 0;
  return cp == 0xA1 || cp == 0xAB || cp == 0xBB || cp == 0xBF || (cp >= 0x2010 && cp <= 0x205E) ||
         (cp >= 0x3008 && cp <= 0x3011);
}
}  // namespace

// Returns false (and restores the captured text to the word buffer) once the
// capture is too long to be an initial letter.
bool ChapterHtmlSlimParser::captureDropCapCodepoint(const char* bytes, const int length, const size_t callbackOffset) {
  constexpr uint8_t MAX_DROP_CAP_CODEPOINTS = 3;
  if (dropCap.length + length > static_cast<int>(MAX_DROP_CAP_BYTES) || dropCap.codepoints >= MAX_DROP_CAP_CODEPOINTS) {
    cancelDropCapToWord();
    return false;
  }
  if (dropCap.length == 0) bindSourceAnchors(characterOrigin(callbackOffset));
  for (int i = 0; i < length; ++i) dropCap.sources[dropCap.length + i] = characterOrigin(callbackOffset + i);
  memcpy(dropCap.text + dropCap.length, bytes, static_cast<size_t>(length));
  dropCap.length = static_cast<uint8_t>(dropCap.length + length);
  dropCap.text[dropCap.length] = '\0';
  dropCap.codepoints++;
  if (transformSourceText(std::string_view(dropCap.text, dropCap.length), dropCap.transform).text.size() >
      MAX_DROP_CAP_BYTES) {
    // Fall back while capture still precedes the remaining paragraph text.
    cancelDropCapToWord();
    return true;  // The current codepoint is already in the restored word buffer.
  }
  if (dropCap.spanDepth < 0) {
    // ::first-letter takes leading punctuation plus the first letter.
    const auto* cursor = reinterpret_cast<const unsigned char*>(bytes);
    if (!isDropCapPunctuation(utf8NextCodepoint(&cursor))) dropCap.firstLetterPending = false;
  }
  return true;
}

void ChapterHtmlSlimParser::cancelDropCapToWord() {
  if (partWordBufferIndex) flushPartWordBuffer();
  partWordVisibleOffset = dropCap.visibleOffset;
  partWordReferenceOffset = dropCap.referenceOffset;
  std::copy_n(dropCap.sources, dropCap.length, partWordSources_ + partWordBufferIndex);
  memcpy(partWordBuffer + partWordBufferIndex, dropCap.text, dropCap.length);
  partWordBufferIndex += dropCap.length;
  dropCap = DropCapState{};
}

bool ChapterHtmlSlimParser::layoutText(const bool includeLastLine) {
  auto& style = currentTextBlock->getBlockStyle();
  const uint16_t width = static_cast<uint16_t>(std::max(1, int(viewportWidth) - style.totalHorizontalInset()));
  const auto emitLine = [this](std::shared_ptr<TextBlock> line, uint32_t visible, uint32_t reference) {
    addLineToPage(std::move(line), visible, reference);
  };
  const int layoutFont = currentTextFontId();
  if (dropCap.length && (currentTextBlock->isEmpty() || style.isRtl)) {
    cancelDropCapToWord();
    flushPartWordBuffer();
  }
  if (dropCap.length && dropCap.spanDepth < 0) {
    auto cap = dropCap;
    const uint8_t sourceLength = cap.length;
    const auto transformedCap = transformSourceText(std::string_view(cap.text, cap.length), cap.transform);
    if (transformedCap.text.size() <= MAX_DROP_CAP_BYTES) {
      std::memcpy(cap.text, transformedCap.text.c_str(), transformedCap.text.size() + 1);
      cap.length = transformedCap.text.size();
    }
    dropCap = {};
    const int lineHeight = effectiveLineHeight();
    const int ascender = renderer.getFontAscenderSize(layoutFont);
    const int capFont = renderer.resolveBookFont(
        fontId, cap.fontFamily,
        std::min<int>(cap.fontFamily ? ScalableContentMaxPointSize : 72, renderer.getFontPointSize(layoutFont) * 3));
    int32_t advance = 0;
    int bodyTop = ascender * 7 / 10, capTop = bodyTop;
    renderer.getCodepointMetrics(layoutFont, 'H', EpdFontFamily::REGULAR, advance, bodyTop);
    const auto capStyle = cap.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    renderer.ensureSdCardFontReady(capFont, cap.text, cap.bold ? 2 : 1);
    renderer.getCodepointMetrics(capFont, 'H', capStyle, advance, capTop);
    const int scale =
        std::clamp(((cap.lines - 1) * lineHeight + std::max(1, bodyTop)) * 256 / std::max(1, capTop), 256, 4096);
    int capWidth = renderer.getSpaceWidth(layoutFont, EpdFontFamily::REGULAR);
    const auto* cursor = reinterpret_cast<const unsigned char*>(cap.text);
    while (const uint32_t cp = utf8NextCodepoint(&cursor)) {
      int top = 0;
      if (renderer.getCodepointMetrics(capFont, cp, capStyle, advance, top))
        capWidth += (advance * scale / 256 + 8) / 16;
    }
    capWidth = std::clamp(capWidth, 1, std::max(1, int(width) / 2));
    if (!currentPage && !startNewPage("drop cap")) return false;
    if (!currentPage->elements.empty() &&
        currentPageNextY + cap.lines * lineHeight + style.topInset() > viewportHeight) {
      completeCurrentPage();
      ++completedPageCount;
      if (!startNewPage("drop cap break")) return false;
    }
    const int top = currentPageNextY + std::max(0, int(style.topInset()));
    auto element = makeUniqueNoThrow<PageDropCap>(renderer.getFontPointSize(capFont), static_cast<uint16_t>(scale),
                                                  capStyle, cap.text, style.leftInset(),
                                                  top + (cap.lines - 1) * lineHeight + ascender, cap.fontFamily);
    if (!element) {
      LOG_ERR("EHP", "OOM: drop cap");
      lowMemoryAbort = true;
      return false;
    }
    SourceWordSpan capSource;
    for (uint8_t i = 0; i < sourceLength; ++i) {
      const uint32_t origin = cap.sources[i];
      if (origin == UINT32_MAX) continue;
      capSource.start = std::min(capSource.start, origin);
      capSource.end = capSource.end == UINT32_MAX ? origin + 1 : std::max(capSource.end, origin + 1);
      if (origin == sourceTarget) capSource.probeByte = 0;
    }
    currentPage->noteSourceSpan(capSource);
    resolveSourceAnchors(capSource.start, capSource.end);
    setCurrentPageVisibleOffset(cap.visibleOffset, cap.referenceOffset);
    currentPage->elements.push_back(std::move(element));
    noteContent(top, top + cap.lines * lineHeight);
    style.textIndent = 0;
    style.textIndentDefined = true;
    currentTextBlock->setContinuation(true);
    currentTextBlock->setInitialLetter(cap.text);
    style.marginLeft += capWidth;
    const bool ok = currentTextBlock->layoutAndExtractLines(renderer, fontId, width - capWidth, emitLine,
                                                            includeLastLine, cap.lines);
    style.marginLeft -= capWidth;
    if (!ok) return false;
    currentPageNextY = static_cast<int16_t>(std::max(int(currentPageNextY), top + cap.lines * lineHeight));
  }
  if (!attachPendingFloatImage()) return false;
  if (previewStopRequested) return true;
  while (activeFloatBottom_ > currentPageNextY && !currentTextBlock->isEmpty()) {
    const auto emitted = wordsExtractedInBlock;
    const int narrowed = std::max(1, int(width) - activeFloatWidth_);
    const int left = activeFloatIsRight_ ? 0 : activeFloatWidth_;
    style.marginLeft += left;
    const bool ok = currentTextBlock->layoutAndExtractLines(renderer, fontId, narrowed, emitLine, includeLastLine, 1);
    style.marginLeft -= left;
    if (!ok) return false;
    if (lowMemoryAbort || previewStopRequested || wordsExtractedInBlock == emitted) return !lowMemoryAbort;
  }
  if (activeFloatBottom_ && currentPageNextY >= activeFloatBottom_) activeFloatBottom_ = 0;
  return currentTextBlock->layoutAndExtractLines(renderer, fontId, width, emitLine, includeLastLine);
}

void ChapterHtmlSlimParser::beginAvoidGroup() {
  if (avoidGroup_.depth >= 0 || boxScopeCount || tableDepth || isPreviewBuild() || pendingFloatImage_ ||
      textFloatParent_ || activeFloatBottom_ > 0 ||
      !MemoryBudget::hasHeap(MemoryBudget::snapshot(), 64 * 1024, 16 * 1024))
    return;
  if (partWordBufferIndex) flushPartWordBuffer();
  if (currentTextBlock && !currentTextBlock->isEmpty()) makePages();
  if (lowMemoryAbort || !currentPage || currentPage->elements.empty()) return;
  // Allocate before moving the live page; resource pressure simply keeps normal pagination.
  auto grouped = makeUniqueNoThrow<Page>();
  if (!grouped) {
    LOG_ERR("EHP", "Could not allocate keep-together page");
    return;
  }
  avoidGroup_.depth = depth;
  avoidGroup_.previous = std::move(currentPage);
  avoidGroup_.y = currentPageNextY;
  avoidGroup_.paragraph = currentPageParagraphIndex;
  avoidGroup_.listItem = currentPageListItemIndex;
  avoidGroup_.visible = currentPageVisibleOffset;
  avoidGroup_.reference = currentPageReferenceOffset;
  avoidGroup_.visibleSet = currentPageVisibleOffsetSet;
  avoidGroup_.anchorStart = anchorData.size();
  currentPage = std::move(grouped);
  currentPageNextY = 0;
  currentPageVisibleOffsetSet = false;
  currentTextBlock.reset();
}

void ChapterHtmlSlimParser::finishAvoidGroup(const bool forceBreak) {
  if (avoidGroup_.depth < 0) return;
  auto grouped = std::move(currentPage);
  const int height = currentPageNextY;
  const int requiredHeight = std::max(height, int(activeFloatBottom_));
  const auto paragraph = currentPageParagraphIndex, listItem = currentPageListItemIndex;
  const auto visible = currentPageVisibleOffset, reference = currentPageReferenceOffset;
  const bool visibleSet = currentPageVisibleOffsetSet;
  const int previousPage = completedPageCount;
  currentPage = std::move(avoidGroup_.previous);
  currentPageNextY = avoidGroup_.y;
  currentPageParagraphIndex = avoidGroup_.paragraph;
  currentPageListItemIndex = avoidGroup_.listItem;
  currentPageVisibleOffset = avoidGroup_.visible;
  currentPageReferenceOffset = avoidGroup_.reference;
  currentPageVisibleOffsetSet = avoidGroup_.visibleSet;
  avoidGroup_.depth = -1;
  if (!grouped) return;
  // The group's open boxes belong to its page, never the preceding one.
  const size_t groupBoxCount = boxScopeCount;
  boxScopeCount = 0;
  const size_t elements = currentPage->elements.size() + grouped->elements.size();
  const size_t footnotes = currentPage->footnotes.size() + grouped->footnotes.size();
  const size_t markers = currentPage->publisherPageMarkers.size() + grouped->publisherPageMarkers.size();
  const size_t pointerBytes =
      elements > currentPage->elements.capacity() ? elements * sizeof(std::unique_ptr<PageElement>) : 0;
  // The two pages already coexist. Recheck before growing their merged vectors;
  // moving the group to a fresh page needs no additional allocation at all.
  const size_t metadataBytes = 2 * (footnotes * sizeof(FootnoteEntry) + markers * sizeof(Page::PublisherPageMarker));
  const size_t reserveBytes = pointerBytes + metadataBytes;
  const bool move = forceBreak || currentPageNextY + requiredHeight > viewportHeight || elements > Page::MAX_ELEMENTS ||
                    footnotes > Page::MAX_FOOTNOTES_PER_PAGE || markers > Page::MAX_PUBLISHER_PAGE_MARKERS_PER_PAGE ||
                    !MemoryBudget::hasHeap(MemoryBudget::snapshot(), 32 * 1024 + reserveBytes, 4096 + reserveBytes);
  if (move) {
    completeCurrentPage();
    ++completedPageCount;
    currentPage = std::move(grouped);
    currentPageNextY = height;
    currentPageParagraphIndex = paragraph;
    currentPageListItemIndex = listItem;
    currentPageVisibleOffset = visible;
    currentPageReferenceOffset = reference;
    currentPageVisibleOffsetSet = visibleSet;
  } else {
    const int offset = currentPageNextY;
    if (visibleSet) setCurrentPageVisibleOffset(visible, reference);
    currentPage->mergeSource(*grouped);
    currentPage->elements.reserve(currentPage->elements.size() + grouped->elements.size());
    for (auto& element : grouped->elements) {
      element->yPos += offset;
      currentPage->elements.push_back(std::move(element));
    }
    for (const auto& footnote : grouped->footnotes)
      currentPage->addFootnote(footnote.number, footnote.href, footnote.linkId);
    for (const auto& marker : grouped->publisherPageMarkers)
      currentPage->addPublisherPageMarker(marker.label, marker.yPos + offset);
    currentPageNextY += height;
    if (!grouped->elements.empty()) {
      currentPageParagraphIndex = paragraph;
      currentPageListItemIndex = listItem;
    }
  }
  const int dy = move ? 0 : avoidGroup_.y;
  if (activeFloatBottom_ > 0) {
    activeFloatTop_ += dy;
    activeFloatBottom_ += dy;
  }
  boxScopeCount = groupBoxCount;
  for (size_t i = 0; i < boxScopeCount; ++i) {
    auto& box = boxScopes[i];
    box.start += (completedPageCount - previousPage) * int(viewportHeight) + dy;
    if (box.top >= 0) box.top += dy;
    if (box.bottom >= 0) box.bottom += dy;
  }
  for (size_t i = avoidGroup_.anchorStart; i < anchorData.size(); ++i)
    anchorData[i].second += completedPageCount - previousPage;
}

void ChapterHtmlSlimParser::completeCurrentPage() {
  if (avoidGroup_.depth >= 0) finishAvoidGroup(true);
  for (size_t i = 0; i < boxScopeCount; ++i) {
    auto& box = boxScopes[i];
    emitBoxSegment(box, false);
    if (box.top >= 0) box.continued = true;
    box.top = box.bottom = -1;
  }
  completePageFn(std::move(currentPage), currentPageParagraphIndex, currentPageListItemIndex, currentPageVisibleOffset,
                 currentPageReferenceOffset);
}

void ChapterHtmlSlimParser::setCurrentPageVisibleOffset(const uint32_t offset, const uint32_t referenceOffset) {
  if (currentPageVisibleOffsetSet) return;
  currentPageVisibleOffset = completedPageCount == 0 && avoidGroup_.depth < 0 ? 0 : offset;
  currentPageReferenceOffset = completedPageCount == 0 && avoidGroup_.depth < 0
                                   ? 0
                                   : (referenceOffset == UINT32_MAX ? referenceTextOffset : referenceOffset);
  currentPageVisibleOffsetSet = true;
}

void ChapterHtmlSlimParser::bindSourceAnchors(const uint32_t sourceStart) {
  if (sourceStart == UINT32_MAX) return;
  for (auto& anchor : sourceAnchors_)
    if (anchor.target == UINT32_MAX && anchor.raw <= sourceStart) anchor.target = sourceStart;
}
void ChapterHtmlSlimParser::resolveSourceAnchors(const uint32_t sourceFirst, const uint32_t sourceEnd,
                                                 const bool final) {
  for (auto at = sourceAnchors_.begin(); at != sourceAnchors_.end();) {
    if (final || (at->target != UINT32_MAX && at->target >= sourceFirst && at->target < sourceEnd)) {
      anchorData.emplace_back(std::move(at->id), static_cast<uint16_t>(completedPageCount));
      at = sourceAnchors_.erase(at);
    } else
      ++at;
  }
}

void ChapterHtmlSlimParser::flushPendingAnchor() {
  if (pendingAnchorId.empty()) return;

  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  if (std::find(tocAnchors.begin(), tocAnchors.end(), pendingAnchorId) != tocAnchors.end()) {
    if (currentPage && !currentPage->elements.empty()) {
      completeCurrentPage();
      completedPageCount++;
      stopPreviewIfPageLimitReached();
      if (previewStopRequested) {
        return;
      }
      if (!startNewPage("TOC anchor page break")) {
        return;
      }
    }
  }

  // Record deferred anchor after previous block is flushed (and any TOC page break)
  anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
  pendingAnchorId.clear();
  pendingAnchorFromInlineA = false;
}

// Resolves previewAnchor to the element ordinal the preview should start rendering at. Runs before
// the render pass so the preview can begin at the block enclosing the anchor instead of
// mid-paragraph. Leaves previewStartOrdinal at 0 on any failure, which falls back to matching the
// anchor id, i.e. the behaviour before block resolution existed.
void ChapterHtmlSlimParser::locatePreviewBlockStart() {
  PreviewBlockLocator locator(previewAnchor.c_str(), isHeaderOrBlock);
  if (!locator.ok()) {
    LOG_ERR("EHP", "Couldn't create parser to locate preview block");
    return;
  }

  // Heap rather than stack: PARSE_BUFFER_SIZE is a quarter of the parsing task's stack. The buffer
  // is released before the render pass opens the file, so the two never overlap.
  auto buf = makeUniqueNoThrow<char[]>(PARSE_BUFFER_SIZE);
  if (!buf) {
    LOG_ERR("EHP", "Couldn't allocate buffer to locate preview block");
    return;
  }

  FsFile file;
  if (!Storage.openFileForRead("EHP", filepath, file)) {
    return;
  }

  while (!locator.done()) {
    const size_t len = file.read(buf.get(), PARSE_BUFFER_SIZE);
    const bool isFinal = file.available() == 0;
    if (len == 0 && !isFinal) {
      LOG_ERR("EHP", "File read error while locating preview block");
      break;
    }
    if (!locator.feed(buf.get(), static_cast<int>(len), isFinal) || isFinal) {
      break;
    }
  }
  file.close();

  previewStartOrdinal = locator.startOrdinal();
  if (previewStartOrdinal == 0) {
    LOG_DBG("EHP", "No enclosing block located for preview anchor '%s'; starting at the anchor", previewAnchor.c_str());
    return;
  }
  LOG_DBG("EHP", "Preview anchor '%s' resolves to block at element #%u", previewAnchor.c_str(), previewStartOrdinal);
}

bool ChapterHtmlSlimParser::handlePreviewScanStart(const XML_Char** atts) {
  previewElementOrdinal += 1;
  if (previewStartOrdinal != 0 && previewElementOrdinal == previewStartOrdinal) {
    startPreviewAtAnchor();
    return false;
  }

  // Fallback for when no block was located, and a safety net in case the two passes disagree on
  // element ordinals: starting at the anchor is the pre-existing behaviour.
  const char* idValue = getAttribute(atts, "id");
  if (!idValue || strcmp(idValue, previewAnchor.c_str()) != 0) {
    depth += 1;
    return true;
  }

  startPreviewAtAnchor();
  return false;
}

void ChapterHtmlSlimParser::startPreviewAtAnchor() {
  previewAnchorFound = true;
  completedPageCount = 0;
  currentPageNextY = 0;
  pendingAnchorId.clear();
  pendingAnchorFromInlineA = false;
  sourceAnchors_.clear();
  anchorData.clear();
  anchorData.push_back({previewAnchor, 0});

  if (!currentTextBlock) {
    auto paragraphAlignmentBlockStyle = BlockStyle();
    paragraphAlignmentBlockStyle.textAlignDefined = true;
    paragraphAlignmentBlockStyle.alignment = (paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                                 ? CssTextAlign::Justify
                                                 : static_cast<CssTextAlign>(paragraphAlignment);
    startNewTextBlock(paragraphAlignmentBlockStyle);
  }
}

void ChapterHtmlSlimParser::stopPreviewIfPageLimitReached() {
  if (!isPreviewBuild() || !previewAnchorFound || previewStopRequested || completedPageCount < previewMaxPages) {
    return;
  }

  previewStopRequested = true;
  if (activeParser) {
    activeParser->stop();
  }
}

void ChapterHtmlSlimParser::addPendingPublisherPageMarker(const char* label) {
  if (!label || label[0] == '\0' || tableDepth > 0) {
    return;
  }

  if (partWordBufferIndex > 0) {
    flushPartWordBuffer();
  }

  PendingPublisherPageMarker marker;
  marker.wordIndex = wordsExtractedInBlock + (currentTextBlock ? static_cast<int>(currentTextBlock->size()) : 0);
  strncpy(marker.label, label, sizeof(marker.label) - 1);
  marker.label[sizeof(marker.label) - 1] = '\0';
  pendingPublisherPageMarkers.push_back(marker);
}

void ChapterHtmlSlimParser::attachPendingPublisherPageMarkers(const int yPos) {
  if (!currentPage || pendingPublisherPageMarkers.empty()) {
    return;
  }

  auto markerIt = pendingPublisherPageMarkers.begin();
  while (markerIt != pendingPublisherPageMarkers.end() && markerIt->wordIndex <= wordsExtractedInBlock) {
    currentPage->addPublisherPageMarker(markerIt->label, yPos);
    ++markerIt;
  }
  pendingPublisherPageMarkers.erase(pendingPublisherPageMarkers.begin(), markerIt);
}

void ChapterHtmlSlimParser::queueInlinePadding(const CssStyle& cssStyle) {
  if (!embeddedStyle || !honorsPublisherDecorations() || !cssStyle.hasPaddingLeft()) {
    return;
  }

  const int paddingPixels = cssStyle.paddingLeft.toPixelsInt16(renderer.getFontAscenderSize(fontId), viewportWidth);
  if (paddingPixels <= 0) {
    return;
  }

  if (partWordBufferIndex > 0) {
    flushPartWordBuffer();
  }
  pendingInlinePadding =
      static_cast<int16_t>(std::min<int>(INT16_MAX, static_cast<int>(pendingInlinePadding) + paddingPixels));
  // CSS inline padding stays glued to the following text, just like the empty
  // <span class="spacey"/> elements used by the affected EPUB.
  nextWordContinues = true;
}

CssStyle ChapterHtmlSlimParser::resolveElementStyle(std::string_view tag, std::string_view classes,
                                                    std::string_view id) const {
  return cssParser ? cssParser->resolveStyle(tag, classes, ancestorStack_, id, false, documentCss_.get()) : CssStyle{};
}

CssStyle ChapterHtmlSlimParser::resolveInlineStyle(std::string_view text) const {
  const CssParser* context = documentCss_ ? documentCss_.get() : cssParser;
  return context ? context->parseInlineStyleWithFonts(text, contentBase) : CssParser::parseInlineStyle(text);
}

uint32_t ChapterHtmlSlimParser::characterOrigin(const size_t index) const {
  if (syntheticCharacterData || !activeParser) return UINT32_MAX;
  const uint32_t origin = activeParser->characterSourceOffset(index);
  return literalEntitySource_ && origin != UINT32_MAX ? origin + index : origin;
}

SourceWordSpan ChapterHtmlSlimParser::bufferedWordSource(const SourceMappedText& mapped) const {
  SourceWordSpan source;
  uint16_t rawTargetOrdinal = UINT16_MAX;
  uint16_t ordinal = 0;
  for (int i = 0; i < partWordBufferIndex; ++i) {
    const uint32_t origin = partWordSources_[i];
    if ((static_cast<uint8_t>(partWordBuffer[i]) & 0xc0) != 0x80) {
      if (origin == sourceTarget && rawTargetOrdinal == UINT16_MAX) rawTargetOrdinal = ordinal;
      ++ordinal;
    }
    if (origin == UINT32_MAX) continue;
    source.start = std::min(source.start, origin);
    source.end = source.end == UINT32_MAX ? origin + 1 : std::max(source.end, origin + 1);
  }
  if (!source.valid()) return source;
  if (source.start == sourceTarget) rawTargetOrdinal = sourceTargetCharacter;
  if (rawTargetOrdinal == UINT16_MAX) return source;
  uint16_t cookedOrdinal = 0;
  const auto* begin = reinterpret_cast<const unsigned char*>(mapped.text.data());
  const auto* at = begin;
  const auto* end = begin + mapped.text.size();
  while (at < end) {
    const uint16_t original = mapped.origins.empty() ? cookedOrdinal : mapped.origins[cookedOrdinal];
    if (original >= rawTargetOrdinal) {
      source.probeByte = static_cast<uint16_t>(at - begin);
      break;
    }
    utf8NextCodepoint(&at);
    ++cookedOrdinal;
  }
  return source;
}

// flush the contents of partWordBuffer to currentTextBlock
void ChapterHtmlSlimParser::flushPartWordBuffer() {
  if (lowMemoryAbort) {
    partWordBufferIndex = 0;
    partWordInlinePadding = 0;
    nextWordContinues = false;
    return;
  }

  // Determine font style from depth-based tracking and CSS effective style
  const bool isBold = boldUntilDepth < depth || effectiveBold;
  const bool isItalic = italicUntilDepth < depth || effectiveItalic;
  const bool isUnderline = underlineUntilDepth < depth || effectiveUnderline;
  const bool isStrikethrough = strikethroughUntilDepth < depth || effectiveStrikethrough;

  // Combine style flags using bitwise OR
  EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
  if (isBold) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::BOLD);
  }
  if (isItalic) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::ITALIC);
  }
  if (isUnderline) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::UNDERLINE);
  }
  if (isStrikethrough) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::STRIKETHROUGH);
  }
  if (effectiveSup) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUP);
  } else if (effectiveSub) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUB);
  }
  if (effectiveSmallCaps) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SMALL_CAPS);
  }

  auto mapped = transformSourceText(std::string_view(partWordBuffer, partWordBufferIndex), effectiveTextTransform);
  const SourceWordSpan wordSource = bufferedWordSource(mapped);
  bindSourceAnchors(wordSource.start);

  if (currentCompactTable && currentCompactTable->valid() && currentCompactTable->hasActiveCell()) {
    if (!currentCompactTable->appendWord(mapped.text, fontStyle, nextWordContinues,
                                         honorsPublisherDecorations() && effectiveBackgroundBlack,
                                         insideFootnoteLink ? currentFootnote.linkId : 0, wordSource,
                                         mapped.origins.data(), mapped.origins.size())) {
      // The row exceeded the compact model's fixed text/token budget. Degrade
      // this table to paragraphs the way the rich path does for its own limits,
      // rather than failing the whole section build.
      LOG_DBG("EHP", "Compact table row capacity exceeded; flattening table");
      compactTableUnsupported = true;
      currentCompactTable->markUnsupported();
    }
    currentTextRunBytes = static_cast<uint16_t>(
        std::min<size_t>(currentTextRunBytes + static_cast<size_t>(partWordBufferIndex), UINT16_MAX));
    partWordBufferIndex = 0;
    partWordInlinePadding = 0;
    nextWordContinues = false;
    return;
  }

  if (!currentTextBlock) {
    // Text outside a compact table cell has no cell buffer. Ignore malformed
    // table content rather than dereferencing a null paragraph block.
    LOG_ERR("EHP", "Discarding text without a paragraph or compact table cell");
    partWordBufferIndex = 0;
    partWordInlinePadding = 0;
    nextWordContinues = false;
    return;
  }

  // flush the buffer
  partWordBuffer[partWordBufferIndex] = '\0';
  currentTextBlock->setNextWordSource(wordSource);
  currentTextBlock->addWord(std::move(mapped.text), fontStyle, false, nextWordContinues,
                            honorsPublisherDecorations() && effectiveBackgroundBlack,
                            insideFootnoteLink ? currentFootnote.linkId : 0, partWordVisibleOffset,
                            partWordReferenceOffset, partWordInlinePadding, effectivePreserveWhitespace,
                            effectiveInlineFontSize, effectiveFontFamily, effectiveLetterSpacing, mapped.origins.data(),
                            mapped.origins.size(), effectiveInlinePoints64, effectiveTypography);
  currentTextRunBytes = static_cast<uint16_t>(
      std::min<size_t>(currentTextRunBytes + static_cast<size_t>(partWordBufferIndex), UINT16_MAX));
  partWordBufferIndex = 0;
  partWordInlinePadding = 0;
  nextWordContinues = false;

  if (textFloatParent_) {
    if (currentTextBlock->size() >= 64 || currentTextRunBytes >= 8192) finishTextFloat();
    return;
  }

  // YACP 4bfcfb4 (Totofaki): one XML callback can contain many fragments.
  // Check each append before CJK token storage grows past the layout budget.
  // Buffered table cells must stay intact until the table lays them out.
  if (!currentTableBuffer && !currentCompactTable) flushLongTextRunIfNeeded();
}

size_t ChapterHtmlSlimParser::bufferedWordsBeforeLayoutLimit() const {
  if (focusReadingEnabled && guideReadingEnabled) {
    return COMBINED_READING_AID_BUFFERED_WORDS_BEFORE_LAYOUT;
  }
  if (focusReadingEnabled || guideReadingEnabled) {
    return SINGLE_READING_AID_BUFFERED_WORDS_BEFORE_LAYOUT;
  }
  return embeddedStyle ? CSS_BUFFERED_WORDS_BEFORE_LAYOUT : DEFAULT_BUFFERED_WORDS_BEFORE_LAYOUT;
}

uint16_t ChapterHtmlSlimParser::textRunBytesBeforeLayoutLimit() const {
  if (focusReadingEnabled && guideReadingEnabled) {
    return COMBINED_READING_AID_TEXT_RUN_BYTES_BEFORE_LAYOUT;
  }
  if (focusReadingEnabled || guideReadingEnabled) {
    return SINGLE_READING_AID_TEXT_RUN_BYTES_BEFORE_LAYOUT;
  }
  return DEFAULT_TEXT_RUN_BYTES_BEFORE_LAYOUT;
}

void ChapterHtmlSlimParser::flushLongTextRunIfNeeded(const bool force, const bool flushLastLine) {
  if (!currentTextBlock) {
    currentTextRunBytes = 0;
    return;
  }
  // A ruby group needs all of its base words together to distribute the
  // annotation and calculate its line-break constraints correctly.
  if (inRuby) return;

  const size_t wordLimit = bufferedWordsBeforeLayoutLimit();
  const uint16_t byteLimit = textRunBytesBeforeLayoutLimit();
  const size_t wordCount = currentTextBlock->size();
  if (!force && wordCount <= wordLimit && currentTextRunBytes <= byteLimit) {
    return;
  }

  if (!layoutText(flushLastLine)) {
    LOG_ERR("EHP", "Failed to lay out long text run");
    lowMemoryAbort = true;
    return;
  }
  if (flushLastLine) currentTextBlock->setContinuation(true);
  currentTextRunBytes = 0;
}

int ChapterHtmlSlimParser::currentTextFontId() const {
  if (!currentTextBlock || tableDepth > 0) return fontId;
  return renderer.getFontIdForSize64(fontId, currentTextBlock->getBlockStyle().fontSize64
                                                 ? currentTextBlock->getBlockStyle().fontSize64
                                                 : currentTextBlock->getBlockStyle().fontSize * 64);
}

void ChapterHtmlSlimParser::applyBlockFontSize(const CssStyle& cssStyle, const char* tag, BlockStyle& style) {
  const uint8_t base = renderer.getFontPointSize(fontId);
  if (!base || isLightMode() || tableDepth > 0) return;
  const float parent = blockStyleBuf_[blockStyleCount_ - 1].cssFontScale;
  float scale = parent;
  if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && tag[2] == '\0') {
    static constexpr float headingScale[] = {2.0f, 1.5f, 1.17f, 1.0f, 0.83f, 0.67f};
    scale *= headingScale[tag[1] - '1'];
  }
  if (embeddedStyle && cssStyle.hasFontSize()) {
    const auto& size = cssStyle.fontSize;
    switch (size.unit) {
      case CssUnit::Em:
        scale = parent * size.value;
        break;
      case CssUnit::Rem:
        scale = rootFontScaleExact_ * size.value;
        break;
      case CssUnit::Percent:
        scale = parent * size.value / 100.0f;
        break;
      // Reader zoom: CSS medium (16px/12pt) maps to the user's body size.
      case CssUnit::Points:
        scale = size.value / 12.0f;
        break;
      case CssUnit::Pixels:
        scale = size.value / 16.0f;
        break;
      case CssUnit::Inherit:
      case CssUnit::Auto:
        scale = parent;
        break;
    }
  }
  if (!std::isfinite(scale)) scale = parent;
  if (strcmp(tag, "html") == 0) {
    rootFontScaleExact_ = std::clamp(scale, 1.0f / 4096.0f, 256.0f);
    rootFontScale = static_cast<uint16_t>(std::clamp(scale * 256.0f + 0.5f, 1.0f, 65535.0f));
  }
  style.cssFontScale = std::clamp(scale, 1.0f / 4096.0f, 256.0f);
  scale = std::clamp(scale, (1.0f / 64) / base, 128.0f / base);
  style.fontScale = static_cast<uint16_t>(scale * 256.0f + 0.5f);
  style.fontSize = static_cast<uint8_t>(base * scale + 0.5f);
  style.fontSize64 = static_cast<uint16_t>(std::round(base * scale * 64.0f));
  // If a recovery font lacks the requested size, layout and drawing must agree.
  style.fontSize = renderer.getFontPointSize(renderer.getFontIdForSize(fontId, style.fontSize));
}

// start a new text block if needed
void ChapterHtmlSlimParser::startNewTextBlock(const BlockStyle& blockStyle) {
  if (shouldAbortForLowMemory("text block start")) {
    return;
  }

  emptyTextBlockAfterImage = false;
  nextWordContinues = false;  // New block = new paragraph, no continuation
  pendingInlinePadding = 0;
  partWordInlinePadding = 0;
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty() && wordsExtractedInBlock == 0) {
      currentTextBlock->setContinuation(false);
      BlockStyle incoming = blockStyle;
      const bool currentIsEmptyBr = currentTextBlock->getBlockStyle().fromBrElement;
      if (currentIsEmptyBr) {
        // The empty block was created by a <br> section separator. Inject a full line of
        // blank space before the following paragraph so the scene/section break is visible.
        // This only fires when the <br> block stayed empty (i.e. no inline text was added).
        const int16_t lineHeight = static_cast<int16_t>(effectiveLineHeight());
        incoming.marginTop = static_cast<int16_t>(incoming.marginTop + lineHeight);
      }

      // The stack accumulates horizontal properties from ancestors. Vertical margins are
      // per-element; merge them only while the placeholder block is still empty.
      const auto style = currentTextBlock->getBlockStyle();
      auto combinedStyle = style.getCombinedBlockStyle(incoming, BlockStyle::CombineAxis::Vertical);
      combinedStyle.fromBrElement = incoming.fromBrElement;
      currentTextBlock->setBlockStyle(combinedStyle);
      currentTextBlockParagraphIndex = xpathParagraphIndex;
      currentTextBlockListItemIndex = xpathListItemIndex;

      flushPendingAnchor();
      return;
    }

    // A <br> flush is only a fragment: text may follow an image in the same paragraph.
    makePages(!blockStyle.fromBrElement);
  }
  currentTextRunBytes = 0;
  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  flushPendingAnchor();
  currentTextBlock.reset(new (std::nothrow) ParsedText(extraParagraphSpacing, forceParagraphIndents, hyphenationEnabled,
                                                       focusReadingEnabled, guideReadingEnabled, wordSpacing,
                                                       blockStyle, trackReferenceCharacters, characterSpacing));
  if (!currentTextBlock) {
    const auto heap = MemoryBudget::snapshot();
    LOG_ERR("EHP", "Failed to create text block (%u free, %u max alloc)", heap.freeHeap, heap.maxAllocHeap);
    lowMemoryAbort = true;
    return;
  }
  currentTextBlockParagraphIndex = xpathParagraphIndex;
  currentTextBlockListItemIndex = xpathListItemIndex;
  wordsExtractedInBlock = 0;
}

void ChapterHtmlSlimParser::pushCssAncestor(const int depth, const char* tag, const std::string_view classAttr,
                                            const std::string_view idAttr) {
  if (usesSimpleCssLookup() || ancestorStack_.size() >= MAX_BLOCK_STYLE_DEPTH) {
    return;
  }
  ancestorStack_.push_back({depth, std::string(tag), std::string(classAttr), std::string(idAttr)});
}

void ChapterHtmlSlimParser::finalizeCurrentTableCell() {
  if (lowMemoryAbort) {
    return;
  }

  if (tableDepth != 1 || !currentTextBlock) {
    if (tableDepth == 1 && currentCompactTable && currentCompactTable->hasActiveCell()) {
      if (!currentCompactTable->endCell(pendingFootnotes)) {
        LOG_ERR("EHP", "Failed to finalize compact table cell");
        lowMemoryAbort = true;
      }
      pendingFootnotes.clear();
      currentTableCellIsHeader = false;
      currentTableCellColSpan = 1;
      currentTextRunBytes = 0;
      wordsExtractedInBlock = 0;
      nextWordContinues = false;
      return;
    }
    return;
  }

  if (!currentTableBuffer) {
    makePages();
    if (previewStopRequested) return;
    currentTextBlock.reset();
    placeTableImage(currentTableCellImage, currentTableCellImageSource);
    currentTableCellImage.reset();
    pendingFootnotes.clear();
    currentTableCellIsHeader = false;
    currentTableCellColSpan = 1;
    wordsExtractedInBlock = 0;
    nextWordContinues = false;
    return;
  }

  if (currentTableBuffer->rows.empty()) {
    currentTableBuffer->rows.push_back({});
  }

  BufferedTableCell cell;
  cell.isHeader = currentTableCellIsHeader;
  cell.colSpan = currentTableCellColSpan;
  cell.visibleTextOffset = currentTableCellVisibleOffset;
  cell.text = std::move(currentTextBlock);
  cell.footnotes = std::move(pendingFootnotes);
  cell.image = std::move(currentTableCellImage);
  cell.imageSource = currentTableCellImageSource;
  pendingFootnotes.clear();

  if (cell.text && cell.text->size() > MAX_SIMPLE_TABLE_CELL_WORDS) {
    currentTableBuffer->unsupported = true;
  }

  auto& row = currentTableBuffer->rows.back();
  row.hasHeaderCell = row.hasHeaderCell || cell.isHeader;
  row.hasDataCell = row.hasDataCell || !cell.isHeader;
  row.effectiveColumnCount = static_cast<uint16_t>(row.effectiveColumnCount + cell.colSpan);
  row.cells.push_back(std::move(cell));

  currentTableBuffer->totalCells++;
  currentTableBuffer->maxCols = std::max<uint16_t>(currentTableBuffer->maxCols, row.effectiveColumnCount);
  if (currentTableBuffer->maxCols > MAX_SIMPLE_TABLE_COLUMNS) {
    currentTableBuffer->unsupported = true;
  }

  currentTableCellIsHeader = false;
  currentTableCellColSpan = 1;
  wordsExtractedInBlock = 0;
  nextWordContinues = false;
}

void ChapterHtmlSlimParser::emitHorizontalRule(const BlockStyle& blockStyle, const bool visible) {
  if (partWordBufferIndex > 0) {
    flushPartWordBuffer();
  }

  if (currentTextBlock) {
    const BlockStyle parentBlockStyle = currentTextBlock->getBlockStyle();
    startNewTextBlock(parentBlockStyle);
  }

  if (!currentPage) {
    if (!startNewPage("horizontal rule")) {
      return;
    }
  }

  const int16_t lineHeight = static_cast<int16_t>(renderer.getLineHeight(fontId) * lineCompression + 0.5f);
  // Invisible thematic breaks retain publisher spacing without a default ornament gap.
  const int16_t defaultVerticalSpacing = visible ? static_cast<int16_t>(lineHeight / 2) : 0;
  const int16_t topSpacing =
      static_cast<int16_t>((blockStyle.marginTop > 0 ? blockStyle.marginTop : defaultVerticalSpacing) +
                           (blockStyle.paddingTop > 0 ? blockStyle.paddingTop : 0));
  const int16_t bottomSpacing =
      static_cast<int16_t>((blockStyle.marginBottom > 0 ? blockStyle.marginBottom : defaultVerticalSpacing) +
                           (blockStyle.paddingBottom > 0 ? blockStyle.paddingBottom : 0));
  const uint8_t ruleThickness = visible ? 2 : 0;
  const int16_t availableWidth =
      std::max<int16_t>(1, static_cast<int16_t>(viewportWidth - blockStyle.totalHorizontalInset()));
  const int16_t width = std::max<int16_t>(1, static_cast<int16_t>(availableWidth / 4));
  const int16_t xPos = static_cast<int16_t>(blockStyle.leftInset() + ((availableWidth - width) / 2));
  const int16_t totalHeight = static_cast<int16_t>(topSpacing + ruleThickness + bottomSpacing);

  if (!headingOpenerActive && !currentPage->elements.empty() && currentPageNextY + totalHeight > viewportHeight) {
    completeCurrentPage();
    completedPageCount++;
    stopPreviewIfPageLimitReached();
    if (previewStopRequested) {
      return;
    }
    if (!startNewPage("horizontal-rule page break")) {
      return;
    }
  }

  currentPageNextY += topSpacing;
  attachPendingPublisherPageMarkers(currentPageNextY);

  if (visible) {
    auto pageRule = makeUniqueNoThrow<PageHorizontalRule>(width, ruleThickness, xPos, currentPageNextY);
    if (!pageRule) {
      LOG_ERR("EHP", "Failed to create PageHorizontalRule");
      lowMemoryAbort = true;
      return;
    }
    noteContent(currentPageNextY, currentPageNextY + ruleThickness);
    currentPage->elements.push_back(std::move(pageRule));
    setCurrentPageVisibleOffset(visibleTextOffset);
    markCurrentPageFromCurrentElement();
  }
  currentPageNextY = static_cast<int16_t>(currentPageNextY + ruleThickness + bottomSpacing);
  headingOpenerActive = false;

  if (!pendingAnchorId.empty()) {
    anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
    pendingAnchorId.clear();
    pendingAnchorFromInlineA = false;
  }
}

void ChapterHtmlSlimParser::emitBufferedTableAsParagraphs(BufferedTable& table) {
  if (!currentPage) {
    if (!startNewPage("table paragraph fallback")) {
      return;
    }
  }

  if (table.blockStyle.marginTop > 0) {
    currentPageNextY += table.blockStyle.marginTop;
  }
  if (table.blockStyle.paddingTop > 0) {
    currentPageNextY += table.blockStyle.paddingTop;
  }

  for (auto& row : table.rows) {
    for (auto& cell : row.cells) {
      if (!cell.text) {
        if (!placeTableImage(cell.image, cell.imageSource) || previewStopRequested) return;
        continue;
      }

      pendingFootnotes = std::move(cell.footnotes);
      currentTextBlock = std::move(cell.text);
      wordsExtractedInBlock = 0;
      makePages();
      if (previewStopRequested) return;
      currentTextBlock.reset();
      pendingFootnotes.clear();
      if (!placeTableImage(cell.image, cell.imageSource) || previewStopRequested) return;
      if (lowMemoryAbort) {
        break;
      }
    }
    std::vector<BufferedTableCell>().swap(row.cells);
    if (lowMemoryAbort) {
      break;
    }
  }
  std::vector<BufferedTableRow>().swap(table.rows);
  if (lowMemoryAbort) {
    return;
  }

  if (table.blockStyle.marginBottom > 0) {
    currentPageNextY += table.blockStyle.marginBottom;
  }
  if (table.blockStyle.paddingBottom > 0) {
    currentPageNextY += table.blockStyle.paddingBottom;
  }

  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;
  if (extraParagraphSpacing) {
    currentPageNextY += lineHeight / 2;
  }
}

void ChapterHtmlSlimParser::noteTableSources(const std::vector<TableFragmentRow>& rows) {
  for (const auto& row : rows)
    for (const auto& cell : row.cells) {
      for (const auto& line : cell.lines) {
        if (!line) continue;
        currentPage->noteSourceLine(*line);
        resolveSourceAnchors(line->sourceMin(), line->sourceEnd());
      }
      if (cell.image) {
        currentPage->noteSourcePoint(cell.imageSource);
        resolveSourceAnchors(cell.imageSource, cell.imageSource == UINT32_MAX ? 0 : cell.imageSource + 1);
      }
    }
}

bool ChapterHtmlSlimParser::flushStreamingTableFragment(BufferedTable& table) {
  if (table.streamingFragmentRows.empty()) {
    return true;
  }

  if (!currentPage && !startNewPage("streaming table fragment")) {
    return false;
  }

  const int horizontalInset = table.blockStyle.totalHorizontalInset();
  const uint16_t tableWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;
  const uint16_t lineHeight = renderer.getLineHeight(fontId) * lineCompression;
  const uint16_t fragmentHeight = table.streamingFragmentHeight;
  noteTableSources(table.streamingFragmentRows);
  auto fragment = makeUniqueNoThrow<PageTableFragment>(
      tableWidth, table.streamingFragmentColumnCount, TABLE_CELL_PADDING, lineHeight,
      std::move(table.streamingFragmentRows), table.blockStyle.leftInset(), currentPageNextY);
  if (!fragment) {
    LOG_ERR("EHP", "Failed to create streaming PageTableFragment");
    lowMemoryAbort = true;
    return false;
  }

  noteContent(currentPageNextY, currentPageNextY + fragmentHeight);
  currentPage->elements.push_back(std::move(fragment));

  setCurrentPageVisibleOffset(table.streamingFragmentVisibleOffset);
  markCurrentPageFromCurrentElement();
  for (const auto& footnote : table.streamingFragmentFootnotes) {
    currentPage->addFootnote(footnote.number, footnote.href, footnote.linkId);
  }
  table.streamingFragmentFootnotes.clear();
  table.streamingFragmentRows.clear();
  table.streamingFragmentHeight = 1;
  table.streamingFragmentColumnCount = 0;
  currentPageNextY = static_cast<int16_t>(currentPageNextY + fragmentHeight);
  return true;
}

void ChapterHtmlSlimParser::emitStreamingTableRowsAsParagraphs(BufferedTable& table) {
  for (auto& row : table.rows) {
    for (auto& cell : row.cells) {
      if (!cell.text) {
        if (!placeTableImage(cell.image, cell.imageSource) || previewStopRequested) return;
        continue;
      }
      pendingFootnotes = std::move(cell.footnotes);
      currentTextBlock = std::move(cell.text);
      wordsExtractedInBlock = 0;
      makePages();
      if (previewStopRequested) return;
      currentTextBlock.reset();
      pendingFootnotes.clear();
      if (!placeTableImage(cell.image, cell.imageSource) || previewStopRequested) return;
      if (lowMemoryAbort) return;
    }
  }
  table.rows.clear();
  table.totalCells = 0;
  table.maxCols = 0;
}

void ChapterHtmlSlimParser::fallbackStreamingTableToParagraphs(const char* reason) {
  if (!currentTableBuffer) return;
  auto& table = *currentTableBuffer;
  LOG_DBG("EHP", "Streaming table fallback: %s", reason);
  if (!flushStreamingTableFragment(table)) return;
  if (!table.streamingTopSpacingApplied) {
    if (!currentPage && !startNewPage("streaming table paragraph fallback")) return;
    currentPageNextY =
        static_cast<int16_t>(currentPageNextY + table.blockStyle.marginTop + table.blockStyle.paddingTop);
    table.streamingTopSpacingApplied = true;
  }
  table.streamingFlattened = true;
  emitStreamingTableRowsAsParagraphs(table);
}

bool ChapterHtmlSlimParser::streamCurrentTableRow() {
  if (!currentTableBuffer || !currentTableBuffer->streaming || currentTableBuffer->rows.empty()) {
    return true;
  }

  auto& table = *currentTableBuffer;
  if (table.streamingFlattened) {
    emitStreamingTableRowsAsParagraphs(table);
    return !lowMemoryAbort;
  }

  auto& row = table.rows.back();
  if (table.unsupported || row.cells.empty()) {
    fallbackStreamingTableToParagraphs(table.unsupported ? "unsupported structure" : "empty row");
    return !lowMemoryAbort;
  }

  if (table.streamingColumnCount == 0) {
    table.streamingColumnCount = static_cast<uint8_t>(row.effectiveColumnCount);
  }
  const bool isFullWidthSingleCellRow = row.cells.size() == 1 && row.cells[0].colSpan == table.streamingColumnCount;
  const bool rowHasMergedCells =
      std::any_of(row.cells.begin(), row.cells.end(), [](const BufferedTableCell& cell) { return cell.colSpan != 1; });
  if (table.streamingColumnCount == 0 || table.streamingColumnCount > MAX_SIMPLE_TABLE_COLUMNS ||
      (row.effectiveColumnCount != table.streamingColumnCount && !isFullWidthSingleCellRow) ||
      (rowHasMergedCells && !isFullWidthSingleCellRow)) {
    fallbackStreamingTableToParagraphs("inconsistent column structure");
    return !lowMemoryAbort;
  }

  const uint8_t columnCount = isFullWidthSingleCellRow ? 1 : table.streamingColumnCount;
  const int horizontalInset = table.blockStyle.totalHorizontalInset();
  const uint16_t tableWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;
  for (uint8_t column = 0; column < columnCount; ++column) {
    if (TableColumnLayout::innerWidth(tableWidth, columnCount, column, 1, TABLE_CELL_PADDING) < 20) {
      fallbackStreamingTableToParagraphs("column width too small");
      return !lowMemoryAbort;
    }
  }

  TableFragmentRow fragmentRow;
  fragmentRow.cells.resize(columnCount);
  fragmentRow.headerSeparator = row.hasHeaderCell && !row.hasDataCell;
  const uint16_t lineHeight = renderer.getLineHeight(fontId) * lineCompression;
  uint32_t rowHeight = static_cast<uint32_t>(lineHeight) + TABLE_CELL_PADDING * 2;
  std::vector<FootnoteEntry> rowFootnotes;
  for (size_t cellIndex = 0; cellIndex < row.cells.size(); ++cellIndex) {
    const auto& sourceCell = row.cells[cellIndex];
    auto& destCell = fragmentRow.cells[cellIndex];
    destCell.isHeader = sourceCell.isHeader;
    if (sourceCell.text &&
        !sourceCell.text->layoutAndExtractLinesPreservingSource(
            renderer, fontId,
            TableColumnLayout::innerWidth(tableWidth, columnCount, static_cast<uint8_t>(cellIndex), 1,
                                          TABLE_CELL_PADDING),
            [&destCell](const std::shared_ptr<TextBlock>& textBlock) { destCell.lines.push_back(textBlock); }, true)) {
      fallbackStreamingTableToParagraphs("cell layout failed");
      return !lowMemoryAbort;
    }
    for (const auto& [wordIndex, footnote] : sourceCell.footnotes) {
      (void)wordIndex;
      rowFootnotes.push_back(footnote);
    }
    if (destCell.lines.size() > TableFragmentCell::MAX_SERIALIZED_LINES) {
      fallbackStreamingTableToParagraphs("cell has too many lines");
      return !lowMemoryAbort;
    }
    if (sourceCell.image) {
      destCell.image = sourceCell.image->fittedTo(
          TableColumnLayout::innerWidth(tableWidth, columnCount, static_cast<uint8_t>(cellIndex), 1,
                                        TABLE_CELL_PADDING),
          std::min<int>(TableFragmentCell::MAX_IMAGE_HEIGHT,
                        std::max(1, int(viewportHeight) - TABLE_CELL_PADDING * 2 - 1)));
      if (!destCell.image) {
        lowMemoryAbort = true;
        return false;
      }
      destCell.imageSource = sourceCell.imageSource;
    }
    const uint32_t cellHeight =
        (destCell.image ? destCell.lines.size() : std::max<size_t>(1, destCell.lines.size())) * lineHeight +
        TABLE_CELL_PADDING * 2 + (destCell.image ? destCell.image->getHeight() : 0);
    if (cellHeight > viewportHeight) {
      fallbackStreamingTableToParagraphs("row exceeds viewport");
      return !lowMemoryAbort;
    }
    rowHeight = std::max(rowHeight, cellHeight);
  }
  fragmentRow.height = static_cast<uint16_t>(rowHeight);

  if (!currentPage && !startNewPage("streaming table row")) return false;
  if (!table.streamingTopSpacingApplied) {
    currentPageNextY =
        static_cast<int16_t>(currentPageNextY + table.blockStyle.marginTop + table.blockStyle.paddingTop);
    table.streamingTopSpacingApplied = true;
  }

  const bool startsNewFragment =
      !table.streamingFragmentRows.empty() &&
      (table.streamingFragmentColumnCount != columnCount ||
       table.streamingFragmentRows.size() >= PageTableFragment::MAX_SERIALIZED_ROWS ||
       currentPageNextY + table.streamingFragmentHeight + fragmentRow.height > viewportHeight);
  if (startsNewFragment && !flushStreamingTableFragment(table)) return false;

  if (table.streamingFragmentRows.empty() && currentPageNextY + 1 + fragmentRow.height > viewportHeight &&
      !currentPage->elements.empty()) {
    completeCurrentPage();
    completedPageCount++;
    stopPreviewIfPageLimitReached();
    if (previewStopRequested) return true;
    if (!startNewPage("streaming table page break")) return false;
  }

  if (table.streamingFragmentRows.empty()) {
    table.streamingFragmentColumnCount = columnCount;
    table.streamingFragmentVisibleOffset = row.cells.front().visibleTextOffset;
  }
  table.streamingFragmentHeight = static_cast<uint16_t>(table.streamingFragmentHeight + fragmentRow.height);
  table.streamingFragmentRows.push_back(std::move(fragmentRow));
  table.streamingFragmentFootnotes.insert(table.streamingFragmentFootnotes.end(), rowFootnotes.begin(),
                                          rowFootnotes.end());
  std::vector<BufferedTableRow>().swap(table.rows);
  table.totalCells = 0;
  table.maxCols = 0;
  return true;
}

void ChapterHtmlSlimParser::finishStreamingTable(BufferedTable& table) {
  if (!table.streamingFlattened && !streamCurrentTableRow()) return;
  if (!flushStreamingTableFragment(table)) return;
  currentPageNextY =
      static_cast<int16_t>(currentPageNextY + table.blockStyle.marginBottom + table.blockStyle.paddingBottom);
  if (extraParagraphSpacing) {
    currentPageNextY += renderer.getLineHeight(fontId) * lineCompression / 2;
  }
}

bool ChapterHtmlSlimParser::flushCompactTableFragment() {
  if (compactFragmentRows.empty()) return true;
  if (!currentPage && !startNewPage("compact table fragment")) return false;

  const auto& style = currentCompactTable->tableStyle();
  const int horizontalInset = style.totalHorizontalInset();
  const uint16_t width =
      horizontalInset < viewportWidth ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;
  const uint16_t lineHeight = renderer.getLineHeight(fontId) * lineCompression;
  noteTableSources(compactFragmentRows);
  auto fragment =
      makeUniqueNoThrow<PageTableFragment>(width, compactFragmentColumnCount, TABLE_CELL_PADDING, lineHeight,
                                           std::move(compactFragmentRows), style.leftInset(), currentPageNextY);
  if (!fragment) {
    LOG_ERR("EHP", "Failed to create compact PageTableFragment");
    lowMemoryAbort = true;
    return false;
  }

  const uint16_t fragmentHeight = compactFragmentHeight;
  noteContent(currentPageNextY, currentPageNextY + fragmentHeight);
  currentPage->elements.push_back(std::move(fragment));

  setCurrentPageVisibleOffset(compactFragmentVisibleOffset);
  markCurrentPageFromCurrentElement();
  for (const auto& footnote : compactFragmentFootnotes) {
    currentPage->addFootnote(footnote.number, footnote.href, footnote.linkId);
  }
  compactFragmentFootnotes.clear();
  compactFragmentRows.clear();
  compactFragmentHeight = 1;
  compactFragmentColumnCount = 0;
  currentPageNextY = static_cast<int16_t>(currentPageNextY + fragmentHeight);
  return true;
}

bool ChapterHtmlSlimParser::emitCompactTableRow(TableFragmentRow& row,
                                                std::vector<std::shared_ptr<TextBlock>>& flatLines,
                                                const std::vector<FootnoteEntry>& footnotes,
                                                const uint32_t rowVisibleOffset, const uint8_t fragmentColumnCount,
                                                const bool flatten) {
  if (!currentCompactTable) return false;
  const auto& style = currentCompactTable->tableStyle();
  if (!currentPage && !startNewPage(flatten ? "compact table paragraph fallback" : "compact table row")) return false;
  if (!compactTableTopSpacingApplied) {
    currentPageNextY = static_cast<int16_t>(currentPageNextY + style.marginTop + style.paddingTop);
    compactTableTopSpacingApplied = true;
  }

  if (flatten) {
    if (!flushCompactTableFragment()) return false;
    const auto emitLine = [&](const std::shared_ptr<TextBlock>& line) {
      if (!line) return true;
      if (!currentPage->elements.empty() &&
          currentPageNextY + renderer.getLineHeight(fontId) * lineCompression > viewportHeight) {
        completeCurrentPage();
        completedPageCount++;
        stopPreviewIfPageLimitReached();
        // A preview that reached its page limit has finished normally.
        if (previewStopRequested) return true;
        if (!startNewPage("compact table paragraph page break")) return false;
      }
      auto pageLine = makeUniqueNoThrow<PageLine>(line, style.leftInset(), currentPageNextY);
      if (!pageLine) {
        LOG_ERR("EHP", "Failed to create compact table paragraph line");
        lowMemoryAbort = true;
        return false;
      }
      currentPage->noteSourceLine(*pageLine->getBlock());
      resolveSourceAnchors(pageLine->getBlock()->sourceMin(), pageLine->getBlock()->sourceEnd());
      currentPage->elements.push_back(std::move(pageLine));
      setCurrentPageVisibleOffset(rowVisibleOffset);
      markCurrentPageFromCurrentElement();
      currentPageNextY = static_cast<int16_t>(currentPageNextY + renderer.getLineHeight(fontId) * lineCompression);
      return true;
    };
    if (!row.cells.empty()) {
      for (const auto& cell : row.cells) {
        for (const auto& line : cell.lines)
          if (!emitLine(line) || previewStopRequested) return !lowMemoryAbort;
        if (!placeTableImage(cell.image, cell.imageSource) || previewStopRequested) return !lowMemoryAbort;
      }
    } else {
      for (const auto& line : flatLines)
        if (!emitLine(line) || previewStopRequested) return !lowMemoryAbort;
    }
    for (const auto& footnote : footnotes) {
      currentPage->addFootnote(footnote.number, footnote.href, footnote.linkId);
    }
    return !lowMemoryAbort;
  }

  if (row.cells.empty() || row.cells.size() > TableFragmentRow::MAX_SERIALIZED_CELLS) {
    return false;
  }
  const uint8_t columnCount = fragmentColumnCount;
  const bool startsNewFragment =
      !compactFragmentRows.empty() && (compactFragmentColumnCount != columnCount ||
                                       compactFragmentRows.size() >= PageTableFragment::MAX_SERIALIZED_ROWS ||
                                       currentPageNextY + compactFragmentHeight + row.height > viewportHeight);
  if (startsNewFragment && !flushCompactTableFragment()) return false;

  if (compactFragmentRows.empty() && currentPageNextY + 1 + row.height > viewportHeight &&
      !currentPage->elements.empty()) {
    completeCurrentPage();
    completedPageCount++;
    stopPreviewIfPageLimitReached();
    if (previewStopRequested) return true;
    if (!startNewPage("compact table page break")) return false;
  }

  if (compactFragmentRows.empty()) {
    compactFragmentColumnCount = columnCount;
    compactFragmentVisibleOffset = rowVisibleOffset;
  }
  compactFragmentHeight = static_cast<uint16_t>(compactFragmentHeight + row.height);
  const size_t availableFootnoteSlots = Page::MAX_FOOTNOTES_PER_PAGE > compactFragmentFootnotes.size()
                                            ? Page::MAX_FOOTNOTES_PER_PAGE - compactFragmentFootnotes.size()
                                            : 0;
  const size_t footnotesToCopy = std::min(availableFootnoteSlots, footnotes.size());
  compactFragmentFootnotes.insert(compactFragmentFootnotes.end(), footnotes.begin(),
                                  footnotes.begin() + footnotesToCopy);
  compactFragmentRows.push_back(std::move(row));
  return true;
}

void ChapterHtmlSlimParser::finishCompactTable() {
  if (!currentCompactTable) return;
  if (!flushCompactTableFragment()) return;
  const auto& style = currentCompactTable->tableStyle();
  currentPageNextY = static_cast<int16_t>(currentPageNextY + style.marginBottom + style.paddingBottom);
  if (extraParagraphSpacing) currentPageNextY += renderer.getLineHeight(fontId) * lineCompression / 2;
}

void ChapterHtmlSlimParser::emitBufferedTableAsFragments(BufferedTable& table) {
  struct PreparedRow {
    TableFragmentRow fragmentRow;
    std::vector<FootnoteEntry> footnotes;
    uint32_t visibleTextOffset = 0;
  };

  struct PreparedSegment {
    uint8_t columnCount = 0;
    std::vector<PreparedRow> rows;
  };

  if (!currentPage) {
    if (!startNewPage("table fragments")) {
      return;
    }
  }

  const int horizontalInset = table.blockStyle.totalHorizontalInset();
  const uint16_t tableWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;
  const uint16_t lineHeight = renderer.getLineHeight(fontId) * lineCompression;
  std::vector<PreparedSegment> preparedSegments;
  preparedSegments.reserve(table.rows.size());

  auto releasePreparedSegments = [&preparedSegments]() {
    for (auto& segment : preparedSegments) {
      for (auto& row : segment.rows) {
        std::vector<TableFragmentCell>().swap(row.fragmentRow.cells);
        std::vector<FootnoteEntry>().swap(row.footnotes);
      }
      std::vector<PreparedRow>().swap(segment.rows);
    }
    std::vector<PreparedSegment>().swap(preparedSegments);
  };

  auto prepareRow = [&](const BufferedTableRow& row, const uint8_t columnCount, PreparedSegment& segment) -> bool {
    if (columnCount == 0) {
      LOG_DBG("EHP", "Table layout fallback: width %u too small for %u columns", tableWidth, columnCount);
      return false;
    }
    for (uint8_t column = 0; column < columnCount; ++column) {
      if (TableColumnLayout::innerWidth(tableWidth, columnCount, column, 1, TABLE_CELL_PADDING) < 20) {
        LOG_DBG("EHP", "Table layout fallback: width %u too small for %u columns", tableWidth, columnCount);
        return false;
      }
    }

    PreparedRow prepared;
    prepared.visibleTextOffset = row.cells.empty() ? visibleTextOffset : row.cells.front().visibleTextOffset;
    prepared.fragmentRow.cells.resize(columnCount);
    prepared.fragmentRow.headerSeparator = row.hasHeaderCell && !row.hasDataCell;

    uint32_t rowHeight = static_cast<uint32_t>(lineHeight) + TABLE_CELL_PADDING * 2;
    if (rowHeight > viewportHeight) {
      LOG_DBG("EHP", "Table layout fallback: row height %lu exceeds viewport %u", static_cast<unsigned long>(rowHeight),
              viewportHeight);
      return false;
    }
    for (size_t colIndex = 0; colIndex < row.cells.size(); colIndex++) {
      const auto& sourceCell = row.cells[colIndex];
      auto& destCell = prepared.fragmentRow.cells[colIndex];
      destCell.isHeader = sourceCell.isHeader;

      if (sourceCell.text) {
        if (!sourceCell.text->layoutAndExtractLinesPreservingSource(
                renderer, fontId,
                TableColumnLayout::innerWidth(tableWidth, columnCount, static_cast<uint8_t>(colIndex), 1,
                                              TABLE_CELL_PADDING),
                [&destCell](const std::shared_ptr<TextBlock>& textBlock) { destCell.lines.push_back(textBlock); },
                true)) {
          LOG_DBG("EHP", "Table layout fallback: cell text layout failed");
          return false;
        }
      }

      for (const auto& [wordIndex, footnote] : sourceCell.footnotes) {
        (void)wordIndex;
        prepared.footnotes.push_back(footnote);
      }

      if (destCell.lines.size() > TableFragmentCell::MAX_SERIALIZED_LINES) {
        LOG_DBG("EHP", "Table layout fallback: cell line count %u exceeds fragment max %u",
                static_cast<uint32_t>(destCell.lines.size()), TableFragmentCell::MAX_SERIALIZED_LINES);
        return false;
      }

      if (sourceCell.image) {
        destCell.image = sourceCell.image->fittedTo(
            TableColumnLayout::innerWidth(tableWidth, columnCount, static_cast<uint8_t>(colIndex), 1,
                                          TABLE_CELL_PADDING),
            std::min<int>(TableFragmentCell::MAX_IMAGE_HEIGHT,
                          std::max(1, int(viewportHeight) - TABLE_CELL_PADDING * 2 - 1)));
        if (!destCell.image) {
          lowMemoryAbort = true;
          return false;
        }
        destCell.imageSource = sourceCell.imageSource;
      }
      const uint32_t cellLineCount =
          destCell.image ? destCell.lines.size() : std::max<size_t>(1, destCell.lines.size());
      const uint32_t cellHeight =
          cellLineCount * lineHeight + TABLE_CELL_PADDING * 2 + (destCell.image ? destCell.image->getHeight() : 0);
      if (cellHeight > viewportHeight) {
        LOG_DBG("EHP", "Table layout fallback: row height %lu exceeds viewport %u",
                static_cast<unsigned long>(cellHeight), viewportHeight);
        return false;
      }

      rowHeight = std::max<uint32_t>(rowHeight, cellHeight);
    }

    prepared.fragmentRow.height = static_cast<uint16_t>(rowHeight);
    segment.rows.push_back(std::move(prepared));
    return true;
  };

  for (const auto& row : table.rows) {
    const bool rowHasMergedCells = std::any_of(row.cells.begin(), row.cells.end(),
                                               [](const BufferedTableCell& cell) { return cell.colSpan != 1; });
    const bool isFullWidthSingleCellRow =
        row.cells.size() == 1 && table.maxCols > 0 && row.cells[0].colSpan == table.maxCols;

    if (rowHasMergedCells && !isFullWidthSingleCellRow) {
      LOG_DBG("EHP", "Table layout fallback: unsupported colspan structure");
      releasePreparedSegments();
      emitBufferedTableAsParagraphs(table);
      return;
    }

    const uint8_t segmentColumnCount = isFullWidthSingleCellRow ? 1 : static_cast<uint8_t>(table.maxCols);
    if (preparedSegments.empty() || preparedSegments.back().columnCount != segmentColumnCount) {
      preparedSegments.push_back({});
      preparedSegments.back().columnCount = segmentColumnCount;
      preparedSegments.back().rows.reserve(table.rows.size());
    }

    if (!prepareRow(row, segmentColumnCount, preparedSegments.back())) {
      releasePreparedSegments();
      emitBufferedTableAsParagraphs(table);
      return;
    }
  }

  if (table.blockStyle.marginTop > 0) {
    currentPageNextY += table.blockStyle.marginTop;
  }
  if (table.blockStyle.paddingTop > 0) {
    currentPageNextY += table.blockStyle.paddingTop;
  }
  for (auto& segment : preparedSegments) {
    size_t nextRowIndex = 0;
    while (nextRowIndex < segment.rows.size()) {
      if (!currentPage) {
        if (!startNewPage("table fragment continuation")) {
          return;
        }
      }

      std::vector<TableFragmentRow> fragmentRows;
      std::vector<FootnoteEntry> fragmentFootnotes;
      fragmentRows.reserve(std::min<size_t>(segment.rows.size() - nextRowIndex, INITIAL_TABLE_FRAGMENT_ROW_RESERVE));
      uint16_t fragmentHeight = 1;  // Bottom border.
      const uint32_t fragmentVisibleOffset = segment.rows[nextRowIndex].visibleTextOffset;

      while (nextRowIndex < segment.rows.size()) {
        const uint16_t nextHeight =
            static_cast<uint16_t>(fragmentHeight + segment.rows[nextRowIndex].fragmentRow.height);
        if (!fragmentRows.empty() && currentPageNextY + nextHeight > viewportHeight) {
          break;
        }
        if (fragmentRows.empty() && currentPageNextY + nextHeight > viewportHeight && !currentPage->elements.empty()) {
          completeCurrentPage();
          completedPageCount++;
          stopPreviewIfPageLimitReached();
          if (previewStopRequested) {
            return;
          }
          if (!startNewPage("table fragment page break")) {
            return;
          }
          continue;
        }

        fragmentHeight = nextHeight;
        fragmentRows.push_back(std::move(segment.rows[nextRowIndex].fragmentRow));
        fragmentFootnotes.insert(fragmentFootnotes.end(), segment.rows[nextRowIndex].footnotes.begin(),
                                 segment.rows[nextRowIndex].footnotes.end());
        nextRowIndex++;
      }

      if (fragmentRows.empty()) {
        fragmentHeight = static_cast<uint16_t>(1 + segment.rows[nextRowIndex].fragmentRow.height);
        fragmentRows.push_back(std::move(segment.rows[nextRowIndex].fragmentRow));
        fragmentFootnotes.insert(fragmentFootnotes.end(), segment.rows[nextRowIndex].footnotes.begin(),
                                 segment.rows[nextRowIndex].footnotes.end());
        nextRowIndex++;
      }

      noteTableSources(fragmentRows);
      auto fragment =
          makeUniqueNoThrow<PageTableFragment>(tableWidth, segment.columnCount, TABLE_CELL_PADDING, lineHeight,
                                               std::move(fragmentRows), table.blockStyle.leftInset(), currentPageNextY);
      if (!fragment) {
        LOG_ERR("EHP", "Failed to create PageTableFragment");
        lowMemoryAbort = true;
        return;
      }
      noteContent(currentPageNextY, currentPageNextY + fragmentHeight);
      currentPage->elements.push_back(std::move(fragment));

      setCurrentPageVisibleOffset(fragmentVisibleOffset);
      markCurrentPageFromCurrentElement();
      for (const auto& footnote : fragmentFootnotes) {
        currentPage->addFootnote(footnote.number, footnote.href, footnote.linkId);
      }
      currentPageNextY += fragmentHeight;

      if (nextRowIndex < segment.rows.size()) {
        completeCurrentPage();
        completedPageCount++;
        stopPreviewIfPageLimitReached();
        if (previewStopRequested) {
          return;
        }
        if (!startNewPage("table fragment split")) {
          return;
        }
      }
    }
  }

  if (table.blockStyle.marginBottom > 0) {
    currentPageNextY += table.blockStyle.marginBottom;
  }
  if (table.blockStyle.paddingBottom > 0) {
    currentPageNextY += table.blockStyle.paddingBottom;
  }

  if (extraParagraphSpacing) {
    currentPageNextY += lineHeight / 2;
  }
}

void ChapterHtmlSlimParser::emitCurrentTableBuffer() {
  if (!currentTableBuffer) {
    return;
  }

  auto table = std::move(currentTableBuffer);
  currentTableCellIsHeader = false;

  if (table->rows.empty() || table->maxCols == 0) {
    return;
  }

  if (table->unsupported) {
    LOG_DBG("EHP", "Table layout fallback: unsupported structure (%u rows, %u cols, %u cells)",
            static_cast<uint32_t>(table->rows.size()), table->maxCols, table->totalCells);
    emitBufferedTableAsParagraphs(*table);
    return;
  }

  emitBufferedTableAsFragments(*table);
}

void ChapterHtmlSlimParser::fallbackCurrentTableBufferToParagraphs(const char* reason) {
  if (!currentTableBuffer) {
    return;
  }

  const auto heap = MemoryBudget::snapshot();
  LOG_DBG("EHP", "Table layout fallback: %s (%u rows, %u cols, %u cells, free=%u, maxAlloc=%u)", reason,
          static_cast<uint32_t>(currentTableBuffer->rows.size()), currentTableBuffer->maxCols,
          currentTableBuffer->totalCells, heap.freeHeap, heap.maxAllocHeap);

  auto activeTextBlock = std::move(currentTextBlock);
  auto activeFootnotes = std::move(pendingFootnotes);
  const int activeWordsExtracted = wordsExtractedInBlock;
  const bool activeNextWordContinues = nextWordContinues;
  const bool activeTableCellIsHeader = currentTableCellIsHeader;
  const uint8_t activeTableCellColSpan = currentTableCellColSpan;

  emitBufferedTableAsParagraphs(*currentTableBuffer);
  currentTableBuffer.reset();

  currentTextBlock = std::move(activeTextBlock);
  pendingFootnotes = std::move(activeFootnotes);
  wordsExtractedInBlock = activeWordsExtracted;
  nextWordContinues = activeNextWordContinues;
  currentTableCellIsHeader = activeTableCellIsHeader;
  currentTableCellColSpan = activeTableCellColSpan;
}

void ChapterHtmlSlimParser::flushMalformedPartialContent() {
  if (partWordBufferIndex > 0) {
    flushPartWordBuffer();
  }
  // A missing closing span must not strand the preceding paragraph in the
  // bounded float capture while the surviving chapter content is finalized.
  if (textFloatParent_) finishTextFloat();
  if (lowMemoryAbort || previewStopRequested) return;

  if (tableDepth > 0) {
    if (tableDepth == 1 && currentTextBlock) {
      finalizeCurrentTableCell();
    }
    if (tableDepth == 1 && currentCompactTable) {
      if (currentCompactTable->hasActiveCell()) {
        currentCompactTable->endCell(pendingFootnotes);
        pendingFootnotes.clear();
      }
      if (currentCompactTable->hasActiveRow()) {
        currentCompactTable->setFlattened();
        TableFragmentRow row;
        std::vector<std::shared_ptr<TextBlock>> flatLines;
        std::vector<FootnoteEntry> rowFootnotes;
        rowFootnotes.reserve(Page::MAX_FOOTNOTES_PER_PAGE);
        uint32_t rowVisibleOffset = visibleTextOffset;
        const auto result = currentCompactTable->finishRow(row, flatLines, rowFootnotes, rowVisibleOffset);
        if (result == CompactTableLayout::RowResult::Abort) {
          lowMemoryAbort = true;
        } else if (result == CompactTableLayout::RowResult::Flatten) {
          compactTableFlattened = true;
          if (!emitCompactTableRow(row, flatLines, rowFootnotes, rowVisibleOffset,
                                   currentCompactTable->fragmentColumnCount(), true)) {
            lowMemoryAbort = true;
          }
        }
      }
      finishCompactTable();
      currentCompactTable.reset();
    } else if (currentTableBuffer) {
      fallbackCurrentTableBufferToParagraphs("malformed markup");
    }
    tableDepth = 0;
    tableRowIndex = 0;
    tableColIndex = 0;
    currentTableCellIsHeader = false;
    currentTableCellColSpan = 1;
  }
}

bool ChapterHtmlSlimParser::appendMalformedMarkupWarningPage() {
  if (currentPage && !currentPage->elements.empty()) {
    completeCurrentPage();
    completedPageCount++;
  }
  currentPage.reset();

  if (!startNewPage("malformed markup warning")) {
    return false;
  }

  const uint16_t textWidth =
      viewportWidth > WARNING_PAGE_SIDE_MARGIN * 2 ? viewportWidth - WARNING_PAGE_SIDE_MARGIN * 2 : viewportWidth;
  const auto titleLines = renderer.wrappedText(fontId, tr(STR_EPUB_CHAPTER_INCOMPLETE_TITLE), textWidth,
                                               WARNING_TITLE_MAX_LINES, EpdFontFamily::BOLD);
  const auto bodyLines =
      renderer.wrappedText(fontId, tr(STR_EPUB_CHAPTER_INCOMPLETE_BODY), textWidth, WARNING_BODY_MAX_LINES);
  const int lineHeight = effectiveLineHeight();
  const int titleBodyGap = lineHeight;
  const int contentLineCount = static_cast<int>(titleLines.size() + bodyLines.size());
  const int contentHeight =
      contentLineCount * lineHeight + (!titleLines.empty() && !bodyLines.empty() ? titleBodyGap : 0);
  int y = std::max(0, (static_cast<int>(viewportHeight) - contentHeight) / 2);

  auto addLine = [this](const std::string& line, const EpdFontFamily::Style style, const int yPos) -> bool {
    std::vector<std::string> words;
    std::vector<int16_t> xPos;
    std::vector<EpdFontFamily::Style> styles;
    words.reserve(1);
    xPos.reserve(1);
    styles.reserve(1);
    words.push_back(line);
    xPos.push_back(0);
    styles.push_back(style);

    auto block = std::make_shared<TextBlock>(std::move(words), std::move(xPos), std::move(styles),
                                             std::vector<uint8_t>{}, std::vector<uint16_t>{}, std::vector<uint16_t>{},
                                             std::vector<uint8_t>{}, std::vector<bool>{false});
    auto pageLine = makeUniqueNoThrow<PageLine>(
        std::move(block),
        static_cast<int16_t>(
            std::max(0, (static_cast<int>(viewportWidth) - renderer.getTextWidth(fontId, line.c_str(), style)) / 2)),
        static_cast<int16_t>(yPos));
    if (!pageLine) {
      LOG_ERR("EHP", "Failed to create malformed-markup warning line");
      lowMemoryAbort = true;
      return false;
    }
    currentPage->noteSourceLine(*pageLine->getBlock());
    resolveSourceAnchors(pageLine->getBlock()->sourceMin(), pageLine->getBlock()->sourceEnd());
    currentPage->elements.push_back(std::move(pageLine));
    setCurrentPageVisibleOffset(visibleTextOffset);
    return true;
  };

  for (const auto& line : titleLines) {
    if (!addLine(line, EpdFontFamily::BOLD, y)) return false;
    y += lineHeight;
  }
  if (!titleLines.empty() && !bodyLines.empty()) {
    y += titleBodyGap;
  }
  for (const auto& line : bodyLines) {
    if (!addLine(line, EpdFontFamily::REGULAR, y)) return false;
    y += lineHeight;
  }

  if (!currentPage || currentPage->elements.empty()) {
    LOG_ERR("EHP", "Failed to create malformed markup warning page");
    return false;
  }

  completeCurrentPage();
  completedPageCount++;
  currentPage.reset();
  return true;
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  if (self->isScanningForPreviewAnchor()) {
    if (self->handlePreviewScanStart(atts)) {
      return;
    }
  }
  if (self->previewStopRequested) {
    return;
  }
  if (self->shouldAbortForLowMemory("element start")) {
    return;
  }

  if (self->trackReferenceCharacters && self->referenceExcludedUntilDepth == INT_MAX &&
      (strcmp(name, "head") == 0 || strcmp(name, "script") == 0 || strcmp(name, "style") == 0 ||
       strcmp(name, "svg") == 0 || strcmp(name, "metadata") == 0)) {
    self->referenceExcludedUntilDepth = self->depth;
  }

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    self->depth += 1;
    return;
  }

  // Inline SVG wrappers can contain raster covers. Other SVG markup remains
  // outside text layout; external SVG illustrations use the bounded decoder.
  if (self->svgUntilDepth < self->depth && !matches(localTagName(name), IMAGE_TAGS, std::size(IMAGE_TAGS))) {
    self->depth += 1;
    return;
  }

  if (strcmp(name, "p") == 0) {
    self->xpathParagraphIndex++;
  }
  if (strcmp(name, "li") == 0) {
    self->xpathListItemIndex++;
  }

  // Skip before ID/TOC processing: invisible targets must not create anchors
  // or page breaks on the following visible block. Empty hidden values count.
  if (getAttribute(atts, "hidden") != nullptr) {
    self->skipCurrentElement();
    return;
  }

  self->emitPendingGeneratedBefore();
  if (self->lowMemoryAbort) return;

  // Borrow parser-owned attribute bytes during this callback; copy only when
  // storing an ancestor/id beyond the current element start.
  const char* rawId = getAttribute(atts, "id");
  const std::string_view idAttr = rawId ? rawId : "";
  std::string_view classAttr;
  std::string_view styleAttr;
  const char* dirAttr = nullptr;
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      const char* attrValue = atts[i + 1] ? atts[i + 1] : "";
      if (strcmp(atts[i], "class") == 0) {
        classAttr = attrValue;
      } else if (strcmp(atts[i], "style") == 0) {
        styleAttr = attrValue;
      } else if (strcmp(atts[i], "dir") == 0) {
        dirAttr = attrValue;
      }
    }
  }

  auto centeredBlockStyle = BlockStyle();
  centeredBlockStyle.textAlignDefined = true;
  centeredBlockStyle.alignment = CssTextAlign::Center;

  // Compute CSS style for this element early so display:none can short-circuit
  // before tag-specific branches emit any content or metadata.
  CssStyle cssStyle;
  if (strcmp(name, "pre") == 0) {
    cssStyle.whiteSpace = CssWhiteSpace::Pre;
    cssStyle.preserveWhitespace = true;
    cssStyle.defined.whiteSpace = 1;
    self->dropPreInitialNewline_ = true;
    self->previousPreCr_ = false;
    self->preColumnMod8_ = 0;
  }
  if (strcmp(name, "pre") == 0 || strcmp(name, "code") == 0 || strcmp(name, "kbd") == 0 || strcmp(name, "samp") == 0 ||
      strcmp(name, "tt") == 0) {
    cssStyle.fontFamily = genericFont::Monospace;
    cssStyle.defined.fontFamily = 1;
  }
  if (strcmp(name, "fieldset") == 0) {
    // Reader defaults sit below publisher CSS. Use a crisp e-ink outline instead
    // of a browser's groove border, reusing the existing bounded box renderer.
    for (auto& side : cssStyle.borders) side = {1, CssBorderStyle::Solid};
    cssStyle.defined.border = 1;
    cssStyle.borderStyleDefined = cssStyle.borderWidthDefined = 0x0F;
    cssStyle.marginLeft = cssStyle.marginRight = CssLength(2);
    cssStyle.defined.marginLeft = cssStyle.defined.marginRight = 1;
    cssStyle.paddingTop = CssLength(0.35f, CssUnit::Em);
    cssStyle.paddingBottom = CssLength(0.625f, CssUnit::Em);
    cssStyle.paddingLeft = cssStyle.paddingRight = CssLength(0.75f, CssUnit::Em);
    cssStyle.defined.paddingTop = cssStyle.defined.paddingBottom = 1;
    cssStyle.defined.paddingLeft = cssStyle.defined.paddingRight = 1;
  }
  if (self->cssParser) {
    cssStyle.applyOver(self->resolveElementStyle(name, classAttr, idAttr));
    if (!styleAttr.empty()) {
      CssStyle inlineStyle = self->resolveInlineStyle(styleAttr);
      cssStyle.applyOver(inlineStyle);
    }
    if (self->shouldAbortForLowMemory("CSS style resolution")) {
      return;
    }
  }

  // Remove only the publisher's outer page box after resolving the cascade.
  // Nested quotations, lists and paragraph spacing keep their authored values.
  if (self->ignorePublisherPageMargins && (strcmp(name, "html") == 0 || strcmp(name, "body") == 0)) {
    cssStyle.marginTop = cssStyle.marginBottom = cssStyle.marginLeft = cssStyle.marginRight = CssLength{};
    cssStyle.paddingTop = cssStyle.paddingBottom = cssStyle.paddingLeft = cssStyle.paddingRight = CssLength{};
  }

  if (dirAttr && dirAttr[0] != '\0') {
    if (strcasecmp(dirAttr, "rtl") == 0) {
      cssStyle.direction = CssTextDirection::Rtl;
      cssStyle.defined.direction = 1;
    } else if (strcasecmp(dirAttr, "ltr") == 0) {
      cssStyle.direction = CssTextDirection::Ltr;
      cssStyle.defined.direction = 1;
    }
  }

  if (!cssStyle.hasDirection() && self->effectiveDirectionDefined) {
    cssStyle.direction = self->effectiveDirection;
    cssStyle.defined.direction = 1;
  }

  // font-variant-caps is inherited in CSS; propagate the parent's small-caps
  // state to this element's style unless it sets its own (including "normal",
  // which explicitly opts back out).
  if (!cssStyle.hasFontVariantCaps() && self->effectiveSmallCaps) {
    cssStyle.fontVariantCaps = (self->effectiveTypography & fontTypography::AllSmallCaps)
                                   ? CssFontVariantCaps::AllSmallCaps
                                   : CssFontVariantCaps::SmallCaps;
    cssStyle.defined.fontVariantCaps = 1;
  }

  // Skip elements with display:none before all fast paths (tables, links, etc.).
  if ((cssStyle.hasDisplay() && cssStyle.display == CssDisplay::None) || cssStyle.isElementHidden()) {
    for (size_t i = 0; i < self->boxScopeCount; ++i) self->boxScopes[i].hiddenContent = true;
    self->skipCurrentElement();
    return;
  }

  const bool opensBlockSpan = self->embeddedStyle && !self->isLightMode() && strcmp(name, "span") == 0 &&
                              cssStyle.hasDisplay() && cssStyle.display == CssDisplay::Block && self->tableDepth == 0 &&
                              self->displayBlockSpanDepth_ < 0 && cssStyle.cssFloat != CssFloat::Left &&
                              cssStyle.cssFloat != CssFloat::Right;
  if (opensBlockSpan) {
    self->displayBlockSpanDepth_ = self->depth;
    self->displayBlockParentStyle_ = self->currentTextBlock ? self->currentTextBlock->getBlockStyle() : BlockStyle{};
    self->displayBlockParentCss_ = self->currentCssStyle;
  }
  // Converter-generated zero-height paragraphs retain the paragraph boundary,
  // but their synthetic NBSP payload cannot join adjacent real prose.
  if (strcmp(name, "p") == 0 && !styleAttr.empty()) {
    const auto inlineStyle = CssParser::parseInlineStyle(styleAttr);
    if (inlineStyle.hasImageHeight() && inlineStyle.imageHeight.value == 0 && inlineStyle.defined.marginTop &&
        inlineStyle.defined.marginBottom && inlineStyle.marginTop.value == 0 && inlineStyle.marginBottom.value == 0 &&
        inlineStyle.defined.border && !inlineStyle.hasVisibleBorder())
      self->skipTextUntilDepth = std::min(self->skipTextUntilDepth, self->depth);
  }

  const bool rootSize = strcmp(name, "html") == 0 || strcmp(name, "body") == 0;
  if (self->embeddedStyle && (cssStyle.isTextTransparent() || (!rootSize && cssStyle.isFontSizeInvisible())) &&
      self->depth < self->skipTextUntilDepth)
    self->skipTextUntilDepth = self->depth;

  if (self->embeddedStyle && !self->isLightMode() && cssStyle.breakInsideAvoid && !cssStyle.pageBreakBefore &&
      (isHeaderOrBlock(name) || opensBlockSpan) && strcmp(name, "br") != 0)
    self->beginAvoidGroup();

  self->inheritLayoutProperties(cssStyle, isHeaderOrBlock(name) || opensBlockSpan || strcmp(name, "caption") == 0);
  if (self->lowMemoryAbort) return;
  self->prepareGeneratedPseudo(name, classAttr, idAttr);
  self->prepareBackgroundPicture(name, cssStyle);
  self->noteContainerWidth(name, cssStyle);
  self->noteFloatStyle(cssStyle);

  if (strcmp(localTagName(name), "svg") == 0) {
    self->svgUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  if (rawId &&
      !(self->isPreviewBuild() && self->previewAnchorFound && strcmp(rawId, self->previewAnchor.c_str()) == 0)) {
    const char* attrValue = rawId;
    // Defer both anchor recording and TOC page breaks until startNewTextBlock,
    // after the previous block is flushed to pages via makePages().
    const char* idValue = attrValue;
    const bool isTocAnchor =
        std::find(self->tocAnchors.begin(), self->tocAnchors.end(), idValue) != self->tocAnchors.end();
    if (!isTocAnchor && self->activeParser &&
        self->anchorData.size() + self->sourceAnchors_.size() < MAX_ANCHORS_PER_CHAPTER) {
      if (idAttr.size() <= 255) {
        if (!MemoryBudget::canAllocateInternal(idAttr.size() + 512, 32 * 1024, 4096)) {
          self->lowMemoryAbort = true;
          return;
        }
        self->sourceAnchors_.push_back({std::string(idAttr), self->activeParser->sourceByteOffset(), UINT32_MAX});
      }
    } else if (isTocAnchor ||
               (!isNonNavigableInlineElement(name) && self->anchorData.size() < MAX_ANCHORS_PER_CHAPTER)) {
      // Flush displaced block anchors before overwriting. Keep dense inline <a id>
      // runs coalesced so converter-generated anchors do not churn heap in link-heavy chapters.
      const bool previousAnchorShouldBeRecorded = !self->pendingAnchorFromInlineA;
      if (previousAnchorShouldBeRecorded && !self->pendingAnchorId.empty()) {
        self->flushPendingAnchor();
      }
      self->pendingAnchorId = idValue;
      self->pendingAnchorFromInlineA = !isTocAnchor && strcmp(name, "a") == 0;

      // A chapter's TOC target is sometimes an empty inline <a id> tucked *inside* an
      // already-open heading, ahead of the chapter-number/ornament/title runs
      // (e.g. <h1><a id="rsecN"/>Chapter N<img/>Title</h1>). Resolving it lazily at the
      // next block flush defers the chapter's page break until after the "Chapter N" run
      // is already on the page: the break then lands mid-heading, orphaning the number and
      // pushing the ornament + title onto the next page. While the heading opener is still
      // active and has produced no content yet, resolve it now so any needed break lands at
      // the heading's start and the whole opener stays together.
      if (isTocAnchor && self->headingOpenerActive && self->partWordBufferIndex == 0 && self->currentTextBlock &&
          self->currentTextBlock->isEmpty()) {
        self->flushPendingAnchor();
      }
    }
  }

  const char* roleAttr = getAttribute(atts, "role");
  const char* epubTypeAttr = getAttribute(atts, "epub:type");
  const bool isPublisherPageBreak =
      attributeContainsToken(roleAttr, "doc-pagebreak") || attributeContainsToken(epubTypeAttr, "pagebreak");
  if (isPublisherPageBreak) {
    if (self->honorsPublisherDecorations()) {
      const char* markerLabel = getAttribute(atts, "title");
      if (!markerLabel || markerLabel[0] == '\0') {
        markerLabel = getAttribute(atts, "aria-label");
      }
      self->addPendingPublisherPageMarker(markerLabel);
    }
    self->skipCurrentElement();
    return;
  }

  // Special handling for tables/cells: stream simple table rows into page fragments,
  // with a clean flat-paragraph fallback for anything more complex.
  if (self->flattensTables()) {
    if (strcmp(name, "table") == 0) {
      if (self->tableDepth > 0) {
        self->tableDepth += 1;
        return;
      }

      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      self->tableDepth += 1;
      self->tableRowIndex = 0;
      self->tableColIndex = 0;
      self->depth += 1;
      return;
    }

    if (self->tableDepth == 1 && strcmp(name, "tr") == 0) {
      self->tableRowIndex += 1;
      self->tableColIndex = 0;
      self->depth += 1;
      return;
    }

    if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      self->tableColIndex += 1;

      auto tableCellBlockStyle = BlockStyle();
      tableCellBlockStyle.textAlignDefined = true;
      const auto align = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                             ? CssTextAlign::Justify
                             : static_cast<CssTextAlign>(self->paragraphAlignment);
      tableCellBlockStyle.alignment = align;
      self->startNewTextBlock(tableCellBlockStyle);

      char headerText[32];
      snprintf(headerText, sizeof(headerText), "Tab Row %d, Cell %d:", self->tableRowIndex, self->tableColIndex);
      StyleStackEntry headerStyle;
      headerStyle.depth = self->depth;
      headerStyle.hasBold = true;
      headerStyle.bold = false;
      headerStyle.hasItalic = true;
      headerStyle.italic = true;
      headerStyle.hasUnderline = true;
      headerStyle.underline = false;
      bool pushedHeaderStyle = false;
      if (self->inlineStyleCount_ < MAX_INLINE_STYLE_DEPTH) {
        self->inlineStyleBuf_[self->inlineStyleCount_++] = headerStyle;
        pushedHeaderStyle = true;
      } else {
        LOG_ERR("EHP", "inline style stack overflow (table cell label)");
      }
      self->updateEffectiveInlineStyle();
      self->syntheticCharacterData = true;
      self->characterData(userData, headerText, static_cast<int>(strlen(headerText)));
      self->syntheticCharacterData = false;
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      self->nextWordContinues = false;
      if (pushedHeaderStyle) {
        self->inlineStyleCount_--;
        self->updateEffectiveInlineStyle();
      }

      self->depth += 1;
      return;
    }

    if (self->tableDepth == 1 && strcmp(name, "hr") == 0) {
      self->depth += 1;
      return;
    }
  }

  if (strcmp(name, "table") == 0) {
    // skip nested tables
    if (self->tableDepth > 0) {
      if (self->currentTableBuffer) {
        self->currentTableBuffer->unsupported = true;
      }
      if (self->currentCompactTable) {
        self->compactTableUnsupported = true;
        self->currentCompactTable->markUnsupported();
      }
      self->tableDepth += 1;
      return;
    }

    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    const float emSize = self->renderer.getLineHeight(self->fontId) * self->lineCompression;
    auto tableBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Left, self->viewportWidth);

    const auto heap = MemoryBudget::snapshot();
    const bool useCompact = !MemoryBudget::hasHeap(heap, MIN_FREE_HEAP_FOR_RICH_TABLE, MIN_MAX_ALLOC_FOR_RICH_TABLE);
    if (useCompact) {
      // Finish the preceding paragraph before allocating compact-table state.
      // On C3 this releases its layout buffers before the table's row buffers
      // need the same constrained heap.
      if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
        self->makePages();
        if (self->lowMemoryAbort) {
          return;
        }
      }
      self->currentTextBlock.reset();
      const uint16_t lineHeight =
          static_cast<uint16_t>(self->renderer.getLineHeight(self->fontId) * self->lineCompression);
      self->currentCompactTable = makeUniqueNoThrow<CompactTableLayout>(
          self->renderer, self->fontId, self->viewportWidth, self->viewportHeight, lineHeight, TABLE_CELL_PADDING,
          tableBlockStyle, self->characterSpacing);
      if (!self->currentCompactTable || !self->currentCompactTable->valid()) {
        LOG_ERR("EHP", "Failed to allocate compact table layout (free=%u, maxAlloc=%u)", heap.freeHeap,
                heap.maxAllocHeap);
        self->lowMemoryAbort = true;
        return;
      }
      self->compactTableFlattened = false;
      self->compactTableUnsupported = false;
      self->compactTableTopSpacingApplied = false;
      self->compactFragmentRows.clear();
      self->compactFragmentRows.reserve(PageTableFragment::MAX_SERIALIZED_ROWS);
      self->compactFragmentFootnotes.clear();
      self->compactFragmentFootnotes.reserve(Page::MAX_FOOTNOTES_PER_PAGE);
      self->compactFragmentHeight = 1;
      self->compactFragmentColumnCount = 0;
      LOG_DBG("EHP", "Compact table layout selected (free=%u, maxAlloc=%u)", heap.freeHeap, heap.maxAllocHeap);
    } else {
      self->currentTableBuffer = makeUniqueNoThrow<BufferedTable>();
      if (!self->currentTableBuffer) {
        LOG_ERR("EHP", "Failed to buffer rich table (free=%u, maxAlloc=%u)", heap.freeHeap, heap.maxAllocHeap);
        self->lowMemoryAbort = true;
        return;
      }
      self->currentTableBuffer->blockStyle = tableBlockStyle;
      self->currentTableBuffer->streaming = true;
      LOG_DBG("EHP", "Rich table layout selected (free=%u, maxAlloc=%u)", heap.freeHeap, heap.maxAllocHeap);
    }
    self->tableDepth += 1;
    self->tableRowIndex = 0;
    self->tableColIndex = 0;
    self->pushCssAncestor(self->depth, name, classAttr, idAttr);
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && strcmp(name, "tr") == 0) {
    self->tableRowIndex += 1;
    self->tableColIndex = 0;
    if (self->currentCompactTable) {
      if (!self->currentCompactTable->beginRow()) {
        LOG_ERR("EHP", "Failed to begin compact table row");
        self->lowMemoryAbort = true;
        return;
      }
    } else if (self->currentTableBuffer) {
      self->currentTableBuffer->rows.push_back({});
    }
    self->pushCssAncestor(self->depth, name, classAttr, idAttr);
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->tableColIndex += 1;

    if (self->currentTableBuffer && self->currentTableBuffer->rows.empty()) {
      self->currentTableBuffer->rows.push_back({});
    }
    const char* colspan = getAttribute(atts, "colspan");
    const char* rowspan = getAttribute(atts, "rowspan");
    uint8_t parsedColSpan = 1;
    if (colspan && colspan[0] != '\0') {
      char* endPtr = nullptr;
      const long parsedValue = std::strtol(colspan, &endPtr, 10);
      if (endPtr == colspan || *endPtr != '\0' || parsedValue <= 0 || parsedValue > UINT8_MAX) {
        if (self->currentTableBuffer) {
          self->currentTableBuffer->unsupported = true;
        }
        if (self->currentCompactTable) {
          self->compactTableUnsupported = true;
          self->currentCompactTable->markUnsupported();
        }
      } else {
        parsedColSpan = static_cast<uint8_t>(parsedValue);
      }
    }
    if (self->currentTableBuffer && rowspan && strcmp(rowspan, "1") != 0) {
      self->currentTableBuffer->unsupported = true;
    }
    if (self->currentCompactTable && rowspan && strcmp(rowspan, "1") != 0) {
      self->compactTableUnsupported = true;
      self->currentCompactTable->markUnsupported();
    }
    self->currentTableCellColSpan = parsedColSpan;
    self->currentTableCellVisibleOffset = self->visibleTextOffset;
    self->currentTableCellImage.reset();
    self->currentTableCellImageSource = UINT32_MAX;

    auto tableCellBlockStyle = BlockStyle();
    tableCellBlockStyle.textAlignDefined = true;
    // Default table cells to left alignment so narrow columns don't inherit paragraph
    // justification or other reader-wide alignment settings that damage readability.
    tableCellBlockStyle.alignment = cssStyle.hasTextAlign() ? cssStyle.textAlign : CssTextAlign::Left;
    // Cell paragraphs are transparent wrappers. Mark the indent explicitly so the
    // generic three-space paragraph fallback does not indent every table cell.
    tableCellBlockStyle.textIndentDefined = true;
    tableCellBlockStyle.textIndent = 0;
    if (cssStyle.hasDirection()) {
      tableCellBlockStyle.isRtl = cssStyle.direction == CssTextDirection::Rtl;
      tableCellBlockStyle.directionDefined = true;
    }
    self->currentTableCellIsHeader = strcmp(name, "th") == 0;
    {
      StyleStackEntry cellStyle;
      cellStyle.depth = self->depth;
      if (self->currentTableCellIsHeader) {
        cellStyle.hasBold = true;
        cellStyle.bold = true;
      }
      if (cssStyle.hasBackgroundBlack()) {
        cellStyle.hasBackgroundBlack = true;
        cellStyle.backgroundBlack = cssStyle.backgroundBlack;
      }
      ChapterHtmlSlimParser::applyDirectionToEntry(cellStyle, cssStyle);
      cellStyle.setsParagraphDirection = true;
      self->applySmallCapsToEntry(cellStyle, cssStyle);
      self->applyScriptSizing(cellStyle, cssStyle);
      if (self->inlineStyleCount_ < MAX_INLINE_STYLE_DEPTH) {
        self->inlineStyleBuf_[self->inlineStyleCount_++] = cellStyle;
      } else {
        LOG_ERR("EHP", "inline style stack overflow (table cell)");
      }
      self->updateEffectiveInlineStyle();
    }
    if (self->currentCompactTable) {
      if (!self->currentCompactTable->beginCell(self->currentTableCellIsHeader, parsedColSpan,
                                                self->currentTableCellVisibleOffset, tableCellBlockStyle)) {
        // Too many cells for the compact row model: flatten instead of aborting.
        LOG_DBG("EHP", "Compact table cell capacity exceeded; flattening table");
        self->compactTableUnsupported = true;
        self->currentCompactTable->markUnsupported();
      }
      self->currentTextBlock.reset();
      self->currentTextRunBytes = 0;
      self->pendingFootnotes.clear();
    } else {
      self->startNewTextBlock(tableCellBlockStyle);
    }

    self->pushCssAncestor(self->depth, name, classAttr, idAttr);
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && strcmp(name, "caption") != 0 &&
      (matches(name, HEADER_TAGS, std::size(HEADER_TAGS)) || matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS)))) {
    // Treat block/header tags inside a table cell as transparent wrappers around the
    // cell's text content instead of forcing the whole table back to paragraph mode.
    // This covers common EPUB patterns like <td><p>...</p></td> and
    // <td><h4><em>...</em></h4></td> while still keeping one buffered cell.
    if (strcmp(name, "br") == 0 && self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = false;
    }
    if (strcmp(name, "br") != 0) {
      self->pushBlockFontStyle(cssStyle);
      self->updateEffectiveInlineStyle();
    }
    self->pushCssAncestor(self->depth, name, classAttr, idAttr);
    self->depth += 1;
    return;
  }

  const bool tableMathImage =
      self->tableDepth == 1 && strcmp(localTagName(name), "math") == 0 && getAttribute(atts, "altimg");
  if (self->tableDepth == 1 && (matches(localTagName(name), IMAGE_TAGS, std::size(IMAGE_TAGS)) || tableMathImage)) {
    const char* source = getAttribute(atts, tableMathImage ? "altimg" : "src");
    if (!source) source = getAttribute(atts, "href");
    if (!source) source = getAttribute(atts, "xlink:href");
    const bool occupied =
        self->currentCompactTable ? self->currentCompactTable->hasCellImage() : bool(self->currentTableCellImage);
    const bool active =
        self->currentCompactTable ? self->currentCompactTable->hasActiveCell() : bool(self->currentTextBlock);
    bool hiddenImage = false;
    if (atts)
      for (int i = 0; atts[i]; i += 2)
        if (strncmp(atts[i], "data-AmznRemoved-M8", 19) == 0) hiddenImage = true;
    if (self->cssParser) {
      auto imageStyle = self->resolveElementStyle("img", classAttr, idAttr);
      if (!styleAttr.empty()) imageStyle.applyOver(self->resolveInlineStyle(styleAttr));
      hiddenImage = hiddenImage || (imageStyle.hasDisplay() && imageStyle.display == CssDisplay::None);
    }
    bool accepted = false;
    if (active && !occupied && !hiddenImage) {
      auto image = self->admitTableImage(source);
      if (image) {
        const uint32_t point = self->activeParser ? self->activeParser->sourceByteOffset() : UINT32_MAX;
        accepted = self->currentCompactTable ? self->currentCompactTable->setCellImage(std::move(image), point) : true;
        if (!self->currentCompactTable) {
          self->currentTableCellImage = std::move(image);
          self->currentTableCellImageSource = point;
        }
        if (accepted) self->bindSourceAnchors(point);
      }
    }
    if (tableMathImage) {
      if (accepted)
        self->skipDescendantsOfCurrentElement();
      else
        self->depth += 1;
      return;
    }
    const char* altAttr = getAttribute(atts, "alt");
    if (!accepted && !hiddenImage && self->imageRendering != 2 && altAttr && altAttr[0] != '\0') {
      self->characterData(userData, altAttr, strlen(altAttr));
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      self->nextWordContinues = false;
    }
    self->skipCurrentElement();
    return;
  }

  if (self->tableDepth == 1 && strcmp(name, "hr") == 0) {
    if (self->currentTableBuffer) {
      self->currentTableBuffer->unsupported = true;
    }
    if (self->currentCompactTable) {
      self->compactTableUnsupported = true;
      self->currentCompactTable->markUnsupported();
    }
    self->pushCssAncestor(self->depth, name, classAttr, idAttr);
    self->depth += 1;
    return;
  }

  const bool mathAltImage = strcmp(localTagName(name), "math") == 0 && self->imageRendering == 0 &&
                            self->tableDepth == 0 && getAttribute(atts, "altimg") && *getAttribute(atts, "altimg");
  if (matches(localTagName(name), IMAGE_TAGS, std::size(IMAGE_TAGS)) || mathAltImage) {
    std::string src;
    std::string alt;
    const auto rejectImage = [&]() {
      // A missing picture must leave the MathML text available to the reader.
      if (mathAltImage)
        self->depth += 1;
      else
        self->skipCurrentElement();
    };
    if (atts != nullptr) {
      bool amznM8Removed = false;
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "src") == 0 || (mathAltImage && strcmp(atts[i], "altimg") == 0)) {
          src = atts[i + 1];
        } else if (src.empty() && (strcmp(atts[i], "href") == 0 || strcmp(atts[i], "xlink:href") == 0)) {
          src = atts[i + 1];
        } else if (strcmp(atts[i], "alt") == 0 || (mathAltImage && strcmp(atts[i], "alttext") == 0)) {
          alt = atts[i + 1];
        } else if (strncmp(atts[i], "data-AmznRemoved-M8", 19) == 0) {
          amznM8Removed = true;
        }
      }
      // Skip low-res Kindle fallback images (not intended for modern readers)
      if (amznM8Removed) {
        LOG_DBG("EHP", "Skipping Kindle M8 low-res fallback image");
        rejectImage();
        return;
      }

      const size_t fragmentPos = src.find('#');
      if (fragmentPos != std::string::npos) {
        src.resize(fragmentPos);
      }

      // imageRendering: 0=display, 1=placeholder (alt text only), 2=suppress entirely
      if (self->imageRendering == 2) {
        self->skipCurrentElement();
        return;
      }

      // Skip hidden images and retain explicit block placement in all render modes.
      bool displayBlockImage = false;
      bool displayInlineImage = false;
      if (self->cssParser) {
        CssStyle imgDisplayStyle = self->resolveElementStyle("img", classAttr, idAttr);
        if (!styleAttr.empty()) {
          imgDisplayStyle.applyOver(self->resolveInlineStyle(styleAttr));
        }
        if (imgDisplayStyle.hasDisplay() && imgDisplayStyle.display == CssDisplay::None) {
          rejectImage();
          return;
        }
        displayBlockImage = imgDisplayStyle.hasDisplay() && imgDisplayStyle.display == CssDisplay::Block;
        displayInlineImage = imgDisplayStyle.hasDisplay() && imgDisplayStyle.display == CssDisplay::Inline;
      }

      if (!src.empty() && self->imageRendering != 1) {
        const std::string resolvedPath = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->contentBase + src));

        uint16_t optimizerWidth = 0;
        uint16_t optimizerHeight = 0;
        const bool dimensionsFromOptimizer =
            self->epub->getOptimizerImageDimensions(resolvedPath, optimizerWidth, optimizerHeight);

        {
          const auto releaseHeapBefore = MemoryBudget::snapshot();
          if (!dimensionsFromOptimizer &&
              MemoryBudget::shouldReleaseSdFontCachesForEpubInlineImage(releaseHeapBefore) &&
              self->renderer.releaseSdCardFontForLowMemory(self->fontId, /*preserveAdvanceTable=*/true)) {
            const auto releaseHeapAfter = MemoryBudget::snapshot();
            LOG_DBG("EHP", "Released SD font caches before image extraction: free=%u->%u maxAlloc=%u->%u src=%s",
                    releaseHeapBefore.freeHeap, releaseHeapAfter.freeHeap, releaseHeapBefore.maxAllocHeap,
                    releaseHeapAfter.maxAllocHeap, src.c_str());
          }

          if (self->lowMemoryImageFallback) {
            rejectImage();
            return;
          } else {
            {  // Actual headers/decoder validation determine format, never the extension.
              // Optimizer PXC files render without a decoder. Ordinary images
              // retain the existing guard before any fallback extraction.
              if (dimensionsFromOptimizer) {
                if (!MemoryBudget::hasHeapForOptimizerPxcImage("EHP", src.c_str())) {
                  self->lowMemoryImageFallback = true;
                  rejectImage();
                  return;
                }
              } else if (!MemoryBudget::hasHeapForEpubInlineImage("EHP", src.c_str())) {
                self->lowMemoryImageFallback = true;
                rejectImage();
                return;
              }

              // Create a unique filename for the cached image
              std::string ext;
              size_t extPos = resolvedPath.rfind('.');
              if (extPos != std::string::npos) {
                ext = resolvedPath.substr(extPos);
              }
              std::string cachedImagePath = self->imageBasePath + std::to_string(self->imageCounter++) + ext;

              // Read just enough compressed data to find dimensions. The full
              // image remains inside the EPUB until its page is first rendered.
              ImageDimensions dims = {0, 0};
              if (dimensionsFromOptimizer) {
                dims = {static_cast<int16_t>(optimizerWidth), static_cast<int16_t>(optimizerHeight)};
              }
              std::string sourcePath = dimensionsFromOptimizer ? resolvedPath : std::string{};
              const bool gotDimensions = dimensionsFromOptimizer ||
                                         self->probeImageDimensions(resolvedPath, cachedImagePath, dims, sourcePath);

              if (gotDimensions) {
                int displayWidth = 0;
                int displayHeight = 0;
                const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
                CssStyle imgStyle;
                if (!self->isLightMode()) {
                  imgStyle = self->resolveElementStyle("img", classAttr, idAttr);
                  // Merge inline style (e.g. style="height: 2em") so it overrides stylesheet rules
                  if (!styleAttr.empty()) {
                    imgStyle.applyOver(self->resolveInlineStyle(styleAttr));
                  }
                }
                const auto resolveImagePixels = [&](const CssLength& length, const int basis) {
                  const float pixels = length.toPixels(emSize, basis);
                  return std::isnan(pixels) ? 1 : static_cast<int>(std::clamp(pixels + .5f, 1.0f, 32767.0f));
                };
                const bool hasCssHeight = imgStyle.hasImageHeight();
                const bool hasCssWidth = imgStyle.hasImageWidth();
                int unwrappedWidth = self->viewportWidth;
                if (self->currentTextBlock) {
                  const int inset = self->currentTextBlock->getBlockStyle().totalHorizontalInset();
                  if (inset > 0 && inset < self->viewportWidth) {
                    unwrappedWidth = self->viewportWidth - inset;
                  }
                }

                int containerWidth = unwrappedWidth;
                bool percentWrapper = false;
                if (self->containerWidthCount_) {
                  const auto& wrapper = self->containerWidths_[self->containerWidthCount_ - 1];
                  containerWidth = std::min<int>(containerWidth, wrapper.width);
                  percentWrapper = !wrapper.inlineBlock && wrapper.fixedWidth != wrapper.width;
                  if (wrapper.fixedWidth > 0) unwrappedWidth = std::min<int>(unwrappedWidth, wrapper.fixedWidth);
                }

                if (hasCssHeight && hasCssWidth && dims.width > 0 && dims.height > 0) {
                  // Both CSS height and width set: resolve both, then clamp to viewport preserving requested ratio
                  displayHeight = resolveImagePixels(imgStyle.imageHeight, self->viewportHeight);
                  displayWidth = resolveImagePixels(imgStyle.imageWidth, containerWidth);
                  if (displayHeight < 1) displayHeight = 1;
                  if (displayWidth < 1) displayWidth = 1;
                  const float contain = std::min(float(displayWidth) / dims.width, float(displayHeight) / dims.height);
                  displayWidth = std::max(1, int(dims.width * contain + .5f));
                  displayHeight = std::max(1, int(dims.height * contain + .5f));
                  if (displayWidth > containerWidth || displayHeight > self->viewportHeight) {
                    float scaleX =
                        (displayWidth > containerWidth) ? static_cast<float>(containerWidth) / displayWidth : 1.0f;
                    float scaleY = (displayHeight > self->viewportHeight)
                                       ? static_cast<float>(self->viewportHeight) / displayHeight
                                       : 1.0f;
                    float scale = (scaleX < scaleY) ? scaleX : scaleY;
                    displayWidth = static_cast<int>(displayWidth * scale + 0.5f);
                    displayHeight = static_cast<int>(displayHeight * scale + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                    if (displayHeight < 1) displayHeight = 1;
                  }
                } else if (hasCssHeight && !hasCssWidth && dims.width > 0 && dims.height > 0) {
                  // Use CSS height (resolve % against viewport height) and derive width from aspect ratio
                  displayHeight = resolveImagePixels(imgStyle.imageHeight, self->viewportHeight);
                  if (displayHeight < 1) displayHeight = 1;
                  displayWidth =
                      static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                  if (displayHeight > self->viewportHeight) {
                    displayHeight = self->viewportHeight;
                    // Rescale width to preserve aspect ratio when height is clamped
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                  }
                  if (displayWidth > containerWidth) {
                    displayWidth = containerWidth;
                    // Rescale height to preserve aspect ratio when width is clamped
                    displayHeight =
                        static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                    if (displayHeight < 1) displayHeight = 1;
                  }
                  if (displayWidth < 1) displayWidth = 1;
                } else if (hasCssWidth && !hasCssHeight && dims.width > 0 && dims.height > 0) {
                  // Use CSS width (resolve % against container width) and derive height from aspect ratio
                  displayWidth = resolveImagePixels(imgStyle.imageWidth, containerWidth);
                  if (displayWidth > containerWidth) displayWidth = containerWidth;
                  if (displayWidth < 1) displayWidth = 1;
                  displayHeight =
                      static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                  if (displayHeight > self->viewportHeight) {
                    displayHeight = self->viewportHeight;
                    // Rescale width to preserve aspect ratio when height is clamped
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                  }
                  if (displayHeight < 1) displayHeight = 1;
                } else {
                  // Scale to fit container while preserving aspect ratio
                  int maxWidth = containerWidth;
                  int maxHeight = self->viewportHeight;
                  float scaleX = (dims.width > maxWidth) ? (float)maxWidth / dims.width : 1.0f;
                  float scaleY = (dims.height > maxHeight) ? (float)maxHeight / dims.height : 1.0f;
                  float scale = (scaleX < scaleY) ? scaleX : scaleY;
                  if (scale > 1.0f) scale = 1.0f;

                  displayWidth = (int)(dims.width * scale);
                  displayHeight = (int)(dims.height * scale);
                }

                // Match the reference's diagram readability floor for percentage wrappers.
                // Absolute sizing and inline-block wrappers retain their requested narrow box.
                const bool sizedByContainer =
                    !hasCssHeight && (!hasCssWidth || imgStyle.imageWidth.unit == CssUnit::Percent);
                if (percentWrapper && sizedByContainer && dims.width > 0 && dims.height > 0 &&
                    self->floatScopeCount_ == 0) {
                  int floorWidth =
                      hasCssWidth ? imgStyle.imageWidth.toPixelsInt16(emSize, unwrappedWidth) : unwrappedWidth;
                  floorWidth = std::min({floorWidth, unwrappedWidth, int(dims.width)});
                  int floorHeight = int(int64_t(floorWidth) * dims.height / dims.width);
                  if (floorHeight > self->viewportHeight) {
                    floorHeight = self->viewportHeight;
                    floorWidth = int(int64_t(floorHeight) * dims.width / dims.height);
                  }
                  if (floorWidth > displayWidth) {
                    displayWidth = std::max(1, floorWidth);
                    displayHeight = std::max(1, floorHeight);
                  }
                }
                const auto maximum = [&](const CssLength& length, int basis, int fallback) {
                  if (length.unit == CssUnit::Auto || length.unit == CssUnit::Inherit) return fallback;
                  const int selected =
                      length.unit == CssUnit::Rem
                          ? self->renderer.getFontIdForSize64(
                                self->fontId,
                                uint16_t(std::clamp(std::round(self->renderer.getFontPointSize64(self->fontId) *
                                                               self->rootFontScaleExact_),
                                                    1.0f, 8192.0f)))
                          : self->currentTextFontId();
                  return std::max(1, int(length.toPixelsInt16(self->renderer.getFontAscenderSize(selected), basis)));
                };
                const int maximumBasis =
                    percentWrapper && imgStyle.maxWidth.unit == CssUnit::Percent && imgStyle.maxWidth.value == 100
                        ? unwrappedWidth
                        : containerWidth;
                const int maxWidth =
                    imgStyle.hasMaxWidth() ? maximum(imgStyle.maxWidth, maximumBasis, displayWidth) : displayWidth;
                const int maxHeight = imgStyle.hasMaxHeight()
                                          ? maximum(imgStyle.maxHeight, self->viewportHeight, displayHeight)
                                          : displayHeight;
                if (displayWidth > maxWidth || displayHeight > maxHeight) {
                  const float scale = std::min(float(maxWidth) / displayWidth, float(maxHeight) / displayHeight);
                  displayWidth = std::max(1, int(displayWidth * scale + .5f));
                  displayHeight = std::max(1, int(displayHeight * scale + .5f));
                }

                if ((!dimensionsFromOptimizer &&
                     !MemoryBudget::hasHeapForEpubInlineImage("EHP", cachedImagePath.c_str())) ||
                    (dimensionsFromOptimizer &&
                     !MemoryBudget::hasHeapForOptimizerPxcImage("EHP", cachedImagePath.c_str()))) {
                  self->lowMemoryImageFallback = true;
                  if (sourcePath.empty()) Storage.remove(cachedImagePath.c_str());
                  rejectImage();
                  return;
                }

                if (self->floatScopeCount_ && !self->pendingFloatImage_ && !self->activeFloatBottom_ &&
                    self->tableDepth == 0 && !self->inRuby && displayWidth > 0 && displayHeight > 0 &&
                    displayWidth <= self->viewportWidth / 2 - 4 && displayHeight <= self->viewportHeight) {
                  if (self->partWordBufferIndex) self->flushPartWordBuffer();
                  if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) self->makePages();
                  if (self->lowMemoryAbort) return;
                  self->pendingFloatImage_ = makeUniqueNoThrow<ImageBlock>(
                      std::move(cachedImagePath), std::move(sourcePath), displayWidth, displayHeight);
                  if (!self->pendingFloatImage_) {
                    self->lowMemoryAbort = true;
                    return;
                  }
                  self->pendingFloatIsRight_ = self->floatScopes_[self->floatScopeCount_ - 1].right;
                  self->pendingFloatSource_ = self->activeParser ? self->activeParser->sourceByteOffset() : UINT32_MAX;
                  self->pendingFloatVisible_ = self->visibleTextOffset;
                  self->pendingFloatReference_ = self->referenceTextOffset;
                  self->pendingFloatParagraph_ = self->xpathParagraphIndex;
                  self->pendingFloatListItem_ = self->xpathListItemIndex;
                  self->pushCssAncestor(self->depth, name, classAttr, idAttr);
                  ++self->depth;
                  if (mathAltImage) self->skipDescendantsOfCurrentElement();
                  return;
                }

                // Small images participate in the text line's width and height.
                // Full-width art and explicit block images retain the existing
                // centered, page-breaking placement below.
                const bool inlineImage =
                    self->currentTextBlock && self->tableDepth == 0 && !self->inRuby && displayWidth > 0 &&
                    displayHeight > 0 && displayWidth < containerWidth &&
                    (displayInlineImage || (imgStyle.hasDisplay() && imgStyle.display == CssDisplay::Inline) ||
                     displayHeight <= self->effectiveLineHeight() * 2) &&
                    !displayBlockImage && !(imgStyle.hasDisplay() && imgStyle.display == CssDisplay::Block);
                if (inlineImage) {
                  const bool attachToPrevious = self->partWordBufferIndex > 0 || self->nextWordContinues;
                  if (self->partWordBufferIndex > 0) self->flushPartWordBuffer();
                  auto imageBlock = makeUniqueNoThrow<ImageBlock>(std::move(cachedImagePath), std::move(sourcePath),
                                                                  displayWidth, displayHeight);
                  if (!imageBlock) {
                    LOG_ERR("EHP", "Failed to create inline ImageBlock");
                    self->lowMemoryAbort = true;
                    return;
                  }
                  const uint16_t imageId = self->nextInlineImageId++;
                  self->pendingInlineImages.push_back(
                      {imageId, std::move(imageBlock),
                       self->activeParser ? self->activeParser->sourceByteOffset() : UINT32_MAX});
                  if (self->activeParser) self->bindSourceAnchors(self->activeParser->sourceByteOffset());
                  self->currentTextBlock->addInlineImage(imageId, static_cast<uint16_t>(displayWidth),
                                                         static_cast<uint16_t>(displayHeight), attachToPrevious,
                                                         self->visibleTextOffset, self->referenceTextOffset);
                  self->nextWordContinues = true;
                  // An icon-only run has no characterData callback to trigger
                  // the usual bounded paragraph flush. Seal the current line
                  // after this many tiny images so C3 memory stays bounded.
                  const bool flushImageLine = self->pendingInlineImages.size() >= MAX_PENDING_INLINE_IMAGES;
                  self->flushLongTextRunIfNeeded(flushImageLine, flushImageLine);
                  if (self->lowMemoryAbort) return;
                  self->pushCssAncestor(self->depth, name, classAttr, idAttr);
                  self->depth += 1;
                  if (mathAltImage) self->skipDescendantsOfCurrentElement();
                  return;
                }

                if (!self->attachPendingFloatImage() || self->previewStopRequested) return;

                // Flush any pending text block so it appears before the image
                if (self->partWordBufferIndex > 0) {
                  self->flushPartWordBuffer();
                }
                bool openerNumberPrecededImage = false;
                if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
                  const BlockStyle parentBlockStyle = self->currentTextBlock->getBlockStyle();
                  self->startNewTextBlock(parentBlockStyle);
                  // The block synthesized above copies the heading style. Inside a chapter opener that
                  // style still carries the heading's drop margin (the large gap that pushes "Chapter N"
                  // down the page), which was already spent positioning the number. Remember that so it
                  // is not re-applied a second time as the ornament's top gap.
                  openerNumberPrecededImage = self->headingOpenerActive;
                }

                self->currentPageNextY = std::max(self->currentPageNextY, self->activeFloatBottom_);
                self->activeFloatBottom_ = 0;

                int16_t imageMarginTop = 0;
                int16_t imageMarginBottom = 0;
                if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                  const auto& bs = self->currentTextBlock->getBlockStyle();
                  imageMarginTop = openerNumberPrecededImage ? 0 : bs.topInset();
                  if (self->blockStyleCount_ > 1) {
                    imageMarginBottom = self->blockStyleBuf_[self->blockStyleCount_ - 1].bottomInset();
                  }
                }

                // Keep a chapter number with its first ornament, but let later heading images continue on the
                // next page instead of overflowing the viewport (including the reserved status-bar area).
                const bool keepFirstOpenerImageWithHeading =
                    self->headingOpenerActive && (!self->currentPage || !self->currentPage->hasImages());
                if (!keepFirstOpenerImageWithHeading && self->currentPage && !self->currentPage->elements.empty() &&
                    (self->currentPageNextY + imageMarginTop + displayHeight + imageMarginBottom >
                     self->viewportHeight)) {
                  self->completeCurrentPage();
                  self->completedPageCount++;
                  self->stopPreviewIfPageLimitReached();
                  if (self->previewStopRequested) {
                    return;
                  }
                  if (!self->startNewPage("image page break")) {
                    return;
                  }
                } else if (!self->currentPage) {
                  if (!self->startNewPage("image page")) {
                    return;
                  }
                }

                // A viewport-height image leaves no room for a container top
                // margin, including after the page break above starts a fresh page.
                // Keep the placement inside the renderable viewport.
                if (self->currentPageNextY + imageMarginTop + displayHeight > self->viewportHeight) {
                  const int remainingTopMargin = self->viewportHeight - displayHeight - self->currentPageNextY;
                  imageMarginTop = static_cast<int16_t>(std::max(0, remainingTopMargin));
                }
                self->currentPageNextY += imageMarginTop;
                self->attachPendingPublisherPageMarkers(self->currentPageNextY);

                // Create ImageBlock and add to page
                auto imageBlock = makeUniqueNoThrow<ImageBlock>(std::move(cachedImagePath), std::move(sourcePath),
                                                                displayWidth, displayHeight);
                if (!imageBlock) {
                  LOG_ERR("EHP", "Failed to create ImageBlock");
                  self->lowMemoryAbort = true;
                  return;
                }
                int xPos = (self->viewportWidth - displayWidth) / 2;
                auto pageImage = makeUniqueNoThrow<PageImage>(std::move(imageBlock), xPos, self->currentPageNextY);
                if (!pageImage) {
                  LOG_ERR("EHP", "Failed to create PageImage");
                  self->lowMemoryAbort = true;
                  return;
                }
                self->currentPage->elements.push_back(std::move(pageImage));
                if (self->activeParser) self->currentPage->noteSourcePoint(self->activeParser->sourceByteOffset());
                if (self->activeParser) {
                  const uint32_t point = self->activeParser->sourceByteOffset();
                  self->bindSourceAnchors(point);
                  self->resolveSourceAnchors(point, point + 1);
                }
                self->setCurrentPageVisibleOffset(self->visibleTextOffset);
                self->markCurrentPageFromCurrentElement();
                self->noteContent(self->currentPageNextY, self->currentPageNextY + displayHeight);
                self->currentPageNextY += displayHeight;  // The container spends its bottom inset when it closes.

                if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                  BlockStyle afterImageStyle =
                      self->blockStyleBuf_[self->blockStyleCount_ - 1].withoutTop().withoutBottom();
                  if (self->headingOpenerActive) {
                    // The title run trailing the ornament copies the heading style too and would inherit
                    // the same drop margin, stacking a second heading-sized gap under the image. Replace
                    // it with a modest half-line gap so the title sits just below the ornament instead.
                    afterImageStyle.marginTop = static_cast<int16_t>(self->effectiveLineHeight() / 2);
                    afterImageStyle.paddingTop = 0;
                  }
                  self->currentTextBlock->setBlockStyle(afterImageStyle);
                  self->emptyTextBlockAfterImage = true;
                }

                self->pushCssAncestor(self->depth, name, classAttr, idAttr);
                self->depth += 1;
                if (mathAltImage) self->skipDescendantsOfCurrentElement();
                return;
              } else {
                Storage.remove(cachedImagePath.c_str());
                const uint32_t postFailureFreeHeap = ESP.getFreeHeap();
                const uint32_t postFailureMaxAllocHeap = ESP.getMaxAllocHeap();
                if (!self->lowMemoryImageFallback &&
                    !MemoryBudget::hasHeapForEpubInlineImage("EHP", cachedImagePath.c_str())) {
                  self->lowMemoryImageFallback = true;
                  LOG_ERR("EHP", "Disabling remaining image extraction after failure (%u free, %u max alloc)",
                          postFailureFreeHeap, postFailureMaxAllocHeap);
                }
                LOG_ERR("EHP", "Failed to get image dimensions");
              }
            }
          }
        }
      }

      if (mathAltImage) {
        self->depth += 1;
        return;
      }
      // Fallback to alt text if image processing fails
      if (!alt.empty()) {
        alt = "[Image: " + alt + "]";
        self->startNewTextBlock(self->blockStyleBuf_[self->blockStyleCount_ - 1]
                                    .getCombinedBlockStyle(centeredBlockStyle, BlockStyle::CombineAxis::Horizontal)
                                    .withoutBottom());
        self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
        self->pushCssAncestor(self->depth, name, classAttr, idAttr);
        self->depth += 1;
        self->characterData(userData, alt.c_str(), alt.length());
        self->skipDescendantsOfCurrentElement();
        return;
      }

      // No alt text, skip
      self->skipCurrentElement();
      return;
    }
  }

  // Ruby tag handling
  if (strcmp(name, "ruby") == 0) {
    // <ruby> is an inline element: a base that follows text with no whitespace between them
    // continues the same visual word, exactly like <b>/<i> handling in endElement().
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->inRuby = true;
    self->rubyStartWordIndex = self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0;
    if (self->currentTextBlock) {
      self->currentTextBlock->ensureRubyCapacity();
    }
    self->rubyTextBuffer.clear();
    self->depth += 1;
    return;
  }
  if (strcmp(name, "rt") == 0) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->collectingRubyText = true;
    self->depth += 1;
    return;
  }

  if (matches(name, SKIP_TAGS, std::size(SKIP_TAGS))) {
    // start skip
    self->skipCurrentElement();
    return;
  }

  // Detect internal <a href="..."> links (footnotes, cross-references)
  // Note: <aside epub:type="footnote"> elements are rendered as normal content
  // without special handling. Links pointing to them are collected as footnotes.
  if (strcmp(name, "a") == 0) {
    const char* href = getAttribute(atts, "href");

    bool isInternalLink = isInternalEpubLink(href);

    // Special case: javascript:void(0) links with data attributes
    // Example: <a href="javascript:void(0)"
    // data-xyz="{&quot;name&quot;:&quot;OPS/ch2.xhtml&quot;,&quot;frag&quot;:&quot;id46&quot;}">
    if (href && strncmp(href, "javascript:", 11) == 0) {
      isInternalLink = false;
      // TODO: Parse data-* attributes to extract actual href
    }

    if (isInternalLink) {
      // Flush buffer before style change
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      self->insideFootnoteLink = true;
      self->footnoteLinkDepth = self->depth;
      strncpy(self->currentFootnote.href, href, sizeof(self->currentFootnote.href) - 1);
      self->currentFootnote.href[sizeof(self->currentFootnote.href) - 1] = '\0';
      self->currentFootnote.number[0] = '\0';
      self->currentFootnote.linkId = self->nextFootnoteLinkId;
      self->nextFootnoteLinkId = static_cast<uint8_t>((self->nextFootnoteLinkId % 63) + 1);
      self->currentFootnoteLinkTextLen = 0;

      // Apply underline style to visually indicate the link
      self->underlineUntilDepth = std::min(self->underlineUntilDepth, self->depth);
      StyleStackEntry entry;
      entry.depth = self->depth;
      entry.hasUnderline = true;
      entry.underline = true;
      if (cssStyle.hasBackgroundBlack()) {
        entry.hasBackgroundBlack = true;
        entry.backgroundBlack = cssStyle.backgroundBlack;
      }
      applyDirectionToEntry(entry, cssStyle);
      self->applyInlineFontSize(entry, cssStyle, name);
      self->applySmallCapsToEntry(entry, cssStyle);
      applyVerticalAlignToEntry(entry, cssStyle);
      self->applyScriptSizing(entry, cssStyle);
      if (self->inlineStyleCount_ < MAX_INLINE_STYLE_DEPTH) {
        self->inlineStyleBuf_[self->inlineStyleCount_++] = entry;
      } else {
        LOG_ERR("EHP", "inline style stack overflow (anchor)");
      }
      self->updateEffectiveInlineStyle();

      // Skip CSS resolution — we already handled styling for this <a> tag
      self->pushCssAncestor(self->depth, name, classAttr, idAttr);
      self->depth += 1;
      return;
    }
  }

  BlockStyle fontStyle;
  self->applyBlockFontSize(cssStyle, name, fontStyle);
  const int blockFontId = self->renderer.getFontIdForSize64(self->fontId, fontStyle.fontSize64);
  if (isHeaderOrBlock(name) && cssStyle.hasLetterSpacing()) {
    cssStyle.letterSpacing = CssLength(self->resolveLetterSpacing(cssStyle.letterSpacing, blockFontId) / 16.0f);
  }
  const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(blockFontId));

  const CssTextAlign requestedAlign = static_cast<CssTextAlign>(self->paragraphAlignment);
  auto userAlignmentBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, requestedAlign, self->viewportWidth);
  userAlignmentBlockStyle.fontSize = fontStyle.fontSize;
  userAlignmentBlockStyle.fontSize64 = fontStyle.fontSize64;
  userAlignmentBlockStyle.fontScale = fontStyle.fontScale;
  userAlignmentBlockStyle.cssFontScale = fontStyle.cssFontScale;
  self->applyFirstLineStyle(userAlignmentBlockStyle, name, classAttr, idAttr);
  if ((strcmp(name, "html") == 0 || strcmp(name, "body") == 0) && fontStyle.fontSize) {
    self->blockStyleBuf_[0].fontSize = fontStyle.fontSize;
    self->blockStyleBuf_[0].fontSize64 = fontStyle.fontSize64;
    self->blockStyleBuf_[0].fontScale = fontStyle.fontScale;
    self->blockStyleBuf_[0].cssFontScale = fontStyle.cssFontScale;
    if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
      auto style = self->currentTextBlock->getBlockStyle();
      style.fontSize = fontStyle.fontSize;
      style.fontSize64 = fontStyle.fontSize64;
      style.fontScale = fontStyle.fontScale;
      style.cssFontScale = fontStyle.cssFontScale;
      self->currentTextBlock->setBlockStyle(style);
    }
  }

  if (!self->embeddedStyle || requestedAlign != CssTextAlign::None) {
    userAlignmentBlockStyle.textAlignDefined = true;
    userAlignmentBlockStyle.alignment = requestedAlign == CssTextAlign::None ? CssTextAlign::Justify : requestedAlign;
  }

  // Figures lay out artwork and captions, independently of the reader's prose alignment.
  if (self->embeddedStyle && strcmp(name, "figure") == 0 && cssStyle.hasTextAlign()) {
    userAlignmentBlockStyle.alignment = cssStyle.textAlign;
  }

  if (!self->embeddedStyle || self->isLightMode()) {
    stripPublisherSpacing(userAlignmentBlockStyle);
  }

  // <html> and <body> are transparent to the block layout stack, but CSS
  // text-indent is inherited by descendant paragraphs. Preserve that value on
  // the root style so ordinary child-block merging applies paragraph overrides.
  if ((strcmp(name, "html") == 0 || strcmp(name, "body") == 0) && self->blockStyleCount_ > 0 &&
      userAlignmentBlockStyle.textIndentDefined) {
    auto& rootBlockStyle = self->blockStyleBuf_[0];
    rootBlockStyle.textIndent = userAlignmentBlockStyle.textIndent;
    rootBlockStyle.textIndentDefined = true;
  }

  if ((strcmp(name, "html") == 0 || strcmp(name, "body") == 0) && self->blockStyleCount_ > 0 &&
      userAlignmentBlockStyle.publisherAlignment != CssTextAlign::None) {
    self->blockStyleBuf_[0].publisherAlignment = userAlignmentBlockStyle.publisherAlignment;
  }

  // Force paragraph indent to prevent unreadable walls of text.
  // This applies if the publisher set text-indent: 0, omitted it, or if it was stripped by disabling embedded styles.
  if (self->forceParagraphIndents && strcmp(name, "p") == 0) {
    static constexpr float forcedIndentEm = 1.0f;
    if (userAlignmentBlockStyle.alignment == CssTextAlign::Left ||
        userAlignmentBlockStyle.alignment == CssTextAlign::Justify ||
        userAlignmentBlockStyle.alignment == CssTextAlign::None) {
      if (!userAlignmentBlockStyle.textIndentDefined || userAlignmentBlockStyle.textIndent == 0) {
        userAlignmentBlockStyle.textIndentDefined = true;
        userAlignmentBlockStyle.textIndent = static_cast<int16_t>(emSize * forcedIndentEm);
      }
    }
  }

  if (strcmp(name, "hr") == 0) {
    if (self->isLightMode()) {
      self->skipCurrentElement();
      return;
    }
    auto hrBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Left, self->viewportWidth);
    if (!self->embeddedStyle || self->isLightMode()) {
      stripPublisherSpacing(hrBlockStyle);
    }
    self->emitHorizontalRule(hrBlockStyle, !cssStyle.suppressesHorizontalRule());
    self->pushCssAncestor(self->depth, name, classAttr, idAttr);
    self->depth += 1;
    return;
  }

  const bool cssPageBreakBefore =
      userAlignmentBlockStyle.pageBreakBefore && (matches(name, HEADER_TAGS, std::size(HEADER_TAGS)) ||
                                                  matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS)) || opensBlockSpan);
  if (cssPageBreakBefore && ((self->currentTextBlock && !self->currentTextBlock->isEmpty()) ||
                             (self->currentPage && !self->currentPage->elements.empty()))) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->makePages();
    }
    if (self->lowMemoryAbort) return;
    // The old paragraph is finalized; do not charge its trailing spacing
    // again when startNewTextBlock() creates the block on the next page.
    self->currentTextBlock.reset();
    self->wordsExtractedInBlock = 0;
    if (self->currentPage && !self->currentPage->elements.empty()) {
      self->completeCurrentPage();
      self->completedPageCount++;
      self->stopPreviewIfPageLimitReached();
      if (self->previewStopRequested) {
        return;
      }
      self->currentPageNextY = 0;
    }
  }

  // <br> is a line break, not a font-style container.
  if ((isHeaderOrBlock(name) || opensBlockSpan) && strcmp(name, "br") != 0) {
    // Finish the preceding word before changing block layout or inherited font styles.
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->pushBlockFontStyle(cssStyle);
  }

  if (matches(name, HEADER_TAGS, std::size(HEADER_TAGS))) {
    self->headingDepth = self->depth;
    self->headingOpenerActive = true;
    self->currentCssStyle = cssStyle;
    auto headerBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Center, self->viewportWidth);
    headerBlockStyle.fontSize = fontStyle.fontSize;
    headerBlockStyle.fontSize64 = fontStyle.fontSize64;
    headerBlockStyle.fontScale = fontStyle.fontScale;
    headerBlockStyle.cssFontScale = fontStyle.cssFontScale;
    self->applyFirstLineStyle(headerBlockStyle, name, classAttr, idAttr);
    headerBlockStyle.textAlignDefined = true;
    if (self->embeddedStyle && cssStyle.hasTextAlign()) {
      headerBlockStyle.alignment = cssStyle.textAlign;
    }
    if (!self->embeddedStyle || self->isLightMode()) {
      stripPublisherSpacing(headerBlockStyle);
    }
    self->openBoxScope(headerBlockStyle, cssStyle);
    const auto accumulated = self->blockStyleBuf_[self->blockStyleCount_ - 1].getCombinedBlockStyle(
        headerBlockStyle, BlockStyle::CombineAxis::Horizontal);
    if (self->blockStyleCount_ < MAX_BLOCK_STYLE_DEPTH) {
      self->blockStyleDepths_[self->blockStyleCount_] = self->depth;
      self->blockStyleBuf_[self->blockStyleCount_++] = accumulated;
    } else {
      LOG_ERR("EHP", "block style stack overflow (header)");
    }
    self->startNewTextBlock(accumulated.withoutBottom());
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS)) || strcmp(name, "caption") == 0 || opensBlockSpan) {
    if (self->headingOpenerActive) {
      self->headingOpenerActive = false;
    }
    if (strcmp(name, "br") == 0) {
      if (self->partWordBufferIndex > 0) {
        // flush word preceding <br/> to currentTextBlock before calling startNewTextBlock
        self->flushPartWordBuffer();
      }
      // A <br> after text is a line break: start the next block without the
      // container's vertical margins, which browsers do not re-apply at a
      // line break. A <br> that leaves its block empty (a consecutive or
      // standalone <br>) retains those margins and becomes a scene break.
      // Use the active stack style rather than the current block so styles
      // from a closed element cannot leak into the next paragraph.
      BlockStyle brStyle = self->blockStyleBuf_[self->blockStyleCount_ - 1];
      if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
        brStyle = brStyle.withoutTop().withoutBottom();
      }
      brStyle.fromBrElement = true;
      self->startNewTextBlock(brStyle);
    } else {
      self->currentCssStyle = cssStyle;
      if (opensBlockSpan && !cssStyle.hasTextAlign()) {
        userAlignmentBlockStyle.alignment = self->displayBlockParentStyle_.alignment;
        userAlignmentBlockStyle.textAlignDefined = self->displayBlockParentStyle_.textAlignDefined;
      }
      self->openBoxScope(userAlignmentBlockStyle, cssStyle);
      const auto accumulated = self->blockStyleBuf_[self->blockStyleCount_ - 1].getCombinedBlockStyle(
          userAlignmentBlockStyle, BlockStyle::CombineAxis::Horizontal);
      if (self->blockStyleCount_ < MAX_BLOCK_STYLE_DEPTH) {
        self->blockStyleDepths_[self->blockStyleCount_] = self->depth;
        self->blockStyleBuf_[self->blockStyleCount_++] = accumulated;
      } else {
        LOG_ERR("EHP", "block style stack overflow (block)");
      }
      // Common EPUB shape: <li><p>text</p></li>. Keep the first paragraph in the marker block
      // so the auto bullet does not become its own orphaned paragraph.
      const bool reuseListMarkerBlock = strcmp(name, "p") == 0 && self->pendingListMarkerDepth >= 0 &&
                                        self->depth == self->pendingListMarkerDepth + 1 && self->currentTextBlock &&
                                        self->currentTextBlock->size() == 1;
      if (reuseListMarkerBlock) {
        const auto mergedStyle = self->currentTextBlock->getBlockStyle().getCombinedBlockStyle(
            accumulated.withoutBottom(), BlockStyle::CombineAxis::Vertical);
        self->currentTextBlock->setBlockStyle(mergedStyle);
      } else {
        self->startNewTextBlock(accumulated.withoutBottom());
      }
      self->updateEffectiveInlineStyle();

      if (strcmp(name, "li") == 0) {
        bool markerAdded = false;
        if (self->listContextCount_ > 0 && self->listContexts_[self->listContextCount_ - 1].styleNone) {
          // Marker-free list item.
        } else if (self->listContextCount_ > 0 && self->listContexts_[self->listContextCount_ - 1].ordered) {
          auto& list = self->listContexts_[self->listContextCount_ - 1];
          int32_t itemValue = 0;
          if (parseListValue(getAttribute(atts, "value"), itemValue)) {
            list.nextValue = itemValue;
          }
          char marker[16];
          snprintf(marker, sizeof(marker), "%ld.", static_cast<long>(list.nextValue));
          if (list.nextValue < INT32_MAX) ++list.nextValue;
          self->currentTextBlock->addWord(marker, EpdFontFamily::REGULAR, false, false,
                                          self->honorsPublisherDecorations() && self->effectiveBackgroundBlack, 0,
                                          self->visibleTextOffset, self->referenceTextOffset);
          markerAdded = true;
        } else {
          self->currentTextBlock->addWord("\xe2\x80\xa2", EpdFontFamily::REGULAR, false, false,
                                          self->honorsPublisherDecorations() && self->effectiveBackgroundBlack, 0,
                                          self->visibleTextOffset, self->referenceTextOffset);
          markerAdded = true;
        }
        if (markerAdded) self->pendingListMarkerDepth = self->depth;
      } else if (strcmp(name, "ul") == 0 || strcmp(name, "ol") == 0) {
        if (self->listContextCount_ < self->listContexts_.size()) {
          auto& list = self->listContexts_[self->listContextCount_++];
          list = {};
          list.ordered = strcmp(name, "ol") == 0;
          int32_t startValue = 0;
          if (list.ordered && parseListValue(getAttribute(atts, "start"), startValue)) {
            list.nextValue = startValue;
          }
          list.styleNone = cssStyle.hasListStyleType() && cssStyle.listStyleType == CssListStyleType::None;
          list.depth = self->depth;
        } else {
          LOG_ERR("EHP", "list context stack overflow");
        }
      }
    }
  } else if (matches(name, UNDERLINE_TAGS, std::size(UNDERLINE_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->underlineUntilDepth = std::min(self->underlineUntilDepth, self->depth);
    // Push inline style entry for underline tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasUnderline = true;
    entry.underline = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    if (cssStyle.hasBackgroundBlack()) {
      entry.hasBackgroundBlack = true;
      entry.backgroundBlack = cssStyle.backgroundBlack;
    }
    applyDirectionToEntry(entry, cssStyle);
    self->applyInlineFontSize(entry, cssStyle, name);
    self->applySmallCapsToEntry(entry, cssStyle);
    self->applyScriptSizing(entry, cssStyle);
    if (self->inlineStyleCount_ < MAX_INLINE_STYLE_DEPTH) {
      self->inlineStyleBuf_[self->inlineStyleCount_++] = entry;
    } else {
      LOG_ERR("EHP", "inline style stack overflow (underline)");
    }
    self->updateEffectiveInlineStyle();
  } else if (matches(name, STRIKETHROUGH_TAGS, std::size(STRIKETHROUGH_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->strikethroughUntilDepth = std::min(self->strikethroughUntilDepth, self->depth);
    // Push inline style entry for strikethrough tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasStrikethrough = true;
    entry.strikethrough = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    if (cssStyle.hasBackgroundBlack()) {
      entry.hasBackgroundBlack = true;
      entry.backgroundBlack = cssStyle.backgroundBlack;
    }
    applyDirectionToEntry(entry, cssStyle);
    self->applyInlineFontSize(entry, cssStyle, name);
    self->applySmallCapsToEntry(entry, cssStyle);
    self->applyScriptSizing(entry, cssStyle);
    if (self->inlineStyleCount_ < MAX_INLINE_STYLE_DEPTH) {
      self->inlineStyleBuf_[self->inlineStyleCount_++] = entry;
    } else {
      LOG_ERR("EHP", "inline style stack overflow (strikethrough)");
    }
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BOLD_TAGS, std::size(BOLD_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    // Push inline style entry for bold tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasBold = true;
    entry.bold = true;
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    if (cssStyle.hasTextDecoration()) {
      entry.hasUnderline = true;
      entry.underline = (cssStyle.textDecoration & CssTextDecoration::Underline) != CssTextDecoration::None;
      entry.hasStrikethrough = true;
      entry.strikethrough = (cssStyle.textDecoration & CssTextDecoration::LineThrough) != CssTextDecoration::None;
    }
    if (cssStyle.hasBackgroundBlack()) {
      entry.hasBackgroundBlack = true;
      entry.backgroundBlack = cssStyle.backgroundBlack;
    }
    applyDirectionToEntry(entry, cssStyle);
    self->applyInlineFontSize(entry, cssStyle, name);
    self->applySmallCapsToEntry(entry, cssStyle);
    self->applyScriptSizing(entry, cssStyle);
    if (self->inlineStyleCount_ < MAX_INLINE_STYLE_DEPTH) {
      self->inlineStyleBuf_[self->inlineStyleCount_++] = entry;
    } else {
      LOG_ERR("EHP", "inline style stack overflow (bold)");
    }
    self->updateEffectiveInlineStyle();
  } else if (matches(name, ITALIC_TAGS, std::size(ITALIC_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
    // Push inline style entry for italic tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasItalic = true;
    entry.italic = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    if (cssStyle.hasTextDecoration()) {
      entry.hasUnderline = true;
      entry.underline = (cssStyle.textDecoration & CssTextDecoration::Underline) != CssTextDecoration::None;
      entry.hasStrikethrough = true;
      entry.strikethrough = (cssStyle.textDecoration & CssTextDecoration::LineThrough) != CssTextDecoration::None;
    }
    if (cssStyle.hasBackgroundBlack()) {
      entry.hasBackgroundBlack = true;
      entry.backgroundBlack = cssStyle.backgroundBlack;
    }
    applyDirectionToEntry(entry, cssStyle);
    self->applyInlineFontSize(entry, cssStyle, name);
    self->applySmallCapsToEntry(entry, cssStyle);
    self->applyScriptSizing(entry, cssStyle);
    if (self->inlineStyleCount_ < MAX_INLINE_STYLE_DEPTH) {
      self->inlineStyleBuf_[self->inlineStyleCount_++] = entry;
    } else {
      LOG_ERR("EHP", "inline style stack overflow (italic)");
    }
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "sup") == 0 || strcmp(name, "sub") == 0) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    StyleStackEntry entry;
    entry.depth = self->depth;
    if (strcmp(name, "sup") == 0) {
      entry.hasSup = true;
      entry.sup = true;
    } else {
      entry.hasSub = true;
      entry.sub = true;
    }
    ChapterHtmlSlimParser::applyDirectionToEntry(entry, cssStyle);
    self->applySmallCapsToEntry(entry, cssStyle);
    self->applyScriptSizing(entry, cssStyle);
    if (self->inlineStyleCount_ < MAX_INLINE_STYLE_DEPTH) {
      self->inlineStyleBuf_[self->inlineStyleCount_++] = entry;
    } else {
      LOG_ERR("EHP", "inline style stack overflow (sup/sub)");
    }
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "span") == 0 || !isHeaderOrBlock(name)) {
    // Handle span and other inline elements for CSS styling
    if (cssStyle.hasFontWeight() || cssStyle.hasFontStyle() || cssStyle.hasTextDecoration() ||
        cssStyle.hasBackgroundBlack() || cssStyle.hasVerticalAlign() || cssStyle.hasDirection() ||
        cssStyle.hasFontVariantCaps() || cssStyle.hasWhiteSpace() || cssStyle.hasFontSize() ||
        cssStyle.hasTextTransform() || cssStyle.hasLetterSpacing() ||
        (cssStyle.defined.fontFamily && self->publisherFamily(cssStyle.fontFamily) != self->effectiveFontFamily)) {
      // Flush buffer before style change so preceding text gets current style
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      StyleStackEntry entry;
      entry.depth = self->depth;  // Track depth for matching pop
      if (cssStyle.hasFontWeight()) {
        entry.hasBold = true;
        entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
      }
      if (cssStyle.hasFontStyle()) {
        entry.hasItalic = true;
        entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
      }
      if (cssStyle.hasTextDecoration()) {
        entry.hasUnderline = true;
        entry.underline = (cssStyle.textDecoration & CssTextDecoration::Underline) != CssTextDecoration::None;
        entry.hasStrikethrough = true;
        entry.strikethrough = (cssStyle.textDecoration & CssTextDecoration::LineThrough) != CssTextDecoration::None;
      }
      if (cssStyle.hasBackgroundBlack()) {
        entry.hasBackgroundBlack = true;
        entry.backgroundBlack = cssStyle.backgroundBlack;
      }
      applyDirectionToEntry(entry, cssStyle);
      entry.setsParagraphDirection = strcmp(name, "html") == 0 || strcmp(name, "body") == 0;
      self->applyInlineFontSize(entry, cssStyle, name);
      self->applySmallCapsToEntry(entry, cssStyle);
      applyVerticalAlignToEntry(entry, cssStyle);
      self->applyScriptSizing(entry, cssStyle);
      if (self->inlineStyleCount_ < MAX_INLINE_STYLE_DEPTH) {
        self->inlineStyleBuf_[self->inlineStyleCount_++] = entry;
      } else {
        LOG_ERR("EHP", "inline style stack overflow (span/inline)");
      }
      self->updateEffectiveInlineStyle();
    }
  }

  if (strcmp(name, "span") == 0 && cssStyle.hasCssFloat() && cssStyle.cssFloat == CssFloat::Right)
    self->startTextFloat(cssStyle);

  if (!isHeaderOrBlock(name) && !isTableStructuralTag(name) && !self->currentCompactTable) {
    self->queueInlinePadding(cssStyle);
  }

  if (self->embeddedStyle && self->honorsPublisherDecorations() && self->tableDepth == 0 && self->currentTextBlock &&
      self->currentTextBlock->isEmpty() && self->partWordBufferIndex == 0 && self->wordsExtractedInBlock == 0 &&
      self->dropCap.length == 0) {
    if (isHeaderOrBlock(name) && strcmp(name, "br") != 0 && self->cssParser) {
      const auto first =
          self->cssParser->resolveStyle(name, classAttr, self->ancestorStack_, idAttr, true, self->documentCss_.get());
      const auto lines = dropCapLines(first);
      if (lines) {
        self->dropCap.lines = lines;
        self->dropCap.firstLetterPending = true;
        self->dropCap.transform = first.hasTextTransform() && first.textTransform != CssTextTransform::Inherit
                                      ? first.textTransform
                                      : self->effectiveTextTransform;
        self->dropCap.bold = first.hasFontWeight() && first.fontWeight == CssFontWeight::Bold;
        const uint32_t family = first.defined.fontFamily ? first.fontFamily : self->effectiveFontFamily;
        self->dropCap.fontFamily =
            self->publisherFonts == PublisherFonts::Off ||
                    (self->publisherFonts == PublisherFonts::Accents && family == self->bodyFontFamily)
                ? 0
                : family;
      }
    } else if (strcmp(name, "span") == 0 && (cssStyle.floatLeft || cssStyle.initialLetter >= 2)) {
      const auto lines = dropCapLines(cssStyle);
      if (lines) {
        self->dropCap.lines = lines;
        self->dropCap.spanDepth = self->depth;
        self->dropCap.transform = self->effectiveTextTransform;
        self->dropCap.bold = self->effectiveBold;
        self->dropCap.fontFamily = self->effectiveFontFamily;
      }
    }
  }

  // Unprocessed tag, just increasing depth and continue forward
  self->pushCssAncestor(self->depth, name, classAttr, idAttr);
  self->depth += 1;
}

void XMLCALL ChapterHtmlSlimParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  if (self->isScanningForPreviewAnchor() || self->previewStopRequested) {
    return;
  }
  if (self->shouldAbortForLowMemory("character data")) {
    return;
  }

  const bool countReferenceCharacters = self->trackReferenceCharacters && !self->syntheticCharacterData &&
                                        self->referenceExcludedUntilDepth >= self->depth;
  const bool contentIsSkipped = self->tableDepth > 1 || self->skipUntilDepth < self->depth ||
                                self->svgUntilDepth < self->depth || self->skipTextUntilDepth < self->depth;
  if ((contentIsSkipped || self->collectingRubyText) && countReferenceCharacters) {
    self->consumeReferenceCharacters(s, len);
  }
  if (contentIsSkipped) {
    for (size_t i = 0; i < self->boxScopeCount; ++i) self->boxScopes[i].hiddenContent = true;
    return;
  }
  self->emitPendingGeneratedBefore();
  if (self->lowMemoryAbort) return;

  // Keep the source coordinate independent of wrapping, fonts, and orientation.
  // `head`/`rp` are skipped above; synthetic table labels and ruby annotations do
  // not represent the document body position.
  const uint32_t callbackVisibleOffset = self->visibleTextOffset;
  const bool countVisibleOffsets = !self->syntheticCharacterData && !self->collectingRubyText;
  if (countVisibleOffsets && self->activeParser && self->sourceTarget != UINT32_MAX &&
      !self->sourceTargetVisibleOffset) {
    uint32_t prefix = 0;
    for (int i = 0; i < len;) {
      const uint32_t origin = self->characterOrigin(i);
      if (origin != UINT32_MAX && origin >= self->sourceTarget) {
        self->sourceTargetVisibleOffset =
            callbackVisibleOffset + prefix + (origin == self->sourceTarget ? self->sourceTargetCharacter : 0);
        break;
      }
      const auto* begin = reinterpret_cast<const unsigned char*>(s + i);
      const auto* next = begin;
      utf8NextCodepoint(&next);
      i += static_cast<int>(next - begin);
      ++prefix;
    }
  }
  if (countVisibleOffsets) {
    const auto* ptr = reinterpret_cast<const unsigned char*>(s);
    const auto* const end = ptr + len;
    while (ptr < end) {
      utf8NextCodepoint(&ptr);
      self->visibleTextOffset++;
    }
  }

  // Collect ruby text instead of normal word processing
  if (self->collectingRubyText) {
    self->rubyTextBuffer.append(s, len);
    return;
  }

  // Collect footnote link display text (for the number label)
  // Skip whitespace and brackets to normalize noterefs like "[1]" → "1"
  if (self->insideFootnoteLink) {
    int start = 0;
    int end = len - 1;

    // Example input and output texts:
    // "     [  12  ]   " => "12"
    // "   turn to 256  " => "turn to 256"

    // Ignore leading whitespaces and left square brackets
    while (start < len && (isWhitespace(s[start]) || (s[start] == '['))) {
      ++start;
    }

    // Ignore trailing whitespaces and right square brackets
    while (end >= start && (isWhitespace(s[end]) || (s[end] == ']'))) {
      --end;
    }

    // Extract footnote link text
    for (int i = start; (self->currentFootnoteLinkTextLen < sizeof(self->currentFootnote.number) - 1) && (i <= end);
         ++i) {
      self->currentFootnote.number[self->currentFootnoteLinkTextLen++] = s[i];
    }
    self->currentFootnote.number[self->currentFootnoteLinkTextLen] = '\0';
  }

  uint32_t codepointOffset = callbackVisibleOffset;
  uint32_t codepointReferenceOffset = self->referenceTextOffset;
  for (int i = 0; i < len; i++) {
    if (self->lowMemoryAbort || self->previewStopRequested) return;
    const bool startsCodepoint = (static_cast<uint8_t>(s[i]) & 0xC0) != 0x80;
    if (startsCodepoint && countReferenceCharacters && !self->collectingRubyText) {
      const auto* codepointPtr = reinterpret_cast<const unsigned char*>(s + i);
      const uint32_t codepoint = utf8NextCodepoint(&codepointPtr);
      codepointReferenceOffset = self->consumeReferenceCodepoint(codepoint);
    }
    if (startsCodepoint && (self->dropCap.firstLetterPending || self->dropCap.spanDepth >= 0) && !isWhitespace(s[i])) {
      const auto* begin = reinterpret_cast<const unsigned char*>(s + i);
      const auto* end = begin;
      utf8NextCodepoint(&end);
      const int bytes = static_cast<int>(end - begin);
      if (self->dropCap.length == 0) {
        self->dropCap.visibleOffset = codepointOffset;
        self->dropCap.referenceOffset = codepointReferenceOffset;
      }
      if (self->captureDropCapCodepoint(s + i, bytes, i)) {
        i += bytes - 1;
        if (countVisibleOffsets) ++codepointOffset;
        continue;
      }
    }
    // A one-letter word must remain its own selectable token, not become a
    // prefix on the next word. Fall back to ordinary text at that boundary.
    if (isWhitespace(s[i]) && self->dropCap.length && self->partWordBufferIndex == 0 && self->currentTextBlock &&
        self->currentTextBlock->isEmpty()) {
      self->cancelDropCapToWord();
    }
    const bool preservedNewline =
        self->effectivePreserveWhitespace || self->effectiveWhiteSpace == CssWhiteSpace::PreLine;
    if (preservedNewline) {
      if (s[i] == '\n' && self->previousPreCr_) {
        self->previousPreCr_ = false;
        if (startsCodepoint && countVisibleOffsets) ++codepointOffset;
        continue;
      }
      self->previousPreCr_ = s[i] == '\r';
      if (self->dropPreInitialNewline_) {
        self->dropPreInitialNewline_ = false;
        if (s[i] == '\n' || s[i] == '\r') {
          if (startsCodepoint && countVisibleOffsets) ++codepointOffset;
          continue;
        }
      }
    }
    if ((self->effectivePreserveWhitespace || (preservedNewline && (s[i] == '\n' || s[i] == '\r'))) &&
        isWhitespace(s[i]) && self->tableDepth == 0) {
      if (self->partWordBufferIndex > 0) self->flushPartWordBuffer();
      if (s[i] == '\n' || s[i] == '\r') {
        self->preColumnMod8_ = 0;
        const bool hadText = self->currentTextBlock && !self->currentTextBlock->isEmpty();
        auto style = self->currentTextBlock ? self->currentTextBlock->getBlockStyle().withoutTop().withoutBottom()
                                            : BlockStyle{};
        style.textIndent = 0;
        style.textIndentDefined = true;
        // Preserved newlines split one paragraph just like <br>; later text is unknown.
        style.publisherAlignment = CssTextAlign::None;
        if (hadText) {
          self->makePages(false);
          self->currentTextBlock.reset();
        } else {
          self->currentPageNextY += self->effectiveLineHeight();
        }
        self->startNewTextBlock(style);
      } else if (self->currentTextBlock) {
        // A source space is a token with a legal wrap before it and no added gap.
        const unsigned count = s[i] == '\t' ? 8 - self->preColumnMod8_ : 1;
        const uint32_t origin = self->characterOrigin(i);
        self->currentTextBlock->setNextWordSource({origin, origin == UINT32_MAX ? UINT32_MAX : origin + 1, 0,
                                                   origin == self->sourceTarget ? uint16_t(0) : uint16_t(UINT16_MAX)});
        self->currentTextBlock->addWord(
            std::string(count, ' '), EpdFontFamily::REGULAR, false, false, false, 0, codepointOffset,
            codepointReferenceOffset, 0, true, self->effectiveInlineFontSize, self->effectiveFontFamily,
            self->effectiveLetterSpacing, nullptr, 0, self->effectiveInlinePoints64, self->effectiveTypography);
        self->preColumnMod8_ = (self->preColumnMod8_ + count) & 7;
      }
      self->nextWordContinues = false;
      if (startsCodepoint && countVisibleOffsets) codepointOffset++;
      continue;
    }
    if (isWhitespace(s[i])) {
      // Currently looking at whitespace, if there's anything in the partWordBuffer, flush it
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      // Whitespace is a real word boundary — reset continuation state
      self->nextWordContinues = false;
      if (startsCodepoint && countVisibleOffsets) codepointOffset++;
      // Skip the whitespace char
      continue;
    }

    // Detect U+00A0 (non-breaking space, UTF-8: 0xC2 0xA0) or
    //        U+202F (narrow no-break space, UTF-8: 0xE2 0x80 0xAF).
    //
    // Both are rendered as a visible space but must never allow a line break around them.
    // We split the no-break space into its own word token and link the surrounding words
    // with continuation flags so the layout engine treats them as an indivisible group.
    //
    // Example: "200&#xA0;Quadratkilometer" or "200&#x202F;Quadratkilometer"
    //   Input bytes:  "200\xC2\xA0Quadratkilometer"  (or 0xE2 0x80 0xAF for U+202F)
    //   Tokens produced:
    //     [0] "200"               continues=false
    //     [1] " "                 continues=true   (attaches to "200", no gap)
    //     [2] "Quadratkilometer"  continues=true   (attaches to " ", no gap)
    //
    //   The continuation flags prevent the line-breaker from inserting a line break
    //   between "200" and "Quadratkilometer". However, "Quadratkilometer" is now a
    //   standalone word for hyphenation purposes, so Liang patterns can produce
    //   "200 Quadrat-" / "kilometer" instead of the unusable "200" / "Quadratkilometer".
    if (static_cast<uint8_t>(s[i]) == 0xC2 && i + 1 < len && static_cast<uint8_t>(s[i + 1]) == 0xA0) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      self->partWordSources_[0] = self->characterOrigin(i);
      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->partWordInlinePadding = self->pendingInlinePadding;
      self->pendingInlinePadding = 0;
      self->partWordVisibleOffset = codepointOffset;
      self->partWordReferenceOffset = codepointReferenceOffset;
      self->nextWordContinues = true;  // Attach space to previous word (no break).
      self->flushPartWordBuffer();

      self->nextWordContinues = true;  // Next real word attaches to this space (no break).

      i++;  // Skip the second byte (0xA0)
      if (countVisibleOffsets) codepointOffset++;
      continue;
    }

    // U+202F (narrow no-break space) — identical logic to U+00A0 above.
    if (static_cast<uint8_t>(s[i]) == 0xE2 && i + 2 < len && static_cast<uint8_t>(s[i + 1]) == 0x80 &&
        static_cast<uint8_t>(s[i + 2]) == 0xAF) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      self->partWordSources_[0] = self->characterOrigin(i);
      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->partWordInlinePadding = self->pendingInlinePadding;
      self->pendingInlinePadding = 0;
      self->partWordVisibleOffset = codepointOffset;
      self->partWordReferenceOffset = codepointReferenceOffset;
      self->nextWordContinues = true;
      self->flushPartWordBuffer();

      self->nextWordContinues = true;

      i += 2;  // Skip the remaining two bytes (0x80 0xAF)
      if (countVisibleOffsets) codepointOffset++;
      continue;
    }

    // Skip Zero Width No-Break Space / BOM (U+FEFF) = 0xEF 0xBB 0xBF
    const XML_Char FEFF_BYTE_1 = static_cast<XML_Char>(0xEF);
    const XML_Char FEFF_BYTE_2 = static_cast<XML_Char>(0xBB);
    const XML_Char FEFF_BYTE_3 = static_cast<XML_Char>(0xBF);

    if (s[i] == FEFF_BYTE_1) {
      // Check if the next two bytes complete the 3-byte sequence
      if ((i + 2 < len) && (s[i + 1] == FEFF_BYTE_2) && (s[i + 2] == FEFF_BYTE_3)) {
        // Sequence 0xEF 0xBB 0xBF found!
        i += 2;    // Skip the next two bytes
        continue;  // Move to the next iteration
      }
    }

    // If we're about to run out of space, then cut the word off and start a new one.
    // For CJK text (no spaces), this is the primary word-breaking mechanism.
    // We must avoid splitting multi-byte UTF-8 sequences across word boundaries,
    // otherwise the trailing bytes become orphaned continuation bytes that the
    // decoder can't interpret.
    if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
      int safeLen = utf8SafeTruncateBuffer(self->partWordBuffer, self->partWordBufferIndex);

      if (safeLen < self->partWordBufferIndex && safeLen > 0) {
        // Incomplete UTF-8 sequence at the end — save it before flushing
        int overflow = self->partWordBufferIndex - safeLen;
        uint32_t overflowOffset = self->partWordVisibleOffset;
        uint32_t overflowReferenceOffset = self->partWordReferenceOffset;
        const auto* codepoint = reinterpret_cast<const unsigned char*>(self->partWordBuffer);
        const auto* const prefixEnd = codepoint + safeLen;
        while (codepoint < prefixEnd) {
          utf8NextCodepoint(&codepoint);
          overflowOffset++;
          overflowReferenceOffset++;
        }
        char saved[4];
        uint32_t savedSources[4];
        for (int j = 0; j < overflow; j++) {
          saved[j] = self->partWordBuffer[safeLen + j];
          savedSources[j] = self->partWordSources_[safeLen + j];
        }
        self->partWordBufferIndex = safeLen;
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
        for (int j = 0; j < overflow; j++) {
          self->partWordBuffer[j] = saved[j];
          self->partWordSources_[j] = savedSources[j];
        }
        self->partWordBufferIndex = overflow;
        self->partWordVisibleOffset = overflowOffset;
        self->partWordReferenceOffset = overflowReferenceOffset;
      } else {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
    }

    if (self->partWordBufferIndex == 0) {
      self->partWordVisibleOffset = codepointOffset;
      self->partWordReferenceOffset = codepointReferenceOffset;
      self->partWordInlinePadding = self->pendingInlinePadding;
      self->pendingInlinePadding = 0;
    }
    if (startsCodepoint && self->effectivePreserveWhitespace) self->preColumnMod8_ = (self->preColumnMod8_ + 1) & 7;
    self->partWordSources_[self->partWordBufferIndex] = self->characterOrigin(i);
    self->partWordBuffer[self->partWordBufferIndex++] = s[i];
    if (startsCodepoint && countVisibleOffsets) codepointOffset++;
  }

  // If a paragraph keeps growing, perform the layout and consume all but the last line.
  // Keep the unfinished word across Expat callbacks: callback boundaries are not word boundaries.
  // MAX_WORD_SIZE already bounds partWordBuffer and flushes it UTF-8 safely above.
  self->flushLongTextRunIfNeeded();
}

void XMLCALL ChapterHtmlSlimParser::defaultHandlerExpand(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  if (self->isScanningForPreviewAnchor() || self->previewStopRequested) {
    return;
  }
  // Check if this looks like an entity reference (&...;)
  if (len >= 3 && s[0] == '&' && s[len - 1] == ';') {
    const char* utf8Value = lookupHtmlEntity(s, static_cast<size_t>(len));
    if (utf8Value != nullptr) {
      // Known entity: expand to its UTF-8 value
      characterData(userData, utf8Value, strlen(utf8Value));
      return;
    }
    // Unknown entity stays literal, so its bytes retain distinct raw positions.
    self->literalEntitySource_ = true;
    characterData(userData, s, len);
    self->literalEntitySource_ = false;
    return;
  }
  // Not an entity we recognize - skip it
}

void XMLCALL ChapterHtmlSlimParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  if (self->isScanningForPreviewAnchor()) {
    if (self->depth > 0) {
      self->depth -= 1;
    }
    return;
  }
  if (self->previewStopRequested) {
    return;
  }
  if (self->lowMemoryAbort) {
    return;
  }

  if (self->skipEndElementStateUntilDepth < self->depth) {
    self->depth -= 1;
    self->clearReferenceExclusionIfClosed();
    if (self->skipUntilDepth == self->depth) {
      self->skipUntilDepth = INT_MAX;
      self->skipEndElementStateUntilDepth = INT_MAX;
    }
    return;
  }

  if (self->svgUntilDepth < self->depth && !matches(localTagName(name), IMAGE_TAGS, std::size(IMAGE_TAGS))) {
    self->depth -= 1;
    if (self->svgUntilDepth == self->depth) self->svgUntilDepth = INT_MAX;
    self->clearReferenceExclusionIfClosed();
    return;
  }

  self->emitPendingGeneratedBefore();
  if (!self->generatedPseudos_.empty() && self->generatedPseudos_.back()->depth == self->depth - 1) {
    auto pseudo = std::move(self->generatedPseudos_.back());
    self->generatedPseudos_.pop_back();
    self->emitGeneratedPseudo(pseudo->after, pseudo->afterStyle);
  }

  if (self->textFloatParent_ && self->textFloatDepth_ == self->depth - 1) {
    if (self->partWordBufferIndex) self->flushPartWordBuffer();
    self->finishTextFloat();
    if (self->lowMemoryAbort) return;
  }

  struct FinishAvoid {
    ChapterHtmlSlimParser* parser;
    int depth;
    ~FinishAvoid() {
      if (parser->avoidGroup_.depth != depth || parser->lowMemoryAbort || parser->previewStopRequested) return;
      if (parser->partWordBufferIndex) parser->flushPartWordBuffer();
      if (parser->currentTextBlock && !parser->currentTextBlock->isEmpty()) parser->makePages();
      if (parser->previewStopRequested || parser->lowMemoryAbort) return;
      parser->finishAvoidGroup(false);
    }
  } finishAvoid{self, self->depth - 1};

  struct FinishBackground {
    ChapterHtmlSlimParser* parser;
    int depth;
    ~FinishBackground() { parser->finishBackgroundPicture(depth); }
  } finishBackground{self, self->depth - 1};

  // Ruby text: </rt> distributes ruby to base words, </ruby> resets ruby state
  if (strcmp(name, "rt") == 0) {
    self->collectingRubyText = false;
    if (self->inRuby && self->currentTextBlock) {
      const int currentWordCount = static_cast<int>(self->currentTextBlock->size());
      const int baseWordCount = currentWordCount - self->rubyStartWordIndex;
      std::string cleanRuby = trimAndNormalize(self->rubyTextBuffer);
      if (!cleanRuby.empty()) {
        if (baseWordCount > 0) {
          self->currentTextBlock->setRubyGroupAt(self->rubyStartWordIndex, baseWordCount, cleanRuby);
          self->rubyStartWordIndex = currentWordCount;
        } else if (self->rubyStartWordIndex > 0) {
          int leaderIdx = self->rubyStartWordIndex - 1;
          while (leaderIdx >= 0 &&
                 (self->currentTextBlock->getWordStyleAt(leaderIdx) & EpdFontFamily::RUBY_CONTINUE) != 0) {
            leaderIdx--;
          }
          if (leaderIdx >= 0) {
            std::string prevRuby = self->currentTextBlock->getRubyTextAt(leaderIdx);
            self->currentTextBlock->setRubyForWordAt(leaderIdx, prevRuby + cleanRuby);
          }
        }
      }
    }
    self->rubyTextBuffer.clear();
    // Inline close: the next base (e.g. 字 in <ruby>漢<rt>かん</rt>字<rt>じ</rt></ruby>) joins the
    // preceding one with no space. Whitespace in the source resets this in characterData().
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->nextWordContinues = true;
    }
    self->depth -= 1;
    self->clearReferenceExclusionIfClosed();
    return;
  }
  if (strcmp(name, "ruby") == 0 && self->inRuby) {
    self->inRuby = false;
    self->rubyStartWordIndex = -1;
    self->rubyTextBuffer.clear();
    // Inline close: text following </ruby> joins the annotated base with no space.
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->nextWordContinues = true;
    }
    self->depth -= 1;
    self->clearReferenceExclusionIfClosed();
    return;
  }
  // Check if any style state will change after we decrement depth
  // If so, we MUST flush the partWordBuffer with the CURRENT style first
  // Note: depth hasn't been decremented yet, so we check against (depth - 1)
  const bool willPopStyleStack =
      self->inlineStyleCount_ > 0 && self->inlineStyleBuf_[self->inlineStyleCount_ - 1].depth == self->depth - 1;
  const bool willClearBold = self->boldUntilDepth == self->depth - 1;
  const bool willClearItalic = self->italicUntilDepth == self->depth - 1;
  const bool willClearUnderline = self->underlineUntilDepth == self->depth - 1;
  const bool willClearStrikethrough = self->strikethroughUntilDepth == self->depth - 1;

  const bool styleWillChange =
      willPopStyleStack || willClearBold || willClearItalic || willClearUnderline || willClearStrikethrough;
  const bool closesBlockSpan = self->displayBlockSpanDepth_ == self->depth - 1 && strcmp(name, "span") == 0;
  const bool headerOrBlockTag = isHeaderOrBlock(name) || closesBlockSpan;
  const bool tableStructuralTag = isTableStructuralTag(name);

  if (self->tableDepth > 1 && strcmp(name, "table") == 0) {
    // get rid of all text inside the nested table
    self->partWordBufferIndex = 0;
    self->tableDepth -= 1;
    LOG_DBG("EHP", "nested table detected, get rid of its content");
    return;
  }

  // Flush buffer with current style BEFORE any style changes
  if (self->partWordBufferIndex > 0) {
    // Flush if style will change OR if we're closing a block/structural element
    const bool isInlineTag = !headerOrBlockTag && !tableStructuralTag &&
                             !matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS)) && self->depth != 1;
    const bool shouldFlush = styleWillChange || headerOrBlockTag || matches(name, BOLD_TAGS, std::size(BOLD_TAGS)) ||
                             matches(name, ITALIC_TAGS, std::size(ITALIC_TAGS)) ||
                             matches(name, UNDERLINE_TAGS, std::size(UNDERLINE_TAGS)) ||
                             matches(name, STRIKETHROUGH_TAGS, std::size(STRIKETHROUGH_TAGS)) || tableStructuralTag ||
                             matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS)) || self->depth == 1;

    if (shouldFlush) {
      self->flushPartWordBuffer();
      // If closing an inline element, the next word fragment continues the same visual word
      if (isInlineTag) {
        self->nextWordContinues = true;
      }
    }
  }

  self->depth -= 1;
  self->clearReferenceExclusionIfClosed();
  if (self->dropCap.spanDepth == self->depth) self->dropCap.spanDepth = -1;

  // Pop ancestor entries that were pushed at or below the new depth
  while (!self->ancestorStack_.empty() && self->ancestorStack_.back().depth >= self->depth) {
    self->ancestorStack_.pop_back();
  }

  // Closing a footnote link — create entry from collected text and href
  if (self->insideFootnoteLink && self->depth == self->footnoteLinkDepth) {
    if (self->currentFootnote.number[0] != '\0' && self->currentFootnote.href[0] != '\0') {
      FootnoteEntry entry;
      strncpy(entry.number, self->currentFootnote.number, sizeof(entry.number) - 1);
      entry.number[sizeof(entry.number) - 1] = '\0';
      strncpy(entry.href, self->currentFootnote.href, sizeof(entry.href) - 1);
      entry.href[sizeof(entry.href) - 1] = '\0';
      entry.linkId = self->currentFootnote.linkId;
      int wordIndex =
          self->wordsExtractedInBlock + (self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0);
      self->pendingFootnotes.push_back({wordIndex, entry});
      if (self->pendingFootnotes.size() >= MAX_PENDING_FOOTNOTES_BEFORE_LAYOUT && self->tableDepth == 0) {
        if (self->partWordBufferIndex > 0) {
          self->flushPartWordBuffer();
        }
        self->flushLongTextRunIfNeeded(true);
      }
    }
    self->insideFootnoteLink = false;
  }

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
    self->skipEndElementStateUntilDepth = INT_MAX;
  }

  if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    self->finalizeCurrentTableCell();
    self->nextWordContinues = false;
  }

  if (self->tableDepth == 1 && (strcmp(name, "tr") == 0)) {
    if (self->currentCompactTable) {
      if (self->compactTableUnsupported || self->compactTableFlattened) {
        self->currentCompactTable->setFlattened();
      }
      TableFragmentRow row;
      std::vector<std::shared_ptr<TextBlock>> flatLines;
      std::vector<FootnoteEntry> rowFootnotes;
      rowFootnotes.reserve(Page::MAX_FOOTNOTES_PER_PAGE);
      uint32_t rowVisibleOffset = self->visibleTextOffset;
      const auto result = self->currentCompactTable->finishRow(row, flatLines, rowFootnotes, rowVisibleOffset);
      if (result == CompactTableLayout::RowResult::Abort) {
        self->lowMemoryAbort = true;
        return;
      }
      const bool flatten = result == CompactTableLayout::RowResult::Flatten;
      if (flatten) self->compactTableFlattened = true;
      if (!self->emitCompactTableRow(row, flatLines, rowFootnotes, rowVisibleOffset,
                                     self->currentCompactTable->fragmentColumnCount(), flatten)) {
        self->lowMemoryAbort = true;
        return;
      }
      self->nextWordContinues = false;
      return;
    }
    if (self->currentTableBuffer && self->currentTableBuffer->streaming && !self->streamCurrentTableRow()) {
      return;
    }
    self->nextWordContinues = false;
  }

  if (self->tableDepth == 1 && strcmp(name, "table") == 0) {
    if (self->currentCompactTable) {
      self->finishCompactTable();
      self->currentCompactTable.reset();
      self->compactTableFlattened = false;
      self->compactTableUnsupported = false;
    } else if (self->currentTableBuffer && self->currentTableBuffer->streaming) {
      self->finishStreamingTable(*self->currentTableBuffer);
      self->currentTableBuffer.reset();
    } else {
      self->emitCurrentTableBuffer();
    }
    self->tableDepth -= 1;
    self->tableRowIndex = 0;
    self->tableColIndex = 0;
    auto paragraphAlignmentBlockStyle = BlockStyle();
    const auto& parentStyle = self->blockStyleBuf_[self->blockStyleCount_ - 1];
    paragraphAlignmentBlockStyle.fontScale = parentStyle.fontScale;
    paragraphAlignmentBlockStyle.cssFontScale = parentStyle.cssFontScale;
    paragraphAlignmentBlockStyle.fontSize = parentStyle.fontSize;
    paragraphAlignmentBlockStyle.fontSize64 = parentStyle.fontSize64;
    paragraphAlignmentBlockStyle.textAlignDefined = true;
    paragraphAlignmentBlockStyle.alignment = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                                 ? CssTextAlign::Justify
                                                 : static_cast<CssTextAlign>(self->paragraphAlignment);
    self->startNewTextBlock(paragraphAlignmentBlockStyle);
    self->nextWordContinues = false;
  }

  if (strcmp(name, "li") == 0 && self->pendingListMarkerDepth == self->depth) {
    self->pendingListMarkerDepth = -1;
  }
  if ((strcmp(name, "ul") == 0 || strcmp(name, "ol") == 0) && self->listContextCount_ > 0 &&
      self->listContexts_[self->listContextCount_ - 1].depth == self->depth) {
    self->listContextCount_--;
  }

  // Leaving bold tag
  if (self->boldUntilDepth == self->depth) {
    self->boldUntilDepth = INT_MAX;
  }

  // Leaving italic tag
  if (self->italicUntilDepth == self->depth) {
    self->italicUntilDepth = INT_MAX;
  }

  // Leaving underline tag
  if (self->underlineUntilDepth == self->depth) {
    self->underlineUntilDepth = INT_MAX;
  }

  // Leaving strikethrough tag
  if (self->strikethroughUntilDepth == self->depth) {
    self->strikethroughUntilDepth = INT_MAX;
  }

  // Pop from inline style stack if we pushed an entry at this depth
  // This also restores inherited font styles when a styled block closes.
  if (self->inlineStyleCount_ > 0 && self->inlineStyleBuf_[self->inlineStyleCount_ - 1].depth == self->depth) {
    self->inlineStyleCount_--;
    self->updateEffectiveInlineStyle();
  }

  // Spend the container's closing inset once, before either decorated or ordinary block cleanup.
  if (headerOrBlockTag && strcmp(name, "br") != 0 && self->blockStyleCount_ > 1 &&
      self->blockStyleDepths_[self->blockStyleCount_ - 1] == self->depth) {
    const auto& closingStyle = self->blockStyleBuf_[self->blockStyleCount_ - 1];
    if (!self->currentTextBlock || (self->emptyTextBlockAfterImage && self->currentTextBlock->isEmpty())) {
      self->currentPageNextY += closingStyle.bottomInset();
      if (self->currentTextBlock) {
        // A direct image already consumed the top inset. Keep the empty buffer usable for
        // following bare text, but restore the parent style before it can inherit figure spacing.
        self->currentTextBlock->setBlockStyle(
            self->blockStyleBuf_[self->blockStyleCount_ - 2].withoutTop().withoutBottom());
      }
    } else {
      self->currentTextBlock->setBlockStyle(self->currentTextBlock->getBlockStyle().addBottom(closingStyle));
      auto& pending = self->currentTextBlock->getBlockStyle();
      const bool emptyBox = self->boxScopeCount && self->boxScopes[self->boxScopeCount - 1].depth == self->depth &&
                            self->boxScopes[self->boxScopeCount - 1].top < 0 &&
                            !self->boxScopes[self->boxScopeCount - 1].continued;
      if (self->currentTextBlock->isEmpty() && self->wordsExtractedInBlock == 0 && pending.topInset() == 0 &&
          !emptyBox) {
        // A flushed child leaves an empty parent block. Spend its closing spacing
        // before that placeholder is reused for the next paragraph. Empty border
        // boxes keep their own spacing for closeBoxScope() to consume.
        self->currentPageNextY += pending.bottomInset();
        pending.marginBottom = pending.paddingBottom = 0;
      }
    }
  }

  if (headerOrBlockTag && self->boxScopeCount && self->boxScopes[self->boxScopeCount - 1].depth == self->depth) {
    if (self->currentTextBlock && (!self->currentTextBlock->isEmpty() || self->dropCap.length)) {
      self->makePages();
      self->currentTextBlock.reset();
    }
    self->closeBoxScope();
  }

  // Clear block style when leaving header or block elements
  if (headerOrBlockTag) {
    self->currentCssStyle.reset();
    self->updateEffectiveInlineStyle();

    // br is self-closing and not a container — it doesn't push/pop the stack.
    if (strcmp(name, "br") != 0 && self->blockStyleCount_ > 1 &&
        self->blockStyleDepths_[self->blockStyleCount_ - 1] == self->depth) {
      self->blockStyleCount_--;
      self->updateEffectiveInlineStyle();
      if (self->tableDepth == 0 && !self->isLightMode() && self->renderer.getFontPointSize(self->fontId)) {
        if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
          self->makePages();
          self->currentTextBlock.reset();
        }
        const bool emptyAfterImage =
            self->currentTextBlock && self->currentTextBlock->isEmpty() && self->emptyTextBlockAfterImage;
        self->startNewTextBlock(self->blockStyleBuf_[self->blockStyleCount_ - 1].withoutTop().withoutBottom());
        self->emptyTextBlockAfterImage = emptyAfterImage;
      }
    }
  }
  if (self->tableDepth == 1 && strcmp(name, "caption") == 0 && self->currentCompactTable && self->currentTextBlock) {
    // Captions are ordinary text, not table cells. Emit them before the first
    // grid row so compact-table capture never sees caption text without a
    // destination block.
    self->makePages();
    self->currentTextBlock.reset();
    self->currentTextRunBytes = 0;
    self->nextWordContinues = false;
  }
  if (self->headingDepth == self->depth) {
    self->headingDepth = -1;
  }

  if (closesBlockSpan) {
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) self->makePages();
    if (self->lowMemoryAbort) return;
    self->currentTextBlock.reset();
    auto resumed = self->displayBlockParentStyle_.withoutTop().withoutBottom();
    resumed.textIndent = 0;
    self->displayBlockSpanDepth_ = -1;
    self->currentCssStyle = self->displayBlockParentCss_;
    self->startNewTextBlock(resumed);
    self->updateEffectiveInlineStyle();
  }

  if (strcmp(name, "html") == 0) {
    self->htmlEnded_ = true;
  }
}

std::unique_ptr<ChapterHtmlSlimParser::AdvancePrewarm> ChapterHtmlSlimParser::startAdvancePrewarm() const {
  if (!renderer.isSdCardFont(fontId) || isPreviewBuild()) {
    return nullptr;
  }

  const auto heap = MemoryBudget::snapshot();
  const uint32_t needFree = MIN_FREE_HEAP_FOR_SECTION_PREWARM;
  const uint32_t needBlock = MIN_MAX_ALLOC_FOR_SECTION_PREWARM;
  if (!MemoryBudget::hasHeap(heap, needFree, needBlock)) {
    LOG_DBG("EHP", "Skipping section advance prewarm: low heap (free=%lu, maxAlloc=%lu, need %lu/%lu)", heap.freeHeap,
            heap.maxAllocHeap, needFree, needBlock);
    return nullptr;
  }

  auto state = makeUniqueNoThrow<AdvancePrewarm>();
  if (state) {
    state->codepoints = makeUniqueNoThrow<uint32_t[]>(SECTION_ADVANCE_PREWARM_MAX_CODEPOINTS);
    state->buffer = makeUniqueNoThrow<uint8_t[]>(SECTION_ADVANCE_PREWARM_READ_BUFFER_SIZE);
    state->unique =
        makeUniqueNoThrow<UniqueCodepointSet>(state->codepoints.get(), SECTION_ADVANCE_PREWARM_MAX_CODEPOINTS);
  }
  if (!state || !state->codepoints || !state->buffer || !state->unique) {
    LOG_ERR("EHP", "Failed to allocate section advance prewarm buffers");
    return nullptr;
  }
  return state;
}

// Scans one buffer of HTML from `file`'s current position (state.offset).
void ChapterHtmlSlimParser::scanAdvancePrewarmChunk(AdvancePrewarm& state, FsFile& file) const {
  const int read = file.read(state.buffer.get(), SECTION_ADVANCE_PREWARM_READ_BUFFER_SIZE);
  const size_t len = read > 0 ? static_cast<size_t>(read) : 0;
  if (len == 0 && file.available() > 0) {
    LOG_DBG("EHP", "Section advance prewarm stopped after short read");
  }

  for (size_t i = 0; i < len && !state.hitCap; ++i) {
    const uint8_t byte = state.buffer[i];
    const char c = static_cast<char>(byte);

    if (state.inTag) {
      if (c == '>') {
        state.inTag = false;
      }
      continue;
    }

    if (state.inEntity) {
      if (c == ';' || isWhitespace(c)) {
        state.inEntity = false;
      } else if (c == '<') {
        state.inEntity = false;
        state.inTag = true;
      }
      continue;
    }

    if (c == '<') {
      state.inTag = true;
      resetPrewarmUtf8(state.utf8Accumulator, state.utf8Remaining);
      continue;
    }
    if (c == '&') {
      state.inEntity = true;
      resetPrewarmUtf8(state.utf8Accumulator, state.utf8Remaining);
      state.hitCap = state.unique->add(' ');
      continue;
    }
    if (isWhitespace(c)) {
      resetPrewarmUtf8(state.utf8Accumulator, state.utf8Remaining);
      state.hitCap = state.unique->add(' ');
      continue;
    }

    state.hitCap = feedPrewarmUtf8Byte(byte, *state.unique, state.utf8Accumulator, state.utf8Remaining);
  }
  state.offset += len;

  if (len > 0 && !state.hitCap && file.available() > 0) {
    return;
  }
  state.scanned = true;
  if (state.utf8Remaining != 0 && !state.hitCap) {
    state.hitCap = state.unique->add(REPLACEMENT_GLYPH);
  }
  state.cpCount = state.unique->finish();
  state.unique.reset();
  if (state.hitCap) {
    LOG_DBG("EHP", "Section advance prewarm hit unique codepoint cap (%u)",
            static_cast<unsigned>(SECTION_ADVANCE_PREWARM_MAX_CODEPOINTS));
  }
}

void ChapterHtmlSlimParser::feedAdvancePrewarm(AdvancePrewarm& state, const uint32_t maxCount) const {
  const uint32_t count = std::min(maxCount, state.cpCount - state.fedCount);
  if (count == 0) return;
  const bool first = state.fedCount == 0;
  renderer.ensureSdCardFontReady(fontId, state.codepoints.get() + state.fedCount, count, /*includeSpace=*/first,
                                 first && hyphenationEnabled, /*styleMask=*/0x0F);
  state.fedCount += count;
}

void ChapterHtmlSlimParser::prewarmSectionAdvanceTable(FsFile& file) const {
  const auto state = startAdvancePrewarm();
  if (!state) {
    file.seekSet(0);
    return;
  }

  const uint32_t startMs = millis();
  while (!state->scanned) {
    scanAdvancePrewarmChunk(*state, file);
  }

  if (!file.seekSet(0)) {
    LOG_ERR("EHP", "Failed to rewind section file after advance prewarm");
    return;
  }

  if (state->cpCount == 0) {
    return;
  }

  feedAdvancePrewarm(*state, state->cpCount);
  LOG_DBG("EHP", "Section advance prewarm: codepoints=%u time=%lu ms free=%u maxAlloc=%u",
          static_cast<unsigned>(state->cpCount), millis() - startMs, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

bool ChapterHtmlSlimParser::prewarmSectionAdvanceStep() {
  if (!advancePrewarmPending_ || !activeParser) {
    return false;
  }
  if (!advancePrewarm_) {
    advancePrewarm_ = startAdvancePrewarm();
    if (!advancePrewarm_) {
      advancePrewarmPending_ = false;
      return false;
    }
  }

  AdvancePrewarm& state = *advancePrewarm_;
  const uint32_t startMs = millis();
  if (!state.scanned) {
    if (!ensureInputFileOpen()) {
      LOG_ERR("EHP", "Deferred advance prewarm: parser input unavailable");
      advancePrewarm_.reset();
      advancePrewarmPending_ = false;
      return false;
    }
    // Scan from the prewarm's own offset, then put the parser back where it was.
    const size_t resume = parseFile_.position();
    if (parseFile_.seek(state.offset)) {
      do {
        scanAdvancePrewarmChunk(state, parseFile_);
      } while (!state.scanned && millis() - startMs < SECTION_ADVANCE_PREWARM_STEP_MS);
    } else {
      LOG_ERR("EHP", "Deferred advance prewarm: seek to %u failed", static_cast<unsigned>(state.offset));
      state.scanned = true;
      state.cpCount = 0;
    }
    if (!parseFile_.seek(resume)) {
      // Reopen at the parser's offset on the next parse step rather than parse from here.
      LOG_ERR("EHP", "Deferred advance prewarm: failed to restore parser input to %u", static_cast<unsigned>(resume));
      parseFile_.close();
      parseFileOffset_ = resume;
    }
    if (state.scanned) {
      // Lowest codepoints first, as the one-shot prewarm leaves them in a full table.
      std::sort(state.codepoints.get(), state.codepoints.get() + state.cpCount);
      state.cpCount = std::min(state.cpCount, SECTION_ADVANCE_PREWARM_FEED_LIMIT);
    }
  } else {
    do {
      feedAdvancePrewarm(state, SECTION_ADVANCE_PREWARM_FEED_BATCH);
    } while (state.fedCount < state.cpCount && millis() - startMs < SECTION_ADVANCE_PREWARM_STEP_MS);
  }
  state.busyMs += millis() - startMs;

  if (state.scanned && state.fedCount >= state.cpCount) {
    LOG_DBG("EHP", "Deferred section advance prewarm: codepoints=%u busy=%lu ms free=%u maxAlloc=%u",
            static_cast<unsigned>(state.cpCount), static_cast<unsigned long>(state.busyMs), ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    advancePrewarm_.reset();
    advancePrewarmPending_ = false;
  }
  return true;
}

ChapterHtmlSlimParser::~ChapterHtmlSlimParser() { abortParse(); }

bool ChapterHtmlSlimParser::ensureInputFileOpen() {
  if (parseFile_) {
    return true;
  }
  if (!Storage.openFileForRead("EHP", filepath, parseFile_)) {
    return false;
  }
  if (parseFileOffset_ > 0 && !parseFile_.seek(parseFileOffset_)) {
    LOG_ERR("EHP", "Failed to seek parser input to %u", static_cast<unsigned>(parseFileOffset_));
    parseFile_.close();
    return false;
  }
  if (parseFileSize_ == 0) {
    parseFileSize_ = parseFile_.size();
  }
  return true;
}

void ChapterHtmlSlimParser::releaseInputFile() {
  if (!parseFile_) {
    return;
  }
  parseFileOffset_ = parseFile_.position();
  parseFile_.close();
}

bool ChapterHtmlSlimParser::scanBodyFont(SaxParser& parser, FsFile& file, const CssParser& css, uint32_t& family,
                                         const bool preferParagraph, const CssParser* overlay) {
  auto probe = makeUniqueNoThrow<BodyFontProbe>(css, preferParagraph, overlay);
  auto buffer = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[PARSE_BUFFER_SIZE]);
  if (!probe || !buffer || !probe->attach(parser)) {
    LOG_ERR("EHP", "OOM: body font probe");
    return false;
  }
  size_t consumed = 0;
  while (file.available() > 0 && consumed < BODY_FONT_SCAN_BYTES) {
    const size_t length = file.read(buffer.get(), PARSE_BUFFER_SIZE);
    if (length == 0) return false;
    consumed += length;
    if (!parser.feed(buffer.get(), length)) break;
    delay(1);
  }
  family = probe->family(preferParagraph);
  return true;
}

bool ChapterHtmlSlimParser::detectBodyFont() {
  SaxParser probeParser;
  if (!scanBodyFont(probeParser, parseFile_, *cssParser, bodyFontFamily, false, documentCss_.get())) return false;
  if (!parseFile_.seek(0)) {
    LOG_ERR("EHP", "Failed to rewind after body font scan");
    return false;
  }
  return true;
}

uint32_t ChapterHtmlSlimParser::publisherBodyFontFamily(const std::string& path, const CssParser& css,
                                                        const std::string& documentCache, const std::string& scope) {
  std::unique_ptr<CssParser> overlay;
  if (!documentCache.empty()) {
    overlay = makeUniqueNoThrow<CssParser>(documentCache, &css);
    if (!overlay) return 0;
    overlay->setFontScope(scope);
    if (!overlay->loadFromCache()) return 0;
  }
  FsFile file;
  if (!Storage.openFileForRead("EHP", path, file)) return 0;
  SaxParser parser;
  uint32_t family = 0;
  const bool ok = scanBodyFont(parser, file, css, family, true, overlay.get());
  file.close();
  return ok ? family : 0;
}

bool ChapterHtmlSlimParser::beginParse() {
  bodyFontFamily = 0;
  generatedPseudos_.clear();
  backgroundPictures_.clear();
  containerWidthCount_ = 0;
  layoutScopeCount_ = 0;
  inheritedLayout_ = LayoutProperties{};
  displayBlockSpanDepth_ = -1;
  floatScopeCount_ = 0;
  activeFloatBottom_ = 0;
  pendingFloatImage_.reset();
  textFloatParent_.reset();
  textFloatParentFootnotes_.clear();
  textFloatParentMarkers_.clear();
  textFloatDepth_ = -1;
  if (embeddedStyle && !documentCssReady_) return false;
  documentCss_.reset();
  if (embeddedStyle && cssParser && !documentCssPath_.empty()) {
    documentCss_ = makeUniqueNoThrow<CssParser>(documentCssPath_, cssParser);
    if (!documentCss_) {
      lowMemoryAbort = true;
      return false;
    }
    documentCss_->setFontScope(documentFontScope_);
    if (!documentCss_->loadFromCache()) {
      documentCss_.reset();
      return false;
    }
  }
  pendingInlineImages.clear();
  nextInlineImageId = 1;
  malformedMarkupTruncated = false;
  htmlEnded_ = false;
  parseFileOffset_ = 0;
  parseFileSize_ = 0;
  listContextCount_ = 0;
  // Runs before the render pass opens the file, so only one reader is ever open at a time.
  if (isPreviewBuild()) {
    locatePreviewBlockStart();
  }
  // Initialize block style stack with a root entry representing "no ancestor block elements".
  // The user's paragraph alignment is set as the default so child elements without explicit
  // text-align inherit it correctly through getCombinedBlockStyle.
  BlockStyle rootBlockStyle;
  rootBlockStyle.alignment = (this->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                 ? CssTextAlign::Justify
                                 : static_cast<CssTextAlign>(this->paragraphAlignment);
  // Compute on the target ABI: the first allocation starts at an aligned slab
  // boundary; the second may need padding for BlockStyle. Keep both bounded
  // stacks in one fallible slab instead of reserving a second 4 KiB slab.
  constexpr size_t inlineBytes = sizeof(StyleStackEntry) * MAX_INLINE_STYLE_DEPTH;
  constexpr size_t blockAlignment = alignof(BlockStyle);
  constexpr size_t blockOffset = (inlineBytes + blockAlignment - 1) & ~(blockAlignment - 1);
  constexpr size_t stackBytes = blockOffset + sizeof(BlockStyle) * MAX_BLOCK_STYLE_DEPTH;
  if (!parseArena_.init(std::max(PARSE_ARENA_SLAB_SIZE, stackBytes))) {
    LOG_ERR("EHP", "Failed to init parse arena");
    lowMemoryAbort = true;
    return false;
  }
  inlineStyleBuf_ = arenaNewArray<StyleStackEntry>(parseArena_, MAX_INLINE_STYLE_DEPTH);
  blockStyleBuf_ = arenaNewArray<BlockStyle>(parseArena_, MAX_BLOCK_STYLE_DEPTH);
  if (!inlineStyleBuf_ || !blockStyleBuf_) {
    LOG_ERR("EHP", "Parse arena OOM for style stacks");
    lowMemoryAbort = true;
    parseArena_.release();
    return false;
  }
  inlineStyleCount_ = 0;
  blockStyleCount_ = 0;
  blockStyleBuf_[blockStyleCount_++] = rootBlockStyle;

  auto paragraphAlignmentBlockStyle = BlockStyle();
  paragraphAlignmentBlockStyle.textAlignDefined = true;
  const auto align = rootBlockStyle.alignment;
  paragraphAlignmentBlockStyle.alignment = align;
  if (!isPreviewBuild()) {
    startNewTextBlock(paragraphAlignmentBlockStyle);
    if (lowMemoryAbort) {
      parseArena_.release();
      inlineStyleBuf_ = nullptr;
      blockStyleBuf_ = nullptr;
      return false;
    }
  }

  if (!usesSimpleCssLookup()) {
    ancestorStack_.reserve(32);
  }

  activeParser = makeUniqueNoThrow<SaxParser>();
  parseBuffer_.reset(new (std::nothrow) uint8_t[PARSE_BUFFER_SIZE]);
  if (!activeParser || !parseBuffer_) {
    LOG_ERR("EHP", "Couldn't allocate memory for parser");
    lowMemoryAbort = true;
    parseArena_.release();
    inlineStyleBuf_ = nullptr;
    blockStyleBuf_ = nullptr;
    return false;
  }

  if (!Storage.openFileForRead("EHP", filepath, parseFile_)) {
    activeParser.reset();
    parseBuffer_.reset();
    parseArena_.release();
    inlineStyleBuf_ = nullptr;
    blockStyleBuf_ = nullptr;
    return false;
  }
  parseFileSize_ = parseFile_.size();

  // Get file size to decide whether to show indexing popup.
  if (popupFn && parseFileSize_ >= MIN_SIZE_FOR_POPUP) {
    popupFn();
  }

  if (publisherFonts == PublisherFonts::Accents && embeddedStyle && !isLightMode() && cssParser && !detectBodyFont()) {
    abortParse();
    return false;
  }
  if (!activeParser->init(this, startElement, endElement, characterData, defaultHandlerExpand, true,
                          SaxParser::Profile::Full, true)) {
    lowMemoryAbort = true;
    abortParse();
    return false;
  }

  // Compute the time taken to parse and build pages
  parseStartTime_ = millis();
  advancePrewarm_.reset();
  advancePrewarmPending_ = deferAdvancePrewarm_;
  if (!deferAdvancePrewarm_) prewarmSectionAdvanceTable(parseFile_);
  return true;
}

ChapterHtmlSlimParser::ParseStatus ChapterHtmlSlimParser::parseStep() {
  if (!activeParser) {
    LOG_ERR("EHP", "parseStep called without an active parser");
    return ParseStatus::Error;
  }
  if (!ensureInputFileOpen()) {
    LOG_ERR("EHP", "Failed to reopen parser input");
    return ParseStatus::Error;
  }

  void* const buf = parseBuffer_.get();
  if (!buf) {
    LOG_ERR("EHP", "Couldn't allocate memory for buffer");
    return ParseStatus::Error;
  }

  const size_t len = parseFile_.read(buf, PARSE_BUFFER_SIZE);
  parseFileOffset_ = parseFile_.position();
  if (len == 0 && parseFile_.available() > 0) {
    LOG_ERR("EHP", "File read error");
    return ParseStatus::Error;
  }

  const bool done = parseFile_.available() == 0;
  const bool parsed = activeParser->feed(static_cast<const uint8_t*>(buf), len) && (!done || activeParser->finalize());
  if (!parsed && !previewStopRequested) {
    LOG_ERR("EHP", "Parse error at line %d: %s", activeParser->errorLine(), activeParser->errorString());
    if (isPreviewBuild()) return ParseStatus::Error;
    malformedMarkupTruncated = true;
    return ParseStatus::Done;
  }
  if (activeParser->truncationFlags() & 0x1fU) malformedMarkupTruncated = true;

  if (lowMemoryAbort) {
    LOG_ERR("EHP", "Aborting section parse due to low heap");
    return ParseStatus::Error;
  }

  if (done || previewStopRequested || activeParser->isStopped()) {
    return ParseStatus::Done;
  }
  return ParseStatus::More;
}

void ChapterHtmlSlimParser::abortParse() {
  avoidGroup_ = {};
  advancePrewarm_.reset();
  advancePrewarmPending_ = false;
  pendingInlineImages.clear();
  generatedPseudos_.clear();
  backgroundPictures_.clear();
  containerWidthCount_ = 0;
  layoutScopeCount_ = 0;
  inheritedLayout_ = LayoutProperties{};
  displayBlockSpanDepth_ = -1;
  floatScopeCount_ = 0;
  activeFloatBottom_ = 0;
  pendingFloatImage_.reset();
  textFloatParent_.reset();
  textFloatParentFootnotes_.clear();
  textFloatParentMarkers_.clear();
  textFloatDepth_ = -1;
  if (activeParser) {
    activeParser.reset();
    parseBuffer_.reset();
  }
  if (parseFile_) {
    parseFile_.close();
  }
  parseFileOffset_ = 0;
  parseFileSize_ = 0;
  parseArena_.release();
  inlineStyleBuf_ = nullptr;
  inlineStyleCount_ = 0;
  blockStyleBuf_ = nullptr;
  blockStyleCount_ = 0;
}

bool ChapterHtmlSlimParser::finishParse() {
  advancePrewarm_.reset();
  advancePrewarmPending_ = false;
  if (activeParser) {
    LOG_DBG("EHP", "Time to parse and build pages: %lu ms (free=%u, maxAlloc=%u)", millis() - parseStartTime_,
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    activeParser.reset();
    parseBuffer_.reset();
  }
  parseFile_.close();
  parseFileOffset_ = 0;
  parseFileSize_ = 0;

  if (malformedMarkupTruncated) {
    LOG_DBG("EHP", "Malformed markup encountered; finalizing partial chapter content");
    flushMalformedPartialContent();
    if (lowMemoryAbort) {
      parseArena_.release();
      inlineStyleBuf_ = nullptr;
      blockStyleBuf_ = nullptr;
      return false;
    }
  }

  // Process last page if there is still text
  if (isPreviewBuild() && !previewAnchorFound) {
    LOG_ERR("EHP", "Preview anchor '%s' was not found", previewAnchor.c_str());
    abortParse();
    return false;
  }

  if (currentTextBlock && !previewStopRequested) {
    if (shouldAbortForLowMemory("final page layout")) {
      abortParse();
      return false;
    }
    makePages();
    if (lowMemoryAbort) {
      abortParse();
      return false;
    }
    if (!pendingAnchorId.empty()) {
      anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
      pendingAnchorId.clear();
      pendingAnchorFromInlineA = false;
    }
    currentTextBlock.reset();
  }
  if (pendingFloatImage_ && !attachPendingFloatImage()) {
    abortParse();
    return false;
  }
  finishAvoidGroup(false);
  resolveSourceAnchors(0, 0, true);
  if (!previewStopRequested && currentPage && !currentPage->elements.empty()) {
    completeCurrentPage();
    completedPageCount++;
    stopPreviewIfPageLimitReached();
  }
  currentPage.reset();

  if (malformedMarkupTruncated && !appendMalformedMarkupWarningPage()) {
    LOG_ERR("EHP", "Failed to append malformed markup warning page");
    parseArena_.release();
    inlineStyleBuf_ = nullptr;
    blockStyleBuf_ = nullptr;
    return false;
  }

  parseArena_.release();
  inlineStyleBuf_ = nullptr;
  blockStyleBuf_ = nullptr;

  return true;
}

bool ChapterHtmlSlimParser::parseAndBuildPages() {
  if (!beginParse()) {
    return false;
  }
  for (;;) {
    const ParseStatus status = parseStep();
    if (status == ParseStatus::Error) {
      abortParse();
      return false;
    }
    if (status == ParseStatus::Done) {
      break;
    }
  }
  return finishParse();
}

void ChapterHtmlSlimParser::addLineToPage(std::shared_ptr<TextBlock> line, const uint32_t visibleOffset,
                                          const uint32_t referenceOffset) {
  if (lowMemoryAbort || previewStopRequested) {
    return;
  }

  const auto& lineImages = currentTextBlock->currentLineImages();
  const int textHeight =
      epubLineHeightPixels(line->maxLineHeight(renderer, fontId), lineCompression,
                           ignorePublisherLineSpacing ? 0 : line->getBlockStyle().lineHeightMultiplier) +
      line->getRubyShift(line->maxAscender(renderer, fontId));
  if (line->getBlockStyle().fontSize || effectiveInlineFontSize || textHeight != effectiveLineHeight()) {
    auto style = line->getBlockStyle();
    style.lineHeight = static_cast<uint16_t>(textHeight);
    line->setBlockStyle(style);
  }
  int lineHeight = textHeight;
  for (const auto& image : lineImages) lineHeight = std::max(lineHeight, static_cast<int>(image.height));

  if (!currentPage) {
    if (!startNewPage("line layout")) {
      return;
    }
  }

  // CrossPoint #3875 (Phạm Bình An): charge initial CSS spacing only
  // when the first actual line is emitted, including incremental flushes.
  if (wordsExtractedInBlock == 0) {
    const auto& style = currentTextBlock->getBlockStyle();
    if (style.marginTop > 0) currentPageNextY += style.marginTop;
    if (style.paddingTop > 0) currentPageNextY += style.paddingTop;
  }

  if (currentPageNextY + lineHeight > viewportHeight) {
    completeCurrentPage();
    completedPageCount++;
    stopPreviewIfPageLimitReached();
    if (previewStopRequested) {
      return;
    }
    if (!startNewPage("line page break")) {
      return;
    }
  }

  setCurrentPageVisibleOffset(visibleOffset, referenceOffset);

  // Keep a link available on every page where its text is visible. Usually this
  // adds one compact entry; a long wrapped link can span lines or pages.
  for (uint16_t wordIndex = 0; wordIndex < line->wordCount(); ++wordIndex) {
    const uint8_t linkId = line->wordLinkId(wordIndex);
    if (linkId == 0) continue;
    // Fragment layout can emit a linked line before </a> queues its final
    // word index. The active link already has the href and display label.
    if (insideFootnoteLink && linkId == currentFootnote.linkId) {
      currentPage->addFootnote(currentFootnote.number, currentFootnote.href, linkId);
      continue;
    }
    const auto entry = std::find_if(pendingFootnotes.begin(), pendingFootnotes.end(),
                                    [linkId](const auto& pending) { return pending.second.linkId == linkId; });
    if (entry != pendingFootnotes.end()) {
      currentPage->addFootnote(entry->second.number, entry->second.href, entry->second.linkId);
    }
  }

  // Track cumulative words to retire links after laying out their final word.
  wordsExtractedInBlock += line->wordCount() + lineImages.size();
  auto footnoteIt = pendingFootnotes.begin();
  while (footnoteIt != pendingFootnotes.end() && footnoteIt->first <= wordsExtractedInBlock) {
    currentPage->addFootnote(footnoteIt->second.number, footnoteIt->second.href, footnoteIt->second.linkId);
    ++footnoteIt;
  }
  pendingFootnotes.erase(pendingFootnotes.begin(), footnoteIt);
  attachPendingPublisherPageMarkers(currentPageNextY);

  // Apply horizontal left inset (margin + padding) as x position offset
  const int16_t xOffset = line->getBlockStyle().leftInset();
  if (!line->isEmpty()) {
    // A taller inline image raises the line's top edge; keep the text beside
    // its lower edge instead of leaving it stranded above the image.
    auto pageLine = makeUniqueNoThrow<PageLine>(line, xOffset, currentPageNextY + lineHeight - textHeight);
    if (!pageLine) {
      LOG_ERR("EHP", "Failed to create PageLine");
      lowMemoryAbort = true;
      return;
    }
    currentPage->noteSourceLine(*pageLine->getBlock());
    resolveSourceAnchors(pageLine->getBlock()->sourceMin(), pageLine->getBlock()->sourceEnd());
    currentPage->elements.push_back(std::move(pageLine));
  }
  for (const auto& image : lineImages) {
    const auto it = std::find_if(pendingInlineImages.begin(), pendingInlineImages.end(),
                                 [&image](const PendingInlineImage& pending) { return pending.id == image.id; });
    if (it == pendingInlineImages.end()) {
      LOG_ERR("EHP", "Missing inline image %u", image.id);
      lowMemoryAbort = true;
      return;
    }
    auto pageImage = makeUniqueNoThrow<PageImage>(std::move(it->block), xOffset + image.x,
                                                  currentPageNextY + lineHeight - image.height, true);
    if (!pageImage) {
      LOG_ERR("EHP", "Failed to create inline PageImage");
      lowMemoryAbort = true;
      return;
    }
    bindSourceAnchors(it->sourceOffset);
    resolveSourceAnchors(it->sourceOffset, it->sourceOffset == UINT32_MAX ? 0 : it->sourceOffset + 1);
    currentPage->noteSourcePoint(it->sourceOffset);
    currentPage->elements.push_back(std::move(pageImage));
    pendingInlineImages.erase(it);
  }
  markCurrentPageFromCurrentTextBlock();
  noteContent(currentPageNextY, currentPageNextY + lineHeight);
  currentPageNextY += lineHeight;
}

int ChapterHtmlSlimParser::effectiveLineHeight() const {
  return epubLineHeightPixels(
      renderer.getLineHeight(currentTextFontId()), lineCompression,
      !ignorePublisherLineSpacing && currentTextBlock ? currentTextBlock->getBlockStyle().lineHeightMultiplier : 0);
}

void ChapterHtmlSlimParser::makePages(const bool completeBlock) {
  if (shouldAbortForLowMemory("page layout")) {
    return;
  }

  if (!currentTextBlock) {
    LOG_ERR("EHP", "!! No text block to make pages for !!");
    return;
  }

  if (!currentPage) {
    if (!startNewPage("page layout")) {
      return;
    }
  }

  // An entire image-only block is artwork, not prose. Keep its source alignment
  // even when the reader overrides paragraph alignment. Wait until final layout
  // so text after an inline image still uses the reader's preference. Previously
  // flushed prose or <br> fragments must not turn an image-only tail into an ornament.
  if (completeBlock && embeddedStyle && !currentTextBlock->getBlockStyle().fromBrElement &&
      wordsExtractedInBlock == 0 && !pendingInlineImages.empty() &&
      currentTextBlock->size() == pendingInlineImages.size()) {
    auto style = currentTextBlock->getBlockStyle();
    if (style.publisherAlignment != CssTextAlign::None) {
      style.alignment = style.publisherAlignment;
      style.textAlignDefined = true;
      currentTextBlock->setBlockStyle(style);
    }
  }

  const BlockStyle& blockStyle = currentTextBlock->getBlockStyle();
  const int lineHeight = effectiveLineHeight();

  if (!layoutText(true)) {
    LOG_ERR("EHP", "Failed to lay out text block");
    lowMemoryAbort = true;
    return;
  }
  if (lowMemoryAbort) {
    return;
  }

  // Fallback: transfer any remaining pending footnotes to current page.
  // Normally addLineToPage handles this via word-index tracking, but this catches
  // edge cases where a footnote's word index equals the exact block size.
  if (!pendingFootnotes.empty() && currentPage) {
    for (const auto& [idx, fn] : pendingFootnotes) {
      currentPage->addFootnote(fn.number, fn.href, fn.linkId);
    }
    pendingFootnotes.clear();
  }
  attachPendingPublisherPageMarkers(currentPageNextY);

  // Apply bottom spacing after the paragraph (stored in pixels)
  if (blockStyle.marginBottom > 0) {
    currentPageNextY += blockStyle.marginBottom;
  }
  if (blockStyle.paddingBottom > 0) {
    currentPageNextY += blockStyle.paddingBottom;
  }

  // Extra paragraph spacing if enabled (default behavior)
  if (extraParagraphSpacing) {
    currentPageNextY += lineHeight / 2;
  }

  if (blockStyle.pageBreakAfter && currentPage && !currentPage->elements.empty()) {
    completeCurrentPage();
    completedPageCount++;
    stopPreviewIfPageLimitReached();
    if (previewStopRequested) {
      return;
    }
    currentPageNextY = 0;
  }
}
