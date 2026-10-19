#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "BuildArena.h"
#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "StoredZipWriter.h"

namespace {
class PublisherBoxLayout : public testing::Test {
 protected:
  std::filesystem::path dir;
  std::shared_ptr<Epub> book;
  GfxRenderer renderer;
  std::string xhtml;
  void SetUp() override {
    dir = std::filesystem::temp_directory_path() / "witch-publisher-box-layout" /
          testing::UnitTest::GetInstance()->current_test_info()->name();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
  }
  void makeBook(const std::string& body, const std::string& css) {
    xhtml =
        "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Synthetic</title><style>"
        "p{margin:0;text-indent:0;text-align:left}" +
        css + "</style></head><body>" + body + "</body></html>";
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<container><rootfiles><rootfile full-path=\"content.opf\" "
            "media-type=\"application/oebps-package+xml\"/></rootfiles></container>");
    zip.add("content.opf",
            "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\"><metadata "
            "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>Synthetic</dc:title></metadata><manifest><item "
            "id=\"c\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/><item id=\"im\" href=\"image.png\" "
            "media-type=\"image/png\"/></manifest><spine><itemref idref=\"c\"/></spine></package>");
    zip.add("chapter.xhtml", xhtml);
    // Original white image: valid PNG bytes, so geometry tests can also replay
    // real image caches without decoder errors or external fixture files.
    const unsigned char png[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
        0x00, 0x80, 0x00, 0x00, 0x00, 0x78, 0x08, 0x00, 0x00, 0x00, 0x00, 0x1f, 0x94, 0xa5, 0x63, 0x00, 0x00, 0x00,
        0x55, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0xed, 0xce, 0x21, 0x01, 0x00, 0x00, 0x00, 0x02, 0x20, 0xff, 0x9f,
        0xd6, 0x19, 0x16, 0x58, 0x40, 0x7a, 0x16, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x81, 0x77, 0x60, 0x13, 0x54, 0xc7, 0x76,
        0x44, 0x69, 0xa5, 0x79, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
    zip.add("image.png", std::string(reinterpret_cast<const char*>(png), sizeof(png)));
    zip.write((dir / "book.epub").string());
    book = std::make_shared<Epub>((dir / "book.epub").string(), (dir / "cache").string());
    ASSERT_TRUE(book->load(true));
    book->loadImageManifest();
  }
  std::unique_ptr<Section> build(int height = 400, bool arena = false) {
    auto section = std::make_unique<Section>(book, 0, renderer);
    Section::BuildParams p;
    p.viewportWidth = 400;
    p.viewportHeight = height;
    p.fontSizeNormalization = false;
    p.embeddedStyle = true;
    p.paragraphAlignment = static_cast<uint8_t>(CssTextAlign::None);
    BuildArena scratch(48 * 1024);
    if (arena) section->setExternalBuildScratch(&scratch);
    EXPECT_TRUE(section->createSectionFile(p, {}, true));
    EXPECT_EQ(scratch.releaseFailures(), 0u);
    section->setExternalBuildScratch(nullptr);
    EXPECT_TRUE(section->loadSectionFile(p));
    return section;
  }
  struct Word {
    int page, x, y, style;
    uint32_t typography;
  };
  std::map<std::string, Word> words(Section& section) {
    std::map<std::string, Word> result;
    for (int pg = 0; pg < section.pageCount; ++pg) {
      section.currentPage = pg;
      auto page = section.loadPageFromSectionFile();
      EXPECT_NE(page, nullptr);
      if (!page) continue;
      for (const auto& el : page->elements)
        if (el->getTag() == TAG_PageLine) {
          const auto* line = static_cast<const PageLine*>(el.get())->getBlock();
          for (uint16_t i = 0; i < line->wordCount(); ++i)
            result.emplace(std::string(line->wordText(i)),
                           Word{pg, el->xPos + line->wordXpos(i), el->yPos, static_cast<int>(line->wordStyle(i)),
                                line->wordTypography(i)});
        }
    }
    return result;
  }
};

TEST_F(PublisherBoxLayout, EmptyHeightAndContainerMinimumAreIndependentAndCached) {
  makeBook("<div class=\"opener\"><p>Heading</p></div><p class=\"space\"></p><p>Following</p><p>Sibling</p>",
           ".opener{min-height:100px}.space{height:30px}");
  for (int warm = 0; warm < 2; ++warm) {
    auto section = build();
    const auto found = words(*section);
    ASSERT_EQ(found.size(), 3u);
    EXPECT_EQ(found.at("Heading").y, 0);
    EXPECT_EQ(found.at("Following").y, 130);
    EXPECT_EQ(found.at("Sibling").y, 154);
  }
}

TEST_F(PublisherBoxLayout, BoxShadeAndRulesSurviveSerializationAndDoNotLeak) {
  makeBook("<div class=\"box\"><p>Inside</p></div><p>Outside <span class=\"rule\">Underlined</span> Plain</p>",
           ".box{background:#e7e7e9;border-top:1px solid;border-bottom:2px solid;padding:3px}.rule{border-bottom:1px "
           "solid}");
  auto section = build();
  section->currentPage = 0;
  auto page = section->loadPageFromSectionFile();
  ASSERT_NE(page, nullptr);
  ASSERT_GE(page->elements.size(), 3u);
  ASSERT_EQ(page->elements.front()->getTag(), TAG_PageBox);
  const auto& box = static_cast<const PageBox&>(*page->elements.front());
  EXPECT_GE(box.gray, 230);
  EXPECT_LE(box.gray, 233);
  EXPECT_EQ(box.top, 1);
  EXPECT_EQ(box.bottom, 2);
  EXPECT_GE(box.height, 24 + 6 + 3);
  const auto found = words(*section);
  EXPECT_LT(found.at("Inside").y, found.at("Outside").y);
  EXPECT_NE(found.at("Underlined").style & EpdFontFamily::UNDERLINE, 0);
  EXPECT_EQ(found.at("Plain").style & EpdFontFamily::UNDERLINE, 0);
}

TEST_F(PublisherBoxLayout, GeneratedPunctuationDoesNotReplaceTheRealSourceAnchor) {
  makeBook("<h1 id=\"heading\">Actual</h1><p class=\"credit\">Attribution</p><p>Plain</p>",
           "h1::before{content:'['}h1::after{content:']'}.credit::before{content:'- '}");
  for (int warm = 0; warm < 2; ++warm) {
    auto section = build();
    const auto found = words(*section);
    EXPECT_EQ(found.count("["), 1u);
    EXPECT_EQ(found.count("]"), 1u);
    EXPECT_EQ(found.count("Actual"), 1u);
    EXPECT_EQ(found.count("Attribution"), 1u);
    const auto anchor = section->getSourceAnchorForPage(0);
    ASSERT_TRUE(anchor);
    EXPECT_EQ(anchor->sourceOffset, xhtml.find("Actual"));
    EXPECT_EQ(section->getPageForAnchor("heading"), 0);
  }
}

TEST_F(PublisherBoxLayout, AvoidGroupMovesTogetherWithAnchorsAndWithBorrowedArena) {
  makeBook(
      "<p>One</p><p>Two</p><p>Three</p><p>Four</p><p>Five</p>"
      "<div class=\"keep\"><p id=\"a\">Together</p><p id=\"b\">Caption</p></div><p>After</p>",
      ".keep{page-break-inside:avoid}");
  for (bool arena : {false, true}) {
    auto section = build(160, arena);
    const auto found = words(*section);
    ASSERT_EQ(found.size(), 8u);
    EXPECT_EQ(found.at("Five").page, 0);
    EXPECT_EQ(found.at("Together").page, 1);
    EXPECT_EQ(found.at("Together").page, found.at("Caption").page);
    EXPECT_EQ(section->getPageForAnchor("a"), found.at("Together").page);
    EXPECT_EQ(section->getPageForAnchor("b"), found.at("Caption").page);
    EXPECT_EQ(found.at("Together").y, 0);
    EXPECT_EQ(found.at("Caption").y, 24);
  }
}

TEST_F(PublisherBoxLayout, OverheightAvoidGroupStreamsWithoutDroppingText) {
  std::string body = "<p>Before</p><div class=\"keep\">";
  for (int i = 0; i < 30; ++i) body += "<p>Item" + std::to_string(i) + "</p>";
  body += "</div><p>After</p>";
  makeBook(body, ".keep{break-inside:avoid}");
  auto section = build(120, true);
  const auto found = words(*section);
  EXPECT_EQ(found.size(), 32u);
  EXPECT_GT(section->pageCount, 5);
  EXPECT_EQ(found.at("Item0").page, 1);
}

TEST_F(PublisherBoxLayout, ExplicitInlineBlockPercentageKeepsDecorativeImageSmall) {
  makeBook("<div class=\"small\"><img src=\"image.png\" style=\"width:100%\"/></div><p>After</p>",
           ".small{display:inline-block;width:25%}");
  auto section = build();
  section->currentPage = 0;
  auto page = section->loadPageFromSectionFile();
  ASSERT_NE(page, nullptr);
  int images = 0;
  for (const auto& el : page->elements)
    if (el->getTag() == TAG_PageImage) {
      EXPECT_EQ(static_cast<const PageImage&>(*el).getImageBlock().getWidth(), 100);
      ++images;
    }
  EXPECT_EQ(images, 1);
}

TEST_F(PublisherBoxLayout, RightTextFloatSharesTheLineAndClearMovesBelowIt) {
  makeBook("<p>Left <span class=\"right\">Right</span> tail</p><p class=\"clear\">Below</p>",
           ".right{float:right}.clear{clear:both}");
  auto section = build();
  const auto found = words(*section);
  ASSERT_EQ(found.size(), 4u);
  EXPECT_EQ(found.at("Left").y, found.at("Right").y);
  EXPECT_GT(found.at("Right").x, found.at("tail").x);
  EXPECT_GT(found.at("Below").y, found.at("Right").y);
}

TEST_F(PublisherBoxLayout, BoxMarginsStayOutsideThePaintedBackground) {
  makeBook("<div class=\"box\">Inside</div><p>After</p>", ".box{margin:20px 10px;background:#e7e7e9}");
  auto section = build();
  section->currentPage = 0;
  auto page = section->loadPageFromSectionFile();
  ASSERT_NE(page, nullptr);
  ASSERT_EQ(page->elements.front()->getTag(), TAG_PageBox);
  const auto& box = static_cast<const PageBox&>(*page->elements.front());
  EXPECT_EQ(box.yPos, 20);
  EXPECT_EQ(box.xPos, 10);
  EXPECT_EQ(box.width, 380);
  EXPECT_EQ(box.height, 24);
  EXPECT_EQ(words(*section).at("After").y, 64);
}

TEST_F(PublisherBoxLayout, ShortAvoidGroupStaysOnItsExistingPage) {
  makeBook("<p>Before</p><div class=\"keep\"><p id=\"group\">Inside</p><p>Caption</p></div><p>After</p>",
           ".keep{page-break-inside:avoid}");
  auto section = build(160, true);
  const auto found = words(*section);
  EXPECT_EQ(section->pageCount, 1);
  EXPECT_EQ(found.at("Inside").y, 24);
  EXPECT_EQ(found.at("Caption").y, 48);
  EXPECT_EQ(found.at("After").y, 72);
  EXPECT_EQ(section->getPageForAnchor("group"), 0);
}

TEST_F(PublisherBoxLayout, ImageAndCaptionGroupMovesAsOneUnit) {
  makeBook(
      "<p>Before</p><p>Another</p><p>Third</p><div class=\"keep\"><img src=\"image.png\"/>"
      "<p id=\"caption\">Caption</p></div><p>After</p>",
      ".keep{page-break-inside:avoid}");
  auto section = build(180, true);
  const auto found = words(*section);
  EXPECT_EQ(found.at("Caption").page, 1);
  EXPECT_EQ(section->getPageForAnchor("caption"), 1);
  section->currentPage = 1;
  auto page = section->loadPageFromSectionFile();
  ASSERT_NE(page, nullptr);
  bool image = false;
  for (const auto& el : page->elements) image |= el->getTag() == TAG_PageImage;
  EXPECT_TRUE(image);
}

TEST_F(PublisherBoxLayout, ExplicitImageMaximaConstrainTheFinalAspectRatio) {
  makeBook("<img src=\"image.png\" style=\"width:100%;max-width:50px;max-height:40px\"/><p>After</p>", "");
  auto section = build();
  section->currentPage = 0;
  auto page = section->loadPageFromSectionFile();
  ASSERT_NE(page, nullptr);
  int images = 0;
  for (const auto& el : page->elements)
    if (el->getTag() == TAG_PageImage) {
      const auto& image = static_cast<const PageImage&>(*el).getImageBlock();
      EXPECT_LE(image.getWidth(), 50);
      EXPECT_EQ(image.getHeight(), 40);
      EXPECT_NEAR(float(image.getWidth()) / image.getHeight(), 128.f / 120, .02);
      ++images;
    }
  EXPECT_EQ(images, 1);
}

TEST_F(PublisherBoxLayout, MultiPageBackgroundFragmentsPreserveEveryWord) {
  std::string body = "<div class=\"box\">";
  for (int i = 0; i < 20; ++i) body += "<p>Item" + std::to_string(i) + "</p>";
  body += "</div><p>After</p>";
  makeBook(body, ".box{background:#e7e7e9;border-top:1px solid;border-bottom:1px solid}");
  auto section = build(120, true);
  EXPECT_EQ(words(*section).size(), 21u);
  int top = 0, bottom = 0;
  for (int pg = 0; pg < section->pageCount; ++pg) {
    section->currentPage = pg;
    auto page = section->loadPageFromSectionFile();
    ASSERT_NE(page, nullptr);
    for (const auto& el : page->elements)
      if (el->getTag() == TAG_PageBox) {
        const auto& box = static_cast<const PageBox&>(*el);
        top += box.top;
        bottom += box.bottom;
        EXPECT_GE(box.yPos, 0);
        EXPECT_LE(box.yPos + box.height, 120);
        EXPECT_GT(box.height, 0);
      }
  }
  EXPECT_EQ(top, 1);
  EXPECT_EQ(bottom, 1);
}

TEST_F(PublisherBoxLayout, NestedOpaqueBackgroundReplacesDitherAndReplayPreservesTextSelectors) {
  std::vector<bool> dark(16 * 16, false);
  renderer.pixelSink = [&](int x, int y, bool value) {
    if (x >= 0 && x < 16 && y >= 0 && y < 16) dark[y * 16 + x] = value;
  };
  PageBox outer(0, 0, 16, 128, 0);
  outer.height = 16;
  PageBox inner(4, 4, 8, 255, 0);
  inner.height = 8;
  outer.render(renderer, 0, 0, 0);
  inner.render(renderer, 0, 0, 0);
  int black = 0;
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x) {
      if (x >= 4 && x < 12 && y >= 4 && y < 12)
        EXPECT_FALSE(dark[y * 16 + x]);
      else
        black += dark[y * 16 + x];
    }
  EXPECT_GT(black, 0);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  int calls = 0;
  renderer.pixelSink = [&](int, int, bool value) {
    ++calls;
    EXPECT_TRUE(value);
  };
  inner.render(renderer, 0, 0, 0);
  EXPECT_EQ(calls, 64);
  renderer.opaqueReplay = true;
  calls = 0;
  outer.render(renderer, 0, 0, 0);
  inner.render(renderer, 0, 0, 0);
  EXPECT_EQ(calls, 0);
}
TEST_F(PublisherBoxLayout, MovedGroupPrintedLabelTracksItsContent) {
  makeBook(
      "<p>One</p><p>Two</p><p>Three</p><p>Four</p><p>Five</p>"
      "<div class='keep'><p id='label' role='doc-pagebreak' "
      "aria-label='42'>Together</p><p>Caption</p></div><p>After</p>",
      ".keep{break-inside:avoid}");
  auto section = build(160, true);
  const auto found = words(*section);
  EXPECT_EQ(found.at("Together").page, 1);
  EXPECT_EQ(section->getPageForAnchor("label"), found.at("Together").page);
  EXPECT_FALSE(section->getPrintedPageLabelForPage(0).has_value());
  EXPECT_TRUE(section->getPrintedPageLabelForPage(1).has_value());
}
TEST_F(PublisherBoxLayout, MultilineFloatDoesNotOverlapItsParentAtPageBreak) {
  makeBook(
      "<p>One</p><p>Two</p><p>Three</p><p>Four</p>"
      "<p>Left <span class='right'>Alpha Bravo Charlie Delta Echo Foxtrot Golf Hotel India Juliet Kilo Lima Mike "
      "November Oscar Papa Quebec Romeo Sierra Tango Uniform Victor Whiskey Xray Yankee Zulu</span> tail</p><p "
      "class='clear'>Below</p>",
      ".right{float:right}.clear{clear:both}");
  auto section = build(120, true);
  const auto found = words(*section);
  const auto left = found.at("Left");
  EXPECT_LE(left.page, found.at("Alpha").page);
  // A normal parent word must not overlap a right-column word on the same line.
  for (const auto& [word, v] : found)
    if (word != "Left" && word != "tail" && v.page == left.page && v.y == left.y)
      EXPECT_TRUE(v.x >= left.x + 40 || v.x + int(word.size()) * 8 <= left.x) << word;
  EXPECT_GT(found.at("Below").page * 120 + found.at("Below").y, found.at("Zulu").page * 120 + found.at("Zulu").y);
}

