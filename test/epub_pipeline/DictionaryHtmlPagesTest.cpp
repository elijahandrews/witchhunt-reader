#include <Arduino.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include "src/util/DictHtmlPages.h"

TEST(DictionaryHtmlPages, KeepsSemanticEmphasisWithoutBionicPrefixes) {
  GfxRenderer renderer;
  std::vector<std::unique_ptr<Page>> pages;
  ASSERT_TRUE(buildDictionaryHtmlPages(
      renderer, "<p><b>Noun</b></p><p>An ordinary definition with <i>usage</i> information.</p>", 1, 440, 700, pages));
  std::vector<std::pair<std::string, EpdFontFamily::Style>> words;
  for (const auto& page : pages) {
    for (const auto& element : page->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto& block = *static_cast<const PageLine&>(*element).getBlock();
      EXPECT_EQ(block.getRenderStyle().alignment, CssTextAlign::Left);
      for (uint16_t i = 0; i < block.wordCount(); ++i) words.emplace_back(block.wordText(i), block.wordStyle(i));
    }
  }
  const std::vector<std::pair<std::string, EpdFontFamily::Style>> expected = {
      {"Noun", EpdFontFamily::BOLD},           {"An", EpdFontFamily::REGULAR},   {"ordinary", EpdFontFamily::REGULAR},
      {"definition", EpdFontFamily::REGULAR},  {"with", EpdFontFamily::REGULAR}, {"usage", EpdFontFamily::ITALIC},
      {"information.", EpdFontFamily::REGULAR}};
  EXPECT_EQ(words, expected);
}

TEST(DictionaryHtmlPages, LowHeapLeavesNoPartialDefinition) {
  GfxRenderer renderer;
  std::vector<std::unique_ptr<Page>> pages;
  ASSERT_TRUE(buildDictionaryHtmlPages(renderer, "<p>Reference text</p>", 1, 440, 700, pages));
  const auto saved = ESP.getFreeHeap();
  ESP.setFreeHeap(30 * 1024);
  const bool success = buildDictionaryHtmlPages(renderer, "<p>Reference text</p>", 1, 440, 700, pages);
  ESP.setFreeHeap(saved);
  EXPECT_FALSE(success);
  EXPECT_TRUE(pages.empty());
}
