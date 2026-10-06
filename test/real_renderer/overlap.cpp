#include <GfxRenderer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>

namespace {
using Plane = std::array<uint8_t, 48000>;
struct Frame {
  Plane bw, lsb, msb;
  bool operator==(const Frame& other) const { return bw == other.bw && lsb == other.lsb && msb == other.msb; }
};
// Decode the SDK overlay contract, independently of renderer draw masks.
int ink(const Frame& frame, int byte, uint8_t bit) {
  if (frame.msb[byte] & bit) return (frame.lsb[byte] & bit) ? 2 : 1;
  return (frame.bw[byte] & bit) ? 0 : 3;
}
bool strongest(const Frame& result, const Frame& first, const Frame& second) {
  for (int i = 0; i < 48000; ++i) {
    if (result.lsb[i] & ~result.msb[i]) return false;
    for (int mask = 1; mask <= 128; mask <<= 1)
      if (ink(result, i, mask) != std::max(ink(first, i, mask), ink(second, i, mask))) return false;
  }
  return true;
}
template <typename Paint>
Frame capture(GfxRenderer& g, HalDisplay& d, Paint paint, bool staged = false) {
  Frame result;
  result.lsb.fill(0);
  result.msb.fill(0);
  g.setRenderMode(GfxRenderer::BW);
  d.fb.fill(255);
  if (!staged) g.beginGrayCapture(result.lsb.data(), result.msb.data());
  paint();
  g.endGrayCapture();
  result.bw = d.fb;
  if (staged) {
    g.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    d.fb.fill(0);
    paint();
    g.eraseOpaqueGlyphs(paint);
    result.lsb = d.fb;
    g.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    d.fb.fill(0);
    paint();
    g.eraseOpaqueGlyphs(paint);
    result.msb = d.fb;
  }
  return result;
}
}  // namespace

