#pragma once
#include <cstdint>
struct FakeClock {
  bool isAvailable() { return false; }
  bool getDateTime(uint16_t&, uint8_t&, uint8_t&, uint8_t&, uint8_t&) { return false; }
};
inline FakeClock halClock;
