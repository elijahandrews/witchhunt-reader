#pragma once
namespace BoardConfig {
inline bool isX4Classic() { return false; }
struct ViewableInsets {
  int top, right, bottom, left;
};
struct Config {
  ViewableInsets viewableInsets;
  unsigned short displayWidth = 800, displayHeight = 480;
  unsigned displaySpiHz = 20000000;
  unsigned char displayControllerVariant = 0x68;
};
inline constexpr Config ACTIVE{{9, 3, 3, 3}};
}  // namespace BoardConfig
