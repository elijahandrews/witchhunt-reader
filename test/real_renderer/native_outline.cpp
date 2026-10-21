#include <OutlineFontFace.h>
#include <SimNorFlash.h>
#include <TtfFont.h>
#include <TtfFontTestUtil.h>

#include <iostream>
#include <memory>

#include "OutlineRenderProbe.h"

namespace {
// Unlike a stable RAM vector, released MMU mappings cease to exist. ASan catches
// an outline face retaining a former style pointer across mapStyle switches.
// Live index/log views stay coherent with NOR writes, as they do on the device.
class TransientNorFlash final : public NorFlash {
 public:
  SimNorFlash backing{4 * 1024 * 1024, 48};
  struct Mapping {
    uint32_t offset, length;
    const uint8_t* backing;
    std::unique_ptr<uint8_t[]> bytes;
  };
  std::vector<Mapping> maps;
  uint32_t switches = 0;
  uint32_t size() const override { return backing.size(); }
  bool read(uint32_t off, void* dst, uint32_t len) const override { return backing.read(off, dst, len); }
  void refresh(uint32_t off, uint32_t len) {
    for (auto& m : maps) {
      const uint32_t a = std::max(off, m.offset), b = std::min(off + len, m.offset + m.length);
      if (b > a) assert(backing.read(a, m.bytes.get() + a - m.offset, b - a));
    }
  }
  bool program(uint32_t off, const void* src, uint32_t len) override {
    const bool ok = backing.program(off, src, len);
    refresh(off, len);
    return ok;
  }
  bool erase(uint32_t off, uint32_t len) override {
    const bool ok = backing.erase(off, len);
    refresh(off, len);
    return ok;
  }
  const uint8_t* map(uint32_t off, uint32_t len) override {
    const auto* original = backing.map(off, len);
    if (!original) return nullptr;
    Mapping m{off, len, original, std::make_unique<uint8_t[]>(len)};
    std::copy_n(original, len, m.bytes.get());
    const auto* result = m.bytes.get();
    maps.push_back(std::move(m));
    return result;
  }
  void unmap(const uint8_t* ptr) override {
    for (auto it = maps.begin(); it != maps.end(); ++it)
      if (it->bytes.get() == ptr) {
        backing.unmap(it->backing);
        maps.erase(it);
        ++switches;
        return;
      }
    assert(false);
  }
  uint32_t freeMapPages() const override { return backing.freeMapPages(); }
};
bool sameMetrics(const EpdGlyphRef& a, const EpdGlyphRef& b) {
  return a.width == b.width && a.height == b.height && a.left == b.left && a.top == b.top && a.advanceX == b.advanceX;
}
}  // namespace

