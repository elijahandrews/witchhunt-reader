#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <unordered_set>

#include "PreparedGrayscaleCache.h"
#include "SingleRefreshGrayscale.h"

namespace {
class PreparedGrayscaleCacheTest : public testing::Test {
 protected:
  static inline std::unordered_set<void*> allocations;
  static inline int calls = 0, failCall = -1;
  void SetUp() override {
    ASSERT_TRUE(allocations.empty());
    calls = 0;
    failCall = -1;
  }
  void TearDown() override { EXPECT_TRUE(allocations.empty()); }
  static void* allocate(size_t bytes) {
    if (++calls == failCall) return nullptr;
    void* memory = std::malloc(bytes);
    if (memory) allocations.insert(memory);
    return memory;
  }
  static void free(void* memory) {
    EXPECT_EQ(allocations.erase(memory), 1u);
    std::free(memory);
  }
  static PreparedGrayscaleCache::Key key() { return {nullptr, 2, 7, 11, 12, 24, 0, 1}; }
};

TEST_F(PreparedGrayscaleCacheTest, ReusesBothPlanesAndClearsPreviousPixels) {
  PreparedGrayscaleCache cache;
  auto token = cache.begin(48000, key(), allocate, free);
  ASSERT_NE(token, 0u);
  ASSERT_NE(cache.lsb(), cache.msb());
  cache.lsb()[47999] = 0xff;
  cache.msb()[1234] = 0xaa;
  ASSERT_TRUE(cache.commit(token));
  ASSERT_TRUE(cache.matches(key()));
  auto* first = cache.lsb();
  token = cache.begin(48000, key(), allocate, free);
  EXPECT_EQ(cache.lsb(), first);
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(cache.lsb()[47999], 0);
  EXPECT_EQ(cache.msb()[1234], 0);
  EXPECT_FALSE(cache.matches(key()));
  EXPECT_TRUE(cache.commit(token));
}

TEST_F(PreparedGrayscaleCacheTest, PartialAllocationFailureRollsBackAndCanRetry) {
  PreparedGrayscaleCache cache;
  failCall = 2;
  EXPECT_EQ(cache.begin(48000, key(), allocate, free), 0u);
  EXPECT_TRUE(allocations.empty());
  EXPECT_EQ(cache.lsb(), nullptr);
  EXPECT_EQ(cache.msb(), nullptr);
  EXPECT_FALSE(cache.matches(key()));
  failCall = -1;
  const auto token = cache.begin(48000, key(), allocate, free);
  ASSERT_NE(token, 0u);
  ASSERT_TRUE(cache.commit(token));
  EXPECT_TRUE(cache.matches(key()));
}

TEST_F(PreparedGrayscaleCacheTest, FailedReplacementNeverServesOldPage) {
  PreparedGrayscaleCache cache;
  auto token = cache.begin(48000, key(), allocate, free);
  ASSERT_TRUE(cache.commit(token));
  failCall = calls + 1;
  EXPECT_EQ(cache.begin(24000, key(), allocate, free), 0u);
  EXPECT_FALSE(cache.matches(key()));
  EXPECT_FALSE(cache.commit(token));
  EXPECT_TRUE(allocations.empty());
}

TEST_F(PreparedGrayscaleCacheTest, CancellationRejectsLateCommitWithoutFreeingReusablePlanes) {
  PreparedGrayscaleCache cache;
  const auto old = cache.begin(48000, key(), allocate, free);
  cache.invalidate();
  EXPECT_FALSE(cache.commit(old));
  EXPECT_EQ(allocations.size(), 2u);
  auto next = key();
  ++next.page;
  const auto token = cache.begin(48000, next, allocate, free);
  EXPECT_FALSE(cache.commit(old));
  ASSERT_TRUE(cache.commit(token));
  EXPECT_FALSE(cache.matches(key()));
  EXPECT_TRUE(cache.matches(next));
  cache.release();
  EXPECT_TRUE(allocations.empty());
  EXPECT_FALSE(cache.matches(next));
}

TEST_F(PreparedGrayscaleCacheTest, RejectsChangedPageLayoutFontAndDarkness) {
  PreparedGrayscaleCache cache;
  ASSERT_TRUE(cache.commit(cache.begin(48000, key(), allocate, free)));
  auto changed = key();
  int marker;
  changed.section = &marker;
  EXPECT_FALSE(cache.matches(changed));
  for (int PreparedGrayscaleCache::Key::* field :
       {&PreparedGrayscaleCache::Key::spine, &PreparedGrayscaleCache::Key::page, &PreparedGrayscaleCache::Key::font,
        &PreparedGrayscaleCache::Key::left, &PreparedGrayscaleCache::Key::top}) {
    changed = key();
    ++(changed.*field);
    EXPECT_FALSE(cache.matches(changed));
  }
  changed = key();
  ++changed.orientation;
  EXPECT_FALSE(cache.matches(changed));
  changed = key();
  ++changed.darkness;
  EXPECT_FALSE(cache.matches(changed));
}

TEST_F(PreparedGrayscaleCacheTest, MissingAllocatorDoesNotAllocateOrLeaveReadyMasks) {
  PreparedGrayscaleCache cache;
  EXPECT_EQ(cache.begin(48000, key(), nullptr, free), 0u);
  EXPECT_EQ(calls, 0);
  EXPECT_FALSE(cache.matches(key()));
}
}  // namespace

