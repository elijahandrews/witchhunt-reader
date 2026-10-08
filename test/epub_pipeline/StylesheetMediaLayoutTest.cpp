#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/css/CssMedia.h"
#include "GfxRenderer.h"
#include "StoredZipWriter.h"

namespace {
class StylesheetMediaLayout : public testing::Test {
 protected:
  std::filesystem::path dir;
  std::shared_ptr<Epub> book;
  GfxRenderer renderer;
  std::vector<std::string> documents;
  void SetUp() override {
    dir = std::filesystem::temp_directory_path() / "witch-media-layout" /
          testing::UnitTest::GetInstance()->current_test_info()->name();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
  }
  void makeBook(const std::vector<std::string>& chapters, const std::string& base = "",
                const std::string& alternate = "") {
    documents = chapters;
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<container><rootfiles><rootfile full-path=\"content.opf\" "
            "media-type=\"application/oebps-package+xml\"/></rootfiles></container>");
    std::string manifest =
        "<item id=\"css\" href=\"base.css\" media-type=\"text/css\"/>"
        "<item id=\"alt\" href=\"alternate.css\" media-type=\"text/css\"/>";
    std::string spine;
    for (size_t i = 0; i < chapters.size(); ++i) {
      const auto n = std::to_string(i);
      manifest += "<item id=\"c" + n + "\" href=\"chapter" + n + ".xhtml\" media-type=\"application/xhtml+xml\"/>";
      spine += "<itemref idref=\"c" + n + "\"/>";
      zip.add("chapter" + n + ".xhtml", chapters[i]);
    }
    zip.add("content.opf",
            "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">"
            "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>Synthetic styles</dc:title></metadata>"
            "<manifest>" +
                manifest + "</manifest><spine>" + spine + "</spine></package>");
    zip.add("base.css", base);
    zip.add("alternate.css", alternate);
    const unsigned char png[] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0,
                                 0,    0,   15,  0,   0,  0,  15, 8,  0, 0, 0, 0,  0,   0,   0,   0};
    zip.add("marker.png", std::string(reinterpret_cast<const char*>(png), sizeof(png)));
    zip.write((dir / "styles.epub").string());
    reopen();
  }
  void reopen() {
    book = std::make_shared<Epub>((dir / "styles.epub").string(), (dir / "cache").string());
    ASSERT_TRUE(book->load(true));
  }
  static std::string html(const std::string& body, const std::string& extraHead = "",
                          const std::string& bodyStyle = "") {
    return "<html><head><title>Synthetic</title><link rel=\"stylesheet\" href=\"base.css\"/>" + extraHead +
           "</head><body style=\"" + bodyStyle + "\">" + body + "</body></html>";
  }
  Section::BuildParams params(int width = 180, int height = 144) {
    Section::BuildParams p;
    p.viewportWidth = width;
    p.viewportHeight = height;
    p.fontSizeNormalization = false;
    p.hyphenationEnabled = false;
    p.embeddedStyle = true;
    p.paragraphAlignment = static_cast<uint8_t>(CssTextAlign::None);
    return p;
  }
  std::unique_ptr<Section> build(int width = 180, int height = 144, SourceAnchor anchor = {}) {
    auto section = std::make_unique<Section>(book, 0, renderer);
    section->setSourceLookupTarget(anchor);
    const auto p = params(width, height);
    EXPECT_TRUE(section->createSectionFile(p, {}, true));
    EXPECT_TRUE(section->loadSectionFile(p));
    return section;
  }
  std::unique_ptr<Page> page(Section& section, int n = 0) {
    section.currentPage = n;
    return section.loadPageFromSectionFile();
  }
};
}  // namespace

TEST(CssMediaTypes, RejectsOtherDevicesAndRespectsListsNegationAndCase) {
  for (const auto* media : {"print", "AMZN-MOBI", "speech, print", "only amzn-mobi", "not screen", "not all"})
    EXPECT_FALSE(cssMedia::mayMatchScreen(media)) << media;
  for (const auto* media : {"", "all", "SCREEN", "only Screen", "print, screen", "not print", "not amzn-mobi"})
    EXPECT_TRUE(cssMedia::mayMatchScreen(media)) << media;
}

