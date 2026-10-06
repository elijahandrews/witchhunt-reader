#pragma once
#include <cstdint>

// Compact annotation copied with a word through line breaking. Zero is the reader default.
// The reserved high byte must stay zero. A uniform line stores one value, not an array.
namespace wordTypography {
enum Family : uint8_t { Reader = 0, Serif = 1, SansSerif = 2, Inherit = 3 };
inline uint32_t pack(uint8_t family, int16_t tracking) {
  return (static_cast<uint32_t>(family & 3u) << 16) | static_cast<uint16_t>(tracking);
}
inline uint8_t family(uint32_t value) { return (value >> 16) & 3u; }
inline int16_t tracking(uint32_t value) { return static_cast<int16_t>(value & 0xffffu); }
}  // namespace wordTypography
