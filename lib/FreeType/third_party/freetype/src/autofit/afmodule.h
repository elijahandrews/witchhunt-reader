/****************************************************************************
 *
 * afmodule.h
 *
 *   Auto-fitter module implementation (specification).
 *
 * Copyright (C) 2003-2026 by
 * David Turner, Robert Wilhelm, and Werner Lemberg.
 *
 * This file is part of the FreeType project, and may only be used,
 * modified, and distributed under the terms of the FreeType project
 * license, LICENSE.TXT.  By continuing to use, modify, or distribute
 * this file you indicate that you have read the license and
 * understand and accept it fully.
 *
 */


#ifndef AFMODULE_H_
#define AFMODULE_H_

#include <freetype/internal/ftobjs.h>
#include <freetype/ftmodapi.h>

#include "ft-hb.h"
#include "aflatin.h"
#include "afcjk.h"

FT_BEGIN_HEADER


  /*
   * This is the `extended' FT_Module structure that holds the
   * autofitter's global data.
   */

  /* Witch-hunt: glyph hinting can discover script metrics recursively, */
  /* so those two operations need distinct bounded hint workspaces.     */
  /* The metrics union is shared only between sequential writing systems. */
  typedef struct  AF_ModuleScratchRec_
  {
    AF_GlyphHintsRec  glyph_hints;
    AF_GlyphHintsRec  metrics_hints;
    union
    {
      AF_LatinMetricsRec  latin;
      AF_CJKMetricsRec    cjk;
    } metrics;

  } AF_ModuleScratchRec, *AF_ModuleScratch;


  typedef struct  AF_ModuleRec_
  {
    FT_ModuleRec  root;

    AF_ModuleScratch  scratch;  /* lazy, per-module FT_Memory allocation */

    FT_UInt       fallback_style;
    AF_Script     default_script;
    FT_Bool       no_stem_darkening;
    FT_Int        darken_params[8];

#if defined( FT_CONFIG_OPTION_USE_HARFBUZZ )         && \
    defined( FT_CONFIG_OPTION_USE_HARFBUZZ_DYNAMIC )
    ft_hb_funcs_t*  hb_funcs;
#endif

  } AF_ModuleRec, *AF_Module;


FT_DECLARE_AUTOHINTER_INTERFACE( af_autofitter_interface )
FT_DECLARE_MODULE( autofit_module_class )


FT_END_HEADER

#endif /* AFMODULE_H_ */


/* END */
