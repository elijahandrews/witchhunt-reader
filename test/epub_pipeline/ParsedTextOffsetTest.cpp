// Each word a ParsedText holds knows where it begins in the chapter's visible text, and every
// line it hands out reports its first word's offset. That is what lets a page know where it
// starts (Section's content-offset LUT) without the layout engine keeping any text around.
#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "Epub/ParsedText.h"
#include "GfxRenderer.h"

namespace {

// Ten words, offsets 0, 10, 20, ... so a line's first-word offset identifies the word.
ParsedText tenWords() {
  ParsedText text(/*extraParagraphSpacing=*/false);
  for (int i = 0; i < 10; ++i) {
    text.addWord("word" + std::to_string(i), EpdFontFamily::REGULAR, false, false, ParsedText::DEFAULT_WORD_SIZE_PCT, 0,
                 {}, static_cast<uint32_t>(i * 10));
  }
  return text;
}

std::vector<uint32_t> lineOffsets(ParsedText& text, const int width, const bool includeLastLine = true) {
  GfxRenderer renderer;
  std::vector<uint32_t> offsets;
  text.layoutAndExtractLines(
      renderer, 0, static_cast<uint16_t>(width),
      [&](std::unique_ptr<TextBlock>, bool, bool) {
        offsets.push_back(text.lastLineVisibleOffset());
        return ParsedText::LineProcessResult::Accepted;
      },
      includeLastLine);
  return offsets;
}

}  // namespace

TEST(ParsedTextOffset, TheFirstWordOfTheBlockIsWhereTheBlockStarts) {
  ParsedText text = tenWords();
  EXPECT_EQ(text.firstWordVisibleOffset(), std::optional<uint32_t>(0));
}

TEST(ParsedTextOffset, AnEmptyBlockHasNoOffset) {
  ParsedText text(false);
  EXPECT_EQ(text.firstWordVisibleOffset(), std::nullopt);
}

TEST(ParsedTextOffset, EveryLineReportsItsFirstWord) {
  ParsedText text = tenWords();
  // Narrow enough that the stub renderer breaks the ten words over several lines.
  const auto offsets = lineOffsets(text, 120);
  ASSERT_GT(offsets.size(), 1u);
  EXPECT_EQ(offsets.front(), 0u);
  for (size_t i = 1; i < offsets.size(); ++i) {
    EXPECT_GT(offsets[i], offsets[i - 1]);
    EXPECT_EQ(offsets[i] % 10, 0u) << "a line began at an offset no word has";
  }
}

TEST(ParsedTextOffset, ConsumedWordsTakeTheirOffsetsWithThem) {
  ParsedText whole = tenWords();
  const auto allLines = lineOffsets(whole, 120);
  ASSERT_GT(allLines.size(), 2u);

  // Withholding the last line (a continuation flush) leaves its words in the block; the block
  // must then start exactly where the whole-paragraph layout started that last line.
  ParsedText text = tenWords();
  const auto emitted = lineOffsets(text, 120, /*includeLastLine=*/false);
  ASSERT_EQ(emitted.size() + 1, allLines.size());
  EXPECT_EQ(text.firstWordVisibleOffset(), std::optional<uint32_t>(allLines.back()));
}

TEST(ParsedTextOffset, ResetForgetsTheOffsets) {
  ParsedText text = tenWords();
  text.reset(BlockStyle());
  EXPECT_EQ(text.firstWordVisibleOffset(), std::nullopt);
}

// Source offsets count decoded authored bytes, while raw anchors select rendered codepoints.
// A CSS transform or NFC composition can change the number of bytes before a split suffix.
#include <filesystem>

#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#include "SyntheticBook.h"

