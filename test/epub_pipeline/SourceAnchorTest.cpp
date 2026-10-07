#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#include "GfxRenderer.h"
#include "StoredZipWriter.h"

namespace {
class SourceAnchorTest : public testing::Test {
 protected:
  std::filesystem::path dir;
  std::shared_ptr<Epub> book;
  GfxRenderer renderer;
  std::string xhtml;

  void SetUp() override {
    dir = std::filesystem::temp_directory_path() / "witch-source-anchor" /
          testing::UnitTest::GetInstance()->current_test_info()->name();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    HalFile::failReadsFrom = -1;
  }
  void TearDown() override { HalFile::failReadsFrom = -1; }
  void makeBook(const std::string& body, const std::string& css = "") {
    xhtml = "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Synthetic</title><style>" + css +
            "</style></head><body>" + body + "</body></html>";
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<container><rootfiles><rootfile full-path=\"content.opf\" "
            "media-type=\"application/oebps-package+xml\"/></rootfiles></container>");
    zip.add("content.opf",
            "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">"
            "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>Synthetic</dc:title></metadata>"
            "<manifest><item id=\"c\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
            "<spine><itemref idref=\"c\"/></spine></package>");
    zip.add("chapter.xhtml", xhtml);
    // Only the dimension header is read by the layout pass; pixel decode is irrelevant here.
    const unsigned char png[] = {0x89, 'P', 'N', 'G', 13, 10, 26,  10, 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0,
                                 0,    0,   128, 0,   0,  0,  120, 8,  0, 0, 0, 0,  0,   0,   0,   0};
    zip.add("image.png", std::string(reinterpret_cast<const char*>(png), sizeof(png)));
    zip.write((dir / "book.epub").string());
    book = std::make_shared<Epub>((dir / "book.epub").string(), (dir / "cache").string());
    ASSERT_TRUE(book->load(true));
  }
  Section::BuildParams params(int width, int height = 120, bool bionic = false) {
    Section::BuildParams p;
    p.viewportWidth = width;
    p.viewportHeight = height;
    p.fontSizeNormalization = false;
    p.hyphenationEnabled = false;
    p.embeddedStyle = true;
    p.bionicReadingEnabled = bionic;
    return p;
  }
  std::unique_ptr<Section> build(int width, SourceAnchor target = {}, int height = 120, bool bionic = false) {
    auto section = std::make_unique<Section>(book, 0, renderer);
    section->setSourceLookupTarget(target);
    const auto p = params(width, height, bionic);
    EXPECT_TRUE(section->createSectionFile(p, {}, true));
    EXPECT_TRUE(section->loadSectionFile(p));
    return section;
  }
  std::string textAt(Section& section, uint16_t which) {
    section.currentPage = which;
    auto page = section.loadPageFromSectionFile();
    EXPECT_NE(page, nullptr);
    std::string result;
    if (!page) return result;
    for (const auto& element : page->elements) {
      if (element->getTag() == TAG_PageLine) {
        const auto& block = *static_cast<const PageLine&>(*element).getBlock();
        for (uint16_t i = 0; i < block.wordCount(); ++i) result += std::string(block.wordText(i)) + " ";
      }
    }
    return result;
  }
  static std::string numberedWords(int count) {
    std::string result;
    for (int i = 0; i < count; ++i) result += "WORD" + std::to_string(i) + " ";
    return result;
  }
};

TEST_F(SourceAnchorTest, LongNestedParagraphRetainsExactPassageAcrossSizeCycles) {
  makeBook("<div><section><p>" + numberedWords(1500) + "</p></section></div>");
  auto original = build(400);
  ASSERT_GT(original->pageCount, 20);
  const uint16_t middle = original->pageCount / 2;
  const auto anchor = original->getSourceAnchorForPage(middle);
  ASSERT_TRUE(anchor);
  const std::string originalText = textAt(*original, middle);
  const auto firstWord = originalText.substr(0, originalText.find(' '));
  ASSERT_FALSE(firstWord.empty());
  // Keep the original anchor throughout the cycle, rather than replacing it with
  // each reflowed page's earlier first word. Nested paragraphs have no usable old p LUT.
  for (int width : {180, 520, 400, 180, 400}) {
    auto next = build(width, *anchor);
    ASSERT_TRUE(next->sourceLookupPage());
    EXPECT_NE(textAt(*next, *next->sourceLookupPage()).find(firstWord + " "), std::string::npos);
    if (width == 400) EXPECT_EQ(*next->sourceLookupPage(), middle);
  }
}

