#pragma once
struct FontSystem {
  template <typename T>
  void releaseForNetwork(T&) {}
};
inline FontSystem sdFontSystem;
