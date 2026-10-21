#include <ft2build.h>

#include "Gpos.h"
#include "OutlineFontFace.h"
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace witchoutline::font::gpos;
using Bytes = std::vector<uint8_t>;
static unsigned checks;
static void check(bool value) {
  ++checks;
  if (!value) std::fprintf(stderr, "check %u failed\n", checks);
  assert(value);
}
static void u16(Bytes& b, uint16_t n) {
  b.push_back(n >> 8);
  b.push_back(n);
}
static void u32(Bytes& b, uint32_t n) {
  u16(b, n >> 16);
  u16(b, n);
}
static void set16(Bytes& b, size_t at, uint16_t n) {
  b.at(at) = n >> 8;
  b.at(at + 1) = n;
}
static void set32(Bytes& b, size_t at, uint32_t n) {
  set16(b, at, n >> 16);
  set16(b, at + 2, n);
}
static void words(Bytes& b, std::initializer_list<uint16_t> list) {
  for (auto n : list) u16(b, n);
}
static Bytes pair(unsigned format, bool deviceOnly = false, bool extension = false, unsigned outer = 0,
                  unsigned inner = 0) {
  Bytes p;
  const unsigned vf = deviceOnly ? 0x40 : 0x44;
  size_t deviceOffset, record;
  if (format == 1) {
    words(p, {1, 0, uint16_t(vf), 0, 1, 12});
    words(p, {1, 2});
    record = p.size();
    if (!deviceOnly) u16(p, uint16_t(-20));
    deviceOffset = p.size();
    u16(p, 0);
    set16(p, deviceOffset, p.size() - 12);
  } else {
    words(p, {2, 0, uint16_t(vf), 0, 0, 0, 1, 1});
    record = p.size();
    if (!deviceOnly) u16(p, uint16_t(-20));
    deviceOffset = p.size();
    u16(p, 0);
    set16(p, deviceOffset, p.size());
  }
  (void)record;
  words(p, {uint16_t(outer), uint16_t(inner), 0x8000});
  set16(p, 2, p.size());
  words(p, {1, 1, 1});
  if (format == 2) {
    set16(p, 8, p.size());
    words(p, {1, 1, 1, 0});
    set16(p, 10, p.size());
    words(p, {1, 2, 1, 0});
  }
  if (extension) {
    Bytes wrapper;
    words(wrapper, {1, 2});
    u32(wrapper, 8);
    wrapper.insert(wrapper.end(), p.begin(), p.end());
    p = wrapper;
  }
  Bytes g;
  words(g, {1, 0, 10, 30, 44});
  u16(g, 1);
  u32(g, 0x6c61746e);
  words(g, {8, 4, 0, 0, 0xffff, 1, 0});
  u16(g, 1);
  u32(g, 0x6b65726e);
  words(g, {8, 0, 1, 0});
  words(g, {1, 4, uint16_t(extension ? 9 : 2), 0, 1, 8});
  g.insert(g.end(), p.begin(), p.end());
  return g;
}
using Axis = std::array<int16_t, 3>;
using Region = std::vector<Axis>;
static Bytes gdef(const std::vector<Region>& regions, const std::vector<int32_t>& deltas, unsigned wide,
                  bool longWords = false) {
  assert(!regions.empty() && regions.size() == deltas.size());
  Bytes b;
  words(b, {1, 3, 0, 0, 0, 0, 0});
  u32(b, 18);
  u16(b, 1);
  u32(b, 12);
  u16(b, 1);
  u32(b, 0);
  words(b, {uint16_t(regions[0].size()), uint16_t(regions.size())});
  for (const auto& region : regions)
    for (const auto& axis : region)
      for (auto value : axis) u16(b, uint16_t(value));
  set32(b, 26, b.size() - 18);
  words(b, {1, uint16_t(wide | (longWords ? 0x8000 : 0)), uint16_t(regions.size())});
  for (unsigned i = 0; i < regions.size(); ++i) u16(b, i);
  for (unsigned i = 0; i < deltas.size(); ++i) {
    if (longWords && i < wide)
      u32(b, uint32_t(deltas[i]));
    else if (longWords || i < wide)
      u16(b, uint16_t(deltas[i]));
    else
      b.push_back(uint8_t(deltas[i]));
  }
  return b;
}
static void parserChecks() {
  const Region positive{{0, 16384, 16384}};
  auto def = gdef({positive}, {-100}, 1);
  int32_t coord = 16384;
  VariationContext context{def.data(), def.size(), &coord, 1};
  for (unsigned format : {1u, 2u})
    for (bool extension : {false, true})
      for (bool only : {false, true}) {
        auto pos = pair(format, only, extension);
        check(PairKernAdjustment(pos.data(), pos.size(), 1, 2, &context) == (only ? -25 : -45));
        check(PairKernAdjustment(pos.data(), pos.size(), 1, 2) == (only ? 0 : -20));
        for (auto sample : std::vector<std::array<int32_t, 2>>{{0, 0}, {32768, -50}, {65536, -100}, {-32768, 0}}) {
          coord = sample[0];
          check(PairKernAdjustment(pos.data(), pos.size(), 1, 2, &context) == sample[1] + (only ? 0 : -20));
        }
        coord = 16384;
      }
  auto pos = pair(1);
  auto evaluate = [&](Bytes data, const std::vector<int32_t>& coords) {
    VariationContext c{data.data(), data.size(), coords.data(), coords.size()};
    return PairKernAdjustment(pos.data(), pos.size(), 1, 2, &c);
  };
  check(evaluate(gdef({{{0, 16384, 16384}, {-16384, -16384, 0}}}, {-100}, 1), {32768, -32768}) == -45);
  check(evaluate(gdef({positive, {{0, 8192, 16384}}, {{0, 0, 0}}}, {-100, 12, -4}, 1), {32768}) == -62);
  check(evaluate(gdef({positive}, {-1}, 0), {32768}) == -20);  // -0.5 rounds to 0
  check(evaluate(gdef({positive}, {1}, 0), {32768}) == -19);
  check(evaluate(gdef({positive, positive}, {70000, -300}, 1, true), {65536}) == 69680);
  check(evaluate(gdef({positive, positive}, {INT32_MAX, INT32_MAX}, 2, true), {65536}) == INT32_MAX);
  check(evaluate(gdef({positive, positive}, {INT32_MIN, INT32_MIN}, 2, true), {65536}) == INT32_MIN);
  check(evaluate(gdef({positive}, {-100}, 1), {65537}) == -20);
  check(evaluate(gdef({positive}, {-100}, 1), {0, 0}) == -20);
  for (auto indices : std::vector<std::array<unsigned, 2>>{{1, 0}, {0, 1}, {0xffff, 0xffff}}) {
    auto bad = pair(1, false, false, indices[0], indices[1]);
    check(PairKernAdjustment(bad.data(), bad.size(), 1, 2, &context) == -20);
  }
  auto badWords = def;
  set16(badWords, 42, 2);
  check(evaluate(badWords, {16384}) == -20);
  auto nullData = def;
  set32(nullData, 26, 0);
  check(evaluate(nullData, {16384}) == -20);
  auto badRegion = def;
  set16(badRegion, 46, 1);
  check(evaluate(badRegion, {16384}) == -20);
  auto hugeMatrix = pair(2);
  set16(hugeMatrix, 56 + 12, 0xffff);
  set16(hugeMatrix, 56 + 14, 0xffff);
  check(PairKernAdjustment(hugeMatrix.data(), hugeMatrix.size(), 1, 2, &context) == 0);
  // Each byte prefix and single-byte corruption is parsed under ASan/UBSan.
  for (unsigned format : {1u, 2u})
    for (bool extension : {false, true}) {
      auto bytes = pair(format, false, extension);
      for (size_t n = 0; n < bytes.size(); ++n) {
        (void)PairKernAdjustment(bytes.data(), n, 1, 2, &context);
        ++checks;
      }
      for (size_t n = 0; n < def.size(); ++n) {
        auto c = context;
        c.gdefSize = n;
        (void)PairKernAdjustment(bytes.data(), bytes.size(), 1, 2, &c);
        ++checks;
      }
      for (size_t n = 0; n < bytes.size(); ++n)
        for (uint8_t mask : {uint8_t(0x80), uint8_t(0xff)}) {
          auto corrupted = bytes;
          corrupted[n] ^= mask;
          (void)PairKernAdjustment(corrupted.data(), corrupted.size(), 1, 2, &context);
          ++checks;
        }
      for (size_t n = 0; n < def.size(); ++n)
        for (uint8_t mask : {uint8_t(0x80), uint8_t(0xff)}) {
          auto corrupted = def;
          corrupted[n] ^= mask;
          (void)evaluate(corrupted, {16384});
          ++checks;
        }
    }
  std::vector<Region> huge(2049, positive);
  std::vector<int32_t> deltas(2049, 1);
  check(evaluate(gdef(huge, deltas, 0), {32768}) == 0);  // shared work budget, no partial result
  check(PairKernAdjustment(nullptr, 10, 1, 2, &context) == 0);
}
struct Bank {
  size_t used = 0, claims = 0, fail = 0;
  bool rejected = false;
  static bool claim(void* p, size_t n) {
    auto& b = *static_cast<Bank*>(p);
    if (++b.claims == b.fail) {
      b.rejected = true;
      return false;
    }
    b.used += n;
    return true;
  }
  static void release(void* p, size_t n) {
    auto& b = *static_cast<Bank*>(p);
    check(b.used >= n);
    b.used -= n;
  }
};
static Bytes read(const char* path) {
  FILE* f = fopen(path, "rb");
  assert(f);
  fseek(f, 0, SEEK_END);
  Bytes b(ftell(f));
  rewind(f);
  assert(fread(b.data(), 1, b.size(), f) == b.size());
  fclose(f);
  return b;
}
static size_t readStream(void* c, uint32_t off, uint8_t* dst, size_t bytes) {
  auto& b = *static_cast<Bytes*>(c);
  if (off > b.size() || bytes > b.size() - off) return 0;
  memcpy(dst, b.data() + off, bytes);
  return bytes;
}
static size_t tableSize(const Bytes& bytes, uint32_t tag) {
  auto word = [&](size_t at) { return unsigned(bytes.at(at)) * 256 + bytes.at(at + 1); };
  auto longword = [&](size_t at) { return uint32_t(word(at)) << 16 | word(at + 2); };
  for (unsigned i = 0; i < word(4); ++i)
    if (longword(12 + size_t(i) * 16) == tag) return longword(24 + size_t(i) * 16);
  return 0;
}
static void exportCoordinates(const char* path, const char* output) {
  // Independent public FreeType calls export the precise avar-mapped instance.
  // FontTools static instancing quantizes these to F2DOT14, losing precision
  // around half-unit kerning boundaries; its VarStoreInstancer does not.
  auto data = read(path);
  FT_Library library = nullptr;
  FT_Face face = nullptr;
  FT_MM_Var* axes = nullptr;
  check(!FT_Init_FreeType(&library));
  check(!FT_New_Memory_Face(library, data.data(), data.size(), 0, &face));
  check(!FT_Get_MM_Var(face, &axes));
  std::vector<FT_Fixed> coords(axes->num_axis);
  FILE* out = fopen(output, "w");
  assert(out);
  for (int weight : {100, 250, 400, 450, 500, 525, 550, 650, 700, 775, 800, 900}) {
    for (unsigned i = 0; i < axes->num_axis; ++i) {
      const auto& axis = axes->axis[i];
      FT_Fixed value = axis.def;
      if (axis.tag == FT_MAKE_TAG('w', 'g', 'h', 't')) value = FT_Fixed(weight) * 65536;
      if (axis.tag == FT_MAKE_TAG('o', 'p', 's', 'z')) value = 16 * 65536;
      if (axis.tag == FT_MAKE_TAG('i', 't', 'a', 'l')) value = 0;
      coords[i] = std::clamp(value, axis.minimum, axis.maximum);
    }
    check(!FT_Set_Var_Design_Coordinates(face, axes->num_axis, coords.data()));
    check(!FT_Get_Var_Blend_Coordinates(face, axes->num_axis, coords.data()));
    for (unsigned i = 0; i < axes->num_axis; ++i) {
      const auto tag = axes->axis[i].tag;
      fprintf(out, "%d,%c%c%c%c,%ld\n", weight, int(tag >> 24) & 255, int(tag >> 16) & 255, int(tag >> 8) & 255,
              int(tag) & 255, long(coords[i]));
    }
  }
  fclose(out);
  FT_Done_MM_Var(library, axes);
  FT_Done_Face(face);
  FT_Done_FreeType(library);
}
static void faceChecks(const char* fixture, const char* oracle) {
  auto bytes = read(fixture);
  FILE* f = fopen(oracle, "r");
  assert(f);
  int weight, left, right, expected, lastWeight = -1;
  OutlineFontFace face;
  OutlineFontFace::Options o;
  o.logicalPx = 31.25f;
  o.opticalPoints = 16;
  o.hinting = OutlineFontFace::Hinting::None;
  size_t pairs = 0;
  while (fscanf(f, "%d,%d,%d,%d\n", &weight, &left, &right, &expected) == 4) {
    if (weight != lastWeight) {
      o.weight = weight;
      check(face.openMemory(bytes.data(), bytes.size(), o));
      lastWeight = weight;
    }
    const int actual = face.kerning(left, right);
    if (actual != expected)
      std::fprintf(stderr, "weight=%d gids=%d,%d expected=%d actual=%d\n", weight, left, right, expected, actual);
    check(actual == expected);
    ++pairs;
  }
  fclose(f);
  check(pairs > 0);
  face.close();
  check(face.memoryUsed() == 0);
  // Stream source and a final-size raster must retain the same variation/layout
  // instance.
  o.weight = 650;
  check(face.openStream(readStream, &bytes, bytes.size(), o));
  const auto a = face.glyphId('A'), v = face.glyphId('V');
  const auto before = face.kerning(a, v);
  EpdRasterGlyph raster;
  check(face.raster(a, .5f, 3, &raster));
  check(face.kerning(a, v) == before);
  face.close();
  check(face.memoryUsed() == 0);
  const size_t tables = tableSize(bytes, 0x47535542) + tableSize(bytes, 0x47504f53) + tableSize(bytes, 0x47444546);
  o.tableLimit = tables;
  check(face.openMemory(bytes.data(), bytes.size(), o));
  face.close();
  o.tableLimit = tables - 1;
  check(!face.openMemory(bytes.data(), bytes.size(), o));
  check(face.memoryUsed() == 0);
  o.tableLimit = 512 * 1024;
  o.weight = 400;
  check(face.openMemory(bytes.data(), bytes.size(), o));
  const auto defaultKern = face.kerning(a, v);
  face.close();
  o.applyVariations = false;
  o.weight = 900;
  check(face.openMemory(bytes.data(), bytes.size(), o));
  check(face.kerning(a, v) == defaultKern);
  face.close();
  o.loadLayoutTables = false;
  o.tableLimit = 0;
  check(face.openMemory(bytes.data(), bytes.size(), o));
  face.close();
  check(face.memoryUsed() == 0);
  o.applyVariations = true;
  o.loadLayoutTables = true;
  o.tableLimit = 512 * 1024;
  o.weight = 650;
  Bank bank;
  o.allocationContext = &bank;
  o.claimMemory = Bank::claim;
  o.releaseMemory = Bank::release;
  check(face.openMemory(bytes.data(), bytes.size(), o));
  const size_t allocations = bank.claims;
  for (int i = 0; i < 1000; ++i) (void)face.kerning(a, v);
  check(bank.claims == allocations);
  face.close();
  check(bank.used == 0);
  for (size_t fail = 1; fail <= allocations; ++fail) {
    bank = {};
    bank.fail = fail;
    const bool opened = face.openMemory(bytes.data(), bytes.size(), o);
    if (bank.rejected) check(!opened);
    face.close();
    check(bank.used == 0 && face.memoryUsed() == 0);
  }
  std::printf("font oracle: %zu pairs; %zu allocation-failure points\n", pairs, allocations);
}
static void extremeFaceChecks(const char* fixture, int expected) {
  const auto bytes = read(fixture);
  OutlineFontFace face;
  OutlineFontFace::Options options;
  options.weight = 900;
  for (float pixels : {1.0f, 31.25f, 120.0f}) {
    options.logicalPx = pixels;
    check(face.openMemory(bytes.data(), bytes.size(), options));
    const auto h = face.glyphId('H'), t = face.glyphId('T');
    check(h && t);
    check(face.kerning(h, t) == expected);
    check(face.kerning(t, h) == expected);
    check(face.kerning(h, h) == 0);
    face.close();
    check(face.memoryUsed() == 0);
  }
}
int main(int argc, char** argv) {
  if (argc == 4 && !strcmp(argv[1], "--coordinates")) {
    exportCoordinates(argv[2], argv[3]);
    return 0;
  }
  parserChecks();
  assert(argc >= 3 && argc % 2 == 1);
  for (int i = 1; i < argc; i += 2) {
    if (!strcmp(argv[i], "--extreme-positive"))
      extremeFaceChecks(argv[i + 1], INT16_MAX);
    else if (!strcmp(argv[i], "--extreme-negative"))
      extremeFaceChecks(argv[i + 1], INT16_MIN);
    else
      faceChecks(argv[i], argv[i + 1]);
  }
  std::printf("%u variable GPOS/parser/avar/stream/OOM checks passed\n", checks);
}
