#include <gtest/gtest.h>

#include <filesystem>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "Epub/css/CssFontCatalog.h"
#include "PipelineRunner.h"

namespace {
std::string buildTypography(const std::string& tag, const bool embeddedStyle = true, const bool warm = false,
                            const int readerFont = 1, const uint16_t viewportHeight = 760) {
  const auto dir = std::filesystem::temp_directory_path() / "css_typography_test" / tag;
  if (!warm) std::filesystem::remove_all(dir);
  pipeline_harness::Profile profile;
  profile.embeddedStyle = embeddedStyle;
  profile.fontId = readerFont;
  profile.viewportHeight = viewportHeight;
  profile.fontSizeNormalization = false;
  std::ostringstream dump;
  EXPECT_TRUE(
      pipeline_harness::runAndDump(std::string(CORPUS_DIR) + "/test_css_typography.epub", dir.string(), profile, dump));
  return dump.str();
}
std::vector<int> linePositions(const std::string& dump, const std::string& label) {
  const std::regex pattern("LINE y=([0-9]+)[^\n]*words=2\n   W[^\n]*t=" + label + "\n   W[^\n]*t=(one|two|three)\n");
  std::vector<int> result;
  for (auto i = std::sregex_iterator(dump.begin(), dump.end(), pattern); i != std::sregex_iterator(); ++i)
    result.push_back(std::stoi((*i)[1]));
  return result;
}
// The page cache now stores named fallback-stack IDs. Verify their identity/order and
// generic fallback rather than treating them as the old two-bit generic enum.
int namedFamily(const std::string& dump, const std::string& tag, const std::string& label,
                const uint8_t expectedFallback) {
  std::smatch match;
  const std::regex word("W[^\\n]* f=([0-9]+) ls=[^ ]+ t=" + label + "(\\n| )");
  EXPECT_TRUE(std::regex_search(dump, match, word)) << label;
  if (match.empty()) return -1;
  const int id = std::stoi(match[1]);
  EXPECT_GE(id, CssFontCatalog::FIRST_NAMED_ID);
  const auto root = std::filesystem::temp_directory_path() / "css_typography_test" / tag;
  bool found = false;
  for (const auto& directory : std::filesystem::directory_iterator(root)) {
    if (!directory.is_directory()) continue;
    CssFontCatalog catalog;
    if (!catalog.load(directory.path().string())) continue;
    const auto* stack = catalog.stack(id);
    EXPECT_NE(stack, nullptr);
    EXPECT_EQ(catalog.fallbackFamily(id), expectedFallback);
    if (stack) {
      EXPECT_GE(stack->families.size(), 2u);
      EXPECT_EQ(stack->families.back().generic, expectedFallback);
    }
    found = true;
  }
  EXPECT_TRUE(found);
  return id;
}
}  // namespace

TEST(CssTypography, TransformsBeforeLayoutAndSurvivesWarmCache) {
  const auto dump = buildTypography("case");
  EXPECT_NE(dump.find("t=MONDAY,"), std::string::npos);
  EXPECT_EQ(dump.find("t=Monday,"), std::string::npos);
  EXPECT_NE(dump.find("t=STRASSE"), std::string::npos);
  EXPECT_NE(dump.find("s=64 z=100 t=mixed"), std::string::npos);
  EXPECT_NE(dump.find("t=OUTER"), std::string::npos);
  EXPECT_NE(dump.find("t=EPSILON"), std::string::npos);
  EXPECT_NE(dump.find("t=Outside"), std::string::npos);
  EXPECT_NE(dump.find("t=PRE\n"), std::string::npos);
  EXPECT_NE(dump.find("t=POST\n"), std::string::npos);
  EXPECT_NE(dump.find("t=DELTA"), std::string::npos);
  EXPECT_NE(dump.find("t=delta"), std::string::npos);
  EXPECT_NE(dump.find("t=D\n"), std::string::npos);
  EXPECT_NE(dump.find("t=q\n"), std::string::npos);
  EXPECT_NE(dump.find("t=Ϊ́Ϊ́Ϊ́"), std::string::npos);
  EXPECT_NE(dump.find("t=ABCDEFGHIJKLMNÉÉÉ"), std::string::npos);
  EXPECT_NE(dump.find("t=ÉREST"), std::string::npos);
  EXPECT_NE(dump.find("t=ḾREST"), std::string::npos);
  EXPECT_NE(dump.find("t=🦊REST"), std::string::npos);
  EXPECT_EQ(dump, buildTypography("case", true, true));
}

TEST(CssTypography, InheritedSpacingResetsAtElementBoundary) {
  const auto dump = buildTypography("spacing");
  for (const auto& [label, step] : std::vector<std::pair<std::string, int>>{{"Tight", 19},
                                                                            {"Normal", 24},
                                                                            {"Loose", 36},
                                                                            {"Reset", 24},
                                                                            {"Outside", 24},
                                                                            {"Inherited", 36},
                                                                            {"Sibling", 24},
                                                                            {"Trailing", 24},
                                                                            {"Nested", 24},
                                                                            {"Section", 36},
                                                                            {"BlankLoose", 72},
                                                                            {"BlankTight", 38}}) {
    const auto y = linePositions(dump, label);
    ASSERT_EQ(y.size(), 3u) << label;
    EXPECT_EQ(y[1] - y[0], step) << label;
    EXPECT_EQ(y[2] - y[1], step) << label;
  }
}

