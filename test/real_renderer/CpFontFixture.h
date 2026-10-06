#pragma once

// Host-only fixture adapter. Read the user's local v4 bitmap file into the
// production font structs; no font bytes are stored in this repository.
// This deliberately does not emulate SD caching, prewarming, or flash mapping.
#include <EpdFont.h>

#include <array>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

class CpFontFixture {
  std::vector<uint8_t> bytes;
  uint32_t read(size_t at, int count) const {
    if (at + count > bytes.size()) throw std::runtime_error("Truncated cpfont fixture");
    uint32_t value = 0;
    for (int i = 0; i < count; ++i) value |= uint32_t(bytes[at + i]) << (8 * i);
    return value;
  }
  template <class T>
  void copy(std::vector<T>& target, size_t& at, size_t count) {
    if (at > bytes.size() || count > (bytes.size() - at) / sizeof(T))
      throw std::runtime_error("Invalid cpfont section");
    target.resize(count);
    if (count) std::memcpy(target.data(), bytes.data() + at, count * sizeof(T));
    at += count * sizeof(T);
  }

 public:
  struct Face {
    std::vector<EpdUnicodeInterval> intervals;
    std::vector<EpdGlyph> glyphs;
    std::vector<EpdKernClassEntry> left, right;
    std::vector<int8_t> matrix;
    std::vector<EpdLigaturePair> ligatures;
    EpdFontData data{};
    std::unique_ptr<EpdFont> font;
  };
  std::array<Face, 4> faces;
  explicit CpFontFixture(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open cpfont fixture");
    bytes.assign(std::istreambuf_iterator<char>(in), {});
    if (bytes.size() < 160 || std::memcmp(bytes.data(), "CPFONT\0\0", 8) || read(8, 2) != 4 || read(10, 2) != 1 ||
        read(12, 1) != 4)
      throw std::runtime_error("Fixture requires a four-face 2-bit v4 cpfont");
    static_assert(sizeof(EpdGlyph) == 16 && sizeof(EpdUnicodeInterval) == 12 && sizeof(EpdKernClassEntry) == 3 &&
                  sizeof(EpdLigaturePair) == 8);
    for (size_t i = 0; i < 4; ++i) {
      const size_t t = 32 + i * 32;
      if (read(t, 1) != i) throw std::runtime_error("Unexpected cpfont face order");
      auto& f = faces[i];
      auto& d = f.data;
      size_t at = read(t + 24, 4);
      copy(f.intervals, at, read(t + 4, 4));
      copy(f.glyphs, at, read(t + 8, 4));
      copy(f.left, at, read(t + 17, 2));
      copy(f.right, at, read(t + 19, 2));
      copy(f.matrix, at, read(t + 21, 1) * read(t + 22, 1));
      copy(f.ligatures, at, read(t + 23, 1));
      for (const auto& g : f.glyphs)
        if (size_t(g.dataOffset) + g.dataLength > bytes.size() - at)
          throw std::runtime_error("Invalid cpfont glyph bitmap");
      d.bitmap = bytes.data() + at;
      d.glyph = f.glyphs.data();
      d.intervals = f.intervals.data();
      d.intervalCount = f.intervals.size();
      d.advanceY = read(t + 12, 1);
      d.ascender = int16_t(read(t + 13, 2));
      d.descender = int16_t(read(t + 15, 2));
      d.is2Bit = true;
      d.kernLeftClasses = f.left.data();
      d.kernRightClasses = f.right.data();
      d.kernMatrix = f.matrix.data();
      d.kernLeftEntryCount = f.left.size();
      d.kernRightEntryCount = f.right.size();
      d.kernLeftClassCount = read(t + 21, 1);
      d.kernRightClassCount = read(t + 22, 1);
      d.ligaturePairs = f.ligatures.data();
      d.ligaturePairCount = f.ligatures.size();
      f.font = std::make_unique<EpdFont>(&d);
    }
  }
  EpdFontFamily family() const {
    return EpdFontFamily(faces[0].font.get(), faces[1].font.get(), faces[2].font.get(), faces[3].font.get());
  }
};