TEST(CssMediaTypes, KeepsViewportDependentQueriesConservatively) {
  EXPECT_TRUE(cssMedia::mayMatchScreen("screen and (min-width: 600px)"));
  EXPECT_TRUE(cssMedia::mayMatchScreen("not screen and (orientation: landscape)"));
  EXPECT_TRUE(cssMedia::mayMatchScreen("(orientation: portrait)"));
  EXPECT_FALSE(cssMedia::mayMatchScreen("amzn-mobi and (orientation: portrait)"));
}

TEST_F(StylesheetMediaLayout, ExcludesAlternateDeviceRulesAndHeadStylesAcrossCachedReopen) {
  makeBook({html("<p class=\"item\">Keeps Hanging Layout</p>",
                 "<link rel=\"stylesheet\" href=\"alternate.css\" media=\"amzn-mobi\"/>"
                 "<style media=\"AMZN-MOBI\">.item {font-weight:bold}</style>"
                 "<style media=\"print, only screen\">.item {font-style:italic}</style>")},
           ".item {margin-left:2em;text-indent:-1em;font-size:150%}",
           ".item {margin-left:0;text-indent:-40px;font-size:80%}");
  for (int pass = 0; pass < 2; ++pass) {
    if (pass) reopen();
    auto* css = book->getCssParser();
    ASSERT_TRUE(css->loadFromCache());
    std::string localPath;
    bool truncated = false;
    ASSERT_TRUE(book->documentStyleCache(0, localPath, truncated));
    ASSERT_FALSE(localPath.empty());
    CssParser local(localPath, css);
    ASSERT_TRUE(local.loadFromCache());
    const auto style = css->resolveStyle("p", "item", "", &local);
    EXPECT_EQ(style.marginLeft.value, 2.0f);
    EXPECT_EQ(style.fontSizeMultiplier, 1.5f);
    EXPECT_EQ(style.fontStyle, CssFontStyle::Italic);
    EXPECT_NE(style.fontWeight, CssFontWeight::Bold);
    auto section = build();
    auto first = page(*section);
    ASSERT_NE(first, nullptr);
    ASSERT_FALSE(first->elements.empty());
    const auto& line = static_cast<const PageLine&>(*first->elements.front());
    EXPECT_GT(line.xPos, 0);
    EXPECT_LT(line.getBlock()->wordXpos(0), 0);
  }
}

TEST_F(StylesheetMediaLayout, AnyApplicableReferenceKeepsSharedStylesheet) {
  makeBook({html("<p>One</p>", "<link rel=\"stylesheet\" href=\"alternate.css\" media=\"print\"/>"),
            html("<p>Two</p>", "<link rel=\"stylesheet\" href=\"alternate.css\" media=\"screen\"/>")},
           "p {font-weight:normal}", "p {font-weight:bold}");
  auto* css = book->getCssParser();
  ASSERT_TRUE(css->loadFromCache());
  EXPECT_EQ(css->resolveStyle("p", "", "").fontWeight, CssFontWeight::Bold);
}

TEST_F(StylesheetMediaLayout, EncodedStylesheetReferenceStillHonorsItsMediaType) {
  makeBook({html("<p>Content</p>", "<link rel=\"stylesheet\" href=\"alternate%2ecss\" media=\"amzn-mobi\"/>")},
           "p{font-weight:normal}", "p{font-weight:bold}");
  auto* css = book->getCssParser();
  ASSERT_TRUE(css->loadFromCache());
  EXPECT_EQ(css->resolveStyle("p", "", "").fontWeight, CssFontWeight::Normal);
}

TEST_F(StylesheetMediaLayout, TransientInventoryMarkerForcesRetryOnNextOpen) {
  makeBook({html("<p>Content</p>")}, "p{font-style:italic}");
  const auto marker = book->getCachePath() + "/doc_css/.media_retry";
  std::ofstream(marker).put(1);
  ASSERT_TRUE(std::filesystem::exists(marker));
  reopen();
  EXPECT_FALSE(std::filesystem::exists(marker));
  auto* css = book->getCssParser();
  ASSERT_TRUE(css->loadFromCache());
  EXPECT_EQ(css->resolveStyle("p", "", "").fontStyle, CssFontStyle::Italic);
}

