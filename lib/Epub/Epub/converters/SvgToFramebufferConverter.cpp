#include "SvgToFramebufferConverter.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <SvgRasterizer.h>

#include <algorithm>

#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "PixelCache.h"

namespace {
struct SourceFile {
  HalFile file;
  ~SourceFile() { file.close(); }
};

// Fixed writer state exceeds the small reader-task stack budget. The native
// PXC writer owns at most its bounded row band; SVG owns geometry plus one row.
struct SvgRenderState {
  const RenderConfig& config;
  DirectPixelWriter pixels{};
  PixelCache cache;
  int screenWidth;
  int screenHeight;

  SvgRenderState(GfxRenderer& renderer, const RenderConfig& config)
      : config(config), screenWidth(renderer.getScreenWidth()), screenHeight(renderer.getScreenHeight()) {
    pixels.init(renderer, config.useGrayscale, config.cacheOnly);
  }

  static bool row(void* context, int y, const uint8_t* gray, int width) {
    auto& state = *static_cast<SvgRenderState*>(context);
    if (state.config.cancelled()) return false;
    const int screenY = state.config.y + y;
    const bool visible = state.pixels.fb && screenY >= 0 && screenY < state.screenHeight;
    if (visible) state.pixels.beginRow(screenY);
    DirectCacheWriter cachePixels{};
    const bool caching = state.cache.ok && state.cache.advanceTo(y);
    if (state.config.cacheOnly && !caching) return false;
    if (caching) {
      cachePixels.init(state.cache.buffer, state.cache.bytesPerRow, state.cache.bandRows, state.cache.originX);
      cachePixels.beginRow(screenY, state.config.y + state.cache.bandStart);
    }
    for (int x = 0; x < width; ++x) {
      const int screenX = state.config.x + x;
      const uint8_t value = state.config.useDithering ? applyBayerDither4Level(gray[x], screenX, screenY)
                                                      : static_cast<uint8_t>(gray[x] / 85);
      if (visible && screenX >= 0 && screenX < state.screenWidth) state.pixels.writePixel(screenX, value);
      // Complete cache pixels are independent of current clipping/orientation.
      if (caching) cachePixels.writePixel(screenX, value);
    }
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
  SourceFile source;
  if (!Storage.openFileForRead("SVG", path, source.file)) return false;
  SvgRasterizer::Dimensions dimensions;
  if (!SvgRasterizer::dimensionsFromFile(source.file, dimensions)) return false;
  out = {static_cast<int16_t>(dimensions.width), static_cast<int16_t>(dimensions.height)};
  return true;
}

bool SvgToFramebufferConverter::decodeToFramebuffer(const std::string& path, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  // SVG remains a streaming file source; PNG/JPEG retain their PSRAM path.
  if (config.cancelled() || (config.cacheOnly && config.cachePath.empty()) || config.sourceData ||
      config.maxWidth <= 0 || config.maxHeight <= 0 || config.cachePath == path)
    return false;
  constexpr int coordinateLimit = 1000000;
  if (config.x < -coordinateLimit || config.x > coordinateLimit || config.y < -coordinateLimit ||
      config.y > coordinateLimit)
    return false;
  SourceFile source;
  if (!Storage.openFileForRead("SVG", path, source.file)) return false;
  SvgRasterizer::Dimensions dimensions;
  if (!SvgRasterizer::dimensionsFromFile(source.file, dimensions)) return false;
  int width = config.maxWidth;
  int height = config.maxHeight;
  if (!config.useExactDimensions) {
    const float scale = std::min(
        1.0f, std::min(static_cast<float>(width) / dimensions.width, static_cast<float>(height) / dimensions.height));
    width = std::max(1, static_cast<int>(dimensions.width * scale));
    height = std::max(1, static_cast<int>(dimensions.height * scale));
  }
  SvgRasterizer rasterizer;
  rasterizer.setAbortCallback([](void* context) { return static_cast<const RenderConfig*>(context)->cancelled(); },
                              const_cast<RenderConfig*>(&config));
  if (!rasterizer.load(source.file, width, height)) return false;
  // No duplicate reader for this path and no source handle held while publishing
  // pixels/caches; complete geometry was validated before any framebuffer change.
  if (!source.file.close()) return false;
  auto state = makeUniqueNoThrow<SvgRenderState>(renderer, config);
  if (!state) {
    LOG_ERR("SVG", "OOM writer state (%u bytes)", unsigned(sizeof(SvgRenderState)));
    return false;
  }
  if (!config.cachePath.empty()) state->cache.begin(config.cachePath, width, height, config.x, config.y, 1);
  if (config.cacheOnly && !state->cache.ok) return false;
  if (!rasterizer.render(SvgRenderState::row, state.get())) return false;
  // As for native raster decoders, cache I/O is optional. Its destructor removes
  // an unfinished cache; an allocation/parse failure never touches the display.
  if (config.cancelled()) return false;
  if (state->cache.ok) {
    const bool complete = state->cache.finalize();
    if (config.cacheOnly) return complete;
  } else if (config.cacheOnly)
    return false;
  return true;
}
