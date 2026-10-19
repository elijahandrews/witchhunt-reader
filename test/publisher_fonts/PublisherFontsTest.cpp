#include <Epub.h>
#include <EpubFontManager.h>
#include <OutlineFontFace.h>
#include <WordTypography.h>
#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {
constexpr int BASE = 101;
const std::filesystem::path FIXTURES(PUBLISHER_FIXTURE_DIR);
using Style = EpdFontFamily::Style;

class PublisherFonts : public testing::Test {
 protected:
  Epub epub;
  GfxRenderer renderer;
  void SetUp() override {
    epub.cache =
        (std::filesystem::temp_directory_path() /
         (std::string("publisher-font-manager-") + testing::UnitTest::GetInstance()->current_test_info()->name()))
            .string();
    std::filesystem::remove_all(epub.cache);
    std::filesystem::create_directories(epub.cache);
    for (const auto& entry : std::filesystem::directory_iterator(FIXTURES))
      if (entry.path().extension() == ".ttf" || entry.path().extension() == ".otf")
        epub.items["Fonts/" + entry.path().filename().string()] = entry.path();
    renderer.registerFontPointSize(BASE, 14);
  }
  void TearDown() override { std::filesystem::remove_all(epub.cache); }
  void face(const std::string& family, const std::string& filename, uint16_t weight = 400,
            CssFontCatalog::Style style = CssFontCatalog::Style::Normal,
            std::vector<CssFontCatalog::Source> before = {}) {
    CssFontCatalog::Face item;
    item.family = family;
    item.weight = weight;
    item.style = style;
    item.sources = std::move(before);
    item.sources.push_back({"Fonts/" + filename, "truetype"});
    epub.css.catalog.addFace(std::move(item));
  }
  void fourFaces() {
    face("forms", "FixtureForms-Regular.ttf");
    face("forms", "FixtureForms-Bold.ttf", 700);
    face("forms", "FixtureForms-Italic.ttf", 400, CssFontCatalog::Style::Italic);
    face("forms", "FixtureForms-BoldItalic.ttf", 700, CssFontCatalog::Style::Italic);
  }
  uint8_t stack(const std::string& names = "forms, serif") { return epub.css.catalog.intern(names); }
  const EpdFontFamily& family(const GfxRenderer::TextFont& selected) { return renderer.fonts.at(selected.fontId); }
  OutlineFontFace& backend(const GfxRenderer::TextFont& selected, unsigned style = 0) {
    const auto* data = family(selected).getData(static_cast<Style>(style));
    return *static_cast<OutlineFontFace*>(data->outlineCtx);
  }
  std::vector<uint8_t> pixels(OutlineFontFace& font, uint32_t cp) {
    const auto id = font.glyphId(cp);
    font.metrics(id);
    size_t bytes = 0;
    const auto* bitmap = font.bitmap(id, &bytes);
    return bitmap ? std::vector<uint8_t>(bitmap, bitmap + bytes) : std::vector<uint8_t>{};
  }
  size_t ink(OutlineFontFace& font, uint32_t cp) {
    const auto id = font.glyphId(cp);
    const auto glyph = font.metrics(id);
    size_t bytes = 0;
    const auto* bitmap = font.bitmap(id, &bytes);
    size_t total = 0;
    EXPECT_GE(bytes, (static_cast<size_t>(glyph.width) * glyph.height + 3) / 4);
    for (size_t i = 0; i < static_cast<size_t>(glyph.width) * glyph.height; ++i)
      total += (bitmap[i / 4] >> ((3 - i % 4) * 2)) & 3;
    return total;
  }
};

