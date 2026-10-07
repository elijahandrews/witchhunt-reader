#pragma once
#include <cstdint>

// Compact annotation copied with a word through line breaking. Zero is the reader default.
// Bits 0..15 hold tracking, 16..23 the family, and bit 24 all-small-caps.
// A uniform line stores one value, not an array. Bits 25..31 remain reserved.
namespace wordTypography {
inline constexpr uint32_t ALL_SMALL_CAPS = 1u << 24;
enum Family : uint8_t { Reader = 0, Serif = 1, SansSerif = 2, Inherit = 3, Monospace = 4 };
inline uint32_t pack(uint8_t family, int16_t tracking) {
  return (static_cast<uint32_t>(family) << 16) | static_cast<uint16_t>(tracking);
}
inline uint8_t family(uint32_t value) { return (value >> 16) & 0xffu; }
inline int16_t tracking(uint32_t value) { return static_cast<int16_t>(value & 0xffffu); }
}  // namespace wordTypography
