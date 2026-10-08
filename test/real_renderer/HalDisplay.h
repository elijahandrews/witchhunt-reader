#pragma once
#include <Arduino.h>

#include <array>
#include <cstring>
class HalDisplay {
 public:
  enum RefreshMode { FULL_REFRESH, FAST_REFRESH, PARTIAL_REFRESH, HALF_REFRESH };
  static constexpr int DISPLAY_WIDTH = 800, DISPLAY_HEIGHT = 480, DISPLAY_WIDTH_BYTES = 100;
  std::array<uint8_t, 48000> fb{};
  bool darkMode = false;
  bool setDarkMode(bool enabled) {
    const bool changed = darkMode != enabled;
    darkMode = enabled;
    return changed;
  }
  bool isDarkMode() const { return darkMode; }
  bool invertedTextAaSupported = true;
  bool supportsTextAntiAliasing() const { return !darkMode || invertedTextAaSupported; }
  std::array<uint8_t, 48000> presented{};
  RefreshMode presentedMode = FAST_REFRESH;
  bool presentedPowerOff = false;
  unsigned presentations = 0, windows = 0;
  void present(RefreshMode mode, bool powerOff) {
    presentedMode = mode;
    presentedPowerOff = powerOff;
    ++presentations;
    for (size_t i = 0; i < fb.size(); ++i) presented[i] = darkMode ? static_cast<uint8_t>(~fb[i]) : fb[i];
  }
  HalDisplay() { fb.fill(255); }
  uint8_t* getFrameBuffer() { return fb.data(); }
  int getDisplayWidth() const { return 800; }
  int getDisplayHeight() const { return 480; }
  int getDisplayWidthBytes() const { return 100; }
  int getBufferSize() const { return 48000; }
  void clearScreen(uint8_t c) { fb.fill(c ? 255 : 0); }
  template <class... T>
  void drawImage(const T&...) {}
  template <class... T>
  void drawImageTransparent(const T&...) {}
  template <class... T>
  void displayWindow(const T&...) {
    ++windows;
  }
  void triggerDisplay(RefreshMode mode, bool powerOff) { present(mode, powerOff); }
  void triggerDisplayAsync(RefreshMode mode, bool powerOff) { present(mode, powerOff); }
  void displayBuffer(RefreshMode mode, bool powerOff) { present(mode, powerOff); }
  template <class... T>
  void copyGrayscaleLsbBuffers(const T&...) {}
  template <class... T>
  void copyGrayscaleMsbBuffers(const T&...) {}
  template <class... T>
  void displayGrayBuffer(const T&...) {}
  bool supportsAbsoluteGrayPlanes() { return false; }
  template <class... T>
  bool beginAbsoluteGrayPass(const T&...) {
    return false;
  }
  int getGrayLevels() { return 4; }
  uint8_t* borrowGray8Canvas(uint16_t*) { return nullptr; }
  template <class... T>
  void displayGray8Canvas(const T&...) {}
  bool supportsGrayFrame() { return false; }
  template <class... T>
  void triggerGrayscaleFrame(const T&...) {}
  template <class... T>
  void displayGrayscaleFrame(const T&...) {}
  template <class... T>
  void cleanupGrayscaleBuffers(const T&...) {}
  void setFastGrayscaleLut(bool) {}
  bool getFastGrayscaleLut() { return false; }
  void releaseBuffers() {}
  void syncWriteBufferFromActive() {}
  bool releaseSecondaryBuffer() { return false; }
  bool reallocSecondaryBuffer() { return false; }
  bool hasSecondaryBuffer() { return false; }
  uint8_t* borrowSecondaryBuffer(size_t*) { return nullptr; }
  bool returnSecondaryBuffer() { return false; }
  void setSingleBufferFastDiff(bool) {}
  bool deviceIsX3() { return false; }
  bool supportsAsyncRefresh() { return false; }
  int getLastFastRefreshMs() { return 0; }
  void finishDisplayAsync() {}
  void completeDisplay() {}
  bool isRefreshPending() { return false; }
  bool isRedRamSynced() { return true; }
  RefreshMode getLastRefreshMode() { return FULL_REFRESH; }
  uint8_t getLastDisplayModeByte() { return 0; }
  void cleanupGrayscaleWithPreviousBuffer() {}
  void syncRedRamFromFrameBuffer() {}
};
