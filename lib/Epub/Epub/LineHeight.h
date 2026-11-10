#pragma once

#include <algorithm>

// User spacing is an integer percentage. Recover it before multiplying so
// 105% of 30 pixels rounds to 32, even when 1.05f is stored slightly below 1.05.
// Share the publisher multiplier and rounding between pagination and preview.
inline int epubLineHeightPixels(const int fontHeight, const float compression, const float publisherMultiplier = 0) {
  const int percent = static_cast<int>(compression * 100.0f + 0.5f);
  const float publisher = publisherMultiplier > 0 ? publisherMultiplier : 1.0f;
  return std::max(1, static_cast<int>((fontHeight * publisher * percent) / 100.0f + 0.5f));
}