TEST_F(PublisherBoxLayout, InheritedMinimumHeightDoesNotDoubleAnEmptyNestedSpacer) {
  makeBook("<div class=\"outer\"><div class=\"inner\"></div></div><p>After</p>",
           ".outer{min-height:90px}.inner{min-height:inherit}");
  auto section = build();
  EXPECT_EQ(words(*section).at("After").y, 90);
}

TEST_F(PublisherBoxLayout, PercentageImageMaximumUsesItsContainingInlineBlock) {
  makeBook("<div class=\"small\"><img src=\"image.png\" style=\"width:100%;max-width:50%\"/></div><p>After</p>",
           ".small{display:inline-block;width:50%}");
  auto section = build();
  section->currentPage = 0;
  auto page = section->loadPageFromSectionFile();
  ASSERT_NE(page, nullptr);
  int images = 0;
  for (const auto& el : page->elements)
    if (el->getTag() == TAG_PageImage) {
      EXPECT_EQ(static_cast<const PageImage&>(*el).getImageBlock().getWidth(), 100);
      ++images;
    }
  EXPECT_EQ(images, 1);
}

TEST_F(PublisherBoxLayout, MultilineFloatMovesWithParentAndHonorsItsLineHeight) {
  makeBook(
      "<p>One</p><p>Two</p><p>Three</p><p>Four</p><p>Left <span class=\"right\">"
      "Alpha Bravo Charlie Delta Echo Foxtrot Golf Hotel</span> tail</p><p class=\"clear\">Below</p>",
      ".right{float:right;line-height:3}.clear{clear:both}");
  auto section = build(120, true);
  const auto found = words(*section);
  EXPECT_EQ(found.at("Left").page, 1);
  EXPECT_EQ(found.at("Alpha").page, 1);
  EXPECT_EQ(found.at("Left").y, 0);
  EXPECT_EQ(found.at("Alpha").y, 0);
  EXPECT_GT(found.at("Hotel").y, found.at("Alpha").y);
  EXPECT_GE(found.at("Below").page * 120 + found.at("Below").y,
            found.at("Hotel").page * 120 + found.at("Hotel").y + 48);
}

