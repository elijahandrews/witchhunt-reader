#pragma once
#if CROSSINK_SCALABLE_FONTS
#include <cstdint>
class GfxRenderer;
void ensureScalableBuiltinFamily(GfxRenderer& renderer, unsigned family);
unsigned prepareScalableGenericFonts(GfxRenderer& renderer);
int scalableBuiltinFontId(int legacyId);
int resolveScalableGenericFont(GfxRenderer& renderer, int readerFontId, bool sans, unsigned points);
int resolveScalableGenericFont64(GfxRenderer& renderer, int readerFontId, bool sans, uint16_t points64);
int resolveScalableBuiltinSize64(GfxRenderer& renderer, int readerFontId, uint16_t points64);
void retryScalableBuiltinSizes();
int scalableBuiltinReaderFontId(unsigned family, unsigned points);
#endif
