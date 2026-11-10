#pragma once
#include <algorithm>
#include <cstdint>

#include "css/CssStyle.h"

namespace publisherBox {
constexpr uint16_t Transparent = 256;
constexpr uint8_t MaxLayer = 7;
inline bool edgeInk(const CssBorderSide& side, int depth, int along) {
  if (!side.visible() || depth < 0 || depth >= side.width || along < 0) return false;
  if (side.style == CssBorderStyle::Double && side.width >= 3) {
    const int line = std::max(1, side.width / 3);
    return depth < line || depth >= side.width - line;
  }
  if (side.style == CssBorderStyle::Dotted || side.style == CssBorderStyle::Dashed) {
    const int dash = side.style == CssBorderStyle::Dotted ? side.width : std::max(3, side.width * 3);
    const int gap = side.style == CssBorderStyle::Dotted ? std::max(2, int(side.width)) : std::max(2, side.width * 2);
    return along % (dash + gap) < dash;
  }
  return true;
}
// True replaces the pixel; false leaves its ancestor untouched. Explicit white
// is opaque, while transparent boxes paint only their borders.
inline bool pixel(int x, int y, int width, int height, int screenX, int screenY, uint16_t gray, bool legacyShaded,
                  const CssBorderSide (&sides)[4], bool& dark) {
  if (x < 0 || y < 0 || x >= width || y >= height) return false;
  const bool border = edgeInk(sides[0], y, x) || edgeInk(sides[1], width - 1 - x, y) ||
                      edgeInk(sides[2], height - 1 - y, x) || edgeInk(sides[3], x, y);
  if (border) {
    dark = true;
    return true;
  }
  if (gray <= 255) {
    static constexpr uint8_t thresholds[16] = {0, 128, 32, 160, 192, 64, 224, 96, 48, 176, 16, 144, 240, 112, 208, 80};
    dark = gray < 255 && gray <= thresholds[(screenY & 3) * 4 + (screenX & 3)];
    return true;
  }
  if (legacyShaded && !(x & 1) && !(y & 1)) {
    dark = true;
    return true;
  }
  return false;
}
}  // namespace publisherBox
