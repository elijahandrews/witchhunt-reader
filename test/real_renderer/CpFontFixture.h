#pragma once
// Real production mmap loader, backed by bytes read from an external fixture.
#include <EpdFontFamily.h>
#include <SdCardFont.h>

#include <fstream>
#include <stdexcept>
#include <vector>
class CpFontFixture {
  std::vector<uint8_t> bytes;
  SdCardFont font;

 public:
  uint8_t density = 1;
  float rasterScale() const { return 1.0f / density; }
  explicit CpFontFixture(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open cpfont fixture");
    bytes.assign(std::istreambuf_iterator<char>(in), {});
    if (!font.loadFromMmap(bytes.data(), bytes.size(), path) || font.styleCount() != 4)
      throw std::runtime_error("Expected a valid four-face cpfont");
    density = font.rasterDensity();
  }
  EpdFontFamily family() {
    return EpdFontFamily(font.getEpdFont(0), font.getEpdFont(1), font.getEpdFont(2), font.getEpdFont(3));
  }
};
