// Original geometric fonts; authored caps have no Unicode cmap entries.
#include <gtest/gtest.h>
#if defined(WITCH_TEST_NATIVE_OUTLINE)
#include <OutlineFontFace.h>
#endif

#include "EpdFontFamily.h"
#include "SimNorFlash.h"
#include "SmallCaps.h"
#include "TtfBuilder.h"
#include "TtfFont.h"
#include "TtfFontTestUtil.h"
#include "TtfLayoutBuilder.h"

using namespace ttf_test;
namespace {
constexpr uint32_t cap(unsigned gid) { return EPD_ALTERNATE_GLYPH_BASE + gid; }
constexpr uint8_t identity[glyphstore::kIdentityBytes] = {'c', 'a', 'p', 's'};

Font capsFont(bool extension = false) {
  Font f;
  f.glyphs.resize(9);
  f.cmap = {{'A', 1}, {'V', 2}, {'a', 3}, {'v', 4}, {'q', 7}, {'Q', 8}};
  for (unsigned i = 0; i < f.glyphs.size(); ++i) {
    f.glyphs[i].advance = 500 + i * 31;
    f.glyphs[i].contours = {rect(-15, 0, 240 + i * 25, i == 5 || i == 6 ? 520 : 700)};
  }
  LayoutLookup smcp, c2sc, kern;
  smcp.type = c2sc.type = 1;
  smcp.extension = c2sc.extension = extension;
  smcp.substitutions = {{3, 5}, {4, 6}};
  c2sc.substitutions = {{1, 5}, {2, 6}};
  PairSubtable pairs;
  pairs.pairs = {{1, {{2, -12}, {6, -40}}}, {5, {{2, -50}, {6, -80}}}};
  kern.pairSubtables = {pairs};
  f.extraTables = {{"GSUB", layoutTable({{"latn", {0, 1}}}, {{"smcp", {0}}, {"c2sc", {1}}}, {smcp, c2sc}, true)},
                   {"GPOS", layoutTable({{"latn", {0}}}, {{"kern", {0}}}, {kern})}};
  return f;
}

class NativeCaps : public ::testing::Test {
 protected:
  SimNorFlash flash{4 * 1024 * 1024};
  MemoryFile source;
  std::unique_ptr<TtfFont> open(unsigned points = 10) {
    ttf::FileSource sources[4] = {source.source(), {}, {}, {}};
    auto font = std::make_unique<TtfFont>(flash);
    if (!font->load(sources, identity) || !font->setBodySize(points)) return {};
    return font;
  }
  void SetUp() override { source.bytes = buildFont(capsFont()); }
  void TearDown() override {
    EXPECT_EQ(0u, flash.violations());
    EXPECT_EQ(0u, flash.reprograms());
  }
};

TEST_F(NativeCaps, AuthoredAndPartialFallbackUseEachCharactersFeature) {
  const auto native = open();
  ASSERT_TRUE(native);
  const EpdFont& font = *native->font(0);
  float scale = 0;
  EXPECT_EQ(cap(5), font.resolveCaps('a', 1, scale));
  EXPECT_FLOAT_EQ(1, scale);
  EXPECT_EQ(cap(6), font.resolveCaps('V', 2, scale));
  EXPECT_FLOAT_EQ(1, scale);
  EXPECT_EQ(cap(5), font.resolveCaps('a', 2, scale));
  EXPECT_FLOAT_EQ(1, scale);
  EXPECT_EQ(uint32_t('A'), font.resolveCaps('A', 1, scale));
  EXPECT_FLOAT_EQ(1, scale);
  EXPECT_EQ(uint32_t('a'), font.resolveCaps('a', 0, scale));
  EXPECT_FLOAT_EQ(1, scale);
  EXPECT_EQ(uint32_t('Q'), font.resolveCaps('q', 1, scale));
  EXPECT_FLOAT_EQ(smallCaps::SCALE, scale);
  EXPECT_EQ(uint32_t('Q'), font.resolveCaps('Q', 2, scale));
  EXPECT_FLOAT_EQ(smallCaps::SCALE, scale);
  EXPECT_EQ(uint32_t('.'), font.resolveCaps('.', 2, scale));
  EXPECT_FLOAT_EQ(1, scale);
  EXPECT_EQ(cap(5), font.resolveCaps(cap(5), 2, scale));
  EXPECT_FLOAT_EQ(1, scale);
  EpdFontFamily family(&font);
  EXPECT_EQ(cap(5),
            family.resolveCaps('a', EpdFontFamily::Style(EpdFontFamily::SMALL_CAPS | EpdFontFamily::BOLD), scale));
  const char* remainder = "i";
  EXPECT_EQ(uint32_t('f'), family.applyLigatures('f', remainder, EpdFontFamily::SMALL_CAPS));
  EXPECT_STREQ("i", remainder);
}

TEST_F(NativeCaps, AlternateMetricsAndBitmapMatchTheActualGlyphAtBodyAndNativeSizes) {
  FontBytes bytes(source.bytes.data(), source.bytes.size());
  ttf::Face face;
  ASSERT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(bytes, &face));
  ttf::Rasterizer reference;
  ASSERT_TRUE(reference.allocate(ttf::planScratch(face, 120 * 64, ttf::kPoolBytesFlash)));
  uint32_t writes = 0;
  for (unsigned session = 0; session < 2; ++session) {
    auto native = open();
    ASSERT_TRUE(native);
    for (unsigned pixels : {0u, 17u, 25u, 31u}) {
      const EpdFont* fonts[4] = {native->font(0), nullptr, nullptr, nullptr};
      uint32_t ppem = native->bodyPpem();
      if (pixels) ASSERT_TRUE(native->nativeFonts(pixels, fonts, &ppem));
      for (unsigned gid : {5u, 6u}) {
        EpdGlyph expected{};
        ASSERT_EQ(ttf::Status::Ok, reference.render(bytes, face, gid, ppem, &expected));
        auto actual = fonts[0]->getGlyph(cap(gid));
        ASSERT_TRUE(actual);
        EXPECT_EQ(expected.advanceX, actual.advanceX);
        EXPECT_EQ(expected.width, actual.width);
        EXPECT_EQ(expected.height, actual.height);
        EXPECT_EQ(expected.left, actual.left);
        EXPECT_EQ(expected.top, actual.top);
        const uint8_t* bitmap = nullptr;
        if (!native->ownsBitmap(actual.sdRecord, bitmap)) bitmap = fonts[0]->data->bitmap + actual.sdRecord->dataOffset;
        ASSERT_NE(nullptr, bitmap);
        EXPECT_EQ(0, std::memcmp(bitmap, reference.output(), expected.dataLength));
        const auto before = native->renders();
        ASSERT_TRUE(fonts[0]->getGlyph(cap(gid)));
        EXPECT_EQ(before, native->renders());
      }
      float scale;
      EXPECT_EQ(cap(5), fonts[0]->resolveCaps('a', 1, scale));
      EXPECT_FLOAT_EQ(1, scale);
    }
    if (!session)
      writes = flash.programCalls();
    else
      EXPECT_EQ(writes, flash.programCalls());
  }
}

