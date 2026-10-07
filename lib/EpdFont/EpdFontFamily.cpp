#include "EpdFontFamily.h"

const EpdFont* EpdFontFamily::getFont(const Style style) const {
  // Extract font style bits (ignore UNDERLINE bit for font selection)
  const bool hasBold = (style & BOLD) != 0;
  const bool hasItalic = (style & ITALIC) != 0;

  if (hasBold && hasItalic) {
    if (boldItalic) return boldItalic;
    if (bold) return bold;
    if (italic) return italic;
  } else if (hasBold && bold) {
    return bold;
  } else if (hasItalic && italic) {
    return italic;
  }

  return regular;
}

void EpdFontFamily::getTextDimensions(const char* string, int* w, int* h, const Style style) const {
  getFont(style)->getTextDimensions(string, w, h, (style & ALL_SMALL_CAPS) ? 2 : (style & SMALL_CAPS) ? 1 : 0);
}

const EpdFontData* EpdFontFamily::getData(const Style style) const { return getFont(style)->data; }

EpdGlyphRef EpdFontFamily::getGlyph(const uint32_t cp, const Style style) const { return getFont(style)->getGlyph(cp); }

uint32_t EpdFontFamily::resolveCaps(uint32_t cp, Style style, float& scale) const {
  return getFont(style)->resolveCaps(cp, (style & ALL_SMALL_CAPS) ? 2 : (style & SMALL_CAPS) ? 1 : 0, scale);
}

int16_t EpdFontFamily::getKerning(const uint32_t leftCp, const uint32_t rightCp, const Style style) const {
  return getFont(style)->getKerning(leftCp, rightCp);
}

uint32_t EpdFontFamily::applyLigatures(const uint32_t cp, const char*& text, const Style style) const {
  // Normal fi/ff substitutions precede caps in neither CSS nor OpenType: they
  // would hide the letters from smcp. Keep the letters separate in caps runs.
  if (style & (SMALL_CAPS | ALL_SMALL_CAPS)) return cp;
  return getFont(style)->applyLigatures(cp, text);
}