TEST_F(PublisherBoxLayout, ShadedPageAndTableImagesMaskTheirActualPlacedRectangles) {
  makeBook(
      "<div class='box'><p>Heading</p><img src='image.png' style='width:64px;height:40px'/>"
      "<p>Bridge</p><table><tr><td>Cell<img src='image.png' style='width:48px;height:32px'/></td>"
      "<td>Neighbor</td></tr></table><p>Ending</p></div>",
      ".box{background:#e7e7e9}table{border-collapse:collapse}");
  struct Rect {
    int x, y, width, height;
    bool cell;
  };
  for (const bool arena : {false, true}) {
    auto section = build(600, arena);
    ASSERT_EQ(section->pageCount, 1);
    section->currentPage = 0;
    auto page = section->loadPageFromSectionFile();
    ASSERT_NE(page, nullptr);
    std::vector<Rect> imageRects;
    int boxes = 0;
    constexpr int dx = 7, dy = 11, font = 1;
    for (const auto& element : page->elements) {
      if (element->getTag() == TAG_PageBox) ++boxes;
      if (element->getTag() == TAG_PageImage) {
        const auto& image = static_cast<const PageImage&>(*element);
        imageRects.push_back({image.xPos + dx, image.yPos + dy, image.getImageBlock().getWidth(),
                              image.getImageBlock().getRenderedHeight(), false});
      }
      if (element->getTag() == TAG_PageTable) {
        const auto& table = static_cast<const PageTableFragment&>(*element);
        ASSERT_GT(table.getColumnCount(), 0);
        int rowY = table.yPos + dy;
        for (const auto& row : table.getRows()) {
          int column = 0;
          for (const auto& cell : row.cells) {
            if (cell.image) {
              const int cellX =
                  table.xPos + dx + TABLE_CELL_PADDING + table.getTotalWidth() * column / table.getColumnCount();
              const int imageY =
                  rowY + TABLE_CELL_PADDING +
                  cell.lines.size() * (cell.lineAdvance ? cell.lineAdvance : renderer.getLineHeight(font));
              imageRects.push_back({cellX, imageY, cell.image->getWidth(), cell.image->getRenderedHeight(), true});
            }
            column += cell.colSpan;
          }
          rowY += row.height;
        }
      }
    }
    ASSERT_EQ(boxes, 1);
    ASSERT_EQ(imageRects.size(), 2u);
    EXPECT_NE(imageRects.front().cell, imageRects.back().cell);
    for (const auto& rect : imageRects) {
      ASSERT_GT(rect.width, 0);
      ASSERT_GT(rect.height, 0);
    }
    // Replay uses production Page geometry and the production bounded mask.
    // A table image follows its cell's text, and the supplied page offset must
    // be applied exactly once to both image kinds.
    renderer.opaqueReplay = true;
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    for (const bool textOnly : {false, true}) {
      std::vector<uint8_t> background(480 * 800, 0);
      renderer.pixelSink = [&](int x, int y, bool dark) {
        ASSERT_GE(x, 0);
        ASSERT_LT(x, 480);
        ASSERT_GE(y, 0);
        ASSERT_LT(y, 800);
        if (dark) background[y * 480 + x] = 1;
      };
      if (textOnly)
        EXPECT_TRUE(page->renderTextOnly(renderer, font, dx, dy));
      else
        page->render(renderer, font, dx, dy, true);
      int retainedDots = 0;
      for (uint8_t pixel : background) retainedDots += pixel;
      EXPECT_GT(retainedDots, 0);
      for (const auto& rect : imageRects) {
        SCOPED_TRACE(rect.cell ? "table image" : "page image");
        for (int y = rect.y; y < rect.y + rect.height; ++y)
          for (int x = rect.x; x < rect.x + rect.width; ++x) EXPECT_EQ(background[y * 480 + x], 0) << x << ',' << y;
      }
    }
    renderer.pixelSink = {};
    renderer.opaqueReplay = false;
    renderer.setRenderMode(GfxRenderer::BW);
  }
}

