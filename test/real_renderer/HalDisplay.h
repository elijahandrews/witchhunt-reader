#pragma once
#include <Arduino.h>

#include <array>
#include <cstring>
#include <vector>
class HalDisplay {
 public:
  enum RefreshMode { FULL_REFRESH, FAST_REFRESH, PARTIAL_REFRESH, HALF_REFRESH };
  static constexpr int DISPLAY_WIDTH = 800, DISPLAY_HEIGHT = 480, DISPLAY_WIDTH_BYTES = 100;
  std::array<uint8_t, 48000> fb{}, copiedLsb{}, copiedMsb{};
  enum Event { Wait = 1, CopyLsb, CopyMsb, Gray, Cleanup, TextAa, Bw };
  std::array<uint8_t, 48000> alternate{};
  uint8_t* writeBuffer = fb.data();
  uint8_t* displayedBuffer = nullptr;
  bool simulateText = false, acceptText = false;
  RefreshMode submittedMode = FULL_REFRESH;
  void submit(RefreshMode mode) {
    submittedMode = mode;
    displayedBuffer = writeBuffer;
    writeBuffer = writeBuffer == fb.data() ? alternate.data() : fb.data();
  }
  bool trace = false;
  std::vector<int> events;
  HalDisplay() { fb.fill(255); }
  uint8_t* getFrameBuffer() { return writeBuffer; }
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
  void displayWindow(const T&...) {}
  template <class... T>
  void triggerDisplay(const T&...) {}
  template <class... T>
  void triggerDisplayAsync(const T&...) {}
  void displayBuffer(RefreshMode mode, bool = false) {
    if (simulateText) {
      events.push_back(Bw);
      submit(mode);
    }
  }
  void copyGrayscaleLsbBuffers(const uint8_t* p) {
    if (trace) {
      events.push_back(CopyLsb);
      memcpy(copiedLsb.data(), p, copiedLsb.size());
    }
  }
  void copyGrayscaleMsbBuffers(const uint8_t* p) {
    if (trace) {
      events.push_back(CopyMsb);
      memcpy(copiedMsb.data(), p, copiedMsb.size());
    }
  }
  template <class... T>
  void displayGrayBuffer(const T&...) {
    if (trace) events.push_back(Gray);
  }
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
  bool supportsTextAa() { return simulateText; }
  bool tryDisplayTextAa(const uint8_t* lsb, const uint8_t* msb, size_t bytes, bool) {
    if (!simulateText || !acceptText || !lsb || !msb || bytes != 48000) return false;
    events.push_back(TextAa);
    submit(FAST_REFRESH);
    return true;
  }
  void releaseTextAa() {}
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
  bool fastRefreshNeedsHostFrame() const { return false; }
  bool asyncRefreshHoldsFrameBuffer() const { return false; }
  bool ghostsUnderFastRefresh() const { return false; }
  bool supportsAsyncRefresh() { return false; }
  bool supportsDeferredGrayscaleBase() const { return false; }
  template <class... T>
  void triggerGrayscaleBaseAsync(const T&...) {}
  int getLastFastRefreshMs() { return 0; }
  void finishDisplayAsync() {
    if (trace) events.push_back(Wait);
  }
  void completeDisplay() {}
  bool isRefreshPending() { return false; }
  bool isRedRamSynced() { return true; }
  RefreshMode getLastRefreshMode() { return FULL_REFRESH; }
  uint8_t getLastDisplayModeByte() { return 0; }
  void cleanupGrayscaleWithPreviousBuffer() {
    if (trace) events.push_back(Cleanup);
  }
  void syncRedRamFromFrameBuffer() {}
};
