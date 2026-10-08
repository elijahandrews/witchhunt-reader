#include <driver/Uc8279X4Driver.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
int main(int argc, char** argv) {
  assert(argc == 2);
  using namespace freeink;
  EpdBus bus;
  Uc8279X4Driver driver;
  driver.begin(bus);
  size_t cases = 0, abortCases = 0, directCases = 0;
  assert(driver.geometry().width == 800 && driver.geometry().height == 480);
  assert(driver.grayscaleCapabilities().encoding == GrayscaleEncoding::OverlayMasks);
  for (const auto& entry : std::filesystem::directory_iterator(argv[1])) {
    if (entry.path().filename().string().find("driver-fixture-") != 0 || entry.path().extension() != ".bin") continue;
    std::array<uint8_t, 48000> base, lsb, msb;
    std::ifstream f(entry.path(), std::ios::binary);
    for (auto* plane : {&base, &lsb, &msb}) {
      f.read(reinterpret_cast<char*>(plane->data()), plane->size());
      assert(f.gcount() == static_cast<std::streamsize>(plane->size()));
    }
    assert(f.peek() == std::char_traits<char>::eof());
    driver.beginGrayscale(bus, base.data(), GrayscaleMode::Overlay, RefreshMode::Half, false);
    driver.copyGrayscaleLsb(bus, lsb.data());
    driver.copyGrayscaleMsb(bus, msb.data());
    assert(bus.oldPlane.size() == 60000 && bus.newPlane.size() == 60000);
    for (size_t i = 0; i < 12000; ++i) {
      assert(bus.oldPlane[i] == 255 && bus.newPlane[i] == 255);
    }
    for (size_t i = 0; i < base.size(); ++i) {
      const uint8_t p0 = base[i] | lsb[i], p1 = p0 ^ msb[i];
      assert(bus.oldPlane[i + 12000] == static_cast<uint8_t>(~p0));
      assert(bus.newPlane[i + 12000] == static_cast<uint8_t>(~p1));
    }
    driver.displayGray(bus, base.data(), false, nullptr, false);
    assert(bus.luts.size() == 5);
    assert(bus.luts[0][0] == 1 && bus.luts[0][1] == 2 && bus.luts[0][2] == 3);
    assert(bus.luts[2][2] == 0x83);
    for (size_t i = 0; i < 12000; ++i) {
      assert(bus.oldPlane[i] == 255 && bus.newPlane[i] == 255);
    }
    for (size_t i = 0; i < base.size(); ++i) {
      assert(bus.oldPlane[i + 12000] == base[i]);
      assert(bus.newPlane[i + 12000] == base[i]);
    }
    ++cases;
    // The same actual-renderer selectors can drive a complete Direct frame.
    // Preparation/upload must issue zero refreshes, followed by exactly one
    // DRF. Consecutive pages must not sneak in a BW recovery/base activation.
    for (bool turnOff : {false, true}) {
      auto absoluteL = lsb, absoluteM = msb;
      for (size_t i = 0; i < base.size(); ++i) {
        absoluteL[i] = base[i] | lsb[i];
        absoluteM[i] = absoluteL[i] ^ msb[i];
      }
      const auto beforeDirect = bus.refreshes;
      driver.beginGrayscale(bus, base.data(), GrayscaleMode::Direct, RefreshMode::Full, turnOff);
      driver.copyGrayscaleLsb(bus, absoluteL.data());
      driver.copyGrayscaleMsb(bus, absoluteM.data());
      assert(bus.refreshes == beforeDirect);
      for (size_t i = 0; i < 12000; ++i) assert(bus.oldPlane[i] == 255 && bus.newPlane[i] == 255);
      for (size_t i = 0; i < base.size(); ++i) {
        assert(bus.oldPlane[i + 12000] == static_cast<uint8_t>(~absoluteL[i]));
        assert(bus.newPlane[i + 12000] == static_cast<uint8_t>(~absoluteM[i]));
      }
      driver.displayGray(bus, base.data(), turnOff, nullptr, true);
      assert(bus.refreshes == beforeDirect + 1);
      // Mirror the full facade's post-Direct state invalidation.
      driver.requestResync(1);
      ++directCases;
    }

    // Prepared-mask cancellation can leave zero, one or both selector planes
    // staged. It must never refresh them, and reseeding the baseline must make
    // the following ordinary page turn correct in every case.
    for (int uploaded : {0, 1, 2}) {
      driver.display(bus, base.data(), base.data(), RefreshMode::Fast, false);
      const unsigned refreshesBefore = bus.refreshes;
      if (uploaded >= 1) driver.copyGrayscaleLsb(bus, lsb.data());
      if (uploaded >= 2) driver.copyGrayscaleMsb(bus, msb.data());
      driver.cleanupGrayscaleBuffers(bus, base.data());
      assert(bus.refreshes == refreshesBefore);
      assert(bus.oldPlane.size() == 60000);
      for (size_t i = 0; i < base.size(); ++i) assert(bus.oldPlane[i + 12000] == base[i]);
      auto next = base;
      for (size_t i = 0; i < next.size(); ++i) next[i] ^= static_cast<uint8_t>(0xA5u + i);
      assert(driver.displayStart(bus, next.data(), base.data(), RefreshMode::Fast, false));
      driver.displayFinish(bus, next.data());
      assert(bus.refreshes == refreshesBefore + 1);
      assert(bus.oldPlane.size() == 60000 && bus.newPlane.size() == 60000);
      for (size_t i = 0; i < 12000; ++i) assert(bus.oldPlane[i] == 255 && bus.newPlane[i] == 255);
      for (size_t i = 0; i < next.size(); ++i) {
        assert(bus.oldPlane[i + 12000] == next[i]);
        assert(bus.newPlane[i + 12000] == next[i]);
      }
      ++abortCases;
    }
  }
  assert(cases == 60);
  assert(abortCases == 180);
  assert(directCases == 120);
  std::cout << "Actual UC8279X4 Direct selectors/no-BW/single-activation checks=" << directCases << " passed\n";
  std::cout << "Actual UC8279X4 prepared-abort/no-refresh/next-BW checks=" << abortCases << " passed\n";
  std::cout << "Actual UC8279X4 wire inversion, gate offset, LUT68 and restored BW checks=" << cases << " passed\n";
}