int runNativeOutlineChecks(GfxRenderer& g, HalDisplay& d, const char* const* paths) {
  using namespace outlineProbe;
  ttf_test::MemoryFile files[4];
  ttf::FileSource sources[4];
  std::unique_ptr<OutlineFontFace> references[4];
  for (int s = 0; s < 4; ++s) {
    files[s].bytes = readFont(paths[s]);
    sources[s] = files[s].source();
    references[s] = std::make_unique<OutlineFontFace>();
    OutlineFontFace::Options o;
    o.logicalPx = 14;
    o.applyVariations = false;
    o.loadLayoutTables = false;
    assert(references[s]->openMemory(files[s].bytes.data(), files[s].bytes.size(), o));
  }
  TransientNorFlash flash;
  const uint8_t identity[glyphstore::kIdentityBytes] = {'o', 'u', 't', 'l', 'i', 'n', 'e', '-', 'h', 'o', 's', 't'};
  int installs = 0, checks = 0, phaseZeroChanged = 0;
  auto open = [&] {
    auto f = std::make_unique<TtfFont>(flash);
    assert(f->load(sources, identity, [&] { ++installs; }));
    return f;
  };
  Frame previous;
  for (int session = 0; session < 2; ++session) {
    auto native = open();
    for (int points : {8, 10, 12}) {
      assert(native->setBodySize(points));
      const EpdFont* fonts[4];
      for (int s = 0; s < 4; ++s) {
        fonts[s] = native->font(s);
        assert(fonts[s]);
      }
      EpdFontFamily family(fonts[0], fonts[1], fonts[2], fonts[3]);
      g.insertFont(82, family);
      for (int iteration = 0; iteration < 8; ++iteration) {
        const int s = iteration % 4;
        const auto style = static_cast<EpdFontFamily::Style>(s);
        for (uint32_t cp : {'H', 'N', 'T', 'j', 'f', 'A', 'V', 'i'}) {
          const auto layout = fonts[s]->getGlyph(cp);
          assert(layout);
          const auto kern = family.getKerning('A', 'V', style);
          const auto* stored = g.getGlyphBitmap(fonts[s]->data, layout);
          assert(stored);
          const std::vector<uint8_t> legacy(stored, stored + glyphDataBytes(layout.width, layout.height, true));
          for (float scale : {.75f, 1.0f, 1.25f})
            for (int phase : {-8, -3, 0, 5, 7}) {
              EpdRasterGlyph actual, expected;
              assert(fonts[s]->rasterGlyph(cp, scale, int8_t(phase), actual));
              const auto gid = references[s]->glyphId(cp);
              assert(
                  references[s]->raster(gid, native->bodyPpem() / (64.0f * 28.0f) * scale, int8_t(phase), &expected));
              assert(sameMetrics(actual.metrics, expected.metrics));
              assert(actual.bytes == expected.bytes);
              if (phase == 0 && scale == 1 &&
                  (actual.metrics.width != layout.width || actual.metrics.height != layout.height ||
                   actual.metrics.left != layout.left || actual.metrics.top != layout.top ||
                   actual.bytes != legacy.size() || !std::equal(legacy.begin(), legacy.end(), actual.bitmap)))
                ++phaseZeroChanged;
              assert(actual.bytes == 0 || std::equal(actual.bitmap, actual.bitmap + actual.bytes, expected.bitmap));
              assert(sameMetrics(layout, fonts[s]->getGlyph(cp)));
              assert(kern == family.getKerning('A', 'V', style));
              ++checks;
            }
        }
        for (auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted, GfxRenderer::LandscapeClockwise,
                                 GfxRenderer::LandscapeCounterClockwise}) {
          g.setOrientation(orientation);
          g.setTextDarkness(0);
          const char* text = "still HNT jfj AVATAR To a\xcc\x81";
          const auto paint = [&] { g.drawTextSpaced(82, -3, 30, text, true, style, 1, 3, false); };
          const auto frame = capture(g, d, paint);
          assert(frame == reference(family, text, style, 1, 3, -3, 30, 0, orientation).frame);
          assert(frame == capture(g, d, paint, true));
          assert(frame == capture(g, d, paint));
          ++checks;
        }
      }
      g.removeFont(82);
    }
    assert(native->setBodySize(10));
    for (int px : {17, 25, 31}) {
      const EpdFont* fonts[4];
      uint32_t served = 0;
      assert(native->nativeFonts(px, fonts, &served));
      assert(served == uint32_t(px) * 64);
      EpdFontFamily family(fonts[0], fonts[1], fonts[2], fonts[3]);
      g.insertFont(82, family);
      g.setOrientation(GfxRenderer::Portrait);
      for (int s = 0; s < 4; ++s) {
        EpdRasterGlyph actual, expected;
        assert(fonts[s]->rasterGlyph('j', 1, 0, actual));
        assert(references[s]->raster(references[s]->glyphId('j'), px / 28.0f, 0, &expected));
        assert(sameMetrics(actual.metrics, expected.metrics));
        assert(actual.bytes == expected.bytes);
        assert(actual.bytes == 0 || std::equal(actual.bitmap, actual.bitmap + actual.bytes, expected.bitmap));
        const auto style = static_cast<EpdFontFamily::Style>(s);
        const auto paint = [&] { g.drawTextSpaced(82, 30, 30, "HNT minimum jfj", true, style, 1, 0, false); };
        const auto frame = capture(g, d, paint);
        assert(frame == reference(family, "HNT minimum jfj", style, 1, 0, 30, 30, 0, GfxRenderer::Portrait).frame);
        int above, below;
        assert(g.getTextInkMetrics(82, "HNT minimum jfj", style, &above, &below));
        const auto extent = reference(family, "HNT minimum jfj", style, 1, 0, 0, 0, 0, GfxRenderer::Portrait);
        assert(above >= -extent.top && below >= extent.bottom);
        if (px == 25 && s == 2) {
          if (session == 0) {
            previous = frame;
            save("native-outline.pgm", frame);
          } else
            assert(frame == previous);
        }
        ++checks;
      }
      g.removeFont(82);
    }
    native.reset();
    assert(flash.maps.empty());
  }
  std::cerr << "native cache sessions busy=" << installs << " mapping releases=" << flash.switches
            << " checks=" << checks << std::endl;
  assert(installs == 1 && flash.switches >= 64 && phaseZeroChanged > 0);
  assert(flash.backing.violations() == 0 && flash.backing.reprograms() == 0);
  std::cout << "native TTF stream/remapping/phase0/style/size/legacy-layout/cold-warm checks=" << checks
            << " passed, phase-zero rasters changed=" << phaseZeroChanged << ", mapping releases=" << flash.switches
            << '\n';
  return 0;
}