TEST_F(PublisherFonts, FourRealFacesRegisterResolveFeaturesAndCleanUp) {
  fourFaces();
  const auto id = stack();
  {
    EpubFontManager manager(epub, renderer);
    ASSERT_NE(renderer.resolver, nullptr);
    ASSERT_NE(renderer.failureGetter, nullptr);
    EXPECT_EQ(renderer.failureGetter(renderer.resolverContext), 0u);
    const auto selected = renderer.resolver(renderer.resolverContext, BASE, id, 1);
    ASSERT_NE(selected.fontId, BASE);
    EXPECT_FLOAT_EQ(selected.scale, 1);
    EXPECT_FLOAT_EQ(renderer.scales.at(selected.fontId), .5f);
    std::set<const void*> faces;
    for (unsigned style = 0; style < 4; ++style) {
      auto& outline = backend(selected, style);
      EXPECT_TRUE(outline.ready());
      faces.insert(&outline);
      EXPECT_NE(outline.glyphId('h', 1), outline.glyphId('h'));
      EXPECT_NE(outline.glyphId('H', 2), outline.glyphId('H'));
      EXPECT_EQ(outline.glyphId('h', 1), outline.glyphId('H', 2));
      const uint16_t fi[] = {outline.glyphId('f'), outline.glyphId('i')};
      EXPECT_NE(outline.ligature(fi, 2), 0);
    }
    EXPECT_EQ(faces.size(), 4);
    EXPECT_GT(ink(backend(selected, 1), 'H'), ink(backend(selected, 0), 'H'));
    EXPECT_GT(family(selected).getGlyph('H', EpdFontFamily::ITALIC).width,
              family(selected).getGlyph('H', EpdFontFamily::REGULAR).width);
    EXPECT_GT(manager.memoryUsed(), 0);
    EXPECT_LE(manager.memoryUsed(), 3u * 1024 * 1024);
    EXPECT_EQ(epub.extracts, 4);
  }
  EXPECT_TRUE(renderer.fonts.empty());
  EXPECT_EQ(renderer.resolverContext, nullptr);
  EXPECT_EQ(renderer.resolver, nullptr);
  EXPECT_EQ(renderer.failureGetter, nullptr);
  EXPECT_GT(renderer.removals, 0);
}

TEST_F(PublisherFonts, MissingAndCorruptSourcesFallThroughToUsableFamily) {
  face("forms", "FixtureForms-Regular.ttf", 400, CssFontCatalog::Style::Normal,
       {{"Fonts/missing.ttf", "truetype"}, {"Fonts/corrupt.ttf", "truetype"}});
  const auto id = stack("absent, forms, sans-serif");
  EpubFontManager manager(epub, renderer);
  const auto selected = manager.resolve(BASE, id, 1);
  ASSERT_NE(selected.fontId, BASE);
  ASSERT_NE(selected.fontId, 2);
  EXPECT_TRUE(backend(selected).ready());
  EXPECT_EQ(epub.extracts, 2);  // Corrupt and usable; missing never extracted.
  EXPECT_EQ(manager.resolve(BASE, id, 1).fontId, selected.fontId);
  EXPECT_EQ(epub.extracts, 2);
}

TEST_F(PublisherFonts, TransientExtractionFailureRetriesAndChangesFingerprintOnRecovery) {
  face("forms", "FixtureForms-Regular.ttf");
  const auto id = stack();
  epub.failingExtractions.insert("Fonts/FixtureForms-Regular.ttf");
  EpubFontManager manager(epub, renderer);
  const auto unavailableFingerprint = manager.fingerprint();
  EXPECT_GT(manager.failureEpoch(), 0u);
  EXPECT_EQ(manager.resolve(BASE, id, 1).fontId, BASE);
  EXPECT_TRUE(renderer.fonts.empty());
  const auto failedEpoch = manager.failureEpoch();
  const auto failedExtractions = epub.extracts;

  epub.failingExtractions.clear();
  const auto recovered = manager.resolve(BASE, id, 1);
  ASSERT_TRUE(renderer.fonts.count(recovered.fontId));
  EXPECT_TRUE(backend(recovered).ready());
  EXPECT_GT(family(recovered).getGlyph('H').advanceX, 0);
  EXPECT_NE(manager.fingerprint(), unavailableFingerprint);
  EXPECT_EQ(manager.failureEpoch(), failedEpoch + 1);  // Recovery invalidates earlier fallback layout.
  EXPECT_EQ(epub.extracts, failedExtractions + 1);
  EXPECT_EQ(manager.resolve(BASE, id, 1).fontId, recovered.fontId);
  EXPECT_EQ(manager.failureEpoch(), failedEpoch + 1);
  EXPECT_EQ(epub.extracts, failedExtractions + 1);
}

