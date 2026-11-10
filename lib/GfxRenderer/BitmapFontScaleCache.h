#pragma once
#if CROSSINK_SCALABLE_FONTS
#include <CpFontFormat.h>
#include <GlyphResample.h>
#include <GlyphScale.h>
#include <Memory.h>
#include <SdCardFont.h>

#include <array>
#include <cmath>

// Borrow native bitmap families through stable descriptors. Sources remain owned
// by their existing loader; GfxRenderer invalidates borrowers before removal.
// One bounded PSRAM pool stores 32 sizes and a 64 KiB coverage cache. Scaling
// always starts from source pixels, even for reduced caps/SUP/SUB, so there is
// only one coverage quantization.
class BitmapFontScaleCache {
  struct Face;
  struct Slot {
    BitmapFontScaleCache* owner = nullptr;
    int id = 0, source = 0;
    uint16_t points = 0, sourcePoints = 0;
    uint32_t used = 0, generation = 0;
    Face* faces = nullptr;
  };
  struct Face {
    Slot* slot = nullptr;
    uint8_t style = 0, next = 0;
    EpdFontData data{};
    EpdFont font{&data};
    EpdGlyph metrics[8]{};
    uint32_t keys[8]{};
  };
  struct Pixel {
    uint32_t generation = 0, cp = 0, offset = 0;
    uint8_t style = 0, scale64 = 0;
    int8_t phase = 0;
    EpdGlyph glyph{};
  };
  struct Raw {
    int source = 0;
    uint32_t cp = 0;
    uint8_t style = 0;
    EpdGlyph glyph{};
  };
  struct Pool {
    Slot slots[32];
    Face faces[32][4];
    Pixel pixels[128];
    Raw raw[128];
  };
  GfxRenderer& renderer_;
  HeapByteBuffer backing_, pixels_;
  Pool* pool_ = nullptr;
  uint32_t sequence_ = 0, generation_ = 0;
  size_t pixelUsed_ = 0;
  int recent_[2]{};
  struct Failure {
    int source = 0;
    uint16_t points = 0;
  };
  std::array<Failure, 32> failures_{};
  bool blocked_ = false;
  static const EpdFontFamily* family(const Face& f) {
    const auto& map = f.slot->owner->renderer_.getFontMap();
    const auto found = map.find(f.slot->source);
    return found == map.end() ? nullptr : &found->second;
  }
  static float scale(const Face& f, uint8_t extra = 64) {
    return float(f.slot->points) * extra / (uint32_t(f.slot->sourcePoints) * 64);
  }
  static int32_t metric(const Face& f, int32_t value) {
    const int64_t product = int64_t(value) * f.slot->points;
    return product >= 0 ? (product + f.slot->sourcePoints / 2) / f.slot->sourcePoints
                        : -((-product + f.slot->sourcePoints / 2) / f.slot->sourcePoints);
  }
  static int rawHd(Face& f, uint32_t cp, const EpdFontData* sourceData, EpdGlyph& out, const uint8_t*& pixels,
                   bool readPixels) {
    auto& cache = *f.slot->owner;
    const auto& fonts = cache.renderer_.getSdCardFonts();
    const auto found = fonts.find(f.slot->source);
    if (found == fonts.end() || found->second->rasterDensity() != 2) return 0;
    auto& sd = *found->second;
    const auto style = sd.resolveStyle(f.style);
    if (sd.getEpdFont(style)->data != sourceData) return 0;  // shared fallback owns different source pixels
    auto& entry = cache.pool_->raw[(cp * 31u + uint32_t(f.slot->source) * 17u + style) % 128];
    if (!readPixels && entry.source == f.slot->source && entry.cp == cp && entry.style == style) {
      out = entry.glyph;
      return 1;
    }
    if (!sd.readHdGlyph(cp, style, out, pixels, readPixels)) {
      cache.renderer_.noteFontFailure();
      return -1;
    }
    entry = {f.slot->source, cp, style, out};
    return 1;
  }
  static bool measured(Face& f, uint32_t cp, int8_t phase, uint8_t extra, EpdGlyph& out) {
    const auto* source = family(f);
    if (!source) return false;
    const auto gd = source->findGlyphData(cp, static_cast<EpdFontFamily::Style>(f.style));
    if (!gd.glyph) return false;
    EpdGlyph raw{};
    const uint8_t* ignored = nullptr;
    const int hd = rawHd(f, cp, gd.fontData, raw, ignored, false);
    if (hd < 0) return false;
    const auto bounds =
        hd ? glyphScale::bounds(raw.left, raw.top, cpfont::width(raw), cpfont::height(raw), scale(f, extra) / 2, phase)
           : glyphScale::bounds(gd.glyph->left, gd.glyph->top, gd.glyph->width, gd.glyph->height, scale(f, extra),
                                phase);
    if (bounds.width > 255 || bounds.height > 255 || bounds.left < INT16_MIN || bounds.left > INT16_MAX ||
        bounds.top < -INT16_MAX || bounds.top > INT16_MAX)
      return false;
    out = *gd.glyph;
    out.left = bounds.left;
    out.top = -bounds.top;
    out.width = bounds.width;
    out.height = bounds.height;
    const auto advance =
        std::clamp<int32_t>((int64_t(metric(f, fp5::advance(*gd.glyph))) * extra + 32) / 64, 0, 131071);
    out.advanceX = advance / 2;
    out.advanceRemainder = advance & 1;
    out.dataOffset = cp;
    out.dataLength = (out.width * out.height + 3) / 4;
    return true;
  }
  static const EpdGlyph* glyph(void* context, uint32_t cp) {
    auto& f = *static_cast<Face*>(context);
    for (unsigned i = 0; i < 8; ++i)
      if (f.keys[i] == cp + 1) return &f.metrics[i];
    const unsigned at = f.next++ % 8;
    if (!measured(f, cp, 0, 64, f.metrics[at])) return nullptr;
    f.keys[at] = cp + 1;
    return &f.metrics[at];
  }
  static bool raster(void* context, uint32_t cp, int8_t phase, EpdGlyph& out, const uint8_t*& bitmap) {
    return rasterScaled(context, cp, 64, phase, out, bitmap);
  }
  static bool rasterScaled(void* context, uint32_t cp, uint8_t extra, int8_t phase, EpdGlyph& out,
                           const uint8_t*& bitmap) {
    auto& f = *static_cast<Face*>(context);
    auto& cache = *f.slot->owner;
    bitmap = nullptr;
    if (!extra || extra > 64 || phase < -8 || phase > 7) return false;
    auto& entry =
        cache.pool_->pixels[(cp * 31u + f.slot->generation * 17u + f.style * 7u + uint8_t(phase) + extra) % 128];
    if (entry.generation == f.slot->generation && entry.cp == cp && entry.style == f.style && entry.phase == phase &&
        entry.scale64 == extra) {
      out = entry.glyph;
      bitmap = out.dataLength ? cache.pixels_.get() + entry.offset : nullptr;
      return true;
    }
    if (!measured(f, cp, phase, extra, out)) return false;
    if (!out.width || !out.height) return true;
    const auto* source = family(f);
    if (!source) return false;
    const auto gd = source->findGlyphData(cp, static_cast<EpdFontFamily::Style>(f.style));
    if (!gd.glyph) return false;
    EpdGlyph original = *gd.glyph;
    const uint8_t* bytes = nullptr;
    const int hd = rawHd(f, cp, gd.fontData, original, bytes, true);
    if (hd < 0) return false;
    if (!hd) bytes = cache.renderer_.getGlyphBitmap(gd.fontData, gd.glyph);
    if (!bytes) return false;
    if (cache.pixelUsed_ + out.dataLength > 65536) cache.clearPixels();
    auto* destination = cache.pixels_.get() + cache.pixelUsed_;
    std::memset(destination, 0, out.dataLength);
    const float ratio = scale(f, extra) / (hd ? 2 : 1);
    constexpr int32_t one = 1 << 16;
    const int64_t step = static_cast<int64_t>(one / ratio + 0.5f);
    const int64_t x = (int64_t(out.left) * 16 - phase) * step - int64_t(original.left) * one * 16;
    const int64_t y = -int64_t(out.top) * step + int64_t(original.top) * one;
    emitScaledGlyphPixels(bytes, gd.fontData->is2Bit, hd ? cpfont::width(original) : original.width,
                          hd ? cpfont::height(original) : original.height, out.width, out.height, ratio, x, y, 0x0e,
                          [&](int x, int y, uint8_t raw) {
                            const unsigned i = y * out.width + x;
                            destination[i / 4] |= raw << (6 - 2 * (i % 4));
                          });
    entry = {f.slot->generation, cp, static_cast<uint32_t>(cache.pixelUsed_), f.style, extra, phase, out};
    cache.pixelUsed_ += out.dataLength;
    bitmap = destination;
    return true;
  }
  static const uint8_t* bitmap(void* context, const EpdGlyph* g) {
    EpdGlyph out{};
    const uint8_t* pixels = nullptr;
    return raster(context, g->dataOffset, 0, out, pixels) ? pixels : nullptr;
  }
  static bool covers(void* context, uint32_t cp) {
    auto& f = *static_cast<Face*>(context);
    const auto* source = family(f);
    return source && source->hasCodepoint(cp, static_cast<EpdFontFamily::Style>(f.style));
  }
  static int16_t kern(void* context, uint32_t a, uint32_t b) {
    auto& f = *static_cast<Face*>(context);
    const auto* source = family(f);
    return source
               ? std::clamp<int32_t>(metric(f, source->getKerningFP5(a, b, static_cast<EpdFontFamily::Style>(f.style))),
                                     INT16_MIN, INT16_MAX)
               : 0;
  }
  static uint32_t caps(void* context, uint32_t cp, bool all) {
    auto& f = *static_cast<Face*>(context);
    const auto* source = family(f);
    return source ? source->smallCapsCodepoint(cp, static_cast<EpdFontFamily::Style>(f.style), all) : 0;
  }
  static uint32_t ligature(void* context, uint32_t a, uint32_t b) {
    auto& f = *static_cast<Face*>(context);
    const auto* source = family(f);
    return source ? EpdFont(source->getData(static_cast<EpdFontFamily::Style>(f.style))).getLigature(a, b) : 0;
  }
  void clearPixels() {
    pixelUsed_ = 0;
    if (pool_)
      for (auto& p : pool_->pixels) p = {};
  }
  void touch(Slot& s) {
    s.used = ++sequence_;
    if (recent_[0] != s.id) {
      recent_[1] = recent_[0];
      recent_[0] = s.id;
    }
  }
  bool initialize() {
    if (pool_) return true;
    auto storage = makePsramByteBufferNoThrow(sizeof(Pool));
    auto pixels = makePsramByteBufferNoThrow(65536);
    if (!storage || !pixels) return false;
    pool_ = new (storage.get()) Pool{};
    backing_ = std::move(storage);
    pixels_ = std::move(pixels);
    return true;
  }

