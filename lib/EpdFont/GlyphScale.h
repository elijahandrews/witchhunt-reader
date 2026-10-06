#pragma once
#include <cmath>

// Scaled bitmap bounds in pen/baseline coordinates. Keep fractional bearing
// offsets inside the coverage filter: independently rounding a crop's bearing
// and dimensions gives every glyph a different sampling phase.
namespace glyphScale {
struct Bounds {
  int left, top, width, height;  // top is y-down, relative to the baseline
};
inline Bounds bounds(int left, int top, int width, int height, float scale) {
  if (scale == 1.0f) return {left, -top, width > 0 && height > 0 ? width : 0, width > 0 && height > 0 ? height : 0};
  const int x = static_cast<int>(std::floor(left * scale));
  const int y = static_cast<int>(std::floor(-top * scale));
  if (width <= 0 || height <= 0) return {x, y, 0, 0};
  return {x, y, static_cast<int>(std::ceil((left + width) * scale)) - x,
          static_cast<int>(std::ceil((height - top) * scale)) - y};
}
}  // namespace glyphScale
