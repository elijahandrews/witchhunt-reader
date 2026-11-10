#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "EpdFontData.h"

// V5/V6 retain the V4 section layout. Metrics are in raster pixels; density
// converts them back to the logical point size advertised by the font catalog.
namespace cpfont {
constexpr size_t HEADER_SIZE = 32;
inline uint32_t u32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline uint8_t rasterDensity(const uint8_t* header) {
  const unsigned version = header[8] | (unsigned(header[9]) << 8);
  if (version == 4) return 1;
  if ((version != 5 && version != 6) || header[10] != 1 || header[11] != 0 || header[13] != 2) return 0;
  for (size_t i = 18; i < HEADER_SIZE; ++i)
    if (header[i]) return 0;
  return 2;
}
// Streaming standard CRC32, matching zlib.crc32. V5 stores the final CRC of
// bytes 32..EOF at offset14 so regenerated bitmaps also change the font ID.
// A size switch validates several MiB of HD glyphs. Keep the 1 KiB table in
// read-only storage and consume a byte per step instead of eight bit steps.
inline constexpr auto CRC32_TABLE = [] {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < table.size(); ++i) {
    uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    table[i] = crc;
  }
  return table;
}();
inline uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t size) {
  for (size_t i = 0; i < size; ++i) crc = (crc >> 8) ^ CRC32_TABLE[(crc ^ data[i]) & 0xFFu];
  return crc;
}
// Density-2 files are adapted once, when entering CrossInk's native-size cache.
// Keep bearings in the common baseline grid: cropping must not shift coverage.
inline unsigned width(const EpdGlyph& g) { return g.width | (unsigned(reinterpret_cast<const uint8_t*>(&g)[10]) << 8); }
inline unsigned height(const EpdGlyph& g) {
  return g.height | (unsigned(reinterpret_cast<const uint8_t*>(&g)[11]) << 8);
}
inline int floorHalf(int n) { return n >= 0 ? n / 2 : -((-n + 1) / 2); }
inline int ceilHalf(int n) { return -floorHalf(-n); }
inline int halfEven(int n) {
  const int lower = floorHalf(n);
  return lower + ((n - lower * 2) && (lower & 1));
}
inline bool logicalGlyph(const EpdGlyph& raw, EpdGlyph& logical) {
  const int left = floorHalf(raw.left), top = ceilHalf(raw.top);
  const int w = width(raw) ? ceilHalf(raw.left + int(width(raw))) - left : 0;
  const int h = height(raw) ? top - floorHalf(raw.top - int(height(raw))) : 0;
  if (w > UINT8_MAX || h > UINT8_MAX) return false;
  logical = {uint8_t(w),    uint8_t(h),   uint16_t(raw.advanceX / 2),
             int16_t(left), int16_t(top), uint16_t((w * h + 3) / 4),
             raw.dataOffset};
  logical.advanceRemainder = raw.advanceX & 1;
  return true;
}
inline void logicalBitmap(const EpdGlyph& raw, const EpdGlyph& logical, const uint8_t* source, uint8_t* dest) {
  if (!logical.dataLength) return;
  std::memset(dest, 0, logical.dataLength);
  const int w = width(raw), h = height(raw);
  for (int y = 0; y < logical.height; ++y) {
    for (int x = 0; x < logical.width; ++x) {
      const int sx = (logical.left + x) * 2 - raw.left;
      const int sy = raw.top - (logical.top - y) * 2;
      unsigned sum = 0;
      for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
          if (sx + dx < 0 || sx + dx >= w || sy + dy < 0 || sy + dy >= h) continue;
          const size_t i = size_t(sy + dy) * w + sx + dx;
          sum += (source[i / 4] >> (6 - 2 * (i % 4))) & 3;
        }
      // One local nearest-even quantization; never diffuse error into adjacent strokes.
      const unsigned lower = sum / 4, rem = sum % 4;
      const unsigned shade = lower + (rem > 2 || (rem == 2 && (lower & 1)));
      const size_t i = size_t(y) * logical.width + x;
      dest[i / 4] |= shade << (6 - 2 * (i % 4));
    }
  }
}
}  // namespace cpfont