TEST_F(NativeCaps, AlternateAndMixedKerningUseGlyphIdsWithoutPoisoningUnicodePairs) {
  const auto native = open();
  ASSERT_TRUE(native);
  const auto& font = *native->font(0);
  auto expected = [&](int units) {
    const int64_t num = int64_t(units) * native->bodyPpem(), den = 4000;
    return int((num >= 0 ? num + den / 2 : num - den / 2) / den);
  };
  for (int repeat = 0; repeat < 3; ++repeat) {
    EXPECT_EQ(expected(-12), font.getKerning('A', 'V'));
    EXPECT_EQ(expected(-80), font.getKerning(cap(5), cap(6)));
    EXPECT_EQ(expected(-40), font.getKerning('A', cap(6)));
    EXPECT_EQ(expected(-50), font.getKerning(cap(5), 'V'));
    EXPECT_EQ(0, font.getKerning(cap(6), cap(5)));
  }
  EXPECT_EQ(0, font.getKerning(EPD_ALTERNATE_GLYPH_BASE, cap(6)));
  EXPECT_EQ(0, font.getKerning(UINT32_MAX, cap(6)));
}

TEST_F(NativeCaps, InvalidAlternateKeysCannotAliasStoredGlyphs) {
  const auto native = open();
  ASSERT_TRUE(native);
  const auto real = native->font(0)->getGlyph('A');
  ASSERT_TRUE(real);
  const uint32_t writes = flash.programCalls();
  for (uint32_t cp : {EPD_ALTERNATE_GLYPH_BASE, cap(9), cap(65535), UINT32_MAX, uint32_t(0x200000 + 'A')}) {
    EXPECT_FALSE(native->font(0)->getGlyph(cp));
  }
  EXPECT_EQ(writes, flash.programCalls());
  EXPECT_EQ(real.advanceX, native->font(0)->getGlyph('A').advanceX);
}

TEST_F(NativeCaps, AbsentFeaturesKeepSyntheticCaps) {
  Font f = capsFont();
  f.extraTables.clear();
  source.bytes = buildFont(f);
  const auto native = open();
  ASSERT_TRUE(native);
  float scale;
  EXPECT_EQ(uint32_t('A'), native->font(0)->resolveCaps('a', 1, scale));
  EXPECT_FLOAT_EQ(smallCaps::SCALE, scale);
  EXPECT_EQ(uint32_t('A'), native->font(0)->resolveCaps('A', 2, scale));
  EXPECT_FLOAT_EQ(smallCaps::SCALE, scale);
}

