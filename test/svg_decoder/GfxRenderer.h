#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

enum Color : uint8_t { Clear = 0, White = 1, LightGray = 5, DarkGray = 10, Black = 16 };
class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };
  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };
  mutable std::array<uint8_t, 64 * 48 / 8> frame{};
  RenderMode mode = BW;
  Orientation orientation = LandscapeCounterClockwise;
  int origin = 0, rows = 48;
  GfxRenderer() { frame.fill(255); }
  uint8_t* getWriteTarget() const { return frame.data(); }
  int getWriteOriginY() const { return origin; }
  int getWriteRows() const { return rows; }
  RenderMode getRenderMode() const { return mode; }
  uint16_t getDisplayWidthBytes() const { return 8; }
  int getDisplayWidth() const { return 64; }
  int getDisplayHeight() const { return 48; }
  int getScreenWidth() const { return orientation == Portrait || orientation == PortraitInverted ? 48 : 64; }
  int getScreenHeight() const { return orientation == Portrait || orientation == PortraitInverted ? 64 : 48; }
  Orientation getOrientation() const { return orientation; }
};
