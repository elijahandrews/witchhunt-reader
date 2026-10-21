#include <GfxRenderer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <vector>

namespace {
using Plane = std::array<uint8_t, 48000>;
struct Frame {
  Plane bw, lsb{}, msb{};
  bool operator==(const Frame& b) const { return bw == b.bw && lsb == b.lsb && msb == b.msb; }
};
template <typename Draw>
Frame capture(GfxRenderer& g, HalDisplay& d, Draw draw, bool staged = false) {
  Frame f;
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
int ink(const Frame& f, int x, int y, GfxRenderer::Orientation orientation) {
  int px = x, py = y;
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
      break;
  }
  const int i = py * 100 + px / 8, b = 128 >> (px % 8);
  return f.bw[i] & b ? 0 : f.msb[i] & b ? (f.lsb[i] & b ? 2 : 1) : 3;
}
}  // namespace

int runStemWeightChecks(GfxRenderer& g, HalDisplay& d) {
  int checks = 0, failures = 0;
  const auto originalOrientation = g.getOrientation();
  g.setTextDarkness(0);
  // Original rectangular stems: complete middle rows have known area, with
  // varying edges separated by a solid core. Their weight must not depend on
  // placement. A five-source-pixel stem at 2x is the reported 2.5px case.
  for (int width : {5, 6, 7, 8}) {
    std::vector<uint8_t> bitmap((width * 16 + 3) / 4, 255);
    const EpdGlyph glyphs[] = {{0, 0, 64, 0, 0, 0, 0},
                               {uint8_t(width), 16, 256, -1, 16, uint16_t(bitmap.size()), 0},
                               {uint8_t(width), 16, 256, -1, 16, uint16_t(bitmap.size()), 0}};
    const EpdUnicodeInterval intervals[] = {{32, 32, 0}, {'A', 'A', 1}, {0x1f130, 0x1f130, 2}};
    EpdFontData data{};
    data.bitmap = bitmap.data();
    data.glyph = glyphs;
    data.intervals = intervals;
    data.intervalCount = 3;
    data.is2Bit = true;
    data.ascender = 16;
    data.advanceY = 20;
    EpdFont font(&data);
    g.insertFont(23, EpdFontFamily(&font));
    for (auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted, GfxRenderer::LandscapeClockwise,
                             GfxRenderer::LandscapeCounterClockwise}) {
      g.setOrientation(orientation);
      for (float scale : {.5f, .75f, 1.f, 1.25f}) {
        // The resampler uses a Q16 reciprocal; compute its exact geometric
        // area, independently of its sampling or quantization implementation.
        const int step = int(std::lround(65536.0 / scale));
        const int expected = (3 * width * 65536 + (step - 1) / 2) / step;
        for (int phase = -8; phase < 8; ++phase) {
          g.invalidateScaledGlyphCache();
          auto draw = [&] { g.drawTextSpaced(23, 32, 40, " A", true, EpdFontFamily::REGULAR, scale, phase); };
          const auto first = capture(g, d, draw);
          bool ok = first == capture(g, d, draw) && first == capture(g, d, draw, true);
          ok &= first == capture(g, d, [&] {
                  g.drawTextSpaced(23, 32, 40, " \xf0\x9f\x84\xb0", true, EpdFontFamily::REGULAR, scale, phase);
                });
          const int baseline = 40 + int(std::lround(16 * scale));
          for (int y = baseline - int(8 * scale); y < baseline - int(4 * scale); ++y) {
            int total = 0;
            for (int x = 20; x < 65; ++x) total += ink(first, x, y, orientation);
            ok &= total == expected;
          }
          ++checks;
          if (!ok && ++failures <= 5)
            std::cerr << "Stem weight changes with phase: width=" << width << " scale=" << scale << " phase=" << phase
                      << " orientation=" << int(orientation) << '\n';
        }
      }
    }
    g.removeFont(23);
  }
  g.setOrientation(GfxRenderer::Portrait);
  // Flat fractional bars must stay uniform, including their first/last pixel.
  // Unrestricted row error diffusion would turn these into alternating shades.
  for (int total : {2, 6, 10}) {
    constexpr int width = 24, height = 2;
    std::array<uint8_t, width * height / 4> bitmap{};
    for (int x = 0; x < width / 2; ++x)
      for (int p = 0; p < 4; ++p) {
        const int raw = std::clamp(total - p * 3, 0, 3), i = (p / 2) * width + 2 * x + p % 2;
        bitmap[i / 4] |= raw << (6 - 2 * (i % 4));
      }
    const EpdGlyph glyph{width, height, 256, 0, height, uint16_t(bitmap.size()), 0};
    const EpdUnicodeInterval interval{'A', 'A', 0};
    EpdFontData data{};
    data.bitmap = bitmap.data();
    data.glyph = &glyph;
    data.intervals = &interval;
    data.intervalCount = 1;
    data.is2Bit = true;
    data.ascender = 2;
    data.advanceY = 4;
    EpdFont font(&data);
    g.insertFont(23, EpdFontFamily(&font));
    auto draw = [&] { g.drawTextSpaced(23, 30, 30, "A", true, EpdFontFamily::REGULAR, .5f, 0); };
    const auto f = capture(g, d, draw);
    const int expected = total == 2 ? 0 : 2;
    bool ok = f == capture(g, d, draw, true);
    for (int x = 30; x < 42; ++x) ok &= ink(f, x, 30, GfxRenderer::Portrait) == expected;
    ++checks;
    if (!ok) {
      ++failures;
      std::cerr << "Flat gray bar gained texture: total=" << total << '\n';
    }
    g.removeFont(23);
  }
  g.setOrientation(originalOrientation);
  g.setRenderMode(GfxRenderer::BW);
  std::cout << "Stem phase-weight and flat-edge checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
