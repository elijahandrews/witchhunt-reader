#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Gpos.h"
#include "LayoutFixtures.h"
#include "OpenTypeCaps.h"
#include "OutlineFontFace.h"
static size_t read(void* p, uint32_t off, uint8_t* b, size_t n) {
  FILE* f = (FILE*)p;
  if (fseek(f, off, SEEK_SET)) return 0;
  return fread(b, 1, n, f);
}
static std::vector<uint8_t> contents(const char* p) {
  FILE* f = fopen(p, "rb");
  assert(f);
  fseek(f, 0, SEEK_END);
  std::vector<uint8_t> b(ftell(f));
  rewind(f);
  assert(fread(b.data(), 1, b.size(), f) == b.size());
  fclose(f);
  return b;
}
static uint64_t sample(OutlineFontFace& face) {
  uint64_t hash = 0;
  unsigned glyphs = 0, caps = 0, kern = 0;
  for (uint32_t cp = 32; cp < 256; ++cp) {
    uint16_t gid = face.glyphId(cp);
    if (!gid) continue;
    auto m = face.metrics(gid);
    assert(m && m.index == gid && !m.sdRecord);
    size_t n;
    auto* p = face.bitmap(gid, &n);
    assert(n == (size_t(m.width) * m.height * 2 + 7) / 8);
    assert(!n || p);
    for (size_t i = 0; i < n; ++i) hash = hash * 131 + p[i];
    hash = hash * 131 + m.advanceX;
    assert(m.advanceX == face.metrics(gid).advanceX);
    ++glyphs;
    if (face.glyphId(cp, 1)) ++caps;
    if (face.kerning(gid, face.glyphId('A'))) ++kern;
  }
  printf("glyphs=%u smcp=%u kernPairs=%u hash=%llx memory=%zu peak=%zu\n", glyphs, caps, kern, (unsigned long long)hash,
         face.memoryUsed(), face.memoryPeak());
  assert(glyphs > 100 && caps > 0 && kern > 0);
  return hash;
}
struct AllocationBank {
  size_t used = 0, limit = 1024 * 1024;
  unsigned failures = 0, claims = 0, denyAt = 0;
  static bool claim(void* ctx, size_t n) {
    auto& bank = *static_cast<AllocationBank*>(ctx);
    if (++bank.claims == bank.denyAt || n > bank.limit - bank.used) return false;
    bank.used += n;
    return true;
  }
  static void release(void* ctx, size_t n) {
    auto& bank = *static_cast<AllocationBank*>(ctx);
    assert(n <= bank.used);
    bank.used -= n;
  }
  static void failed(void* ctx) { ++static_cast<AllocationBank*>(ctx)->failures; }
};
static void sharedBankChecks(const std::vector<uint8_t>& font) {
  // Reject a partially configured FT library even when its void module loader
  // swallows one failed allocation. Sweep startup and initial face/table work.
  for (unsigned allocation = 1; allocation < 50; ++allocation) {
    AllocationBank fault;
    fault.denyAt = allocation;
    OutlineFontFace probe;
    OutlineFontFace::Options denied;
    denied.allocationContext = &fault;
    denied.claimMemory = &AllocationBank::claim;
    denied.releaseMemory = &AllocationBank::release;
    denied.memoryFailure = &AllocationBank::failed;
    const bool opened = probe.openMemory(font.data(), font.size(), denied);
    if (fault.failures) assert(!opened);
    probe.close();
    assert(fault.used == 0);
  }
  AllocationBank bank;
  OutlineFontFace face;
  OutlineFontFace::Options options;
  options.allocationContext = &bank;
  options.claimMemory = &AllocationBank::claim;
  options.releaseMemory = &AllocationBank::release;
  options.memoryFailure = &AllocationBank::failed;
  bank.limit = 8192;
  assert(!face.openMemory(font.data(), font.size(), options));
  assert(bank.failures && bank.used == 0 && face.memoryUsed() == 0);
  bank.limit = 1024 * 1024;
  bank.failures = 0;
  assert(face.openMemory(font.data(), font.size(), options));
  assert(bank.used == face.memoryUsed() && bank.used > 8192);
  const auto before = bank.used;
  OutlineFontFace denied;
  bank.limit = bank.used;
  assert(!denied.openMemory(font.data(), font.size(), options));
  assert(bank.failures && bank.used == before && !denied.memoryUsed());
  bank.limit = 1024 * 1024;
  assert(face.metrics(face.glyphId('H')));
  face.close();
  assert(bank.used == 0);
  bank.limit = 1024 * 1024;
  assert(denied.openMemory(font.data(), font.size(), options));
  denied.close();
  assert(bank.used == 0);
}
int main(int argc, char** argv) {
  assert(freeink::font::gpos::PairKernAdjustment(kernChain, sizeof(kernChain), 1, 2) == -30);
  assert(outlineCapsSubstitution(capsChain, sizeof(capsChain), 1, 0x736d6370) == 4);
  assert(outlineCapsSubstitution(capsUnsupported, sizeof(capsUnsupported), 1, 0x736d6370) == 0);
  for (size_t n = 0; n < sizeof(capsChain); ++n) (void)outlineCapsSubstitution(capsChain, n, 1, 0x736d6370);
  for (size_t n = 0; n < sizeof(kernChain); ++n) (void)freeink::font::gpos::PairKernAdjustment(kernChain, n, 1, 2);

  assert(argc >= 3);
  for (int a = 1; a < argc; ++a) {
    auto b = contents(argv[a]);
    sharedBankChecks(b);
    OutlineFontFace face;
    OutlineFontFace::Options o;
    o.hinting = OutlineFontFace::Hinting::None;
    assert(face.openMemory(b.data(), b.size(), o));
    auto hash = sample(face);
    auto line = face.lineMetrics();
    assert(line.advanceY > 0 && line.advanceY <= 255);
    assert(!face.glyphId(0xffffffff) || false);
    assert(!face.glyphId('a', 3));
    assert(face.glyphId('a', 1) != face.glyphId('a'));
    assert(!face.glyphId('A', 1));
    assert(face.glyphId('A', 2) != face.glyphId('A'));
    uint16_t seq[2] = {face.glyphId('f'), face.glyphId('i')};
    assert(face.ligature(seq, 2) > 0);
    assert(!face.ligature(seq, 0));
    bool variableWeight = face.hasWeightAxis();
    assert(!face.hasItalicAxis());
    bool opsz = face.hasOpticalSize();
    float axis = face.opticalSize();
    face.close();
    assert(face.memoryUsed() == 0);
    FILE* f = fopen(argv[a], "rb");
    assert(face.openStream(read, f, b.size(), o));
    assert(sample(face) == hash);
    face.close();
    fclose(f);
    assert(face.memoryUsed() == 0);
    o.logicalPx = 0;
    assert(!face.openMemory(b.data(), b.size(), o));
    o.logicalPx = 10000;
    assert(!face.openMemory(b.data(), b.size(), o));
    o.logicalPx = 14;
    o.memoryLimit = 16384;
    assert(!face.openMemory(b.data(), b.size(), o));
    face.close();
    assert(face.memoryUsed() == 0);
    o.memoryLimit = 1024 * 1024;
    if (variableWeight) {
      o.weight = 700;
      assert(face.openMemory(b.data(), b.size(), o));
      assert(face.hasWeightAxis());
      assert(sample(face) != hash);
      face.close();
      o.weight = 400;
    }
    if (opsz) {
      o.opticalPoints = 28;
      assert(face.openMemory(b.data(), b.size(), o));
      assert(face.hasOpticalSize() && face.opticalSize() != axis);
      assert(sample(face) != hash);
      face.close();
    }
    uint8_t junk[64] = {};
    assert(!face.openMemory(junk, sizeof(junk), o));
    assert(face.memoryUsed() == 0);
  }
  puts(
      "PASS memory/stream identity, real caps/ligatures/kern, optical axes, malformed/size/memory rejection, zero "
      "leaked face allocations");
}
