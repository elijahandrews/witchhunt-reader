#include <EpdFontFamily.h>
#include <HalScalableFont.h>
#include <PublisherFontRegistration.h>
#include <TrueTypeFormat.h>
#include <ft2build.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_MODULE_H
using Ft = freeink::font::FtFont;
std::vector<uint8_t> read(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), {}};
}
struct DirectFace {
  FT_Library library = nullptr;
  FT_Face face = nullptr;
  explicit DirectFace(const std::vector<uint8_t>& bytes) {
    assert(FT_Init_FreeType(&library) == 0);
    assert(FT_New_Memory_Face(library, bytes.data(), bytes.size(), 0, &face) == 0);
    FT_Bool noDarkening = 1;
    assert(FT_Property_Set(library, "autofitter", "no-stem-darkening", &noDarkening) == 0);
  }
  ~DirectFace() {
    FT_Done_Face(face);
    FT_Done_FreeType(library);
  }
  void instance(unsigned points, unsigned style) { instance64(points * 64, style); }
  void instance64(unsigned points, unsigned style) {
    FT_Fixed coordinates[] = {FT_Fixed(points * 1024), FT_Fixed((style & 1 ? 700 : 400) * 65536),
                              FT_Fixed(style & 2 ? 65536 : 0)};
    assert(FT_Set_Var_Design_Coordinates(face, 3, coordinates) == 0);
  }
};
void compare(const EpdFont* face, Ft& reference, DirectFace& direct, unsigned points, bool light, bool fractional = false) {
  const auto pixels = fractional ? scalableFontPixelSizeFromPoints64(points) : scalableFontPixelSize26_6(points);
  for (int8_t phase : {-7, 0, 7})
    for (uint32_t cp : {'H', 'T', 'h', 't', 'i'}) {
      EpdGlyph actual{};
      const uint8_t* packed = nullptr;
      assert(face->data->rasterHandler(face->data->glyphMissCtx, cp, phase, actual, packed));
      assert(FT_Set_Char_Size(direct.face, pixels, pixels, 72, 72) == 0);
      FT_Vector shift{phase * 4, 0};
      FT_Set_Transform(direct.face, nullptr, &shift);
      const auto flags = FT_LOAD_NO_BITMAP | (light ? FT_LOAD_TARGET_LIGHT : FT_LOAD_NO_HINTING | FT_LOAD_NO_AUTOHINT);
      assert(FT_Load_Char(direct.face, cp, flags) == 0);
      assert(FT_Render_Glyph(direct.face->glyph, FT_RENDER_MODE_NORMAL) == 0);
      const auto& expected = direct.face->glyph->bitmap;
      assert(actual.width == expected.width && actual.height == expected.rows);
      assert(actual.left == direct.face->glyph->bitmap_left && actual.top == direct.face->glyph->bitmap_top);
      for (unsigned y = 0; y < actual.height; ++y)
        for (unsigned x = 0; x < actual.width; ++x) {
          const unsigned at = y * actual.width + x;
          const unsigned alpha = expected.buffer[y * expected.pitch + x];
          assert(((packed[at / 4] >> (6 - (at % 4) * 2)) & 3) == (alpha * 3 + 127) / 255);
        }
      Ft::GlyphMetrics metric;
      assert(reference.metrics26_6(cp, pixels, metric));
      const int advanceTolerance = (pixels + 3999) / 4000;
      assert(std::abs(fp5::advance(actual) - (metric.advance26_6 + 1) / 2) <= advanceTolerance);
    }
  const auto kern = reference.kerning26_6('H', 'T', pixels);
  const int fp32 = kern >= 0 ? (kern + 1) / 2 : -((-kern + 1) / 2);
  assert(face->getKerningFP5('H', 'T') == fp32);
}
int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string folder = argv[1];
  const auto bytes = read(folder + "/variable.ttf");
  const auto supported = [](const std::vector<uint8_t>& b) {
    return truetype::inspect(b.size(), [&](size_t at, uint8_t* out, size_t size) {
      if (at > b.size() || size > b.size() - at) return false;
      std::memcpy(out, b.data() + at, size);
      return true;
    });
  };
  assert(supported(bytes) == truetype::Result::Supported);
  size_t fvar = 0;
  for (size_t at = 12; at + 16 <= bytes.size(); at += 16) {
    if (std::memcmp(bytes.data() + at, "fvar", 4) == 0) {
      fvar = truetype::u32(bytes.data() + at + 8);
      break;
    }
  }
  assert(fvar);
  for (unsigned axes : {0, 17, 65535}) {
    auto bad = bytes;
    bad[fvar + 8] = axes >> 8;
    bad[fvar + 9] = axes;
    assert(supported(bad) == truetype::Result::Unsupported);
  }
  auto bad = bytes;
  bad[fvar + 10] = 0;
  bad[fvar + 11] = 19;
  assert(supported(bad) == truetype::Result::Unsupported);
  // Inject failure at every size, including after partial registration.
  EpdFontData dummyData{};
  EpdFont dummy{&dummyData};
  for (unsigned failure = 8; failure <= 44; ++failure) {
    std::map<unsigned, const EpdFont*> registry{{100, &dummy}};
    unsigned inserted = 0;
    assert(!registerPublisherFontSizes(
        8, 44, [&](unsigned point) { return registry.count(point); },
        [&](unsigned point, const EpdFont** styles) {
          if (point != failure) styles[0] = &dummy;
          return true;
        },
        [&](unsigned point, const EpdFont* const* styles) {
          registry[point] = styles[0];
          ++inserted;
        },
        [&](unsigned point) { registry.erase(point); }));
    assert(inserted == failure - 8 && registry.size() == 1 && registry.count(100));
  }

  {
    std::map<unsigned, const EpdFont*> registry;
    assert(!registerPublisherFontSizes(
        8, 44, [&](unsigned point) { return registry.count(point); },
        [&](unsigned point, const EpdFont** styles) {
          styles[0] = &dummy;
          if (point == 22) return false;  // A loaded bold face failed optical-size setup.
          styles[1] = &dummy;
          return true;
        },
        [&](unsigned point, const EpdFont* const* styles) { registry[point] = styles[0]; },
        [&](unsigned point) { registry.erase(point); }));
    assert(registry.empty());
  }
  // Shared variable styles and fractional sizes must restore axes/cache identity.
  {
    HalScalableFont shared;
    assert(shared.openMemory(bytes.data(), bytes.size()));
    assert(shared.configurePublisherStyle(0));
    DirectFace direct(bytes);
    Ft reference;
    assert(reference.init(bytes.data(), bytes.size(), 1, 0));
    Ft::RenderOptions options;
    options.hinting = Ft::HintingMode::Light;
    assert(reference.setRenderOptions(options));
    for (unsigned style : {0, 1, 2, 3, 0})
      for (unsigned points64 : {717, 820, 717}) {
        const auto* sized = shared.atSize64(points64, style);
        assert(sized && sized->data->pointSize64 == points64);
        assert(reference.setVariationStyle(style & 1 ? 700 : 400, style & 2));
        assert(reference.setOpticalSize26_6(points64));
        direct.instance64(points64, style);
        compare(sized, reference, direct, points64, true, true);
      }
    for (unsigned points64 = 1001; points64 < 1201; ++points64) {
      for (unsigned style=0; style<4; ++style) assert(shared.atSize64(points64, style));
      shared.releaseSize64(points64);
    }
    assert(shared.atSize64(717, 3));
    assert(!shared.atSize64(0));
    assert(!shared.atSize64(65535));
  }
  // Frozen instances independently verify design metrics and GPOS deltas.
  // Raw FreeType supplies exact pixels without freezing's coordinate rounding.
  Ft::RenderOptions unhinted;
  unhinted.hinting = Ft::HintingMode::None;
  for (bool light : {false, true})
    for (uint8_t style = 0; style < 4; ++style) {
      DirectFace direct(bytes);
      HalScalableFont font;
      auto rendering = unhinted;
      if (light) rendering.hinting = Ft::HintingMode::Light;
      assert(font.openMemory(bytes.data(), bytes.size(), rendering));
      assert(font.configurePublisherStyle(style));
      // Alternating sizes checks that a cached descriptor restores its own opsz.
      for (unsigned points : {8, 12, 22, 44, 8, 22}) {
        const auto frozen = read(folder + "/frozen-" + std::to_string(style) + "-" + std::to_string(points) + ".ttf");
        Ft reference;
        assert(reference.init(frozen.data(), frozen.size(), 1));
        Ft::RenderOptions options;
        options.hinting = Ft::HintingMode::None;
        assert(reference.setRenderOptions(options));
        const auto* face = font.atSize(points);
        assert(face);
        direct.instance(points, style);
        compare(face, reference, direct, points, light);
      }
      assert(!font.configurePublisherStyle(3));
    }
  // Native SD admission keeps the font's default instance, including default opsz.
  HalScalableFont native;
  assert(native.openMemory(bytes.data(), bytes.size(), unhinted));
  const auto defaultBytes = read(folder + "/frozen-0-12.ttf");
  Ft defaultReference;
  assert(defaultReference.init(defaultBytes.data(), defaultBytes.size(), 1));
  Ft::RenderOptions options;
  options.hinting = Ft::HintingMode::None;
  assert(defaultReference.setRenderOptions(options));
  DirectFace defaultDirect(bytes);
  compare(native.atSize(22), defaultReference, defaultDirect, 22, false);
  const auto cff = read(folder + "/cff2.otf");
  HalScalableFont cffFont;
  assert(cffFont.openMemory(cff.data(), cff.size()));
  assert(cffFont.probeGlyph('H', 12));
}
