#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifndef FREEINK_FONT_ENABLE_CFF
#define FREEINK_FONT_ENABLE_CFF 0
#endif

namespace truetype {
enum class Result { Supported, Unsupported, Unavailable };
// OpenType CFF needs the SDK's opt-in FreeType CFF driver.
constexpr bool kCffEnabled = FREEINK_FONT_ENABLE_CFF;
inline uint32_t u32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
// Bounded preflight, before FreeType or large allocations. No font-data copy:
// one 16-byte record on the stack, checked offsets, no unaligned integer reads.
// This enforces app policy; FreeType still validates the face and Unicode cmap.
// Accepts TrueType and opt-in CFF/CFF2; variable faces have at most 16 axes.
template <typename Read>
Result inspect(size_t size, Read read) {
  if (size < 12) return Result::Unsupported;
  uint8_t record[16];
  if (!read(0, record, 12)) return Result::Unavailable;
  const bool cff = std::memcmp(record, "OTTO", 4) == 0;
  if (cff ? !kCffEnabled : (u32(record) != 0x00010000 && std::memcmp(record, "true", 4) != 0))
    return Result::Unsupported;  // Includes TTC, WOFF/WOFF2, and OTTO without the CFF driver.
  const unsigned tables = (unsigned(record[4]) << 8) | record[5];
  const size_t directoryEnd = 12 + size_t(tables) * 16;
  if (!tables || directoryEnd > size) return Result::Unsupported;
  uint32_t previous = 0;
  unsigned required = 0;
  bool variable = false, gvar = false;
  for (unsigned i = 0; i < tables; ++i) {
    if (!read(12 + size_t(i) * 16, record, sizeof(record))) return Result::Unavailable;
    const uint32_t tag = u32(record);
    const size_t offset = u32(record + 8), length = u32(record + 12);
    // SFNT records must be sorted by tag. This also rejects duplicate tables.
    if (tag <= previous || offset < directoryEnd || offset > size || length > size - offset) return Result::Unsupported;
    previous = tag;
    switch (tag) {
      case 0x66766172: {
        uint8_t header[16];
        if (length < sizeof(header)) return Result::Unsupported;
        if (!read(offset, header, sizeof(header))) return Result::Unavailable;
        const auto u16 = [](const uint8_t* p) { return unsigned(p[0]) * 256 + p[1]; };
        const unsigned axesOffset = u16(header + 4), axes = u16(header + 8), axisSize = u16(header + 10);
        const unsigned instances = u16(header + 12), instanceSize = u16(header + 14);
        if (u32(header) != 0x10000 || u16(header + 6) != 2 || axesOffset < 16 || !axes || axes > 16 || axisSize < 20 ||
            instanceSize < 4 + axes * 4 ||
            uint64_t(axesOffset) + uint64_t(axes) * axisSize + uint64_t(instances) * instanceSize > length)
          return Result::Unsupported;
        variable = true;
        break;
      }
      case 0x67766172:
        gvar = true;
        break;
      case 0x43464632: {
        uint8_t header[5];
        if (!cff || length < sizeof(header)) return Result::Unsupported;
        if (!read(offset, header, sizeof(header))) return Result::Unavailable;
        const unsigned dictSize = unsigned(header[3]) * 256 + header[4];
        if (header[0] != 2 || header[2] < 5 || uint64_t(header[2]) + dictSize > length) return Result::Unsupported;
        required |= 128;
        break;
      }
      case 0x43464620:
        if (!length) return Result::Unsupported;
        required |= 128;
        break;  // CFF
      case 0x676c7966:
        if (!length) return Result::Unsupported;
        required |= 1;
        break;  // glyf
      case 0x6c6f6361:
        if (length < 4) return Result::Unsupported;
        required |= 2;
        break;  // loca
      case 0x68656164:
        if (length < 54) return Result::Unsupported;
        required |= 4;
        break;  // head
      case 0x6d617870:
        // CFF faces use the 6-byte maxp version 0.5; TrueType needs version 1.0.
        if (length < (cff ? 6u : 32u)) return Result::Unsupported;
        required |= 8;
        break;  // maxp
      case 0x636d6170:
        if (length < 4) return Result::Unsupported;
        required |= 16;
        break;  // cmap
      case 0x68686561:
        if (length < 36) return Result::Unsupported;
        required |= 32;
        break;  // hhea
      case 0x686d7478:
        if (length < 4) return Result::Unsupported;
        required |= 64;
        break;  // hmtx
    }
  }
  // CFF: CFF, head, maxp, cmap, hhea, hmtx. TrueType: glyf, loca, head, maxp, cmap, hhea, hmtx.
  const unsigned needed = cff ? 128 | 124 : 127;
  return (required & needed) == needed && (!gvar || variable) ? Result::Supported : Result::Unsupported;
}
}  // namespace truetype
