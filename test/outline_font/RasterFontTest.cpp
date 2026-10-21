#include <ft2build.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include "OutlineFontFace.h"

static unsigned checks = 0;
static std::vector<uint8_t> contents(const char* path) {
  FILE* f = fopen(path, "rb");
  assert(f);
  assert(fseek(f, 0, SEEK_END) == 0);
  std::vector<uint8_t> bytes(ftell(f));
  rewind(f);
  assert(fread(bytes.data(), 1, bytes.size(), f) == bytes.size());
  fclose(f);
  return bytes;
}
static size_t streamRead(void* p, uint32_t offset, uint8_t* dst, size_t count) {
  const auto& bytes = *static_cast<std::vector<uint8_t>*>(p);
  if (offset > bytes.size() || count > bytes.size() - offset) return 0;
  memcpy(dst, bytes.data() + offset, count);
  return count;
}
struct Bank {
  size_t used = 0, peak = 0, limit = 1024 * 1024;
  unsigned claims = 0, failures = 0, denyAt = 0;
  static bool claim(void* p, size_t n) {
    auto& b = *static_cast<Bank*>(p);
    if (++b.claims == b.denyAt || n > b.limit - b.used) return false;
    b.used += n;
    b.peak = std::max(b.peak, b.used);
    return true;
  }
  static void release(void* p, size_t n) {
    auto& b = *static_cast<Bank*>(p);
    assert(n <= b.used);
    b.used -= n;
  }
  static void failure(void* p) { ++static_cast<Bank*>(p)->failures; }
  void bind(OutlineFontFace::Options& o) {
    o.allocationContext = this;
    o.claimMemory = claim;
    o.releaseMemory = release;
    o.memoryFailure = failure;
  }
};
struct Snapshot {
  EpdGlyphRef metrics{};
  std::vector<uint8_t> pixels;
};
static bool sameMetrics(const EpdGlyphRef& a, const EpdGlyphRef& b) {
  return a.valid == b.valid && a.sdRecord == b.sdRecord && a.index == b.index && a.width == b.width &&
         a.height == b.height && a.left == b.left && a.top == b.top && a.advanceX == b.advanceX;
}
static uint16_t probeGlyph(OutlineFontFace& face) {
  const auto gid = face.glyphId('A') ? face.glyphId('A') : face.glyphId('H');
  assert(gid);
  return gid;
}
static Snapshot saveLegacy(OutlineFontFace& face, uint16_t gid) {
  Snapshot s;
  s.metrics = face.metrics(gid);
  size_t n = 0;
  const auto* p = face.bitmap(gid, &n);
  if (n) s.pixels.assign(p, p + n);
  return s;
}
static void expectLegacy(OutlineFontFace& face, uint16_t gid, const Snapshot& s) {
  const auto current = saveLegacy(face, gid);
  assert(sameMetrics(current.metrics, s.metrics) && current.pixels == s.pixels);
  ++checks;
}
static Snapshot saveRaster(const EpdRasterGlyph& raster) {
  Snapshot s;
  s.metrics = raster.metrics;
  if (raster.bytes) s.pixels.assign(raster.bitmap, raster.bitmap + raster.bytes);
  return s;
}
// Independent path: load the hinted outline without FT_Set_Transform or RENDER,
// translate it explicitly, then render and scatter 8bit source coverage into
// destination cells. Production uses a separate FT_Size and gathers coverage.
static Snapshot reference(FT_Face face, uint16_t gid, float scale, const OutlineFontFace::Options& o, int phase) {
  const int density = o.rasterPolicy == OutlineFontFace::RasterPolicy::Oversample2x ? 2 : 1;
  const int em = int(lround(double(o.logicalPx) * 128.0 * scale));
  assert(!FT_Set_Char_Size(face, 0, em * density, 72, 72));
  FT_Int32 flags = FT_LOAD_NO_BITMAP;
  if (o.hinting == OutlineFontFace::Hinting::None) flags |= FT_LOAD_NO_HINTING | FT_LOAD_NO_AUTOHINT;
  if (o.hinting == OutlineFontFace::Hinting::Native) flags |= FT_LOAD_NO_AUTOHINT;
  if (o.hinting == OutlineFontFace::Hinting::Light) flags |= FT_LOAD_TARGET_LIGHT;
  assert(!FT_Load_Glyph(face, gid, flags));
  const int step = 16 / o.phaseSteps;
  const int rounded = int(std::round(double(phase) / step)) * step;
  FT_Outline_Translate(&face->glyph->outline, rounded * 4 * density, 0);
  assert(!FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL));
  const auto& b = face->glyph->bitmap;
  const int left = int(std::floor(double(face->glyph->bitmap_left) / density));
  const int upper = int(std::floor(double(-face->glyph->bitmap_top) / density));
  const int width = b.width ? int(std::ceil(double(face->glyph->bitmap_left + int(b.width)) / density)) - left : 0;
  const int height = b.rows ? int(std::ceil(double(-face->glyph->bitmap_top + int(b.rows)) / density)) - upper : 0;
  std::vector<unsigned> accumulated(size_t(width) * height);
  for (unsigned y = 0; y < b.rows; ++y) {
    const auto* row = b.buffer + (b.pitch >= 0 ? y : b.rows - 1 - y) * std::abs(b.pitch);
    const int dy = int(std::floor(double(int(y) - face->glyph->bitmap_top) / density)) - upper;
    for (unsigned x = 0; x < b.width; ++x) {
      const int dx = int(std::floor(double(int(x) + face->glyph->bitmap_left) / density)) - left;
      accumulated[size_t(dy) * width + dx] += row[x];
    }
  }
  Snapshot s;
  s.metrics = {nullptr, 0, gid, uint16_t(width), uint16_t(height), int16_t(left), int16_t(-upper), true};
  s.pixels.resize((accumulated.size() + 3) / 4);
  for (size_t n = 0; n < accumulated.size(); ++n) {
    const unsigned value = unsigned(std::lround(double(accumulated[n]) * 3.0 / (255 * density * density)));
    s.pixels[n / 4] |= uint8_t(value << (6 - 2 * (n % 4)));
  }
  return s;
}
static void expectRaster(OutlineFontFace& face, FT_Face ref, uint16_t gid, float scale, int phase,
                         const OutlineFontFace::Options& o) {
  EpdRasterGlyph actual;
  assert(face.raster(gid, scale, int8_t(phase), &actual));
  assert(actual.metrics && actual.bytes == (size_t(actual.metrics.width) * actual.metrics.height + 3) / 4);
  assert(bool(actual.bytes) == bool(actual.bitmap));
  const auto expected = reference(ref, gid, scale, o, phase);
  if (!sameMetrics(actual.metrics, expected.metrics) ||
      (actual.bytes && memcmp(actual.bitmap, expected.pixels.data(), actual.bytes))) {
    fprintf(stderr, "raster mismatch gid=%u scale=%g phase=%d steps=%d policy=%d\n", gid, scale, phase, o.phaseSteps,
            int(o.rasterPolicy));
    abort();
  }
  ++checks;
}
static void testPhases(std::vector<uint8_t>& font) {
  FT_Library library;
  assert(!FT_Init_FreeType(&library));
  FT_Face ref;
  assert(!FT_New_Memory_Face(library, font.data(), font.size(), 0, &ref));
  assert(!FT_Select_Charmap(ref, FT_ENCODING_UNICODE));
  for (auto policy : {OutlineFontFace::RasterPolicy::FinalSize, OutlineFontFace::RasterPolicy::Oversample2x}) {
    for (int steps : {4, 8, 16}) {
      for (auto hinting :
           {OutlineFontFace::Hinting::Light, OutlineFontFace::Hinting::Native, OutlineFontFace::Hinting::None}) {
        OutlineFontFace::Options o;
        o.rasterPolicy = policy;
        o.phaseSteps = steps;
        o.hinting = hinting;
        o.applyVariations = false;
        o.loadLayoutTables = false;
        Bank bank;
        bank.bind(o);
        OutlineFontFace face;
        assert(face.openStream(streamRead, &font, uint32_t(font.size()), o));
        const uint16_t a = probeGlyph(face), v = face.glyphId('V');
        const auto saved = saveLegacy(face, a);
        const auto kern = face.kerning(a, v);
        const auto line = face.lineMetrics();
        for (float scale : {0.25f, 0.375f, 0.5f, 0.75f, 1.123f}) {
          static constexpr uint32_t samples[] = {' ', 's', 't', 'i', 'l', 'N', 'T', 'j', 'f', 'W', 0x00E9, 0x00C5};
          for (uint32_t cp : samples) {
            const uint16_t gid = face.glyphId(cp);
            if (!gid) continue;
            for (int phase = -8; phase < 8; ++phase) {
              expectRaster(face, ref, gid, scale, phase, o);
              assert(face.kerning(a, v) == kern);
              expectLegacy(face, a, saved);
            }
          }
        }
        assert(face.lineMetrics().ascender == line.ascender && face.lineMetrics().advanceY == line.advanceY);
        EpdRasterGlyph actual;
        assert(face.raster(a, 0.5f, 2, &actual));
        const auto raster = saveRaster(actual);
        const auto claims = bank.claims;
        assert(face.raster(a, 0.5f, 2, &actual));
        assert(bank.claims == claims);
        expectLegacy(face, a, saved);
        // Legacy operations must not invalidate the borrowed phase pixels.
        assert(!memcmp(actual.bitmap, raster.pixels.data(), actual.bytes));
        assert(bank.used == face.memoryUsed() && bank.peak <= o.memoryLimit);
        face.close();
        assert(!bank.used && !face.memoryUsed());
      }
    }
  }
  FT_Done_Face(ref);
  FT_Done_FreeType(library);
}
static void testCacheAndFailure(std::vector<uint8_t>& font) {
  OutlineFontFace::Options o;
  o.applyVariations = false;
  o.loadLayoutTables = false;
  o.phaseSteps = 16;
  FT_Library library;
  assert(!FT_Init_FreeType(&library));
  FT_Face ref;
  assert(!FT_New_Memory_Face(library, font.data(), font.size(), 0, &ref));
  assert(!FT_Select_Charmap(ref, FT_ENCODING_UNICODE));
  Bank bank;
  bank.bind(o);
  OutlineFontFace face;
  assert(face.openMemory(font.data(), font.size(), o));
  const auto gid = probeGlyph(face);
  const auto saved = saveLegacy(face, gid);
  for (unsigned pass = 0; pass < 3; ++pass) {
    for (uint32_t cp = 33; cp < 127; ++cp) {
      const auto id = face.glyphId(cp);
      if (!id) continue;
      for (int phase = -8; phase < 8; ++phase) expectRaster(face, ref, id, 0.5f, phase, o);
    }
    // 1504 unique keys exceed the bounded512entry metadata; large rasters also
    // exercise32KiB arena wrap independently of metadata capacity.
    for (uint32_t cp = 'A'; cp <= 'Z'; ++cp) {
      const auto id = face.glyphId(cp);
      if (id) expectRaster(face, ref, id, 2.5f, int(cp % 16) - 8, o);
    }
    expectRaster(face, ref, gid, 0.5f, -8, o);
    expectLegacy(face, gid, saved);
  }
  EpdRasterGlyph invalid;
  for (float scale :
       {0.0f, -1.0f, 0.001f, 1e9f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
    assert(!face.raster(gid, scale, 0, &invalid) && !invalid.metrics && !invalid.bitmap && !invalid.bytes);
  }
  assert(!face.raster(gid, 0.5f, -9, &invalid));
  assert(!face.raster(gid, 0.5f, 8, &invalid));
  assert(!face.raster(0, 0.5f, 0, &invalid));
  assert(!face.raster(65535, 0.5f, 0, &invalid));
  assert(!face.raster(gid, 0.5f, 0, nullptr));
  expectLegacy(face, gid, saved);
  face.close();
  assert(bank.used == 0);
  FT_Done_Face(ref);
  FT_Done_FreeType(library);
  // Sweep failures during the first raster's size, hinter, bitmap and cache
  // allocations. Even failed rasters must leave the legacy face usable.
  for (unsigned at = 1; at <= 32; ++at) {
    Bank fault;
    fault.bind(o);
    assert(face.openMemory(font.data(), font.size(), o));
    const auto legacy = saveLegacy(face, probeGlyph(face));
    fault.denyAt = fault.claims + at;
    face.raster(probeGlyph(face), 0.375f, -7, &invalid);
    fault.denyAt = 0;
    expectLegacy(face, probeGlyph(face), legacy);
    assert(face.raster(probeGlyph(face), 0.375f, -7, &invalid));
    face.close();
    assert(fault.used == 0);
  }
  o = {};
  o.rasterPolicy = OutlineFontFace::RasterPolicy::LegacyBitmap;
  assert(face.openMemory(font.data(), font.size(), o));
  assert(!face.raster(probeGlyph(face), 0.5f, 0, &invalid));
  face.close();
  for (unsigned steps : {0, 1, 7, 9, 32, 255}) {
    o.phaseSteps = steps;
    assert(!face.openMemory(font.data(), font.size(), o));
  }
  o.phaseSteps = 8;
  o.rasterPolicy = static_cast<OutlineFontFace::RasterPolicy>(255);
  assert(!face.openMemory(font.data(), font.size(), o));
}
int main(int argc, char** argv) {
  assert(argc >= 2);
  for (int a = 1; a < argc; ++a) {
    auto font = contents(argv[a]);
    testPhases(font);
    testCacheAndFailure(font);
    printf(
        "PASS raster fixture%d: phase/size/hinting reference, legacy isolation, cache eviction, allocation recovery\n",
        a);
  }
  printf("PASS %u outline raster checks\n", checks);
}
