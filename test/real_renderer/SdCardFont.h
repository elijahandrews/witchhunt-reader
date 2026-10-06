#pragma once
#include <EpdFontData.h>
class SdCardFont {
public:
  static SdCardFont *fromMissCtx(void *) { return nullptr; }
  bool isOverflowGlyph(const EpdGlyph *) { return false; }
  const uint8_t *getOverflowBitmap(const EpdGlyph *) { return nullptr; }
  int prewarm(const char *, uint8_t, bool = false, bool = false) { return 0; }
  void clearAccumulation() {}
  void unloadMetadata() {}
  bool reloadMetadata() { return true; }
};
