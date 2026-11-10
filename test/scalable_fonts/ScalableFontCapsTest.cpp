#include <EpdFontFamily.h>
#include <HalScalableFont.h>
#include <ReaderMono.h>

#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>
int main(int argc, char** argv) {
  assert(argc == 4);
  std::ifstream file(argv[1], std::ios::binary);
  std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
  HalScalableFont font;
  assert(font.openMemory(bytes.data(), bytes.size()));
  const auto* face = font.atSize(12);
  EpdFontFamily family(face);
  const auto h = family.smallCapsCodepoint('h', EpdFontFamily::SMALL_CAPS);
  const auto t = family.smallCapsCodepoint('t', EpdFontFamily::SMALL_CAPS);
  assert(h == 0x110000u + std::strtoul(argv[2], nullptr, 10));
  assert(t == 0x110000u + std::strtoul(argv[3], nullptr, 10));
  assert(family.smallCapsCodepoint('H', EpdFontFamily::SMALL_CAPS) == 0);
  assert(family.smallCapsCodepoint('H', EpdFontFamily::SMALL_CAPS, true) == h);
  assert(family.smallCapsCodepoint('x', EpdFontFamily::SMALL_CAPS) == 0);
  const auto* cap = family.getGlyph(h);
  assert(cap);
  const auto capHeight = cap->height;
  assert(capHeight < family.getGlyph('H')->height);
  freeink::font::FtFont reference;
  assert(reference.init(bytes.data(), bytes.size(), 1));
  freeink::font::FtFont::RenderOptions options;
  options.hinting = freeink::font::FtFont::HintingMode::Light;
  assert(reference.setRenderOptions(options));
  for (int phase = -8; phase <= 7; ++phase) {
    EpdGlyph actual{};
    const uint8_t* pixels = nullptr;
    assert(face->data->rasterHandler(face->data->glyphMissCtx, h, phase, actual, pixels));
    const auto* expected = reference.rasterizeGlyph26_6(h - 0x110000, scalableFontPixelSize26_6(12), phase * 4);
    assert(expected && actual.width == expected->width && actual.height == expected->height);
    for (unsigned i = 0; i < unsigned(actual.width) * actual.height; ++i)
      assert(((pixels[i / 4] >> (6 - 2 * (i % 4))) & 3) == (unsigned(expected->pixels[i]) * 3 + 127) / 255);
  }
  for (uint8_t scale64 : {32,48,64,48,32,64}) for(int phase=-8;phase<=7;++phase) {
    EpdGlyph actual{}; const uint8_t* pixels=nullptr;
    assert(face->data->scaledRasterHandler(face->data->glyphMissCtx,'H',scale64,phase,actual,pixels));
    const auto px=(scalableFontPixelSize26_6(12)*scale64+32)/64;
    const auto* expected=reference.rasterize26_6('H',px,phase*4);
    assert(expected && actual.width==expected->width && actual.height==expected->height);
    assert(actual.left==expected->xoff && actual.top==-expected->yoff);
    for(unsigned i=0;i<unsigned(actual.width)*actual.height;++i)
      assert(((pixels[i/4]>>(6-2*(i%4)))&3)==(unsigned(expected->pixels[i])*3+127)/255);
  }
  assert(family.getKerning(h, t) < 0);
  const auto expectedLigature = 0x110000u + reference.glyphId(0xfb01);
  for (const char* text : {"ffi", "HI"}) {
    const char* tail = text + 1;
    assert(family.applyLigatures(text[0], tail) == expectedLigature);
    assert(*tail == '\0');
  }
  const char* tail = "fi";
  assert(family.applyLigatures('f', tail, EpdFontFamily::SMALL_CAPS) == 'f');
  assert(*tail == 'f');
  HalScalableFont streamed;
  assert(streamed.openFile(argv[1], bytes.size(), options, HalScalableFont::FileMode::Stream));
  EpdFontFamily streamedFamily(streamed.atSize(12));
  assert(streamedFamily.getKerning(h, t) == family.getKerning(h, t));
  const char* directTriple = "fi";
  assert(streamedFamily.applyLigatures('f', directTriple) == expectedLigature);
  assert(*directTriple == '\0');
  for (uint8_t style = 0; style < 4; ++style) {
    size_t count = 0;
    const auto* source = readerMono::data(style, &count);
    HalScalableFont mono;
    assert(mono.openMemory(source, count));
    for (uint16_t points64 : {uint16_t(12 * 64), uint16_t(717)}) {
      const auto* sized = mono.atSize64(points64);
      assert(sized);
      const auto width = fp5::advance(*sized->getGlyph('i'));
      for (uint32_t cp : {'W', 'm', '0', ' '}) assert(fp5::advance(*sized->getGlyph(cp)) == width);
    }
  }
  freeink::font::FtFont denied;
  assert(denied.init(bytes.data(), bytes.size(), 1));
  assert(!denied.retainLayoutTables(1));

}