 public:
  explicit BitmapFontScaleCache(GfxRenderer& renderer) : renderer_(renderer) {}
  ~BitmapFontScaleCache() {
    if (pool_) pool_->~Pool();
  }
  void retry() {
    failures_.fill({});
    blocked_ = false;
  }
  int sourceId(int id) const {
    if (pool_)
      for (const auto& s : pool_->slots)
        if (s.id == id) return s.source;
    return id;
  }
  void remove(int id) {
    if (!pool_) return;
    for (auto& s : pool_->slots)
      if (s.id && (s.source == id || s.id == id)) {
        const int child = s.id;
        s.id = 0;
        for (auto& recent : recent_)
          if (recent == child) recent = 0;
        if (child != id) renderer_.removeFont(child);
      }
    for (auto& raw : pool_->raw)
      if (raw.source == id) raw = {};
    clearPixels();
  }
  void clear() {
    if (pool_)
      for (auto& s : pool_->slots)
        if (s.id) {
          const int id = s.id;
          s.id = 0;
          renderer_.removeFont(id);
        }
    clearPixels();
  }
  int resolve(int base, uint16_t points) {
    if (!points || points > 8192) return 0;
    base = sourceId(base);
    const auto& map = renderer_.getFontMap();
    const auto found = map.find(base);
    if (found == map.end() || !found->second.pointSize()) return 0;
    const uint16_t basePoints = uint16_t(found->second.pointSize()) * 64;
    if (points == basePoints) return base;
    if (pool_)
      for (auto& s : pool_->slots)
        if (s.id && s.source == base && s.points == points) {
          touch(s);
          return s.id;
        }
    if (blocked_) return 0;
    for (const auto& f : failures_)
      if (f.source == base && f.points == points) return 0;
    if (initialize()) {
      Slot* slot = nullptr;
      for (auto& s : pool_->slots)
        if (!s.id) {
          slot = &s;
          break;
        }
      if (!slot)
        for (auto& s : pool_->slots)
          if (s.id != recent_[0] && s.id != recent_[1] && (!slot || s.used < slot->used)) slot = &s;
      if (slot) {
        if (slot->id) renderer_.removeFont(slot->id);
        const unsigned index = slot - pool_->slots;
        const int id = 0x30000000u | (((uint32_t(base) * 16777619u) ^ points) & 0x0fffffffu);
        if (!map.count(id)) {
          *slot = {this, id, base, points, basePoints, 0, ++generation_, pool_->faces[index]};
          for (unsigned style = 0; style < 4; ++style) {
            auto& f = slot->faces[style];
            f = Face{};
            f.font.data = &f.data;
            f.slot = slot;
            f.style = style;
            const auto* data = found->second.getData(static_cast<EpdFontFamily::Style>(style));
            f.data.ascender = std::clamp<int32_t>(metric(f, data->ascender), INT16_MIN, INT16_MAX);
            f.data.descender = std::clamp<int32_t>(metric(f, data->descender), INT16_MIN, INT16_MAX);
            f.data.advanceY = std::clamp<int32_t>(metric(f, data->advanceY), 1, 255);
            f.data.is2Bit = true;
            f.data.fractionalAdvances = true;
            f.data.sizeFamily = found->second.getData();
            f.data.pointSize64 = points;
            f.data.pointSize = (points + 32) / 64;
            f.data.glyphMissCtx = &f;
            f.data.dynamicGlyphHandler = glyph;
            f.data.bitmapHandler = bitmap;
            f.data.rasterHandler = raster;
            f.data.scaledRasterHandler = rasterScaled;
            f.data.coverageHandler = covers;
            f.data.kerning32Handler = kern;
            f.data.smallCapsHandler = caps;
            f.data.ligatureHandler = ligature;
          }
          renderer_.insertFont(id, EpdFontFamily(&slot->faces[0].font, &slot->faces[1].font, &slot->faces[2].font,
                                                 &slot->faces[3].font));
          touch(*slot);
          return id;
        }
      }
    }
    renderer_.noteFontFailure();
    for (auto& f : failures_)
      if (!f.source) {
        f = {base, points};
        return 0;
      }
    blocked_ = true;
    return 0;
  }
};
#endif
