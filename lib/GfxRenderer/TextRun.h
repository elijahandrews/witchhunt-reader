#pragma once
#include <EpdFontFamily.h>
#include <SmallCaps.h>
#include <Utf8.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "GlyphScale.h"

// One glyph walk for the EPUB draw and metric paths. Tracking is in display-pixel
// sixteenths, already computed at the CSS declaration (it does not scale again).
// Scaled advances accumulate in 16.16 fixed point, retaining sub-sixteenth
// precision until drawing or measuring the final width. The callback receives baseline-relative integer
// coordinates plus a signed horizontal phase in display-pixel sixteenths.
// Coverage and bounds use that phase; advances retain the authored precision.
namespace textRun {
struct Metrics {
  int advance = 0;
  int left = 0;
  int right = 0;
  int top = 0, bottom = 0;  // baseline-relative final ink bounds
  bool hasInk = false;
};
// Inclusive source-character interval; ligatures retain the complete glyph when any
// constituent character is selected. Empty/missing glyph ranges stay not found.
struct CharacterRange {
  uint32_t first, last;
  int x = 0, left = 0, right = 0;
  bool found = false;
};
template <bool TrackRange = false, bool RasterBounds = true, typename Emit>
Metrics walk(const EpdFontFamily& font, const char* text, EpdFontFamily::Style style, float scale, int16_t tracking,
             Emit emit, CharacterRange* range = nullptr, bool ligatures = true) {
  Metrics result;
  // Metrics and tracking arrive in sixteenths. Keep twelve more fractional bits
  // while summing: density-2 odd advances otherwise round up at every glyph.
  constexpr int64_t metricToPen = 1 << 12;
  const auto pixel = [](int64_t pen) { return static_cast<int>((pen + (1 << 15)) >> 16); };
  int64_t cursor = 0;
  uint32_t previous = 0;
  int lastX = 0, lastLeft = 0, lastWidth = 0, lastTop = 0;
  int8_t lastPhase = 0;
  const bool fractional = font.getData(style)->is2Bit;
  uint32_t character = 0;
  uint32_t cp;
  const char* glyphStart = text;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    const bool combining = utf8IsCombiningMark(cp);
    if (ligatures && !combining && tracking == 0) cp = font.applyLigatures(cp, text, style);
    uint32_t firstCharacter = 0;
    if constexpr (TrackRange) {
      firstCharacter = character;
      for (const char* p = glyphStart; p < text; ++p)
        if ((static_cast<uint8_t>(*p) & 0xc0) != 0x80) ++character;
      glyphStart = text;
    }
    float capsScale = 1.0f;
    if (!combining) cp = font.resolveCaps(cp, style, capsScale);
    const bool folded = capsScale != 1.0f;
    const float glyphScale = scale * capsScale;
    const float penScale = glyphScale * metricToPen;
    const auto glyph = font.getGlyph(cp, style);
    if (!glyph) {
      if (!combining) previous = 0;
      continue;
    }
    int x, y = 0;
    int8_t phase;
    if (combining) {
      phase = lastPhase;
      x = lastX;
    } else {
      if (previous) cursor += tracking * metricToPen + std::llround(font.getKerning(previous, cp, style) * penScale);
      const int64_t phasePen = (cursor + metricToPen / 2) >> 12;
      x = fractional ? static_cast<int>((phasePen + 8) >> 4) : pixel(cursor);
      phase = fractional ? static_cast<int8_t>(phasePen - static_cast<int64_t>(x) * 16) : 0;
      cursor += std::llround(glyph.advanceX * penScale);
      previous = cp;
    }
    auto bounds = glyphScale::bounds(glyph.left, glyph.top, glyph.width, glyph.height, glyphScale, phase);
    auto positioning = glyphScale::bounds(glyph.left, glyph.top, glyph.width, glyph.height, glyphScale);
    if constexpr (RasterBounds) {
      EpdRasterGlyph raster;
      if (font.rasterGlyph(cp, style, glyphScale, phase, raster)) {
        const auto& ink = raster.metrics;
        bounds = {ink.left, -ink.top, ink.width, ink.height};
        positioning = bounds;
      }
    }
    if (combining) {
      x += lastLeft + (lastWidth - positioning.width) / 2 - positioning.left;
      const int bottom = -positioning.top - positioning.height;
      if (bottom < lastTop) y -= lastTop - bottom;
    } else {
      lastPhase = phase;
      lastX = x;
      lastLeft = positioning.left;
      lastWidth = positioning.width;
      lastTop = -positioning.top;
    }
    emit(cp, x, y, glyphScale, folded, phase);
    if constexpr (TrackRange) {
      if (range && firstCharacter <= range->last && character > range->first) {
        const int a = std::min(x, x + bounds.left);
        const int b = std::max<int>(x + bounds.left + bounds.width, pixel(cursor));
        if (!range->found) range->x = x;
        range->left = range->found ? std::min(range->left, a) : a;
        range->right = range->found ? std::max(range->right, b) : b;
        range->found = true;
      }
    }
    if (bounds.width > 0 && bounds.height > 0) {
      const int a = x + bounds.left, b = a + bounds.width;
      result.left = result.hasInk ? std::min(result.left, a) : a;
      result.right = result.hasInk ? std::max(result.right, b) : b;
      const int top = y + bounds.top, bottom = top + bounds.height;
      result.top = result.hasInk ? std::min(result.top, top) : top;
      result.bottom = result.hasInk ? std::max(result.bottom, bottom) : bottom;
      result.hasInk = true;
    }
  }
  result.advance = std::max(0, pixel(cursor));
  return result;
}
}  // namespace textRun
