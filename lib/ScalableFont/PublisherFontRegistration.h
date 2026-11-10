#pragma once
#include <cstdint>
class EpdFont;

// Publish a complete family across sizes or restore the original registry.
// Load can fail during variable-axis setup or line-metric allocation; no null
// regular face may become visible to a renderer that dereferences it.
template <typename Exists, typename Load, typename Insert, typename Remove>
bool registerPublisherFontSizes(uint8_t first, uint8_t last, Exists exists, Load load, Insert insert, Remove remove) {
  for (unsigned points = first; points <= last; ++points)
    if (exists(points)) return false;
  for (unsigned points = first; points <= last; ++points) {
    const EpdFont* styles[4] = {};
    if (!load(points, styles) || !styles[0]) {
      for (unsigned previous = first; previous < points; ++previous) remove(previous);
      return false;
    }
    insert(points, styles);
  }
  return true;
}
