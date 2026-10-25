// Cover for WHICH PAGE an anchor names.
//
// AnchorMapTest pins that every anchor is in the map. This pins that the page beside it is the
// page the anchor's content actually rendered on -- a different property, and the one a footnote
// jump depends on. An anchor that is present but one page early sends the reader to the page
// before the note, which reads as "the link is broken" just as much as a missing entry does.
//
// The failure it was written for: an anchor's page was taken when its text block STARTED, which
// is before that block's first line has been placed. A block whose first line does not fit on the
// page in progress moves to the next page, and the anchor kept the old count. Anything beginning
// exactly at a page boundary was therefore recorded one page early -- ordinary prose included,
// though a table makes it far likelier, since every fragment flush is a page boundary.
#include <Arduino.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "SyntheticBook.h"
#include "src/activities/reader/EpubLinkBackStack.h"

namespace fs = std::filesystem;

namespace {

const std::string kFixture = std::string(CORPUS_DIR) + "/test_anchor_pagebreak.epub";
constexpr int kAnchorCount = 60;

Section::BuildParams params() {
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = 1.0f;
  p.extraParagraphSpacing = false;
  p.paragraphAlignment = 0;
  p.viewportWidth = 480;
  p.viewportHeight = 800;
  p.hyphenationEnabled = false;
  p.fontSizeNormalization = false;
  p.embeddedStyle = false;
  p.bionicReadingEnabled = false;
  p.inlineFootnotePreviews = false;
  p.imageRendering = 0;
  return p;
}

std::string freshCacheDir(const std::string& tag) {
  const auto dir = fs::temp_directory_path() / "anchor_page_accuracy" / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

// Every word on `page`, so a marker token can be located in the laid-out text rather than in the
// source. Mirrors the walk PipelineRunner's dump uses.
void collectWords(const Page& page, std::vector<std::string>& out) {
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*el);
    const auto& block = line.getBlock();
    if (!block || !block->valid()) continue;
    for (uint16_t w = 0; w < block->wordCount(); ++w) out.emplace_back(block->wordText(w));
  }
}

}  // namespace

// For every anchor: the page the map names must be the page its content is on. Checked over all
// 60 rather than a chosen one, because which paragraphs happen to start at a page boundary is a
// property of the layout, not something the fixture can pin from outside.
TEST(AnchorPageAccuracy, AnchorNamesThePageItsContentIsOn) {
  auto epub = std::make_shared<Epub>(kFixture, freshCacheDir("pages"));
  ASSERT_TRUE(epub->load(true));
  epub->setupCacheDir();

  GfxRenderer renderer;
  Section section(epub, 0, renderer);
  const auto p = params();
  ASSERT_TRUE(section.createSectionFile(p, {}, /*skipEviction=*/true));
  ASSERT_TRUE(section.loadSectionFile(p));
  ASSERT_GT(section.pageCount, 3) << "fixture must span several pages to place starts at breaks";

  // Where each MARKnn actually rendered.
  std::map<std::string, int> markPage;
  for (int pg = 0; pg < section.pageCount; ++pg) {
    section.currentPage = pg;
    const auto page = section.loadPageFromSectionFile();
    ASSERT_TRUE(page) << "page " << pg << " failed to load";
    std::vector<std::string> words;
    collectWords(*page, words);
    for (const auto& w : words) {
      if (w.rfind("MARK", 0) == 0 && markPage.find(w) == markPage.end()) markPage[w] = pg;
    }
  }

  int checked = 0;
  int offBy = 0;
  for (int i = 1; i <= kAnchorCount; ++i) {
    const std::string mark = "MARK" + std::to_string(i);
    const auto seen = markPage.find(mark);
    if (seen == markPage.end()) continue;  // token split across lines; not this test's business
    const auto anchored = section.getPageForAnchor("a" + std::to_string(i));
    ASSERT_TRUE(anchored.has_value()) << "a" << i << " missing from the anchor map";
    ++checked;
    if (static_cast<int>(*anchored) != seen->second) {
      ++offBy;
      EXPECT_EQ(static_cast<int>(*anchored), seen->second)
          << "anchor a" << i << " names page " << *anchored << " but its text is on page " << seen->second;
    }
  }
  EXPECT_GT(checked, kAnchorCount / 2) << "too few anchors located to be meaningful";
  EXPECT_EQ(offBy, 0) << offBy << " of " << checked << " anchors name the wrong page";
}

