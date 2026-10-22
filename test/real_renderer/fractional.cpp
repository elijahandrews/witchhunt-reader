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

// Round the exact forward-projected coverage to the nearest representable
// shade. Equal-distance ties choose the even shade. No neighbor can alter a
// pixel's value: aggregate row-ink conservation must not create edge noise.
std::vector<uint8_t> quantizeReferenceCoverage(const std::vector<int64_t>& coverage, int w, int h, int64_t area) {
  assert(area > 0 && coverage.size() == static_cast<size_t>(w * h));
  std::vector<uint8_t> result;
  result.reserve(coverage.size());
  for (const auto value : coverage) {
    assert(value >= 0 && value <= 3 * area);
    // Choose by distance to all four shades, independently of production's
    // quotient/remainder implementation.
    int best = 0;
    int64_t distance = value;
    for (int shade = 1; shade <= 3; ++shade) {
      const int64_t candidate = std::abs(value - shade * area);
      if (candidate < distance || (candidate == distance && !(shade & 1))) {
        best = shade;
        distance = candidate;
      }
    }
    result.push_back(best);
  }
  return result;
}

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
// Independent forward projection: move each source pixel's rectangle onto the
// display grid, then quantize coverage. No TextRun, GlyphScale, cache, or renderer
// sampling helper participates in this oracle.
Frame project(const std::vector<int>& source, int w, int h, int left, int top, float scale, double pen, int baseline,
              int darkness, GfxRenderer::Orientation orientation) {
  Frame f;
  f.bw.fill(255);
  f.lsb.fill(0);
  f.msb.fill(0);
  // Exact forward geometry in 1/(16*step) display units. The extra factor
  // represents the public 1/16-pixel pen phase without rounding its product.
  const int64_t step = std::lround(65536.0 / scale), unit = step * 16;
  constexpr int64_t one = 65536 * 16;
  const int64_t pen16 = std::llround(pen * 16);
  const int x0 = int(std::floor(pen + left * scale)) - 1;
  const int x1 = int(std::ceil(pen + (left + w) * scale)) + 1;
  const int y0 = baseline + int(std::floor(-top * scale)) - 1;
  const int y1 = baseline + int(std::ceil((h - top) * scale)) + 1;
  const int width = x1 - x0, height = y1 - y0;
  std::vector<int64_t> coverage(width * height);
  for (int sy = 0; sy < h; ++sy)
    for (int sx = 0; sx < w; ++sx) {
      const int64_t a = (pen16 - x0 * 16) * step + (sx + left) * one, b = a + one;
      const int64_t c = (baseline - y0) * unit + (sy - top) * one, d = c + one;
      for (int y = std::max<int64_t>(0, c / unit); y < std::min<int64_t>(height, (d + unit - 1) / unit); ++y)
        for (int x = std::max<int64_t>(0, a / unit); x < std::min<int64_t>(width, (b + unit - 1) / unit); ++x)
          coverage[y * width + x] += source[sy * w + sx] * (std::min(b, (x + 1) * unit) - std::max(a, x * unit)) *
                                     (std::min(d, (y + 1) * unit) - std::max(c, y * unit));
    }
  const auto raw = quantizeReferenceCoverage(coverage, width, height, unit * unit);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) put(f, x0 + x, y0 + y, raw[y * width + x], darkness, orientation);
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
  // Fixed answers exercise the policy independently of both projection code
  // and production: flat gray bars never acquire an alternating shade pattern.
  const auto expect = [&](const std::vector<int64_t>& values, int64_t area, const std::vector<uint8_t>& expected) {
    ++checks;
    if (quantizeReferenceCoverage(values, values.size(), 1, area) != expected) {
      ++failures;
      std::cerr << "Reference plateau/area quantization example failed\n";
    }
  };
  expect({1, 1}, 2, {0, 0});
  expect({1, 1, 1, 1}, 2, {0, 0, 0, 0});
  expect({3, 3, 5, 5}, 2, {2, 2, 2, 2});
  expect({6, 5, 5, 6}, 10, {1, 0, 0, 1});
  expect({1, 6, 1, 0, 1, 6, 1}, 2, {0, 3, 0, 0, 0, 3, 0});
  expect({1, 2, 2, 1}, 2, {0, 1, 1, 0});
  expect({1, 6, 2}, 2, {0, 3, 1});
  expect({29, 48, 41, 2}, 16, {2, 3, 3, 0});
  expect({23, 48, 43, 6}, 16, {1, 3, 3, 0});
  expect({1, 9, 2}, 3, {0, 3, 1});
  // A rounded bowl taper: row diffusion previously made the .25 fringe
  // visible and reduced the adjacent 1.75 stroke to 1 (both .75-level errors).
  expect({5, 10, 12, 7, 1}, 4, {1, 2, 3, 2, 0});
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
            const int adv = int(std::floor(((64.0 + 91) * scale + tracking) / 16 + .5));
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
  // Odd density-2 advances must retain their half-sixteenth through the run.
  // The old walk rounded 32 * (91/2) into 32 * 46: 92 px instead of 91 px.
  // An independent double pen also exercises negative kern/tracking, synthetic
  // caps, integer-only fonts, and the last selected character's draw position.
  const EpdKernClassEntry kernClass[] = {{'A', 1}};
  const int8_t kernValue[] = {-3};
  for (bool kerned : {false, true}) {
    data.kernLeftClasses = data.kernRightClasses = kerned ? kernClass : nullptr;
    data.kernLeftEntryCount = data.kernRightEntryCount = kerned ? 1 : 0;
    data.kernLeftClassCount = data.kernRightClassCount = kerned ? 1 : 0;
    data.kernMatrix = kerned ? kernValue : nullptr;
    for (bool fractional : {false, true}) {
      data.is2Bit = fractional;
      for (float scale : {.5f, .75f, 1.0f, 1.25f})
        for (int tracking : {-49, -3, 0, 3})
          for (auto style : {EpdFontFamily::REGULAR, EpdFontFamily::SMALL_CAPS}) {
            const std::string run(32, style == EpdFontFamily::REGULAR ? 'A' : 'a');
            const double actualScale = scale * (style == EpdFontFamily::REGULAR ? 1.0 : .75);
            double pen = 0;
            int count = 0, lastX = 0;
            textRun::CharacterRange range{31, 31};
            const auto measured = textRun::walk<true, false>(
                family, run.c_str(), style, scale, tracking,
                [&](uint32_t cp, int x, int, float, bool, int8_t phase) {
                  assert(cp == 'A');
                  if (count) pen += (tracking + (kerned ? -3 * actualScale : 0)) / 16;
                  const int sixteenths = int(std::floor(pen * 16 + .5));
                  const int expectedX =
                      fractional ? int(std::floor(sixteenths / 16.0 + .5)) : int(std::floor(pen + .5));
                  const int expectedPhase = fractional ? sixteenths - expectedX * 16 : 0;
                  assert(x == expectedX && phase == expectedPhase);
                  assert(phase >= -8 && phase < 8);
                  lastX = x;
                  pen += 91 * actualScale / 16;
                  ++count;
                },
                &range, false);
            assert(count == 32 && measured.advance == std::max(0, int(std::floor(pen + .5))));
            assert(range.found && range.x == lastX && range.right >= int(std::floor(pen + .5)));
            ++checks;
          }
    }
  }
  data.is2Bit = true;
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
