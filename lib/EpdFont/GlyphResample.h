#pragma once
#include <algorithm>
#include <cstdint>
template <typename Emit>
static inline void emitScaledGlyphPixels(const uint8_t* const bitmap, const bool is2Bit, const int srcW, const int srcH,
                                         const int dstW, const int dstH, const float scale, const int64_t srcXOriginFP,
                                         const int64_t srcYOriginFP, const uint8_t drawMask, Emit&& emit) {
  // The ESP32-C3 has no FPU — every float op in a per-pixel loop is a soft-float call
  // costing hundreds of cycles. The resampler therefore runs in 16.16 fixed point:
  // the ONLY float operation per glyph is the one-time conversion of `scale` into a
  // fixed-point step; the pixel loops are pure integer (the C3's M extension gives a
  // hardware divider for the one division per destination pixel).
  constexpr int FP_SHIFT = 16;
  constexpr int32_t FP_ONE = 1 << FP_SHIFT;

  if (!bitmap || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0 || !(scale > 0)) return;
  // Even a completely black source cannot reach the first coverage threshold
  // at these tiny sizes. Leave a factor-of-two margin for fixed-point rounding.
  // Besides skipping invisible work, this bounds the area products below for
  // native 255px bitmaps and their validated <=510px HD source rasters.
  if (12.0f * srcW * srcH * scale * scale < 1.0f) {
    if (drawMask & 1)
      for (int y = 0; y < dstH; ++y)
        for (int x = 0; x < dstW; ++x) emit(x, y, 0);
    return;
  }

  // Per-source-pixel raw ink level: 2-bit fonts carry 4 AA levels (0..3); 1-bit fonts map
  // to 0 or 3. Used by the coverage integrator below.
  auto srcRaw = [&](const int sx, const int sy) -> uint8_t {
    const int pos = sy * srcW + sx;
    if (is2Bit) {
      const uint8_t byte = bitmap[pos >> 2];
      return (byte >> ((3 - (pos & 3)) * 2)) & 0x3;
    }
    const uint8_t byte = bitmap[pos >> 3];
    return ((byte >> (7 - (pos & 7))) & 1) ? 3 : 0;
  };

  // At native bitmap size a horizontal phase only mixes two neighboring
  // columns. Preserve the same coverage/tie rule without the general 64-bit
  // area loop for every character of ordinary body text.
  if (scale == 1.0f && srcYOriginFP == 0) {
    const int first = static_cast<int>(srcXOriginFP >> 20);
    const int fraction = static_cast<int>((srcXOriginFP - static_cast<int64_t>(first) * (1 << 20)) >> 16);
    for (int y = 0; y < dstH; ++y) {
      for (int x = 0; x < dstW; ++x) {
        const int sx = first + x;
        const int a = sx >= 0 && sx < srcW && y < srcH ? srcRaw(sx, y) : 0;
        const int b = sx + 1 >= 0 && sx + 1 < srcW && y < srcH ? srcRaw(sx + 1, y) : 0;
        const int value = a * (16 - fraction) + b * fraction;
        const int lower = value >> 4, remainder = value & 15;
        const uint8_t raw = lower + (remainder > 8 || (remainder == 8 && (lower & 1)));
        if ((drawMask >> raw) & 1) emit(x, y, raw);
      }
    }
    return;
  }

  // Integrate ALL source pixels covered by each destination pixel. Point sampling
  // while shrinking skipped entire thin stems/serifs (notably 75% uppercase and
  // small caps). Preserve their fractional ink as 2-bit coverage instead of
  // thresholding it away. This also gives reduced text the same grayscale capture,
  // staged AA, and darkness behavior as unscaled and enlarged text.
  //
  // Products and area use 64 bits because a destination pixel
  // covers more than one source pixel when shrinking. Out-of-bitmap area stays
  // transparent, just as in the enlarged-glyph path.
  // Keep four extra horizontal fractional bits: rounding phase*step/16
  // before integration can move an exact half-coverage tie to the wrong shade.
  // Vertical coordinates remain 16.16; horizontal coordinates are 12.20.
  const int64_t invScaleFP = static_cast<int64_t>(FP_ONE / scale + 0.5f);
  constexpr int X_SHIFT = FP_SHIFT + 4;
  const int64_t xStep = static_cast<int64_t>(invScaleFP) * 16;
  const int64_t dstPixelAreaFP = xStep * invScaleFP;
  int64_t srcY0FP = srcYOriginFP;
  for (int dstY = 0; dstY < dstH; dstY++, srcY0FP += invScaleFP) {
    const int64_t srcY1FP = srcY0FP + invScaleFP;
    const int sy0 = std::max(0, static_cast<int>(srcY0FP >> FP_SHIFT));
    const int sy1 = std::min(static_cast<int>((srcY1FP - 1) >> FP_SHIFT), srcH - 1);
    int64_t srcX0FP = srcXOriginFP;
    for (int dstX = 0; dstX < dstW; dstX++, srcX0FP += xStep) {
      const int64_t srcX1FP = srcX0FP + xStep;
      const int sx0 = std::max(0, static_cast<int>(srcX0FP >> X_SHIFT));
      const int sx1 = std::min(static_cast<int>((srcX1FP - 1) >> X_SHIFT), srcW - 1);

      int64_t covered = 0;
      for (int sy = sy0; sy <= sy1; sy++) {
        const int64_t hOverlapFP = std::min(srcY1FP, static_cast<int64_t>(sy + 1) << FP_SHIFT) -
                                   std::max(srcY0FP, static_cast<int64_t>(sy) << FP_SHIFT);
        if (hOverlapFP <= 0) continue;
        for (int sx = sx0; sx <= sx1; sx++) {
          const uint8_t raw = srcRaw(sx, sy);
          if (raw == 0) continue;
          const int64_t wOverlapFP = std::min(srcX1FP, static_cast<int64_t>(sx + 1) << X_SHIFT) -
                                     std::max(srcX0FP, static_cast<int64_t>(sx) << X_SHIFT);
          if (wOverlapFP <= 0) continue;
          covered += static_cast<int64_t>(raw) * hOverlapFP * wOverlapFP;
        }
      }
      // covered / dstPixelArea is 3x the ink fraction (raw already carries the x3 of the
      // source encoding). Exact half levels occur frequently when reducing an HD
      // font's already-quantized 2-bit raster. Always rounding them upward adds
      // extra ink along stems. Use nearest-even ties without another division;
      // non-ties retain the same nearest-level result and every draw path shares it.
      int64_t rounded = (covered + dstPixelAreaFP / 2) / dstPixelAreaFP;
      if ((rounded & 1) && !(dstPixelAreaFP & 1) && rounded * dstPixelAreaFP - covered == dstPixelAreaFP / 2) {
        --rounded;
      }
      const uint8_t raw = static_cast<uint8_t>(std::min<int64_t>(3, rounded));
      if ((drawMask >> raw) & 0x01) {
        emit(dstX, dstY, raw);
      }
    }
  }
}
