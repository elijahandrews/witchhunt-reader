
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Gpos.h"
using namespace freeink::font::gpos;
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

int main() { parserChecks(); }
