#pragma once
#include <Print.h>
struct FakeESP {
  unsigned getFreeHeap() { return 1000000; }
  unsigned getMaxAllocHeap() { return 1000000; }
};
inline FakeESP ESP;
