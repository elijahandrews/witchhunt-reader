# Pinned FreeType outline backend

This is a curated source subset from the official FreeType 2.14.3 release, including its public headers and license. Only the explicit wrapper translation units under `src` compile. The source subset retains the complete relevant module directories so their internal includes stay from the same release.

Source: https://download.savannah.gnu.org/releases/freetype/freetype-2.14.3.tar.xz

Archive SHA-256: `36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f`.
Archive SHA-1: `62e26b89a057ad4f1e28af977945d5c1975a8e67` (matches the official release listing).

The module registry contains SFNT, TrueType, CFF/CFF2, PostScript auxiliary support, native hinter and names, grayscale smoothing, and optional autohinting. Native TrueType hinting and variation support remain enabled. No gzip/WOFF, bitmap/color/image or legacy Macintosh resource container dependencies compile. Adobe glyph-name-to-Unicode conversion is disabled; supported OpenType fonts must provide a Unicode cmap.

`WITCH_FONT_AUTOHINT=1` enables light/autohint rendering. It costs approximately 45 KiB of ESP32-S3 object code before linker garbage collection. The full engine with autohint measures approximately 198 KiB before garbage collection; this is not a replacement for the final linked firmware size check.

The configuration headers select options. One documented source backport changes `tt_done_blend` to use `blend->num_axis` during cleanup, matching [upstream](https://github.com/freetype/freetype/blob/master/src/truetype/ttgxvar.c): release 2.14.3 dereferences a null `blend->mmvar` after a variable-font allocation failure. The real-backend host tests inject each of the first 49 allocation failures and verify clean rejection and zero retained allocations. The local `witch_ftoption.h` name avoids FreeType's relative include resolving its own default `ftoption.h` first. See the bundled `third_party/freetype/LICENSE.TXT` and `docs/FTL.TXT` for distribution requirements.

The CFF native lightly-hinted path requires the PostScript hinter module even when using the Adobe CFF engine. Omitting it leaves the CFF decoder without `hints_funcs` and makes its loader scale already-hinted outline points a second time. It is included explicitly; native/light CFF raster bounds are tested against independent design-unit/reference bounds.