TEST(ParsedTextOffset, TransformedSplitWordsKeepAuthoredVisibleByteOffsets) {
  const auto dir = std::filesystem::temp_directory_path() / "epub_transformed_visible_offsets";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string source =
      "ab\xc4\xb1"
      "cdefghijklmnopqrstuvwxyz";  // dotless i: two bytes, uppercase I: one
  const auto book = syntheticBook(dir, 0, "<p style='hyphens:none;text-transform:uppercase'>" + source + "</p>");
  GfxRenderer renderer;
  Section::BuildParams params;
  params.viewportWidth = 70;
  params.viewportHeight = 24;
  params.lineCompression = 1.0f;
  params.embeddedStyle = true;
  params.hyphenationEnabled = false;
  params.fontSizeNormalization = false;
  Section section(book, 0, renderer);
  ASSERT_TRUE(section.createSectionFile(params, {}, true));
  ASSERT_TRUE(section.loadSectionFile(params));
  ASSERT_GT(section.pageCount, 2);
  for (uint16_t page = 0; page < section.pageCount; ++page) {
    const auto raw = section.getSourceAnchorForPage(page);
    const auto visible = section.getVisibleTextOffsetForPage(page);
    ASSERT_TRUE(raw && visible);
    const uint32_t expected = raw->characterOffset + (raw->characterOffset > 2 ? 1 : 0);
    EXPECT_EQ(*visible, expected) << "page " << page;
  }
  std::filesystem::remove_all(dir);
}

TEST(ParsedTextOffset, ExceptionalMapsSurviveBionicRetryAndMeasurement) {
  GfxRenderer renderer;
  const std::string word = "AAI" + std::string(23, 'A');
  std::vector<uint32_t> origins;
  for (size_t i = 0; i <= word.size(); ++i) origins.push_back(100 + i + (i > 2 ? 1 : 0));
  for (bool bionic : {false, true}) {
    ParsedText text(false, true, {}, bionic);
    text.addWord(word, EpdFontFamily::REGULAR, false, false, 100, 0, {1000, 1027}, 100, &origins);
    for (bool measure : {true, false}) {
      bool retried = false;
      unsigned accepted = 0;
      text.layoutAndExtractLines(
          renderer, 0, 65,
          [&](std::unique_ptr<TextBlock> line, bool hyphenated, bool suppressRetry) {
            EXPECT_GT(line->wordCount(), 0);
            const size_t start = line->sourceAnchor().characterOffset;
            EXPECT_LT(start, word.size());
            EXPECT_EQ(text.lastLineVisibleOffset(), origins[start]);
            if (hyphenated && !suppressRetry && !retried && accepted > 0) {
              retried = true;
              return ParsedText::LineProcessResult::RetryWithoutHyphenation;
            }
            ++accepted;
            return ParsedText::LineProcessResult::Accepted;
          },
          true, 0, 0, measure);
      EXPECT_GT(accepted, 1u);
      EXPECT_TRUE(retried);
      if (measure) EXPECT_EQ(text.firstWordVisibleOffset(), 100u);
    }
    EXPECT_TRUE(text.isEmpty());
    EXPECT_EQ(text.visibleMappingBytes(), 0u);
  }
}

TEST(ParsedTextOffset, MapBudgetAndAllocationFailureRefuseInsteadOfGuessing) {
  ParsedText text(false);
  const std::string word(200, 'x');
  std::vector<uint32_t> origins(word.size() + 1, 5);
  unsigned accepted = 0;
  for (; accepted < 100 && !text.wordGrowthRefused(); ++accepted)
    text.addWord(word, EpdFontFamily::REGULAR, false, false, 100, 0, {}, 5, &origins);
  EXPECT_TRUE(text.wordGrowthRefused());
  EXPECT_LT(accepted, 100u);
  EXPECT_LE(text.visibleMappingBytes(), 16384u);
  text.reset({});
  EXPECT_EQ(text.visibleMappingBytes(), 0u);
  EXPECT_TRUE(text.wordGrowthRefused());  // allocation refusal remains latched for this parse
  ParsedText retry(false);
  // Warm ordinary word capacity, then make only the exceptional-map allocation impossible.
  retry.addWord("plain", EpdFontFamily::REGULAR);
  const uint32_t saved = ESP.getMaxAllocHeap();
  ESP.setMaxAllocHeap(1024);
  retry.addWord(word, EpdFontFamily::REGULAR, false, false, 100, 0, {}, 5, &origins);
  ESP.setMaxAllocHeap(saved);
  EXPECT_TRUE(retry.wordGrowthRefused());
}

