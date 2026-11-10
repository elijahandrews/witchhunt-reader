#pragma once

#include <cstdint>

struct EpdFontFamily {
  enum Style : uint8_t {
    REGULAR = 0,
    BOLD = 1,
    SUP = 16,
    SUB = 32,
  };
};
