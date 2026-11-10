#include <CpFontFormat.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <PageWordGeometry.h>
#include <SdCardFont.h>
#include <TouchReaderPreviewModel.h>
#include <gtest/gtest.h>

#include <array>
#include <cmath>

#include "Epub/converters/DirectPixelWriter.h"
#include "lib/Epub/Epub/blocks/ImageBlock.cpp"

bool ImageDecoderFactory::supportsMemorySource(const std::string&) { return false; }
ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string&) { return nullptr; }
bool ImageDecoderFactory::supportsMemorySource(ImageFormatDetector::Format) { return false; }
ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const uint8_t*, size_t) { return nullptr; }
ImageFormatDetector::Format ImageFormatDetector::detect(const uint8_t*, size_t) { return Format::Unknown; }
bool ImageFormatDetector::isBaselineJpeg(const uint8_t*, size_t) { return true; }

namespace {
// Deterministic 2-bit glyphs with negative bearings and descenders. Both the
// built-in and real .cpfont loaders use these bytes, including RTL/CJK/marks.
struct RasterFont {
  std::vector<EpdUnicodeInterval> intervals;
  std::vector<EpdGlyph> glyphs;
  std::vector<uint8_t> pixels;
  EpdFontData data{};
  EpdFont font{&data};
  explicit RasterFont(int size) {
    for (auto [first, last] : {std::pair{32u, 127u},
                               {0xB7u, 0xB7u},
                               {0x300u, 0x36Fu},
                               {0x590u, 0x6FFu},
                               {0x4E00u, 0x4E20u},
                               {0xFB00u, 0xFB06u},
                               {0xFB50u, 0xFEFFu},
                               {0xFFFDu, 0xFFFDu}}) {
      intervals.push_back({first, last, static_cast<uint32_t>(glyphs.size())});
      for (unsigned cp = first; cp <= last; ++cp) {
        EpdGlyph g{};
        g.width = size;
        g.height = size + 4;
        g.advanceX = size * 16;
        g.left = -3;
        g.top = size + 1;
        if (cp >= 0x300 && cp <= 0x36f) {
          g.advanceX = 0;
          g.top = size + 5;
        }
        g.dataOffset = pixels.size();
        g.dataLength = (g.width * g.height + 3) / 4;
        glyphs.push_back(g);
        for (int b = 0; b < g.dataLength; ++b) pixels.push_back(b % 2 ? 0x1B : 0xE4);
      }
    }
    data.bitmap = pixels.data();
    data.glyph = glyphs.data();
    data.intervals = intervals.data();
    data.intervalCount = intervals.size();
    data.advanceY = size + 6;
    data.ascender = size + 1;
    data.descender = -3;
    data.is2Bit = true;
  }
  template <class T>
  static void append(std::vector<uint8_t>& bytes, const T& value) {
    auto p = reinterpret_cast<const uint8_t*>(&value);
    bytes.insert(bytes.end(), p, p + sizeof(value));
  }
  std::vector<uint8_t> file() const {
    std::vector<uint8_t> bytes(64);
    memcpy(bytes.data(), "CPFONT\0\0", 8);
    auto put16 = [&](int o, uint16_t v) { memcpy(bytes.data() + o, &v, 2); };
    auto put32 = [&](int o, uint32_t v) { memcpy(bytes.data() + o, &v, 4); };
    put16(8, CPFONT_VERSION);
    put16(10, 1);
    bytes[12] = 1;
    put32(36, intervals.size());
    put32(40, glyphs.size());
    bytes[44] = data.advanceY;
    put16(45, data.ascender);
    put16(47, uint16_t(data.descender));
    put32(56, 64);
    for (auto& i : intervals) append(bytes, i);
    for (auto& g : glyphs) append(bytes, g);
    bytes.insert(bytes.end(), pixels.begin(), pixels.end());
    return bytes;
  }
};

// Exercise the actual callback ordering in the production tiled text renderer.
TEST(EpubTextGrayscaleTest, OffStripScalableGlyphsDoNotRequestRasters) {
  fakeheap::reset(true);
  Storage.reset();
  RasterFont fixture(12);
  HalDisplay display(800, 480);
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  struct Context {
    RasterFont* font;
    int calls = 0;
  } context{&fixture};
  fixture.data.glyphMissCtx = &context;
  fixture.data.rasterHandler = [](void* ptr, uint32_t cp, int8_t phase, EpdGlyph& glyph, const uint8_t*& bitmap) {
    auto& ctx = *static_cast<Context*>(ptr);
    ++ctx.calls;
    glyph = *ctx.font->font.getGlyph(cp);
    glyph.left += phase > 0 ? 1 : 0;
    bitmap = ctx.font->pixels.data() + glyph.dataOffset;
    return true;
  };
  renderer.insertFont(1, EpdFontFamily(&fixture.font));
  std::vector<uint8_t> strip(display.stride * 8);
  renderer.beginStripTarget(strip.data(), 0, 8);
  renderer.drawText(1, 20, 200, "ABC");
  renderer.endStripTarget();
  EXPECT_EQ(context.calls, 0);
  renderer.drawText(1, 20, 200, "ABC");
  EXPECT_EQ(context.calls, 3);
}

TEST(EpubTextGrayscaleTest, AuthoredCapsUseTheirOwnAdvanceAndPixels) {
  fakeheap::reset(true);
  Storage.reset();
  RasterFont fixture(12);
  auto& cap = fixture.glyphs['A' - 32];
  cap.advanceX = 9 * 16;
  fixture.data.smallCapsHandler = [](void*, uint32_t cp, bool) -> uint32_t { return cp == 'a' ? 'A' : 0; };
  HalDisplay display(800, 480);
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  renderer.insertFont(1, EpdFontFamily(&fixture.font));
  EXPECT_EQ(renderer.getTextAdvanceX(1, "aaa", EpdFontFamily::SMALL_CAPS), 27);
  renderer.clearScreen();
  renderer.drawText(1, 20, 20, "aaa", true, EpdFontFamily::SMALL_CAPS);
  const auto authored = display.bw;
  renderer.clearScreen();
  renderer.drawText(1, 20, 20, "AAA");
  EXPECT_EQ(authored, display.bw);
  // Unmapped lowercase retains CrossInk's synthetic small caps fallback.
  EXPECT_EQ(renderer.getTextAdvanceX(1, "bbb", EpdFontFamily::SMALL_CAPS), 27);
}

TEST(EpubTextGrayscaleTest, CharacterSpacingMeasuresAndDrawsTheSameGlyphPositions) {
  for (bool sd : {false, true}) {
    fakeheap::reset(true);
    Storage.reset();
    RasterFont fixture(12);
    HalDisplay display(800, 480);
    GfxRenderer renderer(display);
    renderer.begin();
    SdCardFont sdFont;
    if (sd) {
      Storage.put("spacing.cpfont", fixture.file());
      ASSERT_TRUE(sdFont.load("spacing.cpfont"));
      renderer.insertFont(1, EpdFontFamily(sdFont.getEpdFont()));
      renderer.registerSdCardFont(1, &sdFont);
      sdFont.prewarm(
          "ABCD A B A\xcc\x81"
          "BCD",
          0x01);
    } else {
      renderer.insertFont(1, EpdFontFamily(&fixture.font));
    }
    renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
    for (int spacing = -5; spacing <= 5; ++spacing) {
      SCOPED_TRACE(spacing);
      // Four equal glyphs: three adjustable gaps, with half-pixel rounding.
      const int shift = (3 * spacing + 1) >> 1;
      EXPECT_EQ(renderer.getTextAdvanceX(1, "ABCD", EpdFontFamily::REGULAR, 0, spacing), 48 + shift);
      EXPECT_EQ(renderer.getTextAdvanceX(1, "A B", EpdFontFamily::REGULAR, 0, spacing), 36);
      EXPECT_EQ(renderer.getTextAdvanceX(1,
                                         "A\xcc\x81"
                                         "BCD",
                                         EpdFontFamily::REGULAR, 0, spacing),
                48 + shift);
      renderer.clearScreen();
      renderer.drawText(1, 20, 20, "ABCD", true, EpdFontFamily::REGULAR, BidiUtils::BidiBaseDir::AUTO, spacing);
      const auto actual = display.bw;
      renderer.clearScreen();
      for (int i = 0; i < 4; ++i) {
        char letter[] = {static_cast<char>('A' + i), 0};
        renderer.drawText(1, 20 + i * 12 + ((i * spacing + 1) >> 1), 20, letter);
      }
      EXPECT_EQ(display.bw, actual);
    }
    EXPECT_EQ(renderer.getTextAdvanceX(1, "ABCD", EpdFontFamily::REGULAR), 48);
  }
}
TEST(EpubTextGrayscaleTest, PublisherTrackingMeasuresAndDrawsTheSameGlyphPositions) {
  for (bool sd : {false, true}) {
    fakeheap::reset(true);
    Storage.reset();
    RasterFont fixture(12);
    HalDisplay display(800, 480);
    GfxRenderer renderer(display);
    renderer.begin();
    SdCardFont sdFont;
    if (sd) {
      Storage.put("spacing.cpfont", fixture.file());
      ASSERT_TRUE(sdFont.load("spacing.cpfont"));
      renderer.insertFont(1, EpdFontFamily(sdFont.getEpdFont()));
      renderer.registerSdCardFont(1, &sdFont);
      sdFont.prewarm(
          "ABCD A B A\xcc\x81"
          "BCD",
          0x01);
    } else {
      renderer.insertFont(1, EpdFontFamily(&fixture.font));
    }
    renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
    for (int tracking : {-127, -31, -8, 0, 7, 19, 65}) {
      SCOPED_TRACE(tracking);
      const int spacing = 2;
      // Four equal glyphs: three adjustable gaps, with half-pixel rounding.
      const int step16 = spacing * 8 + tracking;
      const int shift = (3 * step16 + 8) >> 4;
      EXPECT_EQ(renderer.getTextAdvanceX(1, "ABCD", EpdFontFamily::REGULAR, 0, spacing, tracking), 48 + shift);
      EXPECT_EQ(renderer.getTextAdvanceX(1, "A B", EpdFontFamily::REGULAR, 0, spacing, tracking), 36);
      EXPECT_EQ(renderer.getTextAdvanceX(1,
                                         "A\xcc\x81"
                                         "BCD",
                                         EpdFontFamily::REGULAR, 0, spacing, tracking),
                48 + shift);
      renderer.clearScreen();
      renderer.drawText(1, 20, 20, "ABCD", true, EpdFontFamily::REGULAR, BidiUtils::BidiBaseDir::AUTO, spacing,
                        tracking);
      const auto actual = display.bw;
      renderer.clearScreen();
      for (int i = 0; i < 4; ++i) {
        char letter[] = {static_cast<char>('A' + i), 0};
        renderer.drawText(1, 20 + i * 12 + ((i * step16 + 8) >> 4), 20, letter);
      }
      EXPECT_EQ(display.bw, actual);
    }
    EXPECT_EQ(renderer.getTextAdvanceX(1, "ABCD", EpdFontFamily::REGULAR), 48);
  }
}

TEST(EpubTextGrayscaleTest, CharacterSpacingDecorationsFollowTextWidth) {
  fakeheap::reset(true);
  Storage.reset();
  RasterFont fixture(12);
  HalDisplay display(800, 480);
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.insertFont(1, EpdFontFamily(&fixture.font));
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  for (int8_t spacing : {-5, 5}) {
    for (auto decoration : {EpdFontFamily::UNDERLINE, EpdFontFamily::STRIKETHROUGH}) {
      for (auto size : {EpdFontFamily::REGULAR, EpdFontFamily::SUP}) {
        const auto style = static_cast<EpdFontFamily::Style>(decoration | size);
        TextBlock line({"ABCD"}, {0}, {style}, {}, {}, {}, {}, {}, {}, {}, {}, "", spacing);
        renderer.clearScreen();
        line.render(renderer, 1, 20, 20, true);
        const auto actual = display.bw;
        renderer.clearScreen();
        const int wordY = 20 + line.wordYOffset(renderer, 1, 0);
        renderer.drawText(1, 20, wordY, "ABCD", true, style, BidiUtils::BidiBaseDir::AUTO, spacing);
        const int width = renderer.getTextAdvanceX(1, "ABCD", style, 0, spacing);
        const int glyphHeight = size == EpdFontFamily::SUP ? 7 : 13;
        const int lineY = decoration == EpdFontFamily::UNDERLINE ? wordY + 15 : wordY + 13 - glyphHeight / 2;
        renderer.drawLine(20, lineY, 20 + width, lineY, 3, true);
        EXPECT_EQ(display.bw, actual) << int(spacing) << ' ' << int(style);
      }
    }
  }
}

TEST(EpubTextGrayscaleTest, RealTextRasterMatchesFullAndStripTargets) {
  for (bool sd : {false, true})
    for (int size : {12, 20})
      for (int orientation = 0; orientation < 4; ++orientation) {
        SCOPED_TRACE(testing::Message() << sd << ' ' << size << ' ' << orientation);
        fakeheap::reset(true);
        Storage.reset();
        RasterFont fixture(size);
        SdCardFont sdFont;
        HalDisplay display(orientation % 2 ? 800 : 792, 481);
        GfxRenderer renderer(display);
        renderer.begin();
        if (sd) {
          Storage.put("font.cpfont", fixture.file());
          ASSERT_TRUE(sdFont.load("font.cpfont"));
          renderer.insertFont(1, EpdFontFamily(sdFont.getEpdFont()));
          renderer.registerSdCardFont(1, &sdFont);
        } else
          renderer.insertFont(1, EpdFontFamily(&fixture.font));
        renderer.setOrientation(GfxRenderer::Orientation(orientation));
        FontCacheManager cache(renderer.getFontMap(), renderer.getSdCardFonts());
        renderer.setFontCacheManager(&cache);
        const std::vector<std::string> words = {"Abc", "e\u0301", "שלום", "سلام", "一", "\U0001F642", " "};
        const std::vector<int16_t> offsets = {0, 65, 120, 190, 260, 290, 330};
        const std::vector<EpdFontFamily::Style> styles = {
            EpdFontFamily::REGULAR, EpdFontFamily::BOLD,    EpdFontFamily::ITALIC, EpdFontFamily::BOLD_ITALIC,
            EpdFontFamily::REGULAR, EpdFontFamily::REGULAR, EpdFontFamily::REGULAR};
        const auto bw = display.bw;
        for (int feature = 0; feature < 5; ++feature) {
          auto variants = styles;
          BlockStyle blockStyle;
          blockStyle.isRtl = feature == 4;
          for (auto& style : variants)
            style = EpdFontFamily::Style(style | (feature == 0 ? EpdFontFamily::UNDERLINE | EpdFontFamily::STRIKETHROUGH
                                                  : feature == 1 ? EpdFontFamily::SMALL_CAPS
                                                  : feature == 2 ? EpdFontFamily::SUP
                                                  : feature == 3 ? EpdFontFamily::SUB
                                                                 : 0));
          TextBlock line(words, offsets, variants,
                         feature == 1 ? std::vector<uint8_t>(words.size(), 1) : std::vector<uint8_t>{},
                         feature == 1 ? std::vector<uint16_t>(words.size(), 12) : std::vector<uint16_t>{},
                         std::vector<uint16_t>(words.size(), 6),
                         std::vector<uint8_t>(words.size(), TextBlock::WORD_FLAG_BACKGROUND_BLACK), {}, blockStyle,
                         feature == 4 ? std::vector<std::string>(words.size(), "Ab") : std::vector<std::string>{});
          ASSERT_TRUE(line.valid());
          auto draw = [&] {
            for (int y : {-8, 65, 79, 145, 159, 230, 310, 390, 470}) line.render(renderer, 1, 7, y, true);
          };
          {
            auto scan = cache.createPrewarmScope();
            draw();
            scan.endScanAndPrewarm();
          }
          EXPECT_EQ(display.bw, bw);
          for (auto mode : {GfxRenderer::BW, GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
            renderer.setRenderMode(mode);
            std::vector<uint8_t> full(display.bw.size()), strips(display.bw.size());
            renderer.beginStripTarget(full.data(), 0, display.height);
            renderer.clearScreen(mode == GfxRenderer::BW ? 255 : 0);
            draw();
            renderer.endStripTarget();
            for (int y = 0; y < display.height; y += 80) {
              int rows = std::min(80, display.height - y);
              renderer.beginStripTarget(strips.data() + size_t(y) * display.stride, y, rows);
              renderer.clearScreen(mode == GfxRenderer::BW ? 255 : 0);
              draw();
              renderer.endStripTarget();
            }
            EXPECT_EQ(full, strips) << "feature=" << feature << " mode=" << mode;
            EXPECT_TRUE(std::any_of(full.begin(), full.end(),
                                    [&](auto b) { return b != (mode == GfxRenderer::BW ? 255 : 0); }));
            EXPECT_EQ(display.bw, bw);
            EXPECT_EQ(renderer.getWriteTarget(), display.bw.data());
          }
          renderer.setRenderMode(GfxRenderer::BW);
        }
        renderer.setFontCacheManager(nullptr);
        renderer.removeFont(1);
      }
}
}  // namespace

TEST(EpubTextRaster, VariationSelectorsDoNotDrawOrAdvance) {
  for (const bool sd : {false, true}) {
    SCOPED_TRACE(testing::Message() << "sd=" << sd);
    fakeheap::reset(true);
    Storage.reset();
    RasterFont fixture(12);
    SdCardFont sdFont;
    HalDisplay display;
    GfxRenderer renderer(display);
    renderer.begin();
    if (sd) {
      Storage.put("font.cpfont", fixture.file());
      ASSERT_TRUE(sdFont.load("font.cpfont"));
      renderer.insertFont(1, EpdFontFamily(sdFont.getEpdFont()));
      renderer.registerSdCardFont(1, &sdFont);
    } else {
      renderer.insertFont(1, EpdFontFamily(&fixture.font));
    }

    const auto render = [&renderer, &display](const char* text) {
      renderer.clearScreen();
      renderer.drawText(1, 25, 40, text);
      return display.bw;
    };

    const auto base = render("!");
    const int baseAdvance = renderer.getTextAdvanceX(1, "!", EpdFontFamily::REGULAR);
    for (const char* text : {"!\xE1\xA0\x8B", "!\xEF\xB8\x8E", "!\xEF\xB8\x8F"}) {  // U+180B/U+FE0E/U+FE0F
      EXPECT_EQ(renderer.getTextAdvanceX(1, text, EpdFontFamily::REGULAR), baseAdvance);
      EXPECT_EQ(render(text), base);
    }
    renderer.removeFont(1);
  }
}

TEST(AbsoluteImageRaster, TextMatchesBlackWhiteInBothPlanesAndCancellationResetsMode) {
  fakeheap::reset(true);
  Storage.reset();
  RasterFont fixture(12);
  HalDisplay display;
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.insertFont(1, EpdFontFamily(&fixture.font));
  for (int orientation = 0; orientation < 4; ++orientation) {
    renderer.setOrientation(GfxRenderer::Orientation(orientation));
    renderer.clearScreen();
    renderer.drawText(1, 25, 40, "Book cover");
    const auto expected = display.bw;
    ASSERT_TRUE(renderer.displayAbsoluteGrayscaleBase());
    for (auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
      renderer.clearScreen();
      renderer.setRenderMode(mode);
      renderer.drawText(1, 25, 40, "Book cover");
      EXPECT_EQ(display.bw, expected);
    }
    renderer.setRenderMode(GfxRenderer::BW);
    EXPECT_FALSE(renderer.grayPlanesAreAbsolute());
  }
  EXPECT_EQ(display.canceled, 4);
  display.absoluteSupported = false;
  EXPECT_FALSE(renderer.supportsAbsoluteGrayscale());
  EXPECT_FALSE(renderer.displayAbsoluteGrayscaleBase());
  EXPECT_FALSE(renderer.grayPlanesAreAbsolute());
}

TEST(AbsoluteImageRaster, BitmapPlanesPreserveFourTonesAndWhiteMargins) {
  fakeheap::reset(true);
  Storage.reset();
  // Two identical bottom-up rows of black/dark/light/white, padded to four bytes.
  auto data = std::make_shared<HostFileData>();
  data->bytes.resize(78, 0);
  auto put16 = [&](int offset, uint16_t value) { memcpy(data->bytes.data() + offset, &value, 2); };
  auto put32 = [&](int offset, uint32_t value) { memcpy(data->bytes.data() + offset, &value, 4); };
  put16(0, 0x4d42);
  put32(2, 78);
  put32(10, 70);
  put32(14, 40);
  put32(18, 4);
  put32(22, 2);
  put16(26, 1);
  put16(28, 2);
  put32(34, 8);
  put32(46, 4);
  for (int level = 0; level < 4; ++level)
    for (int channel = 0; channel < 3; ++channel) data->bytes[54 + level * 4 + channel] = level * 85;
  data->bytes[70] = data->bytes[74] = 0x1b;
  HalFile file(data);
  Bitmap bitmap(file);
  ASSERT_EQ(bitmap.parseHeaders(), BmpReaderError::Ok);
  HalDisplay display(8, 4);
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  ASSERT_TRUE(renderer.displayAbsoluteGrayscaleBase());
  int plane = 0;
  for (auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
    ASSERT_EQ(bitmap.rewindToData(), BmpReaderError::Ok);
    renderer.clearScreen();
    renderer.setRenderMode(mode);
    ASSERT_TRUE(renderer.drawBitmap(bitmap, 0, 3, 8, 4));
    // The first file row lies below the screen. The second must still be read.
    EXPECT_EQ(display.bw[0], 0xff);
    EXPECT_EQ(display.bw[1], 0xff);
    EXPECT_EQ(display.bw[2], 0xff);
    EXPECT_EQ(display.bw[3], plane++ == 0 ? 0x5f : 0x3f);
  }
  // A truncated second pass must be reported, allowing the caller to cancel it.
  ASSERT_EQ(bitmap.rewindToData(), BmpReaderError::Ok);
  data->readFailAt = 74;
  EXPECT_FALSE(renderer.drawBitmap(bitmap, 0, 0, 8, 4));
  renderer.setRenderMode(GfxRenderer::BW);
  EXPECT_EQ(display.canceled, 1);
  file.close();
}

// A font switch begins with no resident glyphs. A wide replacement glyph makes
// the cold scan fit fewer words than the final, correctly measured preview.
TEST(EpubTextGrayscaleTest, ColdSdSamplePreviewMatchesFullyLoadedFont) {
  for (int size : {12, 20}) {
    for (int width : {160, 280}) {
      for (bool focus : {false, true}) {
        for (bool guide : {false, true}) {
          SCOPED_TRACE(testing::Message() << size << " width=" << width << " focus=" << focus << " guide=" << guide);
          fakeheap::reset(false);
          Storage.reset();
          RasterFont fixture(size);
          fixture.glyphs.back().advanceX = size * 16 * 4;
          SdCardFont sdFont;
          Storage.put("preview.cpfont", fixture.file());
          ASSERT_TRUE(sdFont.load("preview.cpfont"));
          HalDisplay display;
          GfxRenderer renderer(display);
          renderer.begin();
          renderer.insertFont(1, EpdFontFamily(sdFont.getEpdFont()));
          renderer.registerSdCardFont(1, &sdFont);
          renderer.insertFont(2, EpdFontFamily(&fixture.font));
          FontCacheManager cache(renderer.getFontMap(), renderer.getSdCardFonts());
          renderer.setFontCacheManager(&cache);
          SampleReaderPreviewModel model;
          ASSERT_TRUE(model.captureParagraph(READER_PREVIEW_PARAGRAPH));
          auto draw = [&](int font) {
            model.renderText(renderer, font, 10, 10, width, 100, 0, static_cast<uint8_t>(CssTextAlign::Left), focus,
                             guide, true, 85);
          };
          renderer.clearScreen();
          draw(2);
          const auto expected = display.bw;
          renderer.clearScreen();
          const auto blank = display.bw;
          auto scope = cache.createPrewarmScope();
          draw(1);
          EXPECT_EQ(display.bw, blank);  // Scanning must not paint the display.
          ASSERT_TRUE(scope.endScanAndPrewarm());
          draw(1);
          EXPECT_NE(display.bw, blank);
          EXPECT_TRUE(display.bw == expected);
        }
      }
    }
  }
}

TEST(EpubTextGrayscaleTest, PublisherSpacingPreviewMatchesPixelsBeforeAndAfterCacheReload) {
  fakeheap::reset(true);
  Storage.reset();
  RasterFont fixture(11);
  fixture.data.advanceY = 29;
  HalDisplay display(120, 100);
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.insertFont(1, EpdFontFamily(&fixture.font).withSize(100, 11));
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  style.lineHeightMultiplier = 0.8f;  // CSS line-height: 1.2, normalized by the parser.
  style.lineHeight = 23;              // Source page at 100%.
  Page page;
  for (const bool second : {false, true}) {
    auto block = std::make_shared<TextBlock>(
        std::vector<std::string>{second ? "BB" : "AA"}, std::vector<int16_t>{0},
        std::vector<EpdFontFamily::Style>{EpdFontFamily::REGULAR}, std::vector<uint8_t>{},
        std::vector<uint16_t>{}, std::vector<uint16_t>{}, std::vector<uint8_t>{},
        std::vector<bool>{second}, style);
    page.elements.push_back(std::make_unique<PageLine>(block, 0, second ? 23 : 0));
  }
  HalFile file;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", "publisher-spacing", file));
  ASSERT_TRUE(page.serialize(file));
  ASSERT_TRUE(file.seek(0));
  auto cached = Page::deserialize(file);
  file.close();
  ASSERT_TRUE(cached);
  for (const Page* source : {&page, cached.get()}) {
    TouchReaderPreviewModel preview;
    ASSERT_TRUE(preview.capture(*source, renderer, 1, 100));
    for (const uint8_t percent : {100, 105}) {
      renderer.clearScreen();
      renderer.drawText(1, 10, 10, "AA");
      renderer.drawText(1, 10, percent == 100 ? 33 : 34, "BB");
      const auto expected = display.bw;
      renderer.clearScreen();
      preview.renderText(renderer, 1, 10, 10, 22, percent, 0, static_cast<uint8_t>(CssTextAlign::Left), false,
                         false, true);
      EXPECT_EQ(display.bw, expected) << "spacing=" << int(percent) << ", cached=" << (source != &page);
    }
  }
}

TEST(EpubTextGrayscaleTest, NightModeImagesKeepPolarityAndMidtonesOnRepeatedDraws) {
  for (int orientation = 0; orientation < 4; ++orientation) {
    for (bool grayscale : {false, true}) {
      for (int level = 0; level < 4; ++level) {
        SCOPED_TRACE(testing::Message() << orientation << ' ' << grayscale << ' ' << level);
        HalDisplay display(800, 480);
        GfxRenderer renderer(display);
        renderer.begin();
        renderer.setOrientation(GfxRenderer::Orientation(orientation));
        // The light-mode monochrome image is the expected Night Mode output.
        renderer.clearScreen(255);
        DirectPixelWriter writer;
        const auto draw = [&](bool imageGrayscale) {
          writer.init(renderer, imageGrayscale);
          for (int y = 13; y < 29; ++y) {
            writer.beginRow(y);
            for (int x = 9; x < 25; ++x) writer.writePixel(x, level);
          }
          renderer.preserveImagePolarity(9, 13, 16, 16);
        };
        draw(false);
        const auto expected = display.bw;
        display.inverted = true;
        renderer.clearScreen(255);
        for (int redraw = 0; redraw < 3; ++redraw) {
          draw(grayscale);
          // Apply output inversion only to the image rectangle for comparison;
          // surrounding text/background is expected to change in Night Mode.
          renderer.invertRect(9, 13, 16, 16);
          EXPECT_EQ(display.bw, expected) << redraw;
          renderer.invertRect(9, 13, 16, 16);
        }
      }
    }
  }
}

TEST(EpubTextGrayscaleTest, CachedNightModeImagesMatchMonochromeAcrossStoragePaths) {
  for (bool psram : {false, true}) {
    for (int orientation = 0; orientation < 4; ++orientation) {
      SCOPED_TRACE(testing::Message() << psram << ' ' << orientation);
      fakeheap::reset(psram);
      Storage.reset();
      ImageBlock::clearSessionRenderFailures();
      std::vector<uint8_t> pixels(4 + 16 * 4, 0x1B);
      pixels[0] = pixels[2] = 16;
      pixels[1] = pixels[3] = 0;
      Storage.put("image.pxc", pixels);
      HalDisplay display(800, 480);
      GfxRenderer renderer(display);
      renderer.begin();
      renderer.setOrientation(GfxRenderer::Orientation(orientation));
      ImageBlock image("image.png", "source.png", 16, 16);
      renderer.clearScreen(255);
      image.render(renderer, 9, 13, true, false);
      const auto expected = display.bw;
      display.inverted = true;
      renderer.clearScreen(255);
      for (int redraw = 0; redraw < 3; ++redraw) {
        image.render(renderer, 9, 13, true, true);
        renderer.invertRect(9, 13, 16, 16);
        EXPECT_EQ(display.bw, expected) << redraw;
        renderer.invertRect(9, 13, 16, 16);
      }
      ImageBlock::clearSessionRenderFailures();
    }
  }
}

namespace {
class TaggedPageElement final : public PageElement {
 public:
  explicit TaggedPageElement(PageElementTag tag) : PageElement(0, 0), tag(tag) {}
  void render(GfxRenderer&, int, int, int, bool) override {}
  bool serialize(Print&) override { return true; }
  PageElementTag getTag() const override { return tag; }

