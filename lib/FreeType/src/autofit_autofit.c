#define FT2_BUILD_LIBRARY
#define FT_CONFIG_OPTIONS_H <witch_ftoption.h>
#define FT_CONFIG_MODULES_H <witch_ftmodule.h>
#if WITCH_FONT_AUTOHINT
#include "../third_party/freetype/src/autofit/autofit.c"
#endif