TEST(TtfCapsTables, ExtensionAndMalformedTablesRemainBoundedAndReturnNoInvalidGlyphs) {
  for (bool extension : {false, true}) {
    auto file = buildFont(capsFont(extension));
    FontBytes bytes(file.data(), file.size());
    ttf::Face face;
    ASSERT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(bytes, &face));
    EXPECT_EQ(5u, ttf::capsGlyph(bytes, face, 'a', 1));
    EXPECT_EQ(6u, ttf::capsGlyph(bytes, face, 'V', 2));
    EXPECT_EQ(0u, ttf::capsGlyph(bytes, face, 'A', 1));
    EXPECT_EQ(0u, ttf::capsGlyph(bytes, face, 'a', 3));
    EXPECT_EQ(0u, ttf::capsGlyph(bytes, face, cap(5), 1));
    for (unsigned length = 0; length < face.gsubLength; ++length) {
      auto truncated = face;
      truncated.gsubLength = length;
      const auto gid = ttf::capsGlyph(bytes, truncated, 'a', 1);
      EXPECT_TRUE(gid == 0 || gid == 5);
    }
    face.gsubLength = UINT32_MAX;
    EXPECT_EQ(0u, ttf::capsGlyph(bytes, face, 'a', 1));
    face.gsub = UINT32_MAX;
    EXPECT_EQ(0u, ttf::capsGlyph(bytes, face, 'a', 1));
  }
  auto bad = capsFont();
  LayoutLookup lookup;
  lookup.type = 1;
  lookup.substitutions = {{3, 60000}};
  bad.extraTables = {{"GSUB", layoutTable({{"latn", {0}}}, {{"smcp", {0}}}, {lookup}, true)}};
  auto file = buildFont(bad);
  FontBytes bytes(file.data(), file.size());
  ttf::Face face;
  ASSERT_EQ(ttf::FaceStatus::Ok, ttf::parseFace(bytes, &face));
  EXPECT_EQ(0u, ttf::capsGlyph(bytes, face, 'a', 1));
}

#if defined(WITCH_TEST_NATIVE_OUTLINE)
TEST_F(NativeCaps, FinalGridRasterUsesAlternateGlyphInEveryStyleAndSize) {
  MemoryFile files[4];
  ttf::FileSource inputs[4];
  std::unique_ptr<OutlineFontFace> references[4];
  for (unsigned style = 0; style < 4; ++style) {
    Font fixture = capsFont();
    fixture.glyphs[5].contours = {rect(-25 - style * 5, 0, 300 + style * 35, 500 + style * 30)};
    files[style].bytes = buildFont(fixture);
    inputs[style] = files[style].source();
    references[style] = std::make_unique<OutlineFontFace>();
    OutlineFontFace::Options options;
    options.applyVariations = false;
    options.loadLayoutTables = false;
    ASSERT_TRUE(references[style]->openMemory(files[style].bytes.data(), files[style].bytes.size(), options));
  }
  for (unsigned session = 0; session < 2; ++session) {
    TtfFont native(flash);
    ASSERT_TRUE(native.load(inputs, identity));
    ASSERT_TRUE(native.setBodySize(10));
    for (unsigned px : {0u, 17u, 25u, 31u}) {
      const EpdFont* fonts[4] = {native.font(0), native.font(1), native.font(2), native.font(3)};
      uint32_t ppem = native.bodyPpem();
      if (px) ASSERT_TRUE(native.nativeFonts(px, fonts, &ppem));
      for (unsigned style = 0; style < 4; ++style) {
        float capsScale;
        ASSERT_EQ(cap(5), fonts[style]->resolveCaps('a', 1, capsScale));
        ASSERT_FLOAT_EQ(1, capsScale);
        const auto layout = fonts[style]->getGlyph(cap(5));
        ASSERT_TRUE(layout);
        for (float scale : {.75f, 1.0f, 1.25f}) {
          for (int phase = -8; phase < 8; ++phase) {
            EpdRasterGlyph actual, expected;
            ASSERT_TRUE(fonts[style]->rasterGlyph(cap(5), scale, phase, actual));
            ASSERT_TRUE(references[style]->rasterAtPpem(5, ppem, scale, phase, &expected));
            EXPECT_EQ(expected.metrics.left, actual.metrics.left);
            EXPECT_EQ(expected.metrics.top, actual.metrics.top);
            EXPECT_EQ(expected.metrics.width, actual.metrics.width);
            EXPECT_EQ(expected.metrics.height, actual.metrics.height);
            ASSERT_EQ(expected.bytes, actual.bytes);
            EXPECT_EQ(0, std::memcmp(expected.bitmap, actual.bitmap, actual.bytes));
          }
        }
        EXPECT_EQ(layout.advanceX, fonts[style]->getGlyph(cap(5)).advanceX);
        EpdRasterGlyph invalid;
        EXPECT_FALSE(fonts[style]->rasterGlyph(cap(60000), 1, 0, invalid));
      }
    }
  }
}
#endif
}  // namespace
