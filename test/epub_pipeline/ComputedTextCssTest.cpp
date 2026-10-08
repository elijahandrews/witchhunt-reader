#include <gtest/gtest.h>

#include <filesystem>
#include <regex>
#include <sstream>
#include <string>

#include "EpdFontFamily.h"
#include "PipelineRunner.h"
#include "StoredZipWriter.h"

namespace {
class ComputedTextCss : public testing::Test {
 protected:
  std::filesystem::path root;
  void SetUp() override {
    root = std::filesystem::temp_directory_path() / "witch-computed-text-css" /
           testing::UnitTest::GetInstance()->current_test_info()->name();
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
  }
  std::string layout(const std::string& variant, const std::string& css, const std::string& body, bool warm = false) {
    const auto dir = root / variant;
    std::filesystem::create_directories(dir);
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
    zip.add("chapter.xhtml", "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Synthetic</title><style>" +
                                 css + "</style></head><body>" + body + "</body></html>");
    if (!warm) zip.write((dir / "book.epub").string());
    pipeline_harness::Profile profile;
    profile.fontSizeNormalization = false;
    profile.inlineFootnotePreviews = false;
    std::ostringstream dump;
    EXPECT_TRUE(pipeline_harness::runAndDump((dir / "book.epub").string(), (dir / "cache").string(), profile, dump));
    return dump.str();
  }
  static int style(const std::string& dump, const std::string& word) {
    std::smatch match;
    const std::regex record("W x=[^ ]+ s=([0-9]+) [^\\n]* t=" + word + "\\n");
    EXPECT_TRUE(std::regex_search(dump, match, record));
    return match.empty() ? -1 : std::stoi(match[1]);
  }
};

TEST_F(ComputedTextCss, ExplicitLinkDecorationWinsWithoutRemovingNavigationOrDefaultUnderline) {
  const std::string css = "p{text-indent:0}a.plain{text-decoration:none}a.strike{text-decoration:line-through}";
  const std::string body =
      "<p><a href=\"#target\">Default</a> <a class=\"plain\" href=\"#target\">Plain</a> "
      "<a class=\"strike\" href=\"#target\">Strike</a> "
      "<a style=\"text-decoration:none\" href=\"#target\">Inline</a> "
      "<u><a class=\"plain\" href=\"#target\">Ancestor</a></u> Outside</p>"
      "<p id=\"target\">Destination</p>";
  const auto cold = layout("links", css, body);
  EXPECT_NE(style(cold, "Default") & EpdFontFamily::UNDERLINE, 0);
  EXPECT_EQ(style(cold, "Plain") & EpdFontFamily::UNDERLINE, 0);
  EXPECT_EQ(style(cold, "Inline") & EpdFontFamily::UNDERLINE, 0);
  EXPECT_EQ(style(cold, "Strike") & EpdFontFamily::UNDERLINE, 0);
  EXPECT_NE(style(cold, "Strike") & EpdFontFamily::STRIKETHROUGH, 0);
  EXPECT_NE(style(cold, "Ancestor") & EpdFontFamily::UNDERLINE, 0);
  EXPECT_EQ(style(cold, "Outside") & EpdFontFamily::UNDERLINE, 0);
  EXPECT_NE(cold.find("FN n=Plain href=#target"), std::string::npos);
  EXPECT_EQ(cold, layout("links", css, body, true));
}

TEST_F(ComputedTextCss, EnlargedEmMarginsAndPaddingMatchComputedPixels) {
  // The deterministic host font has an 18px em; 150% therefore resolves 1em to 27px.
  const std::string body = "<p class=\"scaled\">Heading</p><p>Following</p>";
  const std::string relative = "p{text-indent:0}.scaled{font-size:150%;margin:1em;padding:.5em;text-indent:-.5em}";
  const std::string fixed = "p{text-indent:0}.scaled{font-size:150%;margin:27px;padding:13.5px;text-indent:-13.5px}";
  const auto expected = layout("pixels", fixed, body);
  const auto cold = layout("relative", relative, body);
  EXPECT_EQ(cold, expected);
  EXPECT_EQ(cold, layout("relative", relative, body, true));
}

TEST_F(ComputedTextCss, ReducedEmHangingIndentMatchesComputedPixels) {
  const std::string body =
      "<p class=\"list\">Marker alpha beta gamma delta epsilon zeta eta theta iota kappa lambda</p>";
  const auto expected =
      layout("pixels", ".list{font-size:90%;margin-left:29.808px;margin-right:16.2px;text-indent:-29.808px}", body);
  EXPECT_EQ(layout("relative", ".list{font-size:90%;margin-left:1.84em;margin-right:1em;text-indent:-1.84em}", body),
            expected);
}

TEST_F(ComputedTextCss, InlineEmIndentComposesWithTheParentFontSize) {
  const std::string body = "<p class=\"large\"><span class=\"small\">Indented</span><br/>Following</p>";
  const std::string common = ".large{font-size:150%;text-indent:0}.small{font-size:50%;";
  const auto expected = layout("pixels", common + "margin-left:13.5px}", body);
  const auto cold = layout("relative", common + "margin-left:1em}", body);
  EXPECT_EQ(cold, expected);
  EXPECT_EQ(cold, layout("relative", common + "margin-left:1em}", body, true));
}

TEST_F(ComputedTextCss, DeepEnlargedScopesKeepComputedInsetsRepresentable) {
  std::string body;
  for (int i = 0; i < 24; ++i) body += "<div class=\"large\">";
  body += "<p>Bounded</p>";
  for (int i = 0; i < 24; ++i) body += "</div>";
  const auto cold = layout("deep", ".large{font-size:200%;margin-left:100em;padding-left:100em}p{text-indent:0}", body);
  EXPECT_NE(cold.find("t=Bounded"), std::string::npos);
  EXPECT_EQ(cold.find("LINE y=-"), std::string::npos);
  EXPECT_EQ(cold.find(" x=-"), std::string::npos);
  EXPECT_EQ(cold,
            layout("deep", ".large{font-size:200%;margin-left:100em;padding-left:100em}p{text-indent:0}", body, true));
}

TEST_F(ComputedTextCss, RemStaysAtRootSizeWhileHeadingEmUsesItsOwnSize) {
  const std::string body = "<p class=\"heading\">Heading</p><p>Following</p>";
  const auto expected =
      layout("pixels", "p{text-indent:0}.heading{font-size:170%;margin:18px;padding-bottom:122.4px}", body);
  EXPECT_EQ(layout("relative", "p{text-indent:0}.heading{font-size:170%;margin:1rem;padding-bottom:4em}", body),
            expected);
}
}  // namespace