 private:
  PageElementTag tag;
};
}  // namespace
TEST(PageTextDetection, OnlyTextAndTablesNeedTextAntialiasing) {
  Page page;
  EXPECT_FALSE(page.hasText());
  for (auto tag : {TAG_PageImage, TAG_PageHorizontalRule}) {
    page.elements.emplace_back(std::make_unique<TaggedPageElement>(tag));
    EXPECT_FALSE(page.hasText());
  }
  for (auto tag : {TAG_PageLine, TAG_PageTableFragment}) {
    page.elements.emplace_back(std::make_unique<TaggedPageElement>(tag));
    EXPECT_TRUE(page.hasText());
    page.elements.pop_back();
  }
}

TEST(EpubTextGrayscaleTest, CssOnlyTrackingKeepsLigatureLettersSeparate) {
  fakeheap::reset(true);
  Storage.reset();
  RasterFont fixture(12);
  HalDisplay display(800, 480);
  GfxRenderer renderer(display);
  renderer.begin();
  const EpdLigaturePair pairs[] = {{(uint32_t('f') << 16) | 'f', 0xFB00}, {(0xFB00u << 16) | 'i', 0xFB03}};
  fixture.data.ligaturePairs = pairs;
  fixture.data.ligaturePairCount = 2;
  renderer.insertFont(1, EpdFontFamily(&fixture.font));
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  EXPECT_EQ(renderer.getTextAdvanceX(1, "ffi", EpdFontFamily::REGULAR), 12);
  EXPECT_EQ(renderer.getTextAdvanceX(1, "ffi", EpdFontFamily::REGULAR, 0, 0, 16), 38);
  renderer.clearScreen();
  renderer.drawText(1, 20, 30, "ffi", true, EpdFontFamily::REGULAR, BidiUtils::BidiBaseDir::AUTO, 0, 16);
  const auto actual = display.bw;
  renderer.clearScreen();
  renderer.drawText(1, 20, 30, "f");
  renderer.drawText(1, 33, 30, "f");
  renderer.drawText(1, 46, 30, "i");
  EXPECT_EQ(display.bw, actual);
}
TEST(EpubTextGrayscaleTest, RubyCentersOnEachWordsOwnCssTracking) {
  fakeheap::reset(true);
  Storage.reset();
  RasterFont fixture(12);
  HalDisplay display(800, 480);
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.insertFont(1, EpdFontFamily(&fixture.font));
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  TextBlock block({"AB", "CD"}, {0, 24}, {EpdFontFamily::REGULAR, EpdFontFamily::RUBY_CONTINUE}, {}, {}, {}, {}, {},
                  BlockStyle{}, {"AB", ""}, {}, "", 0, {}, {0, 64});
  ASSERT_TRUE(block.valid());
  renderer.clearScreen();
  block.render(renderer, 1, 40, 50);
  const auto actual = display.bw;
  renderer.clearScreen();
  renderer.drawText(1, 40, 56, "AB");
  renderer.drawText(1, 64, 56, "CD", true, EpdFontFamily::RUBY_CONTINUE, BidiUtils::BidiBaseDir::AUTO, 0, 64);
  const int groupWidth = renderer.getTextAdvanceX(1, "AB", EpdFontFamily::REGULAR) +
                         renderer.getTextAdvanceX(1, "CD", EpdFontFamily::RUBY_CONTINUE, 0, 0, 64);
  const int rubyWidth = renderer.getTextAdvanceX(1, "AB", EpdFontFamily::SUP);
  renderer.drawText(1, 40 + (groupWidth - rubyWidth) / 2, 56 - renderer.getFontAscenderSize(1), "AB", true,
                    EpdFontFamily::SUP);
  EXPECT_EQ(display.bw, actual);
}

TEST(EpubTextGrayscaleTest, HitGeometryUsesCachedPublisherTracking) {
  fakeheap::reset(true);
  Storage.reset();
  RasterFont fixture(12);
  HalDisplay display(800, 480);
  GfxRenderer renderer(display);
  renderer.begin();
  renderer.insertFont(1, EpdFontFamily(&fixture.font));
  TextBlock block({"ABCD"}, {7}, {EpdFontFamily::REGULAR}, {}, {}, {}, {}, {}, BlockStyle{}, {}, {}, "", 0, {}, {32});
  PageTextLine line{};
  const auto geometry = pageWordGeometry(renderer, 1, line, block, 0);
  EXPECT_EQ(geometry.xOffset, 7);
  EXPECT_EQ(geometry.width, 54);
  line.clipWidth = 40;
  EXPECT_EQ(pageWordGeometry(renderer, 1, line, block, 0).width, 33);
}

TEST(EpubTextGrayscaleTest, AllSmallCapsUsesC2scAndUnicodeFallbackWithoutChangingText) {
  fakeheap::reset(true); Storage.reset(); RasterFont fixture(12);
  fixture.glyphs['A'-32].advanceX = 9*16;
  fixture.glyphs['Z'-32].advanceX = 7*16;
  fixture.data.smallCapsHandler = [](void*, uint32_t cp, bool all) -> uint32_t {
    return cp == 'a' ? 'A' : all && cp == 'B' ? 'Z' : 0;
  };
  HalDisplay display(800,480); GfxRenderer renderer(display); renderer.begin();
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  renderer.insertFont(1,EpdFontFamily(&fixture.font));
  EXPECT_EQ(renderer.getTextAdvanceX(1,"aB",EpdFontFamily::SMALL_CAPS),21);
  EXPECT_EQ(renderer.getTextAdvanceX(1,"aB",EpdFontFamily::REGULAR,0,0,0,fontTypography::AllSmallCaps),16);
  EXPECT_EQ(renderer.getTextAdvanceX(1,"CCC",EpdFontFamily::REGULAR,0,0,0,fontTypography::AllSmallCaps),27);
  renderer.clearScreen(); renderer.drawText(1,20,30,"aB",true,EpdFontFamily::REGULAR,BidiUtils::BidiBaseDir::AUTO,0,0,fontTypography::AllSmallCaps);
  const auto actual=display.bw;
  renderer.clearScreen(); renderer.drawText(1,20,30,"AZ"); EXPECT_EQ(display.bw,actual);
}
TEST(EpubTextGrayscaleTest, ExplicitNoLigaturesMatchesSeparateGlyphPixels) {
  fakeheap::reset(true); Storage.reset(); RasterFont fixture(12);
  const EpdLigaturePair pairs[] = {{(uint32_t('f')<<16)|'f',0xFB00},{(0xFB00u<<16)|'i',0xFB03}};
  fixture.data.ligaturePairs=pairs; fixture.data.ligaturePairCount=2;
  HalDisplay display(800,480); GfxRenderer renderer(display); renderer.begin();
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise); renderer.insertFont(1,EpdFontFamily(&fixture.font));
  EXPECT_EQ(renderer.getTextAdvanceX(1,"ffi",EpdFontFamily::REGULAR),12);
  EXPECT_EQ(renderer.getTextAdvanceX(1,"ffi",EpdFontFamily::REGULAR,0,0,0,fontTypography::NoLigatures),36);
  renderer.clearScreen(); renderer.drawText(1,20,30,"ffi",true,EpdFontFamily::REGULAR,BidiUtils::BidiBaseDir::AUTO,0,0,fontTypography::NoLigatures);
  const auto actual=display.bw;
  renderer.clearScreen(); renderer.drawText(1,20,30,"f"); renderer.drawText(1,32,30,"f"); renderer.drawText(1,44,30,"i");
  EXPECT_EQ(display.bw,actual);
}
TEST(EpubTextGrayscaleTest, PublisherResolverPreservesFractionalPointsAndGenericOrder) {
  fakeheap::reset(true); Storage.reset(); RasterFont fixture(12);
  fixture.data.pointSize=12; fixture.data.pointSize64=768; fixture.data.sizeFamily=&fixture;
  HalDisplay display(800,480); GfxRenderer renderer(display); renderer.begin(); renderer.insertFont(1,EpdFontFamily(&fixture.font));
  struct Calls {int reader=0; uint16_t points=0; unsigned generic=0; bool sans=false;} calls;
  renderer.setBookFontResolver(&calls,[](void* context,uint32_t family,uint16_t points) {
    static_cast<Calls*>(context)->points=points;
    return family==10 ? genericFont::resolverResult(genericFont::SansSerif) : family==11 ? 99 : 0;
  });
  renderer.setGenericFontResolver(&calls,[](void* context,int reader,bool sans,uint16_t points) {
    auto& c=*static_cast<Calls*>(context);c.reader=reader;c.sans=sans;c.points=points;++c.generic;return 42;
  });
  EXPECT_EQ(renderer.resolveBookFont64(1,10,717),42); EXPECT_EQ(calls.reader,1); EXPECT_TRUE(calls.sans); EXPECT_EQ(calls.points,717);
  EXPECT_EQ(renderer.resolveBookFont64(1,11,819),99); EXPECT_EQ(calls.generic,1u);
  EXPECT_EQ(renderer.resolveBookFont64(1,12,768),1); EXPECT_EQ(calls.generic,1u);
}
TEST(EpubTextGrayscaleTest, ExactTypographySurvivesNativeCacheAndRejectsCorruption) {
  fakeheap::reset(true); Storage.reset(); BlockStyle style; style.fontSize64=717; style.lineHeightMultiplier=0.8f;
  TextBlock block({"aB","ffi"},{0,30},{EpdFontFamily::REGULAR,EpdFontFamily::REGULAR},{},{},{},{},{},style,{}, {},"",0,{}, {},{717,819},{fontTypography::AllSmallCaps | fontTypography::ScriptSizeResolved,fontTypography::NoLigatures});
  ASSERT_TRUE(block.valid()); HalFile file; ASSERT_TRUE(Storage.openFileForWrite("TEST","typography",file)); ASSERT_TRUE(block.serialize(file));
  ASSERT_TRUE(file.seek(0)); auto copy=TextBlock::deserialize(file); ASSERT_TRUE(copy);
  EXPECT_FLOAT_EQ(copy->getBlockStyle().lineHeightMultiplier, 0.8f);
  file.close();
  for (const float invalidHeight : {-1.0f, 0.5f, 2.1f, std::numeric_limits<float>::infinity(),
                                   std::numeric_limits<float>::quiet_NaN()}) {
    auto invalid = Storage.bytes("typography");
    std::memcpy(invalid.data() + invalid.size() - sizeof(float), &invalidHeight, sizeof(float));
    Storage.put("invalid-line-height", invalid);
    auto input = Storage.open("invalid-line-height", O_RDONLY);
    EXPECT_FALSE(TextBlock::deserialize(input));
    input.close();
  }
  EXPECT_EQ(copy->wordFontSize64(0),717); EXPECT_EQ(copy->wordFontSize64(1),819); EXPECT_EQ(copy->wordTypography(0),fontTypography::AllSmallCaps | fontTypography::ScriptSizeResolved);
  EXPECT_EQ(copy->wordTypography(1),fontTypography::NoLigatures); EXPECT_EQ(copy->getBlockStyle().fontSize64,717);
  auto invalidFlags=Storage.bytes("typography");
  // This unique metadata sequence locates the exact-size/style payload without
  // depending on source-anchor or other independent cache header fields.
  const std::vector<uint8_t> marker{0xcd,0x02,0x33,0x03,5,2};
  const auto at=std::search(invalidFlags.begin(),invalidFlags.end(),marker.begin(),marker.end()); ASSERT_NE(at,invalidFlags.end());
  *(at+4)=8;Storage.put("bad-typography",invalidFlags);auto corrupt=Storage.open("bad-typography",O_RDONLY);EXPECT_FALSE(TextBlock::deserialize(corrupt));
  auto bytes=Storage.bytes("typography"); bytes.pop_back(); Storage.put("truncated",bytes); auto input=Storage.open("truncated",O_RDONLY); EXPECT_FALSE(TextBlock::deserialize(input));
  TextBlock bad({"bad"},{0},{EpdFontFamily::REGULAR},{},{},{},{},{},style,{}, {},"",0,{}, {},{8193},{0}); EXPECT_FALSE(bad.valid());
}

