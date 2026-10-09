#include <Arduino.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
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
    return build(params(width, height), anchor);
  }
  std::unique_ptr<Section> build(const Section::BuildParams& p, SourceAnchor anchor = {}) {
    auto section = std::make_unique<Section>(book, 0, renderer);
    section->setSourceLookupTarget(anchor);
    EXPECT_TRUE(section->createSectionFile(p, {}, true));
    EXPECT_TRUE(section->loadSectionFile(p));
    return section;
  }
  // Layout-only signature: ignore source byte offsets so chapters with different
  // outer style declarations can be compared. All content here is synthetic text.
  std::string textLayout(Section& section) {
    std::ostringstream out;
    for (uint16_t n = 0; n < section.pageCount; ++n) {
      out << "PAGE " << n << '\n';
      auto loaded = page(section, n);
      EXPECT_NE(loaded, nullptr);
      if (!loaded) return {};
      for (const auto& element : loaded->elements) {
        EXPECT_EQ(element->getTag(), TAG_PageLine);
        if (element->getTag() != TAG_PageLine) continue;
        const auto& line = static_cast<const PageLine&>(*element);
        const auto& block = *line.getBlock();
        const auto& style = block.getRenderStyle();
        out << line.xPos << ',' << line.yPos << ':' << style.fontSizeMultiplier << ',' << style.headingFontId << ','
            << static_cast<int>(style.alignment) << '\n';
        for (uint16_t i = 0; i < block.wordCount(); ++i)
          out << block.wordText(i) << ':' << block.wordXpos(i) << ',' << static_cast<int>(block.wordStyle(i)) << ','
              << static_cast<int>(block.wordSizePct(i)) << ',' << block.wordTypography(i) << '\n';
      }
    }
    return out.str();
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

TEST_F(StylesheetMediaLayout, PublisherMarginOverridePreservesNestedLayoutAndTypography) {
  const std::string content =
      "<div class=\"page\"><h2>Beacon heading</h2>"
      "<p>First paragraph with an <em>emphasized phrase</em> and several words to wrap.</p>"
      "<blockquote><p>Quiet quotation with its own inset and spacing.</p></blockquote>"
      "<ol><li>First list entry</li><li>Second list entry</li></ol>"
      "<p class=\"hanging\">Marker alpha beta gamma delta epsilon zeta eta theta iota.</p></div>";
  const auto chapter = [&](const std::string& rootStyle) {
    return "<html style=\"margin:2em;padding:1em\"><head>"
           "<link rel=\"stylesheet\" href=\"base.css\"/></head>"
           "<body class=\"page\" style=\"" +
           rootStyle + "\">" + content + "</body></html>";
  };
  makeBook({chapter("margin:31px 43px;padding:7px 9px"), chapter("margin:0;padding:0")},
           ".page{margin:11px;padding:3px} body{font-style:italic;text-transform:uppercase}"
           "h2{font-size:150%;font-weight:bold;margin:20px 0 12px}"
           "p{margin:0 0 6px;text-indent:1em;text-align:left}"
           "blockquote{margin:14px 17px;padding:2px 4px}"
           "ol{margin:8px 9px;padding-left:20px}"
           ".hanging{margin-left:2em;text-indent:-1em}");
  auto authored = build(240, 240);
  auto p = params(240, 240);
  p.ignorePublisherPageMargins = true;
  auto overridden = build(p);
  Section zeroOuterBox(book, 1, renderer);
  ASSERT_TRUE(zeroOuterBox.createSectionFile(params(240, 240), {}, true));
  ASSERT_TRUE(zeroOuterBox.loadSectionFile(params(240, 240)));
  // Independent control: the same content with explicit zero outer spacing. Compare
  // every line, word position, face and size through serialization/cache read-back.
  EXPECT_EQ(textLayout(*overridden), textLayout(zeroOuterBox));
  EXPECT_NE(textLayout(*authored), textLayout(*overridden));
  auto first = page(*overridden);
  ASSERT_FALSE(first->elements.empty());
  ASSERT_EQ(first->elements.front()->getTag(), TAG_PageLine);
  const auto& heading = static_cast<const PageLine&>(*first->elements.front());
  EXPECT_EQ(heading.xPos, 14);  // shared .page class still insets the nested div
  EXPECT_STREQ(heading.getBlock()->wordText(0), "BEACON");
  EXPECT_NE(heading.getBlock()->wordStyle(0) & EpdFontFamily::BOLD, 0);
  EXPECT_NE(heading.getBlock()->wordStyle(0) & EpdFontFamily::ITALIC, 0);
  EXPECT_FLOAT_EQ(heading.getBlock()->getRenderStyle().fontSizeMultiplier, 1.5f);
}

TEST_F(StylesheetMediaLayout, PublisherMarginOverrideWinsAfterLocalAndInlineCascade) {
  makeBook({html("<p>First paragraph</p><p class=\"page\">Indented paragraph</p>",
                 "<style>body{margin:7% !important;padding:9px !important}</style>",
                 "margin:3em !important;padding:12px !important")},
           "body{margin:2em;padding:1em}p{margin:0;text-indent:1em}"
           ".page{margin-left:13px;padding-left:5px}");
  auto p = params(400, 240);
  p.ignorePublisherPageMargins = true;
  auto section = build(p);
  auto first = page(*section);
  ASSERT_EQ(first->elements.size(), 2u);
  const auto& plain = static_cast<const PageLine&>(*first->elements[0]);
  const auto& inset = static_cast<const PageLine&>(*first->elements[1]);
  EXPECT_EQ(plain.xPos, 0);
  EXPECT_EQ(plain.yPos, 0);
  EXPECT_EQ(plain.getBlock()->wordXpos(0), 18);  // paragraph indent survives
  EXPECT_EQ(inset.xPos, 18);                     // descendant margin + padding survive
  EXPECT_EQ(inset.getBlock()->wordXpos(0), 18);
}

TEST_F(StylesheetMediaLayout, PublisherMarginToggleSeparatesCachesAndPreservesSourceLookup) {
  std::string words;
  for (int i = 0; i < 400; ++i) words += "TOKEN" + std::to_string(i) + " ";
  makeBook({html("<p>" + words + "</p>")},
           "body{margin:0 36px;padding:0 6px}p{margin:0;text-indent:0;text-align:left}");
  const auto originalParams = params(240, 144);
  auto original = build(originalParams);
  const auto originalLayout = textLayout(*original);
  const uint16_t middle = original->pageCount / 2;
  const auto anchor = original->getSourceAnchorForPage(middle);
  ASSERT_TRUE(anchor);
  auto originalPage = page(*original, middle);
  const std::string target = static_cast<const PageLine&>(*originalPage->elements.front()).getBlock()->wordText(0);

  auto overrideParams = originalParams;
  overrideParams.ignorePublisherPageMargins = true;
  Section uncached(book, 0, renderer);
  EXPECT_FALSE(uncached.loadSectionFile(overrideParams));
  auto overridden = build(overrideParams, *anchor);
  EXPECT_LT(overridden->pageCount, original->pageCount);  // both side insets are released
  const auto found = overridden->sourceLookupPage();
  ASSERT_TRUE(found);
  auto foundPage = page(*overridden, *found);
  bool targetVisible = false;
  for (const auto& element : foundPage->elements) {
    ASSERT_EQ(element->getTag(), TAG_PageLine);
    const auto& line = static_cast<const PageLine&>(*element);
    EXPECT_EQ(line.xPos, 0);
    for (uint16_t i = 0; i < line.getBlock()->wordCount(); ++i) targetVisible |= target == line.getBlock()->wordText(i);
  }
  EXPECT_TRUE(targetVisible);
  // A warm cache and toggling back must select their own original layouts.
  reopen();
  Section warm(book, 0, renderer);
  ASSERT_TRUE(warm.loadSectionFile(overrideParams));
  EXPECT_EQ(textLayout(warm), textLayout(*overridden));
  Section restored(book, 0, renderer);
  ASSERT_TRUE(restored.loadSectionFile(originalParams));
  EXPECT_EQ(textLayout(restored), originalLayout);

  // With publisher styles disabled, the toggle must not create another variant.
  auto noCss = originalParams;
  noCss.embeddedStyle = false;
  auto plain = build(noCss);
  noCss.ignorePublisherPageMargins = true;
  Section plainWarm(book, 0, renderer);
  ASSERT_TRUE(plainWarm.loadSectionFile(noCss));
  EXPECT_EQ(textLayout(*plain), textLayout(plainWarm));
}

TEST_F(StylesheetMediaLayout, PublisherMarginToggleRestartsAnIncrementalBuild) {
  std::string content;
  for (int i = 0; i < 200; ++i) content += "<p>Paragraph " + std::to_string(i) + " with several words.</p>";
  makeBook({html(content)}, "body{margin:1em;padding:6px}p{margin:0;text-indent:0}");
  auto p = params(240, 144);
  Section sliced(book, 0, renderer);
  const host_clock::Ticking tick(1);
  ASSERT_EQ(sliced.stepSectionBuild(p, 1, {}, true), Section::BuildStep::More);
  ASSERT_TRUE(sliced.hasActiveBuild());
  p.ignorePublisherPageMargins = true;
  auto result = Section::BuildStep::More;
  int slices = 0;
  while (result == Section::BuildStep::More && ++slices < 20000) result = sliced.stepSectionBuild(p, 1, {}, true);
  ASSERT_EQ(result, Section::BuildStep::Done);
  ASSERT_GT(slices, 1);
  ASSERT_TRUE(sliced.loadSectionFile(p));
  const auto slicedLayout = textLayout(sliced);
  auto synchronous = build(p);
  EXPECT_EQ(slicedLayout, textLayout(*synchronous));
  auto first = page(sliced);
  ASSERT_FALSE(first->elements.empty());
  EXPECT_EQ(first->elements.front()->xPos, 0);
  EXPECT_EQ(first->elements.front()->yPos, 0);
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
