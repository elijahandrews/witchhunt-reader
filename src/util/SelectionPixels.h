#pragma once
#include <algorithm>
#include <cstdint>

// Invert the union of selection rectangles in the existing framebuffer. Reading
// original pixels preserves glyph phase, tracking and ligatures exactly; a union
// prevents overlapping word padding from being inverted twice. Row tiles use a
// bounded 64-byte stack mask, independent of page length and screen orientation.
namespace selectionPixels {
struct Rect {
  int x, y, width, height;
};
template <typename Renderer, typename Rectangles>
void invert(Renderer& renderer, Rectangles forEachRect) {
  const int width = renderer.getScreenWidth(), height = renderer.getScreenHeight();
  int top = height, bottom = 0;
  forEachRect([&](Rect r) {
    if (r.width <= 0 || r.height <= 0) return;
    top = std::min(top, std::max(0, r.y));
    bottom = std::max(bottom, std::min(height, r.y + r.height));
  });
  for (int y = top; y < bottom; ++y) {
    for (int tile = 0; tile < width; tile += 512) {
      uint8_t mask[64] = {};
      const int end = std::min(width, tile + 512);
      forEachRect([&](Rect r) {
        if (r.height <= 0 || y < r.y || y >= r.y + r.height) return;
        for (int x = std::max(tile, r.x); x < std::min(end, r.x + r.width); ++x)
          mask[(x - tile) / 8] |= static_cast<uint8_t>(1u << ((x - tile) % 8));
      });
      for (int x = tile; x < end; ++x)
        if (mask[(x - tile) / 8] & (1u << ((x - tile) % 8))) renderer.drawPixel(x, y, !renderer.getPixel(x, y));
    }
  }
}
}  // namespace selectionPixels
