#pragma once

#include <GfxRenderer.h>

#include <algorithm>
#include <array>
#include <cstdint>

// Publisher shading is ordered monochrome dither, independent of text AA.
// An opaque background replaces the pixels beneath it. Gray passes clear its
// selectors before drawing child text; opaque-ink replay must leave that later
// text alone, since the background is earlier in paint order.
inline bool publisherBoxDark(int x, int y, uint16_t gray, bool border) {
  static constexpr uint8_t thresholds[16] = {0, 128, 32, 160, 192, 64, 224, 96, 48, 176, 16, 144, 240, 112, 208, 80};
  return border || (gray < 255 && gray <= thresholds[(y & 3) * 4 + (x & 3)]);
}

inline void drawPublisherBox(GfxRenderer& renderer, int x, int y, int width, int height, uint16_t gray, uint8_t top,
                             uint8_t bottom) {
  if (renderer.isErasingOpaqueGlyphs() || renderer.isFontCacheScanning()) return;
  const bool bw = renderer.getRenderMode() == GfxRenderer::BW;
  const int firstY = std::max(0, -y), lastY = std::min(height, renderer.getScreenHeight() - y);
  const int firstX = std::max(0, -x), lastX = std::min(width, renderer.getScreenWidth() - x);
  for (int row = firstY; row < lastY; ++row) {
    for (int col = firstX; col < lastX; ++col) {
      const bool border = row < top || row >= height - bottom;
      if (gray <= 255 || border) {
        const bool dark = publisherBoxDark(x + col, y + row, gray, border);
        renderer.drawPixel(x + col, y + row, bw ? dark : true);
      }
    }
  }
}

// Image decoders flatten their resources to white but skip white pixels when
// blitting. Clear that opaque rectangle explicitly so an underlying publisher
// shade cannot show through. Keep both gray selectors clear before cache replay.
inline void clearPublisherImageBackdrop(GfxRenderer& renderer, int x, int y, int width, int height) {
  if (renderer.isErasingOpaqueGlyphs() || renderer.isFontCacheScanning()) return;
  renderer.fillRect(x, y, width, height, renderer.getRenderMode() != GfxRenderer::BW);
}

// Finalize only the visible solid background ink. Compositing a bounded row in
// paint order lets an inner white box replace its parent's dither. Replaying
// each box separately would either erase child text AA or lighten black dots.
// The enumerator also emits opaque image rectangles as white masks because an
// image replaces its underlying background. No image-sized buffer is allocated.
template <typename ForEachBox, typename Abort>
bool finalizePublisherBoxes(GfxRenderer& renderer, ForEachBox boxes, Abort abort) {
  if (!renderer.isErasingOpaqueGlyphs()) return true;
  int firstY = renderer.getScreenHeight(), lastY = 0;
  boxes([&](int x, int y, int width, int height, uint16_t gray, uint8_t top, uint8_t bottom) {
    if (width <= 0 || height <= 0 || x + width <= 0 || x >= renderer.getScreenWidth() ||
        (gray > 255 && !top && !bottom))
      return;
    firstY = std::min(firstY, std::max(0, y));
    lastY = std::max(lastY, std::min(renderer.getScreenHeight(), y + height));
  });
  constexpr int tileWidth = 1024;
  std::array<uint8_t, tileWidth / 8> row{};
  for (int y = firstY; y < lastY; ++y) {
    if (abort()) return false;
    for (int start = 0; start < renderer.getScreenWidth(); start += tileWidth) {
      const int end = std::min(renderer.getScreenWidth(), start + tileWidth);
      row.fill(0);
      boxes([&](int x, int boxY, int width, int height, uint16_t gray, uint8_t top, uint8_t bottom) {
        if (y < boxY || y >= boxY + height) return;
        const bool border = y - boxY < top || y - boxY >= height - bottom;
        if (gray > 255 && !border) return;
        for (int pixel = std::max(start, x); pixel < std::min(end, x + width); ++pixel) {
          const int bit = pixel - start;
          const uint8_t mask = 1u << (bit & 7);
          if (publisherBoxDark(pixel, y, gray, border))
            row[bit / 8] |= mask;
          else
            row[bit / 8] &= static_cast<uint8_t>(~mask);
        }
      });
      for (int pixel = start; pixel < end; ++pixel)
        if (row[(pixel - start) / 8] & (1u << ((pixel - start) & 7))) renderer.drawPixel(pixel, y, true);
    }
  }
  return true;
}