TEST_F(StylesheetMediaLayout, IncompleteHeadDoesNotExcludePotentiallyApplicableStylesheet) {
  makeBook({html("<p>One</p>", "<link rel=\"stylesheet\" href=\"alternate.css\" media=\"print\"/>"),
            "<html><head><title>" + std::string(66000, 'a') + "</title></head><body><p>Two</p></body></html>"},
           "p {font-weight:normal}", "p {font-weight:bold}");
  auto* css = book->getCssParser();
  ASSERT_TRUE(css->loadFromCache());
  EXPECT_EQ(css->resolveStyle("p", "", "").fontWeight, CssFontWeight::Bold);
}

TEST_F(StylesheetMediaLayout, OldDocumentIndexInvalidatesPreviouslyMergedRules) {
  makeBook({html("<p>Content</p>")}, "p{font-style:italic}");
  const auto index = book->getCachePath() + "/doc_css/index.bin";
  {
    std::fstream out(index, std::ios::binary | std::ios::in | std::ios::out);
    out.put(1);
  }
  reopen();
  std::ifstream in(index, std::ios::binary);
  EXPECT_EQ(in.get(), 2);
}

TEST_F(StylesheetMediaLayout, BodyMarginsComposeOnceWithNestedInsetsAndInlineReset) {
  makeBook({html("<div><p>Alpha beta gamma delta epsilon zeta eta theta</p></div>")},
           "body{margin:1em} div{margin-left:10px;margin-right:12px} p{margin:0;text-indent:0;text-align:left}");
  auto section = build();
  auto first = page(*section);
  ASSERT_NE(first, nullptr);
  ASSERT_GE(first->elements.size(), 2u);
  const auto& line = static_cast<const PageLine&>(*first->elements.front());
  EXPECT_EQ(line.xPos, 28);
  EXPECT_EQ(line.yPos, 18);
  EXPECT_EQ(line.getBlock()->wordXpos(0), 0);
  for (const auto& element : first->elements) {
    const auto& row = static_cast<const PageLine&>(*element);
    EXPECT_EQ(row.xPos, 28);
    EXPECT_LE(row.getBlock()->wordXpos(row.getBlock()->wordCount() - 1), 122);
  }
  makeBook({html("<p>Reset body</p>", "", "margin:0")}, "body{margin:1em} p{margin:0;text-indent:0}");
  section = build();
  first = page(*section);
  const auto& reset = static_cast<const PageLine&>(*first->elements.front());
  EXPECT_EQ(reset.xPos, 0);
  EXPECT_EQ(reset.yPos, 0);
}

TEST_F(StylesheetMediaLayout, SmallPrefixImageSharesFirstLineAndPreservesHangingIndent) {
  makeBook({html("<p><img src=\"marker.png\" width=\"15\" height=\"15\"/><strong>Marker</strong> "
                 "alpha beta gamma delta epsilon zeta eta theta iota</p>")},
           "body{margin:1em}p{margin:0 0 0 2em;text-indent:-2em;text-align:left}");
  auto section = build();
  auto first = page(*section);
  ASSERT_NE(first, nullptr);
  ASSERT_GE(first->elements.size(), 3u);
  ASSERT_EQ(first->elements[0]->getTag(), TAG_PageImage);
  ASSERT_EQ(first->elements[1]->getTag(), TAG_PageLine);
  const auto& image = static_cast<const PageImage&>(*first->elements[0]);
  const auto& line = static_cast<const PageLine&>(*first->elements[1]);
  const auto& second = static_cast<const PageLine&>(*first->elements[2]);
  EXPECT_EQ(image.xPos, 18);
  EXPECT_EQ(image.yPos, line.yPos + 3);
  EXPECT_EQ(line.xPos + line.getBlock()->wordXpos(0), 33);
  EXPECT_NE(line.getBlock()->wordStyle(0) & EpdFontFamily::BOLD, 0);
  EXPECT_EQ(second.xPos + second.getBlock()->wordXpos(0), 54);
  EXPECT_EQ(second.yPos, line.yPos + 24);
}

