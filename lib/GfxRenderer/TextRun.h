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
// then scales a second time. The callback receives baseline-relative integer
// coordinates plus a signed horizontal phase in display-pixel sixteenths.
// Coverage and bounds use that phase; advances retain the authored precision.
namespace textRun {
struct Metrics {
  int advance = 0;
  int left = 0;
  int right = 0;
};
// Inclusive source-character interval; ligatures retain the complete glyph when any
// constituent character is selected. Empty/missing glyph ranges stay not found.
struct CharacterRange {
  uint32_t first, last;
  int x = 0, left = 0, right = 0;
  bool found = false;
};
template <bool TrackRange = false, typename Emit>
Metrics walk(const EpdFontFamily& font, const char* text, EpdFontFamily::Style style, float scale, int16_t tracking,
             Emit emit, CharacterRange* range = nullptr, bool ligatures = true) {
  Metrics result;
  int32_t cursor = 0;
  uint32_t previous = 0;
  int lastX = 0, lastLeft = 0, lastWidth = 0, lastTop = 0;
  int8_t lastPhase = 0;
  const bool fractional = font.getData(style)->is2Bit;
  bool hasInk = false;
  const auto round = [](float value) { return static_cast<int>(std::lround(value)); };
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
    const auto glyph = font.getGlyph(cp, style);
    if (!glyph) {
      if (!combining) previous = 0;
      continue;
    }
    auto bounds = glyphScale::bounds(glyph.left, glyph.top, glyph.width, glyph.height, glyphScale);
    const int left = bounds.left, width = bounds.width, top = -bounds.top;
    int x, y = 0;
    int8_t phase = 0;
    if (combining) {
      x = lastX + lastLeft + (lastWidth - width) / 2 - left;
      phase = lastPhase;
      const int height = bounds.height;
      if (top - height < lastTop) y -= lastTop - (top - height);
    } else {
      if (previous) cursor += tracking + round(font.getKerning(previous, cp, style) * glyphScale);
      x = (cursor + 8) >> 4;
      phase = fractional ? static_cast<int8_t>(cursor - x * 16) : 0;
      lastPhase = phase;
      lastX = x;
      lastLeft = left;
      lastWidth = width;
      lastTop = top;
      cursor += round(glyph.advanceX * glyphScale);
      previous = cp;
    }
    if (phase) bounds = glyphScale::bounds(glyph.left, glyph.top, glyph.width, glyph.height, glyphScale, phase);
    emit(cp, x, y, glyphScale, folded, phase);
    if constexpr (TrackRange) {
      if (range && firstCharacter <= range->last && character > range->first) {
        const int a = std::min(x, x + bounds.left);
        const int b = std::max<int>(x + bounds.left + bounds.width, (cursor + 8) >> 4);
        if (!range->found) range->x = x;
        range->left = range->found ? std::min(range->left, a) : a;
        range->right = range->found ? std::max(range->right, b) : b;
        range->found = true;
      }
    }
    if (width > 0 && glyph.height > 0) {
      const int a = x + bounds.left, b = a + bounds.width;
      result.left = hasInk ? std::min(result.left, a) : a;
      result.right = hasInk ? std::max(result.right, b) : b;
      hasInk = true;
    }
  }
  result.advance = std::max<int32_t>(0, (cursor + 8) >> 4);
  return result;
}
}  // namespace textRun
