#include <gtest/gtest.h>

#include <filesystem>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "PipelineRunner.h"

namespace {
std::string buildTypography(const std::string& tag, const bool embeddedStyle = true, const bool warm = false) {
  const auto dir = std::filesystem::temp_directory_path() / "css_typography_test" / tag;
  if (!warm) std::filesystem::remove_all(dir);
  pipeline_harness::Profile profile;
  profile.embeddedStyle = embeddedStyle;
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