TEST_F(StylesheetMediaLayout, PrefixImageMovesWithItsTextAndAnchorAcrossPageSizes) {
  const auto text = html(
      "<p>Before</p><p class=\"marked\"><img src=\"marker.png\" width=\"15\" height=\"15\"/>"
      "<strong>AnchorWord</strong> alpha beta gamma delta epsilon zeta eta theta iota kappa</p>");
  makeBook({text},
           "p{margin:0;text-indent:0;text-align:left}.marked{margin-top:18px;margin-left:30px;text-indent:-30px}");
  auto section = build(180, 48);
  ASSERT_GT(section->pageCount, 1);
  auto first = page(*section);
  ASSERT_EQ(first->elements.size(), 1u);
  auto next = page(*section, 1);
  ASSERT_GE(next->elements.size(), 2u);
  EXPECT_EQ(next->elements[0]->getTag(), TAG_PageImage);
  const auto& line = static_cast<const PageLine&>(*next->elements[1]);
  EXPECT_STREQ(line.getBlock()->wordText(0), "AnchorWord");
  const auto anchor = section->getSourceAnchorForPage(1);
  ASSERT_TRUE(anchor);
  EXPECT_EQ(anchor->sourceOffset, text.find("AnchorWord"));
  auto resized = build(240, 96, *anchor);
  ASSERT_TRUE(resized->sourceLookupPage());
}

TEST_F(StylesheetMediaLayout, PrefixKeepsAuthoredSpacesWithoutInventingAnExtraGap) {
  const std::pair<const char*, int> cases[] = {{"", 15}, {" \n\t ", 21}, {"&#160;&#160;&#160;&#160;", 39}};
  for (const auto& [space, expected] : cases) {
    makeBook({html(std::string("<p><img src=\"marker.png\" width=\"15\" height=\"15\"/>") + space +
                   "<strong>Marker</strong> following text</p>")},
             "p{margin:0;text-indent:0;text-align:left}");
    auto section = build(280);
    auto first = page(*section);
    ASSERT_GE(first->elements.size(), 2u);
    const auto& image = static_cast<const PageImage&>(*first->elements[0]);
    const auto& line = static_cast<const PageLine&>(*first->elements[1]);
    EXPECT_EQ(image.xPos, 0);
    bool found = false;
    for (uint16_t i = 0; i < line.getBlock()->wordCount(); ++i) {
      if (std::string(line.getBlock()->wordText(i)) != "Marker") continue;
      found = true;
      EXPECT_EQ(line.xPos + line.getBlock()->wordXpos(i), expected) << space;
    }
    EXPECT_TRUE(found);
  }
}

TEST_F(StylesheetMediaLayout, MarkerOnlyParagraphDoesNotStealNextParagraph) {
  makeBook({html("<p><img src=\"marker.png\" width=\"15\" height=\"15\"/></p><p>AfterMarker</p>")},
           "p{margin:0;text-indent:0;text-align:left}");
  auto section = build();
  auto first = page(*section);
  ASSERT_GE(first->elements.size(), 3u);
  const auto& image = static_cast<const PageImage&>(*first->elements[0]);
  const auto& last = static_cast<const PageLine&>(*first->elements.back());
  EXPECT_EQ(image.xPos, 0);
  EXPECT_STREQ(last.getBlock()->wordText(0), "AfterMarker");
  EXPECT_EQ(last.xPos + last.getBlock()->wordXpos(0), 0);
  EXPECT_GT(last.yPos, image.yPos);
}

TEST_F(StylesheetMediaLayout, AdjacentImagesKeepSourceOrderWhenPrefixSlotIsOccupied) {
  makeBook({html("<p><img src=\"marker.png\" width=\"12\" height=\"12\"/>"
                 "<img src=\"marker.png\" width=\"15\" height=\"15\"/>AfterImages</p>")},
           "p{margin:0;text-indent:0;text-align:left}");
  auto section = build();
  auto first = page(*section);
  int previousY = -1, count = 0;
  for (const auto& element : first->elements) {
    if (element->getTag() != TAG_PageImage) continue;
    const auto& image = static_cast<const PageImage&>(*element);
    EXPECT_GE(image.yPos, previousY);
    previousY = image.yPos;
    EXPECT_EQ(image.getImageBlock().getWidth(), count ? 15 : 12);
    ++count;
  }
  EXPECT_EQ(count, 2);
}
