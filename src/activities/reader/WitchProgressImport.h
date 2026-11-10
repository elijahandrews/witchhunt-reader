#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Witch Hunt's unversioned prefix is NOT CrossInk's 10-byte visible-text
// record. Keep decoding explicit and confined to the read-only legacy path.
namespace witch_progress {
constexpr size_t MAX_BYTES = 39;
struct Position {
  uint16_t spine = 0;
  uint16_t page = 0;
  uint16_t pageCount = 0;
  uint16_t paragraph = UINT16_MAX;
  uint32_t sourceOffset = UINT32_MAX;
  uint16_t sourceCharacter = 0;
};
inline uint16_t read16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline bool decode(const uint8_t* data, const size_t size, Position& out) {
  if (!data || size < 4 || size == 5 || size > MAX_BYTES) return false;
  // Byte 6 is percent, never the first byte of a text offset.
  if (size >= 7 && data[6] > 100) return false;
  Position decoded;
  decoded.spine = read16(data);
  decoded.page = read16(data + 2);
  if (size >= 6) decoded.pageCount = read16(data + 4);
  if (size >= 9 && read16(data + 7) != 0) decoded.paragraph = read16(data + 7);
  if (size >= 14 && data[13] <= 15) {
    const size_t tail = 14 + data[13];
    if (size >= tail + 10 && std::memcmp(data + tail, "SRC1", 4) == 0) {
      const uint8_t* p = data + tail + 4;
      decoded.sourceOffset = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                             (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
      if (decoded.sourceOffset != UINT32_MAX) decoded.sourceCharacter = read16(p + 4);
    }
  }
  out = decoded;
  return true;
}
}  // namespace witch_progress
