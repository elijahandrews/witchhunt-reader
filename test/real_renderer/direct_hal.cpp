#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// Same state inspection technique as the SDK's host/test_pro.cpp. The actual
// HAL header, extracted production methods, facade, and drivers remain intact.
#define private public
#include <HalDisplay.h>
#include <driver/Ssd1677Driver.h>
#include <driver/Uc8179Driver.h>
#include <driver/Uc8279X4Driver.h>
#undef private

using Bytes = std::vector<uint8_t>;
using freeink::EpdBus;
using freeink::GrayscaleMode;
using freeink::Uc8279X4Driver;
static unsigned directCases = 0, rejectedCases = 0, recoveryCases = 0;

static Bytes frame(unsigned seed) {
  Bytes bytes(48000);
  for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>((i * 37 + i / 100 * 11 + seed) ^ (i >> 8));
  return bytes;
}

struct Planes {
  Bytes lsb, msb, bw;
  explicit Planes(unsigned seed) : lsb(frame(seed)), msb(frame(seed + 91)), bw(48000) {
    for (size_t i = 0; i < bw.size(); ++i) bw[i] = lsb[i] & msb[i];
  }
};

struct Panel {
  HalDisplay hal;
  explicit Panel(BoardConfig::DisplayController controller = BoardConfig::DisplayController::UC8279) {
    BoardConfig::ACTIVE = {};
    BoardConfig::ACTIVE.displayController = controller;
    hal.einkDisplay.begin();  // Actual SDK runtime controller selection and initialization.
    bus().clear();
  }
  ~Panel() {
    hal.einkDisplay.releaseBuffers();
    // Drivers are SDK singletons. Free their optional host scratch between cases.
    if (hal.einkDisplay._driver == &freeink::uc8279X4Driver()) {
      auto& d = driver();
      free(d._grayBase);
      d._grayBase = nullptr;
    } else if (hal.einkDisplay._driver == &freeink::uc8179Driver()) {
      auto& d = static_cast<freeink::Uc8179Driver&>(*hal.einkDisplay._driver);
      free(d._grayBase);
      d._grayBase = nullptr;
    }
  }
  EpdBus& bus() { return hal.einkDisplay._bus; }
  Uc8279X4Driver& driver() { return static_cast<Uc8279X4Driver&>(*hal.einkDisplay._driver); }
};

static size_t count(const EpdBus& bus, uint8_t command) {
  return std::count_if(bus.writes.begin(), bus.writes.end(), [command](const auto& w) { return w.command == command; });
}

static const EpdBus::Write& last(const EpdBus& bus, uint8_t command) {
  const auto found =
      std::find_if(bus.writes.rbegin(), bus.writes.rend(), [command](const auto& w) { return w.command == command; });
  assert(found != bus.writes.rend());
  return *found;
}

static void checkPlane(const EpdBus::Write& write, const Bytes& pixels, bool invert) {
  assert(write.transactions == 1 && write.bytes.size() == 60000);
  for (size_t i = 0; i < 12000; ++i) assert(write.bytes[i] == 0xff);
  for (size_t i = 0; i < pixels.size(); ++i)
    assert(write.bytes[i + 12000] == (invert ? static_cast<uint8_t>(~pixels[i]) : pixels[i]));
}

static void rejected(Panel& panel, const uint8_t* lsb, const uint8_t* msb) {
  auto& sdk = panel.hal.einkDisplay;
  const auto* before = sdk.frameBuffer;
  const Bytes content = before ? Bytes(before, before + sdk.bufferSize) : Bytes();
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  const auto* active = sdk.frameBufferActive;
  const Bytes activeContent = active ? Bytes(active, active + sdk.bufferSize) : Bytes();
#endif
  const auto grayMode = sdk._grayscaleMode;
  const auto refreshMode = panel.hal.lastRefreshMode;
  panel.bus().clear();
  HalDisplay::DirectGrayTimings timings{99, 99, 99};
  assert(!panel.hal.displayDirectGrayPlanes(lsb, msb, timings));
  assert(panel.bus().writes.empty());  // Neither uploads nor any activation on rejection.
  assert(timings.uploadMs == 0 && timings.displayMs == 0 && timings.baselineMs == 0);
  assert(sdk.frameBuffer == before && sdk._grayscaleMode == grayMode);
  assert(panel.hal.lastRefreshMode == refreshMode);
  if (before) assert(std::equal(content.begin(), content.end(), before));
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  assert(sdk.frameBufferActive == active);
  if (active) assert(std::equal(activeContent.begin(), activeContent.end(), active));
#endif
  ++rejectedCases;
}

