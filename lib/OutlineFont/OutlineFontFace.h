#pragma once

#include <cstddef>
#include <cstdint>

#include "EpdOutlineFontCallbacks.h"

// Standalone bounded-memory outline face. No ownership of the caller's source.
// Face lifetime must not exceed its source; callbacks serialize on one face.
class OutlineFontFace {
 public:
  using ReadFn = size_t (*)(void* ctx, uint32_t offset, uint8_t* buffer, size_t count);
  enum class Hinting : uint8_t { None, Native, Light };
  enum class RasterPolicy : uint8_t { LegacyBitmap, Oversample2x, FinalSize };
  struct Options {
    float logicalPx = 14;
    // Zero derives typographic points from the display em at 150dpi.
    float opticalPoints = 0;
    uint16_t weight = 400;
    bool italic = false;
    Hinting hinting = Hinting::Light;
    RasterPolicy rasterPolicy = RasterPolicy::FinalSize;
    // Quantize fractional origins to 4, 8, or 16 phases per display pixel.
    uint8_t phaseSteps = 8;
    // Native SD adapters may keep the source's default variation instance and
    // use their existing layout tables instead of allocating another copy.
    bool applyVariations = true;
    bool loadLayoutTables = true;
    size_t memoryLimit = 1024 * 1024;
    size_t tableLimit = 512 * 1024;
    // Optional shared budget. Callbacks and context must outlive the face.
    void* allocationContext = nullptr;
    bool (*claimMemory)(void* ctx, size_t bytes) = nullptr;
    void (*releaseMemory)(void* ctx, size_t bytes) = nullptr;
    void (*memoryFailure)(void* ctx) = nullptr;
  };
  struct LineMetrics {
    int ascender;
    int descender;
    uint16_t advanceY;
  };

  OutlineFontFace();
  ~OutlineFontFace();
  OutlineFontFace(const OutlineFontFace&) = delete;
  OutlineFontFace& operator=(const OutlineFontFace&) = delete;
  bool openMemory(const uint8_t* data, size_t bytes, const Options& options);
  bool openStream(ReadFn read, void* ctx, uint32_t bytes, const Options& options);
  void close();
  bool ready() const;
  int lastError() const;
  size_t memoryUsed() const;
  size_t memoryPeak() const;
  LineMetrics lineMetrics() const;
  bool hasOpticalSize() const;
  bool hasWeightAxis() const;
  bool hasItalicAxis() const;
  float opticalSize() const;
  uint16_t glyphId(uint32_t unicodeCp, uint8_t capsMode = 0);
  EpdGlyphRef metrics(uint16_t gid);
  const uint8_t* bitmap(uint16_t gid, size_t* bytes);
  bool raster(uint16_t gid, float scale, int8_t phase, EpdRasterGlyph* out);
  // Native integer-font adapter: ppem is 26.6 fixed-point display pixels.
  bool rasterAtPpem(uint16_t gid, uint32_t ppem, float scale, int8_t phase, EpdRasterGlyph* out);
  int16_t kerning(uint16_t left, uint16_t right);
  uint16_t ligature(const uint16_t* gids, size_t count);
  static const EpdOutlineFontCallbacks& callbacks();

 private:
  struct Impl;
  Impl* impl_;
};
