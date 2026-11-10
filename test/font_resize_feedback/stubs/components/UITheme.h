#pragma once
#include "SdCardFontSystem.h"
#include "activities/RenderLock.h"
struct TestGui {
  void drawPopup(GfxRenderer&, const char*, bool preserve) {
    assert(locked && preserve);
    events.push_back(2);
  }
};
inline TestGui GUI;
