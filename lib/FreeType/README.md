# Pinned FreeType outline backend

This is a curated source subset from the official FreeType 2.14.3 release, including its public headers and license. Only the explicit wrapper translation units under `src` compile. The source subset retains the complete relevant module directories so their internal includes stay from the same release.

Source: https://download.savannah.gnu.org/releases/freetype/freetype-2.14.3.tar.xz

Archive SHA-256: `36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f`.
Archive SHA-1: `62e26b89a057ad4f1e28af977945d5c1975a8e67` (matches the official release listing).

The module registry contains SFNT, TrueType, CFF/CFF2, PostScript auxiliary support, native hinter and names, grayscale smoothing, and optional autohinting. Native TrueType hinting and variation support remain enabled. No gzip/WOFF, bitmap/color/image or legacy Macintosh resource container dependencies compile. Adobe glyph-name-to-Unicode conversion is disabled; supported OpenType fonts must provide a Unicode cmap.

`WITCH_FONT_AUTOHINT=1` enables light/autohint rendering. It costs approximately 45 KiB of ESP32-S3 object code before linker garbage collection. The full engine with autohint measures approximately 198 KiB before garbage collection; this is not a replacement for the final linked firmware size check.

The configuration headers select options. One documented source backport changes `tt_done_blend` to use `blend->num_axis` during cleanup, matching [upstream](https://github.com/freetype/freetype/blob/master/src/truetype/ttgxvar.c): release 2.14.3 dereferences a null `blend->mmvar` after a variable-font allocation failure. The real-backend host tests inject each of the first 49 allocation failures and verify clean rejection and zero retained allocations. The local `witch_ftoption.h` name avoids FreeType's relative include resolving its own default `ftoption.h` first. See the bundled `third_party/freetype/LICENSE.TXT` and `docs/FTL.TXT` for distribution requirements.

The CFF native lightly-hinted path requires the PostScript hinter module even when using the Adobe CFF engine. Omitting it leaves the CFF decoder without `hints_funcs` and makes its loader scale already-hinted outline points a second time. It is included explicitly; native/light CFF raster bounds are tested against independent design-unit/reference bounds.

## Bounded stack use

The reader uses 8 KiB loop and 10 KiB render task stacks. Stock 2.14.3 places large CFF hint maps, the grayscale cell pool and autofitter hint arrays on the stack, exceeding those limits before the caller's own frames. Local source changes move that existing storage into allocator-owned workspaces without changing the raster/hinting algorithms or reducing their capacities:

- The CFF instance lazily retains one interpreter block per supported recursion level (root glyph and one `seac` component). Nested `seac` remains rejected; each call still initializes and finalizes its internal arrays, and the instance finalizer frees both blocks.
- The grayscale raster retains its full cell pool in the raster allocation.
- The autofitter module lazily retains separate glyph and metrics hint workspaces plus a union for the sequential Latin/CJK metrics scratch. They are allocated before glyph loading and released with the module. Metrics discovery performs nested unscaled loads, so it must not reuse the active glyph workspace.

All allocations use `FT_Memory`, hence the existing per-face/shared PSRAM budgets. The new workspaces are reused across glyphs; no permanent task-stack increase or global static scratch is introduced. One `OutlineFontFace` owns one FreeType library and requires serialized calls on that face. Different faces/libraries can render concurrently; callbacks must not recursively render through the same library. The firmware's render lock preserves this contract.

Two related 2.14.3 failure paths also needed correction: the optional reverse character map can be null after allocation failure, so both adjustment consumers check it, and failed hash-table initialization frees only its empty wrapper before common cleanup. This preserves ordinary hinting when optional adjustments are unavailable.

`test/real_renderer/stack_budget.py` rejects oversized optimized host/target frames and probes constrained runtime stack use; `test/outline_font/run_scratch.py` tests generated CFF counter-mask/seac input, nested-component rejection, allocation failure/recovery, scratch reuse, cleanup and concurrent independent faces. The corrected Xtensa build's largest audited FreeType frame is 1,568 bytes. Stack frames add along call chains, so the runtime probe and on-device validation remain necessary.
