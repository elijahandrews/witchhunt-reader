#include <CpFontFormat.h>
#include <gtest/gtest.h>

#include <array>

namespace {
uint32_t bitwiseCrc(uint32_t crc, const uint8_t* data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}
}  // namespace

TEST(CpFontCrc, StandardCheckValueAndEmptyInput) {
  const uint8_t text[] = "123456789";
  EXPECT_EQ(cpfont::crc32Update(0xFFFFFFFFu, text, 9) ^ 0xFFFFFFFFu, 0xCBF43926u);
  EXPECT_EQ(cpfont::crc32Update(0x12345678u, nullptr, 0), 0x12345678u);
}

TEST(CpFontCrc, UnalignedAndChunkedUpdatesMatchIndependentBitwiseReference) {
  std::array<uint8_t, 8195> data;
  uint32_t random = 0x5369676Eu;
  for (auto& byte : data) {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    byte = static_cast<uint8_t>(random);
  }
  for (size_t offset : {0u, 1u, 2u, 3u}) {
    for (uint32_t seed : {0u, 0xFFFFFFFFu, 0x13579BDFu}) {
      const auto* bytes = data.data() + offset;
      const size_t size = data.size() - offset;
      const uint32_t expected = bitwiseCrc(seed, bytes, size);
      EXPECT_EQ(cpfont::crc32Update(seed, bytes, size), expected);
      for (size_t chunk : {1u, 127u, 128u, 511u, 512u, 4096u}) {
        uint32_t crc = seed;
        for (size_t pos = 0; pos < size; pos += chunk)
          crc = cpfont::crc32Update(crc, bytes + pos, std::min(chunk, size - pos));
        EXPECT_EQ(crc, expected) << offset << ":" << chunk;
      }
    }
  }
}
