#pragma once
#include <EpdFontFamily.h>

#include <cstdint>
#include <map>
class HalScalableFont {
 public:
  static constexpr uint16_t MaxPublisherPoints64 = 8192;
  std::map<unsigned, EpdFont> descriptors;
  unsigned calls = 0;
  bool fail = false;
  const EpdFont* atSize64(uint16_t points) {
    ++calls;
    if (fail) return nullptr;
    auto& f = descriptors[points];
    f.points = points;
    return &f;
  }
  void releaseSize64(uint16_t points) { descriptors.erase(points); }
};