TEST(EpubTextGrayscaleTest, IntegerSizeLookupCannotSelectNearbyFractionalFace) {
  fakeheap::reset(true); Storage.reset(); RasterFont base(12), fractional(12), integer(12);
  base.data.sizeFamily = fractional.data.sizeFamily = integer.data.sizeFamily = &base;
  base.data.pointSize = 14; base.data.pointSize64 = 896;
  fractional.data.pointSize = integer.data.pointSize = 12;
  fractional.data.pointSize64 = 784; integer.data.pointSize64 = 768;
  HalDisplay display(800,480); GfxRenderer renderer(display); renderer.begin();
  renderer.insertFont(1,EpdFontFamily(&fractional.font)); renderer.insertFont(2,EpdFontFamily(&integer.font));
  renderer.insertFont(3,EpdFontFamily(&base.font));
  EXPECT_EQ(renderer.getFontIdForSize(3,12),2); EXPECT_EQ(renderer.getFontIdForSize(1,12),2);
}
TEST(EpubTextGrayscaleTest, ReducedBitmapUsesAreaCoverageAndFractionalBearings) {
  fakeheap::reset(true); Storage.reset(); RasterFont source(13);
  HalDisplay display(800,480); GfxRenderer renderer(display); renderer.begin();
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  renderer.insertFont(1,EpdFontFamily(&source.font).withSize(100,12));
  const auto original=source.glyphs['A'-32];
  for (const int numerator : {2,3}) {
    const double scale=numerator/4.0;
    EpdGlyph expected=original;
    expected.left=static_cast<int>(std::floor(original.left*scale));
    expected.top=-static_cast<int>(std::floor(-original.top*scale));
    expected.width=static_cast<int>(std::ceil((original.left+original.width)*scale))-expected.left;
    expected.height=static_cast<int>(std::ceil((original.height-original.top)*scale))+expected.top;
    expected.dataOffset=0; expected.dataLength=(expected.width*expected.height+3)/4;
    std::vector<uint8_t> pixels(expected.dataLength,0);
    // Independent exact overlap oracle (the production integrator uses fixed point).
    for (unsigned y=0;y<expected.height;++y) for(unsigned x=0;x<expected.width;++x) {
      const double x0=(expected.left+int(x))/scale-original.left, x1=x0+1/scale;
      const double y0=(-expected.top+int(y))/scale+original.top, y1=y0+1/scale;
      double area=0;
      for(unsigned sy=0;sy<original.height;++sy) for(unsigned sx=0;sx<original.width;++sx) {
        const double overlap=std::max(0.0,std::min(x1,double(sx+1))-std::max(x0,double(sx)))*
                             std::max(0.0,std::min(y1,double(sy+1))-std::max(y0,double(sy)));
        const unsigned i=sy*original.width+sx;
        area+=overlap*((source.pixels[original.dataOffset+i/4]>>(6-2*(i%4)))&3);
      }
      const auto raw=static_cast<unsigned>(std::nearbyint(area*scale*scale));
      const unsigned i=y*expected.width+x; pixels[i/4]|=raw<<(6-2*(i%4));
    }
    EpdUnicodeInterval interval{'A','A',0}; EpdFontData data{};
    data.bitmap=pixels.data(); data.glyph=&expected; data.intervals=&interval; data.intervalCount=1;
    data.is2Bit=true; data.ascender=source.data.ascender; data.descender=source.data.descender; data.advanceY=source.data.advanceY;
    EpdFont expectedFont(&data); renderer.insertFont(2,EpdFontFamily(&expectedFont));
    for (auto mode : {GfxRenderer::BW,GfxRenderer::GRAYSCALE_LSB,GfxRenderer::GRAYSCALE_MSB}) {
      renderer.setRenderMode(mode); renderer.clearScreen();
      renderer.drawText(1,40,60,numerator==2?"A":"a",true,numerator==2?EpdFontFamily::SUP:EpdFontFamily::SMALL_CAPS);
      const auto actual=display.bw;
      renderer.clearScreen(); renderer.drawText(2,40,60,"A"); EXPECT_EQ(display.bw,actual) << numerator << " " << mode;
      const int sized=renderer.getFontIdForSize64(1,12*64*numerator/4);
      ASSERT_NE(sized,1); EXPECT_EQ(renderer.getFontPointSize64(sized),12*64*numerator/4);
      renderer.clearScreen(); renderer.drawText(sized,40,60+source.data.ascender-renderer.getFontAscenderSize(sized),"A");
      EXPECT_EQ(display.bw,actual) << "bitmap size adapter " << numerator << " " << mode;
    }
    renderer.removeFont(2);
  }
}

