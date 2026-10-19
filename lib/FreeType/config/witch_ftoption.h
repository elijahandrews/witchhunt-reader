#pragma once
#include <freetype/config/ftoption.h>
// EPUB outline fonts only: no compressed/bitmap/color/resource containers.
#undef FT_CONFIG_OPTION_USE_ZLIB
#undef FT_CONFIG_OPTION_USE_LZW
#undef FT_CONFIG_OPTION_MAC_FONTS
#undef FT_CONFIG_OPTION_INCREMENTAL
#undef FT_CONFIG_OPTION_SVG
#undef FT_CONFIG_OPTION_ENVIRONMENT_PROPERTIES
#undef FT_CONFIG_OPTION_ADOBE_GLYPH_LIST
#undef FT_CONFIG_OPTION_SUBPIXEL_RENDERING
