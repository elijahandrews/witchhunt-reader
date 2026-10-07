#include <EpdFontFamily.h>
#include <SmallCaps.h>
#include <gtest/gtest.h>

#include <vector>

#include "lib/GfxRenderer/TextRun.h"
#include "lib/OutlineFont/EpdOutlineFontCallbacks.h"

namespace {
uint16_t glyphId(void*, uint32_t cp, uint8_t mode) {
  if (mode == 1) return cp == 'a' ? 10 : 0;
  if (mode == 2) return cp == 'A' ? 11 : 0;
  switch (cp) {
    case 'A':
      return 1;
    case 'a':
      return 2;
    case 'f':
      return 3;
    case 'i':
      return 4;
    case ' ':
      return 7;
    case 'N':
      return 8;
    case '1':
      return 9;
    default:
      return 0;
  }
}
EpdGlyphRef metrics(void*, uint16_t gid) {
  if (!gid || gid > 11) return {};
  const uint16_t advance = gid == 10 ? 160 : gid == 11 ? 144 : gid == 7 ? 80 : 320;
  return {nullptr, advance, gid, static_cast<uint16_t>(advance / 16), 16, 0, 16, true};
}
const uint8_t* bitmap(void*, uint16_t, size_t* bytes) {
  *bytes = 0;
  return nullptr;
}
int16_t kern(void*, uint16_t a, uint16_t b) { return a == 1 && b == 10 ? -16 : a == 8 && b == 8 ? -12 : 0; }
uint16_t liga(void*, const uint16_t* gids, size_t count) {
  if (count == 3 && gids[0] == 3 && gids[1] == 3 && gids[2] == 4) return 5;
  if (count == 2 && gids[0] == 3 && gids[1] == 3) return 6;
  return 0;
}
const EpdOutlineFontCallbacks callbacks{glyphId, metrics, bitmap, kern, liga};
struct Font {
  EpdFontData data{};
  EpdFont font{&data};
  EpdFontFamily family{&font};
  Font() {
    data.outline = &callbacks;
    data.advanceY = 24;
    data.ascender = 16;
    data.is2Bit = true;
  }
};
constexpr auto ALL = static_cast<EpdFontFamily::Style>(EpdFontFamily::SMALL_CAPS | EpdFontFamily::ALL_SMALL_CAPS);
}  // namespace

