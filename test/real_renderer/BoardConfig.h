#pragma once
namespace BoardConfig {
struct ViewableInsets {
  int top, right, bottom, left;
};
struct Config {
  ViewableInsets viewableInsets;
};
inline constexpr Config ACTIVE{{9, 3, 3, 3}};
} // namespace BoardConfig
