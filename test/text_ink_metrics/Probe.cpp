#include <EpdFontFamily.h>
#include <GlyphProvider.h>
#include <TextRun.h>

#include <cstdio>
#include <cstdlib>
#include <tuple>

struct Provider final : GlyphProvider {
  EpdGlyph letter{6, 8, 147, -1, 7, 12, 0}, descender{6, 10, 151, 0, 7, 15, 0};
  EpdGlyph mark{3, 2, 0, 0, 10, 2, 0}, space{0, 0, 64, 0, 0, 0, 0};
  int glyphCalls = 0, rasterCalls = 0;
  bool finalRaster = true;
  const EpdGlyph* shape(uint32_t cp) const {
    if (cp == ' ') return &space;
    if (cp == 0x301) return &mark;
    return cp == 'g' ? &descender : &letter;
  }
  const EpdGlyph* missGlyph(uint32_t cp) override {
    ++glyphCalls;
    return shape(cp);
  }
  bool rasterGlyph(uint32_t cp, float scale, int8_t phase, EpdRasterGlyph& out) override {
    ++rasterCalls;
    if (!finalRaster) return false;
    const auto* glyph = shape(cp);
    auto b = glyphScale::bounds(glyph->left, glyph->top, glyph->width, glyph->height, scale, phase);
    if (b.height) {
      --b.top;
      b.height += 2;
    }
    out.metrics = {nullptr, 0, 1, uint16_t(b.width), uint16_t(b.height), int16_t(b.left), int16_t(-b.top), true};
    return true;
  }
  void resetCounts() { glyphCalls = rasterCalls = 0; }
};
struct GfxRenderer {
  EpdFontFamily* family = nullptr;
  float scale = 1;
  const EpdFontFamily* findFontFamily(int id) const { return id == 1 ? family : nullptr; }
  float fontBaseScale(int) const { return scale; }
  bool getTextInkMetrics(int, const char*, EpdFontFamily::Style, int*, int*) const;
};
// PRODUCTION_METHOD
int checks = 0;
void check(bool okay, const char* why) {
  ++checks;
  if (!okay) {
    std::fprintf(stderr, "FAIL: %s\n", why);
    std::exit(1);
  }
}
int main() {
  Provider provider;
  EpdFontData data{};
  data.is2Bit = true;
  data.provider = &provider;
  EpdFont font(&data);
  EpdFontFamily family(&font);
  GfxRenderer renderer{&family};
  int above, below;
  // Exact expected geometry, including the native raster's different bounds.
  check(renderer.getTextInkMetrics(1, "Ag", EpdFontFamily::REGULAR, &above, &below), "ink present");
  check(above == 8 && below == 4, "final raster ascent and descender");
  check(renderer.getTextInkMetrics(1, "A\xcc\x81", EpdFontFamily::REGULAR, &above, &below), "combining ink present");
  check(above == 12 && below == 2, "combining offset uses final raster geometry");
  // Independent parity oracle: union the actual emitted glyph rasters, including
  // y offsets calculated by the production walker; old getTextInkMetrics did this twice.
  for (bool raster : {false, true})
    for (float scale : {.5f, .75f, 1.0f, 1.25f})
      for (auto style : {EpdFontFamily::REGULAR, EpdFontFamily::SMALL_CAPS, EpdFontFamily::ALL_SMALL_CAPS})
        for (const char* text : {"Ag gA", "A\xcc\x81g", "A g\xcc\x81", " ", ""}) {
          provider.finalRaster = raster;
          renderer.scale = scale;
          int refAbove = 0, refBelow = 0;
          bool any = false;
          textRun::walk(family, text, style, scale, 0, [&](uint32_t cp, int, int y, float size, bool, int8_t phase) {
            const auto* glyph = provider.shape(cp);
            auto b = glyphScale::bounds(glyph->left, glyph->top, glyph->width, glyph->height, size, phase);
            if (raster && b.height) {
              --b.top;
              b.height += 2;
            }
            if (!b.width || !b.height) return;
            refAbove = std::max(refAbove, -(y + b.top));
            refBelow = std::max(refBelow, y + b.top + b.height);
            any = true;
          });
          const bool actual = renderer.getTextInkMetrics(1, text, style, &above, &below);
          check(actual == any && above == refAbove && below == refBelow, "vertical union parity");
        }
  check(!renderer.getTextInkMetrics(1, nullptr, EpdFontFamily::REGULAR, &above, &below) && !above && !below,
        "null text remains empty");
  check(!renderer.getTextInkMetrics(0, "A", EpdFontFamily::REGULAR, &above, &below) && !above && !below,
        "unknown font remains empty");
  provider.finalRaster = true;
  renderer.scale = 1;
  provider.resetCounts();
  renderer.getTextInkMetrics(1, "AgA", EpdFontFamily::REGULAR, &above, &below);
  std::printf("glyph lookups=%d raster lookups=%d for 3 glyphs\n", provider.glyphCalls, provider.rasterCalls);
  check(provider.glyphCalls == 3 && provider.rasterCalls == 3, "one glyph and raster lookup per measured glyph");
  std::printf("%d vertical ink metrics checks passed; Metrics size=%zu bytes\n", checks, sizeof(textRun::Metrics));
}
