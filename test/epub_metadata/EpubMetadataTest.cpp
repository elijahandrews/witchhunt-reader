#include <Epub/CoverImageRefScanner.h>
#include <Epub/SpineHrefResolver.h>
#include <Epub/TocFallbackPolicy.h>
#include <Epub/parsers/TocNavParser.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>
#define private public
#include <Epub/BookMetadataCache.h>
#undef private

TEST(EpubMetadata, ExactPathsWinAndAmbiguousBasenamesStayUnmapped) {
  const std::vector<std::string> paths{"one/ch.xhtml", "two/ch.xhtml", "other.xhtml"};
  const auto resolve = [&](std::string_view target) {
    return spine_href::resolve(target, paths.size(), [&](int i) { return paths[i]; });
  };
  EXPECT_EQ(resolve("two/ch.xhtml"), 1);
  EXPECT_EQ(resolve("ch.xhtml"), -1);
  EXPECT_EQ(resolve("lost/other.xhtml"), 2);
  EXPECT_EQ(resolve(""), -1);
}

TEST(EpubMetadata, SparseNavOnlyYieldsToStrictlyRicherMappedNcx) {
  EXPECT_TRUE(toc_fallback::navIsSparse(true, 2, 5));
  EXPECT_FALSE(toc_fallback::navIsSparse(true, 3, 5));
  EXPECT_FALSE(toc_fallback::navIsSparse(false, 0, 5));
  EXPECT_TRUE(toc_fallback::shouldAdoptNcx(true, 2, true, 3));
  EXPECT_FALSE(toc_fallback::shouldAdoptNcx(true, 2, true, 2));
  EXPECT_FALSE(toc_fallback::shouldAdoptNcx(true, 2, false, 50));
}

TEST(EpubMetadata, RealCacheResetDiscardsLosingTocAndKeepsSpine) {
  Storage.reset();
  BookMetadataCache cache("/book");
  ASSERT_TRUE(cache.beginWrite());
  ASSERT_TRUE(cache.beginContentOpfPass());
  for (const auto* path : {"one/ch.xhtml", "two/ch.xhtml", "other.xhtml"}) cache.createSpineEntry(path);
  ASSERT_TRUE(cache.endContentOpfPass());
  const auto spine = Storage.bytes("/book/spine.bin.tmp");
  ASSERT_TRUE(cache.beginTocPass());
  cache.createTocEntry("exact", "two/ch.xhtml", "", 1);
  cache.createTocEntry("ambiguous", "ch.xhtml", "", 1);
  cache.createTocEntry("unique", "lost/other.xhtml", "", 1);
  EXPECT_EQ(cache.getTocMappedCount(), 2);
  ASSERT_TRUE(cache.passOut->flush());
  ASSERT_TRUE(cache.tocFile.seek(0));
  EXPECT_EQ(cache.readTocEntry(cache.tocFile).spineIndex, 1);
  EXPECT_EQ(cache.readTocEntry(cache.tocFile).spineIndex, -1);
  EXPECT_EQ(cache.readTocEntry(cache.tocFile).spineIndex, 2);
  ASSERT_TRUE(cache.resetTocPassOutput());
  EXPECT_EQ(cache.getTocCount(), 0);
  EXPECT_EQ(cache.getTocMappedCount(), 0);
  EXPECT_EQ(Storage.bytes("/book/spine.bin.tmp"), spine);
  cache.createTocEntry("replacement", "other.xhtml", "anchor", 2);
  ASSERT_TRUE(cache.endTocPass());
  HalFile output;
  ASSERT_TRUE(Storage.openFileForRead("TST", "/book/toc.bin.tmp", output));
  const auto entry = cache.readTocEntry(output);
  EXPECT_EQ(entry.title, "replacement");
  EXPECT_EQ(entry.spineIndex, 2);
  EXPECT_EQ(entry.anchor, "anchor");
  EXPECT_EQ(output.position(), output.size());
  output.close();
}

TEST(EpubMetadata, TocResetFailsClosedOnWriteError) {
  Storage.reset();
  BookMetadataCache cache("/book");
  ASSERT_TRUE(cache.beginWrite());
  ASSERT_TRUE(cache.beginContentOpfPass());
  cache.createSpineEntry("ch.xhtml");
  ASSERT_TRUE(cache.endContentOpfPass());
  ASSERT_TRUE(cache.beginTocPass());
  cache.createTocEntry("chapter", "ch.xhtml", "", 1);
  Storage.failWritesAt("/book/toc.bin.tmp", 0);
  EXPECT_FALSE(cache.resetTocPassOutput());
  EXPECT_FALSE(cache.tocFile);
  EXPECT_TRUE(cache.spineFile);
  cache.spineFile.close();
}

TEST(EpubMetadata, NavRepairsVoidMarkupAndKeepsNestedEntries) {
  Storage.reset();
  BookMetadataCache cache("/book");
  ASSERT_TRUE(cache.beginWrite());
  ASSERT_TRUE(cache.beginContentOpfPass());
  cache.createSpineEntry("ch.xhtml");
  ASSERT_TRUE(cache.endContentOpfPass());
  ASSERT_TRUE(cache.beginTocPass());
  const std::string html =
      "<html><head><meta charset='utf-8'></head><body><nav epub:type='toc'><ol><li><a href='ch.xhtml'>Part<br> "
      "one</a><ol><li><a href='ch.xhtml#b'>Nested</a></li></ol></li></ol></nav></body></html>";
  const std::string base;
  TocNavParser parser(base, html.size(), &cache);
  ASSERT_TRUE(parser.setup());
  for (const auto byte : html) ASSERT_EQ(parser.write(static_cast<uint8_t>(byte)), 1u);
  EXPECT_EQ(cache.getTocMappedCount(), 2);
  ASSERT_TRUE(cache.endTocPass());
  HalFile output;
  ASSERT_TRUE(Storage.openFileForRead("TST", "/book/toc.bin.tmp", output));
  EXPECT_EQ(cache.readTocEntry(output).level, 1);
  EXPECT_EQ(cache.readTocEntry(output).level, 2);
  output.close();
}

TEST(EpubMetadata, CoverScannerHandlesQuotesNamespacesCommentsAndBounds) {
  for (const std::string document : std::vector<std::string>{
           "<svg><image xlink:href = '../cover.png'/></svg>", "<svg:svg><svg:image href=\"../cover.png\"/></svg:svg>",
           "<html><!-- <img src='false.jpg'> --><body><img src='../cover.png'></body></html>",
           "<html><body><img src='ornament.gif'/><svg><image xlink:href='../cover.png'/></svg></body></html>"}) {
    CoverImageRefScanner scanner;
    for (const auto byte : document)
      if (!scanner.write(static_cast<uint8_t>(byte))) break;
    EXPECT_EQ(scanner.result(), "../cover.png");
  }
  for (const std::string document : std::vector<std::string>{
           "<html><a href='false.jpg'>text</a></html>", "<svg><image href='data:image/png,xx'/></svg>",
           "<html><img src='" + std::string(600, 'x') + "'/></html>",
           "<html>" + std::string(CoverImageRefScanner::MaxScanBytes, ' ') + "<img src='late.jpg'/></html>"}) {
    CoverImageRefScanner scanner;
    for (const auto byte : document)
      if (!scanner.write(static_cast<uint8_t>(byte))) break;
    EXPECT_TRUE(scanner.result().empty());
  }
}