#if !defined(EINK_DISPLAY_SINGLE_BUFFER_MODE) && defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
static void direct(Panel& panel, const Planes& planes, bool turnOff = false) {
  auto& sdk = panel.hal.einkDisplay;
  assert(panel.hal.supportsDirectGrayPanel());
  assert(panel.hal.supportsDirectGrayPlanes());
  std::memcpy(sdk.frameBuffer, planes.bw.data(), planes.bw.size());
  uint8_t* submitted = sdk.frameBuffer;
  uint8_t* previous = sdk.frameBufferActive;
  panel.bus().clear();
  HalDisplay::DirectGrayTimings timings;
  assert(panel.hal.displayDirectGrayPlanes(planes.lsb.data(), planes.msb.data(), timings, turnOff));
  const auto& bus = panel.bus();
  assert(count(bus, 0x12) == 1);                           // One Direct DRF; no prior BW activation.
  assert(count(bus, 0xe5) == 0 && count(bus, 0x91) == 0);  // No BW TSSET or partial mode.
  assert(count(bus, 0x10) == 1 && count(bus, 0x13) == 1);
  checkPlane(last(bus, 0x10), planes.lsb, true);
  checkPlane(last(bus, 0x13), planes.msb, true);
  const auto drf = std::find_if(bus.writes.begin(), bus.writes.end(), [](const auto& w) { return w.command == 0x12; });
  assert(drf != bus.writes.end());
  assert(std::count_if(bus.writes.begin(), drf, [](const auto& w) { return w.command == 0x10 || w.command == 0x13; }) ==
         2);
  for (uint8_t lut = 0x20; lut <= 0x24; ++lut) assert(last(bus, lut).bytes.size() == 49);
  assert(last(bus, 0x22).bytes != last(bus, 0x23).bytes);  // Full four-tone quality bank.
  assert(last(bus, 0x00).bytes[0] & 0x20);                 // External LUT at activation.
  assert(count(bus, 0x02) == (turnOff ? 1u : 0u));
  assert(sdk.frameBuffer == previous && sdk.frameBufferActive == submitted);
  assert(std::equal(planes.bw.begin(), planes.bw.end(), sdk.frameBuffer));
  assert(std::equal(planes.bw.begin(), planes.bw.end(), sdk.frameBufferActive));
  assert(!sdk.isRefreshPending() && !sdk._inversionDirty && !sdk._redRamSynced);
  assert(sdk._grayscaleMode == GrayscaleMode::Overlay);
  assert(panel.driver()._directGrayOnPanel && panel.driver()._needFullClear);
  assert(!panel.driver()._oldPlaneValid && !panel.driver()._absoluteInput && !panel.driver()._directGrayPass);
  assert(panel.driver()._isScreenOn == !turnOff);
  assert(panel.hal.lastRefreshMode == HalDisplay::FULL_REFRESH && panel.hal.lastDisplayModeByte == 0x34);
  assert(timings.uploadMs && timings.displayMs && timings.baselineMs);
  ++directCases;
}