TEST_F(PublisherBoxLayout, MinimumInheritanceStopsAtAnUnstyledImmediateParent) {
  makeBook("<div class='outer'><div class='lead'></div><div><div class='inner'></div></div></div><p>After</p>",
           ".outer{min-height:90px;background:#e7e7e9}.lead{height:90px}.inner{min-height:inherit}");
  for (const bool arena : {false, true}) {
    auto section = build(400, arena);
    EXPECT_EQ(words(*section).at("After").y, 90);
  }
}

TEST_F(PublisherBoxLayout, InheritedMinimumExcludesTheParentsPadding) {
  makeBook("<div class='outer'><div class='inner'></div></div><p>After</p>",
           ".outer{min-height:90px;padding:10px}.inner{min-height:inherit}");
  for (const bool arena : {false, true}) {
    auto section = build(400, arena);
    EXPECT_EQ(words(*section).at("After").y, 110);
  }
}
TEST_F(PublisherBoxLayout, TableSectionBorderDoesNotBecomeTextUnderline) {
  makeBook(
      "<table><tbody class='rule'><tr><td>Cell</td></tr></tbody></table>"
      "<p><span class='rule'>Inline</span> Plain</p>",
      ".rule{border-bottom:1px solid}");
  auto section = build();
  const auto found = words(*section);
  EXPECT_NE(found.at("Inline").style & EpdFontFamily::UNDERLINE, 0);
  EXPECT_EQ(found.at("Plain").style & EpdFontFamily::UNDERLINE, 0);
  section->currentPage = 0;
  auto page = section->loadPageFromSectionFile();
  ASSERT_NE(page, nullptr);
  bool sawCell = false;
  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageTable) continue;
    const auto& table = static_cast<const PageTableFragment&>(*element);
    for (const auto& row : table.getRows())
      for (const auto& cell : row.cells)
        for (const auto& line : cell.lines)
          for (uint16_t i = 0; i < line->wordCount(); ++i) {
            sawCell = true;
            EXPECT_EQ(line->wordStyle(i) & EpdFontFamily::UNDERLINE, 0);
          }
  }
  EXPECT_TRUE(sawCell);
}

