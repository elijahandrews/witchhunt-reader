#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
namespace freeink {
enum class BusyPolarity { X3TwoPhase, UcIdleHigh };
class EpdBus {
  uint8_t command = 0;

 public:
  std::vector<uint8_t> oldPlane, newPlane;
  std::vector<std::vector<uint8_t>> luts;
  unsigned refreshes = 0;
  void cmd(uint8_t c) {
    command = c;
    if (c == 0x12) ++refreshes;
    if (c == 0x10) oldPlane.clear();
    if (c == 0x13) newPlane.clear();
  }
  void data(uint8_t) {}
  void data(const uint8_t* p, size_t n) {
    if (n == 49 && command >= 0x20 && command <= 0x24) {
      if (command == 0x20) luts.clear();
      luts.emplace_back(p, p + n);
    }
  }
  void beginTxn() {}
  void endTxn() {}
  void rawWriteBytes(const uint8_t* p, size_t n) {
    if (command == 0x10) oldPlane.insert(oldPlane.end(), p, p + n);
    if (command == 0x13) newPlane.insert(newPlane.end(), p, p + n);
  }
  void fillPlane(uint8_t c, uint8_t v, uint16_t h, uint16_t wb) {
    cmd(c);
    (c == 0x10 ? oldPlane : newPlane).assign(size_t(h) * wb, v);
  }
  void reset(int) {}
  void waitBusy(const char*) {}
  void waitRefreshComplete(const char*) {}
  struct Pins {
    int8_t busy = 0;
  };
  Pins pins() { return {}; }
};
}  // namespace freeink
