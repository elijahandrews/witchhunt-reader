#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

namespace {
void productionCopy(unsigned char* put, unsigned char* from, unsigned copy) {
#include "PngInflateCopy.inc"
}
}  // namespace

TEST(PngCopyBounds, EveryOverlapAndShortTailStaysInsideTheRequestedSpan) {
  for (unsigned alignment = 0; alignment < 8; ++alignment)
    for (unsigned distance = 1; distance < 9; ++distance)
      for (unsigned count = 0; count < 33; ++count) {
        std::array<unsigned char, 96> actual{};
        for (unsigned i = 0; i < actual.size(); ++i) actual[i] = static_cast<unsigned char>(i * 7);
        auto expected = actual;
        const unsigned offset = 16 + alignment;
        for (unsigned i = 0; i < count; ++i) expected[offset + i] = expected[offset - distance + i];
        productionCopy(actual.data() + offset, actual.data() + offset - distance, count);
        ASSERT_EQ(actual, expected) << "alignment=" << alignment << " distance=" << distance << " count=" << count;
      }
}

TEST(PngCopyBounds, SeparateHistoryWindowCopiesWithoutPointerSubtraction) {
  std::array<unsigned char, 64> window{};
  for (unsigned i = 0; i < window.size(); ++i) window[i] = static_cast<unsigned char>(i * 3);
  for (unsigned count = 1; count < 33; ++count) {
    std::array<unsigned char, 64> actual{};
    auto expected = actual;
    std::memcpy(expected.data() + 1, window.data() + 3, count);
    productionCopy(actual.data() + 1, window.data() + 3, count);
    EXPECT_EQ(actual, expected);
  }
}
