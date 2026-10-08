#pragma once

class GfxRenderer;
class HalDisplay;

// Tests actual glyph planes; the saved fixtures are subsequently replayed by
// the complete SDK facade/driver audit, not by the in-memory renderer HAL.
void runDarkAaFontChecks(GfxRenderer& renderer, HalDisplay& display, int fontId, const char* label);
