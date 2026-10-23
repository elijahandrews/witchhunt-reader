#include <GfxRenderer.h>

int runRepeatedBitmapStemChecks(GfxRenderer&, HalDisplay&);

int main() {
  HalDisplay display;
  GfxRenderer renderer(display);
  renderer.begin();
  return runRepeatedBitmapStemChecks(renderer, display);
}
