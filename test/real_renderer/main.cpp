#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <TextRun.h>
#include <builtinFonts/bookerly_14_bold.h>
#include <builtinFonts/bookerly_14_bolditalic.h>
#include <builtinFonts/bookerly_14_italic.h>
#include <builtinFonts/bookerly_14_regular.h>
#include <builtinFonts/notosans_14_bold.h>
#include <builtinFonts/notosans_14_bolditalic.h>
#include <builtinFonts/notosans_14_italic.h>
#include <builtinFonts/notosans_14_regular.h>

#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
int runPreparedGrayscaleChecks(GfxRenderer&, HalDisplay&);
int runDirectGrayscaleChecks();
int runDownscaleChecks(GfxRenderer&, HalDisplay&, const EpdFontFamily&, const EpdFontFamily&, const char*);
#ifdef WITCH_TEST_OUTLINE
int runOutlineChecks(GfxRenderer&, HalDisplay&, const char*);
#endif
int main(int argc, char** argv) {
  HalDisplay d;
  GfxRenderer g(d);
  g.begin();
  EpdFont book(&bookerly_14_regular), sans(&notosans_14_regular);
  EpdFont bookBold(&bookerly_14_bold), bookItalic(&bookerly_14_italic), bookBoldItalic(&bookerly_14_bolditalic);
  EpdFont sansBold(&notosans_14_bold), sansItalic(&notosans_14_italic), sansBoldItalic(&notosans_14_bolditalic);
  const EpdFontFamily bookFamily(&book, &bookBold, &bookItalic, &bookBoldItalic);
  const EpdFontFamily sansFamily(&sans, &sansBold, &sansItalic, &sansBoldItalic);
  g.insertFont(1, bookFamily);
  g.insertFont(2, sansFamily);
  std::map<int, EpdFontFamily> fm;
  std::map<int, SdCardFont*> sd;
  FontDecompressor decomp;
  decomp.init();
  FontCacheManager cache(fm, sd, sd);
  cache.setFontDecompressor(&decomp);
  g.setFontCacheManager(&cache);
  g.registerReaderFontPair(1, 2);
  const char* text = "Sample: AV office fi 123";
  int row = 20;
  g.drawTextScaled(2, 20, row, "Production glyph rendering", true, EpdFontFamily::REGULAR, 0.5f);
  row += 36;
  for (int id = 1; id <= 2; ++id) {
    for (float scale : {1.0f, 0.75f}) {
      const std::string label =
          (id == 1 ? "Bookerly 14" : "Noto Sans 14") + std::string(" / scale ") + std::to_string(scale).substr(0, 4);
      g.drawTextScaled(2, 20, row, label.c_str(), true, EpdFontFamily::REGULAR, 0.5f);
      row += 20;
      g.drawTextScaled(id, 20, row, text, true, EpdFontFamily::REGULAR, scale);
      row += 42;
    }
  }
  for (int id = 1; id <= 2; ++id)
    for (int tracking : {0, 8, 16, -8}) {
      const auto font = g.resolveTextFont(id, 2);
      assert(font.fontId == 2 && font.scale == 1.0f);
      assert(g.resolveTextFont(id, 1).fontId == 1);
      assert(g.resolveTextFont(id, 0).fontId == id);
      const std::string label =
          (id == 1 ? "Bookerly" : "Noto Sans") + std::string(" 0.75 / tracking ") + std::to_string(tracking) + "/16px";
      g.drawTextScaled(2, 20, row, label.c_str(), true, EpdFontFamily::REGULAR, 0.5f);
      row += 20;
      g.drawTextSpaced(id, 20, row, text, true, EpdFontFamily::REGULAR, 0.75f, tracking);
      row += 30;
    }
  const auto sampleFrame = d.fb;
  int failures = 0, checks = 0;
  for (int id = 1; id <= 2; ++id)
    for (float scale : {0.75f, 1.0f, 1.2f})
      for (int tracking : {0, 8, 16, -8})
        for (auto style : {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, EpdFontFamily::ITALIC,
                           EpdFontFamily::BOLD_ITALIC, EpdFontFamily::SMALL_CAPS,
                           static_cast<EpdFontFamily::Style>(EpdFontFamily::BOLD | EpdFontFamily::SMALL_CAPS),
                           static_cast<EpdFontFamily::Style>(EpdFontFamily::ITALIC | EpdFontFamily::SMALL_CAPS),
                           static_cast<EpdFontFamily::Style>(EpdFontFamily::BOLD_ITALIC | EpdFontFamily::SMALL_CAPS)})
          for (const char* sample : {"AV office fi", "e\xcc\x81 cafe", "abc 123", "Sample", "jfj fffi"}) {
            d.fb.fill(255);
            const auto& family = id == 1 ? bookFamily : sansFamily;
            const auto m =
                textRun::walk(family, sample, style, scale, tracking, [](uint32_t, int, int, float, bool) {});
            g.drawTextSpaced(id, 30, 100, sample, true, style, scale, tracking);
            int minx = 480, maxx = -1;
            for (int yy = 0; yy < 800; ++yy)
              for (int xx = 0; xx < 480; ++xx)
                if (!(d.fb[(479 - xx) * 100 + yy / 8] & (0x80 >> (yy % 8)))) {
                  minx = std::min(minx, xx);
                  maxx = std::max(maxx, xx);
                }
            // Bitmap rectangles can have empty edge columns; ink must stay
            // within promised extents.
            const int adv = g.getTextAdvanceXSpaced(id, sample, style, scale, tracking);
            bool ok = maxx >= minx && minx >= 30 + m.left - 1 && minx <= 30 + m.left + 2 && maxx < 30 + m.right + 1 &&
                      maxx >= 30 + m.right - 3 && adv == m.advance &&
                      g.getTextWidthSpaced(id, sample, style, scale, tracking) >= maxx - minx + 1;
            if (!ok) {
              ++failures;
              std::cout << "FAIL id=" << id << " scale=" << scale << " track=" << tracking
                        << " style=" << static_cast<int>(style) << " sample=" << sample << " ink=" << minx << ","
                        << maxx << " promised=" << m.left << "," << m.right << "\n";
            }
            ++checks;
          }
  // Independently calculated fixed-point advance for four identical
  // non-ligating H glyphs. This oracle does not call textRun::walk and catches
  // ignored tracking/scale/kerning.
  for (int id = 1; id <= 2; ++id)
    for (float scale : {0.75f, 1.0f, 1.2f})
      for (int tracking : {0, 8, 16, -8}) {
        const EpdFont& font = id == 1 ? book : sans;
        const auto h = font.getGlyph('H');
        const int expected = (4 * static_cast<int>(std::lround(h.advanceX * scale)) +
                              3 * (tracking + static_cast<int>(std::lround(font.getKerning('H', 'H') * scale))) + 8) >>
                             4;
        assert(g.getTextAdvanceXSpaced(id, "HHHH", EpdFontFamily::REGULAR, scale, tracking) == expected);
        assert(g.getTextWidthSpaced(id, "", EpdFontFamily::REGULAR, scale, tracking) == 0);
        d.fb.fill(255);
        g.drawTextSpaced(id, 30, 100, " ", true, EpdFontFamily::REGULAR, scale, tracking);
        for (auto byte : d.fb) assert(byte == 255);  // Empty-ink glyph must not add visible marks.
        assert(g.getTextAdvanceXSpaced(id, " ", EpdFontFamily::REGULAR, scale, tracking) > 0);
      }
  std::cout << "independent HHHH advance and empty-ink checks=24 passed\n";
  std::cout << "real glyph metric/ink checks=" << checks << " failures=" << failures << "\n";
  assert(failures == 0);
  d.fb = sampleFrame;
  std::ofstream out("frame.pgm", std::ios::binary);
  out << "P5\n480 800\n255\n";
  for (int y = 0; y < 800; ++y)
    for (int x = 0; x < 480; ++x) {
      uint8_t v = d.fb[(479 - x) * 100 + y / 8] & (0x80 >> (y % 8)) ? 255 : 0;
      out.write((char*)&v, 1);
    }
  int result = runPreparedGrayscaleChecks(g, d);
  result +=
      runDownscaleChecks(g, d, bookFamily, sansFamily, argc > 1 && std::string(argv[1]) != "-" ? argv[1] : nullptr);
#ifdef WITCH_TEST_OUTLINE
  if (argc > 2) result |= runOutlineChecks(g, d, argv[2]);
#endif
  result |= runDirectGrayscaleChecks();
  return result;
}