static void testRejections() {
  const Planes planes(14);
  Panel panel;
  auto& sdk = panel.hal.einkDisplay;
  direct(panel, planes);
  rejected(panel, nullptr, planes.msb.data());
  rejected(panel, planes.lsb.data(), nullptr);
  rejected(panel, nullptr, nullptr);
  for (auto board : {BoardConfig::Board::XteinkX3, BoardConfig::Board::XteinkX4, BoardConfig::Board::Sticky}) {
    BoardConfig::ACTIVE.board = board;
    assert(!panel.hal.supportsDirectGrayPanel());
    assert(!panel.hal.supportsDirectGrayPlanes());
    rejected(panel, planes.lsb.data(), planes.msb.data());
  }
  BoardConfig::ACTIVE.board = BoardConfig::Board::XteinkX4Pro;
  for (uint8_t variant : {0x02, 0x67, 0x69}) {
    BoardConfig::ACTIVE.displayControllerVariant = variant;
    assert(!panel.hal.supportsDirectGrayPanel());
    assert(!panel.hal.supportsDirectGrayPlanes());
    rejected(panel, planes.lsb.data(), planes.msb.data());
  }
  BoardConfig::ACTIVE.displayControllerVariant = 0x68;
  auto* driver = sdk._driver;
  sdk._driver = nullptr;
  assert(panel.hal.supportsDirectGrayPanel());
  assert(!panel.hal.supportsDirectGrayPlanes());
  rejected(panel, planes.lsb.data(), planes.msb.data());
  sdk._driver = driver;
  sdk.setInverted(true);
  assert(panel.hal.supportsDirectGrayPanel());
  assert(!panel.hal.supportsDirectGrayPlanes());
  rejected(panel, planes.lsb.data(), planes.msb.data());
  sdk.setInverted(false);  // Dirty polarity is safe: Direct supplies the entire new frame.
  assert(sdk._inversionDirty);
  direct(panel, Planes(42));
  assert(sdk.releaseSecondaryBuffer());
  assert(panel.hal.supportsDirectGrayPanel());
  assert(!panel.hal.supportsDirectGrayPlanes());
  rejected(panel, planes.lsb.data(), planes.msb.data());
  assert(sdk.reallocSecondaryBuffer());
  direct(panel, Planes(53));
  size_t size = 0;
  uint8_t* loan = sdk.borrowSecondaryBuffer(&size);
  assert(loan && size == planes.bw.size());
  std::memset(loan, 0xa5, size);
  assert(panel.hal.supportsDirectGrayPanel());
  assert(!panel.hal.supportsDirectGrayPlanes());
  rejected(panel, planes.lsb.data(), planes.msb.data());
  assert(sdk.returnSecondaryBuffer());
  direct(panel, Planes(67));
  // The primary can be lent independently of the secondary. Panel eligibility
  // stays visible, but there is no complete logical BW page to promote.
  uint32_t buildSize = 0;
  assert(sdk.lendBuildStorage(&buildSize) && buildSize == planes.bw.size());
  assert(panel.hal.supportsDirectGrayPanel());
  assert(!panel.hal.supportsDirectGrayPlanes());
  rejected(panel, planes.lsb.data(), planes.msb.data());
  sdk.returnBuildStorage();
  direct(panel, Planes(73));
  sdk.releaseBuffers();
  assert(panel.hal.supportsDirectGrayPanel());
  rejected(panel, planes.lsb.data(), planes.msb.data());
}

