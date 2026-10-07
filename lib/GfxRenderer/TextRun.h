#pragma once
#include <EpdFontFamily.h>
#include <SmallCaps.h>
#include <Utf8.h>

#include <algorithm>
#include <cmath>

#include "GlyphScale.h"

// One glyph walk for the EPUB draw and metric paths. Tracking is in display-pixel
// sixteenths, already computed at the CSS declaration (it does not scale again).
// All positions accumulate in fixed point; measurement never rounds per word and
// then scales a second time. The callback receives baseline-relative coordinates.
namespace textRun {
struct Metrics {
  int advance = 0;
  int left = 0;
  int right = 0;
};
template <typename Emit>
Metrics walk(const EpdFontFamily& font, const char* text, EpdFontFamily::Style style, float scale, int16_t tracking,
             Emit emit) {
  Metrics result;
  int32_t cursor = 0;
  uint32_t previous = 0;
  int lastX = 0, lastLeft = 0, lastWidth = 0, lastTop = 0;
  bool hasInk = false;
  const auto round = [](float value) { return static_cast<int>(std::lround(value)); };
  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    const bool combining = utf8IsCombiningMark(cp);
    if (!combining && tracking == 0) cp = font.applyLigatures(cp, text, style);
    float capsScale = 1.0f;
    if (!combining) cp = font.resolveCaps(cp, style, capsScale);
    const bool folded = capsScale != 1.0f;
    const float glyphScale = scale * capsScale;
    const auto glyph = font.getGlyph(cp, style);
    if (!glyph) {
      if (!combining) previous = 0;
      continue;
    }
    const auto bounds = glyphScale::bounds(glyph.left, glyph.top, glyph.width, glyph.height, glyphScale);
    const int left = bounds.left, width = bounds.width, top = -bounds.top;
    int x, y = 0;
    if (combining) {
      x = lastX + lastLeft + (lastWidth - width) / 2 - left;
      const int height = bounds.height;
      if (top - height < lastTop) y -= lastTop - (top - height);
    } else {
      if (previous) cursor += tracking + round(font.getKerning(previous, cp, style) * glyphScale);
      x = (cursor + 8) >> 4;
      lastX = x;
      lastLeft = left;
      lastWidth = width;
      lastTop = top;
      cursor += round(glyph.advanceX * glyphScale);
      previous = cp;
    }
    emit(cp, x, y, glyphScale, folded);
    if (width > 0 && glyph.height > 0) {
      const int a = x + left, b = a + width;
      result.left = hasInk ? std::min(result.left, a) : a;
      result.right = hasInk ? std::max(result.right, b) : b;
      hasInk = true;
    }
  }
  result.advance = std::max<int32_t>(0, (cursor + 8) >> 4);
  return result;
}
}  // namespace textRun