TEST(FontFeatures, AuthoredCapsUseFullScaleAndAlternateKerning) {
  Font f;
  std::vector<uint32_t> keys;
  const auto m = textRun::walk(f.family, "Aa", EpdFontFamily::SMALL_CAPS, 1, 0,
                               [&](uint32_t cp, int, int, float scale, bool synthetic) {
                                 keys.push_back(cp);
                                 EXPECT_FLOAT_EQ(scale, 1);
                                 EXPECT_FALSE(synthetic);
                               });
  EXPECT_EQ(keys, (std::vector<uint32_t>{'A', EPD_ALTERNATE_GLYPH_BASE + 10}));
  EXPECT_EQ(m.advance, 29);  // 20 + authored10 - authored1px kern, not scaled capital15
}
TEST(FontFeatures, AllSmallCapsIsDistinctAndLeavesNumbersAlone) {
  Font f;
  float scale;
  EXPECT_EQ(f.font.resolveCaps('A', 1, scale), 'A');
  EXPECT_EQ(f.font.resolveCaps('A', 2, scale), EPD_ALTERNATE_GLYPH_BASE + 11);
  EXPECT_FLOAT_EQ(scale, 1);
  EXPECT_EQ(f.font.resolveCaps('a', 2, scale), EPD_ALTERNATE_GLYPH_BASE + 10);
  EXPECT_EQ(f.font.resolveCaps('1', 2, scale), '1');
  EXPECT_FLOAT_EQ(scale, 1);
  EXPECT_EQ(textRun::walk(f.family, "Aa", ALL, 1, 0, [](auto...) {}).advance, 19);
}
TEST(FontFeatures, MissingSmallCapFeatureUsesSyntheticUppercaseOnlyForLetters) {
  Font f;
  float scale;
  EXPECT_EQ(f.font.resolveCaps('n', 1, scale), 'N');
  EXPECT_FLOAT_EQ(scale, .75f);
  EXPECT_EQ(f.font.resolveCaps('N', 2, scale), 'N');
  EXPECT_FLOAT_EQ(scale, .75f);
  EXPECT_EQ(f.font.resolveCaps('1', 2, scale), '1');
  EXPECT_FLOAT_EQ(scale, 1);
}
TEST(FontFeatures, UnicodeFallbackDoesNotMisclassifyLatinExtendedCapitals) {
  uint32_t cp = 0x0141;  // capital L with stroke, an odd codepoint
  EXPECT_FALSE(smallCaps::fold(cp));
  EXPECT_EQ(cp, 0x0141u);
  cp = 0x0142;
  EXPECT_TRUE(smallCaps::fold(cp));
  EXPECT_EQ(cp, 0x0141u);
  cp = 0x03b1;
  EXPECT_TRUE(smallCaps::fold(cp));
  EXPECT_EQ(cp, 0x0391u);
  EXPECT_TRUE(smallCaps::isUppercase(0x0391));
  EXPECT_FALSE(smallCaps::isUppercase(0x03b1));
}
TEST(FontFeatures, ThreeLetterLigaturePrecedesTwoLetterPrefixAndConsumesExactly) {
  Font f;
  const char* text = "fi tail";
  EXPECT_EQ(f.font.applyLigatures('f', text), EPD_ALTERNATE_GLYPH_BASE + 5);
  EXPECT_STREQ(text, " tail");
  text = "fX";
  EXPECT_EQ(f.font.applyLigatures('f', text), EPD_ALTERNATE_GLYPH_BASE + 6);
  EXPECT_STREQ(text, "X");
  text = "a";
  EXPECT_EQ(f.font.applyLigatures('f', text), 'f');
  EXPECT_STREQ(text, "a");
}
TEST(FontFeatures, NormalLigaturesNeverHideLettersFromCaps) {
  Font f;
  const char* text = "fi";
  EXPECT_EQ(f.family.applyLigatures('f', text, EpdFontFamily::SMALL_CAPS), 'f');
  EXPECT_STREQ(text, "fi");
  EXPECT_EQ(f.family.applyLigatures('f', text, ALL), 'f');
  EXPECT_STREQ(text, "fi");
}
TEST(FontFeatures, OpaqueGlyphKeysDoNotBecomeUnicodeAndAreBoundsChecked) {
  Font f;
  const auto a = f.font.getGlyph(EPD_ALTERNATE_GLYPH_BASE + 10);
  ASSERT_TRUE(a);
  EXPECT_EQ(a.index, 10);
  EXPECT_FALSE(f.font.getGlyph(EPD_ALTERNATE_GLYPH_BASE + 65536));
  EXPECT_FALSE(f.font.getGlyph(EPD_ALTERNATE_GLYPH_BASE));
}

TEST(FontFeatures, OpaqueGlyphKeysCannotWrapInKerningOrLigatures) {
  Font f;
  const uint32_t invalidA = EPD_ALTERNATE_GLYPH_BASE + 65536 + 1;
  const uint32_t invalidF = EPD_ALTERNATE_GLYPH_BASE + 65536 + 3;
  EXPECT_EQ(f.font.getKerning(invalidA, EPD_ALTERNATE_GLYPH_BASE + 10), 0);
  EXPECT_EQ(f.font.getKerning('A', EPD_ALTERNATE_GLYPH_BASE + 65536 + 10), 0);
  EXPECT_EQ(f.font.getLigature(invalidF, 'f'), 0u);
  EXPECT_EQ(f.font.getLigature('f', invalidF), 0u);
  const char* text = "fi tail";
  EXPECT_EQ(f.font.applyLigatures(invalidF, text), invalidF);
  EXPECT_STREQ(text, "fi tail");
}

TEST(FontFeatures, SyntheticCapsPreserveNegativeFractionalKerning) {
  Font f;
  int width = 0, height = 0;
  f.font.getTextDimensions("nn", &width, &height, 1);
  // Each capital is20px wide, reduced to15px. Pair kerning is -12/16
  // sourcepx, reduced to -9/16; the second glyph starts at14px.
  EXPECT_EQ(width, 29);
  EXPECT_EQ(textRun::walk(f.family, "nn", EpdFontFamily::SMALL_CAPS, 1, 0, [](auto...) {}).right, 29);
}