TEST(CssTypography, EmbeddedStyleOffPreservesSourceCase) {
  const auto dump = buildTypography("disabled", false);
  EXPECT_NE(dump.find("t=Monday,"), std::string::npos);
  EXPECT_EQ(dump.find("t=MONDAY,"), std::string::npos);
  EXPECT_NE(dump.find("t=outer"), std::string::npos);
  EXPECT_EQ(dump.find("t=OUTER"), std::string::npos);
}

TEST(CssTypography, GenericFamiliesAreScopedAndSurvivePageCache) {
  const auto dump = buildTypography("families");
  const int serif = namedFamily(dump, "families", "SerifBody", wordTypography::Serif);
  const int sans = namedFamily(dump, "families", "SansInline", wordTypography::SansSerif);
  EXPECT_NE(serif, sans);
  EXPECT_EQ(namedFamily(dump, "families", "SerifOverride", wordTypography::Serif), serif);
  for (const auto* label : {"SansBold", "SansItalic", "SansRestored", "SansCell", "N", "Q"})
    EXPECT_EQ(namedFamily(dump, "families", label, wordTypography::SansSerif), sans) << label;
  EXPECT_NE(dump.find("s=1 z=100 f=" + std::to_string(sans) + " ls=0 t=SansBold"), std::string::npos);
  EXPECT_NE(dump.find("s=2 z=100 f=" + std::to_string(sans) + " ls=0 t=SansItalic"), std::string::npos);
  EXPECT_NE(dump.find("z=250 f=" + std::to_string(sans) + " ls=0 t=Q\n"), std::string::npos);
  EXPECT_NE(dump.find("z=100 t=ReaderDefault"), std::string::npos);
  EXPECT_NE(dump.find("ROW h=40"), std::string::npos);
  EXPECT_EQ(dump, buildTypography("families", true, true));
  EXPECT_EQ(buildTypography("families-disabled", false).find(" f="), std::string::npos);
}
TEST(CssTypography, TrackingIsComputedThenInheritedAcrossDifferentSizes) {
  const auto dump = buildTypography("tracking");
  EXPECT_NE(dump.find("f=0 ls=29 t=TrackedInline"), std::string::npos);
  EXPECT_NE(dump.find("z=100 f=0 ls=0 t=ResetInline"), std::string::npos);
  EXPECT_NE(dump.find("f=0 ls=29 t=TrackedAgain"), std::string::npos);
  EXPECT_NE(dump.find("z=100 f=0 ls=0 t=NormalAfter"), std::string::npos);
  EXPECT_NE(dump.find("f=0 ls=32 t=FixedTracking"), std::string::npos);
  EXPECT_NE(dump.find("z=150 f=0 ls=32 t=LargerInherited"), std::string::npos);
  EXPECT_NE(dump.find("f=0 ls=32 t=SameTracking"), std::string::npos);
  EXPECT_NE(dump.find("f=0 ls=-14 t=NegativeSpacing"), std::string::npos);
  const int sans = namedFamily(dump, "tracking", "SansTracked", wordTypography::SansSerif);
  EXPECT_NE(dump.find("f=" + std::to_string(sans) + " ls=29 t=SansTracked"), std::string::npos);
  EXPECT_EQ(dump, buildTypography("tracking", true, true));
}

TEST(CssTypography, BlankLinesKeepTheirCapturedFamilyAndCssSpacing) {
  for (const int bodyFont : {1, -2000000}) {
    const std::string tag = "blank-families-" + std::to_string(bodyFont);
    const auto dump = buildTypography(tag, true, false, bodyFont, 3000);
    for (const auto& [label, step] : std::vector<std::pair<std::string, int>>{{"BlankSans", 60},
                                                                              {"BlankSerif", 48},
                                                                              {"HeldSans", 60},
                                                                              {"HeldSerif", 48},
                                                                              {"CrossSans", 60},
                                                                              {"CrossSerif", 48},
                                                                              {"LooseSans", 90},
                                                                              {"LooseSerif", 72}}) {
      const auto y = linePositions(dump, label);
      ASSERT_GE(y.size(), 2u) << label;
      for (size_t i = 1; i < y.size(); ++i) EXPECT_EQ(y[i] - y[i - 1], step) << label << " reader " << bodyFont;
    }
    EXPECT_EQ(dump, buildTypography(tag, true, true, bodyFont, 3000));
  }
}

TEST(CssTypography, ReducedUppercaseRegressionRetainsSizeAndFourFaces) {
  const auto dump = buildTypography("reduced-uppercase");
  // A uniform 75% span is promoted to the line multiplier; per-word z remains
  // 100. Verify the effective scale together with the case and face selection.
  for (int style = 0; style < 4; ++style) {
    const std::regex sample("LINE[^\n]*mult=0[.]750[^\n]*\n   W[^\n]*s=" + std::to_string(style) +
                            " z=100 t=CAPITALS\n");
    EXPECT_TRUE(std::regex_search(dump, sample));
  }
  const std::regex alphabet("LINE[^\n]*mult=0[.]750[^\n]*\n   W[^\n]*s=0 z=100 t=ABCDEFGHIJKLMNOPQRSTUVWXYZ\n");
  EXPECT_TRUE(std::regex_search(dump, alphabet));
  EXPECT_EQ(dump, buildTypography("reduced-uppercase", true, true));
}
