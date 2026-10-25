#include <gtest/gtest.h>

#include "activities/reader/EpubJumpBack.h"

namespace {
EpubLinkBackStack::Entry origin(int page = 19) {
  EpubLinkBackStack::Entry entry;
  entry.spineIndex = 3;
  entry.pageNumber = page;
  entry.pageCount = 80;
  entry.paragraphIndex = 7;
  entry.hasParagraph = true;
  entry.sourceOffset = 12345;
  entry.sourceCharacterOffset = 246;
  return entry;
}
}  // namespace

TEST(EpubJumpBack, OrdinaryReadingHasNoReturnTarget) {
  EpubJumpBack back;
  EpubJumpBack::Target out{origin(), true};
  for (int i = 0; i < 20; ++i) back.pageTurned();
  EXPECT_FALSE(back.available());
  EXPECT_FALSE(back.take(out));
  EXPECT_EQ(out.position.sourceOffset, 12345u);
}

TEST(EpubJumpBack, ReturnsExactSourceWithinLongParagraphAndConsumesTheJump) {
  EpubJumpBack back;
  back.remember(origin());
  EpubJumpBack::Target out;
  ASSERT_TRUE(back.take(out));
  EXPECT_EQ(out.position.spineIndex, 3);
  EXPECT_EQ(out.position.pageNumber, 19);
  EXPECT_EQ(out.position.pageCount, 80);
  EXPECT_EQ(out.position.paragraphIndex, 7);
  EXPECT_TRUE(out.position.hasParagraph);
  EXPECT_EQ(out.position.sourceOffset, 12345u);
  EXPECT_EQ(out.position.sourceCharacterOffset, 246);
  EXPECT_FALSE(out.followedLink);
  EXPECT_FALSE(back.available());
  EXPECT_FALSE(back.take(out));
}

TEST(EpubJumpBack, RemainsAvailableThroughFiveTurnsAndExpiresOnSixth) {
  EpubJumpBack back;
  back.remember(origin());
  EXPECT_TRUE(back.available());
  for (int i = 0; i < 5; ++i) {
    back.pageTurned();
    EXPECT_TRUE(back.available()) << i;
  }
  back.pageTurned();
  EXPECT_FALSE(back.available());
  EpubJumpBack::Target out;
  EXPECT_FALSE(back.take(out));
  for (int i = 0; i < 255; ++i) back.pageTurned();
  EXPECT_FALSE(back.available());
}

TEST(EpubJumpBack, NewJumpReplacesOriginAndRenewsGracePeriod) {
  EpubJumpBack back;
  back.remember(origin(), true);
  for (int i = 0; i < 4; ++i) back.pageTurned();
  back.remember(origin(43));
  for (int i = 0; i < 5; ++i) back.pageTurned();
  EpubJumpBack::Target out;
  ASSERT_TRUE(back.take(out));
  EXPECT_EQ(out.position.pageNumber, 43);
  EXPECT_FALSE(out.followedLink);
}

TEST(EpubJumpBack, LinkReturnRetainsItsDistinctStackSemantics) {
  EpubJumpBack back;
  back.remember(origin(), true);
  back.pageTurned();
  EpubJumpBack::Target out;
  ASSERT_TRUE(back.take(out));
  EXPECT_TRUE(out.followedLink);
  EXPECT_EQ(out.position.pageNumber, 19);
}

TEST(EpubJumpBack, ExplicitReturnClearsUndoRatherThanCreatingAReturnLoop) {
  EpubJumpBack back;
  back.remember(origin(), true);
  back.clear();
  EXPECT_FALSE(back.available());
  back.pageTurned();
  EpubJumpBack::Target out;
  EXPECT_FALSE(back.take(out));
  back.remember(origin(2));
  ASSERT_TRUE(back.take(out));
  EXPECT_EQ(out.position.pageNumber, 2);
}
