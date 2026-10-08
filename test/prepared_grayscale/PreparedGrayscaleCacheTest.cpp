#include <gtest/gtest.h>

#include <cstdlib>
#include <unordered_set>

#include "PreparedGrayscaleCache.h"

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