TEST_F(SourceAnchorTest, CacheReopenAndIoFailuresAreConservative) {
  makeBook("<p>" + numberedWords(600) + "</p>");
  auto section = build(320);
  const uint16_t page = section->pageCount / 2;
  const auto anchor = section->getSourceAnchorForPage(page);
  ASSERT_TRUE(anchor);
  Section reopened(book, 0, renderer);
  ASSERT_TRUE(reopened.loadSectionFile(params(320)));
  EXPECT_EQ(reopened.getPageForSourceOffset(*anchor), std::optional<uint16_t>(page));
  HalFile::failReadsFrom = 0;
  EXPECT_FALSE(reopened.getSourceAnchorForPage(page));
  EXPECT_FALSE(reopened.getPageForSourceOffset(*anchor));
}

TEST_F(SourceAnchorTest, InteriorLongWordProbeAndCookedFragmentSurviveReflow) {
  const std::string longWord = std::string(135, 'a') + "NEEDLE" + std::string(40, 'z');
  makeBook("<p>" + longWord + "</p>");
  const SourceAnchor raw{static_cast<uint32_t>(xhtml.find("NEEDLE")), 0};
  auto section = build(65, raw, 48);
  ASSERT_TRUE(section->sourceLookupPage());
  ASSERT_GT(*section->sourceLookupPage(), 5);
  const auto anchor = section->getSourceAnchorForPage(*section->sourceLookupPage());
  ASSERT_TRUE(anchor);
  ASSERT_GT(anchor->characterOffset, 100);
  ASSERT_LE(anchor->characterOffset, 135);
  const auto after = section->getSourceAnchorForPage(*section->sourceLookupPage() + 1);
  ASSERT_TRUE(after);
  ASSERT_EQ(after->sourceOffset, anchor->sourceOffset);
  EXPECT_GT(after->characterOffset, 135);
  EXPECT_FALSE(section->getPageForSourceOffset(*anchor));  // duplicated whole-word intervals
  const auto originalPage = *section->sourceLookupPage();
  for (int width : {140, 45, 65}) {
    auto resized = build(width, *anchor, 48);
    ASSERT_TRUE(resized->sourceLookupPage());
    if (width == 65) EXPECT_EQ(*resized->sourceLookupPage(), originalPage);
    const auto landed = resized->getSourceAnchorForPage(*resized->sourceLookupPage());
    ASSERT_TRUE(landed);
    EXPECT_LE(landed->characterOffset, anchor->characterOffset);
  }
}

TEST_F(SourceAnchorTest, EntitiesInlineBoundariesAndBionicProbesLandOnTheirText) {
  makeBook("<p>" + numberedWords(200) + "<span>UNIQUE&#x4e;</span><em>EEDLE</em> &madeup; " + numberedWords(300) +
           "</p>");
  for (const std::string needle : {"&#x4e;", "EEDLE", "madeup"}) {
    SourceAnchor target{static_cast<uint32_t>(xhtml.find(needle)), 0};
    for (bool bionic : {false, true}) {
      auto section = build(170, target, 96, bionic);
      ASSERT_TRUE(section->sourceLookupPage()) << needle;
      auto text = textAt(*section, *section->sourceLookupPage());
      if (bionic) text.erase(std::remove(text.begin(), text.end(), ' '), text.end());
      EXPECT_NE(text.find(needle == "madeup"  ? "madeup"
                          : needle == "EEDLE" ? "EEDLE"
                                              : "UNIQUE"),
                std::string::npos)
          << text;
    }
  }
}

