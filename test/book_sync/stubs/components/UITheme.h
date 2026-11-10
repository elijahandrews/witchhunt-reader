#pragma once
#include "activities/Activity.h"
struct EpdFontFamily {
  static constexpr int REGULAR = 0;
};
struct UITheme {
  static UITheme& getInstance() {
    static UITheme t;
    return t;
  }
  Rect getScreenSafeArea(const GfxRenderer&, bool, bool) { return {0, 0, 400, 800}; }
  static int drawCenteredWrappedText(GfxRenderer& r, Rect, int, int, const char* s, int, bool, int, int) {
    r.drawCenteredText(0, 0, s);
    return 20;
  }
};
struct Gui {
  void drawHeader(GfxRenderer&, Rect, const char*) {}
  void drawButtonHints(GfxRenderer&, const char*, const char*, const char*, const char*) {}
};
inline Gui GUI;
