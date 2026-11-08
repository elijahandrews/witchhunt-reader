#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

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
}  // namespace cpfont