int runOverlapChecks(GfxRenderer& g, HalDisplay& d, int id, const EpdFontFamily& family) {
  int failures = 0, checks = 0;
  const auto savedOrientation = g.getOrientation();
  g.setOrientation(GfxRenderer::Portrait);
  // "fT" has actual overlapping outline pixels at ordinary tracking in both
  // Bookerly and Garamond. Separate calls model adjacent styled EPUB fragments.
  for (auto firstStyle : {EpdFontFamily::REGULAR, EpdFontFamily::ITALIC})
    for (auto secondStyle : {EpdFontFamily::REGULAR, EpdFontFamily::ITALIC})
      for (float scale : {.75f, 1.0f, 1.2f})
        for (uint8_t darkness = 0; darkness <= 4; ++darkness) {
          g.setTextDarkness(darkness);
          const float actualScale = scale * g.fontBaseScale(id);
          const auto f = family.getGlyph('f', firstStyle);
          const int secondX = 20 + ((int(std::lround(f.advanceX * actualScale)) +
                                     int(std::lround(family.getKerning('f', 'T', firstStyle) * actualScale)) + 8) >>
                                    4);
          auto first = [&] { g.drawTextSpaced(id, 20, 20, "f", true, firstStyle, scale, 0); };
          auto second = [&] { g.drawTextSpaced(id, secondX, 20, "T", true, secondStyle, scale, 0); };
          const Frame a = capture(g, d, first), b = capture(g, d, second);
          for (bool reversed : {false, true}) {
            auto both = [&] {
              if (reversed) {
                second();
                first();
              } else {
                first();
                second();
              }
            };
            const Frame composed = capture(g, d, both), staged = capture(g, d, both, true);
            bool ok = strongest(composed, a, b) && composed == staged;
            if (firstStyle == secondStyle) {
              const Frame oneRun =
                  capture(g, d, [&] { g.drawTextSpaced(id, 20, 20, "fT", true, firstStyle, scale, 0); });
              ok &= oneRun == composed;
            }
            ++checks;
            if (!ok) {
              ++failures;
              if (failures <= 5)
                std::cerr << "Overlap failed font=" << id << " styles=" << int(firstStyle) << ',' << int(secondStyle)
                          << " scale=" << scale << " darkness=" << int(darkness) << " reversed=" << reversed << '\n';
            }
          }
        }
  // Real reader decorations: underline/strike use drawLine, table borders use
  // drawRect. Each solid mark must win over AA in either paint order.
  for (auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted, GfxRenderer::LandscapeClockwise,
                           GfxRenderer::LandscapeCounterClockwise}) {
    g.setOrientation(orientation);
    g.setTextDarkness(0);
    for (float scale : {.75f, 1.0f, 1.2f}) {
      const float actualScale = scale * g.fontBaseScale(id);
      const int baseline = 30 + int(family.getData(EpdFontFamily::REGULAR)->ascender * actualScale + .5f);
      const int width = g.getTextWidthSpaced(id, "gypjfT", EpdFontFamily::REGULAR, scale, 0);
      auto text = [&] { g.drawTextSpaced(id, 30, 30, "gypjfT", true, EpdFontFamily::REGULAR, scale, 0); };
      for (int decoration = 0; decoration < 3; ++decoration) {
        auto mark = [&] {
          if (decoration == 0)
            g.drawLine(29, baseline + 1, 31 + width, baseline + 1, 1, true);
          else if (decoration == 1)
            g.drawLine(29, baseline - 5, 31 + width, baseline - 5, 1, true);
          else
            g.drawRect(30, baseline - 8, std::max(2, width), 10, true);
        };
        const Frame a = capture(g, d, text), b = capture(g, d, mark);
        for (bool reversed : {false, true}) {
          auto both = [&] {
            if (reversed) {
              mark();
              text();
            } else {
              text();
              mark();
            }
          };
          const Frame composed = capture(g, d, both);
          ++checks;
          if (!strongest(composed, a, b) || !(composed == capture(g, d, both, true))) {
            ++failures;
            if (failures <= 5)
              std::cerr << "Decoration overlap font=" << id << " kind=" << decoration << " scale=" << scale
                        << " reversed=" << reversed << '\n';
          }
        }
      }
    }
  }
  // The opaque finalization pass must neither repaint white backgrounds
  // nor lose an abort request that is no longer true on the next predicate call.
  if (id == 1) {
    d.fb.fill(0xA5);
    const Plane before = d.fb;
    g.eraseOpaqueGlyphs([&] {
      g.clearScreen();
      g.drawPixel(5, 5, false);
      g.drawLine(0, 0, 25, 0, false);
      g.fillRect(10, 10, 20, 20, false);
    });
    ++checks;
    if (d.fb != before || g.isErasingOpaqueGlyphs()) ++failures;
    for (bool interleaved : {false, true})
      for (int abortAt : {0, 1, 2, 3, 4}) {
        int predicates = 0, normalPasses = 0, erasePasses = 0;
        auto paint = [&](GfxRenderer::RenderMode) {
          if (g.isErasingOpaqueGlyphs())
            ++erasePasses;
          else
            ++normalPasses;
        };
        auto abort = [&] { return ++predicates == abortAt; };
        const auto result = interleaved ? g.renderGrayscalePlanesInterleaved(paint, abort)
                                        : g.renderGrayscalePlanesSequential(paint, abort);
        const bool complete = abortAt == 0;
        const int expectedNormal = complete || abortAt >= 3 ? 2 : 1;
        const int expectedErase = complete || abortAt == 4 ? 2 : abortAt == 1 ? 0 : 1;
        ++checks;
        if (result.aborted == complete || g.isErasingOpaqueGlyphs() || g.getRenderMode() != GfxRenderer::BW ||
            normalPasses != expectedNormal || erasePasses != expectedErase)
          ++failures;
      }
  }
  // Exhaustive source-coverage pairs, including opaque black after gray and
  // gray after opaque black. The four glyphs are uniform 4x4 coverage patches.
  if (id == 1) {
    const uint8_t bitmap[] = {0, 0, 0, 0, 0x55, 0x55, 0x55, 0x55, 0xAA, 0xAA, 0xAA, 0xAA, 255, 255, 255, 255};
    const EpdGlyph glyphs[] = {
        {4, 4, 64, 0, 4, 4, 0}, {4, 4, 64, 0, 4, 4, 4}, {4, 4, 64, 0, 4, 4, 8}, {4, 4, 64, 0, 4, 4, 12}};
    const EpdUnicodeInterval interval = {65, 68, 0};
    EpdFontData data{};
    data.bitmap = bitmap;
    data.glyph = glyphs;
    data.intervals = &interval;
    data.intervalCount = 1;
    data.is2Bit = true;
    data.ascender = 8;
    data.advanceY = 10;
    EpdFont font(&data);
    g.insertFont(11, EpdFontFamily(&font));
    for (auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted, GfxRenderer::LandscapeClockwise,
                             GfxRenderer::LandscapeCounterClockwise}) {
      g.setOrientation(orientation);
      for (float scale : {.75f, 1.0f, 1.2f})
        for (uint8_t darkness = 0; darkness <= 4; ++darkness) {
          g.setTextDarkness(darkness);
          for (char a = 'A'; a <= 'D'; ++a)
            for (char b = 'A'; b <= 'D'; ++b) {
              const char firstText[] = {a, 0}, secondText[] = {b, 0};
              auto first = [&] { g.drawTextSpaced(11, -1, -4, firstText, true, EpdFontFamily::REGULAR, scale, 0); };
              auto second = [&] { g.drawTextSpaced(11, -1, -4, secondText, true, EpdFontFamily::REGULAR, scale, 0); };
              const Frame firstFrame = capture(g, d, first), secondFrame = capture(g, d, second);
              auto both = [&] {
                first();
                second();
              };
              const Frame combined = capture(g, d, both);
              ++checks;
              if (!strongest(combined, firstFrame, secondFrame) || !(combined == capture(g, d, both, true))) ++failures;
            }
        }
    }
    // Four-pixel gray patch crossed by black rules: this reproduces the
    // underline bug directly. Exercise both per-pixel and optimized span writes,
    // rotations, partial-byte clipping, darkness levels, and both draw orders.
    for (auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted, GfxRenderer::LandscapeClockwise,
                             GfxRenderer::LandscapeCounterClockwise}) {
      g.setOrientation(orientation);
      for (float scale : {.75f, 1.0f, 1.2f})
        for (uint8_t darkness = 0; darkness <= 4; ++darkness) {
          g.setTextDarkness(darkness);
          for (bool clipped : {false, true}) {
            const int x = clipped ? -1 : 20, y = clipped ? -4 : 20;
            const int top = y + int(8 * scale + .5f) + int(std::floor(-4 * scale));
            const int size = int(std::ceil(4 * scale));
            auto text = [&] { g.drawTextSpaced(11, x, y, "B", true, EpdFontFamily::REGULAR, scale, 0); };
            for (int primitive = 0; primitive < 6; ++primitive) {
              auto mark = [&] {
                switch (primitive) {
                  case 0:
                    g.drawPixel(x + 1, top, true);
                    break;
                  case 1:
                    g.drawLine(x, top, x + size - 1, top, true);
                    break;
                  case 2:
                    g.drawLine(x + 1, top, x + 1, top + size - 1, true);
                    break;
                  case 3:
                    g.drawLine(x, top, x + size - 1, top + size - 1, true);
                    break;
                  case 4:
                    g.fillRect(x + 1, top, 2, 2, true);
                    break;
                  case 5:
                    g.drawRect(x, top, size, size, true);
                    break;
                }
              };
              const Frame a = capture(g, d, text), b = capture(g, d, mark);
              for (bool reversed : {false, true}) {
                auto both = [&] {
                  if (reversed) {
                    mark();
                    text();
                  } else {
                    text();
                    mark();
                  }
                };
                const Frame composed = capture(g, d, both);
                ++checks;
                if (!strongest(composed, a, b) || !(composed == capture(g, d, both, true))) {
                  ++failures;
                  if (failures <= 5)
                    std::cerr << "Primitive overlap kind=" << primitive << " scale=" << scale
                              << " darkness=" << int(darkness) << " clipped=" << clipped << " reversed=" << reversed
                              << '\n';
                }
              }
            }
          }
        }
    }
    g.removeFont(11);
    // Dense space metrics must round once after applying raster density.
    // 201/16 raster pixels at density2 =6.28125 logical pixels, not7.
    const EpdGlyph spaceGlyph{0, 0, 201, 0, 0, 0, 0};
    const EpdUnicodeInterval spaceInterval{' ', ' ', 0};
    EpdFontData spaceData{};
    spaceData.glyph = &spaceGlyph;
    spaceData.intervals = &spaceInterval;
    spaceData.intervalCount = 1;
    EpdFont spaceFont(&spaceData);
    g.insertScaledFont(12, EpdFontFamily(&spaceFont), .5f);
    ++checks;
    if (g.getSpaceWidth(12) != 6 || g.getSpaceAdvance(12, 0, 0, EpdFontFamily::REGULAR) != 6 ||
        g.getTextAdvanceX(12, " ", EpdFontFamily::REGULAR) != 6)
      ++failures;
    g.removeFont(12);
    const EpdGlyph truncationGlyphs[] = {{8, 8, 201, 0, 8, 16, 0}, {4, 2, 64, 0, 2, 2, 16}};
    const EpdUnicodeInterval truncationIntervals[] = {{'A', 'A', 0}, {0x2026, 0x2026, 1}};
    EpdFontData truncationData{};
    truncationData.glyph = truncationGlyphs;
    truncationData.intervals = truncationIntervals;
    truncationData.intervalCount = 2;
    EpdFont truncationFont(&truncationData);
    g.insertScaledFont(13, EpdFontFamily(&truncationFont), .5f);
    const int fittingWidth = g.getTextWidth(13, "AAAA");
    ++checks;
    if (g.truncatedText(13, "AAAA", fittingWidth) != "AAAA") ++failures;
    g.removeFont(13);
  }
  g.setOrientation(savedOrientation);
  g.setRenderMode(GfxRenderer::BW);
  g.setTextDarkness(0);
  std::cout << "Overlap max-coverage, staged/captured and styled-fragment checks=" << checks << " failures=" << failures
            << '\n';
  return failures;
}
