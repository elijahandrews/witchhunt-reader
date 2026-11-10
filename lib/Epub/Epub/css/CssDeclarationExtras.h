#pragma once
#include <charconv>
#include <cmath>
#include <optional>
#include <string_view>

#include "CssStyle.h"

namespace cssExtras {
inline bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f'; }
inline std::string_view trim(std::string_view v) {
  while (!v.empty() && space(v.front())) v.remove_prefix(1);
  while (!v.empty() && space(v.back())) v.remove_suffix(1);
  return v;
}
inline bool eq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char c = a[i];
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (c != b[i]) return false;
  }
  return true;
}
inline bool number(std::string_view v, float& n) {
  if (v.empty() || v.size() > 23) return false;
  char text[24];
  std::copy(v.begin(), v.end(), text);
  text[v.size()] = 0;
  char* tail = nullptr;
  n = std::strtof(text, &tail);
  return tail == text + v.size() && tail != text && std::isfinite(n);
}
inline bool length(std::string_view v, CssLength& out) {
  static constexpr std::string_view suffixes[] = {"rem", "em", "px", "pt", "%"};
  static constexpr CssUnit units[] = {CssUnit::Rem, CssUnit::Em, CssUnit::Pixels, CssUnit::Points, CssUnit::Percent};
  for (size_t i = 0; i < 5; ++i) {
    if (v.size() > suffixes[i].size() && eq(v.substr(v.size() - suffixes[i].size()), suffixes[i])) {
      if (!number(v.substr(0, v.size() - suffixes[i].size()), out.value) || out.value < 0) return false;
      out.unit = units[i];
      return true;
    }
  }
  out = {};
  return number(v, out.value) && out.value == 0;
}
inline int hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
inline bool transparent(std::string_view v) {
  if (eq(v, "transparent")) return true;
  if ((v.size() == 5 || v.size() == 9) && v.front() == '#') {
    for (size_t i = 1; i < v.size(); ++i)
      if (hex(v[i]) < 0) return false;
    return v.back() == '0' && (v.size() == 5 || v[v.size() - 2] == '0');
  }
  if (v.size() > 6 && (eq(v.substr(0, 5), "rgba(") || eq(v.substr(0, 5), "hsla(")) && v.back() == ')') {
    size_t start = v.find_last_of(",/");
    if (start == std::string_view::npos) return false;
    auto alpha = trim(v.substr(start + 1, v.size() - start - 2));
    if (!alpha.empty() && alpha.back() == '%') alpha.remove_suffix(1);
    float value;
    return number(alpha, value) && value == 0;
  }
  return false;
}
inline std::optional<uint16_t> luminance(std::string_view v) {
  if (eq(v, "transparent") || eq(v, "none") || eq(v, "initial") || eq(v, "unset") || transparent(v)) return 256;
  if (eq(v, "inherit")) return 257;
  if (eq(v, "white")) return 255;
  if (eq(v, "black")) return 0;
  if (eq(v, "gray") || eq(v, "grey")) return 128;
  if (eq(v, "silver")) return 192;
  if (eq(v, "lightgray") || eq(v, "lightgrey")) return 211;
  unsigned c[3]{};
  if (v.size() == 4 && v[0] == '#') {
    for (size_t i = 0; i < 3; ++i) {
      int h = hex(v[i + 1]);
      if (h < 0) return {};
      c[i] = h * 17;
    }
  } else if ((v.size() == 7 || v.size() == 9) && v[0] == '#') {
    for (size_t i = 1; i < v.size(); ++i)
      if (hex(v[i]) < 0) return {};
    for (size_t i = 0; i < 3; ++i) c[i] = hex(v[1 + 2 * i]) * 16 + hex(v[2 + 2 * i]);
  } else if (v.size() > 5 && eq(v.substr(0, 4), "rgb(") && v.back() == ')') {
    auto args = v.substr(4, v.size() - 5);
    for (size_t i = 0; i < 3; ++i) {
      args = trim(args);
      size_t end = args.find_first_of(", \t");
      if (end == std::string_view::npos) end = args.size();
      auto part = args.substr(0, end);
      bool pct = !part.empty() && part.back() == '%';
      if (pct) part.remove_suffix(1);
      float value;
      if (!number(part, value) || value < 0 || value > (pct ? 100 : 255)) return {};
      c[i] = static_cast<unsigned>(value * (pct ? 2.55f : 1.0f) + 0.5f);
      args.remove_prefix(end);
      while (!args.empty() && (args.front() == ',' || space(args.front()))) args.remove_prefix(1);
    }
    if (!args.empty()) return {};
  } else
    return {};
  return static_cast<uint16_t>((c[0] * 299 + c[1] * 587 + c[2] * 114 + 500) / 1000);
}
// Returns true only when this extension owns the property; ordinary native CSS stays intact.
inline bool parse(std::string_view name, std::string_view value, CssStyle& s) {
  value = trim(value);
  if (eq(name, "white-space")) {
    static constexpr std::string_view names[] = {"normal", "pre", "pre-wrap", "pre-line", "nowrap", "inherit"};
    bool found = false;
    for (uint8_t i = 0; i < 6; ++i)
      if (eq(value, names[i])) {
        s.whiteSpace = static_cast<CssWhiteSpace>(i);
        found = true;
        break;
      }
    if (eq(value, "break-spaces")) {
      s.whiteSpace = CssWhiteSpace::PreWrap;
      found = true;
    }
    if (eq(value, "unset")) {
      s.whiteSpace = CssWhiteSpace::Inherit;
      found = true;
    }
    if (eq(value, "initial")) {
      s.whiteSpace = CssWhiteSpace::Normal;
      found = true;
    }
    if (found) {
      s.defined.whiteSpace = 1;
      s.preserveWhitespace = s.whiteSpace == CssWhiteSpace::Pre || s.whiteSpace == CssWhiteSpace::PreWrap;
    }
    return true;
  }
  if (eq(name, "line-height")) {
    float n;
    if (eq(value, "normal") || eq(value, "initial"))
      n = 1;
    else if (eq(value, "inherit") || eq(value, "unset"))
      n = 0;
    else {
      float divisor = 1.5f;
      if (!value.empty() && value.back() == '%') {
        value.remove_suffix(1);
        divisor *= 100;
      } else if (value.size() > 2 && eq(value.substr(value.size() - 2), "em"))
        value.remove_suffix(2);
      if (!number(value, n) || n <= 0) return true;
      n = std::clamp(n / divisor, 0.7f, 2.0f);
    }
    s.lineHeightMultiplier = n;
    s.defined.lineHeight = 1;
    return true;
  }
  if (eq(name, "min-height") || eq(name, "max-height") || eq(name, "max-width")) {
    CssLength n;
    bool minimum = eq(name, "min-height");
    if (eq(value, "inherit"))
      n = {0, CssUnit::Inherit};
    else if (eq(value, "none") || eq(value, "auto") || eq(value, "initial") || eq(value, "unset"))
      n = {0, minimum ? CssUnit::Pixels : CssUnit::Auto};
    else if (!length(value, n))
      return true;
    if (minimum) {
      s.minHeight = n;
      s.defined.minHeight = 1;
    } else if (eq(name, "max-height")) {
      s.maxHeight = n;
      s.defined.maxHeight = 1;
    } else {
      s.maxWidth = n;
      s.defined.maxWidth = 1;
    }
    return true;
  }
  if (eq(name, "float")) {
    if (eq(value, "left"))
      s.cssFloat = CssFloat::Left;
    else if (eq(value, "right"))
      s.cssFloat = CssFloat::Right;
    else if (eq(value, "none") || eq(value, "initial") || eq(value, "unset"))
      s.cssFloat = CssFloat::None;
    else
      return true;
    s.defined.cssFloat = s.defined.floatLeft = 1;
    s.floatLeft = s.cssFloat == CssFloat::Left;
    return true;
  }
  if (eq(name, "clear")) {
    static constexpr std::string_view names[] = {"none", "left", "right", "both", "inherit"};
    for (uint8_t i = 0; i < 5; ++i)
      if (eq(value, names[i])) {
        s.cssClear = static_cast<CssClear>(i);
        s.defined.cssClear = 1;
        return true;
      }
    if (eq(value, "initial") || eq(value, "unset")) {
      s.cssClear = CssClear::None;
      s.defined.cssClear = 1;
    }
    return true;
  }
  if (eq(name, "break-inside") || eq(name, "page-break-inside")) {
    if (eq(value, "avoid") || eq(value, "avoid-page") || eq(value, "auto") || eq(value, "initial") ||
        eq(value, "unset") || eq(value, "inherit")) {
      s.breakInsideAvoid = eq(value, "avoid") || eq(value, "avoid-page");
      s.breakInsideInherit = eq(value, "inherit");
      s.defined.breakInsideAvoid = 1;
    }
    return true;
  }
  if (eq(name, "color") || eq(name, "-webkit-text-fill-color")) {
    if (eq(value, "inherit") || eq(value, "unset") || eq(value, "currentcolor")) return true;
    s.colorTransparent = transparent(value);
    s.defined.colorTransparent = s.colorTransparent;
    return true;
  }
  if (eq(name, "visibility")) {
    if (eq(value, "hidden") || eq(value, "collapse")) {
      s.visibilityHidden = true;
      s.defined.visibilityHidden = 1;
    } else if (eq(value, "visible")) {
      s.visibilityHidden = false;
      s.defined.visibilityHidden = 0;
    }
    return true;
  }
  if (eq(name, "opacity")) {
    float n;
    if (number(value, n)) {
      s.opacityZero = n <= 0;
      s.defined.opacityZero = s.opacityZero;
    }
    return true;
  }
  if (eq(name, "hyphens") || eq(name, "adobe-hyphenate") || eq(name, "-webkit-hyphens") || eq(name, "-moz-hyphens") ||
      eq(name, "-epub-hyphens")) {
    if (eq(value, "none"))
      s.hyphens = CssHyphens::None;
    else if (eq(value, "auto"))
      s.hyphens = CssHyphens::Auto;
    else if (eq(value, "manual") || eq(value, "initial"))
      s.hyphens = CssHyphens::Manual;
    else if (eq(value, "inherit") || eq(value, "unset"))
      s.hyphens = CssHyphens::Inherit;
    else
      return true;
    s.defined.hyphens = 1;
    return true;
  }
  auto limitName = name;
  if (limitName.size() > 8 && eq(limitName.substr(0, 8), "-webkit-")) limitName.remove_prefix(8);
  if (limitName.size() > 5 && eq(limitName.substr(0, 5), "-moz-")) limitName.remove_prefix(5);
  if (eq(limitName, "hyphenate-limit-before") || eq(limitName, "hyphenate-limit-after") ||
      eq(limitName, "hyphenate-limit-lines")) {
    const bool lines = eq(limitName, "hyphenate-limit-lines");
    unsigned n = 0;
    if (eq(value, "inherit") || eq(value, "unset"))
      n = 255;
    else if (eq(value, "initial") || eq(value, "auto") || eq(value, "no-limit"))
      n = lines ? 0 : 2;
    else {
      if (value.empty()) return true;
      for (char c : value) {
        if (c < '0' || c > '9' || n > 25) return true;
        n = n * 10 + c - '0';
      }
      if (n > 254 || (!lines && n == 0)) return true;
    }
    if (lines) {
      s.hyphenateLimitLines = n;
      s.defined.hyphenateLimitLines = 1;
    } else if (eq(limitName, "hyphenate-limit-before")) {
      s.hyphenateLimitBefore = n;
      s.defined.hyphenateLimitBefore = 1;
    } else {
      s.hyphenateLimitAfter = n;
      s.defined.hyphenateLimitAfter = 1;
    }
    return true;
  }
  if (eq(name, "font-variant-ligatures")) {
    if (eq(value, "none") || eq(value, "no-common-ligatures"))
      s.ligatures = CssLigatures::None;
    else if (eq(value, "normal") || eq(value, "initial") || eq(value, "common-ligatures"))
      s.ligatures = CssLigatures::Normal;
    else if (eq(value, "inherit") || eq(value, "unset"))
      s.ligatures = CssLigatures::Inherit;
    else
      return true;
    s.defined.ligatures = 1;
    return true;
  }
  if (eq(name, "background") || eq(name, "background-color")) {
    auto n = luminance(value);
    if (!n && eq(name, "background")) {
      size_t start = 0;
      for (size_t i = 0; i <= value.size(); ++i)
        if (i == value.size() || space(value[i])) {
          if (auto c = luminance(value.substr(start, i - start))) n = c;
          start = i + 1;
        }
    }
    if (n) {
      s.backgroundGray = *n;
      s.defined.backgroundGray = 1;
    }  // Keep native black/shaded handling too.
  }
  if (eq(name, "background-repeat")) {
    static constexpr std::string_view names[] = {"repeat", "no-repeat", "repeat-x", "repeat-y", "inherit"};
    for (uint8_t i = 0; i < 5; ++i)
      if (eq(value, names[i])) {
        s.backgroundRepeat = static_cast<CssBackgroundRepeat>(i);
        s.defined.backgroundRepeat = 1;
        return true;
      }
    if (eq(value, "initial") || eq(value, "unset")) {
      s.backgroundRepeat = CssBackgroundRepeat::Repeat;
      s.defined.backgroundRepeat = 1;
    }
    return true;
  }
  return false;
}
}  // namespace cssExtras