// Inline ids must follow their own line through streaming paragraph chunks. Deferring <a id>
// to the next block also loses every caller except the last one in a paragraph.
TEST(AnchorPageAccuracy, InlineCallersAndSpanTargetsFollowTheirOwnLines) {
  const auto work = fs::path(freshCacheDir("inline_callers"));
  std::string body = "<section><p>";
  for (int i = 0; i < 18; ++i) {
    for (int word = 0; word < 80; ++word) body += "filler ";
    body += "<sup><a id=\"caller" + std::to_string(i) + "\" href=\"chapter.xhtml#note" + std::to_string(i) + "\">MARK" +
            std::to_string(i) + "</a></sup> ";
    body += "<span id=\"span" + std::to_string(i) + "\">SPAN" + std::to_string(i) + "</span> ";
  }
  body += "</p><ol>";
  for (int i = 0; i < 18; ++i)
    body += "<li id=\"note" + std::to_string(i) + "\"><a href=\"chapter.xhtml#caller" + std::to_string(i) +
            "\">Return</a> synthetic note.</li>";
  body += "</ol></section>";
  auto book = syntheticBook(work, 0, body);
  GfxRenderer renderer;
  Section section(book, 0, renderer);
  auto p = params();
  p.viewportWidth = 240;
  p.viewportHeight = 160;
  ASSERT_TRUE(section.createSectionFile(p, {}, true));
  ASSERT_TRUE(section.loadSectionFile(p));
  ASSERT_GT(section.pageCount, 10);
  std::map<std::string, int> markerPages;
  for (int pageIndex = 0; pageIndex < section.pageCount; ++pageIndex) {
    section.currentPage = pageIndex;
    const auto page = section.loadPageFromSectionFile();
    ASSERT_TRUE(page);
    std::vector<std::string> words;
    collectWords(*page, words);
    for (const auto& word : words)
      if (word.rfind("MARK", 0) == 0 || word.rfind("SPAN", 0) == 0) markerPages.emplace(word, pageIndex);
  }
  for (int i = 0; i < 18; ++i) {
    for (const auto& pair : {std::pair{"caller", "MARK"}, std::pair{"span", "SPAN"}}) {
      const auto marker = markerPages.find(pair.second + std::to_string(i));
      ASSERT_NE(marker, markerPages.end());
      const auto page = section.getPageForAnchor(pair.first + std::to_string(i));
      ASSERT_TRUE(page.has_value()) << pair.first << i;
      EXPECT_EQ(*page, marker->second) << pair.first << i;
    }
  }
}

TEST(AnchorPageAccuracy, AnInlineIdAfterASplitWordDoesNotPointIntoTheEarlierWord) {
  const auto work = fs::path(freshCacheDir("after_split_word"));
  auto book = syntheticBook(work, 0, "<p>" + std::string(600, 'x') + "<span id=\"tail\">TAIL</span></p>");
  GfxRenderer renderer;
  Section section(book, 0, renderer);
  auto p = params();
  p.viewportWidth = 100;
  p.viewportHeight = 60;
  ASSERT_TRUE(section.createSectionFile(p, {}, true));
  ASSERT_TRUE(section.loadSectionFile(p));
  int markerPage = -1;
  for (int pageIndex = 0; pageIndex < section.pageCount; ++pageIndex) {
    section.currentPage = pageIndex;
    const auto page = section.loadPageFromSectionFile();
    ASSERT_TRUE(page);
    std::vector<std::string> words;
    collectWords(*page, words);
    for (const auto& word : words)
      if (word.find("TAIL") != std::string::npos) markerPage = pageIndex;
  }
  ASSERT_GT(markerPage, 3);
  const auto anchored = section.getPageForAnchor("tail");
  ASSERT_TRUE(anchored);
  EXPECT_EQ(*anchored, markerPage);
}

