// A table row that falls back to paragraphs, built the way the reader builds: inside the lent
// framebuffer (a 52 KB BuildArena), not on the heap.
//
// The replayed cell text used to keep the arena wiring of a grid row -- lines in the arena, no
// page-fit hook -- so a line that did not fit was built in the current page's arena block before
// that page was emitted, the emit rewound the block under it, and the lines after it overwrote it.
// The page it went to could not be read back ("corrupt word offset"); on an X3 that was a page of
// Alice's chapter 2, blank after every rebuild. Whether the overwrite lands depends on where the
// page boundary falls, so the fixture is built at several viewport heights, and every page must
// read back word for word as the heap build of the same layout.
#include <Arduino.h>
#include <BuildArena.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "ZipFile.h"

namespace fs = std::filesystem;

namespace {

constexpr size_t kLentFramebufferBytes = 52272;  // the X3's secondary framebuffer

using PageWords = std::vector<std::vector<std::string>>;

// This fixture's body contains plain words and structural tags only. Read its
// complete word stream independently of the layout/parser, so two builds losing
// the same text cannot pass merely by agreeing with one another or a new golden.
std::vector<std::string> sourceWords() {
  const std::string path = std::string(CORPUS_DIR) + "/test_table_cell_overflow.epub";
  ZipFile zip(path);
  size_t size = 0;
  std::unique_ptr<uint8_t, decltype(&std::free)> bytes(zip.readFileToMemory("OEBPS/chapter1.xhtml", &size), &std::free);
  EXPECT_NE(bytes, nullptr);
  if (!bytes) return {};
  const std::string html(reinterpret_cast<const char*>(bytes.get()), size);
  const auto start = html.find("<body>");
  const auto end = html.find("</body>");
  EXPECT_NE(start, std::string::npos);
  EXPECT_NE(end, std::string::npos);
  if (start == std::string::npos || end == std::string::npos) return {};
  std::string plain;
  bool tag = false;
  for (size_t i = start; i < end; ++i) {
    if (html[i] == '<') tag = true;
    if (!tag) plain += html[i];
    if (html[i] == '>') {
      tag = false;
      plain += ' ';
    }
  }
  std::vector<std::string> words;
  std::istringstream input(plain);
  for (std::string word; input >> word;) words.push_back(std::move(word));
  return words;
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

// Builds spine 0 at `viewportHeight` (in a lent-size arena when `inArena`) and reads every page
// back. A page that does not load is recorded as a single "<unreadable>" word.
PageWords buildAndRead(const std::string& tag, const int viewportHeight, const bool inArena) {
  const auto dir = fs::temp_directory_path() / "table_row_fallback_arena" / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  auto epub = std::make_shared<Epub>(std::string(CORPUS_DIR) + "/test_table_cell_overflow.epub", dir.string());
  EXPECT_TRUE(epub->load(true));
  epub->setupCacheDir();

  GfxRenderer renderer;
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = 1.0f;
  p.viewportWidth = 480;
  p.viewportHeight = static_cast<uint16_t>(viewportHeight);
  p.fontSizeNormalization = false;
  p.embeddedStyle = false;

  Section section(epub, 0, renderer);
  std::vector<uint8_t> lent(kLentFramebufferBytes);
  BuildArena arena(lent.data(), lent.size());
  if (inArena) section.setExternalBuildScratch(&arena);
  EXPECT_TRUE(section.createSectionFile(p, {}, /*skipEviction=*/true));
  section.setExternalBuildScratch(nullptr);
  EXPECT_TRUE(section.loadSectionFile(p));

  PageWords pages;
  for (int pg = 0; pg < section.pageCount; ++pg) {
    section.currentPage = pg;
    const auto page = section.loadPageFromSectionFile();
    pages.push_back(page ? wordsOn(*page) : std::vector<std::string>{"<unreadable>"});
  }
  return pages;
}

}  // namespace

TEST(TableRowFallbackArena, ReplayedCellPagesReadBackLikeTheHeapBuild) {
  const auto expected = sourceWords();
  ASSERT_FALSE(expected.empty());
  for (const int viewportHeight : {300, 360, 420, 480, 540, 600, 700, 800}) {
    const std::string tag = std::to_string(viewportHeight);
    const PageWords heap = buildAndRead(tag + "_heap", viewportHeight, /*inArena=*/false);
    const PageWords arena = buildAndRead(tag + "_arena", viewportHeight, /*inArena=*/true);
    ASSERT_GT(heap.size(), 2u) << "viewport " << viewportHeight << ": the fixture must span pages";
    ASSERT_EQ(heap.size(), arena.size()) << "viewport " << viewportHeight;
    for (size_t pg = 0; pg < heap.size(); ++pg) {
      EXPECT_EQ(heap[pg], arena[pg]) << "viewport " << viewportHeight << ", page " << pg;
    }
    std::vector<std::string> actual;
    for (const auto& page : heap) actual.insert(actual.end(), page.begin(), page.end());
    EXPECT_EQ(actual, expected) << "viewport " << viewportHeight << ": fallback must preserve every word in order";
  }
}