TEST(EpubTextGrayscaleTest, BitmapSizesAreBoundedInvalidateBeforeSourceRemovalAndRetryAllocation) {
  fakeheap::reset(true); Storage.reset(); RasterFont source(12);
  HalDisplay display(800,480); GfxRenderer renderer(display); renderer.begin();
  renderer.insertFont(1,EpdFontFamily(&source.font).withSize(100,12));
  fakeheap::external.fail=1;
  EXPECT_EQ(renderer.getFontIdForSize64(1,717),1); EXPECT_TRUE(renderer.fontsNeedRetry());
  const auto attempts=fakeheap::external.attempts;
  EXPECT_EQ(renderer.getFontIdForSize64(1,717),1); EXPECT_EQ(fakeheap::external.attempts,attempts);
  renderer.retryUnavailableFontSizes();
  const int fractional=renderer.getFontIdForSize64(1,717); ASSERT_NE(fractional,1);
  EXPECT_EQ(renderer.getFontPointSize64(fractional),717); EXPECT_FALSE(renderer.fontsNeedRetry());
  int previous=fractional;
  for(unsigned points=700;points<900;++points) {
    const int id=renderer.getFontIdForSize64(1,points); ASSERT_TRUE(renderer.getFontMap().count(id));
    EXPECT_EQ(renderer.getFontPointSize64(id),points); EXPECT_TRUE(renderer.getFontMap().count(previous));
    EXPECT_LE(renderer.getFontMap().size(),33u); previous=id;
  }
  renderer.removeFont(1); EXPECT_TRUE(renderer.getFontMap().empty());
  renderer.insertFont(1,EpdFontFamily(&source.font).withSize(100,12));
  const int fresh=renderer.getFontIdForSize64(1,717); ASSERT_NE(fresh,1);
  EXPECT_EQ(renderer.getTextAdvanceX(fresh,"AAAA",EpdFontFamily::REGULAR),45);
  renderer.releaseBitmapFontSizes(); EXPECT_EQ(renderer.getFontMap().size(),1u);
}
TEST(EpubTextGrayscaleTest, BitmapSizesRetainSdPrewarmAndMatchResidentCoverage) {
  fakeheap::reset(true); Storage.reset(); RasterFont fixture(12); SdCardFont sd;
  Storage.put("scaled.cpfont",fixture.file()); ASSERT_TRUE(sd.load("scaled.cpfont"));
  HalDisplay display(800,480); GfxRenderer renderer(display); renderer.begin();
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  renderer.insertFont(1,EpdFontFamily(sd.getEpdFont()).withSize(1,12)); renderer.registerSdCardFont(1,&sd);
  renderer.insertFont(2,EpdFontFamily(&fixture.font).withSize(2,12));
  FontCacheManager cache(renderer.getFontMap(),renderer.getSdCardFonts()); renderer.setFontCacheManager(&cache);
  const int scaled=renderer.getFontIdForSize64(1,1001),reference=renderer.getFontIdForSize64(2,1001);
  ASSERT_NE(scaled,1); ASSERT_NE(reference,2);
  {auto scan=cache.createPrewarmScope();renderer.drawText(scaled,20,40,"abcdef");ASSERT_TRUE(scan.endScanAndPrewarm());}
  for(auto mode:{GfxRenderer::BW,GfxRenderer::GRAYSCALE_LSB,GfxRenderer::GRAYSCALE_MSB}) {
    renderer.setRenderMode(mode);renderer.clearScreen();renderer.drawText(scaled,20,40,"abcdef");const auto actual=display.bw;
    renderer.clearScreen();renderer.drawText(reference,20,40,"abcdef");EXPECT_EQ(display.bw,actual);
  }
  renderer.removeFont(1); EXPECT_FALSE(renderer.getFontMap().count(scaled)); EXPECT_TRUE(renderer.getFontMap().count(reference));
}

