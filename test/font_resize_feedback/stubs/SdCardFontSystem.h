#pragma once
#include <cassert>
#include <vector>

#include "CrossPointSettings.h"
inline std::vector<int> events;
struct TestFonts {
  bool resident = true, effective = true, fail = false;
  bool canResizeResidentScalableFamilyWithoutReload(const char*) { return resident; }
  bool changeReaderFontSize(bool, FontSizeStepMode) {
    events.push_back(1);
    return effective && !fail;
  }
};
inline TestFonts sdFontSystem;
