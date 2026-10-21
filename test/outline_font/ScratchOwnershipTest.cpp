// Exercise the production backend's retained FreeType scratch ownership and OOM paths.
#include <ft2build.h>
#include "afmodule.h"
#include "OutlineFontFace.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

struct Bank {
  size_t used = 0;
  unsigned claims = 0, deny = 0, failures = 0;
  std::vector<size_t> largeClaims;
  static bool claim(void* context, size_t bytes) {
    auto& b = *static_cast<Bank*>(context);
    if (++b.claims == b.deny) return false;
    b.used += bytes;
    if (bytes >= 12000) b.largeClaims.push_back(bytes);
    return true;
  }
  static void release(void* context, size_t bytes) {
    auto& b = *static_cast<Bank*>(context);
    assert(b.used >= bytes);
    b.used -= bytes;
  }
  static void failure(void* context) { ++static_cast<Bank*>(context)->failures; }
  void bind(OutlineFontFace::Options& o) {
    o.allocationContext = this;
    o.claimMemory = claim;
    o.releaseMemory = release;
    o.memoryFailure = failure;
    o.loadLayoutTables = false;
    o.applyVariations = false;
  }
};
static std::vector<uint8_t> read(const char* path) {
  FILE* f = fopen(path, "rb");
  assert(f);
  assert(fseek(f, 0, SEEK_END) == 0);
  std::vector<uint8_t> bytes(ftell(f));
  rewind(f);
  assert(fread(bytes.data(), 1, bytes.size(), f) == bytes.size());
  fclose(f);
  return bytes;
}
static void add(uint64_t& hash, uint64_t value) { hash = hash * 131 + value; }
static uint64_t glyphHash(OutlineFontFace& face, unsigned cp) {
  const auto id = face.glyphId(cp);
  if (!id) return 0;
  const auto m = face.metrics(id);
  assert(m);
  uint64_t hash = 1;
  add(hash, m.width); add(hash, m.height); add(hash, m.left); add(hash, m.top); add(hash, m.advanceX);
  size_t count;
  const auto* pixels = face.bitmap(id, &count);
  for (size_t i = 0; i < count; ++i) add(hash, pixels[i]);
  for (int phase = -8; phase < 8; ++phase) {
    EpdRasterGlyph r;
    assert(face.raster(id, .5f, int8_t(phase), &r));
    add(hash, r.metrics.width); add(hash, r.metrics.height); add(hash, r.metrics.left); add(hash, r.metrics.top);
    for (size_t i = 0; i < r.bytes; ++i) add(hash, r.bitmap[i]);
  }
  return hash;
}
static unsigned probeCodepoint(OutlineFontFace& face) {
  return face.glyphId(0xC1) ? 0xC1 : (face.glyphId('A') ? 'A' : 'H');
}
int main(int argc, char** argv) {
  // First: invalid nested-seac fixture. Second: valid counter-mask/seac fixture.
  assert(argc >= 3);
  unsigned checks = 0;
  {
    auto bytes = read(argv[1]);
    Bank bank;
    OutlineFontFace::Options o;
    bank.bind(o);
    OutlineFontFace face;
    assert(face.openMemory(bytes.data(), bytes.size(), o));
    const auto before = glyphHash(face, 'I');
    assert(!face.metrics(face.glyphId(0xC1)));
    EpdRasterGlyph raster;
    assert(!face.raster(face.glyphId(0xC1), .5f, 0, &raster));
    assert(glyphHash(face, 'I') == before);
    face.close();
    assert(!bank.used);
    ++checks;
  }
  for (int arg = 2; arg < argc; ++arg) {
    auto bytes = read(argv[arg]);
    uint64_t combined = 0;
    for (auto hint : {OutlineFontFace::Hinting::None, OutlineFontFace::Hinting::Native,
                      OutlineFontFace::Hinting::Light}) {
      for (float size : {7.f, 14.f, 21.f}) {
        OutlineFontFace face;
        Bank bank;
        OutlineFontFace::Options o;
        bank.bind(o);
        o.hinting = hint;
        o.logicalPx = size;
        assert(face.openMemory(bytes.data(), bytes.size(), o));
        for (unsigned cp = 32; cp < 256; ++cp) {
          add(combined, glyphHash(face, cp));
          ++checks;
        }
        face.close();
        assert(!bank.used);
      }
    }
    printf("font=%d raster-metrics-hash=%016llx\n", arg - 1, (unsigned long long)combined);
    // Includes module creation, lazy interpreter/hinter scratch, and component
    // scratch allocation. After denial, supported glyphs must recover safely.
    for (unsigned fault = 1; fault <= 100; ++fault) {
      if (std::getenv("STACK_SKIP_FAILURES")) break;  // pre-fix parity runs only
      Bank bank;
      bank.deny = fault;
      OutlineFontFace::Options o;
      bank.bind(o);
      OutlineFontFace face;
      if (face.openMemory(bytes.data(), bytes.size(), o)) {
        const auto id = face.glyphId(probeCodepoint(face));
        assert(id);
        face.metrics(id);
        EpdRasterGlyph raster;
        face.raster(id, .5f, -5, &raster);
        bank.deny = 0;
        assert(face.metrics(id));
        assert(face.raster(id, .5f, -5, &raster));
      }
      face.close();
      assert(!bank.used);
      ++checks;
    }
    {
      Bank bank;
      OutlineFontFace::Options o;
      bank.bind(o);
      OutlineFontFace face;
      assert(face.openMemory(bytes.data(), bytes.size(), o));
      for (unsigned cp = 32; cp < 256; ++cp) glyphHash(face, cp);
      const auto before = bank.largeClaims;
      for (int pass = 0; pass < 3; ++pass)
        for (unsigned cp = 32; cp < 256; ++cp) glyphHash(face, cp);
      // The synthetic CFF fixture has fixed size and exercises both seac slots:
      // no large allocation may recur. Real variable fonts retain their existing
      // transient allocations, so separately check the autofitter scratch size.
      if (arg == 2) assert(bank.largeClaims == before);
      const size_t hintScratch = sizeof(AF_ModuleScratchRec);
      assert(std::count(bank.largeClaims.begin(), bank.largeClaims.end(), hintScratch) ==
             std::count(before.begin(), before.end(), hintScratch));
      face.close();
      assert(!bank.used);
    }
    // Each OutlineFontFace owns its FT_Library. Concurrent distinct faces must
    // not share raster cells or either interpreter/hinter scratch workspace.
    auto worker = [&](float size) {
      OutlineFontFace face;
      Bank bank;
      OutlineFontFace::Options o;
      bank.bind(o);
      o.logicalPx = size;
      assert(face.openMemory(bytes.data(), bytes.size(), o));
      const auto cp = probeCodepoint(face);
      const auto expected = glyphHash(face, cp);
      for (int pass = 0; pass < 100; ++pass) {
        assert(glyphHash(face, cp) == expected);
        if (face.glyphId('I')) glyphHash(face, 'I');
      }
      face.close();
      assert(!bank.used);
    };
    std::thread first(worker, 10.f), second(worker, 21.f);
    first.join();
    second.join();
  }
  printf("%u stack-scratch checks plus concurrent face isolation passed\n", checks);
}