TEST_F(PublisherBoxLayout, HiddenOnlyMinimumCollapsesButEmptyAndVisibleBoxesKeepHeight) {
  for (const auto& property : {"height", "min-height"}) {
    makeBook(
        "<div class='box'><p class='hidden'>Invisible</p></div><p>Afterhidden</p>"
        "<div class='box'></div><p>Afterempty</p>"
        "<div class='box'><span class='hidden'>Invisible</span><p>Visible</p></div><p>Aftervisible</p>",
        std::string(".box{") + property + ":90px}.hidden{color:transparent}");
    for (const bool arena : {false, true}) {
      auto section = build(500, arena);
      const auto found = words(*section);
      EXPECT_EQ(found.count("Invisible"), 0u);
      EXPECT_EQ(found.at("Afterhidden").y, 0);
      EXPECT_EQ(found.at("Afterempty").y, 114);
      EXPECT_EQ(found.at("Visible").y, 138);
      EXPECT_EQ(found.at("Aftervisible").y, 228);
    }
  }
}
TEST_F(PublisherBoxLayout, FirstLineInheritedTrackingPreservesTheParentsSpacing) {
  makeBook("<p>Alpha Bravo Charlie Delta Echo Foxtrot Golf Hotel India Juliet Kilo Lima Mike November</p>",
           "p{letter-spacing:2px}p::first-line{letter-spacing:inherit}");
  for (const bool arena : {false, true}) {
    auto section = build(400, arena);
    const auto found = words(*section);
    ASSERT_EQ(found.size(), 14u);
    EXPECT_GT(found.at("November").y, found.at("Alpha").y);
    for (const auto& entry : found) EXPECT_EQ(wordTypography::tracking(entry.second.typography), 32);
  }
}
TEST_F(PublisherBoxLayout, OversizedPrefixIconSharesTextAndReservesItsHeight) {
  makeBook(
      "<p>Beforeone</p><p>Beforetwo</p><p><span><img class='icon' src='image.png'/></span>Linktext</p>"
      "<p>Following</p>",
      ".icon{width:1.8em;height:auto}");
  for (const bool arena : {false, true}) {
    auto section = build(76, arena);
    const auto found = words(*section);
    EXPECT_EQ(found.at("Beforetwo").page, 0);
    EXPECT_EQ(found.at("Linktext").page, 1);
    const auto anchor = section->getSourceAnchorForPage(1);
    ASSERT_TRUE(anchor);
    EXPECT_GE(anchor->sourceOffset, xhtml.find("<img"));
    EXPECT_LE(anchor->sourceOffset, xhtml.find("Linktext"));
    section->currentPage = found.at("Linktext").page;
    auto page = section->loadPageFromSectionFile();
    ASSERT_NE(page, nullptr);
    const PageImage* icon = nullptr;
    for (const auto& el : page->elements)
      if (el->getTag() == TAG_PageImage) icon = static_cast<const PageImage*>(el.get());
    ASSERT_NE(icon, nullptr);
    EXPECT_GT(icon->getImageBlock().getRenderedHeight(), 24);
    EXPECT_EQ(icon->yPos, found.at("Linktext").y);
    EXPECT_GE(found.at("Linktext").x, icon->xPos + icon->getImageBlock().getWidth());
    EXPECT_GE(found.at("Following").page * 76 + found.at("Following").y,
              found.at("Linktext").page * 76 + icon->yPos + icon->getImageBlock().getRenderedHeight());
  }
}

