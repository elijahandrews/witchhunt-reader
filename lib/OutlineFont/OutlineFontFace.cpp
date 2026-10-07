#include "OutlineFontFace.h"

#ifndef FT_CONFIG_OPTIONS_H
#define FT_CONFIG_OPTIONS_H <witch_ftoption.h>
#endif
#include <ft2build.h>

#include "Gpos.h"
#include "Gsub.h"
#include "OpenTypeCaps.h"
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_TRUETYPE_TABLES_H
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif
namespace {
void* platformAllocate(size_t n) {
#if defined(ESP_PLATFORM)
  return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  return std::malloc(n);
#endif
}
void platformFree(void* p) {
#if defined(ESP_PLATFORM)
  heap_caps_free(p);
#else
  std::free(p);
#endif
}
struct alignas(std::max_align_t) Allocation {
  size_t size;
  bool internal;
};
int32_t toRasterFp4(FT_Pos x) { return x >= 0 ? int32_t((x + 2) / 4) : -int32_t((-x + 2) / 4); }
}  // namespace
struct OutlineFontFace::Impl {
  Options options{};
  FT_MemoryRec_ memory{};
  FT_Library library = nullptr;
  FT_Face face = nullptr;
  FT_StreamRec stream{};
  ReadFn read = nullptr;
  void* readCtx = nullptr;
  size_t used = 0, peak = 0, internalUsed = 0;
  int error = 0;
  bool allocationFailed = false;
  bool opsz = false, weightAxis = false, italicAxis = false;
  float optical = 0;
  uint8_t *gsub = nullptr, *gpos = nullptr, *packed = nullptr;
  size_t gsubSize = 0, gposSize = 0, packedCapacity = 0, packedSize = 0;
  uint16_t loaded = 0;
  EpdGlyphRef last{};
  LineMetrics line{};
  struct Cached {
    uint16_t gid = 0;
    EpdGlyphRef metrics{};
  };
  Cached cache[96]{};
  unsigned next = 0;
  void* allocate(size_t n) {
    if (!n) return nullptr;
    if (n > options.memoryLimit - used || n > SIZE_MAX - sizeof(Allocation)) {
      allocationFailed = true;
      if (options.memoryFailure) options.memoryFailure(options.allocationContext);
      return nullptr;
    }
    if (options.claimMemory && !options.claimMemory(options.allocationContext, n)) {
      allocationFailed = true;
      if (options.memoryFailure) options.memoryFailure(options.allocationContext);
      return nullptr;
    }
    bool internal = false;
#if defined(ESP_PLATFORM)
    auto* a = (Allocation*)heap_caps_malloc(n + sizeof(Allocation), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!a && n <= 2048 - internalUsed) {
      a = (Allocation*)heap_caps_malloc(n + sizeof(Allocation), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      internal = a != nullptr;
    }
#else
    auto* a = (Allocation*)platformAllocate(n + sizeof(Allocation));
#endif
    if (!a) {
      if (options.releaseMemory) options.releaseMemory(options.allocationContext, n);
      allocationFailed = true;
      if (options.memoryFailure) options.memoryFailure(options.allocationContext);
      return nullptr;
    }
    a->size = n;
    a->internal = internal;
    if (internal) internalUsed += n;
    used += n;
    peak = std::max(peak, used);
    return a + 1;
  }
  void release(void* p) {
    if (!p) return;
    auto* a = (Allocation*)p - 1;
    used -= a->size;
    if (a->internal) internalUsed -= a->size;
    if (options.releaseMemory) options.releaseMemory(options.allocationContext, a->size);
    platformFree(a);
  }
  static void* alloc(FT_Memory m, long n) { return n > 0 ? ((Impl*)m->user)->allocate(size_t(n)) : nullptr; }
  static void free(FT_Memory m, void* p) { ((Impl*)m->user)->release(p); }
  static void* realloc(FT_Memory m, long old, long n, void* p) {
    auto* self = (Impl*)m->user;
    if (n <= 0) {
      self->release(p);
      return nullptr;
    }
    void* r = self->allocate(size_t(n));
    if (!r) return nullptr;
    if (p && old > 0) std::memcpy(r, p, size_t(std::min(old, n)));
    self->release(p);
    return r;
  }
  static unsigned long streamRead(FT_Stream s, unsigned long off, unsigned char* b, unsigned long n) {
    auto* self = (Impl*)s->descriptor.pointer;
    if (off > s->size || n > s->size - off) return n ? 0 : 1;
    if (!n) return 0;
    return self->read(self->readCtx, uint32_t(off), b, size_t(n));
  }
  bool start(const Options& o) {
    options = o;
#if defined(ESP_PLATFORM)
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
      error = FT_Err_Out_Of_Memory;
      return false;
    }
#endif
    error = 0;
    allocationFailed = false;
    peak = 0;
    if (!std::isfinite(o.logicalPx) || o.logicalPx < 1 || o.logicalPx > 120 || !std::isfinite(o.opticalPoints) ||
        o.opticalPoints < 0 || o.opticalPoints > 512 || bool(o.claimMemory) != bool(o.releaseMemory) ||
        o.memoryLimit < 16384 || o.memoryLimit > 8 * 1024 * 1024) {
      error = FT_Err_Invalid_Argument;
      return false;
    }
    memory.user = this;
    memory.alloc = alloc;
    memory.free = free;
    memory.realloc = realloc;
    error = FT_New_Library(&memory, &library);
    if (error) return false;
    FT_Add_Default_Modules(library);
    // This void API ignores individual module allocation failures. In particular,
    // native CFF without PSHinter can scale its outline twice while keeping advances.
    for (const char* module : {"sfnt", "truetype", "cff", "psaux", "psnames", "pshinter", "smooth"
#if defined(WITCH_FONT_AUTOHINT) && WITCH_FONT_AUTOHINT
                               ,
                               "autofitter"
#endif
         }) {
      if (!FT_Get_Module(library, module)) {
        error = FT_Err_Out_Of_Memory;
        return false;
      }
    }
    if (allocationFailed) {
      error = FT_Err_Out_Of_Memory;
      return false;
    }
    return true;
  }
  bool table(FT_ULong tag, uint8_t*& dst, size_t& length) {
    FT_ULong n = 0;
    FT_Error result = FT_Load_Sfnt_Table(face, tag, 0, nullptr, &n);
    if (result == FT_Err_Table_Missing) return true;
    if (result) {
      error = result;
      return false;
    }
    if (!n) return true;
    if (n > options.tableLimit || gsubSize > options.tableLimit || n > options.tableLimit - gsubSize) {
      error = FT_Err_Array_Too_Large;
      return false;
    }
    dst = (uint8_t*)allocate(n);
    if (!dst) {
      error = FT_Err_Out_Of_Memory;
      return false;
    }
    FT_ULong got = n;
    result = FT_Load_Sfnt_Table(face, tag, 0, dst, &got);
    if (result || got != n) {
      release(dst);
      dst = nullptr;
      error = result ? result : FT_Err_Invalid_Table;
      return false;
    }
    length = n;
    return true;
  }
  bool finish() {
    if (face->num_glyphs < 1 || face->num_glyphs > 65535 || !FT_IS_SCALABLE(face) ||
        FT_Select_Charmap(face, FT_ENCODING_UNICODE)) {
      error = FT_Err_Invalid_Face_Handle;
      return false;
    }
    if (FT_HAS_MULTIPLE_MASTERS(face)) {
      FT_MM_Var* v = nullptr;
      error = FT_Get_MM_Var(face, &v);
      if (error) return false;
      auto* coords = (FT_Fixed*)allocate(size_t(v->num_axis) * sizeof(FT_Fixed));
      if (!coords) {
        FT_Done_MM_Var(library, v);
        error = FT_Err_Out_Of_Memory;
        return false;
      }
      for (unsigned i = 0; i < v->num_axis; ++i) {
        FT_Fixed value = v->axis[i].def;
        switch (v->axis[i].tag) {
          case FT_MAKE_TAG('o', 'p', 's', 'z'):
            value = FT_Fixed(std::lround(
                (options.opticalPoints > 0 ? options.opticalPoints : options.logicalPx * 72.0 / 150.0) * 65536.0));
            opsz = true;
            break;
          case FT_MAKE_TAG('w', 'g', 'h', 't'):
            weightAxis = true;
            value = FT_Fixed(options.weight) * 65536;
            break;
          case FT_MAKE_TAG('i', 't', 'a', 'l'):
            italicAxis = true;
            value = options.italic ? 65536 : 0;
            break;
          default:
            break;
        }
        coords[i] = std::clamp(value, v->axis[i].minimum, v->axis[i].maximum);
        if (v->axis[i].tag == FT_MAKE_TAG('o', 'p', 's', 'z')) optical = float(coords[i]) / 65536;
      }
      error = FT_Set_Var_Design_Coordinates(face, v->num_axis, coords);
      release(coords);
      FT_Done_MM_Var(library, v);
      if (error) return false;
    }
    error = FT_Set_Char_Size(face, 0, FT_F26Dot6(std::lround(options.logicalPx * 2 * 64)), 72, 72);
    if (error) return false;
    line.ascender = int((face->size->metrics.ascender + 63) / 64);
    line.descender = int((-face->size->metrics.descender + 63) / 64);
    const int64_t lineHeight = (int64_t(face->size->metrics.height) + 63) / 64;
    line.advanceY = uint16_t(std::clamp<int64_t>(lineHeight, 0, 255));
    if (line.ascender < 0 || line.descender < 0 || face->size->metrics.height < 0 || lineHeight < 1 ||
        lineHeight > 255) {
      error = FT_Err_Invalid_Pixel_Size;
      return false;
    }
    return table(FT_MAKE_TAG('G', 'S', 'U', 'B'), gsub, gsubSize) &&
           table(FT_MAKE_TAG('G', 'P', 'O', 'S'), gpos, gposSize);
  }
  FT_Int32 flags() const {
    FT_Int32 f = FT_LOAD_NO_BITMAP | FT_LOAD_RENDER;
    if (options.hinting == Hinting::None) return f | FT_LOAD_NO_HINTING | FT_LOAD_NO_AUTOHINT;
    if (options.hinting == Hinting::Native) return f | FT_LOAD_NO_AUTOHINT;
    return f | FT_LOAD_TARGET_LIGHT;
  }
  bool load(uint16_t gid) {
    if (!face || !gid || gid >= face->num_glyphs) return false;
    if (loaded == gid) return bool(last);
    error = FT_Load_Glyph(face, gid, flags());
    loaded = 0;
    if (error) return false;
    auto* g = face->glyph;
    auto& b = g->bitmap;
    // FreeType linear advances are 16.16 raster pixels. Keep fractional authored
    // spacing even when native/light hinting snaps the bitmap to pixels.
    const int64_t linear = g->linearHoriAdvance;
    int32_t adv = linear >= 0 ? int32_t((linear + 2048) / 4096) : -int32_t((-linear + 2048) / 4096);
    if (adv < 0 || adv > 65535 || b.width > 65535 || b.rows > 65535 || g->bitmap_left < INT16_MIN ||
        g->bitmap_left > INT16_MAX || g->bitmap_top < INT16_MIN || g->bitmap_top > INT16_MAX ||
        uint64_t(b.width) * b.rows > 262140 || b.pixel_mode != FT_PIXEL_MODE_GRAY) {
      error = FT_Err_Invalid_Glyph_Format;
      return false;
    }
    last = {nullptr,
            uint16_t(adv),
            gid,
            uint16_t(b.width),
            uint16_t(b.rows),
            int16_t(g->bitmap_left),
            int16_t(g->bitmap_top),
            true};
    loaded = gid;
    cache[next] = {gid, last};
    next = (next + 1) % 96;
    return true;
  }
};
OutlineFontFace::OutlineFontFace() : impl_(nullptr) {
  void* block = platformAllocate(sizeof(Impl));
  if (block) impl_ = new (block) Impl;
}
OutlineFontFace::~OutlineFontFace() {
  close();
  if (impl_) {
    impl_->~Impl();
    platformFree(impl_);
  }
}
void OutlineFontFace::close() {
  if (!impl_) return;
  auto& i = *impl_;
  if (i.face) FT_Done_Face(i.face);
  i.face = nullptr;
  if (i.library) FT_Done_Library(i.library);
  i.library = nullptr;
  i.release(i.gsub);
  i.release(i.gpos);
  i.release(i.packed);
  i.gsub = i.gpos = i.packed = nullptr;
  i.gsubSize = i.gposSize = i.packedSize = i.packedCapacity = 0;
  i.loaded = 0;
  i.last = {};
  i.opsz = i.weightAxis = i.italicAxis = false;
  i.optical = 0;
  i.line = {};
  i.read = nullptr;
  i.readCtx = nullptr;
  i.next = 0;
  for (auto& c : i.cache) c = {};
}
bool OutlineFontFace::openMemory(const uint8_t* data, size_t bytes, const Options& o) {
  close();
  if (!impl_) {
    if (o.memoryFailure) o.memoryFailure(o.allocationContext);
    return false;
  }
  if (!data || !bytes || bytes > INT32_MAX) return false;
  auto& i = *impl_;
  if (!i.start(o)) {
    close();
    return false;
  }
  i.error = FT_New_Memory_Face(i.library, data, FT_Long(bytes), 0, &i.face);
  if (!i.error && i.finish() && !i.allocationFailed) return true;
  close();
  return false;
}
bool OutlineFontFace::openStream(ReadFn read, void* ctx, uint32_t bytes, const Options& o) {
  close();
  if (!impl_) {
    if (o.memoryFailure) o.memoryFailure(o.allocationContext);
    return false;
  }
  if (!read || !bytes || bytes > INT32_MAX) return false;
  auto& i = *impl_;
  if (!i.start(o)) {
    close();
    return false;
  }
  i.read = read;
  i.readCtx = ctx;
  i.stream = {};
  i.stream.size = bytes;
  i.stream.descriptor.pointer = &i;
  i.stream.read = Impl::streamRead;
  FT_Open_Args a{};
  a.flags = FT_OPEN_STREAM;
  a.stream = &i.stream;
  i.error = FT_Open_Face(i.library, &a, 0, &i.face);
  if (!i.error && i.finish() && !i.allocationFailed) return true;
  close();
  return false;
}
bool OutlineFontFace::ready() const { return impl_ && impl_->face; }
int OutlineFontFace::lastError() const { return impl_ ? impl_->error : FT_Err_Out_Of_Memory; }
size_t OutlineFontFace::memoryUsed() const { return impl_ ? impl_->used : 0; }
size_t OutlineFontFace::memoryPeak() const { return impl_ ? impl_->peak : 0; }
OutlineFontFace::LineMetrics OutlineFontFace::lineMetrics() const { return ready() ? impl_->line : LineMetrics{}; }
bool OutlineFontFace::hasOpticalSize() const { return ready() && impl_->opsz; }
bool OutlineFontFace::hasWeightAxis() const { return ready() && impl_->weightAxis; }
bool OutlineFontFace::hasItalicAxis() const { return ready() && impl_->italicAxis; }
float OutlineFontFace::opticalSize() const { return ready() ? impl_->optical : 0; }
uint16_t OutlineFontFace::glyphId(uint32_t cp, uint8_t caps) {
  if (!ready()) return 0;
  auto& i = *impl_;
  unsigned gid = FT_Get_Char_Index(i.face, cp);
  if (!gid || gid > 65535 || caps > 2) return 0;
  if (!caps) return uint16_t(gid);
  uint32_t tag = caps == 1 ? FT_MAKE_TAG('s', 'm', 'c', 'p') : FT_MAKE_TAG('c', '2', 's', 'c');
  uint16_t alternate = outlineCapsSubstitution(i.gsub, i.gsubSize, uint16_t(gid), tag);
  return alternate < i.face->num_glyphs ? alternate : 0;
}
EpdGlyphRef OutlineFontFace::metrics(uint16_t gid) {
  if (!ready() || !gid) return {};
  auto& i = *impl_;
  for (auto& c : i.cache)
    if (c.gid == gid) return c.metrics;
  return i.load(gid) ? i.last : EpdGlyphRef{};
}
const uint8_t* OutlineFontFace::bitmap(uint16_t gid, size_t* bytes) {
  if (bytes) *bytes = 0;
  if (!ready()) return nullptr;
  auto& i = *impl_;
  if (!i.load(gid)) return nullptr;
  auto& b = i.face->glyph->bitmap;
  size_t pixels = size_t(b.width) * b.rows, n = (pixels * 2 + 7) / 8;
  if (n > i.packedCapacity) {
    auto* p = (uint8_t*)i.allocate(n);
    if (!p) {
      i.error = FT_Err_Out_Of_Memory;
      return nullptr;
    }
    i.release(i.packed);
    i.packed = p;
    i.packedCapacity = n;
  }
  if (n) std::memset(i.packed, 0, n);
  for (unsigned y = 0; y < b.rows; ++y) {
    const uint8_t* row =
        b.pitch >= 0 ? b.buffer + size_t(y) * b.pitch : b.buffer + size_t(b.rows - 1 - y) * size_t(-b.pitch);
    for (unsigned x = 0; x < b.width; ++x) {
      size_t at = size_t(y) * b.width + x;
      unsigned c = (unsigned(row[x]) * 3 + 127) / 255;
      i.packed[at / 4] |= uint8_t(c << (6 - (at % 4) * 2));
    }
  }
  i.packedSize = n;
  if (bytes) *bytes = n;
  return n ? i.packed : nullptr;
}
int16_t OutlineFontFace::kerning(uint16_t l, uint16_t r) {
  if (!ready() || !l || !r) return 0;
  auto& i = *impl_;
  FT_Pos value = 0;
  if (i.gpos) {
    int32_t units = freeink::font::gpos::PairKernAdjustment(i.gpos, i.gposSize, l, r);
    value = FT_MulFix(units, i.face->size->metrics.x_scale);
  } else {
    FT_Vector delta{};
    if (!FT_Get_Kerning(i.face, l, r, FT_KERNING_UNFITTED, &delta)) value = delta.x;
  }
  return int16_t(std::clamp(toRasterFp4(value), int32_t(INT16_MIN), int32_t(INT16_MAX)));
}
uint16_t OutlineFontFace::ligature(const uint16_t* gids, size_t n) {
  if (!ready() || !gids || n < 2 || n > 3) return 0;
  uint32_t seq[3] = {gids[0], gids[1], n == 3 ? gids[2] : 0u};
  auto& i = *impl_;
  uint32_t gid = freeink::font::gsub::LigatureGlyphId(i.gsub, i.gsubSize, seq, unsigned(n));
  return gid > 0 && gid < unsigned(i.face->num_glyphs) ? uint16_t(gid) : 0;
}
const EpdOutlineFontCallbacks& OutlineFontFace::callbacks() {
  static const EpdOutlineFontCallbacks c = {
      [](void* p, uint32_t cp, uint8_t m) { return ((OutlineFontFace*)p)->glyphId(cp, m); },
      [](void* p, uint16_t g) { return ((OutlineFontFace*)p)->metrics(g); },
      [](void* p, uint16_t g, size_t* n) { return ((OutlineFontFace*)p)->bitmap(g, n); },
      [](void* p, uint16_t l, uint16_t r) { return ((OutlineFontFace*)p)->kerning(l, r); },
      [](void* p, const uint16_t* g, size_t n) { return ((OutlineFontFace*)p)->ligature(g, n); }};
  return c;
}
