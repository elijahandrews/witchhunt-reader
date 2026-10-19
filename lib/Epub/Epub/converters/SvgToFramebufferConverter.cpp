#include "SvgToFramebufferConverter.h"

#include <BitmapHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <new>

#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "PixelCache.h"
#include "SvgRasterizer.h"

namespace {
struct OpenFile {
  FsFile file;
  ~OpenFile() {
    if (file) file.close();
  }
};

// Cache bands and writer state live on the heap to preserve the small device
// task stack. Geometry and a single raster row replace an image-sized canvas.
struct RenderState {
  const RenderConfig& config;
  DirectPixelWriter pixels{};
  PixelCache primary, companion;
  DirectCacheWriter primaryWriter{}, companionWriter{};
  std::unique_ptr<Atkinson1BitDitherer> monochrome;
  int screenWidth, screenHeight;
  bool mono;

  RenderState(GfxRenderer& renderer, const RenderConfig& config)
      : config(config),
        screenWidth(renderer.getScreenWidth()),
        screenHeight(renderer.getScreenHeight()),
        mono(config.monochromeOutput || !config.useGrayscale) {
    pixels.init(renderer);
  }
  static bool prepareCache(PixelCache& cache, DirectCacheWriter& writer, int row, int screenY) {
    if (!cache.ok || !cache.advanceTo(row)) return false;
    writer.init(cache.buffer, cache.bytesPerRow, cache.originX, cache.originY + cache.bandStart, cache.width,
                cache.bandRows);
    writer.beginRow(screenY);
    return true;
  }
  static bool row(void* context, int y, const uint8_t* gray, int width) {
    auto& s = *static_cast<RenderState*>(context);
    const int screenY = s.config.y + y;
    if (s.config.gray8) {
      s.config.gray8->beginRow(screenY);
      for (int x = 0; x < width; ++x)
        s.config.gray8->writePixel(s.config.x + x, adaptive_tone::apply(s.config.adaptiveTone, gray[x]));
      return true;
    }
    const bool primary = prepareCache(s.primary, s.primaryWriter, y, screenY);
    const bool companion = prepareCache(s.companion, s.companionWriter, y, screenY);
    const bool visible = screenY >= 0 && screenY < s.screenHeight && s.pixels.fb;
    if (visible) s.pixels.beginRow(screenY);
    for (int x = 0; x < width; ++x) {
      const int screenX = s.config.x + x;
      const uint8_t sample = adaptive_tone::apply(s.config.adaptiveTone, gray[x]);
      const uint8_t gray4 =
          s.config.useDithering ? applyBayerDither4Level(sample, screenX, screenY) : quantizeGray4Level(sample);
      const uint8_t bw = s.monochrome ? s.monochrome->processPixel(sample, x) * 3 : (sample < 128 ? 0 : 3);
      const uint8_t value = s.mono ? bw : gray4;
      if (visible && screenX >= 0 && screenX < s.screenWidth) s.pixels.writePixel(screenX, value);
      // Cache the entire image, including clipped rows/columns. Its replay must
      // be independent of the page orientation or current grayscale strip.
      if (primary) s.primaryWriter.writePixel(screenX, value);
      if (companion) s.companionWriter.writePixel(screenX, s.mono ? gray4 : bw);
    }
    if (s.monochrome) s.monochrome->nextRow();
    return true;
  }
};
}  // namespace

bool SvgToFramebufferConverter::getDimensionsFromBuffer(const uint8_t* data, size_t size, ImageDimensions& out) {
  SvgRasterizer::Dimensions dimensions;
  if (!SvgRasterizer::dimensionsFromBuffer(data, size, dimensions)) return false;
  out = {static_cast<int16_t>(dimensions.width), static_cast<int16_t>(dimensions.height)};
  return true;
}

bool SvgToFramebufferConverter::getDimensions(const std::string& path, ImageDimensions& out) const {
  OpenFile source;
  if (!Storage.openFileForRead("SVG", path, source.file)) return false;
  SvgRasterizer::Dimensions dimensions;
  if (!SvgRasterizer::dimensionsFromFile(source.file, dimensions)) return false;
  out = {static_cast<int16_t>(dimensions.width), static_cast<int16_t>(dimensions.height)};
  return true;
}

bool SvgToFramebufferConverter::decodeToFramebuffer(const std::string& path, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  if (config.maxWidth <= 0 || config.maxHeight <= 0) return false;
  OpenFile source;
  if (!Storage.openFileForRead("SVG", path, source.file)) return false;
  SvgRasterizer::Dimensions dimensions;
  if (!SvgRasterizer::dimensionsFromFile(source.file, dimensions)) return false;
  int width = config.maxWidth, height = config.maxHeight;
  if (!config.useExactDimensions) {
    const float scale = std::min(
        1.0f, std::min(static_cast<float>(width) / dimensions.width, static_cast<float>(height) / dimensions.height));
    width = std::max(1, static_cast<int>(dimensions.width * scale));
    height = std::max(1, static_cast<int>(dimensions.height * scale));
  }
  // Writer transforms and cache origins use signed ints. Bound positions as
  // well as extents before any addition/subtraction in those shared helpers.
  constexpr int coordinateLimit = 1000000;
  if (config.x < -coordinateLimit || config.x > coordinateLimit || config.y < -coordinateLimit ||
      config.y > coordinateLimit)
    return false;
  SvgRasterizer rasterizer;
  if (!rasterizer.load(source.file, width, height)) return false;
  std::unique_ptr<RenderState> state(new (std::nothrow) RenderState(renderer, config));
  if (!state) return false;
  if (!config.gray8) {
    if (!config.cachePath.empty()) state->primary.begin(config.cachePath, width, height, config.x, config.y, 1);
    if (!config.companionCachePath.empty() && config.companionCachePath != config.cachePath)
      state->companion.begin(config.companionCachePath, width, height, config.x, config.y, 1);
    if (config.useDithering && (state->mono || state->companion.ok))
      state->monochrome.reset(new (std::nothrow) Atkinson1BitDitherer(width));
  }
  if (!rasterizer.render(RenderState::row, state.get())) return false;
  // Failed cache I/O is best-effort, as in the raster-image decoders. PixelCache
  // removes unfinished .part files; only complete renders can publish a cache.
  if (state->primary.ok) state->primary.finalize();
  if (state->companion.ok) state->companion.finalize();
  return true;
}
