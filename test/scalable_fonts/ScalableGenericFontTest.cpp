#include <GfxRenderer.h>
#include <HalScalableFont.h>
#include <ScalableBuiltins.h>

#include <cassert>
int main() {
  GfxRenderer renderer;
  renderer.setMask(prepareScalableGenericFonts(renderer));
  assert(renderer.genericFontMask() == 3);
  const int serif = scalableBuiltinReaderFontId(1, 12), sans = scalableBuiltinReaderFontId(0, 12);
  assert(serif != sans);
  assert(resolveScalableGenericFont(renderer, sans, false, 12) == serif);
  assert(resolveScalableGenericFont(renderer, serif, true, 22) == scalableBuiltinReaderFontId(0, 22));
  const auto before = renderer.getFontMap().size();
  int previous = 0;
  for (unsigned point = 700; point < 900; ++point) {
    const int id = resolveScalableGenericFont64(renderer, serif, true, point);
    assert(id); assert(renderer.getFontMap().at(id).getData()->pointSize64 == point);
    if (previous) assert(renderer.getFontMap().count(previous));
    assert(renderer.getFontMap().size() <= before + 32);
    previous = id;
  }
  assert(resolveScalableGenericFont64(renderer, serif, true, 717));
  EpdFontData customData{}, smallerData{};
  customData.sizeFamily = &customData;
  customData.pointSize = 16;
  smallerData.sizeFamily = &customData;
  smallerData.pointSize = 12;
  EpdFont custom(&customData), smaller(&smallerData);
  renderer.insertFont(1, EpdFontFamily(&custom));
  renderer.insertFont(2, EpdFontFamily(&smaller));
  assert(resolveScalableGenericFont(renderer, 1, false, 12) == 2);
  assert(resolveScalableGenericFont(renderer, 1, true, 16) == scalableBuiltinReaderFontId(0, 16));
  // A failed preflight stays on fallback, even if the global face is now ready.
  renderer.setMask(2);
  assert(resolveScalableGenericFont(renderer, 1, true, 16) == 0);
  assert(resolveScalableGenericFont(renderer, 1, false, 12) == 2);
  assert(resolveScalableGenericFont(renderer, 999, true, 12) == 0);
}
