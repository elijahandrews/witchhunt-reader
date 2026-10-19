#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "Epub/ParsedText.h"
#include "GfxRenderer.h"

namespace {
std::vector<std::unique_ptr<TextBlock>> layoutIndent(int width, int indent, CssTextAlign alignment,
                                                     EpdFontFamily::Style style, bool hyphenation = false) {
  GfxRenderer renderer;
  BlockStyle block;
  block.alignment = alignment;
  block.marginLeft = 54;
  block.textIndent = indent;
  block.textIndentDefined = true;
  ParsedText paragraph(false, hyphenation, block);
  for (int i = 0; i < 40; ++i) paragraph.addWord("sample", style);
  std::vector<std::unique_ptr<TextBlock>> lines;
  paragraph.layoutAndExtractLines(renderer, 1, width, [&lines](std::unique_ptr<TextBlock> line, bool, bool) {
    lines.push_back(std::move(line));
    return ParsedText::LineProcessResult::Accepted;
  });
  return lines;
}
}  // namespace

TEST(ParagraphIndent, HangingIndentDoesNotWidenContinuationLines) {
  GfxRenderer renderer;
  for (int width : {120, 130, 200, 210, 320, 330, 440, 450}) {
    for (int indent : {-36, -16, 0, 16, 36}) {
      for (auto alignment : {CssTextAlign::Left, CssTextAlign::Justify}) {
        for (auto style : {EpdFontFamily::REGULAR, EpdFontFamily::ITALIC}) {
          for (bool hyphenation : {false, true}) {
            SCOPED_TRACE(::testing::Message()
                         << "width=" << width << " indent=" << indent << " align=" << int(alignment)
                         << " style=" << int(style) << " hyphenation=" << hyphenation);
            const auto lines = layoutIndent(width, indent, alignment, style, hyphenation);
            ASSERT_GT(lines.size(), 1u);
            for (size_t row = 0; row < lines.size(); ++row) {
              const auto& line = *lines[row];
              for (uint16_t i = 0; i < line.wordCount(); ++i) {
                const int advance = renderer.getTextAdvanceXSpaced(1, line.wordText(i), line.wordStyle(i), 1.0f, 0);
                EXPECT_GE(line.wordXpos(i), row == 0 ? std::min(0, indent) : 0);
                EXPECT_LE(line.wordXpos(i) + advance, width) << "row=" << row << " word=" << i;
              }
            }
          }
        }
      }
    }
  }
}

TEST(ParagraphIndent, PositiveIndentOnlyNarrowsFirstLine) {
  GfxRenderer renderer;
  const int word = renderer.getTextAdvanceXSpaced(1, "sample", EpdFontFamily::REGULAR, 1.0f, 0);
  const int gap = renderer.getSpaceWidth(1);
  const int width = 4 * word + 3 * gap;
  const auto lines = layoutIndent(width, word + gap, CssTextAlign::Left, EpdFontFamily::REGULAR);
  ASSERT_GT(lines.size(), 2u);
  EXPECT_LE(lines.front()->wordCount(), 3u);
  EXPECT_TRUE(std::any_of(lines.begin() + 1, lines.end(), [](const auto& line) { return line->wordCount() == 4; }));
}