TEST_F(PublisherBoxLayout, OversizedPrefixWithoutTextFallsBackToAnImageBlock) {
  makeBook("<p><span><img class='icon' src='image.png'/></span></p><p>Following</p>", ".icon{width:1.8em;height:auto}");
  for (const bool arena : {false, true}) {
    auto section = build(400, arena);
    const auto found = words(*section);
    ASSERT_EQ(section->pageCount, 1);
    section->currentPage = 0;
    auto page = section->loadPageFromSectionFile();
    ASSERT_NE(page, nullptr);
    const PageImage* icon = nullptr;
    for (const auto& el : page->elements)
      if (el->getTag() == TAG_PageImage) icon = static_cast<const PageImage*>(el.get());
    ASSERT_NE(icon, nullptr);
    EXPECT_GE(found.at("Following").y, icon->yPos + icon->getImageBlock().getRenderedHeight());
  }
}
TEST_F(PublisherBoxLayout, OversizedPrefixWrappedTextAndLargerFallbackDoNotOverlapImages) {
  for (const auto& width : {"1.8em", "4em"}) {
    makeBook(
        "<p><img class='icon' src='image.png'/>Alpha Bravo Charlie Delta Echo Foxtrot Golf Hotel India Juliet "
        "Kilo Lima Mike November Oscar Papa</p><p>Following</p>",
        std::string(".icon{width:") + width + ";height:auto}");
    auto section = build(400);
    const auto found = words(*section);
    section->currentPage = 0;
    auto page = section->loadPageFromSectionFile();
    ASSERT_NE(page, nullptr);
    const PageImage* icon = nullptr;
    for (const auto& el : page->elements)
      if (el->getTag() == TAG_PageImage) icon = static_cast<const PageImage*>(el.get());
    ASSERT_NE(icon, nullptr);
    const int imageBottom = icon->yPos + icon->getImageBlock().getRenderedHeight();
    if (std::string(width) == "1.8em") {
      EXPECT_EQ(found.at("Alpha").y, icon->yPos);
      EXPECT_GT(found.at("Papa").y, found.at("Alpha").y);
      for (const auto& entry : found)
        if (entry.second.y != found.at("Alpha").y) EXPECT_GE(entry.second.y, imageBottom);
    } else {
      EXPECT_GE(found.at("Alpha").y, imageBottom);
    }
  }
}

