#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
namespace HalDisplay {
enum RefreshMode { FAST_REFRESH };
}
class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };
  std::array<uint8_t, 8> bw{};
  uint8_t* target = nullptr;
  RenderMode mode = BW;
  bool textCapable = true, deferredCapable = false, accepts = true;
  unsigned textSubmissions = 0, baseSubmissions = 0, graySubmissions = 0, cleanup = 0;
  unsigned getDisplayWidthBytes() const { return 2; }
  unsigned getDisplayHeight() const { return 4; }
  bool supportsTextAa() const { return textCapable; }
  bool supportsDeferredGrayscaleBase() const { return deferredCapable; }
  void setRenderMode(RenderMode m) { mode = m; }
  void beginStripTarget(uint8_t* p, unsigned, unsigned) {
    assert(!target);
    target = p;
  }
  void endStripTarget() {
    assert(target);
    target = nullptr;
  }
  void clearScreen(uint8_t value) {
    assert(target);
    std::fill_n(target, 8, value);
  }
  bool drawPlane() {
    assert(target && mode != BW);
    std::fill_n(target, 8, mode == GRAYSCALE_LSB ? 0x20 : 0x40);
    return true;
  }
  bool tryDisplayTextAa(const uint8_t* lsb, const uint8_t* msb) {
    assert(!target && mode == BW);
    assert(lsb[0] == 0x20 && msb[0] == 0x40);
    ++textSubmissions;
    return accepts;
  }
  void displayGrayscaleBaseAsync(HalDisplay::RefreshMode) { ++baseSubmissions; }
  void waitRefreshComplete() {}
  void copyGrayscalePlanes(const uint8_t*, const uint8_t*) {}
  void displayGrayBuffer() { ++graySubmissions; }
  void cleanupGrayscaleWithFrameBuffer() { ++cleanup; }
};
