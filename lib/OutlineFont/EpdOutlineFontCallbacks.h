#pragma once

#include <cstddef>
#include <cstdint>

#include "EpdFontData.h"

// All metrics are in raster pixels (density 2), advances/kerning in 1/16 raster pixel.
// gid zero means missing. Caps modes: 0 normal cmap, 1 smcp, 2 c2sc.
// Caps callbacks return zero when the requested feature has no substitution.
// Bitmap is continuous MSB-first 2bpp coverage: 0 white .. 3 black. The pointer
// remains valid until another bitmap request or destruction of the owning face.
struct EpdOutlineFontCallbacks {
  uint16_t (*glyphId)(void* ctx, uint32_t unicodeCp, uint8_t capsMode);
  EpdGlyphRef (*metrics)(void* ctx, uint16_t gid);
  const uint8_t* (*bitmap)(void* ctx, uint16_t gid, size_t* bytes);
  int16_t (*kerning)(void* ctx, uint16_t left, uint16_t right);
  uint16_t (*ligature)(void* ctx, const uint16_t* gids, size_t count);
};
