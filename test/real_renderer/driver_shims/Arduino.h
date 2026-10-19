#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#define HIGH 1
#define PROGMEM
#define log_e(...) ((void)0)
inline unsigned long millis() {
  static unsigned long t = 0;
  return ++t;
}
inline void delay(unsigned long) {}
inline int digitalRead(int) { return 0; }
