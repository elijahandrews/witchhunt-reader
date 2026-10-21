#pragma once

#include <cstddef>
#include <cstdint>

#include "EpdFontData.h"

// A borrowed, final-display-pixel raster. The bitmap is continuous MSB-first
// 2bpp coverage (0 white .. 3 black), with no row padding. Bearings already
// include the requested fractional origin; do not scale or shift it again.
// advanceX is unused: layout keeps the original font's linear advances.
struct EpdRasterGlyph {
  EpdGlyphRef metrics{};
  const uint8_t* bitmap = nullptr;
  size_t bytes = 0;
};