TEST_F(PublisherFonts, AbsentSourceIsDeterministicFallbackWithoutFailureEpoch) {
  face("missing", "missing.ttf");
  const auto id = stack("missing, serif");
  EpubFontManager manager(epub, renderer);
  const auto fingerprint = manager.fingerprint();
  for (int i = 0; i < 3; ++i) EXPECT_EQ(manager.resolve(BASE, id, 1).fontId, BASE);
  EXPECT_EQ(manager.failureEpoch(), 0u);
  EXPECT_EQ(manager.fingerprint(), fingerprint);
  EXPECT_EQ(epub.extracts, 0u);
  EXPECT_TRUE(renderer.fonts.empty());
}

TEST_F(PublisherFonts, UnsupportedSourceFormatIsSkippedBeforeExtraction) {
  face("forms", "FixtureWide-Regular.ttf", 400, CssFontCatalog::Style::Normal,
       {{"Fonts/FixtureForms-Regular.ttf", "woff2"}});
  const auto id = stack();
  EpubFontManager manager(epub, renderer);
  const auto selected = manager.resolve(BASE, id, 1);
  ASSERT_TRUE(renderer.fonts.count(selected.fontId));
  // The wide source's original 930-unit H advance differs from the first
  // source's 670-unit advance. A supported byte signature cannot override an
  // explicitly unsupported format hint.
  EXPECT_NEAR(family(selected).getGlyph('H').advanceX, 930 * (14 * 150.0 / 72 * 2 / 1000) * 16, 1.1);
  EXPECT_EQ(epub.extracts, 1u);
  EXPECT_EQ(manager.failureEpoch(), 0u);
}

TEST_F(PublisherFonts, StaticFaceReusesFourStylesButVariableAxesGetDistinctStyles) {
  face("forms", "FixtureForms-Regular.ttf");
  face("optical", "FixtureOptical-Variable.ttf");
  const auto staticId = stack(), variableId = stack("optical, serif");
  EpubFontManager manager(epub, renderer);
  const auto selected = manager.resolve(BASE, staticId, 1);
  for (unsigned style = 1; style < 4; ++style) EXPECT_EQ(&backend(selected, style), &backend(selected));
  const auto variable = manager.resolve(BASE, variableId, 1);
  ASSERT_TRUE(renderer.fonts.count(variable.fontId));
  EXPECT_NE(&backend(variable, 0), &backend(variable, 1));
  EXPECT_NE(&backend(variable, 0), &backend(variable, 2));
  EXPECT_NE(&backend(variable, 1), &backend(variable, 3));
  EXPECT_TRUE(backend(variable).hasItalicAxis());
  EXPECT_GT(family(variable).getGlyph('H', EpdFontFamily::ITALIC).width,
            family(variable).getGlyph('H', EpdFontFamily::REGULAR).width);
  EXPECT_TRUE(backend(variable).hasWeightAxis());
  EXPECT_GT(ink(backend(variable, 1), 'H'), ink(backend(variable, 0), 'H'));
}

