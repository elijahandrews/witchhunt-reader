#include <GfxRenderer.h>
#include <SmallCaps.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>

#include "CpFontFixture.h"

namespace {
using Plane = std::array<uint8_t, 48000>;
bool bit(const Plane& p, int x, int y) { return p[(479 - x) * 100 + y / 8] & (0x80 >> (y % 8)); }
void save(const char* path, const Plane& bw, const Plane& lsb, const Plane& msb) {
  std::ofstream out(path, std::ios::binary);
  out << "P5\n480 800\n255\n";
  for (int y = 0; y < 800; ++y)
    for (int x = 0; x < 480; ++x) {
      const bool l = bit(lsb, x, y), m = bit(msb, x, y);
      const uint8_t v = bit(bw, x, y) ? 255 : m ? (l ? 0 : 170) : (l ? 85 : 0);
      out.write(reinterpret_cast<const char*>(&v), 1);
    }
}
void sample(GfxRenderer& g, HalDisplay& d, int id, const char* path) {
  Plane lsb{}, msb{};
  g.setRenderMode(GfxRenderer::BW);
  g.setTextDarkness(0);
  d.fb.fill(255);
  g.beginGrayCapture(lsb.data(), msb.data());
  int y = 18;
  for (float scale : {1.0f, 0.75f, 0.5f}) {
    const std::string label = "Size " + std::to_string(scale).substr(0, 4);
    g.drawTextScaled(2, 16, y, label.c_str(), true, EpdFontFamily::REGULAR, 0.6f);
    y += 30;
    for (auto style :
         {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::BOLD_ITALIC}) {
      g.drawTextScaled(id, 16, y, "CAPITALS AND THIN STROKES", true, style, scale);
      y += 40;
    }
    g.drawTextScaled(id, 16, y, "Small capitals and dates", true, EpdFontFamily::SMALL_CAPS, scale);
    y += 65;
  }
  g.endGrayCapture();
  save(path, d.fb, lsb, msb);
}

void set(Plane& p, int x, int y, bool value) {
  if (x < 0 || x >= 480 || y < 0 || y >= 800) return;
  auto& byte = p[(479 - x) * 100 + y / 8];
  const uint8_t mask = 0x80 >> (y % 8);
  if (value)
    byte |= mask;
  else
    byte &= ~mask;
}

// Independent source-pixel projection oracle: project each source pixel's
// rectangle onto the destination and accumulate ink there. The production
// sampler instead walks destination pixels in fixed point. Match its documented
// 16.16 reciprocal precision, but not its traversal or threshold implementation.
std::vector<uint8_t> reference(const uint8_t* bitmap, bool twoBit, int sw, int sh, float scale) {
  const int w = std::lround(sw * scale), h = std::lround(sh * scale);
  std::vector<double> coverage(w * h);
  const double factor = 65536.0 / std::lround(65536.0 / scale);
  for (int sy = 0; sy < sh; ++sy)
    for (int sx = 0; sx < sw; ++sx) {
      const int i = sy * sw + sx;
      const int raw = twoBit ? (bitmap[i / 4] >> (6 - 2 * (i % 4))) & 3 : ((bitmap[i / 8] >> (7 - i % 8)) & 1) * 3;
      const double left = sx * factor, right = (sx + 1) * factor;
      const double top = sy * factor, bottom = (sy + 1) * factor;
      for (int y = std::max(0, int(top)); y < std::min(h, int(std::ceil(bottom))); ++y)
        for (int x = std::max(0, int(left)); x < std::min(w, int(std::ceil(right))); ++x)
          coverage[y * w + x] += raw * (std::min(right, x + 1.0) - std::max(left, double(x))) *
                                 (std::min(bottom, y + 1.0) - std::max(top, double(y)));
    }
  std::vector<uint8_t> result(w * h);
  for (size_t i = 0; i < result.size(); ++i) result[i] = std::clamp(int(std::floor(coverage[i] + 0.5)), 0, 3);
  return result;
}

int checks = 0, failures = 0;
void checkGlyph(GfxRenderer& g, HalDisplay& d, int id, const EpdFontFamily& family, const char* text,
                uint32_t codepoint, float scale, EpdFontFamily::Style style, bool spaced) {
  const bool folded = (style & EpdFontFamily::SMALL_CAPS) && smallCaps::fold(codepoint);
  const float glyphScale = scale * (folded ? smallCaps::SCALE : 1.0f);
  const auto glyph = family.getGlyph(codepoint, style);
  const auto data = family.getData(style);
  const auto raw = reference(g.getGlyphBitmap(data, glyph), data->is2Bit, glyph.width, glyph.height, glyphScale);
  const int w = std::lround(glyph.width * glyphScale), h = std::lround(glyph.height * glyphScale);
  const int x0 = 30 + int(glyph.left * glyphScale + .5f);
  const int y0 =
      100 + std::lround(family.getData(EpdFontFamily::REGULAR)->ascender * scale) - std::lround(glyph.top * glyphScale);
  auto draw = [&] {
    if (spaced)
      g.drawTextSpaced(id, 30, 100, text, true, style, scale, 8);
    else
      g.drawTextScaled(id, 30, 100, text, true, style, scale);
  };
  for (uint8_t darkness = 0; darkness <= 4; ++darkness) {
    Plane expectedBW, expectedL{}, expectedM{};
    expectedBW.fill(255);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const int r = raw[y * w + x];
        if (r) set(expectedBW, x0 + x, y0 + y, false);
        // Darkness mapping specified independently from drawMaskFor2BitMode.
        const bool aa = r == 1 || r == 2;
        const bool m = aa && (darkness == 0 ? r == 1 : darkness != 3);
        const bool l = aa && (darkness == 2 || ((darkness == 0 || darkness == 1) && r == 2));
        if (m) set(expectedM, x0 + x, y0 + y, true);
        if (l) set(expectedL, x0 + x, y0 + y, true);
      }
    Plane capturedL{}, capturedM{};
    g.setTextDarkness(darkness);
    g.setRenderMode(GfxRenderer::BW);
    d.fb.fill(255);
    g.beginGrayCapture(capturedL.data(), capturedM.data());
    draw();
    g.endGrayCapture();
    const Plane first = d.fb;
    d.fb.fill(255);
    draw();  // Warm-cache output must equal the first render.
    bool ok = first == expectedBW && d.fb == first && capturedL == expectedL && capturedM == expectedM;
    g.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    d.fb.fill(0);
    draw();
    ok = ok && d.fb == expectedL;
    g.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    d.fb.fill(0);
    draw();
    ok = ok && d.fb == expectedM;
    ++checks;
    if (!ok) {
      if (failures < 8)
        std::cerr << "Scaled glyph FAIL font=" << id << " cp=" << codepoint << " scale=" << scale
                  << " style=" << int(style) << " darkness=" << int(darkness) << " spaced=" << spaced << '\n';
      ++failures;
    }
  }
  g.setRenderMode(GfxRenderer::BW);
}

