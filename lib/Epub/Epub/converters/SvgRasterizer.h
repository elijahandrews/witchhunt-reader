#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <memory>

// Small, bounded SVG image subset: svg/g/path with solid fills. Geometry is
// flattened at output resolution, then composited in document order one row at
// a time. No DOM, external resources, scripts, or image-sized working buffer.
class SvgRasterizer {
 public:
  static constexpr size_t MAX_SOURCE_BYTES = 32768;
  static constexpr size_t MAX_PATH_BYTES = 8192;
  static constexpr size_t MAX_EDGES = 1536;
  static constexpr size_t MAX_PATHS = 64;
  static constexpr int MAX_OUTPUT_DIMENSION = 2048;
  struct Dimensions {
    int width = 0;
    int height = 0;
  };
  using RowSink = bool (*)(void*, int, const uint8_t*, int);

  SvgRasterizer();
  ~SvgRasterizer();
  SvgRasterizer(const SvgRasterizer&) = delete;
  SvgRasterizer& operator=(const SvgRasterizer&) = delete;
  static bool dimensionsFromBuffer(const uint8_t* data, size_t size, Dimensions& out);
  static bool dimensionsFromFile(FsFile& file, Dimensions& out);
  bool load(FsFile& file, int outputWidth, int outputHeight);
  bool render(RowSink sink, void* context) const;

 private:
  struct State;
  std::unique_ptr<State> state_;
};