TEST(ParsedTextOffset, NormalizationControlsAndDropCapReplayRetainAuthoredOffsets) {
  const auto dir = std::filesystem::temp_directory_path() / "epub_cooked_visible_offsets";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  struct Case {
    std::string input, style;
    std::vector<uint32_t> expected;
    bool cap;
  };
  const std::string tail = "cdefghijklmnopqrstuvwxyz";
  const auto positions = [](size_t count, unsigned before, unsigned delta) {
    std::vector<uint32_t> result;
    for (size_t i = 0; i < count; ++i) result.push_back(i + (i >= before ? delta : 0));
    return result;
  };
  const std::vector<Case> cases = {
      {"ab\xc4\xb1" + tail, "text-transform:uppercase", positions(27, 3, 1), false},
      {"abe\xcc\x81" + tail, "", positions(27, 3, 2), false},
      {"ab\xc2\xad" + tail, "", positions(26, 2, 2), false},
      {"ab\xe2\x80\x8d" + tail, "", positions(26, 2, 3), false},
      {"ab\xc4\xb1" + tail, "float:left;font-size:3em;text-transform:uppercase", positions(27, 3, 1), true},
  };
  int index = 0;
  for (const auto& c : cases)
    for (bool bionic : {false, true}) {
      SCOPED_TRACE(index);
      GfxRenderer renderer;
      const auto book =
          syntheticBook(dir, index++,
                        c.cap ? "<p style='hyphens:none'><span style='" + c.style + "'>" + c.input + "</span></p>"
                              : "<p style='hyphens:none;" + c.style + "'>" + c.input + "</p>");
      Section::BuildParams params;
      params.viewportWidth = 70;
      params.viewportHeight = 24;
      params.embeddedStyle = true;
      params.fontSizeNormalization = false;
      params.bionicReadingEnabled = bionic;
      Section section(book, 0, renderer);
      ASSERT_TRUE(section.createSectionFile(params, {}, true));
      ASSERT_TRUE(section.loadSectionFile(params));
      ASSERT_FALSE(section.isTruncatedCache());
      ASSERT_GT(section.pageCount, 2);
      for (uint16_t page = 0; page < section.pageCount; ++page) {
        const auto raw = section.getSourceAnchorForPage(page);
        const auto visible = section.getVisibleTextOffsetForPage(page);
        ASSERT_TRUE(raw && visible);
        ASSERT_LT(raw->characterOffset, c.expected.size());
        uint32_t expected = c.expected[raw->characterOffset];
        // Bionic segmentation can leave a leading SHY-only token. Sync counts that
        // authored token; the independent raw anchor skips it to the first glyph.
        if (c.input.find("\xc2\xad") != std::string::npos && bionic && raw->characterOffset == 2) {
          section.currentPage = page;
          const auto cached = section.loadPageFromSectionFile();
          ASSERT_TRUE(cached);
          ASSERT_FALSE(cached->elements.empty());
          const auto& first = static_cast<const PageLine&>(*cached->elements.front());
          if (first.getBlock()->wordTextLen(0) == 0) expected = 2;
        }
        EXPECT_EQ(*visible, expected) << "page " << page;
      }
    }
  std::filesystem::remove_all(dir);
}

TEST(ParsedTextOffset, GeneratedWordsStayAtPlacementAndExceptionalParagraphsStream) {
  const auto dir = std::filesystem::temp_directory_path() / "epub_visible_streaming";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  GfxRenderer renderer;
  const auto generated =
      syntheticBookFromDocument(dir, 0,
                                "<html><head><style>p{hyphens:none}p::before{content:'ABCDEFGHIJKLMNOPQRSTUVWXYZ'}</"
                                "style></head><body><p>Tail</p></body></html>");
  Section::BuildParams params;
  params.viewportWidth = 70;
  params.viewportHeight = 24;
  params.embeddedStyle = true;
  params.fontSizeNormalization = false;
  Section first(generated, 0, renderer);
  ASSERT_TRUE(first.createSectionFile(params, {}, true));
  ASSERT_TRUE(first.loadSectionFile(params));
  ASSERT_GT(first.pageCount, 2);
  for (uint16_t page = 0; page < first.pageCount; ++page) EXPECT_EQ(first.getVisibleTextOffsetForPage(page), 0u);
  std::string paragraph = "<p style='hyphens:none;text-transform:uppercase'>";
  for (int i = 0; i < 50; ++i) paragraph += "\xc4\xb1" + std::string(99, 'a') + ' ';
  paragraph += "END</p>";
  const auto streamed = syntheticBook(dir, 1, paragraph);
  params.viewportWidth = 480;
  params.viewportHeight = 700;
  Section second(streamed, 0, renderer);
  ASSERT_TRUE(second.createSectionFile(params, {}, true));
  ASSERT_TRUE(second.loadSectionFile(params));
  ASSERT_FALSE(second.isTruncatedCache());
  EXPECT_GT(second.getVisibleTextOffsetForPage(second.pageCount - 1), 4000u);
  std::filesystem::remove_all(dir);
}