TEST_F(PublisherFonts, ActualCssSizePinsOpticalPointsAndPreservesDisplayedScale) {
  face("optical", "FixtureOptical-Variable.ttf");
  const auto id = stack("optical, serif");
  EpubFontManager manager(epub, renderer);
  for (float scale : {.8f, 1.0f, 1.6f, 2.0f}) {
    SCOPED_TRACE(scale);
    const auto selected = manager.resolve(BASE, id, scale);
    ASSERT_TRUE(renderer.fonts.count(selected.fontId));
    auto& outline = backend(selected);
    EXPECT_TRUE(outline.hasOpticalSize());
    const float requested = std::round(14 * scale * 64) / 64;
    EXPECT_NEAR(outline.opticalSize(), requested, .02f);
    EXPECT_NEAR(renderer.points.at(selected.fontId), requested, .001f);
    EXPECT_NEAR(selected.scale * scale * requested, 14 * scale, .002f);
    // A direct production face with explicit final design coordinates is an
    // independent oracle for the manager's scale/axis selection.
    const auto path = FIXTURES / "FixtureOptical-Variable.ttf";
    std::ifstream stream(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)), {});
    OutlineFontFace direct;
    OutlineFontFace::Options options;
    options.logicalPx = requested * 150 / 72;
    options.opticalPoints = requested;
    ASSERT_TRUE(direct.openMemory(bytes.data(), bytes.size(), options));
    const auto a = outline.metrics(outline.glyphId('H')), b = direct.metrics(direct.glyphId('H'));
    EXPECT_EQ(a.advanceX, b.advanceX);
    EXPECT_EQ(a.width, b.width);
    EXPECT_EQ(ink(outline, 'H'), ink(direct, 'H'));
  }
}

TEST_F(PublisherFonts, SameCountCatalogChangeInvalidatesFingerprintAndRegistration) {
  face("forms", "FixtureForms-Regular.ttf");
  auto id = stack();
  EpubFontManager manager(epub, renderer);
  const auto firstHash = manager.fingerprint();
  const auto first = manager.resolve(BASE, id, 1);
  ASSERT_TRUE(renderer.fonts.count(first.fontId));
  const auto firstAdvance = family(first).getGlyph('H').advanceX;
  epub.css.catalog.reset();
  face("forms", "FixtureWide-Regular.ttf");
  id = stack();
  EXPECT_NE(manager.fingerprint(), firstHash);
  const auto second = manager.resolve(BASE, id, 1);
  ASSERT_TRUE(renderer.fonts.count(second.fontId));
  EXPECT_GT(family(second).getGlyph('H').advanceX, firstAdvance);
  EXPECT_FALSE(renderer.fonts.count(first.fontId));
}

TEST_F(PublisherFonts, SourceSizeBoundsRejectBeforeExtractionAndPreserveGenericFallback) {
  epub.reportedSizes["Fonts/empty.ttf"] = 0;
  epub.reportedSizes["Fonts/large.ttf"] = 8 * 1024 * 1024 + 1;
  face("empty", "empty.ttf");
  face("large", "large.ttf");
  const auto id = stack("empty, large, sans-serif");
  EpubFontManager manager(epub, renderer);
  const auto selected = manager.resolve(BASE, id, 1);
  EXPECT_EQ(selected.fontId, 2);
  EXPECT_EQ(epub.extracts, 0);
  EXPECT_EQ(manager.memoryUsed(), 0);
  for (float scale : {0.0f, -1.0f, INFINITY, NAN, 1000.0f}) EXPECT_EQ(manager.resolve(BASE, id, scale).fontId, 2);
}

TEST_F(PublisherFonts, MonospaceUsesRealEqualAdvanceOutlineFamily) {
  EpubFontManager manager(epub, renderer);
  const auto selected = manager.resolve(BASE, wordTypography::Monospace, 1);
  ASSERT_TRUE(renderer.fonts.count(selected.fontId));
  const auto& fonts = family(selected);
  for (unsigned style = 0; style < 4; ++style) {
    const auto variant = static_cast<Style>(style);
    const auto width = fonts.getGlyph('i', variant).advanceX;
    EXPECT_GT(width, 0);
    for (char cp : {'W', 'm', '0', ' '}) EXPECT_EQ(fonts.getGlyph(cp, variant).advanceX, width);
  }
}