TEST(EpubTextGrayscaleTest, ResolvedScriptSizeDoesNotShrinkAgain) {
  fakeheap::reset(true); Storage.reset(); RasterFont source(12);
  HalDisplay display(800,480); GfxRenderer renderer(display); renderer.begin();
  renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise); renderer.insertFont(1,EpdFontFamily(&source.font));
  for(auto style:{EpdFontFamily::SUP,EpdFontFamily::SUB}) {
    EXPECT_EQ(renderer.getTextAdvanceX(1,"ABC",style,0,0,0,fontTypography::ScriptSizeResolved),36);
    EXPECT_EQ(renderer.getTextAdvanceX(1,"ABC",style),18);
    renderer.clearScreen();renderer.drawText(1,20,40,"ABC",true,style,BidiUtils::BidiBaseDir::AUTO,0,0,fontTypography::ScriptSizeResolved);
    const auto actual=display.bw;renderer.clearScreen();renderer.drawText(1,20,40,"ABC");EXPECT_EQ(actual,display.bw);
  }
}
TEST(EpubTextGrayscaleTest, HdBitmapScalingUsesOriginalSourceCoverageOnce) {
  fakeheap::reset(true); Storage.reset(); RasterFont source(13); SdCardFont hd;
  auto bytes=source.file(); bytes[8]=5;bytes[13]=2;
  const auto crc=cpfont::crc32Update(0xffffffff,bytes.data()+32,bytes.size()-32)^0xffffffff;
  for(unsigned i=0;i<4;++i)bytes[14+i]=crc>>(i*8);
  Storage.put("hd.cpfont",bytes);ASSERT_TRUE(hd.load("hd.cpfont"));ASSERT_EQ(hd.rasterDensity(),2);
  HalDisplay display(800,480);GfxRenderer renderer(display);renderer.begin();renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  renderer.insertFont(1,EpdFontFamily(hd.getEpdFont()).withSize(1,12));renderer.registerSdCardFont(1,&hd);
  renderer.insertFont(2,EpdFontFamily(&source.font).withSize(2,24));
  for(uint16_t points:{717,1152,1491}) {
    const int actualId=renderer.getFontIdForSize64(1,points),referenceId=renderer.getFontIdForSize64(2,points);
    ASSERT_NE(actualId,1);ASSERT_NE(referenceId,2);
    for(auto mode:{GfxRenderer::BW,GfxRenderer::GRAYSCALE_LSB,GfxRenderer::GRAYSCALE_MSB}) {
      renderer.setRenderMode(mode);renderer.clearScreen();renderer.drawText(actualId,30,60,"ABCDE");const auto actual=display.bw;
      renderer.clearScreen();renderer.drawText(referenceId,30,60+renderer.getFontAscenderSize(actualId)-renderer.getFontAscenderSize(referenceId),"ABCDE");
      EXPECT_EQ(actual,display.bw)<<points<<" "<<mode;
    }
  }
}