void checkFamily(GfxRenderer& g, HalDisplay& d, int id, const EpdFontFamily& family) {
  for (float scale : {0.5f, 0.75f, 0.9f})
    for (auto style : {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::BOLD_ITALIC})
      for (char c = 'A'; c <= 'Z'; ++c) {
        const char text[] = {c, 0};
        checkGlyph(g, d, id, family, text, c, scale, style, false);
        checkGlyph(g, d, id, family, text, c, scale, style, true);
      }
  for (auto style :
       {EpdFontFamily::SMALL_CAPS, static_cast<EpdFontFamily::Style>(EpdFontFamily::BOLD | EpdFontFamily::SMALL_CAPS),
        static_cast<EpdFontFamily::Style>(EpdFontFamily::ITALIC | EpdFontFamily::SMALL_CAPS),
        static_cast<EpdFontFamily::Style>(EpdFontFamily::BOLD_ITALIC | EpdFontFamily::SMALL_CAPS)})
    for (bool spaced : {false, true})
      for (float scale : {0.75f, 1.0f}) checkGlyph(g, d, id, family, "a", 'a', scale, style, spaced);
}
}  // namespace

int runDownscaleChecks(GfxRenderer& g, HalDisplay& d, const EpdFontFamily& book, const EpdFontFamily& sans,
                       const char* path) {
  sample(g, d, 1, "bookerly.pgm");
  checkFamily(g, d, 1, book);
  checkFamily(g, d, 2, sans);
  // A one-pixel cross exactly BETWEEN the old point samples disappeared entirely
  // at 50%. Both 1-bit and 2-bit fixtures must preserve it. A non-BMP alias takes
  // the uncached path and must produce the same pixels as the cached ASCII glyph.
  for (bool twoBit : {false, true}) {
    std::array<uint8_t, 16> bitmap{};
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 8; ++x)
        if (x == 3 || y == 3) {
          const int i = y * 8 + x;
          if (twoBit)
            bitmap[i / 4] |= 3 << (6 - 2 * (i % 4));
          else
            bitmap[i / 8] |= 1 << (7 - i % 8);
        }
    const EpdGlyph glyphs[] = {{8, 8, 128, 0, 8, uint16_t(twoBit ? 16 : 8), 0},
                               {8, 8, 128, 0, 8, uint16_t(twoBit ? 16 : 8), 0}};
    const EpdUnicodeInterval intervals[] = {{65, 65, 0}, {0x1f130, 0x1f130, 1}};
    EpdFontData data{};
    data.bitmap = bitmap.data();
    data.glyph = glyphs;
    data.intervals = intervals;
    data.intervalCount = 2;
    data.is2Bit = twoBit;
    data.ascender = 8;
    data.advanceY = 10;
    EpdFont font(&data);
    EpdFontFamily family(&font);
    g.insertFont(4, family);
    for (float scale : {0.5f, 0.75f, 1.2f}) {
      checkGlyph(g, d, 4, family, "A", 'A', scale, EpdFontFamily::REGULAR, false);
      checkGlyph(g, d, 4, family, "\xf0\x9f\x84\xb0", 0x1f130, scale, EpdFontFamily::REGULAR, false);
    }
    g.removeFont(4);
  }
  if (path) {
    CpFontFixture fixture(path);
    g.insertFont(3, fixture.family());
    sample(g, d, 3, "sd-font.pgm");
    checkFamily(g, d, 3, fixture.family());
    g.removeFont(3);
  }
  std::cout << "Scaled glyph coverage, capture, staged AA and cache checks=" << checks << " failures=" << failures
            << '\n';
  return failures ? 1 : 0;
}