TEST_F(PublisherFonts, IdenticalCatalogClearReloadRetainsFacesAndFingerprint) {
  fourFaces();
  auto id = stack();
  EpubFontManager manager(epub, renderer);
  const auto hash = manager.fingerprint();
  const auto selected = manager.resolve(BASE, id, 1);
  ASSERT_TRUE(renderer.fonts.count(selected.fontId));
  const void* oldFace = &backend(selected);
  const auto memory = manager.memoryUsed();
  const auto removals = renderer.removals;
  epub.css.catalog.reset();
  fourFaces();
  id = stack();
  EXPECT_EQ(manager.fingerprint(), hash);
  const auto again = manager.resolve(BASE, id, 1);
  EXPECT_EQ(again.fontId, selected.fontId);
  EXPECT_EQ(&backend(again), oldFace);
  EXPECT_EQ(manager.memoryUsed(), memory);
  EXPECT_EQ(renderer.removals, removals);
  EXPECT_EQ(epub.extracts, 4);
}

TEST_F(PublisherFonts, FingerprintTracksFontBytesAcrossBookCacheInvalidation) {
  face("forms", "FixtureForms-Regular.ttf");
  stack();
  uint32_t first = 0;
  {
    EpubFontManager manager(epub, renderer);
    first = manager.fingerprint();
  }
  // The production EPUB cache owner removes extracted files when ZIP contents
  // change. Model that boundary here; this test does not pretend to test ZIPs.
  std::filesystem::remove_all(epub.cache);
  std::filesystem::create_directories(epub.cache);
  const auto source = FIXTURES / "FixtureForms-Regular.ttf";
  std::ifstream input(source, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
  const auto read16 = [&](size_t at) { return (unsigned(bytes.at(at)) << 8) | bytes.at(at + 1); };
  bool changed = false;
  for (unsigned i = 0; i < read16(4); ++i) {
    const size_t at = 12 + i * 16;
    if (std::string(reinterpret_cast<const char*>(bytes.data() + at), 4) == "head") {
      const uint32_t offset = (read16(at + 8) << 16) | read16(at + 10);
      bytes.at(offset + 35) ^= 1;  // Modified timestamp: same size, valid design.
      changed = true;
      break;
    }
  }
  ASSERT_TRUE(changed);
  const auto altered = std::filesystem::path(epub.cache) / "same-size-source.ttf";
  std::ofstream output(altered, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  output.close();
  epub.items["Fonts/FixtureForms-Regular.ttf"] = altered;
  EpubFontManager reopened(epub, renderer);
  EXPECT_NE(reopened.fingerprint(), first);
}

TEST_F(PublisherFonts, ManySizesStayBoundedAndPreviouslyUsedSizeCanBeRevisited) {
  face("optical", "FixtureOptical-Variable.ttf");
  const auto id = stack("optical, serif");
  std::vector<uint8_t> originalPixels;
  uint16_t originalAdvance = 0;
  uint32_t originalFingerprint = 0;
  {
    EpubFontManager manager(epub, renderer);
    originalFingerprint = manager.fingerprint();
    const auto initial = manager.resolve(BASE, id, 1);
    ASSERT_TRUE(renderer.fonts.count(initial.fontId));
    originalPixels = pixels(backend(initial), 'H');
    originalAdvance = family(initial).getGlyph('H').advanceX;
    int previousId = initial.fontId;
    for (unsigned size = 8; size <= 40; ++size) {
      SCOPED_TRACE(size);
      const auto selected = manager.resolve(BASE, id, size / 14.0f);
      ASSERT_TRUE(renderer.fonts.count(selected.fontId));
      EXPECT_LE(manager.memoryUsed(), 3u * 1024 * 1024);
      // ParsedText can still measure the preceding word while resolving the
      // next one. Keep the most recent pair of family registrations usable.
      EXPECT_TRUE(renderer.fonts.count(previousId));
      previousId = selected.fontId;
    }
    const auto again = manager.resolve(BASE, id, 1);
    ASSERT_TRUE(renderer.fonts.count(again.fontId));
    EXPECT_NEAR(backend(again).opticalSize(), 14, .01f);
    EXPECT_EQ(pixels(backend(again), 'H'), originalPixels);
    EXPECT_EQ(family(again).getGlyph('H').advanceX, originalAdvance);
    EXPECT_EQ(manager.fingerprint(), originalFingerprint);
    EXPECT_EQ(manager.failureEpoch(), 0u);
    EXPECT_LE(manager.memoryUsed(), 3u * 1024 * 1024);
  }
  EXPECT_TRUE(renderer.fonts.empty());
  EpubFontManager reopened(epub, renderer);
  EXPECT_EQ(reopened.fingerprint(), originalFingerprint);
  const auto again = reopened.resolve(BASE, id, 1);
  ASSERT_TRUE(renderer.fonts.count(again.fontId));
  EXPECT_EQ(pixels(backend(again), 'H'), originalPixels);
  EXPECT_EQ(family(again).getGlyph('H').advanceX, originalAdvance);
}
TEST_F(PublisherFonts, LargeLayoutSourcesReclaimWithinAggregateMemoryBudget) {
  face("large-layout", "FixtureLargeLayout.ttf");
  const auto id = stack("large-layout, serif");
  EpubFontManager manager(epub, renderer);
  int previousId = 0;
  size_t peak = 0;
  for (unsigned size = 10; size <= 30; size += 2) {
    SCOPED_TRACE(size);
    const auto selected = manager.resolve(BASE, id, size / 14.0f);
    ASSERT_TRUE(renderer.fonts.count(selected.fontId));
    EXPECT_TRUE(backend(selected).ready());
    EXPECT_FALSE(backend(selected).hasOpticalSize());
    EXPECT_NEAR(renderer.points.at(selected.fontId), size, .02f);
    const size_t used = manager.memoryUsed();
    peak = std::max(peak, used);
    EXPECT_LE(used, 3u * 1024 * 1024);
    if (previousId) EXPECT_TRUE(renderer.fonts.count(previousId));
    previousId = selected.fontId;
  }
  EXPECT_EQ(manager.failureEpoch(), 0u);
  EXPECT_GT(peak, 1024u * 1024);  // The large-table branch actually exerted pressure.
  const auto again = manager.resolve(BASE, id, 1);
  ASSERT_TRUE(renderer.fonts.count(again.fontId));
  EXPECT_NEAR(renderer.points.at(again.fontId), 14, .02f);
  EXPECT_LE(manager.memoryUsed(), 3u * 1024 * 1024);
}

TEST_F(PublisherFonts, CffAndTrueTypeMetricsStayWithinOriginalDesignBounds) {
  face("forms", "FixtureForms-Regular.ttf");
  face("cff", "FixtureCff-Regular.otf");
  const auto ttfId = stack(), cffId = stack("cff, serif");
  EpubFontManager manager(epub, renderer);
  // Independent source geometry: H has a 580x700 design-unit box and a 670
  // advance in a 1000-unit em. Density 2 at 150 dpi defines the raster pixel scale.
  for (unsigned points : {10, 14, 24, 36}) {
    for (auto id : {ttfId, cffId}) {
      SCOPED_TRACE(points);
      SCOPED_TRACE(id);
      const auto selected = manager.resolve(BASE, id, points / 14.0f);
      ASSERT_TRUE(renderer.fonts.count(selected.fontId));
      const auto glyph = family(selected).getGlyph('H');
      ASSERT_TRUE(glyph.valid);
      const float pixelsPerDesignUnit = points * 150.0f / 72 * 2 / 1000;
      EXPECT_NEAR(glyph.width, 580 * pixelsPerDesignUnit, 2.5f);
      EXPECT_NEAR(glyph.height, 700 * pixelsPerDesignUnit, 2.5f);
      EXPECT_NEAR(glyph.advanceX, 670 * pixelsPerDesignUnit * 16, 1.1f);
      EXPECT_LE(std::abs(glyph.left), 2);
      EXPECT_NEAR(glyph.top, 700 * pixelsPerDesignUnit, 2.5f);
      EXPECT_GT(ink(backend(selected), 'H'), 0);
    }
  }
}

TEST_F(PublisherFonts, SameLengthCorruptExtractedCacheIsRepairedFromAuthoredSource) {
  face("forms", "FixtureForms-Regular.ttf");
  const auto id = stack();
  uint32_t originalFingerprint = 0;
  std::vector<uint8_t> originalPixels;
  {
    EpubFontManager first(epub, renderer);
    originalFingerprint = first.fingerprint();
    const auto selected = first.resolve(BASE, id, 1);
    ASSERT_TRUE(renderer.fonts.count(selected.fontId));
    originalPixels = pixels(backend(selected), 'H');
  }
  std::filesystem::path cache;
  for (const auto& entry : std::filesystem::directory_iterator(epub.cache))
    if (entry.path().extension() == ".bin") cache = entry.path();
  ASSERT_FALSE(cache.empty());
  const auto length = std::filesystem::file_size(cache);
  // A header and a payload mutation both keep the same file length. Do not rely
  // on FreeType rejecting bad magic: it does not validate every authored checksum.
  for (const auto offset : {uintmax_t(0), length / 2}) {
    {
      std::fstream data(cache, std::ios::binary | std::ios::in | std::ios::out);
      ASSERT_TRUE(data);
      data.seekg(offset);
      char byte = 0;
      data.get(byte);
      data.seekp(offset);
      data.put(static_cast<char>(static_cast<unsigned char>(byte) ^ 0x40));
    }
    ASSERT_EQ(std::filesystem::file_size(cache), length);
    const unsigned extracts = epub.extracts;
    EpubFontManager repaired(epub, renderer);
    EXPECT_EQ(epub.extracts, extracts + 1);
    EXPECT_EQ(repaired.fingerprint(), originalFingerprint);
    const auto selected = repaired.resolve(BASE, id, 1);
    ASSERT_TRUE(renderer.fonts.count(selected.fontId));
    EXPECT_EQ(pixels(backend(selected), 'H'), originalPixels);
    EXPECT_EQ(repaired.failureEpoch(), 0u);
  }
}

TEST_F(PublisherFonts, CorruptCacheRepairFailureIsTransientAndRecoveryUsesAuthoredMetrics) {
  face("forms", "FixtureForms-Regular.ttf");
  const auto id = stack();
  uint32_t originalFingerprint;
  {
    EpubFontManager first(epub, renderer);
    originalFingerprint = first.fingerprint();
  }
  for (const auto& entry : std::filesystem::directory_iterator(epub.cache)) {
    if (entry.path().extension() != ".bin") continue;
    std::fstream data(entry.path(), std::ios::binary | std::ios::in | std::ios::out);
    char byte = 0;
    data.get(byte);
    data.seekp(0);
    data.put(byte ^ 0x40);
  }
  epub.failingExtractions.insert("Fonts/FixtureForms-Regular.ttf");
  EpubFontManager manager(epub, renderer);
  EXPECT_NE(manager.fingerprint(), originalFingerprint);
  EXPECT_GT(manager.failureEpoch(), 0u);
  EXPECT_EQ(manager.resolve(BASE, id, 1).fontId, BASE);
  const auto failedEpoch = manager.failureEpoch();
  epub.failingExtractions.clear();
  const auto recovered = manager.resolve(BASE, id, 1);
  ASSERT_TRUE(renderer.fonts.count(recovered.fontId));
  EXPECT_EQ(manager.fingerprint(), originalFingerprint);
  EXPECT_GT(manager.failureEpoch(), failedEpoch);
  EXPECT_TRUE(family(recovered).getGlyph('H'));
}

}  // namespace
