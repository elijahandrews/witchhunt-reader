#pragma once
#include <cstddef>
#include <cstdint>

// V5 retains the V4 section layout. Metrics are in raster pixels; density
// converts them back to the logical point size advertised by the font catalog.
namespace cpfont {
constexpr size_t HEADER_SIZE = 32;
inline uint32_t u32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline uint8_t rasterDensity(const uint8_t* header) {
  const unsigned version = header[8] | (unsigned(header[9]) << 8);
  if (version == 4) return 1;
  if (version != 5 || header[10] != 1 || header[11] != 0 || header[13] != 2) return 0;
  for (size_t i = 18; i < HEADER_SIZE; ++i)
    if (header[i]) return 0;
  return 2;
}
// Streaming standard CRC32, matching zlib.crc32. V5 stores the final CRC of
// bytes 32..EOF at offset14 so regenerated bitmaps also change the font ID.
inline uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}
}  // namespace cpfont
