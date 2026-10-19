#pragma once
#include_next <Arduino.h>
inline unsigned long micros() { return 0; }
