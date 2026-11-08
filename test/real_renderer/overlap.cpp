#include <EpdOutlineFontCallbacks.h>
#include <GfxRenderer.h>
#include <PublisherBox.h>

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
            // Separate public draw calls start on integer origins. They equal a
            // continuous run only when the boundary itself is integral; fractional
            // boundaries are checked against a pixel oracle in fractional.cpp.
            const int boundary = int(std::lround(f.advanceX * actualScale)) +
                                 int(std::lround(family.getKerning('f', 'T', firstStyle) * actualScale));
            if (firstStyle == secondStyle && boundary % 16 == 0) {
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
  // Run the actual page-box paint helper through both production AA paths.
  // The final background (including a nested white replacement) and glyph ink
  // must compose identically, with solid dither pixels winning over gray edges.
  for (auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted, GfxRenderer::LandscapeClockwise,
                           GfxRenderer::LandscapeCounterClockwise}) {
    g.setOrientation(orientation);
    g.setTextDarkness(0);
    for (uint16_t shade : {uint16_t(232), uint16_t(128), uint16_t(255)}) {
      for (int nested : {0, 1, 2}) {
        auto boxes = [&](auto emit) {
          emit(20, 20, 230, 110, shade, 1, 2);
          if (nested) emit(40, 35, 150, 65, uint16_t(255), 0, 0);
        };
        auto background = [&] {
          drawPublisherBox(g, 20, 20, 230, 110, shade, 1, 2);
          if (nested == 1) drawPublisherBox(g, 40, 35, 150, 65, 255, 0, 0);
          if (nested == 2) clearPublisherImageBackdrop(g, 40, 35, 150, 65);
          finalizePublisherBoxes(g, boxes, [] { return false; });
        };
        auto text = [&] { g.drawTextSpaced(id, 30, 40, "fT office", true, EpdFontFamily::ITALIC, .75f, 0); };
        auto page = [&] {
          background();
          text();
        };
        const Frame bg = capture(g, d, background), glyphs = capture(g, d, text);
        const Frame captured = capture(g, d, page), staged = capture(g, d, page, true);
        ++checks;
        if (!(captured == staged) || !strongest(captured, bg, glyphs)) {
          ++failures;
          std::cerr << "Publisher box AA mismatch font=" << id << " shade=" << shade << " nested=" << nested
                    << " orientation=" << int(orientation) << '\n';
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
  // Independent authored metrics make caps measurement mistakes visible: lower
  // x-height3, capitals10, designed small capitals6, and distinct space kerning.
  {
    const EpdOutlineFontCallbacks callbacks{
        [](void*, uint32_t cp, uint8_t mode) -> uint16_t {
          if ((mode == 1 && cp == 'a') || (mode == 2 && cp == 'A')) return 4;
          if (mode) return 0;
          return cp == 'A' || cp == 'H' ? 1 : cp == 'a' ? 2 : cp == ' ' ? 3 : 0;
        },
        [](void*, uint16_t gid) -> EpdGlyphRef {
          if (!gid || gid > 4) return {};
          const uint16_t h = gid == 1 ? 10 : gid == 2 ? 3 : gid == 4 ? 6 : 0;
          return {nullptr, uint16_t(gid == 3 ? 160 : 192), gid, uint16_t(h ? 8 : 0), h, 0, int16_t(h), true};
        },
        nullptr,
        [](void*, uint16_t a, uint16_t b) -> int16_t {
          return a == 4 && b == 3 ? -16 : a == 3 && b == 4 ? -32 : a == 4 && b == 4 ? -48 : a == 1 && b == 1 ? -12 : 0;
        },
        nullptr};
    EpdFontData data{};
    data.outline = &callbacks;
    EpdFont font(&data);
    g.insertFont(14, EpdFontFamily(&font));
    int above = 0, below = 0;
    for (auto style : {EpdFontFamily::SMALL_CAPS, EpdFontFamily::ALL_SMALL_CAPS}) {
      ++checks;
      if (!g.getTextInkMetrics(14, "a", style, &above, &below) || above != 6 || below != 0 ||
          g.getSpaceAdvance(14, 'a', 'a', style) != 7 || g.getKerning(14, 'a', 'a', style) != -3)
        ++failures;
    }
    ++checks;
    if (!g.getTextInkMetrics(14, "A", EpdFontFamily::ALL_SMALL_CAPS, &above, &below) || above != 6) ++failures;
    ++checks;
    if (g.getTextInkMetrics(14, " ", EpdFontFamily::REGULAR, &above, &below)) ++failures;
    // Synthetic 0.75 scaling keeps negative fixed-point kerning negative.
    ++checks;
    const int advance = g.getTextAdvanceX(14, "hh", EpdFontFamily::SMALL_CAPS);
    const int inkWidth = g.getTextWidth(14, "hh", EpdFontFamily::SMALL_CAPS);
    // Spaced width includes the full cursor advance, including trailing sidebearing.
    const int spacedWidth = g.getTextWidthSpaced(14, "hh", EpdFontFamily::SMALL_CAPS, 1, 0);
    if (advance != 17 || inkWidth != 14 || spacedWidth != 17) {
      ++failures;
      std::cerr << "Synthetic caps negative kerning: advance=" << advance << " ink=" << inkWidth
                << " spaced=" << spacedWidth << '\n';
    }
    g.removeFont(14);
  }
  // The PSRAM path must match the old plane pixels, preserve BOTH host
  // framebuffer slots, finish all rendering before waiting, and defer every
  // controller write until after that wait. Exercise styles, rotations,
  // darkness, publisher fills and overlapping opaque ink through real code.
  Plane bufferedLsb{}, bufferedMsb{};
  for (auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted, GfxRenderer::LandscapeClockwise,
                           GfxRenderer::LandscapeCounterClockwise}) {
    g.setOrientation(orientation);
    for (uint8_t darkness = 0; darkness <= 4; ++darkness) {
      g.setTextDarkness(darkness);
      for (auto style :
           {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::BOLD_ITALIC}) {
        auto paint = [&] {
          drawPublisherBox(g, 10, 10, 270, 110, 232, 1, 2);
          g.drawTextSpaced(id, 30, 40, "fT office gypj", true, style, .75f, 0);
          g.drawLine(20, 55, 130, 55, 1, true);
        };
        const Frame expected = capture(g, d, paint, true);
        d.fb.fill(0xA5);
        const Plane saved = d.fb;
        d.trace = true;
        d.events.clear();
        const auto result = g.renderGrayscalePlanesInterleaved(
            [&](GfxRenderer::RenderMode mode) {
              d.events.push_back(10 + int(mode) * 2 + (g.isErasingOpaqueGlyphs() ? 1 : 0));
              paint();
            },
            [] { return false; }, bufferedLsb.data(), bufferedMsb.data());
        const std::vector<int> order = {12,
                                        13,
                                        14,
                                        15,
                                        HalDisplay::Wait,
                                        HalDisplay::CopyLsb,
                                        HalDisplay::CopyMsb,
                                        HalDisplay::Gray,
                                        HalDisplay::Cleanup};
        ++checks;
        if (result.aborted || d.events != order || d.fb != saved || g.getFrameBuffer() != d.fb.data() ||
            g.getRenderMode() != GfxRenderer::BW || g.isErasingOpaqueGlyphs() || d.copiedLsb != expected.lsb ||
            d.copiedMsb != expected.msb) {
          ++failures;
          std::cerr << "Buffered AA mismatch font=" << id << " orientation=" << int(orientation)
                    << " darkness=" << int(darkness) << " style=" << int(style) << '\n';
        }
        d.trace = false;
      }
    }
  }
  // Cancellation at either normal/opaque render gate, or while waiting, must
  // latch even a transient request, drain exactly once and upload nothing.
  for (int abortAt = 1; abortAt <= 5; ++abortAt) {
    int predicates = 0;
    d.fb.fill(0x5A);
    const Plane saved = d.fb;
    d.trace = true;
    d.events.clear();
    const auto result = g.renderGrayscalePlanesInterleaved([&](GfxRenderer::RenderMode) { g.drawPixel(20, 20, true); },
                                                           [&] { return ++predicates == abortAt; }, bufferedLsb.data(),
                                                           bufferedMsb.data());
    ++checks;
    if (!result.aborted || d.events != std::vector<int>{HalDisplay::Wait, HalDisplay::Cleanup} || d.fb != saved ||
        g.getFrameBuffer() != d.fb.data() || g.getRenderMode() != GfxRenderer::BW || g.isErasingOpaqueGlyphs())
      ++failures;
    d.trace = false;
  }
  // Navigation can arrive during the synchronous SPI upload too. Restore the
  // controller baseline and skip the gray waveform after either upload.
  for (auto abortDuring : {HalDisplay::CopyLsb, HalDisplay::CopyMsb}) {
    d.fb.fill(0x3C);
    const Plane saved = d.fb;
    d.trace = true;
    d.events.clear();
    const auto result = g.renderGrayscalePlanesInterleaved(
        [&](GfxRenderer::RenderMode) { g.drawPixel(20, 20, true); },
        [&] { return !d.events.empty() && d.events.back() == abortDuring; }, bufferedLsb.data(), bufferedMsb.data());
    std::vector<int> expected = {HalDisplay::Wait, HalDisplay::CopyLsb};
    if (abortDuring == HalDisplay::CopyMsb) expected.push_back(HalDisplay::CopyMsb);
    expected.push_back(HalDisplay::Cleanup);
    ++checks;
    if (!result.aborted || d.events != expected || d.fb != saved || g.getFrameBuffer() != d.fb.data() ||
        g.getRenderMode() != GfxRenderer::BW || g.isErasingOpaqueGlyphs()) {
      ++failures;
      std::cerr << "Buffered AA upload cancellation failed at " << int(abortDuring) << '\n';
    }
    d.trace = false;
  }
  // The production presenter consumes a refresh override exactly once and
  // transfers the new target exactly once, whether AA or its BW fallback wins.
  d.simulateText = true;
  for (int scenario = 0; scenario < 5; ++scenario) {
    d.acceptText = scenario != 1;
    d.events.clear();
    auto* target = g.getFrameBuffer();
    memset(target, 0x53 + scenario, 48000);
    const auto requested = HalDisplay::FAST_REFRESH;
    if (scenario == 2) g.setNextDisplayRefreshMode(HalDisplay::HALF_REFRESH);
    if (scenario == 3) g.setNextDisplayRefreshMode(HalDisplay::FULL_REFRESH);
    const bool result =
        g.displayTextAaOrBw(requested, scenario == 4 ? nullptr : bufferedLsb.data(), bufferedMsb.data());
    const auto expectedMode = scenario == 2   ? HalDisplay::HALF_REFRESH
                              : scenario == 3 ? HalDisplay::FULL_REFRESH
                                              : HalDisplay::FAST_REFRESH;
    ++checks;
    if (result != (scenario == 0) ||
        d.events != std::vector<int>{scenario == 0 ? HalDisplay::TextAa : HalDisplay::Bw} ||
        d.displayedBuffer != target || g.getFrameBuffer() == target || g.getFrameBuffer() != d.getFrameBuffer() ||
        d.submittedMode != expectedMode || g.hasRefreshOverridePending() || d.displayedBuffer[0] != 0x53 + scenario)
      ++failures;
  }
  d.simulateText = false;
  d.writeBuffer = d.fb.data();
  // Resynchronize the renderer using its public presentation operation.
  g.triggerDisplay(HalDisplay::FAST_REFRESH);
  g.setOrientation(savedOrientation);
  g.setRenderMode(GfxRenderer::BW);
  g.setTextDarkness(0);
  std::cout << "Overlap max-coverage, staged/captured and styled-fragment checks=" << checks << " failures=" << failures
            << '\n';
  return failures;
}