TEST_F(SourceAnchorTest, ComposingCharactersMapToContainingNormalizedGlyph) {
  for (const std::string cluster :
       {std::string("e\xcc\x81"), std::string("\xe1\x84\x80\xe1\x85\xa1"), std::string("a\xcc\x82\xcc\x81")}) {
    makeBook("<p>" + cluster + std::string(100, 'b') + "</p>");
    const auto start = xhtml.find(cluster);
    const size_t second = static_cast<unsigned char>(cluster[0]) < 0x80 ? 1 : 3;
    auto section = build(45, {static_cast<uint32_t>(start + second), 0}, 48);
    ASSERT_TRUE(section->sourceLookupPage());
    EXPECT_EQ(*section->sourceLookupPage(), 0);
    auto original = build(45, {static_cast<uint32_t>(start), 30}, 48);
    ASSERT_TRUE(original->sourceLookupPage());
    const auto expected = *original->sourceLookupPage();
    for (int width : {100, 30, 45}) {
      auto resized = build(width, {static_cast<uint32_t>(start), 30}, 48);
      ASSERT_TRUE(resized->sourceLookupPage());
      if (width == 45) EXPECT_EQ(*resized->sourceLookupPage(), expected);
    }
  }
}

TEST_F(SourceAnchorTest, DropCapsAndOverflowRetainActualBodyOrigins) {
  for (const std::string cap : {std::string("Q"), std::string("abcdefghijklmn\xc3\x9fXYZ")}) {
    makeBook("<p>" + numberedWords(100) + "</p><p><span class=\"cap\">" + cap + "</span>suffix AFTERCAP</p>",
             ".cap{float:left;font-size:300%;text-transform:uppercase}");
    const SourceAnchor target{static_cast<uint32_t>(xhtml.find(cap)), 0};
    auto section = build(180, target, 120);
    ASSERT_TRUE(section->sourceLookupPage());
    EXPECT_GT(*section->sourceLookupPage(), 0);
    const auto text = textAt(*section, *section->sourceLookupPage());
    EXPECT_NE(text.find(cap == "Q" ? "Q" : "ABC"), std::string::npos) << text;
  }
}

TEST_F(SourceAnchorTest, FirstLastBlankPagesAndRepeatedHeadersHaveStableOrigins) {
  std::string body = "<pre>\n\n\n\n\n\nSTART\n</pre><table><tr><th>HEADER</th></tr>";
  for (int i = 0; i < 40; ++i) body += "<tr><td>ROW" + std::to_string(i) + "</td></tr>";
  body += "</table><p>FINAL</p>";
  makeBook(body);
  auto section = build(200, {}, 96);
  ASSERT_GT(section->pageCount, 4);
  for (uint16_t page : {uint16_t(0), uint16_t(section->pageCount - 1)}) {
    const auto anchor = section->getSourceAnchorForPage(page);
    ASSERT_TRUE(anchor);
    auto resized = build(145, *anchor, 96);
    EXPECT_TRUE(resized->sourceLookupPage());
  }
  const auto header = static_cast<uint32_t>(xhtml.find("HEADER"));
  for (uint16_t page = 1; page < section->pageCount; ++page) {
    const auto anchor = section->getSourceAnchorForPage(page);
    if (anchor) EXPECT_NE(anchor->sourceOffset, header);  // repeated header isn't new reading progress
  }
}

TEST_F(SourceAnchorTest, ImageOnlyAndFloatingPagesCarryStableSourcePositions) {
  for (const std::string style : {std::string(""), std::string("float:left;width:40px")}) {
    makeBook("<p>" + numberedWords(70) + "</p><p><img src=\"image.png\" style=\"" + style + "\"/></p><p>" +
             numberedWords(100) + "</p>");
    const auto imageTag = xhtml.find("<img");
    const SourceAnchor target{static_cast<uint32_t>(xhtml.find('>', imageTag)), 0};
    auto section = build(200, target, 120);
    ASSERT_TRUE(section->sourceLookupPage());
    ASSERT_GT(*section->sourceLookupPage(), 0);
    section->currentPage = *section->sourceLookupPage();
    auto page = section->loadPageFromSectionFile();
    ASSERT_TRUE(page);
    EXPECT_TRUE(std::any_of(page->elements.begin(), page->elements.end(),
                            [](const auto& element) { return element->getTag() == TAG_PageImage; }));
    const auto anchor = section->getSourceAnchorForPage(*section->sourceLookupPage());
    ASSERT_TRUE(anchor);
    for (int width : {145, 260, 200}) {
      auto resized = build(width, target, 120);
      ASSERT_TRUE(resized->sourceLookupPage());
      if (width == 200) EXPECT_EQ(*resized->sourceLookupPage(), *section->sourceLookupPage());
    }
  }
}