TEST(EpubTextGrayscaleTest, TextBlockKeepsStableReaderIdentityAcrossManyExactSizes) {
  fakeheap::reset(true);Storage.reset();RasterFont source(12);
  HalDisplay display(800,480);GfxRenderer renderer(display);renderer.begin();renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  renderer.insertFont(1,EpdFontFamily(&source.font).withSize(1,12));
  std::vector<std::string> words(64,"A");std::vector<int16_t> positions;std::vector<uint16_t> sizes;
  for(unsigned i=0;i<64;++i){positions.push_back(i*8);sizes.push_back(700+i);}
  BlockStyle style;style.fontSize64=717;
  TextBlock block(words,positions,std::vector<EpdFontFamily::Style>(64,EpdFontFamily::REGULAR),{},{},{},{},{},style,{}, {},"",0,{}, {},sizes,{});
  ASSERT_TRUE(block.valid());renderer.clearScreen();block.render(renderer,1,10,40);const auto actual=display.bw;
  renderer.releaseBitmapFontSizes();renderer.clearScreen();int baseline=0;
  for(auto points:sizes)baseline=std::max(baseline,renderer.getFontAscenderSize(renderer.getFontIdForSize64(1,points)));
  for(unsigned i=0;i<64;++i){const int id=renderer.getFontIdForSize64(1,sizes[i]);renderer.drawText(id,10+positions[i],40+baseline-renderer.getFontAscenderSize(id),"A");}
  EXPECT_EQ(actual,display.bw);
}

