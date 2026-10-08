#include <GfxRenderer.h>

#include <array>
#include <chrono>
#include <iostream>
#include <vector>

namespace {
using Plane = std::array<uint8_t, 48000>;
using Clock = std::chrono::steady_clock;
long long micros(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
}
void paintPage(GfxRenderer& g, float scale) {
  const int width = g.getScreenWidth(), height = g.getScreenHeight();
  int row = 0;
  for (int y = 30; y < height - 45; y += 35, ++row) {
    const auto style = static_cast<EpdFontFamily::Style>(row % 4);
    const int font = 1 + (row % 2);
    g.drawTextSpaced(font, 25, y, row % 2 ? "A clear path: AV office fi 123" : "CAPITALS HNT / e\xcc\x81 STRASSE", true,
                    style, scale, row % 3 == 0 ? -8 : 4);
    if (row % 4 == 0) g.drawLine(24, y + 14, width - 25, y + 14, 1, true);
  }
  g.drawRect(16, 20, width - 32, height - 40, true);
}
void paintLiveChrome(GfxRenderer& g) {
  // Deliberately overlap glyph fringes: live UI must erase their retained gray
  // selectors as well as repainting the BW framebuffer.
  g.fillRect(0, 20, g.getScreenWidth(), 26, false);
  g.fillRect(18, 26, 19, 7, true);
  g.drawLine(8, 44, g.getScreenWidth() - 8, 44, 1, true);
}
}  // namespace

int runPreparedGrayscaleChecks(GfxRenderer& g, HalDisplay& d) {
  int failures = 0, checks = 0;
  const auto savedOrientation = g.getOrientation();
  const auto savedDarkness = g.getTextDarkness();
  Plane capturedL{}, capturedM{};
  long long capturedPrepareUs = 0, legacyBwUs = 0, legacyReplayUs = 0;
  for (auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted, GfxRenderer::LandscapeClockwise,
                           GfxRenderer::LandscapeCounterClockwise}) {
    g.setOrientation(orientation);
    for (uint8_t darkness = 0; darkness <= 4; ++darkness)
      for (float scale : {.75f, 1.0f, 1.2f}) {
        g.setTextDarkness(darkness);
        g.setRenderMode(GfxRenderer::BW);
        d.recordGray = true;
        d.grayEvents.clear();
        d.fb.fill(255);
        capturedL.fill(0);
        capturedM.fill(0);
        g.invalidateScaledGlyphCache();
        auto start = Clock::now();
        g.beginGrayCapture(capturedL.data(), capturedM.data());
        paintPage(g, scale);
        g.endGrayCapture();
        capturedPrepareUs += micros(start);
        const Plane capturedBw = d.fb;
        ++checks;
        if (!d.grayEvents.empty()) ++failures;  // preparation never reaches controller RAM

        g.invalidateScaledGlyphCache();
        d.fb.fill(255);
        start = Clock::now();
        paintPage(g, scale);
        legacyBwUs += micros(start);
        const Plane legacyBw = d.fb;
        start = Clock::now();
        g.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        d.fb.fill(0);
        paintPage(g, scale);
        g.eraseOpaqueGlyphs([&] { paintPage(g, scale); });
        const Plane legacyL = d.fb;
        g.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        d.fb.fill(0);
        paintPage(g, scale);
        g.eraseOpaqueGlyphs([&] { paintPage(g, scale); });
        const Plane legacyM = d.fb;
        legacyReplayUs += micros(start);

        g.setRenderMode(GfxRenderer::BW);
        Plane freshL{}, freshM{}, retainedL = capturedL, retainedM = capturedM;
        d.fb.fill(255);
        g.beginGrayCapture(freshL.data(), freshM.data());
        paintPage(g, scale);
        paintLiveChrome(g);
        g.endGrayCapture();
        const Plane freshChromeBw = d.fb;
        d.fb = capturedBw;
        g.beginGrayCapture(retainedL.data(), retainedM.data());
        paintLiveChrome(g);
        g.endGrayCapture();
        ++checks;
        if (d.fb != freshChromeBw || retainedL != freshL || retainedM != freshM || !d.grayEvents.empty()) {
          ++failures;
          std::cerr << "Retained chrome differs: orientation=" << int(orientation) << " darkness=" << int(darkness)
                    << " scale=" << scale << '\n';
        }
        ++checks;
        if (capturedBw != legacyBw || capturedL != legacyL || capturedM != legacyM) {
          ++failures;
          std::cerr << "Prepared page differs: orientation=" << int(orientation) << " darkness=" << int(darkness)
                    << " scale=" << scale << '\n';
        }

        // Model a completed BW refresh. Upload must not redraw or use its write
        // framebuffer as scratch, and the bytes crossing the HAL must be exactly
        // the production renderer's previous staged output.
        d.fb = capturedBw;
        d.grayEvents.clear();
        d.grayKeptPowered = false;
        const auto result = g.displayPreparedGrayscale(capturedL.data(), capturedM.data(), [] { return false; });
        ++checks;
        if (result.aborted || d.grayEvents != std::vector<char>{'L', 'M', 'G', 'C'} || !d.grayKeptPowered ||
            d.fb != capturedBw || d.uploadedLsb != legacyL || d.uploadedMsb != legacyM ||
            capturedL != legacyL || capturedM != legacyM || g.getRenderMode() != GfxRenderer::BW)
          ++failures;
      }
  }

  const Plane before = d.fb, beforeL = capturedL, beforeM = capturedM;
  for (int abortAt : {1, 2, 3}) {
    d.grayEvents.clear();
    int predicates = 0;
    const auto result = g.displayPreparedGrayscale(capturedL.data(), capturedM.data(),
                                                    [&] { return ++predicates == abortAt; });
    std::vector<char> expected;
    if (abortAt > 1) expected.push_back('L');
    if (abortAt > 2) expected.push_back('M');
    expected.push_back('C');
    ++checks;
    if (!result.aborted || d.grayEvents != expected || d.fb != before || capturedL != beforeL ||
        capturedM != beforeM || g.getRenderMode() != GfxRenderer::BW)
      ++failures;
  }
  for (bool missingLsb : {false, true}) {
    d.grayEvents.clear();
    int predicates = 0;
    const auto result = g.displayPreparedGrayscale(missingLsb ? nullptr : capturedL.data(),
                                                    missingLsb ? capturedM.data() : nullptr,
                                                    [&] { ++predicates; return false; });
    ++checks;
    if (!result.aborted || predicates != 0 || d.grayEvents != std::vector<char>{'C'} || d.fb != before) ++failures;
  }
  d.recordGray = false;
  d.grayEvents.clear();
  g.setRenderMode(GfxRenderer::BW);
  g.setOrientation(savedOrientation);
  g.setTextDarkness(savedDarkness);
  std::cout << "prepared grayscale page/transfer/cancellation checks=" << checks << " failures=" << failures << '\n';
  // Host-only timings, never an assertion or an estimate of panel milliseconds.
  // Both initial BW latency and all preparation work are reported: capture may
  // cost more before the first refresh even when it removes the later replay.
  std::cout << "host preparation totals us: capture=" << capturedPrepareUs << " legacy_bw=" << legacyBwUs
            << " legacy_replay=" << legacyReplayUs << " legacy_total=" << legacyBwUs + legacyReplayUs << '\n';
  return failures;
}
