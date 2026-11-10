#pragma once

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "SdCardFontSystem.h"
#include "activities/RenderLock.h"
#include "components/UITheme.h"

namespace ReaderUtils {
// A scalable catalog summary does not guarantee that its font bytes are still
// loaded: network handoff, low-memory recovery, or a replaced file may require
// another SD read. Only an unchanged resident family can preview another point
// size without blocking.
inline bool shouldShowFontPreviewLoading(const char* familyName) {
  return !sdFontSystem.canResizeResidentScalableFamilyWithoutReload(familyName);
}

// Gesture/button callers run on the main loop. Resolve the size first so
// clamped gestures remain silent. Changing the in-memory setting does not load
// a font or save settings; the caller performs those and reflow after feedback.
inline bool changeReaderFontSizeWithFeedback(GfxRenderer& renderer, const bool larger,
                                             const FontSizeStepMode mode = FontSizeStepMode::Wrap) {
  RenderLock lock;
  if (!sdFontSystem.changeReaderFontSize(larger, mode)) return false;
  GUI.drawPopup(renderer, tr(STR_LOADING), true);
  return true;
}
}  // namespace ReaderUtils
