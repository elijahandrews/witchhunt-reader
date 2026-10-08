#include <EpdOutlineFontCallbacks.h>
#include <GfxRenderer.h>
#include <SmallCaps.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>

#include "CpFontFixture.h"
#include "DarkAaFixture.h"

int runOverlapChecks(GfxRenderer&, HalDisplay&, int, const EpdFontFamily&);

namespace rendererTest {
void setScanning(bool);
int recordedCount();
}  // namespace rendererTest

namespace {
using Plane = std::array<uint8_t, 48000>;
bool bit(const Plane& p, int x, int y) { return p[(479 - x) * 100 + y / 8] & (0x80 >> (y % 8)); }
void save(const char* path, const Plane& bw, const Plane& lsb, const Plane& msb, bool gray = true) {
  std::ofstream out(path, std::ios::binary);
  out << "P5\n480 800\n255\n";
  for (int y = 0; y < 800; ++y)
    for (int x = 0; x < 480; ++x) {
      const bool l = bit(lsb, x, y), m = bit(msb, x, y);
      const uint8_t v = bit(bw, x, y) ? 255 : gray && m ? (l ? 85 : 170) : 0;
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

void sampleNativeWeight(GfxRenderer& g, HalDisplay& d, int id) {
  Plane lsb{}, msb{};
  g.setRenderMode(GfxRenderer::BW);
  g.setTextDarkness(0);
  d.fb.fill(255);
  g.beginGrayCapture(lsb.data(), msb.data());
  int y = 18;
  for (auto style : {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::BOLD_ITALIC}) {
    for (const char* text : {"Tt Hh Nn Ii Ll Mm Ww", "The tall tree stood still."}) {
      g.drawTextSpaced(id, 16, y, text, true, style, 1.0f, 0);
      y += g.getLineHeight(id) + 8;
    }
    y += 24;
  }
  g.endGrayCapture();
  save("sd-font-native-aa.pgm", d.fb, lsb, msb);
  save("sd-font-native-bw.pgm", d.fb, lsb, msb, false);
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
std::vector<uint8_t> reference(const uint8_t* bitmap, bool twoBit, int sw, int sh, int bearingLeft, int bearingTop,
                               float scale) {
  const int originX = std::floor(bearingLeft * scale), originY = std::floor(-bearingTop * scale);
  const int w = std::ceil((bearingLeft + sw) * scale) - originX;
  const int h = std::ceil((sh - bearingTop) * scale) - originY;
  std::vector<double> coverage(w * h);
  const double factor = 65536.0 / std::lround(65536.0 / scale);
  for (int sy = 0; sy < sh; ++sy)
    for (int sx = 0; sx < sw; ++sx) {
      const int i = sy * sw + sx;
      const int raw = twoBit ? (bitmap[i / 4] >> (6 - 2 * (i % 4))) & 3 : ((bitmap[i / 8] >> (7 - i % 8)) & 1) * 3;
      const double left = (sx + bearingLeft) * factor - originX, right = (sx + bearingLeft + 1) * factor - originX;
      const double top = (sy - bearingTop) * factor - originY, bottom = (sy - bearingTop + 1) * factor - originY;
      for (int y = std::max(0, int(top)); y < std::min(h, int(std::ceil(bottom))); ++y)
        for (int x = std::max(0, int(left)); x < std::min(w, int(std::ceil(right))); ++x)
          coverage[y * w + x] += raw * (std::min(right, x + 1.0) - std::max(left, double(x))) *
                                 (std::min(bottom, y + 1.0) - std::max(top, double(y)));
    }
  std::vector<uint8_t> result(w * h);
  for (size_t i = 0; i < result.size(); ++i) {
    const int lower = std::floor(coverage[i]);
    // The independent floating-point projection can land a few ulps away from
    // an exact half. Resolve that tie to the even level; all other values round
    // to the nearest level without changing the coverage calculation.
    const bool tie = std::abs(coverage[i] - lower - 0.5) < 1e-12;
    result[i] = std::clamp(tie ? lower + (lower & 1) : int(std::floor(coverage[i] + 0.5)), 0, 3);
  }
  return result;
}

int checks = 0, failures = 0;
void checkGlyph(GfxRenderer& g, HalDisplay& d, int id, const EpdFontFamily& family, const char* text,
                uint32_t codepoint, float scale, EpdFontFamily::Style style, bool spaced) {
  // Read the authored mapping directly; do not use the production text walk
  // to decide the independent pixel projection's source glyph or scale.
  const auto data = family.getData(style);
  uint32_t alternate = 0;
  const auto mapped = [&](uint8_t mode) -> uint32_t {
    if (data->outline) {
      const auto gid = data->outline->glyphId(data->outlineCtx, codepoint, mode);
      return gid ? EPD_ALTERNATE_GLYPH_BASE + gid : 0;
    }
    return data->capsGlyph ? data->capsGlyph(data->glyphMissCtx, codepoint, mode) : 0;
  };
  if (style & EpdFontFamily::ALL_SMALL_CAPS) alternate = mapped(2);
  if (!alternate && (style & (EpdFontFamily::SMALL_CAPS | EpdFontFamily::ALL_SMALL_CAPS))) alternate = mapped(1);
  bool folded = false;
  if (alternate)
    codepoint = alternate;
  else if (style & (EpdFontFamily::SMALL_CAPS | EpdFontFamily::ALL_SMALL_CAPS)) {
    const bool upper = (style & EpdFontFamily::ALL_SMALL_CAPS) && smallCaps::isUppercase(codepoint);
    folded = smallCaps::fold(codepoint) || upper;
  }
  const float glyphScale = scale * g.fontBaseScale(id) * (folded ? smallCaps::SCALE : 1.0f);
  const auto glyph = family.getGlyph(codepoint, style);
  const auto raw = reference(g.getGlyphBitmap(data, glyph), data->is2Bit, glyph.width, glyph.height, glyph.left,
                             glyph.top, glyphScale);
  const int left = std::floor(glyph.left * glyphScale), top = std::floor(-glyph.top * glyphScale);
  const int w = std::ceil((glyph.left + glyph.width) * glyphScale) - left;
  const int h = std::ceil((glyph.height - glyph.top) * glyphScale) - top;
  const int x0 = 30 + left;
  const int y0 =
      100 + std::lround(family.getData(EpdFontFamily::REGULAR)->ascender * scale * g.fontBaseScale(id)) + top;
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
        const bool m = aa && (darkness == 0 || darkness == 4 || (darkness == 1 && r == 1));
        const bool l = darkness == 0 && r == 2;
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
  for (float scale : {0.5f, 0.75f, 0.9f, 1.0f})
    for (auto style : {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::BOLD_ITALIC})
      for (char c = 'A'; c <= 'Z'; ++c) {
        const char text[] = {c, 0};
        checkGlyph(g, d, id, family, text, c, scale, style, false);
        checkGlyph(g, d, id, family, text, c, scale, style, true);
      }
  // Native HD body text reduces the raster by 2x even when its CSS scale is 1.
  // Include lowercase stems, curves and ascenders in the same plane oracle.
  for (auto style : {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::BOLD_ITALIC})
    for (char c : std::string("thnilomw")) {
      const char text[] = {c, 0};
      for (bool spaced : {false, true}) checkGlyph(g, d, id, family, text, c, 1.0f, style, spaced);
    }
  for (auto style :
       {EpdFontFamily::SMALL_CAPS, static_cast<EpdFontFamily::Style>(EpdFontFamily::BOLD | EpdFontFamily::SMALL_CAPS),
        static_cast<EpdFontFamily::Style>(EpdFontFamily::ITALIC | EpdFontFamily::SMALL_CAPS),
        static_cast<EpdFontFamily::Style>(EpdFontFamily::BOLD_ITALIC | EpdFontFamily::SMALL_CAPS)})
    for (bool spaced : {false, true})
      for (float scale : {0.75f, 1.0f}) checkGlyph(g, d, id, family, "a", 'a', scale, style, spaced);
  const auto all = static_cast<EpdFontFamily::Style>(EpdFontFamily::SMALL_CAPS | EpdFontFamily::ALL_SMALL_CAPS);
  for (bool spaced : {false, true})
    for (float scale : {.75f, 1.0f}) {
      checkGlyph(g, d, id, family, "A", 'A', scale, all, spaced);
      checkGlyph(g, d, id, family, "a", 'a', scale, all, spaced);
    }
}

// Thirteen 2x2 tiles cover every quarter-level average from 0 through 3.
// The literal expected sequence independently fixes endpoints, monotonicity,
// exact half ties, and their immediate neighbors. In particular, a 0.75-level
// hairline survives, while a 0.5 fringe does not always gain an extra ink level.
void checkCoverageTies(GfxRenderer& g, HalDisplay& d) {
  constexpr uint8_t expected[] = {0, 0, 0, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3};
  constexpr int width = 26, height = 2;
  std::array<uint8_t, width * height / 4> bitmap{};
  for (int total = 0; total <= 12; ++total)
    for (int pixel = 0; pixel < 4; ++pixel) {
      const int raw = std::clamp(total - pixel * 3, 0, 3);
      const int i = (pixel / 2) * width + total * 2 + pixel % 2;
      bitmap[i / 4] |= raw << (6 - 2 * (i % 4));
    }
  const EpdGlyph glyphs[] = {{width, height, width * 16, 0, height, uint16_t(bitmap.size()), 0},
                             {width, height, width * 16, 0, height, uint16_t(bitmap.size()), 0}};
  const EpdUnicodeInterval intervals[] = {{'A', 'A', 0}, {0x1f130, 0x1f130, 1}};
  EpdFontData data{};
  data.bitmap = bitmap.data();
  data.glyph = glyphs;
  data.intervals = intervals;
  data.intervalCount = 2;
  data.is2Bit = true;
  data.ascender = height;
  data.advanceY = 4;
  EpdFont font(&data);
  EpdFontFamily family(&font);
  g.insertScaledFont(4, family, .5f);
  for (const char* text : {"A", "\xf0\x9f\x84\xb0"}) {
    // ASCII takes the scaled cache; the non-BMP alias exercises direct sampling.
    for (int repeat = 0; repeat < 2; ++repeat) {
      Plane lsb{}, msb{};
      g.setTextDarkness(0);
      g.setRenderMode(GfxRenderer::BW);
      d.fb.fill(255);
      g.beginGrayCapture(lsb.data(), msb.data());
      g.drawTextSpaced(4, 30, 100, text, true, EpdFontFamily::REGULAR, 1.0f, 0);
      g.endGrayCapture();
      bool ok = true;
      for (int x = 0; x <= 12; ++x) {
        const int raw = bit(d.fb, 30 + x, 100) ? 0 : bit(msb, 30 + x, 100) ? (bit(lsb, 30 + x, 100) ? 2 : 1) : 3;
        ok &= raw == expected[x];
      }
      ++checks;
      if (!ok) {
        ++failures;
        std::cerr << "Area coverage exact ties/neighbors/endpoints fail\n";
      }
    }
  }
  for (bool spaced : {false, true}) {
    checkGlyph(g, d, 4, family, "A", 'A', 1.0f, EpdFontFamily::REGULAR, spaced);
    checkGlyph(g, d, 4, family, "\xf0\x9f\x84\xb0", 0x1f130, 1.0f, EpdFontFamily::REGULAR, spaced);
  }
  g.removeFont(4);
}

// Cropping is a storage choice, not a placement choice. Adding transparent
// pixels while compensating the bearing must not move or reshape any ink.
void checkPaddingInvariance(GfxRenderer& g, HalDisplay& d) {
  for (bool twoBit : {false, true}) {
    for (int left : {-3, -1, 0, 2}) {
      const int top = 9, w = 9, h = 11;
      std::array<Plane, 3> original;
      for (float scale : {.5f, .75f, .9f, 1.2f, 1.6f}) {
        for (int padding = 0; padding <= 2; ++padding) {
          const int width = w + padding, height = h + padding;
          std::vector<uint8_t> bitmap((width * height * (twoBit ? 2 : 1) + 7) / 8);
          for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
              // N diagonal, top bar, thin vertical, and a descender.
              const int raw = (x == 1 || x == 7 || x == y % w || y == 1) ? 3 : (twoBit && (x == (y + 1) % w)) ? 1 : 0;
              const int i = (y + padding) * width + x + padding;
              if (twoBit)
                bitmap[i / 4] |= raw << (6 - 2 * (i % 4));
              else if (raw)
                bitmap[i / 8] |= 1 << (7 - i % 8);
            }
          const EpdGlyph glyph = {uint8_t(width),         uint8_t(height),         160, int16_t(left - padding),
                                  int16_t(top + padding), uint16_t(bitmap.size()), 0};
          const EpdUnicodeInterval interval = {65, 65, 0};
          EpdFontData data{};
          data.bitmap = bitmap.data();
          data.glyph = &glyph;
          data.intervals = &interval;
          data.intervalCount = 1;
          data.is2Bit = twoBit;
          data.ascender = 12;
          data.advanceY = 16;
          EpdFont font(&data);
          g.insertFont(5, EpdFontFamily(&font));
          g.setTextDarkness(0);
          int plane = 0;
          for (auto mode : {GfxRenderer::BW, GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
            g.setRenderMode(mode);
            d.fb.fill(mode == GfxRenderer::BW ? 255 : 0);
            g.drawTextSpaced(5, 30, 100, "A", true, EpdFontFamily::REGULAR, scale, 0);
            if (padding == 0)
              original[plane] = d.fb;
            else {
              ++checks;
              if (d.fb != original[plane]) {
                if (failures < 8)
                  std::cerr << "Padding changes ink: bpp=" << (twoBit ? 2 : 1) << " left=" << left << " scale=" << scale
                            << " pad=" << padding << " plane=" << plane << '\n';
                ++failures;
              }
            }
            ++plane;
          }
          if (scale == .75f) {
            // Legacy (untracked) small caps use EpdFont::getTextBounds for
            // centering and truncation; its box must contain the actual ink.
            g.setRenderMode(GfxRenderer::BW);
            d.fb.fill(255);
            g.drawText(5, 30, 100, "a", true, EpdFontFamily::SMALL_CAPS);
            int measuredWidth, measuredHeight;
            font.getTextDimensions("a", &measuredWidth, &measuredHeight, true);
            int minx = 480, maxx = -1, miny = 800, maxy = -1;
            for (int y = 0; y < 800; ++y)
              for (int x = 0; x < 480; ++x)
                if (!bit(d.fb, x, y)) {
                  minx = std::min(minx, x);
                  maxx = std::max(maxx, x);
                  miny = std::min(miny, y);
                  maxy = std::max(maxy, y);
                }
            ++checks;
            if (maxx - minx + 1 > measuredWidth || maxy - miny + 1 > measuredHeight) {
              ++failures;
              std::cerr << "Legacy small-caps bounds miss ink\n";
            }
            // Explicit scaling and a synthesized font must include partial
            // edge pixels too, including negative bearings.
            g.insertScaledFont(6, EpdFontFamily(&font), scale);
            for (bool alias : {false, true}) {
              d.fb.fill(255);
              if (alias)
                g.drawText(6, 30, 100, "A", true);
              else
                g.drawTextScaled(5, 30, 100, "A", true, EpdFontFamily::REGULAR, scale);
              minx = 480;
              maxx = -1;
              for (int y = 0; y < 800; ++y)
                for (int x = 0; x < 480; ++x)
                  if (!bit(d.fb, x, y)) {
                    minx = std::min(minx, x);
                    maxx = std::max(maxx, x);
                  }
              const int measured =
                  alias ? g.getTextWidth(6, "A") : g.getTextWidthScaled(5, "A", EpdFontFamily::REGULAR, scale);
              ++checks;
              if (maxx - minx + 1 > measured) {
                ++failures;
                std::cerr << "Scaled width misses ink\n";
              }
            }
            g.removeFont(6);
          }
          g.removeFont(5);
        }
      }
    }
  }
  g.setRenderMode(GfxRenderer::BW);
}
// Legacy scaled entry points must share the EPUB path's shaping and bounds.
// Cover ligatures, negative kerning, combining marks, small caps, and cancelling
// a synthesized font scale with a CSS scale (which must not reapply the base).
void checkScaledEntryPoints(GfxRenderer& g, HalDisplay& d, int id, const EpdFontFamily& font) {
  g.setTextDarkness(0);
  for (auto style : {EpdFontFamily::REGULAR, EpdFontFamily::ITALIC, EpdFontFamily::SMALL_CAPS})
    for (const char* text : {"AV office fi", "e\xcc\x81 cafe", "jfj fffi", "abc 123"})
      for (float base : {.75f, .9f, 1.2f}) {
        g.insertScaledFont(6, font, base * g.fontBaseScale(id));
        for (float residual : {1.0f, 1.0f / base})
          for (auto mode : {GfxRenderer::BW, GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
            g.setRenderMode(mode);
            const int clear = mode == GfxRenderer::BW ? 255 : 0;
            d.fb.fill(clear);
            g.drawTextSpaced(6, 30, 100, text, true, style, residual, 0);
            const auto expected = d.fb;
            d.fb.fill(clear);
            g.drawTextScaled(6, 30, 100, text, true, style, residual);
            bool ok = d.fb == expected && g.getTextWidthScaled(6, text, style, residual) ==
                                              g.getTextWidthSpaced(6, text, style, residual, 0);
            if (residual == 1.0f) {
              d.fb.fill(clear);
              g.drawText(6, 30, 100, text, true, style);
              ok &= d.fb == expected && g.getTextWidth(6, text, style) == g.getTextWidthSpaced(6, text, style, 1, 0) &&
                    g.getTextAdvanceX(6, text, style) == g.getTextAdvanceXSpaced(6, text, style, 1, 0);
              d.fb.fill(clear);
              g.drawTextScaled(id, 30, 100, text, true, style, base);
              ok &= d.fb == expected;
            }
            ++checks;
            if (!ok) {
              ++failures;
              std::cerr << "Scaled entry point diverged\n";
            }
          }
        g.removeFont(6);
      }
  g.setRenderMode(GfxRenderer::BW);
}
void checkLocalFontMetrics(GfxRenderer& g, HalDisplay& d, int id) {
  g.setRenderMode(GfxRenderer::BW);
  for (float scale : {.5f, .75f, .9f, 1.0f, 1.2f})
    for (auto style : {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::BOLD_ITALIC,
                       EpdFontFamily::SMALL_CAPS})
      for (int tracking : {-8, 0, 8})
        for (const char* text : {"HIN", "AVf", "office", "cafe\xcc\x81", "Agj", "P. P,"}) {
          d.fb.fill(255);
          g.drawTextSpaced(id, 40, 100, text, true, style, scale, tracking);
          int minx = 480, maxx = -1;
          for (int y = 0; y < 800; ++y)
            for (int x = 0; x < 480; ++x)
              if (!bit(d.fb, x, y)) {
                minx = std::min(minx, x);
                maxx = std::max(maxx, x);
              }
          const int measured = g.getTextWidthSpaced(id, text, style, scale, tracking);
          ++checks;
          if (maxx < minx || maxx - minx + 1 > measured) {
            ++failures;
            std::cerr << "Local-font measurement misses visible ink: " << text << " scale=" << scale << '\n';
          }
        }
}
void checkScanMetrics(GfxRenderer& g, const EpdFontFamily& font) {
  g.insertScaledFont(6, font, .75f);
  rendererTest::setScanning(true);
  const auto style = EpdFontFamily::REGULAR;
  bool ok = g.getTextWidthSpaced(6, nullptr, style, 1, 0) == 0 &&
            g.getTextAdvanceXSpaced(6, nullptr, style, 1, 0) == 0 && g.getTextWidthSpaced(-1, "A", style, 1, 0) == 0 &&
            g.getTextAdvanceXSpaced(-1, "A", style, 1, 0) == 0 && rendererTest::recordedCount() == 0;
  ok &= g.getTextWidth(6, "A") == 0 && g.getTextWidthScaled(6, "A", style, .75f) == 0 &&
        g.getTextAdvanceX(6, "A", style) == 0 && g.getTextWidthSpaced(6, "A", style, 1, 0) == 0 &&
        g.getTextAdvanceXSpaced(6, "A", style, 1, 0) == 0 && rendererTest::recordedCount() == 5;
  ++checks;
  if (!ok) {
    ++failures;
    std::cerr << "Scaled measurement changed font-cache scanning\n";
  }
  rendererTest::setScanning(false);
  g.removeFont(6);
}
}  // namespace

int runDownscaleChecks(GfxRenderer& g, HalDisplay& d, const EpdFontFamily& book, const EpdFontFamily& sans,
                       const char* path) {
  checkScanMetrics(g, book);
  checkCoverageTies(g, d);
  failures += runOverlapChecks(g, d, 1, book);
  failures += runOverlapChecks(g, d, 2, sans);
  checkPaddingInvariance(g, d);
  checkScaledEntryPoints(g, d, 1, book);
  checkScaledEntryPoints(g, d, 2, sans);
  sample(g, d, 1, "bookerly.pgm");
  checkFamily(g, d, 1, book);
  checkFamily(g, d, 2, sans);
  runDarkAaFontChecks(g, d, 1, "bookerly");
  runDarkAaFontChecks(g, d, 2, "sans");
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
  // A 2x three-em dash can exceed 255 source pixels. Exercise the complete
  // rasterizer, not only the file decoder, with wide dimensions and bearings.
  {
    constexpr int w = 267, h = 9;
    std::vector<uint8_t> bitmap((w * h * 2 + 7) / 8, 0xff);
    const EpdGlyph glyph{w, h, 4400, -130, 132, static_cast<uint16_t>(bitmap.size()), 0};
    const EpdUnicodeInterval interval{'A', 'A', 0};
    EpdFontData data{};
    data.bitmap = bitmap.data();
    data.glyph = &glyph;
    data.intervals = &interval;
    data.intervalCount = 1;
    data.is2Bit = true;
    data.wideGlyphs = true;
    data.ascender = 140;
    data.advanceY = 150;
    EpdFont font(&data);
    EpdFontFamily family(&font);
    g.insertScaledFont(4, family, .5f);
    for (float scale : {.5f, .75f, 1.0f, 1.2f})
      for (bool spaced : {false, true}) checkGlyph(g, d, 4, family, "A", 'A', scale, EpdFontFamily::REGULAR, spaced);
    g.removeFont(4);
  }
  if (path) {
    CpFontFixture fixture(path);
    g.insertScaledFont(3, fixture.family(), fixture.rasterScale());
    sample(g, d, 3, "sd-font.pgm");
    sampleNativeWeight(g, d, 3);
    checkLocalFontMetrics(g, d, 3);
    checkScaledEntryPoints(g, d, 3, fixture.family());
    checkFamily(g, d, 3, fixture.family());
    failures += runOverlapChecks(g, d, 3, fixture.family());
    runDarkAaFontChecks(g, d, 3, "sd-font");
    g.removeFont(3);
  }
  std::cout << "Scaled glyph coverage, capture, staged AA and cache checks=" << checks << " failures=" << failures
            << '\n';
  return failures ? 1 : 0;
}
