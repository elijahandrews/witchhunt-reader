#include <EpdOutlineFontCallbacks.h>
#include <GfxRenderer.h>

#include <iostream>

#include "OutlineRenderProbe.h"

namespace {
struct RasterState {
  int calls = 0, accepted = 0, rejected = 0;
  void reset() { calls = accepted = rejected = 0; }
  bool matches(int glyphs, int passes = 1) const {
    return calls == glyphs * passes && accepted == (glyphs - 1) * passes && rejected == passes;
  }
};
constexpr uint8_t legacyBase[] = {0x66, 0x66};  // Four rows of coverage 1, 2.
constexpr uint8_t legacyMark[] = {0xc0};
constexpr uint8_t finalBase[] = {0x7a, 0xd7, 0xad, 0x7a, 0xd7, 0xad};
constexpr uint8_t finalMark[] = {0x7d};
constexpr int finalColumns[] = {1, 3, 2, 2, 3, 1};
uint16_t glyphId(void*, uint32_t cp, uint8_t) { return cp == 'A' ? 1 : cp == 0x301 ? 2 : 0; }
EpdGlyphRef metrics(void*, uint16_t gid) {
  if (gid == 1) return {nullptr, 72, gid, 2, 4, 0, 4, true};
  if (gid == 2) return {nullptr, 0, gid, 1, 1, 0, 5, true};
  return {};
}
const uint8_t* bitmap(void*, uint16_t gid, size_t* bytes) {
  *bytes = gid == 1 ? sizeof(legacyBase) : sizeof(legacyMark);
  return gid == 1 ? legacyBase : legacyMark;
}
int16_t kerning(void*, uint16_t, uint16_t) { return 0; }
uint16_t ligature(void*, const uint16_t*, size_t) { return 0; }
bool raster(void* context, uint16_t gid, float, int8_t phase, EpdRasterGlyph* out) {
  auto& state = *static_cast<RasterState*>(context);
  ++state.calls;
  // Model a cached phase-zero raster and an unavailable new phase. Native
  // bounds deliberately differ from the legacy fallback's source rectangle.
  if (phase != 0) {
    ++state.rejected;
    return false;
  }
  ++state.accepted;
  if (gid == 1) {
    out->metrics = {nullptr, 0, gid, 6, 4, 0, 4, true};
    out->bitmap = finalBase;
    out->bytes = sizeof(finalBase);
  } else {
    out->metrics = {nullptr, 0, gid, 2, 2, 0, 6, true};
    out->bitmap = finalMark;
    out->bytes = sizeof(finalMark);
  }
  return true;
}
const EpdOutlineFontCallbacks callbacks{glyphId, metrics, bitmap, kerning, ligature, raster};
}  // namespace

int runRasterFallbackChecks(GfxRenderer& g, HalDisplay& d) {
  using namespace outlineProbe;
  int checks = 0, failures = 0;
  const auto originalOrientation = g.getOrientation();
  const auto originalDarkness = g.getTextDarkness();
  RasterState state;
  EpdFontData data{};
  data.outline = &callbacks;
  data.outlineCtx = &state;
  data.is2Bit = true;
  data.ascender = 4;
  data.advanceY = 6;
  EpdFont font(&data);
  g.insertFont(24, EpdFontFamily(&font));
  g.setTextDarkness(0);
  for (const bool combining : {false, true}) {
    const char* text = combining ? "AA\xcc\x81" : "AA";
    const int glyphs = combining ? 3 : 2;
    for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted,
                                   GfxRenderer::LandscapeClockwise, GfxRenderer::LandscapeCounterClockwise}) {
      g.setOrientation(orientation);
      g.invalidateScaledGlyphCache();
      // Independent geometry: advance 4.5 puts the second base at x=5 with
      // phase -8. Its declined raster must remain the 2px bitmap fallback.
      state.reset();
      bool ok = g.getTextAdvanceXSpaced(24, text, EpdFontFamily::REGULAR, 1, 0) == 9 && state.calls == 0;
      ok &= g.getTextWidthSpaced(24, text, EpdFontFamily::REGULAR, 1, 0) == 9 && state.matches(glyphs);
      state.reset();
      const auto range = g.getTextRangeSpaced(24, text, EpdFontFamily::REGULAR, 1, 0, 1, 1);
      ok &= range.x == 5 && range.left == 5 && range.right == 9 && state.matches(glyphs);
      state.reset();
      int above = 0, below = 0;
      ok &= g.getTextInkMetrics(24, text, EpdFontFamily::REGULAR, &above, &below);
      ok &= above == (combining ? 6 : 4) && below == 0 && state.matches(glyphs);
      Frame expected;
      for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 6; ++x) put(expected, 30 + x, 30 + y, finalColumns[x], 0, orientation);
        for (int x = 0; x < 2; ++x) put(expected, 35 + x, 30 + y, x + 1, 0, orientation);
      }
      if (combining) {
        // The mark centers on the fallback base's 2px box and shares its
        // snapped origin. Its own phase-zero final raster remains available.
        for (int y = 0; y < 2; ++y)
          for (int x = 0; x < 2; ++x) put(expected, 35 + x, 28 + y, x == y ? 1 : 3, 0, orientation);
      }
      const auto draw = [&] { g.drawTextSpaced(24, 30, 30, text, true, EpdFontFamily::REGULAR, 1, 0); };
      state.reset();
      const auto cold = capture(g, d, draw);
      ok &= cold == expected && state.matches(glyphs);
      state.reset();
      ok &= capture(g, d, draw) == expected && state.matches(glyphs);
      state.reset();
      ok &= capture(g, d, draw, true) == expected && state.matches(glyphs, 5);
      ++checks;
      if (!ok) {
        ++failures;
        std::cerr << "Raster fallback changed selected bounds/path: combining=" << combining
                  << " orientation=" << int(orientation) << " calls=" << state.calls << '\n';
      }
    }
  }
  g.removeFont(24);
  g.setOrientation(originalOrientation);
  g.setTextDarkness(originalDarkness);
  g.setRenderMode(GfxRenderer::BW);
  std::cout << "Resolved raster/fallback/bounds/combining checks=" << checks << " failures=" << failures << '\n';
  return failures ? 1 : 0;
}
