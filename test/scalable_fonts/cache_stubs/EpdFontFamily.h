#pragma once
struct EpdFont {
  unsigned points = 0;
};
struct EpdFontFamily {
  const EpdFont* faces[4];
  EpdFontFamily(const EpdFont* a, const EpdFont* b, const EpdFont* c, const EpdFont* d) : faces{a, b, c, d} {}
};