TEST(AnchorPageAccuracy, EmptyTrailingInlineIdsNameTheLastExistingPage) {
  const auto work = fs::path(freshCacheDir("trailing_inline_ids"));
  std::string body = "<p>";
  for (int i = 0; i < 200; ++i) body += "filler ";
  body += "<a id=\"end-link\"></a><span id=\"end-span\"></span></p>";
  auto book = syntheticBook(work, 0, body);
  GfxRenderer renderer;
  Section section(book, 0, renderer);
  auto p = params();
  p.viewportWidth = 240;
  p.viewportHeight = 160;
  ASSERT_TRUE(section.createSectionFile(p, {}, true));
  ASSERT_TRUE(section.loadSectionFile(p));
  ASSERT_GT(section.pageCount, 1);
  EXPECT_EQ(section.getPageForAnchor("end-link"), std::optional<uint16_t>(section.pageCount - 1));
  EXPECT_EQ(section.getPageForAnchor("end-span"), std::optional<uint16_t>(section.pageCount - 1));
}

TEST(AnchorPageAccuracy, BacklinksUnwindTheOriginalPageRangeAfterReflow) {
  const auto work = fs::path(freshCacheDir("backlink_reflow"));
  std::string body = "<p>";
  for (int i = 0; i < 18; ++i) {
    for (int word = 0; word < 80; ++word) body += "filler ";
    body += "<a id=\"caller" + std::to_string(i) + "\" href=\"chapter.xhtml#note\">MARK" + std::to_string(i) + "</a> ";
  }
  body += "</p><p id=\"note\">Note.</p>";
  auto book = syntheticBook(work, 0, body);
  GfxRenderer renderer;
  auto p = params();
  p.viewportWidth = 320;
  p.viewportHeight = 120;
  Section original(book, 0, renderer);
  ASSERT_TRUE(original.createSectionFile(p, {}, true));
  ASSERT_TRUE(original.loadSectionFile(p));
  std::vector<EpubLinkBackStack::Entry> callers;
  callers.reserve(18);
  for (int i = 0; i < 18; ++i) {
    const auto page = original.getPageForAnchor("caller" + std::to_string(i));
    ASSERT_TRUE(page);
    const auto source = original.getSourceAnchorForPage(*page);
    const auto range = original.getSourceRangeForPage(*page);
    ASSERT_TRUE(source);
    ASSERT_TRUE(range);
    EpubLinkBackStack::Entry saved;
    saved.pageNumber = *page;
    saved.pageCount = original.pageCount;
    saved.sourceOffset = source->sourceOffset;
    saved.sourceCharacterOffset = source->characterOffset;
    saved.sourceRangeStart = range->first;
    saved.sourceRangeEnd = range->end;
    callers.push_back(saved);
  }
  p.viewportWidth = 190;
  Section reflowed(book, 0, renderer);
  ASSERT_TRUE(reflowed.createSectionFile(p, {}, true));
  ASSERT_TRUE(reflowed.loadSectionFile(p));
  int differentFromFirstWord = 0;
  for (int i = 0; i < 18; ++i) {
    const auto page = reflowed.getPageForAnchor("caller" + std::to_string(i));
    ASSERT_TRUE(page);
    const auto range = reflowed.getSourceRangeForPage(*page);
    ASSERT_TRUE(range);
    EpubLinkBackStack::Entry stack[EpubLinkBackStack::kMaxDepth] = {callers[i]};
    const auto firstPage = reflowed.getPageForSourceOffset({stack[0].sourceOffset, stack[0].sourceCharacterOffset});
    ASSERT_TRUE(firstPage);
    differentFromFirstWord += *firstPage != *page;
    stack[0].pageNumber = *firstPage;  // old policy used only this reflowed first word
    stack[1].pageNumber = reflowed.pageCount - 1;
    int depth = 2, sessionDepth = 2;
    const int target = EpubLinkBackStack::returnDepth(stack, depth, 0, *page, range->first, range->end);
    EXPECT_EQ(target, 0) << "caller " << i;
    if (target >= 0)
      while (depth > target) EpubLinkBackStack::pop(depth, sessionDepth);
    EXPECT_EQ(depth, 0);
    EXPECT_EQ(sessionDepth, 0);
  }
  EXPECT_GE(differentFromFirstWord, 8) << "the regression must distinguish caller from the old page's first word";
}
