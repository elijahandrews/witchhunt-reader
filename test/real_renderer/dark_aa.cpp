#include <GfxRenderer.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>

#include "DarkAaFixture.h"

namespace {
using Plane = std::array<uint8_t, 48000>;
struct Frame {
  Plane bw{}, lsb{}, msb{};
  bool operator==(const Frame&) const = default;
};

// Independent typed four-tone oracle: no driver folding formula is used here.
// Ink 0/1/2/3 means white/light gray/dark gray/black, before panel calibration.
int ink(const Frame& frame, size_t byte, uint8_t mask) {
  if (frame.bw[byte] & mask) return 0;
  if (frame.msb[byte] & mask) return (frame.lsb[byte] & mask) ? 2 : 1;
  return 3;
}

void screenshot(const std::string& name, const Frame& frame, bool dark) {
  std::ofstream out(name, std::ios::binary);
  out << "P5\n480 800\n255\n";
  constexpr uint8_t light[] = {255, 170, 85, 0};
  constexpr uint8_t inverted[] = {0, 85, 170, 255};
  for (int y = 0; y < 800; ++y)
    for (int x = 0; x < 480; ++x) {
      const int level = ink(frame, (479 - x) * 100 + y / 8, 0x80 >> (y % 8));
      const uint8_t value = dark ? inverted[level] : light[level];
      out.write(reinterpret_cast<const char*>(&value), 1);
    }
  assert(out);
}

template <typename Paint>
Frame capture(GfxRenderer& g, HalDisplay& d, Paint paint, bool staged) {
  Frame frame;
  g.setRenderMode(GfxRenderer::BW);
  d.fb.fill(255);
  if (!staged) g.beginGrayCapture(frame.lsb.data(), frame.msb.data());
  paint();
  g.endGrayCapture();
  frame.bw = d.fb;
  if (staged) {
    g.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    d.fb.fill(0);
    paint();
    g.eraseOpaqueGlyphs(paint);
    frame.lsb = d.fb;
    g.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    d.fb.fill(0);
    paint();
    g.eraseOpaqueGlyphs(paint);
    frame.msb = d.fb;
  }
  g.setRenderMode(GfxRenderer::BW);
  return frame;
}
}  // namespace

void runDarkAaFontChecks(GfxRenderer& g, HalDisplay& d, int fontId, const char* label) {
  const auto savedOrientation = g.getOrientation();
  unsigned cases = 0;
  size_t grayPixels = 0, solidPixels = 0;
  const auto allCaps = static_cast<EpdFontFamily::Style>(EpdFontFamily::SMALL_CAPS | EpdFontFamily::ALL_SMALL_CAPS);
  for (auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted, GfxRenderer::LandscapeClockwise,
                           GfxRenderer::LandscapeCounterClockwise}) {
    g.setOrientation(orientation);
    for (float scale : {.5f, .75f, 1.0f})
      for (uint8_t darkness = 0; darkness <= 4; ++darkness) {
        g.setTextDarkness(darkness);
        const auto paint = [&] {
          int row = 12;
          const int line = std::max(16, int(std::lround(g.getLineHeight(fontId) * scale)) + 3);
          for (auto style :
               {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::BOLD_ITALIC}) {
            g.drawTextSpaced(fontId, 18, row, "Tt Hh Nn Ii Ll AV ffi", true, style, scale, 0);
            row += line;
            const auto caps = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::SMALL_CAPS);
            g.drawTextSpaced(fontId, 18, row, "Thin tall trees", true, caps, scale, 8);
            row += line;
          }
          g.drawTextScaled(fontId, 18, row, "T\xcc\x81 t\xcc\x81 Caf\xc3\xa9", true, allCaps, scale);
          row += line;
          // Adjacent styled fragments, overlapping italics, underline and
          // opaque primitives exercise the same composition in either polarity.
          g.drawTextSpaced(fontId, 18, row, "fT", true, EpdFontFamily::ITALIC, scale, -16);
          g.drawLine(18, row + line - 5, 95, row + line - 5, true);
          g.fillRect(130, row + 3, 14, 14, true);
          g.drawRect(152, row + 3, 14, 14, true);
          // Negative origin tests identical clipping and partial-byte masks.
          g.drawTextScaled(fontId, -5, -5, "T", true, EpdFontFamily::REGULAR, scale);
        };
        g.setDarkMode(false);
        g.invalidateScaledGlyphCache();
        const Frame light = capture(g, d, paint, false);
        assert(light == capture(g, d, paint, true));
        const int width = g.getTextWidthSpaced(fontId, "Tt Hh Nn Ii Ll AV ffi", EpdFontFamily::REGULAR, scale, 0);
        g.setDarkMode(true);
        assert(g.supportsTextAntiAliasing());
        const Frame dark = capture(g, d, paint, false);
        assert(dark == light && dark == capture(g, d, paint, true));
        assert(width == g.getTextWidthSpaced(fontId, "Tt Hh Nn Ii Ll AV ffi", EpdFontFamily::REGULAR, scale, 0));
        // A warm cache and a fresh glyph raster must agree in dark mode too.
        g.invalidateScaledGlyphCache();
        assert(dark == capture(g, d, paint, false));
        size_t frameSolid = 0, frameGray = 0;
        for (size_t i = 0; i < light.bw.size(); ++i) {
          assert(!(light.lsb[i] & ~light.msb[i]));
          assert(!(light.msb[i] & light.bw[i]));
          for (unsigned mask = 1; mask <= 128; mask <<= 1) {
            const int level = ink(light, i, mask);
            frameGray += level == 1 || level == 2;
            frameSolid += level == 3;
          }
        }
        assert(frameSolid > 30);
        if (darkness == 0) assert(frameGray > 30);
        grayPixels += frameGray;
        solidPixels += frameSolid;
        const std::string stem = "dark-aa-" + std::string(label) + "-" + std::to_string(cases);
        std::ofstream fixture(stem + ".bin", std::ios::binary);
        for (const auto* plane : {&light.bw, &light.lsb, &light.msb})
          fixture.write(reinterpret_cast<const char*>(plane->data()), plane->size());
        assert(fixture);
        if (orientation == GfxRenderer::Portrait && scale == 1.0f && darkness == 0) {
          screenshot("light-aa-" + std::string(label) + ".pgm", light, false);
          screenshot("dark-aa-" + std::string(label) + ".pgm", dark, true);
        }
        ++cases;
      }
  }
  assert(cases == 60 && grayPixels > 0 && solidPixels > 0);
  g.setDarkMode(false);
  g.setOrientation(savedOrientation);
  g.setTextDarkness(0);
  g.setRenderMode(GfxRenderer::BW);
  std::cout << "Dark AA actual font=" << label << " scenes=" << cases
            << " light/dark logical planes, capture/staged/cache/metrics identical; gray=" << grayPixels
            << " solid=" << solidPixels << '\n';
}