TEST(SingleRefreshGrayscaleTest, ConvertsEveryInkLevelAndDarknessWithoutChangingBw) {
  // Independent absolute selector truth table: white=11, black=00,
  // light=01, dark=10 (first digit is LSB). Keep all byte phases covered.
  for (unsigned darkness = 0; darkness < 5; ++darkness) {
    std::array<uint8_t, 48000> bw{}, lsb{}, msb{};
    for (size_t byte = 0; byte < bw.size(); ++byte) {
      for (unsigned bit = 0; bit < 8; ++bit) {
        const unsigned raw = (byte + bit) % 4;
        const uint8_t mask = 1u << bit;
        if (!raw) bw[byte] |= mask;
        if (raw == 2 && darkness == 0) lsb[byte] |= mask;
        if ((raw == 1 || raw == 2) && (darkness == 0 || darkness == 4 || (darkness == 1 && raw == 1)))
          msb[byte] |= mask;
      }
    }
    const auto original = bw;
    ASSERT_TRUE(convertPreparedOverlayToAbsolute(bw.data(), lsb.data(), msb.data(), bw.size()));
    ASSERT_EQ(bw, original);
    for (size_t byte = 0; byte < bw.size(); ++byte) {
      unsigned expectedL = 0, expectedM = 0;
      for (unsigned bit = 0; bit < 8; ++bit) {
        const unsigned raw = (byte + bit) % 4;
        unsigned code = 0;
        if (!raw)
          code = 3;
        else if (darkness == 0 && raw == 1)
          code = 2;
        else if (darkness == 0 && raw == 2)
          code = 1;
        else if ((darkness == 1 && raw == 1) || (darkness == 4 && (raw == 1 || raw == 2)))
          code = 2;
        expectedL |= (code & 1) << bit;
        expectedM |= ((code >> 1) & 1) << bit;
      }
      ASSERT_EQ(lsb[byte], expectedL) << "darkness=" << darkness << " byte=" << byte;
      ASSERT_EQ(msb[byte], expectedM) << "darkness=" << darkness << " byte=" << byte;
    }
  }
}

TEST(SingleRefreshGrayscaleTest, RejectsNullEmptyAndAliasedPlanesBeforeMutation) {
  uint8_t bw = 0x55, lsb = 0x33, msb = 0x0f;
  EXPECT_FALSE(convertPreparedOverlayToAbsolute(nullptr, &lsb, &msb, 1));
  EXPECT_FALSE(convertPreparedOverlayToAbsolute(&bw, nullptr, &msb, 1));
  EXPECT_FALSE(convertPreparedOverlayToAbsolute(&bw, &lsb, nullptr, 1));
  EXPECT_FALSE(convertPreparedOverlayToAbsolute(&bw, &lsb, &msb, 0));
  EXPECT_FALSE(convertPreparedOverlayToAbsolute(&bw, &bw, &msb, 1));
  EXPECT_FALSE(convertPreparedOverlayToAbsolute(&bw, &lsb, &bw, 1));
  EXPECT_FALSE(convertPreparedOverlayToAbsolute(&bw, &lsb, &lsb, 1));
  EXPECT_EQ(bw, 0x55);
  EXPECT_EQ(lsb, 0x33);
  EXPECT_EQ(msb, 0x0f);
}

TEST(SingleRefreshGrayscaleTest, OptInAndRuntimeExclusionsLeaveOrdinaryPagesOnFallback) {
  EXPECT_FALSE(singleRefreshTextAaEligible(false, true, true, false, true, false));
  EXPECT_TRUE(singleRefreshTextAaEligible(true, true, true, false, true, false));
  EXPECT_FALSE(singleRefreshTextAaEligible(true, false, true, false, true, false));
  EXPECT_FALSE(singleRefreshTextAaEligible(true, true, false, false, true, false));
  EXPECT_FALSE(singleRefreshTextAaEligible(true, true, true, true, true, false));
  EXPECT_FALSE(singleRefreshTextAaEligible(true, true, true, false, false, false));
  EXPECT_FALSE(singleRefreshTextAaEligible(true, true, true, false, true, true));
}

TEST(SingleRefreshGrayscaleTest, FourStrategiesKeepForegroundAaIndependentOfNextPagePreparation) {
  for (bool direct : {false, true})
    for (bool preRender : {false, true}) {
      EXPECT_EQ(singleRefreshTextAaEligible(direct, true, true, false, true, false), direct);
      EXPECT_EQ(readerCanPrepareNextPage(preRender, false, 7, 12), preRender);
      EXPECT_EQ(readerCanConsumeNextPage(preRender, true, true, true, 7, 12, 8), preRender);
      // A ready page left over from the previous setting cannot be consumed
      // after Off, but foreground masks can still present the same current text.
      EXPECT_FALSE(readerCanConsumeNextPage(false, true, true, true, 7, 12, 8));
    }
}

TEST(SingleRefreshGrayscaleTest, NavigationAndSectionEdgesNeverConsumeUnrelatedPreparedPixels) {
  EXPECT_FALSE(readerCanPrepareNextPage(true, false, -1, 12));
  EXPECT_FALSE(readerCanPrepareNextPage(true, false, 0, 0));
  EXPECT_FALSE(readerCanPrepareNextPage(true, false, 11, 12));
  EXPECT_FALSE(readerCanPrepareNextPage(true, true, 7, 12));
  EXPECT_FALSE(readerCanConsumeNextPage(true, false, true, true, 7, 12, 8));
  EXPECT_FALSE(readerCanConsumeNextPage(true, true, false, true, 7, 12, 8));
  EXPECT_FALSE(readerCanConsumeNextPage(true, true, true, false, 7, 12, 8));
  EXPECT_FALSE(readerCanConsumeNextPage(true, true, true, true, 7, 12, 7));
  EXPECT_FALSE(readerCanConsumeNextPage(true, true, true, true, 7, 12, 9));
  EXPECT_FALSE(readerCanConsumeNextPage(true, true, true, true, 11, 12, 12));
  EXPECT_TRUE(readerCanConsumeNextPage(true, true, true, true, 10, 12, 11));
}
