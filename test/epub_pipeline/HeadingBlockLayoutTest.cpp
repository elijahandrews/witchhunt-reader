// Blocks laid out inside a heading, and <span>s the book's CSS turns into blocks (issue #388).
//
// St. Martin's chapter openers put the number and the title in two spans styled display:block
// inside one <h1>:
//
//   <h1 class="CHAPTER"><span class="CN">10</span><span class="CT">The Tempest</span></h1>
//
// Every reader stacks them, centered. The parser used to decide block-ness by tag name alone,
// so both spans flowed into one line, "10 The Tempest". Laying them out as blocks exposed a
// second gap: only the FIRST block inside a heading inherited its centering and size (it merges
// into the heading's still-empty block); the next started over with the reader's paragraph
// alignment, Justified by default.
//
// What has to hold:
//   1. Every block inside a heading keeps the heading's centering and size, unless the book's
//      own alignment applies (the "Book's style" setting) or it sets its own size.
//   2. The heading's scope ends with it: the paragraph after it is aligned as a paragraph.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

struct Line {
  int y;
  CssTextAlign align;
  float mult;
  std::string text;  // the line's words joined by single spaces
};

struct HeadingBlockLayoutFixture : testing::Test {
  fs::path work;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("epub_headblock_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  std::string makeBook(const std::string& css, const std::string& body) {
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
            "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n<rootfiles><rootfile "
            "full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n</container>\n");
    zip.add("content.opf",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
            "version=\"3.0\" unique-identifier=\"id\">\n<metadata "
            "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">headblock</dc:identifier>"
            "<dc:title>Heading blocks</dc:title><dc:language>en</dc:language></metadata>\n<manifest>\n"
            "<item id=\"c\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
            "<item id=\"s\" href=\"style.css\" media-type=\"text/css\"/>\n"
            "</manifest>\n<spine><itemref idref=\"c\"/></spine>\n</package>\n");
    zip.add("style.css", "p, h1, h2 { margin: 0; text-indent: 0; }\n" + css);
    zip.add("chapter.xhtml",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\">"
            "<head><title>C</title><link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/></head><body>\n" +
                body + "\n</body></html>\n");
    const std::string path = (work / "headblock.epub").string();
    zip.write(path);
    return path;
  }

  // Every text line of the chapter, in page order. `paragraphAlignment` is the reader setting:
  // 0 = Justified (the default), 4 = the book's own alignment.
  std::vector<Line> layout(const std::string& css, const std::string& body, const uint8_t paragraphAlignment = 0) {
    auto epub = std::make_shared<Epub>(makeBook(css, body), (work / "cache").string());
    EXPECT_TRUE(epub->load(true));
    Section::BuildParams params;
    params.viewportWidth = 480;
    params.viewportHeight = 800;
    params.lineCompression = 1.0f;
    params.embeddedStyle = true;
    params.paragraphAlignment = paragraphAlignment;
    GfxRenderer renderer;
    Section section(epub, 0, renderer);
    EXPECT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));
    EXPECT_TRUE(section.loadSectionFile(params));
    std::vector<Line> lines;
    for (uint16_t p = 0; p < section.pageCount; ++p) {
      section.currentPage = p;
      const auto page = section.loadPageFromSectionFile();
      if (!page) continue;
      for (const auto& el : page->elements) {
        if (el->getTag() != TAG_PageLine) continue;
        const TextBlock& block = *static_cast<const PageLine&>(*el).getBlock();
        Line line{el->yPos, block.getRenderStyle().alignment, block.getRenderStyle().fontSizeMultiplier, {}};
        for (uint16_t w = 0; w < block.wordCount(); ++w) {
          if (w > 0) line.text += ' ';
          line.text += block.wordText(w);
        }
        lines.push_back(line);
      }
    }
    return lines;
  }
};

const char* alignName(const CssTextAlign a) {
  switch (a) {
    case CssTextAlign::Justify:
      return "justify";
    case CssTextAlign::Left:
      return "left";
    case CssTextAlign::Center:
      return "center";
    case CssTextAlign::Right:
      return "right";
    case CssTextAlign::None:
      return "none";
  }
  return "?";
}

}  // namespace

TEST_F(HeadingBlockLayoutFixture, EveryBlockInAHeadingIsCentered) {
  const auto lines = layout("", "<h1><div>Alpha</div><div>Bravo</div></h1><p>Body text</p>");
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0].text, "Alpha");
  EXPECT_STREQ(alignName(lines[0].align), "center");
  EXPECT_EQ(lines[1].text, "Bravo");
  EXPECT_STREQ(alignName(lines[1].align), "center") << "the second block in the heading lost its centering";
  EXPECT_EQ(lines[2].text, "Body text");
  EXPECT_STREQ(alignName(lines[2].align), "justify") << "the heading's centering leaked past </h1>";
}

// Under the Justified setting a book's text-align is overridden for paragraphs. A block in a
// heading is heading text, so it stays centered rather than taking the paragraph setting.
TEST_F(HeadingBlockLayoutFixture, BookAlignedBlockInAHeadingStaysCenteredUnderJustified) {
  const auto lines =
      layout("div.cn { text-align: center; }\n", "<h1><div class=\"cn\">Alpha</div><div class=\"cn\">Bravo</div></h1>");
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_STREQ(alignName(lines[0].align), "center");
  EXPECT_STREQ(alignName(lines[1].align), "center");
}

// With the "Book's style" setting the book's own text-align on the block wins, as it does for
// the heading itself.
TEST_F(HeadingBlockLayoutFixture, BooksStyleSettingKeepsTheBlocksOwnAlignment) {
  const auto lines = layout("div.left { text-align: left; }\n",
                            "<h1><div class=\"left\">Alpha</div><div class=\"left\">Bravo</div></h1>",
                            /*paragraphAlignment=*/4);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_STREQ(alignName(lines[0].align), "left");
  EXPECT_STREQ(alignName(lines[1].align), "left");
}

TEST_F(HeadingBlockLayoutFixture, EveryBlockInAHeadingKeepsTheHeadingSize) {
  const auto lines = layout("", "<h2><div>Alpha</div><div>Bravo</div></h2>");
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_GT(lines[0].mult, 1.0f) << "an h2 without a CSS size should get the default heading size";
  EXPECT_EQ(lines[1].mult, lines[0].mult) << "the second block in the heading lost the heading size";
}

// A size the block sets for itself wins over the heading's.
TEST_F(HeadingBlockLayoutFixture, BlockInAHeadingKeepsItsOwnSize) {
  const auto lines = layout("div.big { font-size: 2em; }\n", "<h2><div>Alpha</div><div class=\"big\">Bravo</div></h2>");
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_GT(lines[1].mult, lines[0].mult);
}
