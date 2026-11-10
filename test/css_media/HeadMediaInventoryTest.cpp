#include <Epub/css/CssMedia.h>
#include <Epub/css/HeadMediaInventory.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

TEST(HeadMediaInventoryTest, MediaListsPreserveScreenAndRejectOnlyDefiniteNonmatches) {
  for (const char* value :
       {"", "screen", "SCREEN", "only screen", "print, screen", "not print", "(width: 1px)", "not screen and (color)"})
    EXPECT_TRUE(cssMedia::mayMatchScreen(value)) << value;
  for (const char* value : {"print", "only print", "speech", "not screen", "not all", "print, speech"})
    EXPECT_FALSE(cssMedia::mayMatchScreen(value)) << value;
}
TEST(HeadMediaInventoryTest, ChunkedInventoryStopsAtHeadAndPreservesMediaDecision) {
  const std::string html =
      "<h:html><h:head><link rel='alternate stylesheet' href='print.css' media='print'/><LINK REL='StyleSheet' "
      "HREF='screen.css' MEDIA='screen'/></h:head><body><link rel='stylesheet' href='ignored.css'/></body></h:html>";
  for (size_t chunk : {1, 7, 256}) {
    std::vector<std::pair<std::string, bool>> links;
    HeadMediaInventory scan(
        [](void* p, const char* href, bool screen) { static_cast<decltype(links)*>(p)->emplace_back(href, screen); },
        &links);
    for (size_t offset = 0; offset < html.size() && !scan.complete(); offset += chunk)
      ASSERT_TRUE(
          scan.feed(reinterpret_cast<const uint8_t*>(html.data() + offset), std::min(chunk, html.size() - offset)));
    ASSERT_TRUE(scan.complete());
    ASSERT_TRUE(scan.valid());
    ASSERT_EQ(links.size(), 2U);
    EXPECT_EQ(links[0].first, "print.css");
    EXPECT_FALSE(links[0].second);
    EXPECT_TRUE(links[1].second);
  }
}
TEST(HeadMediaInventoryTest, MalformedAndOversizedHeadsCannotAuthorizeExclusion) {
  HeadMediaInventory malformed(nullptr, nullptr);
  const std::string bad = "<html><head></other>";
  EXPECT_FALSE(malformed.feed(reinterpret_cast<const uint8_t*>(bad.data()), bad.size()));
  EXPECT_FALSE(malformed.complete());
  HeadMediaInventory oversized(nullptr, nullptr);
  std::string big = "<html><head>" + std::string(HeadMediaInventory::MaxBytes, ' ');
  EXPECT_FALSE(oversized.feed(reinterpret_cast<const uint8_t*>(big.data()), big.size()));
  EXPECT_FALSE(oversized.complete());
}