TEST(EpubTextGrayscaleTest, BitmapExtremeBearingsAndTinySizesStayBounded) {
  fakeheap::reset(true);
  Storage.reset();
  RasterFont source(13);
  HalDisplay display(800, 480);
  GfxRenderer renderer(display);
  renderer.begin();
  for (const int bearing : {-32768, -32767, -1, 0, 32767}) {
    auto& raw = source.glyphs['A' - 32];
    raw.left = bearing;
    raw.top = bearing;
    renderer.insertFont(1, EpdFontFamily(&source.font).withSize(100, 255));
    for (const uint16_t points : {1, 2, 65, 717, 8192}) {
      const int id = renderer.getFontIdForSize64(1, points);
      const auto* data = renderer.getFontMap().at(id).getData();
      ASSERT_NE(data->scaledRasterHandler, nullptr);
      for (const uint8_t extra : {1, 32, 48, 64}) {
        EpdGlyph glyph{};
        const uint8_t* pixels = nullptr;
        ASSERT_TRUE(data->scaledRasterHandler(data->glyphMissCtx, 'A', extra, -8, glyph, pixels));
        if (points <= 2 && pixels)
          EXPECT_TRUE(std::all_of(pixels, pixels + glyph.dataLength, [](uint8_t b) { return b == 0; }));
      }
    }
    renderer.removeFont(1);
  }
}

