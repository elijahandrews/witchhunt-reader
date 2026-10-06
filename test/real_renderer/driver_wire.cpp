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
  size_t cases = 0;
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
  }
  assert(cases == 60);
  std::cout << "Actual UC8279X4 wire inversion, gate offset, LUT68 and restored BW checks=" << cases << " passed\n";
}
