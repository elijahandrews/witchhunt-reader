#include <GfxRenderer.h>
#include <TextRun.h>
#include <builtinFonts/bookerly_10_regular.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <vector>

namespace {
using Plane = std::array<uint8_t, 48000>;
struct Frame {
  Plane bw, lsb, msb;
  bool operator==(const Frame& b) const { return bw == b.bw && lsb == b.lsb && msb == b.msb; }
};
template <class Draw>
Frame capture(GfxRenderer& g, HalDisplay& d, Draw draw, bool staged = false) {
  Frame f;
  f.lsb.fill(0);
  f.msb.fill(0);
  g.setRenderMode(GfxRenderer::BW);
  d.fb.fill(255);
  if (!staged) g.beginGrayCapture(f.lsb.data(), f.msb.data());
  draw();
  g.endGrayCapture();
  f.bw = d.fb;
  if (staged) {
    g.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    d.fb.fill(0);
    draw();
    g.eraseOpaqueGlyphs(draw);
    f.lsb = d.fb;
    g.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    d.fb.fill(0);
    draw();
    g.eraseOpaqueGlyphs(draw);
    f.msb = d.fb;
  }
  return f;
}
void put(Frame& f, int x, int y, int raw, int darkness, GfxRenderer::Orientation orientation) {
  int px, py;
  switch (orientation) {
    case GfxRenderer::Portrait:
      px = y;
      py = 479 - x;
      break;
    case GfxRenderer::PortraitInverted:
      px = 799 - y;
      py = x;
      break;
    case GfxRenderer::LandscapeClockwise:
      px = 799 - x;
      py = 479 - y;
      break;
    default:
      px = x;
      py = y;
      break;
  }
  if (!raw || px < 0 || px >= 800 || py < 0 || py >= 480) return;
  int i = py * 100 + px / 8;
  uint8_t b = 0x80 >> (px % 8);
  f.bw[i] &= ~b;
  if (raw < 3 && (darkness == 0 || darkness == 4 || (darkness == 1 && raw == 1))) f.msb[i] |= b;
  if (raw == 2 && darkness == 0) f.lsb[i] |= b;
}
int nearestEven(double v) {
  int lo = int(std::floor(v));
  if (std::abs(v - lo - .5) < 1e-9) return lo + (lo & 1);
  return int(std::floor(v + .5));
}
// Independent forward projection: move each source pixel's rectangle onto the
// display grid, then quantize coverage. No TextRun, GlyphScale, cache, or renderer
// sampling helper participates in this oracle.
Frame project(const std::vector<int>& source, int w, int h, int left, int top, float scale, double pen, int baseline,
              int darkness, GfxRenderer::Orientation orientation) {
  Frame f;
  f.bw.fill(255);
  f.lsb.fill(0);
  f.msb.fill(0);
  // Match the documented fixed-point inverse scale, not its sampling algorithm.
  const double step = std::lround(65536.0 / scale) / 65536.0;
  const double factor = 1 / step;
  const int x0 = int(std::floor(pen + left * scale)) - 1;
  const int x1 = int(std::ceil(pen + (left + w) * scale)) + 1;
  const int y0 = baseline + int(std::floor(-top * scale)) - 1;
  const int y1 = baseline + int(std::ceil((h - top) * scale)) + 1;
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      double coverage = 0;
      for (int sy = 0; sy < h; ++sy)
        for (int sx = 0; sx < w; ++sx) {
          double a = pen + (left + sx) * factor, b = a + factor;
          double c = baseline + (sy - top) * factor, d = c + factor;
          coverage += source[sy * w + sx] * std::max(0.0, std::min(b, x + 1.0) - std::max(a, double(x))) *
                      std::max(0.0, std::min(d, y + 1.0) - std::max(c, double(y)));
        }
      put(f, x, y, std::clamp(nearestEven(coverage), 0, 3), darkness, orientation);
    }
  return f;
}
void save(const Frame& f) {
  std::ofstream out("fractional-spacing.pgm", std::ios::binary);
  out << "P5\n480 800\n255\n";
  for (int y = 0; y < 800; ++y)
    for (int x = 0; x < 480; ++x) {
      int i = (479 - x) * 100 + y / 8;
      uint8_t b = 0x80 >> (y % 8);
      uint8_t v = f.bw[i] & b ? 255 : f.msb[i] & b ? (f.lsb[i] & b ? 85 : 170) : 0;
      out.write(reinterpret_cast<const char*>(&v), 1);
    }
}
}  // namespace

