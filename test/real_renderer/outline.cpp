#include <GfxRenderer.h>
#include <OutlineFontFace.h>
#include <TextRun.h>

#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <vector>

#include "DarkAaFixture.h"

namespace {
using Plane = std::array<uint8_t, 48000>;
bool bit(const Plane& p, int x, int y) { return p[(479 - x) * 100 + y / 8] & (0x80 >> (y % 8)); }
void save(const char* path, const Plane& bw, const Plane& lsb, const Plane& msb) {
  std::ofstream out(path, std::ios::binary);
  out << "P5\n480 800\n255\n";
  for (int y = 0; y < 800; ++y)
    for (int x = 0; x < 480; ++x) {
      const uint8_t value = bit(bw, x, y) ? 255 : bit(msb, x, y) ? (bit(lsb, x, y) ? 85 : 170) : 0;
      out.write(reinterpret_cast<const char*>(&value), 1);
    }
}
}  // namespace

int runOutlineChecks(GfxRenderer& renderer, HalDisplay& display, const char* path) {
  std::ifstream file(path, std::ios::binary);
  assert(file);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
  int checks = 0;
  const auto allCaps = static_cast<EpdFontFamily::Style>(EpdFontFamily::SMALL_CAPS | EpdFontFamily::ALL_SMALL_CAPS);
  Plane lsb{}, msb{};
  display.fb.fill(255);
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.setTextDarkness(0);
  renderer.beginGrayCapture(lsb.data(), msb.data());
  int row = 12;
  for (const float points : {11.2f, 14.0f, 22.4f}) {
    OutlineFontFace outline;
    OutlineFontFace::Options options;
    options.logicalPx = points * 150.0f / 72.0f;
    options.opticalPoints = points;
    assert(outline.openMemory(bytes.data(), bytes.size(), options));
    EpdFontData data{};
    const auto line = outline.lineMetrics();
    data.advanceY = line.advanceY;
    data.ascender = line.ascender;
    data.descender = line.descender;
    data.is2Bit = true;
    data.outline = &OutlineFontFace::callbacks();
    data.outlineCtx = &outline;
    EpdFont font(&data);
    EpdFontFamily family(&font);
    renderer.insertScaledFont(9, family, .5f);
    // Rendering obtains packed data through the runtime provider; measurement
    // must preserve the exact source glyph's fractional advances at density2.
    const auto h = outline.metrics(outline.glyphId('H'));
    const auto n = outline.metrics(outline.glyphId('N'));
    const auto t = outline.metrics(outline.glyphId('T'));
    const int expected = (std::lround(h.advanceX * .5f) + std::lround(n.advanceX * .5f) +
                          std::lround(t.advanceX * .5f) + std::lround(outline.kerning(h.index, n.index) * .5f) +
                          std::lround(outline.kerning(n.index, t.index) * .5f) + 8) >>
                         4;
    assert(renderer.getTextAdvanceXSpaced(9, "HNT", EpdFontFamily::REGULAR, 1, 0) == expected);
    const uint16_t gids[] = {outline.glyphId('f'), outline.glyphId('f'), outline.glyphId('i')};
    if (const auto triple = outline.ligature(gids, 3)) {
      const char* tail = "fi N";
      assert(font.applyLigatures('f', tail) == EPD_ALTERNATE_GLYPH_BASE + triple);
      assert(std::string(tail) == " N");
      ++checks;
    }
    const std::string label = "Outline " + std::to_string(points).substr(0, 4) + "pt";
    renderer.drawTextScaled(2, 16, row, label.c_str(), true, EpdFontFamily::REGULAR, .5f);
    row += 24;
    for (auto style : {EpdFontFamily::REGULAR, EpdFontFamily::SMALL_CAPS, allCaps}) {
      const char* text = "HNT Thin ffi 123";
      const int expectedWidth = textRun::walk(family, text, style, .5f, 0, [](auto...) {}).advance;
      assert(renderer.getTextAdvanceXSpaced(9, text, style, 1, 0) == expectedWidth);
      renderer.drawTextSpaced(9, 16, row, text, true, style, 1, 0);
      row += renderer.getLineHeight(9) + 3;
      ++checks;
    }
    float capsScale;
    const auto small = outline.glyphId('n', 1);
    if (small) {
      assert(font.resolveCaps('n', 1, capsScale) == EPD_ALTERNATE_GLYPH_BASE + small);
      assert(capsScale == 1);
      ++checks;
    }
    const auto capital = outline.glyphId('N', 2);
    if (capital) {
      assert(font.resolveCaps('N', 2, capsScale) == EPD_ALTERNATE_GLYPH_BASE + capital);
      assert(capsScale == 1);
      ++checks;
    }
    renderer.endGrayCapture();
    const auto saved = display.fb;
    const auto fixtureLabel = "outline-" + std::to_string(int(points * 10));
    runDarkAaFontChecks(renderer, display, 9, fixtureLabel.c_str());
    display.fb = saved;
    renderer.beginGrayCapture(lsb.data(), msb.data());
    renderer.removeFont(9);
    ++checks;
  }
  renderer.endGrayCapture();
  save("outline-font.pgm", display.fb, lsb, msb);
  size_t gray = 0, black = 0;
  for (int y = 0; y < 800; ++y)
    for (int x = 0; x < 480; ++x) {
      if (!bit(display.fb, x, y)) {
        if (bit(msb, x, y))
          ++gray;
        else
          ++black;
      }
    }
  assert(gray > 100 && black > 100);
  std::cout << "runtime outline/real screen/caps/optical-size/ffi checks=" << checks << " passed, gray=" << gray
            << " black=" << black << '\n';
  return 0;
}
