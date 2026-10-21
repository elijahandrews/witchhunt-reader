#pragma once
// Host-only framebuffer checks. Coordinates and SDK gray-plane decoding here do
// not call the renderer's projection, clipping, phase or glyph-cache helpers.
#include <GfxRenderer.h>
#include <Utf8.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace outlineProbe {
using Plane = std::array<uint8_t, 48000>;
struct Frame {
  Plane bw{}, lsb{}, msb{};
  Frame() { bw.fill(255); }
  bool operator==(const Frame& b) const { return bw == b.bw && lsb == b.lsb && msb == b.msb; }
};
inline bool physical(int x, int y, GfxRenderer::Orientation o, int& offset, uint8_t& mask) {
  int px, py;
  switch (o) {
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
  if (px < 0 || px >= 800 || py < 0 || py >= 480) return false;
  offset = py * 100 + px / 8;
  mask = uint8_t(0x80 >> (px % 8));
  return true;
}
inline int ink(const Frame& f, int offset, uint8_t mask) {
  if (f.msb[offset] & mask) return f.lsb[offset] & mask ? 2 : 1;
  return f.bw[offset] & mask ? 0 : 3;
}
inline void put(Frame& f, int x, int y, int raw, unsigned darkness, GfxRenderer::Orientation o) {
  int offset;
  uint8_t mask;
  if (!raw || !physical(x, y, o, offset, mask)) return;
  const int mapped = darkness == 4 ? (raw < 3 ? 1 : 3) : darkness >= 2 ? 3 : darkness == 1 ? (raw == 1 ? 1 : 3) : raw;
  const int value = std::max(mapped, ink(f, offset, mask));
  f.bw[offset] &= ~mask;
  f.lsb[offset] &= ~mask;
  f.msb[offset] &= ~mask;
  if (value < 3) f.msb[offset] |= mask;
  if (value == 2) f.lsb[offset] |= mask;
}
template <class Paint>
Frame capture(GfxRenderer& g, HalDisplay& d, Paint paint, bool staged = false) {
  Frame f;
  g.setRenderMode(GfxRenderer::BW);
  d.fb.fill(255);
  if (!staged) g.beginGrayCapture(f.lsb.data(), f.msb.data());
  paint();
  g.endGrayCapture();
  f.bw = d.fb;
  if (staged) {
    g.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    d.fb.fill(0);
    paint();
    g.eraseOpaqueGlyphs(paint);
    f.lsb = d.fb;
    g.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    d.fb.fill(0);
    paint();
    g.eraseOpaqueGlyphs(paint);
    f.msb = d.fb;
  }
  return f;
}
inline void save(const std::string& path, const Frame& f, int width = 480, int height = 800) {
  std::ofstream out(path, std::ios::binary);
  out << "P5\n" << width << ' ' << height << "\n255\n";
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      int at;
      uint8_t mask;
      assert(physical(x, y, GfxRenderer::Portrait, at, mask));
      const uint8_t value = uint8_t(255 - 85 * ink(f, at, mask));
      out.write(reinterpret_cast<const char*>(&value), 1);
    }
}
struct Reference {
  Frame frame;
  int advance = 0, minimumAdvance = 0, maximumAdvance = 0;
  int left = 0, right = 0, top = 0, bottom = 0;
  bool hasInk = false;
};
// Independent double-precision pen walk. No TextRun/GlyphScale/Gfx measuring helper.
// Ligatures are deliberately off so source codepoints and advances form the
// oracle; the separate outline checks still exercise the production ffi path.
inline Reference reference(const EpdFontFamily& family, const char* text, EpdFontFamily::Style style, float scale,
                           int tracking, int x, int y, unsigned darkness, GfxRenderer::Orientation orientation) {
  Reference r;
  const int baseline = int(std::lround(family.getData(EpdFontFamily::REGULAR)->ascender * scale));
  double cursor = 0, quantizedCursor = 0, advanceError = 0;
  // Reproduce the documented Q16 input grid with a double-precision sum. Keep
  // an independent, unquantized pen and error envelope: this permits only the
  // FP32 product + Q16 conversion error, never a generic one-pixel tolerance.
  // Multiplying by the power-of-two unit conversion commutes with FP32 rounding
  // for these finite normal scales; no production positioning helper is used.
  const auto addMetric = [&](int value, float actual) {
    const double term = double(value) * actual / 16;
    cursor += term;
    quantizedCursor += std::round(double(float(value * actual)) * 4096) / 65536;
    advanceError += std::fabs(term) * std::ldexp(1.0, -24) + std::ldexp(.5, -16) +
                    std::fabs(cursor) * std::numeric_limits<double>::epsilon();
  };
  int previous = 0, lastX = 0, lastLeft = 0, lastWidth = 0, lastTop = 0, lastPhase = 0;
  const auto* input = reinterpret_cast<const uint8_t*>(text);
  while (uint32_t cp = utf8NextCodepoint(&input)) {
    const bool combining = utf8IsCombiningMark(cp);
    float caps = 1;
    if (!combining) cp = family.resolveCaps(cp, style, caps);
    const float actual = scale * caps;
    const auto metric = family.getGlyph(cp, style);
    if (!metric) {
      if (!combining) previous = 0;
      continue;
    }
    int dx = lastX, dy = 0, phase = lastPhase;
    if (!combining) {
      if (previous) {
        cursor += tracking / 16.0;
        quantizedCursor += tracking / 16.0;
        advanceError += std::fabs(cursor) * std::numeric_limits<double>::epsilon();
        addMetric(family.getKerning(previous, cp, style), actual);
      }
      assert(std::fabs(quantizedCursor - cursor) <= advanceError);
      const int phasePen = int(std::floor(quantizedCursor * 16 + .5));
      assert(phasePen >= int(std::floor((cursor - advanceError) * 16 + .5)) &&
             phasePen <= int(std::floor((cursor + advanceError) * 16 + .5)));
      dx = int(std::floor((phasePen + 8) / 16.0));
      phase = phasePen - dx * 16;
      addMetric(metric.advanceX, actual);
      previous = cp;
    }
    EpdRasterGlyph glyph;
    assert(family.rasterGlyph(cp, style, actual, int8_t(phase), glyph));
    const auto& m = glyph.metrics;
    assert(glyph.bytes == (size_t(m.width) * m.height + 3) / 4);
    if (combining) {
      dx += lastLeft + (lastWidth - int(m.width)) / 2 - m.left;
      const int markBottom = m.top - m.height;
      if (markBottom < lastTop) dy -= lastTop - markBottom;
    } else {
      lastX = dx;
      lastLeft = m.left;
      lastWidth = m.width;
      lastTop = m.top;
      lastPhase = phase;
    }
    for (int yy = 0; yy < m.height; ++yy)
      for (int xx = 0; xx < m.width; ++xx) {
        const size_t i = size_t(yy) * m.width + xx;
        const int raw = (glyph.bitmap[i / 4] >> (6 - 2 * (i % 4))) & 3;
        if (!raw) continue;
        const int rx = dx + m.left + xx, ry = dy - m.top + yy;
        if (!r.hasInk) {
          r.left = rx;
          r.right = rx + 1;
          r.top = ry;
          r.bottom = ry + 1;
          r.hasInk = true;
        } else {
          r.left = std::min(r.left, rx);
          r.right = std::max(r.right, rx + 1);
          r.top = std::min(r.top, ry);
          r.bottom = std::max(r.bottom, ry + 1);
        }
        put(r.frame, x + rx, y + baseline + ry, raw, darkness, orientation);
      }
  }
  assert(std::fabs(quantizedCursor - cursor) <= advanceError);
  r.advance = std::max(0, int(std::floor(quantizedCursor + .5)));
  r.minimumAdvance = std::max(0, int(std::floor(cursor - advanceError + .5)));
  r.maximumAdvance = std::max(0, int(std::floor(cursor + advanceError + .5)));
  assert(r.advance >= r.minimumAdvance && r.advance <= r.maximumAdvance);
  return r;
}
inline std::vector<uint8_t> readFont(const char* path) {
  std::ifstream in(path, std::ios::binary);
  assert(in);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}
}  // namespace outlineProbe
