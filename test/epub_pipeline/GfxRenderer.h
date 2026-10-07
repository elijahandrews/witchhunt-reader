#pragma once
// Host-test GfxRenderer stub for the full-pipeline harness (test/epub_pipeline).
//
// Unlike the minimal gif_decoder stub, this one implements the text-measurement
// API with DETERMINISTIC synthetic metrics so the real layout engine
// (Section/ParsedText/TextBlock/ChapterHtmlSlimParser) can run on the host and
// produce reproducible line breaks and word positions. The numbers do not match
// any real font; equivalence testing only requires that both sides of a
// comparison (two runs, or old-path vs new-path) use the same metrics.
//
// Metric model (all per codepoint, style-aware, kerning-free):
//   advance(cp)        = 6 + (cp % 7)            -> 6..12 px
//   advance(cp, BOLD)  = advance(cp) + 1
//   space width        = 6 px
//   line height        = 24 px, ascender = 18 px
// Scaled variants multiply and truncate toward zero, mirroring the integer
// snapping the device renderer applies.
#include <Arduino.h>
#include <EpdFontFamily.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

enum Color : uint8_t { Clear = 0x00, White = 0x01, LightGray = 0x05, DarkGray = 0x0A, Black = 0x10 };

class GfxRenderer {
 public:
  struct TextFont {
    int fontId;
    float scale;
  };
  uint32_t bookFontFailureEpoch() const { return 0; }
  std::function<uint8_t(uint8_t)> familyFallback;
  TextFont resolveTextFont(int fontId, uint8_t family, float = 1.0f) const {
    if (family >= 5 && familyFallback) family = familyFallback(family);
    return {family == 2 ? -2000000 : (family == 1 ? -1000000 : (family == 4 ? -3000000 : fontId)), 1.0f};
  }
  int getTextAdvanceXSpaced(int fontId, const char* text, EpdFontFamily::Style style, float scale,
                            int16_t tracking) const {
    int count = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p)
      if ((*p & 0xc0) != 0x80) ++count;
    const float familyScale = fontId == -2000000 ? 1.2f : 1.0f;
    const int width =
        text[0] == ' ' && text[1] == 0 ? getSpaceWidth(fontId, style) : getTextAdvanceX(fontId, text, style);
    return std::max(
        0, static_cast<int>(std::lround(width * scale * familyScale + std::max(0, count - 1) * tracking / 16.0f)));
  }
  int getTextWidthSpaced(int fontId, const char* text, EpdFontFamily::Style style, float scale,
                         int16_t tracking) const {
    return getTextAdvanceXSpaced(fontId, text, style, scale, tracking);
  }
  void drawTextSpaced(int fontId, int x, int y, const char* text, bool black, EpdFontFamily::Style style, float scale,
                      int16_t) const {
    drawTextScaled(fontId, x, y, text, black, style, scale);
  }

  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };

  static constexpr int VIEWABLE_MARGIN_TOP = 9;
  static constexpr int VIEWABLE_MARGIN_RIGHT = 3;
  static constexpr int VIEWABLE_MARGIN_BOTTOM = 3;
  static constexpr int VIEWABLE_MARGIN_LEFT = 3;

  // --- Screen / framebuffer surface (converters, DirectPixelWriter) ---
  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }
  int getDisplayWidth() const { return 480; }
  int getDisplayHeight() const { return 800; }
  uint16_t getDisplayWidthBytes() const { return 60; }
  Orientation getOrientation() const { return Portrait; }
  RenderMode getRenderMode() const { return BW; }
  uint8_t* getWriteTarget() const { return frameBuffer_; }
  int getWriteOriginY() const { return 0; }
  int getWriteRows() const { return 800; }
  void clearScreen(uint8_t = 0xFF) const {}

  // --- Drawing (no-ops: the harness never inspects pixels) ---
  void drawPixel(int, int, bool = true) const {}
  void drawLine(int, int, int, int, bool = true) const {}
  void drawLine(int, int, int, int, int, bool) const {}
  void drawRect(int, int, int, int, bool = true) const {}
  void drawRect(int, int, int, int, int, bool) const {}
  void fillRect(int, int, int, int, bool = true) const {}
  void fillRectDither(int, int, int, int, Color) const {}

  // --- Font lifecycle (built-in-font behavior: everything is always ready) ---
  bool isFontCacheScanning() const { return false; }
  void ensureFontReady(int, const char*) const {}
  void clearFontAccumulation() const {}
  void dropFontMetadata() const {}
  bool restoreFontMetadata() const { return true; }

  // --- Deterministic text metrics ---
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const {
    return getTextAdvanceX(fontId, text, style);
  }
  int getTextWidthScaled(int fontId, const char* text, EpdFontFamily::Style style, float scale) const {
    return static_cast<int>(getTextWidth(fontId, text, style) * scale);
  }
  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style) const {
    int w = 0;
    for (const uint8_t* p = reinterpret_cast<const uint8_t*>(text); *p;) {
      uint32_t cp;
      p = decodeUtf8(p, cp);
      w += fontId == -3000000 ? 9 : advance(cp, style);
    }
    return w;
  }
  int getSpaceWidth(int fontId, EpdFontFamily::Style = EpdFontFamily::REGULAR) const {
    return fontId == -3000000 ? 9 : 6;
  }
  int getSpaceAdvance(int fontId, uint32_t, uint32_t, EpdFontFamily::Style) const { return fontId == -3000000 ? 9 : 6; }
  int getKerning(int, uint32_t, uint32_t, EpdFontFamily::Style) const { return 0; }
  int getLineHeight(int fontId) const { return fontId == -2000000 ? 30 : 24; }
  int getLineHeightScaled(int fontId, float scale) const { return static_cast<int>(getLineHeight(fontId) * scale); }
  // Test seam for the image-header heap recovery. 0 (the default) models a host build with
  // no cache manager at all, so callers are told nothing was released. When set, this models
  // the real thing — dropping the SD-font glyph caches frees heap — by reporting success and
  // raising the simulated free heap to that figure.
  uint32_t heapAfterFontCacheRelease = 0;
  bool releaseFontCaches() {
    if (heapAfterFontCacheRelease == 0) return false;
    ESP.setFreeHeap(heapAfterFontCacheRelease);
    return true;
  }
  int getFontAscenderSize(int) const { return 18; }
  int getFontAscenderSizeScaled(int, float scale) const { return static_cast<int>(18 * scale); }
  int getTextHeight(int) const { return 24; }
  // Synthetic ink model: caps span 14 px above the baseline, nothing below
  // (matches the deterministic-metrics philosophy above; 18-14=4 px leading).
  bool getTextInkMetrics(int, const char* text, EpdFontFamily::Style, int* aboveBaseline, int* belowBaseline) const {
    *aboveBaseline = 0;
    *belowBaseline = 0;
    if (text == nullptr || *text == '\0') return false;
    *aboveBaseline = 14;
    return true;
  }

  // --- Text drawing (no-ops) ---
  void drawText(int, int, int, const char*, bool = true, EpdFontFamily::Style = EpdFontFamily::REGULAR) const {}
  void drawTextScaled(int, int, int, const char*, bool, EpdFontFamily::Style, float) const {}
  void drawCenteredText(int, int, const char*, bool = true, EpdFontFamily::Style = EpdFontFamily::REGULAR) const {}
  void drawTextRotated90CW(int, int, int, const char*, bool = true,
                           EpdFontFamily::Style = EpdFontFamily::REGULAR) const {}

 private:
  static int advance(const uint32_t cp, const EpdFontFamily::Style style) {
    const int base = 6 + static_cast<int>(cp % 7);
    // Style is a bitmask; only the BOLD bit affects width in this synthetic model.
    return (style & EpdFontFamily::BOLD) ? base + 1 : base;
  }
  // Minimal UTF-8 decode; invalid bytes consume one byte and yield U+FFFD so
  // malformed input still measures deterministically.
  static const uint8_t* decodeUtf8(const uint8_t* p, uint32_t& cp) {
    if (*p < 0x80) {
      cp = *p;
      return p + 1;
    }
    if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
      cp = ((*p & 0x1Fu) << 6) | (p[1] & 0x3Fu);
      return p + 2;
    }
    if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
      cp = ((*p & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
      return p + 3;
    }
    if ((*p & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
      cp = ((*p & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
      return p + 4;
    }
    cp = 0xFFFD;
    return p + 1;
  }

  // Converters write decoded pixels through getWriteTarget(); give them a real
  // buffer so those paths are memory-safe if exercised.
  mutable uint8_t frameBufferStorage_[60 * 800] = {};
  uint8_t* const frameBuffer_ = frameBufferStorage_;
};
