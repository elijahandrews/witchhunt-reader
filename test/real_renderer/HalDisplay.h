#pragma once
#include <Arduino.h>
#include <array>
#include <cstring>
#include <vector>
class HalDisplay {
public:
  enum RefreshMode {
    FULL_REFRESH,
    FAST_REFRESH,
    PARTIAL_REFRESH,
    HALF_REFRESH
  };
  static constexpr int DISPLAY_WIDTH = 800, DISPLAY_HEIGHT = 480,
                       DISPLAY_WIDTH_BYTES = 100;
  std::array<uint8_t, 48000> fb{};
  HalDisplay() { fb.fill(255); }
  uint8_t *getFrameBuffer() { return directSwapped ? directPrevious.data() : fb.data(); }
  int getDisplayWidth() const { return 800; }
  int getDisplayHeight() const { return 480; }
  int getDisplayWidthBytes() const { return 100; }
  int getBufferSize() const { return 48000; }
  void clearScreen(uint8_t c) { fb.fill(c ? 255 : 0); }
  template <class... T> void drawImage(const T &...) {}
  template <class... T> void drawImageTransparent(const T &...) {}
  template <class... T> void displayWindow(const T &...) {}
  template <class... T> void triggerDisplay(const T &...) {}
  template <class... T> void triggerDisplayAsync(const T &...) {}
  template <class... T> void displayBuffer(const T &...) {}
  // Opt-in recording keeps existing renderer fixtures unchanged while checking
  // prepared overlays at the actual GfxRenderer/HAL boundary.
  bool recordGray = false;
  std::vector<char> grayEvents;
  std::array<uint8_t, 48000> uploadedLsb{}, uploadedMsb{};
  bool grayKeptPowered = false;
  void copyGrayscaleLsbBuffers(const uint8_t* plane) {
    if (recordGray) {
      grayEvents.push_back('L');
      std::memcpy(uploadedLsb.data(), plane, uploadedLsb.size());
    }
  }
  void copyGrayscaleMsbBuffers(const uint8_t* plane) {
    if (recordGray) {
      grayEvents.push_back('M');
      std::memcpy(uploadedMsb.data(), plane, uploadedMsb.size());
    }
  }
  void displayGrayBuffer(bool turnOff) {
    if (recordGray) {
      grayEvents.push_back('G');
      grayKeptPowered = !turnOff;
    }
  }
  bool directSupported = false, directSwapped = false;
  std::array<uint8_t, 48000> directPrevious{};
  struct DirectGrayTimings {
    unsigned long uploadMs = 0, displayMs = 0, baselineMs = 0;
  };
  bool supportsDirectGrayPanel() { return directSupported; }
  bool supportsDirectGrayPlanes() { return directSupported; }
  bool displayDirectGrayPlanes(const uint8_t* lsb, const uint8_t* msb, DirectGrayTimings& t, bool turnOff) {
    t = {};
    if (!lsb || !msb || !directSupported) return false;
    grayEvents.push_back('D');
    copyGrayscaleLsbBuffers(lsb);
    copyGrayscaleMsbBuffers(msb);
    displayGrayBuffer(turnOff);
    directSwapped = !directSwapped;
    std::memcpy(getFrameBuffer(), directSwapped ? fb.data() : directPrevious.data(), fb.size());
    grayEvents.push_back('S');
    t = {1, 2, 3};
    return true;
  }
  bool supportsAbsoluteGrayPlanes() { return false; }
  template <class... T> bool beginAbsoluteGrayPass(const T &...) {
    return false;
  }
  int getGrayLevels() { return 4; }
  uint8_t *borrowGray8Canvas(uint16_t *) { return nullptr; }
  template <class... T> void displayGray8Canvas(const T &...) {}
  bool supportsGrayFrame() { return false; }
  template <class... T> void triggerGrayscaleFrame(const T &...) {}
  template <class... T> void displayGrayscaleFrame(const T &...) {}
  template <class... T> void cleanupGrayscaleBuffers(const T &...) {}
  void setFastGrayscaleLut(bool) {}
  bool getFastGrayscaleLut() { return false; }
  void releaseBuffers() {}
  void syncWriteBufferFromActive() {}
  bool releaseSecondaryBuffer() { return false; }
  bool reallocSecondaryBuffer() { return false; }
  bool hasSecondaryBuffer() { return false; }
  uint8_t *borrowSecondaryBuffer(size_t *) { return nullptr; }
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
  void cleanupGrayscaleWithPreviousBuffer() { if (recordGray) grayEvents.push_back('C'); }
  void syncRedRamFromFrameBuffer() {}
};
