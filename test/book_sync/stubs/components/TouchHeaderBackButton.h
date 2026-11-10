#pragma once
#include "activities/Activity.h"
namespace TouchHeaderBackButton {
inline Rect headerRect(const GfxRenderer&, const MappedInputManager&) { return {0, 0, 400, 80}; }
inline bool wasTapped(MappedInputManager&, GfxRenderer&) {
  if (injectTouch && tapX < 60 && tapY < 80) {
    injectTouch = false;
    return true;
  }
  return false;
}
inline void draw(GfxRenderer&, const Rect&, const char*, bool) {}
}  // namespace TouchHeaderBackButton