TEST_F(SourceAnchorTest, ReusingSectionDoesNotKeepAStaleExactProbe) {
  makeBook("<p>" + numberedWords(700) + "</p>");
  const SourceAnchor target{static_cast<uint32_t>(xhtml.find("WORD500")), 0};
  auto section = build(150, target);
  ASSERT_TRUE(section->sourceLookupPage());
  const auto old = *section->sourceLookupPage();
  ASSERT_TRUE(section->createSectionFile(params(450), {}, true));
  ASSERT_TRUE(section->sourceLookupPage());
  EXPECT_LT(*section->sourceLookupPage(), old);
  section->setSourceLookupTarget({UINT32_MAX - 1, 0});
  ASSERT_TRUE(section->createSectionFile(params(200), {}, true));
  EXPECT_FALSE(section->sourceLookupPage());
}

TEST_F(SourceAnchorTest, WrappedDropCapFallbackProbesTheTargetFragment) {
  makeBook("<p>PAD</p><p><span class=\"cap\">abcdefghijklmno</span>suffix AFTER</p>",
           ".cap{float:left;font-size:300%;text-transform:uppercase}");
  const auto word = xhtml.find("abcdefghijklmno");
  auto first = build(45, {static_cast<uint32_t>(word), 0}, 48);
  auto later = build(45, {static_cast<uint32_t>(word + 12), 0}, 48);
  ASSERT_TRUE(first->sourceLookupPage());
  ASSERT_TRUE(later->sourceLookupPage());
  EXPECT_GT(*later->sourceLookupPage(), *first->sourceLookupPage());
  const auto text = textAt(*later, *later->sourceLookupPage());
  EXPECT_NE(text.find('M'), std::string::npos) << text;
  const auto anchor = later->getSourceAnchorForPage(*later->sourceLookupPage());
  ASSERT_TRUE(anchor);
  ASSERT_GT(anchor->characterOffset, 0);
  for (int width : {70, 30, 45}) {
    auto resized = build(width, *anchor, 48);
    ASSERT_TRUE(resized->sourceLookupPage());
    if (width == 45) EXPECT_EQ(*resized->sourceLookupPage(), *later->sourceLookupPage());
  }
}

TEST_F(SourceAnchorTest, LutGrowthFailureStopsWithoutEmittingOrAppendingStalePage) {
  makeBook("<p>" + numberedWords(1000) + "</p>");
  const auto saved = ESP.getMaxAllocHeap();
  struct RestoreHeap {
    uint32_t value;
    ~RestoreHeap() { ESP.setMaxAllocHeap(value); }
  } restore{saved};
  size_t completed = 0;
  auto parser = std::make_unique<ChapterHtmlSlimParser>(
      book, renderer, 0, 1.0f, false, 0, 100, 24, false, false, false,
      [&](std::unique_ptr<Page> page) {
        ASSERT_TRUE(page);
        ++completed;
        // The first allocation has16 entries. Drop contiguous space exactly after
        // completing them, while this word batch still has many lines to lay out.
        if (completed == 16) ESP.setMaxAllocHeap(700);
      },
      false, "", book->getCachePath());
  ASSERT_TRUE(parser->setup(0));  // no reserve hint, so the first LUT growth is16
  parser->write(reinterpret_cast<const uint8_t*>(xhtml.data()), xhtml.size());
  parser->finalize();
  EXPECT_FALSE(parser->streamSucceeded());
  EXPECT_EQ(completed, 16u);
  EXPECT_EQ(parser->getParagraphLutPerPage().size(), completed);
  // Further feeds after refusal cannot append the retained, uncompleted page.
  parser->write(reinterpret_cast<const uint8_t*>(xhtml.data()), xhtml.size());
  EXPECT_EQ(completed, 16u);
}

}  // namespace
