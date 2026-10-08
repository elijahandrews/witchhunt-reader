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

static void drawBase(HalDisplay& hal, const Bytes& logical) {
  auto& sdk = hal.einkDisplay;
  std::memcpy(sdk.frameBuffer, logical.data(), logical.size());
  sdk.displayBuffer(EInkDisplay::FULL_REFRESH);
  sdk._bus.clear();
}

static void restoreBase(HalDisplay& hal, const Bytes& logical) {
  auto& sdk = hal.einkDisplay;
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  sdk.cleanupGrayscaleWithPreviousBuffer();
  assert(std::equal(logical.begin(), logical.end(), sdk.frameBufferActive));
#else
  sdk.cleanupGrayscaleBuffers(logical.data());
#endif
  assert(std::equal(logical.begin(), logical.end(), sdk.frameBuffer));
  plane(sdk._bus, 0x10, logical, hal.isDarkMode());
  plane(sdk._bus, 0x13, logical, hal.isDarkMode());
}

static void checkGray(HalDisplay& hal) {
  auto& sdk = hal.einkDisplay;
  auto& bus = sdk._bus;
  // Each nibble contains background, opaque ink and the two gray-edge masks.
  Bytes base(48000, 0xff), lsb(48000, 0), msb(48000, 0);
  // Sparse like text, so the light-mode image-coverage heuristic does not pick
  // its distinct quality/image waveform for this native text-AA comparison.
  std::fill(base.begin() + 5000, base.begin() + 5128, 0x88);
  std::fill(lsb.begin() + 5000, lsb.begin() + 5128, 0x22);
  std::fill(msb.begin() + 5000, msb.begin() + 5128, 0x33);
  unsigned passes = 0, rejected = 0;
  for (bool dark : {false, true, true, false, true}) {
    hal.setDarkMode(dark);
    assert(hal.supportsTextAntiAliasing());
    drawBase(hal, base);
    // As in the reader, the write framebuffer ends up holding a glyph plane;
    // it must never be mistaken for the page on the panel.
    std::memcpy(sdk.frameBuffer, lsb.data(), lsb.size());
    sdk.copyGrayscaleLsbBuffers(sdk.frameBuffer);
    std::memcpy(sdk.frameBuffer, msb.data(), msb.size());
    sdk.copyGrayscaleMsbBuffers(sdk.frameBuffer);
    Bytes oldSelector(48000, dark ? 0xff : 0x00);
    Bytes newSelector(48000, dark ? 0xff : 0x00);
    std::fill(oldSelector.begin() + 5000, oldSelector.begin() + 5128, dark ? 0xaa : 0x55);
    std::fill(newSelector.begin() + 5000, newSelector.begin() + 5128, dark ? 0x99 : 0x66);
    plane(bus, 0x10, oldSelector, false);
    plane(bus, 0x13, newSelector, false);
    sdk.displayGrayBuffer(passes % 2 != 0);
    assert(count(bus, 0x12) == 1);
    for (auto command : {0x22, 0x23}) {
      const auto lut = std::find_if(bus.writes.rbegin(), bus.writes.rend(),
                                    [command](const auto& w) { return w.command == command; });
      assert(lut != bus.writes.rend() && lut->bytes.size() == 49);
      assert(lut->bytes[2] == (dark ? 0x43 : 0x83));
    }
    restoreBase(hal, base);
    // Correct physical baseline is needed again after gray, including power-off.
    Bytes following = base;
    following[100] ^= 0xf0;
    std::memcpy(sdk.frameBuffer, following.data(), following.size());
    sdk.displayBuffer(EInkDisplay::FAST_REFRESH);
    plane(bus, 0x13, following, dark);
    ++passes;
  }

  // Inverted grayscale must reject incomplete or stale planes without running
  // a waveform. The ordinary light-mode legacy API remains independently tested.
  hal.setDarkMode(true);
  for (unsigned scenario = 0; scenario < 8; ++scenario) {
    drawBase(hal, base);
    switch (scenario) {
      case 0:
        break;
      case 1:
        sdk.copyGrayscaleLsbBuffers(lsb.data());
        break;
      case 2:
        sdk.copyGrayscaleMsbBuffers(msb.data());
        break;
      case 3:
        sdk.copyGrayscaleLsbBuffers(lsb.data());
        sdk.copyGrayscaleLsbBuffers(lsb.data());
        sdk.copyGrayscaleMsbBuffers(msb.data());
        break;
      case 4:
        sdk.copyGrayscaleBuffers(lsb.data(), msb.data());
        sdk.copyGrayscaleMsbBuffers(msb.data());
        break;
      case 5:
        sdk.copyGrayscaleBuffers(nullptr, msb.data());
        break;
      default:
        sdk.copyGrayscaleBuffers(lsb.data(), msb.data());
        break;
    }
    const unsigned char customLut[49]{};
    sdk.displayGrayBuffer(false, scenario == 6 ? customLut : nullptr, scenario == 7);
    assert(count(bus, 0x12) == 0);
    restoreBase(hal, base);
    ++rejected;
  }

  drawBase(hal, base);
  sdk.copyGrayscaleLsbBuffers(lsb.data());
  hal.setDarkMode(false);
  sdk.copyGrayscaleMsbBuffers(msb.data());
  sdk.displayGrayBuffer();
  assert(count(bus, 0x12) == 0);  // A mode switch invalidates the entire old pass.
  drawBase(hal, base);
  hal.setDarkMode(true);
  drawBase(hal, base);
  // Explicit-buffer cleanup must preserve a caller-owned logical snapshot.
  const auto before = base;
  sdk.cleanupGrayscaleBuffers(base.data());
  assert(base == before);
  plane(bus, 0x13, base, true);
  sdk.cleanupGrayscaleBuffers(nullptr);
  auto& driver = static_cast<freeink::Uc8279X4Driver&>(*sdk._driver);
  assert(driver._needFullClear);
  drawBase(hal, base);
  std::printf(
      "Actual dark AA HAL/SDK: %u complete passes, %u malformed passes rejected, polarity cancellation, "
      "logical/physical baselines and subsequent page continuity passed\n",
      passes, rejected);
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
      assert(hal.supportsTextAntiAliasing());
      for (auto mode : {freeink::GrayscaleMode::Absolute, freeink::GrayscaleMode::Direct}) {
        assert(!sdk.grayscaleCapabilities(mode).supported());
        bus.clear();
        assert(!sdk.displayGrayscaleBase(mode));
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
  checkGray(hal);
  sdk.releaseBuffers();
  free(driver._grayBase);
  driver._grayBase = nullptr;
  std::printf("Actual dark-mode HAL/SDK: %u transitions, async drain, logical baseline and AA restoration passed\n",
              turns);
}