namespace {
std::unique_ptr<Page> tableImagePage() {
  auto page = std::make_unique<Page>();
  TableFragmentCell cell;
  cell.lines.push_back(std::make_shared<TextBlock>(std::vector<std::string>{"A"}, std::vector<int16_t>{0},
      std::vector<EpdFontFamily::Style>{EpdFontFamily::REGULAR}, std::vector<uint8_t>{}, std::vector<uint16_t>{},
      std::vector<uint16_t>{}, std::vector<uint8_t>{}, std::vector<bool>{}));
  cell.image = std::make_shared<ImageBlock>("table-image.img", "source", 16, 16);
  cell.imageSource = 123;
  TableFragmentRow row; row.height = 30; row.cells.push_back(std::move(cell)); row.cells.push_back({});
  std::vector<TableFragmentRow> rows; rows.push_back(std::move(row));
  page->elements.push_back(std::make_unique<PageTableFragment>(100, 2, 2, 10, std::move(rows), 7, 11));
  page->noteSourcePoint(123);
  return page;
}
void putTableImageCache() {
  std::vector<uint8_t> pixels(4 + 16 * 4, 0x1B);
  pixels[0] = pixels[2] = 16; pixels[1] = pixels[3] = 0;
  Storage.put("table-image.pxc", std::move(pixels));
}
}

TEST(EpubTextGrayscaleTest, TableImagePixelsUseNativeImagePassAndStripCoordinates) {
  ImageBlock::clearSessionRenderFailures(); fakeheap::reset(true); Storage.reset(); putTableImageCache();
  auto page = tableImagePage();
  ASSERT_TRUE(page->hasImages()); EXPECT_FALSE(page->hasImagesNeedingDecode());
  int16_t x, y, width, height;
  ASSERT_TRUE(page->getImageBoundingBox(x, y, width, height));
  EXPECT_EQ(x, 24); EXPECT_EQ(y, 23); EXPECT_EQ(width, 16); EXPECT_EQ(height, 16);
  EXPECT_GT(page->imageEstimateUnits(100, 100), 0);
  HalDisplay display(120, 80); GfxRenderer renderer(display); renderer.begin();
  ImageBlock reference("table-image.img", "source", 16, 16);
  for (int orientation = 0; orientation < 4; ++orientation) {
    renderer.setOrientation(GfxRenderer::Orientation(orientation));
    for (auto mode : {GfxRenderer::BW, GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
      renderer.setRenderMode(mode);
      for (auto offset : {std::pair{0, 0}, {-30, -27}, {84, 47}}) {
        std::vector<uint8_t> expected(display.bw.size(), mode == GfxRenderer::BW ? 255 : 0);
        auto actual = expected;
        renderer.beginStripTarget(expected.data(), 0, display.height);
        reference.render(renderer, 24 + offset.first, 23 + offset.second, true);
        renderer.endStripTarget();
        for (int row = 0; row < display.height; row += 7) {
          renderer.beginStripTarget(actual.data() + row * display.stride, row, std::min(7, display.height - row));
          page->renderImages(renderer, 0, offset.first, offset.second);
          renderer.endStripTarget();
        }
        EXPECT_EQ(actual, expected);
      }
    }
  }
  renderer.setRenderMode(GfxRenderer::BW);
  bool changed = true;
  EXPECT_EQ(page->warmImageCaches(renderer, 0, 0, nullptr, nullptr, &changed), ImageBlock::CacheWarmResult::Ready);
  EXPECT_FALSE(changed);
  EXPECT_EQ(page->warmImageCaches(renderer, 0, 0, [](void*) { return true; }, nullptr), ImageBlock::CacheWarmResult::Cancelled);
  ImageBlock::clearSessionRenderFailures();
}

TEST(EpubTextGrayscaleTest, TableImageCacheRoundTripPreservesPresenceBoundsAndRawOrigin) {
  ImageBlock::clearSessionRenderFailures(); fakeheap::reset(true); Storage.reset();
  auto page = tableImagePage();
  HalFile file; ASSERT_TRUE(Storage.openFileForWrite("TEST", "table-page", file));
  ASSERT_TRUE(page->serialize(file)); ASSERT_TRUE(file.seek(0));
  auto restored = Page::deserialize(file); ASSERT_TRUE(restored); file.close();
  EXPECT_TRUE(restored->hasImages()); EXPECT_TRUE(restored->hasImagesNeedingDecode());
  int16_t x, y, w, h; ASSERT_TRUE(restored->getImageBoundingBox(x,y,w,h));
  EXPECT_EQ(x,24); EXPECT_EQ(y,23); EXPECT_EQ(w,16); EXPECT_EQ(h,16);
  const auto complete = Storage.bytes("table-page");
  for (size_t count = 0; count < complete.size(); ++count) {
    Storage.put("truncated-table", {complete.begin(), complete.begin()+count});
    auto input = Storage.open("truncated-table", O_RDONLY);
    EXPECT_FALSE(Page::deserialize(input)) << count;
  }
  TableFragmentCell cell; cell.image = std::make_shared<ImageBlock>("large", "", 100, 100); cell.imageSource=87;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", "table-cell", file)); ASSERT_TRUE(cell.serialize(file));
  ASSERT_TRUE(file.seek(0)); TableFragmentCell restoredCell; ASSERT_TRUE(TableFragmentCell::deserialize(file, restoredCell));
  EXPECT_EQ(restoredCell.imageSource, 87u); file.close();
  TableFragmentRow row; row.height=30; row.cells.push_back(std::move(cell));
  std::vector<TableFragmentRow> rows; rows.push_back(std::move(row));
  PageTableFragment invalid(50,1,2,10,std::move(rows),0,0);
  ASSERT_TRUE(Storage.openFileForWrite("TEST","invalid-table",file)); ASSERT_TRUE(invalid.serialize(file));
  ASSERT_TRUE(file.seek(0)); EXPECT_FALSE(PageTableFragment::deserialize(file)); file.close();
}

TEST(EpubTextGrayscaleTest, InvalidTableImageCannotHideLaterImagesOrMarkWarmPageReady) {
  ImageBlock::clearSessionRenderFailures(); fakeheap::reset(true); Storage.reset(); putTableImageCache();
  TableFragmentRow row; row.height = 30;
  TableFragmentCell invalid; invalid.image = std::make_shared<ImageBlock>("invalid", "", 200, 200);
  TableFragmentCell valid; valid.image = std::make_shared<ImageBlock>("table-image.img", "", 16, 16);
  row.cells.push_back(std::move(invalid)); row.cells.push_back(std::move(valid));
  std::vector<TableFragmentRow> rows; rows.push_back(std::move(row));
  Page page; page.elements.push_back(std::make_unique<PageTableFragment>(100,2,2,10,std::move(rows),0,0));
  int count = 0; bool invalidGeometry = false;
  EXPECT_TRUE(page.forEachImage([](const PageImagePlacement& image,void* context) {
    ++*static_cast<int*>(context); EXPECT_EQ(image.block->getWidth(),16); EXPECT_GE(image.x,52); return true;
  }, &count, &invalidGeometry));
  EXPECT_TRUE(invalidGeometry); EXPECT_EQ(count,1); EXPECT_TRUE(page.hasImages());
  HalDisplay display(120,80); GfxRenderer renderer(display); renderer.begin();
  EXPECT_EQ(page.warmImageCaches(renderer,0,0,nullptr,nullptr),ImageBlock::CacheWarmResult::Unavailable);
  ImageBlock::clearSessionRenderFailures();
}
