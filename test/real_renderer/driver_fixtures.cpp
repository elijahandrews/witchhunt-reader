#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <TextRun.h>
#include <builtinFonts/bookerly_14_regular.h>
#include <builtinFonts/notosans_14_regular.h>

#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>

static int fixtureCount = 0;
static void saveDriverFixture(const std::array<uint8_t, 48000>& base, const std::array<uint8_t, 48000>& lsb,
                              const std::array<uint8_t, 48000>& msb) {
  const auto name = "driver-fixture-" + std::to_string(fixtureCount++) + ".bin";
  std::ofstream f(name, std::ios::binary);
  assert(f);
  for (const auto* plane : {&base, &lsb, &msb}) f.write(reinterpret_cast<const char*>(plane->data()), plane->size());
  assert(f);
}
int main() {
  HalDisplay d;
  GfxRenderer g(d);
  g.begin();
  g.setOrientation(GfxRenderer::LandscapeCounterClockwise);
  EpdFont book(&bookerly_14_regular);
  g.insertFont(1, EpdFontFamily(&book));
  std::map<int, EpdFontFamily> fm;
  std::map<int, SdCardFont*> sd;
  FontDecompressor decomp;
  decomp.init();
  FontCacheManager cache(fm, sd, sd);
  cache.setFontDecompressor(&decomp);
  g.setFontCacheManager(&cache);
  for (float scale : {0.75f, 1.0f, 1.2f})
    for (int darkness = 0; darkness <= 4; ++darkness) {
      g.setTextDarkness(darkness);
      g.setRenderMode(GfxRenderer::BW);
      d.fb.fill(255);
      g.drawTextSpaced(1, 30, 100, "AV office", true, EpdFontFamily::REGULAR, scale, 0);
      auto base = d.fb;
      g.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      d.fb.fill(0);
      g.drawTextSpaced(1, 30, 100, "AV office", true, EpdFontFamily::REGULAR, scale, 0);
      auto lsb = d.fb;
      g.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      d.fb.fill(0);
      g.drawTextSpaced(1, 30, 100, "AV office", true, EpdFontFamily::REGULAR, scale, 0);
      auto msb = d.fb;
      saveDriverFixture(base, lsb, msb);
      int oldNonwhite = 0, restoredWhite = 0, invalidMask = 0;
      for (size_t i = 0; i < base.size(); ++i) {
        auto p0 = base[i] | lsb[i];
        auto p1 = p0 ^ msb[i];
        auto restored = p0 & p1;
        oldNonwhite += __builtin_popcount(static_cast<uint8_t>(~base[i]));
        restoredWhite += __builtin_popcount(static_cast<uint8_t>(restored & ~base[i]));
        invalidMask += __builtin_popcount(static_cast<uint8_t>(lsb[i] & ~msb[i]));
      }
      std::array<uint8_t, 48000> capLsb{}, capMsb{};
      g.setRenderMode(GfxRenderer::BW);
      d.fb.fill(255);
      g.beginGrayCapture(capLsb.data(), capMsb.data());
      g.drawTextSpaced(1, 30, 100, "AV office", true, EpdFontFamily::REGULAR, scale, 0);
      g.endGrayCapture();
      saveDriverFixture(base, capLsb, capMsb);
      if (capLsb != lsb || capMsb != msb) {
        std::cout << "CAPTURE/STAGED mismatch\n";
        return 2;
      }
      g.invalidateScaledGlyphCache();
      g.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      d.fb.fill(0);
      g.drawTextSpaced(1, 30, 100, "AV office", true, EpdFontFamily::REGULAR, scale, 0);
      if (d.fb != lsb) {
        std::cout << "CACHE/COLD mismatch\n";
        return 3;
      }
      std::cout << "scale=" << scale << " darkness=" << darkness << " nonwhite=" << oldNonwhite
                << " baseline-black-to-white=" << restoredWhite << " forbidden-overlay-mask=" << invalidMask << "\n";
    }
  // Synthetic four-stripe glyph: each 4x4 block has constant raw coverage
  // 0,1,2,3.
  static const uint8_t bitmap[18] = {0, 85, 170, 255, 0, 85, 170, 255, 0, 85, 170, 255, 0, 85, 170, 255, 0, 0};
  static const EpdGlyph glyph = {16, 4, 256, 0, 4, 16, 0};
  static const EpdUnicodeInterval interval = {'X', 'X', 0};
  EpdFontData data{};
  data.bitmap = bitmap;
  data.glyph = &glyph;
  data.intervals = &interval;
  data.intervalCount = 1;
  data.advanceY = 4;
  data.ascender = 4;
  data.is2Bit = true;
  EpdFont fixture(&data);
  g.insertFont(3, EpdFontFamily(&fixture));
  int failures = 0, cases = 0;
  for (float scale : {0.75f, 1.0f, 1.2f})
    for (int darkness = 0; darkness <= 4; ++darkness) {
      g.setTextDarkness(darkness);
      g.setRenderMode(GfxRenderer::BW);
      d.fb.fill(255);
      g.drawTextSpaced(3, 30, 100, "X", true, EpdFontFamily::REGULAR, scale, 0);
      auto base = d.fb;
      g.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      d.fb.fill(0);
      g.drawTextSpaced(3, 30, 100, "X", true, EpdFontFamily::REGULAR, scale, 0);
      auto lsb = d.fb;
      g.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      d.fb.fill(0);
      g.drawTextSpaced(3, 30, 100, "X", true, EpdFontFamily::REGULAR, scale, 0);
      auto msb = d.fb;
      saveDriverFixture(base, lsb, msb);
      std::array<uint8_t, 48000> capLsb{}, capMsb{};
      g.setRenderMode(GfxRenderer::BW);
      d.fb.fill(255);
      g.beginGrayCapture(capLsb.data(), capMsb.data());
      g.drawTextSpaced(3, 30, 100, "X", true, EpdFontFamily::REGULAR, scale, 0);
      g.endGrayCapture();
      saveDriverFixture(base, capLsb, capMsb);
      assert(capLsb == lsb && capMsb == msb);  // Both real renderer paths must emit identical masks.
      for (int raw = 0; raw < 4; ++raw) {
        const int x = 30 + static_cast<int>((raw * 4 + 2) * scale);
        const int y = 100 + static_cast<int>(2 * scale);
        const int idx = y * 100 + x / 8, bit = 0x80 >> (x % 8);
        const bool bw = (base[idx] & bit) != 0, l = (lsb[idx] & bit) != 0, m = (msb[idx] & bit) != 0;
        // SDK documented overlay fold, as implemented in UC8279X4/Uc8179;
        // normalized before wire inversion.
        const bool p0 = bw || l, p1 = p0 != m, restored = p0 && p1;
        // Independent desired coverage-to-tone oracle. 0 white, 1 light, 2
        // dark, 3 black.
        int tone = raw;
        if (darkness == 1 && raw == 2) tone = 3;
        if ((darkness == 2 || darkness == 3) && raw > 0) tone = 3;
        if (darkness == 4 && (raw == 1 || raw == 2)) tone = 1;
        const bool want0 = tone == 0 || tone == 2, want1 = tone == 0 || tone == 1;
        const bool ok = p0 == want0 && p1 == want1 && restored == bw && !(l && !m);
        ++cases;
        if (!ok) {
          ++failures;
          std::cout << "CONTRACT FAIL scale=" << scale << " darkness=" << darkness << " raw=" << raw << " masks=" << l
                    << m << " folded=" << p0 << p1 << " wanted=" << want0 << want1 << " restored=" << restored
                    << " bw=" << bw << "\n";
        }
      }
    }
  std::cout << "four-coverage overlay-contract checks=" << cases << " failures=" << failures << "\n";
  std::cout << "real-renderer staged/captured fixtures=" << fixtureCount << " written\n";
  return failures ? 1 : 0;
}