TEST(ParsedTextOffset, TransformedTableCellsChargeMapsBeforeRowFallback) {
  const auto dir = std::filesystem::temp_directory_path() / "epub_visible_table_budget";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  GfxRenderer renderer;
  std::string body = "<table><tr>";
  for (int i = 0; i < 6; ++i)
    body += "<td style='text-transform:uppercase;hyphens:none'>\xc4\xb1" + std::string(99, 'a') + " \xc4\xb1" +
            std::string(99, 'a') + "</td>";
  body += "</tr></table><p>END</p>";
  const auto book = syntheticBook(dir, 0, body);
  Section::BuildParams params;
  params.viewportWidth = 2000;
  params.viewportHeight = 700;
  params.embeddedStyle = true;
  params.fontSizeNormalization = false;
  Section section(book, 0, renderer);
  ASSERT_TRUE(section.createSectionFile(params, {}, true));
  ASSERT_TRUE(section.loadSectionFile(params));
  EXPECT_FALSE(section.isTruncatedCache());
  // The six-column row fits geometrically, but mapped source storage exceeds the byte budget.
  std::string visible;
  for (section.currentPage = 0; section.currentPage < section.pageCount; ++section.currentPage) {
    const auto page = section.loadPageFromSectionFile();
    ASSERT_TRUE(page);
    for (const auto& e : page->elements) {
      EXPECT_NE(e->getTag(), TAG_PageTable);
      if (e->getTag() == TAG_PageLine) {
        const auto& block = *static_cast<const PageLine&>(*e).getBlock();
        for (uint16_t w = 0; w < block.wordCount(); ++w) visible += block.wordText(w);
      }
    }
  }
  EXPECT_EQ(std::count(visible.begin(), visible.end(), 'I'), 12);
  EXPECT_EQ(std::count(visible.begin(), visible.end(), 'A'), 12 * 99);
  EXPECT_NE(visible.find("END"), std::string::npos);
  std::filesystem::remove_all(dir);
}

TEST(ParsedTextOffset, DropCapReplayChecksHeapBeforeAllocatingBothOriginMaps) {
  const auto dir = std::filesystem::temp_directory_path() / "epub_visible_dropcap_heap";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  GfxRenderer renderer;
  const auto book = syntheticBook(dir, 0, "<p>Unused</p>");
  auto parser = std::make_unique<ChapterHtmlSlimParser>(
      book, renderer, 0, 1.0f, false, 0, 100, 120, false, false, false, [](std::unique_ptr<Page>) { return true; },
      true, "", book->getCachePath(), 0, std::vector<std::string>{}, nullptr, book->getCssParser());
  ASSERT_TRUE(parser->setup(0));
  const std::string open = "<html><body><p><span style='float:left;font-size:3em;text-transform:uppercase'>";
  parser->write(reinterpret_cast<const uint8_t*>(open.data()), open.size());
  const uint32_t saved = ESP.getMaxAllocHeap();
  ESP.setMaxAllocHeap(7000);
  const std::string text = "\xc4\xb1" + std::string(253, 'a') + "</span></p></body></html>";
  parser->write(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  parser->finalize();
  ESP.setMaxAllocHeap(saved);
  EXPECT_FALSE(parser->streamSucceeded());
  std::filesystem::remove_all(dir);
}
