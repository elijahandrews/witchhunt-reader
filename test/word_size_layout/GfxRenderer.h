#pragma once
// Host-test shim for GfxRenderer — resolves ahead of lib/GfxRenderer/GfxRenderer.h
// via include-path order (this directory is listed before lib/GfxRenderer, which is
// deliberately NOT on the include path of the WordSizeLayoutTest target).
//
// Implements exactly the measurement/draw surface that ParsedText.cpp and
// TextBlock.cpp use, with deterministic fixed-width metrics:
//   - every codepoint advances GLYPH_W pixels (scaled by the per-word scale)
//   - space advance is SPACE_W, kerning is always 0
//   - ascender is ASCENDER, scaled variants multiply and round
// drawText/drawTextScaled calls are recorded so tests can assert baseline
// alignment and per-word scales.

#include <EpdFontFamily.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer {
 public:
  bool scanning = false;
  mutable int spacedMeasurements = 0;
  struct TextFont {
    int fontId;
    float scale;
  };
  TextFont resolveTextFont(int fontId, uint8_t family) const {
    return {family == 2 ? -2000000 : (family == 1 ? -1000000 : fontId), 1.0f};
  }
  int getTextAdvanceXSpaced(int fontId, const char* text, EpdFontFamily::Style style, float scale,
                            int16_t tracking) const {
    ++spacedMeasurements;
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

  static constexpr int GLYPH_W = 10;
  static constexpr int SPACE_W = 5;
  static constexpr int ASCENDER = 16;
  static constexpr int LINE_HEIGHT = 20;

  struct DrawCall {
    std::string text;
    int fontId;
    int x;
    int y;
    float scale;
    EpdFontFamily::Style style;
  };
  mutable std::vector<DrawCall> drawCalls;

  struct RectCall {
    int x;
    int y;
    int w;
    int h;
    bool state;
  };
  mutable std::vector<RectCall> fillRectCalls;

  static int countCodepoints(const char* text) {
    int count = 0;
    for (const auto* p = reinterpret_cast<const unsigned char*>(text); *p != 0; ++p) {
      if ((*p & 0xC0) != 0x80) ++count;  // count UTF-8 lead bytes
    }
    return count;
  }

  // --- Measurement surface used by ParsedText ---
  int getSpaceWidth(int /*fontId*/, EpdFontFamily::Style /*style*/ = EpdFontFamily::REGULAR) const { return SPACE_W; }
  int getTextAdvanceX(int /*fontId*/, const char* text, EpdFontFamily::Style /*style*/) const {
    return countCodepoints(text) * GLYPH_W;
  }
  int getSpaceAdvance(int /*fontId*/, uint32_t /*leftCp*/, uint32_t /*rightCp*/, EpdFontFamily::Style /*style*/) const {
    return SPACE_W;
  }
  int getKerning(int /*fontId*/, uint32_t /*leftCp*/, uint32_t /*rightCp*/, EpdFontFamily::Style /*style*/) const {
    return 0;
  }
  int getFontAscenderSize(int /*fontId*/) const { return ASCENDER; }
  int getFontAscenderSizeScaled(int /*fontId*/, const float scale) const {
    return static_cast<int>(ASCENDER * scale + 0.5f);
  }
  int getLineHeight(int /*fontId*/) const { return LINE_HEIGHT; }
  int getLineHeightScaled(int /*fontId*/, const float scale) const {
    return static_cast<int>(LINE_HEIGHT * scale + 0.5f);
  }
  void ensureFontReady(int /*fontId*/, const char* /*utf8Text*/) const {}

  // --- Render surface used by TextBlock ---
  bool isFontCacheScanning() const { return scanning; }
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const {
    return getTextAdvanceX(fontId, text, style);
  }
  int getTextWidthScaled(int fontId, const char* text, EpdFontFamily::Style style, const float scale) const {
    return static_cast<int>(getTextAdvanceX(fontId, text, style) * scale + 0.5f);
  }
  void drawText(int fontId, int x, int y, const char* text, bool /*black*/ = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR) const {
    drawCalls.push_back({text, fontId, x, y, 1.0f, style});
  }
  void drawTextScaled(int fontId, int x, int y, const char* text, bool /*black*/, EpdFontFamily::Style style,
                      const float scale) const {
    drawCalls.push_back({text, fontId, x, y, scale, style});
  }
  void drawLine(int /*x1*/, int /*y1*/, int /*x2*/, int /*y2*/, int /*lineWidth*/, bool /*state*/) const {}
  void fillRect(int x, int y, int width, int height, bool state = true) const {
    fillRectCalls.push_back({x, y, width, height, state});
  }
};