static void testTransitions() {
  Panel panel;
  auto& sdk = panel.hal.einkDisplay;
  for (unsigned seed = 0; seed < 12; ++seed) direct(panel, Planes(seed * 19), (seed & 1) != 0);

  // A menu changes only a patch, relying on the promoted current-page baseline.
  auto menu = Bytes(sdk.frameBufferActive, sdk.frameBufferActive + sdk.bufferSize);
  sdk.syncWriteBufferFromActive();
  for (size_t i = 1400; i < 2300; ++i) sdk.frameBuffer[i] = menu[i] ^= 0xff;
  panel.bus().clear();
  sdk.displayBuffer(EInkDisplay::FAST_REFRESH);
  // Existing SDK recovery first paints the BW target then runs its clearing refresh.
  assert(count(panel.bus(), 0x12) == 2);
  checkPlane(last(panel.bus(), 0x10), menu, false);
  checkPlane(last(panel.bus(), 0x13), menu, false);
  assert(std::equal(menu.begin(), menu.end(), sdk.frameBufferActive));
  assert(!panel.driver()._needFullClear && !panel.driver()._directGrayOnPanel && panel.driver()._oldPlaneValid);
  assert(sdk._redRamSynced);
  ++recoveryCases;

  direct(panel, Planes(119));
  const Planes overlay(87);
  std::memcpy(sdk.frameBuffer, overlay.bw.data(), overlay.bw.size());
  panel.bus().clear();
  sdk.displayBuffer(EInkDisplay::FAST_REFRESH);
  assert(count(panel.bus(), 0x12) == 2);
  Bytes deltaLsb(48000), deltaMsb(48000);
  for (size_t i = 0; i < deltaLsb.size(); ++i) {
    deltaLsb[i] = overlay.lsb[i] & static_cast<uint8_t>(~overlay.msb[i]);
    deltaMsb[i] = overlay.lsb[i] ^ overlay.msb[i];
  }
  panel.bus().clear();
  sdk.copyGrayscaleLsbBuffers(deltaLsb.data());
  sdk.copyGrayscaleMsbBuffers(deltaMsb.data());
  sdk.displayGrayBuffer();
  assert(count(panel.bus(), 0x12) == 1);
  checkPlane(last(panel.bus(), 0x10), overlay.bw, false);
  checkPlane(last(panel.bus(), 0x13), overlay.bw, false);
  sdk.cleanupGrayscaleWithPreviousBuffer();
  assert(std::equal(overlay.bw.begin(), overlay.bw.end(), sdk.frameBuffer));
  assert(!panel.driver()._needFullClear && !panel.driver()._directGrayOnPanel);
  ++recoveryCases;

  direct(panel, Planes(167));
  sdk.requestResync();
  assert(panel.driver()._needFullClear);
  direct(panel, Planes(175));
  ++recoveryCases;

  panel.bus().clear();
  sdk.deepSleep();
  assert(count(panel.bus(), 0x12) == 0 && count(panel.bus(), 0x07) == 1);
  assert(!panel.driver()._isScreenOn && !panel.driver()._directGrayOnPanel);
  sdk.begin();  // Actual wake/reinitialize lifecycle, not a refresh while deep asleep.
  direct(panel, Planes(199));
  ++recoveryCases;

  sdk.setInverted(true);
  const auto invertedTarget = frame(201);
  std::memcpy(sdk.frameBuffer, invertedTarget.data(), invertedTarget.size());
  panel.bus().clear();
  sdk.displayBuffer(EInkDisplay::FAST_REFRESH);
  assert(count(panel.bus(), 0x12) == 2);
  checkPlane(last(panel.bus(), 0x10), invertedTarget, true);
  checkPlane(last(panel.bus(), 0x13), invertedTarget, true);
  assert(std::equal(invertedTarget.begin(), invertedTarget.end(), sdk.frameBufferActive));
  sdk.setInverted(false);
  assert(sdk._inversionDirty);
  direct(panel, Planes(213));
  ++recoveryCases;
}
#endif

int main() {
  const Planes planes(7);
  // Real alternate SDK controller selections, not merely fake capability flags.
  for (auto controller : {BoardConfig::DisplayController::SSD1677, BoardConfig::DisplayController::UC8179}) {
    Panel panel(controller);
    assert(!panel.hal.supportsDirectGrayPanel());
    assert(!panel.hal.supportsDirectGrayPlanes());
    rejected(panel, planes.lsb.data(), planes.msb.data());
  }
#if !defined(EINK_DISPLAY_SINGLE_BUFFER_MODE) && defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
  testRejections();
  testTransitions();
  assert(directCases == 22 && rejectedCases == 17 && recoveryCases == 5);
#else
  Panel panel;
  assert(!panel.hal.supportsDirectGrayPanel());
  assert(!panel.hal.supportsDirectGrayPlanes());
  rejected(panel, planes.lsb.data(), planes.msb.data());
  assert(directCases == 0 && rejectedCases == 3);
#endif
  std::printf("Actual HAL + SDK Direct: %u pages, %u atomic rejections, %u state recoveries passed\n", directCases,
              rejectedCases, recoveryCases);
}
