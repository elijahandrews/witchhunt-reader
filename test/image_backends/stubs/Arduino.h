#pragma once
#include <algorithm>
#include <cstdint>
struct EspHostStub {
  uint32_t getFreeHeap() const { return UINT32_MAX; }
  uint32_t getMaxAllocHeap() const { return UINT32_MAX; }
};
inline EspHostStub ESP;
inline uint32_t testClock = 0, testClockStep = 0, testYields = 0;
inline uint32_t millis() {
  const auto n = testClock;
  testClock += testClockStep;
  return n;
}
inline void vTaskDelay(uint32_t) { ++testYields; }
inline void delay(uint32_t) {}
