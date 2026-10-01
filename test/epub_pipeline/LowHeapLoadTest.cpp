// Loading a page or a chapter on a heap that is low or split into small blocks.
//
// The device is built with -fno-exceptions: a std::vector or std::string that cannot grow does not
// throw, it aborts and reboots the reader. Where the size of an allocation comes from the cache
// file -- a page's footnote count, a chapter's page count -- the loaders ask
// for the block first (HeapFit.h) and carry on without it. These tests put the loaders under a
// heap whose largest block is too small and check what comes out still reads.
#include <Arduino.h>
#include <Serialization.h>
#include <esp_heap_caps.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"

namespace fs = std::filesystem;

namespace {

std::string freshDir(const std::string& tag) {
  const auto dir = fs::temp_directory_path() / "low_heap_load_test" / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

Section::BuildParams params() {
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = 1.0f;
  p.viewportWidth = 480;
  p.viewportHeight = 800;
  p.fontSizeNormalization = false;
  p.embeddedStyle = false;
  return p;
}

std::vector<std::string> wordsOn(const Page& page) {
  std::vector<std::string> out;
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto& block = static_cast<const PageLine&>(*el).getBlock();
    if (!block || !block->valid()) continue;
    for (uint16_t w = 0; w < block->wordCount(); ++w) out.emplace_back(block->wordText(w));
  }
  return out;
}

}  // namespace

TEST(LowHeapLoad, PageWithoutABlockForItsFootnotesLoadsWithoutThem) {
  const std::string path = freshDir("footnotes") + "/page.bin";
  {
    Page page;
    ASSERT_TRUE(page.addFootnote("1", "notes.xhtml#n1"));
    ASSERT_TRUE(page.addFootnote("2", "notes.xhtml#n2"));
    ASSERT_TRUE(page.addFootnote("3", "notes.xhtml#n3"));
    FsFile out;
    ASSERT_TRUE(Storage.openFileForWrite("TST", path, out));
    ASSERT_TRUE(page.serialize(out));
    out.close();
  }

  FsFile in;
  ASSERT_TRUE(Storage.openFileForRead("TST", path, in));
  const auto full = Page::deserialize(in);
  ASSERT_TRUE(full);
  ASSERT_EQ(3u, full->footnotes.size());
  EXPECT_STREQ("notes.xhtml#n2", full->footnotes[1].href);

  // Three entries need 384 bytes in one block.
  const HostLargestFreeBlock fragmented(256);
  ASSERT_TRUE(in.seek(0));
  const auto bare = Page::deserialize(in);
  ASSERT_TRUE(bare) << "the page itself must still load";
  EXPECT_TRUE(bare->footnotes.empty());
  in.close();
}

// Without a block for the page-offset table the chapter still opens, and every page reads its
// offset from the file instead -- the same pages, word for word. (The TOC boundaries and page-break
// labels give way under the same heap; they have their own fallbacks and are not checked here.)
TEST(LowHeapLoad, ChapterWithoutABlockForItsPageTableReadsOffsetsFromTheFile) {
  auto epub = std::make_shared<Epub>(std::string(CORPUS_DIR) + "/test_anchor_pagebreak.epub", freshDir("lut"));
  ASSERT_TRUE(epub->load(true));
  epub->setupCacheDir();
  GfxRenderer renderer;
  const auto p = params();

  std::vector<std::vector<std::string>> expected;
  {
    Section section(epub, 0, renderer);
    ASSERT_TRUE(section.createSectionFile(p, {}, /*skipEviction=*/true));
    ASSERT_TRUE(section.loadSectionFile(p));
    ASSERT_GT(section.pageCount, 3);
    for (int pg = 0; pg < section.pageCount; ++pg) {
      section.currentPage = pg;
      const auto page = section.loadPageFromSectionFile();
      ASSERT_TRUE(page);
      expected.push_back(wordsOn(*page));
    }
  }

  Section section(epub, 0, renderer);
  {
    const HostLargestFreeBlock fragmented(16);
    ASSERT_TRUE(section.loadSectionFile(p)) << "a good cache must not be refused, let alone deleted";
  }
  ASSERT_EQ(expected.size(), static_cast<size_t>(section.pageCount));
  // Backwards, so every load seeks rather than following on from the previous one.
  for (int pg = section.pageCount - 1; pg >= 0; --pg) {
    section.currentPage = pg;
    const auto page = section.loadPageFromSectionFile();
    ASSERT_TRUE(page) << "page " << pg;
    EXPECT_EQ(expected[pg], wordsOn(*page)) << "page " << pg;
  }
  section.currentPage = section.pageCount;
  EXPECT_FALSE(section.loadPageFromSectionFile()) << "the range check still applies";
}
