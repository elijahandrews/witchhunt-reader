#pragma once
#include "activities/Activity.h"
namespace TouchActionButtons {
struct Layout {
  Rect box;
};
inline Layout vertical(Rect r, int) { return {r}; }
inline int indexAt(Layout l, int x, int y) {
  return x >= l.box.x && x < l.box.x + l.box.width && y >= l.box.y && y < l.box.y + l.box.height ? 0 : -1;
}
inline void draw(const GfxRenderer& r, Layout, const char* const* s, int) { r.footer = *s; }
}  // namespace TouchActionButtons
