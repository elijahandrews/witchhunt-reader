#include <cassert>

#include "activities/reader/ReaderFontLoading.h"
int main() {
  GfxRenderer renderer;
  for (bool resident : {false, true})
    for (bool larger : {false, true}) {
      events.clear();
      sdFontSystem = {resident, true, false};
      assert(ReaderUtils::changeReaderFontSizeWithFeedback(renderer, larger, FontSizeStepMode::Clamp));
      assert((events == std::vector<int>{1, 2}) && !locked);
    }
  events.clear();
  sdFontSystem.effective = false;
  assert(!ReaderUtils::changeReaderFontSizeWithFeedback(renderer, true, FontSizeStepMode::Clamp));
  assert((events == std::vector<int>{1}) && !locked);
  events.clear();
  sdFontSystem.effective = true;
  sdFontSystem.fail = true;
  assert(!ReaderUtils::changeReaderFontSizeWithFeedback(renderer, false, FontSizeStepMode::Clamp));
  assert((events == std::vector<int>{1}) && !locked);
}
