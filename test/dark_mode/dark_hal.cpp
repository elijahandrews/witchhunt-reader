#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// Production HAL methods, complete SDK facade and driver; only the electrical
// bus is replaced with the SDK's own host recorder.
#define private public
#include <HalDisplay.h>
#include <driver/Uc8279X4Driver.h>
#undef private

using Bytes = std::vector<uint8_t>;
using freeink::EpdBus;

static size_t count(const EpdBus& bus, uint8_t command) {
  return std::count_if(bus.writes.begin(), bus.writes.end(),
                       [command](const auto& write) { return write.command == command; });
}

static void plane(const EpdBus& bus, uint8_t command, const Bytes& logical, bool inverted) {
  const auto found = std::find_if(bus.writes.rbegin(), bus.writes.rend(),
                                  [command](const auto& write) { return write.command == command; });
  assert(found != bus.writes.rend());
  assert(found->bytes.size() == 60000 && found->transactions == 1);
  for (size_t i = 0; i < 12000; ++i) assert(found->bytes[i] == 0xff);
  for (size_t i = 0; i < logical.size(); ++i)
    assert(found->bytes[i + 12000] == (inverted ? static_cast<uint8_t>(~logical[i]) : logical[i]));
}

int main() {
  BoardConfig::ACTIVE = {};
  BoardConfig::ACTIVE.displayController = BoardConfig::DisplayController::UC8279;
  HalDisplay hal;
  auto& sdk = hal.einkDisplay;
  sdk.begin();
  auto& bus = sdk._bus;
  auto& driver = static_cast<freeink::Uc8279X4Driver&>(*sdk._driver);
  Bytes logical(48000, 0xff);
  // White background, sparse text, solid selection and white icon.
  std::fill(logical.begin() + 6000, logical.begin() + 7000, 0);
  for (size_t i = 500; i < 46000; i += 71) logical[i] = 0x42;
  std::fill(logical.begin() + 6200, logical.begin() + 6250, 0xff);
  assert(!hal.isDarkMode() && !hal.setDarkMode(false));
  std::memcpy(sdk.frameBuffer, logical.data(), logical.size());
  sdk.displayBuffer(EInkDisplay::FULL_REFRESH);

  unsigned turns = 0;
  for (bool dark : {true, true, false, true, false}) {
    const bool previous = hal.isDarkMode();
    const auto before = Bytes(sdk.frameBuffer, sdk.frameBuffer + logical.size());
    bus.clear();
    assert(hal.setDarkMode(dark) == (dark != previous));
    assert(hal.isDarkMode() == dark && bus.writes.empty());
    assert(std::equal(before.begin(), before.end(), sdk.frameBuffer));
    if (dark != previous) assert(sdk._inversionDirty);
    // The renderer test separately proves FULL on transitions. Repeated pages
    // use FAST here to exercise the inverted differential baseline too.
    std::memcpy(sdk.frameBuffer, logical.data(), logical.size());
    sdk.displayBuffer(dark != previous ? EInkDisplay::FULL_REFRESH : EInkDisplay::FAST_REFRESH);
    plane(bus, 0x10, logical, dark);
    plane(bus, 0x13, logical, dark);
    assert(!sdk._inversionDirty && !sdk.isRefreshPending());
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
    assert(std::equal(logical.begin(), logical.end(), sdk.frameBufferActive));
    sdk.syncWriteBufferFromActive();
#endif
    assert(std::equal(logical.begin(), logical.end(), sdk.frameBuffer));
    if (dark) {
      assert(!sdk.supportsAsyncRefresh() && !sdk.supportsGrayFrame());
      for (auto mode :
           {freeink::GrayscaleMode::Overlay, freeink::GrayscaleMode::Absolute, freeink::GrayscaleMode::Direct}) {
        assert(!sdk.grayscaleCapabilities(mode).supported());
        bus.clear();
        assert(!sdk.displayGrayscaleBase(mode));
        sdk.copyGrayscaleBuffers(logical.data(), logical.data());
        sdk.displayGrayBuffer();
        assert(bus.writes.empty());
      }
    } else {
      assert(sdk.grayscaleCapabilities().supported());
    }
    logical[500 + turns] ^= 0x77;
    ++turns;
  }

  // A pending normal refresh finishes before the polarity changes.
  sdk.displayBufferAsync(EInkDisplay::FAST_REFRESH);
  assert(hal.setDarkMode(true));
  assert(!sdk.isRefreshPending());
  std::memcpy(sdk.frameBuffer, logical.data(), logical.size());
  sdk.displayBuffer(EInkDisplay::FULL_REFRESH, true);
  assert(!driver._isScreenOn);
  plane(bus, 0x13, logical, true);

#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  // Menu patches after a layout loan still use the displayed logical page.
  sdk.syncWriteBufferFromActive();
  size_t lentBytes = 0;
  auto* lent = sdk.borrowSecondaryBuffer(&lentBytes);
  assert(lent && lentBytes == logical.size());
  std::memset(lent, 0xa5, lentBytes);
  assert(sdk.returnSecondaryBuffer());
  logical[6200] = 0x3c;
  std::memcpy(sdk.frameBuffer, logical.data(), logical.size());
  bus.clear();
  sdk.displayBuffer(EInkDisplay::FAST_REFRESH);
  plane(bus, 0x13, logical, true);
  assert(std::equal(logical.begin(), logical.end(), sdk.frameBufferActive));
#endif

  assert(hal.setDarkMode(false));
  std::memcpy(sdk.frameBuffer, logical.data(), logical.size());
  sdk.displayBuffer(EInkDisplay::FULL_REFRESH);
  assert(sdk.grayscaleCapabilities().supported());
  assert(sdk.displayGrayscaleBase(freeink::GrayscaleMode::Overlay));
  Bytes empty(logical.size(), 0);
  sdk.copyGrayscaleBuffers(empty.data(), empty.data());
  bus.clear();
  sdk.displayGrayBuffer();
  assert(count(bus, 0x12) == 1);
  sdk.cleanupGrayscaleBuffers(logical.data());
  sdk.releaseBuffers();
  free(driver._grayBase);
  driver._grayBase = nullptr;
  std::printf("Actual dark-mode HAL/SDK: %u transitions, async drain, logical baseline and AA restoration passed\n",
              turns);
}