int runFractionalChecks(GfxRenderer& g, HalDisplay& d) {
  int checks = 0, failures = 0;
  const auto orientation = g.getOrientation();
  const std::vector<int> source{0, 1, 3, 0, 0, 1, 3, 2, 0, 0, 3, 3, 3, 2, 1, 0, 1, 3, 0, 0};
  std::array<uint8_t, 6> bits{};
  for (int i = 0; i < 20; ++i) bits[i / 4] |= source[i] << (6 - 2 * (i % 4));
  const EpdGlyph glyphs[] = {{0, 0, 64, 0, 0, 0, 0}, {5, 4, 91, -1, 3, 5, 0}, {5, 4, 91, -1, 3, 5, 0}};
  const EpdUnicodeInterval intervals[] = {{32, 32, 0}, {65, 65, 1}, {0x1f130, 0x1f130, 2}};
  EpdFontData data{};
  data.bitmap = bits.data();
  data.glyph = glyphs;
  data.intervals = intervals;
  data.intervalCount = 3;
  data.is2Bit = true;
  data.ascender = 8;
  data.advanceY = 10;
  EpdFont font(&data);
  EpdFontFamily family(&font);
  g.insertFont(21, family);
  for (auto dir : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted, GfxRenderer::LandscapeClockwise,
                   GfxRenderer::LandscapeCounterClockwise}) {
    g.setOrientation(dir);
    for (float scale : {.5f, .75f, 1.0f, 1.25f})
      for (int dark = 0; dark < 5; ++dark)
        for (bool clipped : {false, true}) {
          g.setTextDarkness(dark);
          g.invalidateScaledGlyphCache();
          // Interleave phases sharing the same dimensions: a missing phase in
          // the cache key must fail even though all cold glyphs look correct.
          for (int phase : {0, 7, -8, -1, 3, -5, 6, -7, 1, -3, 5, -6, 2, -4, 4, -2}) {
            const int tracking = clipped ? phase - 96 : phase;
            const int x = clipped ? 0 : 20, y = clipped ? -6 : 20;
            const double pen = x + (std::lround(64 * scale) + tracking) / 16.0;
            const int baseline = y + int(std::lround(8 * scale));
            auto draw = [&] { g.drawTextSpaced(21, x, y, " A", true, EpdFontFamily::REGULAR, scale, tracking); };
            const Frame expected = project(source, 5, 4, -1, 3, scale, pen, baseline, dark, dir);
            const Frame actual = capture(g, d, draw), warm = capture(g, d, draw), staged = capture(g, d, draw, true);
            const Frame direct = capture(g, d, [&] {
              g.drawTextSpaced(21, x, y, " \xf0\x9f\x84\xb0", true, EpdFontFamily::REGULAR, scale, tracking);
            });
            bool ok = actual == expected && actual == warm && actual == staged && actual == direct;
            const int adv = (int(std::lround(64 * scale)) + tracking + int(std::lround(91 * scale)) + 8) >> 4;
            ok &= g.getTextAdvanceXSpaced(21, " A", EpdFontFamily::REGULAR, scale, tracking) == std::max(0, adv);
            const auto range = g.getTextRangeSpaced(21, " A", EpdFontFamily::REGULAR, scale, tracking, 1, 1);
            ok &= range.left <= int(std::floor(pen - x - scale));
            ok &= range.right >= int(std::ceil(pen - x + 4 * scale));
            ++checks;
            if (!ok && ++failures <= 8)
              std::cerr << "Fractional projection/cache/staged/range mismatch phase=" << phase << " scale=" << scale
                        << " darkness=" << dark << " clipped=" << clipped << " orientation=" << int(dir) << '\n';
          }
        }
  }
  g.removeFont(21);
  g.setOrientation(GfxRenderer::Portrait);
  g.setTextDarkness(0);
  EpdFont book(&bookerly_10_regular);
  EpdFontFamily bookFamily(&book);
  g.insertFont(21, bookFamily);
  // The reported word: check full fractional origins against the original
  // font's advances, rather than rounding an expected answer through TextRun.
  const int expected[] = {0, 160, 297, 407, 515};
  int count = 0;
  textRun::walk(bookFamily, "still", EpdFontFamily::REGULAR, 1, 0,
                [&](uint32_t, int x, int, float, bool, int8_t phase) {
                  assert(count < 5 && x * 16 + phase == expected[count++]);
                });
  assert(count == 5);
  auto specimen = [&] {
    int y = 12;
    for (float scale : {.8f, 1.0f, 1.2f, 1.4f}) {
      for (const char* text : {"still  ill  little  sitting  minimum", "The tall tree stood still.",
                               "AV office ffi 123  thin stems", "jazz fjord \xc3\xa9lan e\xcc\x81"}) {
        g.drawTextSpaced(21, 18, y, text, true, EpdFontFamily::REGULAR, scale, 0);
        y += int(32 * scale);
      }
      y += 22;
    }
  };
  const Frame specimenFrame = capture(g, d, specimen);
  if (!(specimenFrame == capture(g, d, specimen, true))) ++failures;
  save(specimenFrame);
  ++checks;
  g.removeFont(21);
  g.setOrientation(orientation);
  g.setRenderMode(GfxRenderer::BW);
  std::cout << "Fractional projection/cache/staged/rotation/clipping/range checks=" << checks
            << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
