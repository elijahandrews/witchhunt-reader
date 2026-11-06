#include <GfxRenderer.h>
#include <PublisherBox.h>

#include <iostream>

#include "../../lib/Epub/Epub/converters/DirectPixelWriter.h"

int runImageWarmChecks(GfxRenderer& g, HalDisplay& d) {
  int failures = 0, checks = 0;
  const auto savedOrientation = g.getOrientation();
  for (auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted, GfxRenderer::LandscapeClockwise,
                           GfxRenderer::LandscapeCounterClockwise}) {
    g.setOrientation(orientation);
    for (auto mode : {GfxRenderer::BW, GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
      g.setRenderMode(mode);
      // Stand in for an already prepared text page. Distinct bytes expose writes
      // anywhere, including a white rectangle with no decoded pixels in it.
      for (size_t i = 0; i < d.fb.size(); ++i) d.fb[i] = static_cast<uint8_t>(i * 37 + 19);
      const auto before = d.fb;
      const int x = g.getScreenWidth() * 2 / 5, y = 40;
      const int width = g.getScreenWidth() / 5, height = g.getScreenHeight() / 5;
      {
        GfxRenderer::ScopedCacheOnlyImageWrites cacheOnly(g);
        // PageImage::renderWithForceLoad clears the backdrop before invoking
        // the decoder. Both operations must leave the prepared page untouched.
        clearPublisherImageBackdrop(g, x, y, width, height);
        DirectPixelWriter writer;
        writer.init(g);
        for (int row = y; row < y + height; ++row) {
          writer.beginRow(row);
          for (int col = x; col < x + width; ++col) writer.writePixel(col, 0);
        }
      }
      ++checks;
      if (d.fb != before) {
        ++failures;
        std::cerr << "Image warm changed prepared page: orientation=" << orientation << " mode=" << mode << '\n';
      }
      // The real image still needs an opaque backdrop when it is displayed.
      // Check exact geometry and polarity after the cache-only scope ends.
      clearPublisherImageBackdrop(g, x, y, width, height);
      const auto foreground = d.fb;
      d.fb = before;
      for (int row = y; row < y + height; ++row)
        for (int col = x; col < x + width; ++col) g.drawPixel(col, row, mode != GfxRenderer::BW);
      ++checks;
      if (d.fb != foreground || d.fb == before) ++failures;
    }
  }
  g.setRenderMode(GfxRenderer::BW);
  g.setOrientation(savedOrientation);
  std::cout << "Image warm framebuffer isolation checks=" << checks << " failures=" << failures << '\n';
  return failures;
}
