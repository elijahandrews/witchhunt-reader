#include <CpFontFormat.h>
#include <EpdFontFamily.h>
#include <HalStorage.h>
#include <SdCardFont.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {
void put(std::vector<uint8_t>& b, size_t at, uint32_t value, size_t n) {
  for (size_t i = 0; i < n; ++i) b[at + i] = value >> (i * 8);
}
void append(std::vector<uint8_t>& b, const void* data, size_t n) {
  const auto* p = static_cast<const uint8_t*>(data);
  b.insert(b.end(), p, p + n);
}
void checksum(std::vector<uint8_t>& b) {
  put(b, 14, cpfont::crc32Update(0xFFFFFFFF, b.data() + 32, b.size() - 32) ^ 0xFFFFFFFF, 4);
}
EpdGlyph rawGlyph(int left = -3, int top = 3, unsigned w = 5, unsigned h = 4) {
  EpdGlyph g{};
  std::memset(&g, 0, sizeof(g));
  g.width = w;
  g.height = h;
  g.left = left;
  g.top = top;
  g.advanceX = 321;
  g.dataLength = (w * h + 3) / 4;
  auto* bytes = reinterpret_cast<uint8_t*>(&g);
  bytes[10] = w >> 8;
  bytes[11] = h >> 8;
  return g;
}
std::vector<uint8_t> fixture(unsigned version = 6) {
  std::vector<uint8_t> b(64);
  std::memcpy(b.data(), "CPFONT\0\0", 8);
  put(b, 8, version, 2);
  b[10] = 1;
  b[12] = 1;
  b[13] = version >= 5 ? 2 : 0;
  put(b, 36, 1, 4);
  put(b, 40, 2, 4);
  b[44] = 43;
  put(b, 45, 35, 2);
  put(b, 47, uint16_t(-9), 2);
  put(b, 49, 1, 2);
  put(b, 51, 1, 2);
  b[53] = b[54] = 1;
  put(b, 56, 64, 4);
  EpdUnicodeInterval interval{'a', 'b', 0};
  append(b, &interval, sizeof(interval));
  auto g = rawGlyph();
  append(b, &g, sizeof(g));
  g.dataOffset = g.dataLength;
  append(b, &g, sizeof(g));
  EpdKernClassEntry left{'a', 1}, right{'b', 1};
  append(b, &left, 3);
  append(b, &right, 3);
  const int16_t kern = -300;
  append(b, &kern, version >= 5 ? 2 : 1);
  const uint8_t pixels[]{0x1b, 0x6c, 0xb1, 0xc6, 0xff};
  append(b, pixels, sizeof(pixels));
  append(b, pixels, sizeof(pixels));
  if (version == 6) {
    const uint32_t offset = b.size();
    put(b, 60, offset, 4);
    b.resize(b.size() + 24);
    std::memcpy(b.data() + offset, "SCAP", 4);
    b[offset + 4] = 1;
    put(b, offset + 6, 1, 2);
    put(b, offset + 10, 1, 2);
    put(b, offset + 16, 1, 4);
    put(b, offset + 20, sizeof(pixels), 4);
    const size_t map = b.size();
    b.resize(map + 6);
    put(b, map, 'a', 4);
    g = rawGlyph();
    g.advanceX = 401;
    append(b, &g, sizeof(g));
    const size_t pair = b.size();
    b.resize(pair + 10);
    put(b, pair, 0x110000, 4);
    put(b, pair + 4, 'b', 4);
    put(b, pair + 8, uint16_t(-400), 2);
    append(b, pixels, sizeof(pixels));
  }
  if (version >= 5) checksum(b);
  return b;
}
struct File {
  std::filesystem::path path;
  explicit File(const std::vector<uint8_t>& bytes) {
    static unsigned serial = 0;
    path = std::filesystem::temp_directory_path() / ("crossink-hd-" + std::to_string(++serial) + ".cpfont");
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  ~File() { std::filesystem::remove(path); }
};
void assertPixels(const EpdGlyph& g, const uint8_t* bitmap) {
  assert(g.width == 3 && g.height == 3 && g.left == -2 && g.top == 2 && g.advanceX == 160);
  // Independent per-cell integration in common baseline coordinates.
  const unsigned source[4][5] = {{0, 1, 2, 3, 1}, {2, 3, 0, 2, 3}, {0, 1, 3, 0, 1}, {2, 3, 3, 3, 3}};
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 3; ++x) {
      unsigned sum = 0;
      for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
          const int sx = x * 2 - 1 + dx, sy = y * 2 - 1 + dy;
          if (sx >= 0 && sx < 5 && sy >= 0 && sy < 4) sum += source[sy][sx];
        }
      const unsigned lower = sum / 4, rem = sum % 4;
      const unsigned expected = lower + (rem > 2 || (rem == 2 && (lower % 2)));
      const int i = y * 3 + x;
      assert(((bitmap[i / 4] >> (6 - 2 * (i % 4))) & 3) == expected);
    }
}
void production() {
  for (unsigned version : {5u, 6u}) {
    File file(fixture(version));
    SdCardFont font;
    assert(font.load(file.path.c_str()));
    auto* face = font.getEpdFont();
    assert(face->data->advanceY == 22 && face->data->ascender == 18 && face->data->descender == -5);
    const auto* glyph = face->getGlyph('a');
    assert(glyph);
    assertPixels(*glyph, font.getOverflowBitmap(glyph));
    assert(fp5::advance(*glyph) == 321);
    assert(font.prewarm("ab", 1, true) == 1);
    assert(face->getGlyph('a')->advanceX == 160);
    assert(font.prewarm("ab", 1, false) == 1);
    glyph = face->getGlyph('a');
    assertPixels(*glyph, face->data->bitmap + glyph->dataOffset);
    assert(face->getKerning('a', 'b') == -150);
    assert(face->getKerningFP5('a', 'b') == -300);
    assert(font.buildAdvanceTable("ab", 1) == 0);
    assert(font.getAdvance('a', 0) == 160);
    font.clearCache();
    assert(font.getAdvance('a', 0) == 160);
    if (version == 6) {
      EpdFontFamily family(face);
      assert(family.smallCapsCodepoint('a', EpdFontFamily::SMALL_CAPS) == 0x110000);
      assert(family.smallCapsCodepoint('b', EpdFontFamily::SMALL_CAPS) == 0);
      const auto* cap = face->getGlyph(0x110000);
      assert(cap && cap->advanceX == 200);
      assert(face->getKerning(0x110000, 'b') == -200);
      assert(face->hasCodepoint(0x110000));
      assert(font.prewarm("ab", 1, false, true) == 1);
      assert(face->getKerningFP5(0x110000, 'b') == -400);
      const auto* warmedCap = face->getGlyph(0x110000);
      assert(warmedCap && !font.getOverflowBitmap(warmedCap));
      assert(font.prewarm("ab", 1, false, false) == 1);
      assert(face->getKerningFP5(0x110000, 'b') == 0);
      assert(font.prewarm("ab", 1, false, true) == 1);
      assert(face->getKerningFP5(0x110000, 'b') == -400);
    }
  }
}
void invalid() {
  auto bytes = fixture();
  bytes.back() ^= 1;
  {
    File file(bytes);
    SdCardFont f;
    assert(!f.load(file.path.c_str()));
  }
  bytes = fixture();
  bytes[13] = 3;
  {
    File file(bytes);
    SdCardFont f;
    assert(!f.load(file.path.c_str()));
  }
  bytes = fixture();
  put(bytes, 60, 1, 4);
  checksum(bytes);
  {
    File file(bytes);
    SdCardFont f;
    assert(!f.load(file.path.c_str()));
  }
  bytes = fixture();
  bytes[76 + 10] = 255;
  checksum(bytes);
  {
    File file(bytes);
    SdCardFont f;
    assert(!f.load(file.path.c_str()));
  }
}
void leanHd() {
  auto b = fixture(5);
  auto g = rawGlyph(0, 140, 140, 140);  // 4900 source bytes, 1225 logical bytes.
  std::memcpy(b.data() + 76, &g, sizeof(g));
  g.dataOffset = g.dataLength;
  std::memcpy(b.data() + 92, &g, sizeof(g));
  b.resize(116 + size_t(g.dataLength) * 2, 0xff);
  checksum(b);
  File file(b);
  SdCardFont font;
  assert(font.load(file.path.c_str(), 65536));
  const auto* glyph = font.getEpdFont()->getGlyph('a');
  assert(glyph && glyph->width == 70 && glyph->height == 70 && glyph->dataLength == 1225);
  assert(font.getOverflowBitmap(glyph));
}
void hdReadLifetime() {
  File file(fixture());
  SdCardFont font;
  assert(font.load(file.path.c_str()));
  EpdGlyph raw{};
  const uint8_t* pixels = nullptr;
  assert(font.readHdGlyph('a', 0, raw, pixels, true));
  assert(pixels && raw.width == 5 && openFiles == 0);
  failNextClose = true;
  assert(!font.readHdGlyph('a', 0, raw, pixels, true));
  assert(!pixels && openFiles == 0 && !failNextClose);
  failNextClose = true;
  assert(!font.readHdGlyph('a', 0, raw, pixels, false));
  assert(!pixels && openFiles == 0 && !failNextClose);
  failReadCall = readCalls;
  assert(!font.readHdGlyph('a', 0, raw, pixels, true));
  assert(!pixels && openFiles == 0);
  failReadCall = -1;
  assert(font.readHdGlyph('a', 0, raw, pixels, true));
  assert(pixels && openFiles == 0);
}
void geometry() {
  for (int left : {-5, -2, -1, 0, 1, 2, 5})
    for (int top : {-3, -1, 0, 1, 2, 5}) {
      auto raw = rawGlyph(left, top);
      EpdGlyph logical{};
      assert(cpfont::logicalGlyph(raw, logical));
      assert(logical.left * 2 <= left && (logical.left + logical.width) * 2 >= left + 5);
      assert(logical.top * 2 >= top && (logical.top - logical.height) * 2 <= top - 4);
    }
  auto raw = rawGlyph(0, 2, 300, 2);
  EpdGlyph logical{};
  assert(cpfont::logicalGlyph(raw, logical) && logical.width == 150);
  raw = rawGlyph(0, 2, 600, 2);
  assert(!cpfont::logicalGlyph(raw, logical));
  for (int n = -200; n <= 200; ++n) assert(cpfont::halfEven(-n) == -cpfont::halfEven(n));
}
}  // namespace
int main() {
  geometry();
  production();
  invalid();
  leanHd();
  hdReadLifetime();
  assert(openFiles == 0);
}
