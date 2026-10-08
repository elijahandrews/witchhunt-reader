#include <GfxRenderer.h>

#include <cassert>
#include <fstream>
#include <iostream>

namespace {
void screenshot(const char* name, const std::array<uint8_t, 48000>& pixels) {
  std::ofstream out(name, std::ios::binary);
  out << "P5\n480 800\n255\n";
  for (int y = 0; y < 800; ++y)
    for (int x = 0; x < 480; ++x) {
      const uint8_t value = pixels[(479 - x) * 100 + y / 8] & (0x80 >> (y % 8)) ? 255 : 0;
      out.write(reinterpret_cast<const char*>(&value), 1);
    }
}
}  // namespace

int runDarkModeChecks(GfxRenderer& g, HalDisplay& d) {
  assert(!g.isDarkMode());
  assert(g.supportsTextAntiAliasing());
  d.invertedTextAaSupported = false;
  g.setDarkMode(true);
  assert(!g.supportsTextAntiAliasing());
  d.invertedTextAaSupported = true;
  assert(g.supportsTextAntiAliasing());
  g.setDarkMode(false);
  const auto draw = [&] {
    g.clearScreen();
    g.drawText(1, 20, 40, "Clear text in either mode");
    g.drawText(2, 20, 100, "Menus and icons stay readable");
    g.fillRect(20, 170, 380, 60);
    g.drawText(2, 32, 180, "Selected item", false);
    g.drawLine(24, 268, 24, 250, 2, true);
    g.drawLine(16, 258, 24, 250, 2, true);
    g.drawLine(32, 258, 24, 250, 2, true);
  };
  unsigned cases = 0;
  for (auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted, GfxRenderer::LandscapeClockwise,
                           GfxRenderer::LandscapeCounterClockwise}) {
    g.setOrientation(orientation);
    g.setRenderMode(GfxRenderer::BW);
    draw();
    const auto logical = d.fb;
    const int width = g.getTextWidth(1, "Clear text in either mode");
    g.setDarkMode(false);
    g.displayBuffer();
    const auto light = d.presented;
    g.setDarkMode(true);
    assert(g.hasRefreshOverridePending());
    g.clearRefreshOverride();  // An indexing popup cannot drop the polarity clean.
    g.setNextDisplayRefreshMode(HalDisplay::FAST_REFRESH);
    draw();
    assert(d.fb == logical && g.getTextWidth(1, "Clear text in either mode") == width);
    g.displayBuffer();
    assert(d.presentedMode == HalDisplay::FULL_REFRESH && !g.hasRefreshOverridePending());
    assert(d.fb == logical);  // No cumulative/double inversion of glyphs or icons.
    for (size_t i = 0; i < light.size(); ++i) assert(d.presented[i] == static_cast<uint8_t>(~light[i]));
    if (orientation == GfxRenderer::Portrait) {
      screenshot("dark-mode.pgm", d.presented);
      screenshot("light-mode.pgm", light);
    }
    g.setDarkMode(true);
    g.displayBuffer();
    assert(d.presentedMode == HalDisplay::FAST_REFRESH);  // No extra clean on ordinary page turns.
    g.setDarkMode(false);
    g.triggerDisplayAsync(HalDisplay::FAST_REFRESH);
    g.completeDisplay();
    assert(d.presentedMode == HalDisplay::FULL_REFRESH && d.presented == light);
    ++cases;
  }
  g.setDarkMode(true);
  const auto windows = d.windows;
  g.displayWindow(0, 0, 32, 32, true);
  assert(d.windows == windows && d.presentedMode == HalDisplay::FULL_REFRESH && d.presentedPowerOff);
  assert(!g.hasRefreshOverridePending());
  g.displayWindow(0, 0, 32, 32);
  assert(d.windows == windows + 1);
  g.setDarkMode(false);
  g.setFadingFix(true);
  g.displayBuffer();
  assert(d.presentedPowerOff && d.presentedMode == HalDisplay::FULL_REFRESH);
  g.setFadingFix(false);
  g.setOrientation(GfxRenderer::Portrait);
  std::cout << "Dark-mode production renderer: " << cases
            << " orientations, unchanged glyph/layout pixels, full transition/window/power checks passed\n";
  return 0;
}
