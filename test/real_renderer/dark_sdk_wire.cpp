#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#define private public
#include <FreeInkDisplay.h>
#include <driver/Uc8279X4Driver.h>
#undef private

namespace {
using Plane = std::array<uint8_t, 48000>;
using freeink::EpdBus;
using freeink::FreeInkDisplay;

const EpdBus::Write& last(const EpdBus& bus, uint8_t command) {
  const auto it = std::find_if(bus.writes.rbegin(), bus.writes.rend(),
                               [command](const auto& write) { return write.command == command; });
  assert(it != bus.writes.rend());
  return *it;
}

void checkPlane(const EpdBus& bus, uint8_t command, const Plane& expected) {
  const auto& write = last(bus, command);
  assert(write.bytes.size() == 60000 && write.transactions == 1);
  for (size_t i = 0; i < 12000; ++i) assert(write.bytes[i] == 0xff);
  assert(std::equal(expected.begin(), expected.end(), write.bytes.begin() + 12000));
}

void nativeLut(const EpdBus& bus, bool dark) {
  // Literal expected native68 rows, independent of the SDK's row-transform
  // implementation. Complemented selectors reverse SOURCE rails only; timings
  // and VCOM stay fixed. Identical middle rows are not four optical tones.
  const uint8_t first[] = {0x03, 0x03, uint8_t(dark ? 0x43 : 0x83), uint8_t(dark ? 0x43 : 0x83), 0x03};
  const uint8_t second[] = {0x01, 0x41, 0x01, 0x01, 0x81};
  for (unsigned row = 0; row < 5; ++row) {
    const auto& write = last(bus, 0x20 + row);
    std::array<uint8_t, 49> wanted{};
    wanted[0] = 1;
    wanted[1] = 2;
    wanted[2] = first[row];
    wanted[3] = second[row];
    wanted[4] = wanted[5] = wanted[6] = wanted[12] = wanted[13] = 1;
    assert(write.bytes.size() == wanted.size());
    assert(std::equal(wanted.begin(), wanted.end(), write.bytes.begin()));
  }
}
}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  BoardConfig::ACTIVE = {};
  BoardConfig::ACTIVE.displayController = BoardConfig::DisplayController::UC8279;
  FreeInkDisplay display(12, 11, 13, 18, 14, 6);
  display.begin();
  auto& bus = display._bus;
  auto& driver = static_cast<freeink::Uc8279X4Driver&>(*display._driver);
  std::vector<std::filesystem::path> paths;
  for (const auto& entry : std::filesystem::directory_iterator(argv[1]))
    if (entry.path().filename().string().starts_with("dark-aa-") && entry.path().extension() == ".bin")
      paths.push_back(entry.path());
  std::sort(paths.begin(), paths.end());
  assert(paths.size() >= 120 && paths.size() % 60 == 0);
  size_t pages = 0;
  for (const auto& path : paths) {
    Plane bw, lsb, msb;
    std::ifstream file(path, std::ios::binary);
    for (auto* plane : {&bw, &lsb, &msb}) {
      file.read(reinterpret_cast<char*>(plane->data()), plane->size());
      assert(file.gcount() == static_cast<std::streamsize>(plane->size()));
    }
    assert(file.peek() == std::char_traits<char>::eof());
    for (bool dark : {false, true, true, false}) {
      const bool changed = display.isInverted() != dark;
      display.setInverted(dark);
      assert(display.grayscaleCapabilities(freeink::GrayscaleMode::Overlay).supported());
      std::memcpy(display.getFrameBuffer(), bw.data(), bw.size());
      display.displayBuffer(changed ? FreeInkDisplay::FULL_REFRESH : FreeInkDisplay::FAST_REFRESH);
      assert(std::equal(bw.begin(), bw.end(), display.frameBufferActive));
      const Plane previous = [&] {
        Plane result;
        std::memcpy(result.data(), display.getFrameBuffer(), result.size());
        return result;
      }();
      bus.clear();
      display.copyGrayscaleLsbBuffers(lsb.data());
      display.copyGrayscaleMsbBuffers(msb.data());
      Plane wire0{}, wire1{}, physicalBase{};
      // Typed selector truth table: white=11, light=01, dark=10, black=00.
      // Polarity complements BOTH absolute selectors, then the wire transport
      // inverts them. This oracle never calls/copies the driver's folding code.
      constexpr bool selector0[] = {true, false, true, false};
      constexpr bool selector1[] = {true, true, false, false};
      for (size_t byte = 0; byte < bw.size(); ++byte) {
        assert(!(lsb[byte] & ~msb[byte]) && !(msb[byte] & bw[byte]));
        for (unsigned mask = 1; mask <= 128; mask <<= 1) {
          const unsigned tone = (bw[byte] & mask) ? 0 : (msb[byte] & mask) ? ((lsb[byte] & mask) ? 2 : 1) : 3;
          if (selector0[tone] == dark) wire0[byte] |= mask;
          if (selector1[tone] == dark) wire1[byte] |= mask;
        }
        physicalBase[byte] = dark ? static_cast<uint8_t>(~bw[byte]) : bw[byte];
      }
      checkPlane(bus, 0x10, wire0);
      checkPlane(bus, 0x13, wire1);
      display.displayGrayBuffer(false, nullptr, false);
      const auto activations =
          std::count_if(bus.writes.begin(), bus.writes.end(), [](const auto& write) { return write.command == 0x12; });
      assert(activations == 1);
      nativeLut(bus, dark);
      checkPlane(bus, 0x10, physicalBase);
      checkPlane(bus, 0x13, physicalBase);
      assert(std::equal(physicalBase.begin(), physicalBase.end(), driver._grayBase));
      assert(std::equal(previous.begin(), previous.end(), display.getFrameBuffer()));
      assert(std::equal(bw.begin(), bw.end(), display.frameBufferActive));
      display.cleanupGrayscaleBuffers(bw.data());
      assert(std::equal(bw.begin(), bw.end(), display.getFrameBuffer()));
      assert(std::equal(bw.begin(), bw.end(), display.frameBufferActive));
      ++pages;
    }
  }
  display.releaseBuffers();
  std::free(driver._grayBase);
  driver._grayBase = nullptr;
  std::cout << "Actual renderer -> complete SDK -> UC8279 dark/light wire: fixtures=" << paths.size()
            << " pages=" << pages
            << ", exact native LUTs, complement selectors, gate offset, one AA activation, "
               "physical/controller and logical/host baselines passed\n";
}