TEST_F(PublisherBoxLayout, FigureWrappersDoNotDonateLargeIconsToChildOrFollowingParagraphs) {
  for (const bool childCaption : {false, true}) {
    const std::string image = "<img src='image.png' width='40' height='40'/>";
    makeBook(childCaption ? "<div>" + image + "<p>Caption</p></div><p>Following</p>"
                          : "<div>" + image + "</div><p>Following</p>",
             "");
    for (const bool arena : {false, true}) {
      auto section = build(400, arena);
      const auto found = words(*section);
      section->currentPage = 0;
      auto page = section->loadPageFromSectionFile();
      ASSERT_NE(page, nullptr);
      const PageImage* picture = nullptr;
      for (const auto& element : page->elements) {
        if (element->getTag() == TAG_PageImage) picture = static_cast<const PageImage*>(element.get());
        if (element->getTag() == TAG_PageLine) {
          const auto& line = *static_cast<const PageLine&>(*element).getBlock();
          for (uint16_t i = 0; i < line.wordCount(); ++i) EXPECT_NE(std::string(line.wordText(i)), " ");
        }
      }
      ASSERT_NE(picture, nullptr);
      EXPECT_EQ(picture->xPos, 180);
      const int bottom = picture->yPos + picture->getImageBlock().getRenderedHeight();
      EXPECT_GE(found.at(childCaption ? "Caption" : "Following").y, bottom);
    }
  }
}
}  // namespace
