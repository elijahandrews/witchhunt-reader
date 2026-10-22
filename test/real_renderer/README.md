# Local production-font renderer smoke loop

Run on macOS with Xcode command-line clang. This needs no network, device, CMake, SDL, extra Python packages, or firmware flash:

```sh
python3 test/real_renderer/run.py /absolute/path/to/witch-hunt --out /tmp/witch-render-output
```

The repository argument selects the production sources and font tables to compile.

The executable compiles production `GfxRenderer.cpp`, `EpdFont`, `EpdFontFamily`, `FontDecompressor`, `InflateReader`, uzlib, `GlyphFallback`, and `Utf8`. It includes the actual compressed Bookerly 14 and Noto Sans 14 generated tables from the selected repository. Widths and glyph pixels are not fabricated. The HAL display is an in-memory 800 × 480 device framebuffer, rotated by the production renderer to 480 × 800 portrait. The PNG contains the exact 1-bit framebuffer output.

The test draws 960 combinations of font, scale, tracking, style, and text. It checks the framebuffer's horizontal ink against the promised metrics, including proximity to both ends of the glyph rectangles and a strict measured-width requirement. This catches blank glyphs, end clipping, and drawing outside the promised width. Cases cover both families; scales 0.75, 1, and 1.2; tracking 0, +0.5, +1, and −0.5 px; regular, bold, italic, and bold italic, each with and without small caps; kerning, ligatures, combining acute marks, and digits. Generic family resolution checks reader, serif, and sans selection. The image labels each font, scale, and tracking row.

A separate oracle computes expected advance for `HHHH` from real glyph advance and kerning plus three tracking gaps, without calling the shared glyph walk. These 24 cases catch ignored tracking or scale. Spaces must retain positive advance and draw no pixels. The scaled `AV office fi` cases also catch the negative-bearing metric mismatch found during development.

These are bounded smoke checks, not an exhaustive proof. They do not test SD I/O, cache and prewarming policy, EPUB parser and page wiring, power and UI, grayscale refresh waveforms, or device panel output. The optional local-font coverage checks below add SD bitmap decoding and grayscale-plane composition. Stubs replace hardware refresh, ESP memory and time, and unused SD font paths. `FontCacheManager` scan and prewarm orchestration is disabled; its decoder pointer still delegates to production `FontDecompressor`.

Host portability: `ScaledGlyphEntry` has 64-bit pointers on a host, so its device 16-byte size assertion fails. `run.py` copies the header to the output directory and changes only that assertion to `sizeof(void*) > 4 || sizeof(ScaledGlyphEntry) <= 16`. It copies `GfxRenderer.cpp` byte-identically to preserve quote-include adjacency. No production code is edited, and all glyph and rendering algorithms remain unchanged. The same conditional assertion is a proposed production portability patch that retains the 16-byte bound on 32-bit devices.

Artifacts in the output directory are `real_render`, `frame.pgm`, `frame.png`, copied renderer header and source, and `build-command.json`. The command file records the exact source paths and compiler flags. Re-run after every production change. A full EPUB and UI simulator is a separate integration stage.

During development, deliberate mutations that blanked all glyphs or ignored tracking were both rejected by these checks. The harness also caught a genuine one-pixel underestimate of scaled ligature ink, which was fixed in the production glyph walk.

Reduced uppercase regression: the harness now renders Bookerly at 75% size and
checks every A–Z glyph in all four faces at 50%, 75%, and 90%. An independent
source-pixel projection oracle checks every output pixel in BW and both AA
planes, all five darkness settings, captured and staged grayscale, repeated
cache use, and both scaled and tracked text entry points. Small caps are also
covered. Synthetic one-pixel crosses reproduce stems that the former point
sampler skipped; non-BMP aliases exercise the uncached path for 1-bit and 2-bit
source bitmaps. The run prints the number of pixel and metric checks and fails on any mismatch.

To repeat the Adobe Garamond Pro case using a locally installed font file:

```sh
python3 test/real_renderer/run.py /absolute/path/to/witch-hunt \
  --out /tmp/garamond-render \
  --cpfont '/path/to/AdobeGaramondPro_14.cpfont'
```

The optional fixture reads a four-face v4/v5 `.cpfont` into production font structs
and runs the same checks for those faces. Font bytes remain local. This tests the
exact SD glyph bitmaps and metrics, but does not emulate the device's SD loading,
prewarming, or flash cache. `bookerly.png` and optional `sd-font.png` compose the
actual BW and captured grayscale planes at Normal darkness; they model the four
logical ink levels, not the physical e-ink waveform. `frame.png` remains the
original BW smoke sheet. The EPUB's “Reduced uppercase strokes” chapter supplies
matching original text for device testing.

`sd-font-native-aa.png` and `sd-font-native-bw.png` show the same native-size
uppercase/lowercase stem specimen in all four styles, before and after the gray
overlay. For example, pass a local 10pt HD font to inspect small body text without
an additional CSS reduction. The initial BW image draws every nonzero fringe
as black and is intentionally heavier than the settled AA image.

These cases reproduced missing Garamond strokes and jagged Bookerly capitals
before the fix. Shrinking now integrates all contributing source pixels and
retains antialiased coverage. The previous nearest-neighbor downscaler discarded
entire source rows/columns and bypassed grayscale capture.

Baseline-relative scaling is also tested: adding transparent top/left padding
and compensating the glyph bearings must leave every output pixel unchanged.
This storage-invariance check catches per-glyph sampling phase shifts without
assuming a particular font design. It covers negative bearings, descenders,
one- and two-bit sources, reduction and enlargement, and all three planes.
Legacy small caps and synthesized sizes must report bounds containing the ink.
Scaled entry-point tests cover ligatures, combining accents, kerning and scale
cancellation; drawing and measurement use the same glyph walk. Scan-mode tests
also ensure measurement continues to request the needed font glyphs.

Bitmap resampling integrates source coverage and selects each pixel's nearest
available shade independently, with exact half ties choosing the even shade.
Carrying rounding error across a glyph row was removed: it could promote faint
curve fringes into visible spurs and weaken stronger edges, depending on unrelated
pixels to their left. Repeated or flat coverage must always produce the same shade.

The exact-area source-forward oracle checks every output pixel. Independent
geometric tests cover stem error bounds, uniform flat edges, and permutations
and reflections of all quarter-shade coverages in captured/staged, cached/direct,
and rotated paths. Each output must be within half a shade of its own coverage.
Each independently rounded two-edge stem has at most one shade unit of total
error; totals can differ across phases. Forcing identical totals sacrificed edge
accuracy. The earlier
“will” total-ink equality is intentionally not a quality requirement. Native
outline rendering and fractional advances are unchanged by this correction.

Use the synthetic EPUB's **Repeated stems and smooth edges** and **Curves and faint
edges** chapters at 10pt and 12pt, Normal darkness and antialiasing enabled. Inspect
curved letters as well as upright stems, both before and after the gray overlay.
Host images model logical shades, not the physical panel waveform.

## Outline references and quality criteria

For a separate comparison with final-size outlines, install `freetype-py` in a
local Python environment and run:

```sh
python3 test/real_renderer/quality_reference.py \
  --repo /absolute/path/to/witch-hunt --out /tmp/witch-outline-reference
```

Optional `--adobe-font`/`--adobe-cpfont` and `--eb-font`/`--eb-cpfont` pairs add
local Regular outlines and matching legacy 14pt/150 DPI v4 bitmap files.
The third comparison column uses a 2x LIGHT-hinted source raster, matching the
new high-resolution conversion policy. Output must be
outside the repository. The comparison records source hashes, FreeType version,
load flags and limitations in `metadata.json`; no font files are modified.

Compare at actual pixel size as well as nearest-neighbor enlargement. Check
continuous diagonals, preserved thin strokes and counters, consistent baseline
placement, and the font's intended stroke contrast and overshoots. A heavier N
diagonal or taller T is not alone evidence of a renderer defect: compare the
outline before altering its shape. Padding must not change ink, and repeated,
cached, captured and staged rendering must agree.

The reference renders at the final fractional point size with FreeType's LIGHT
hinting, disables stem darkening, and retains 8-bit coverage until final nearest
four-level quantization. FreeType documents why [hinting depends on the final
pixel grid](https://freetype.org/freetype2/docs/glyphs/glyphs-3.html) and how
[LIGHT prioritizes vertical alignment](https://freetype.org/freetype2/docs/reference/ft2-glyph_retrieval.html#ft_load_target_xxx).
This is a useful reference, not a universal aesthetic oracle. Its scaling
columns are independent mathematical models; `run.py` supplies actual
production pixels. Pixel-error measurements diagnose differences, not aesthetic
quality. Monitor gamma and e-ink reflectance differ; these images use ideal
linear coverage and do not predict the physical waveform. See FreeType's
[coverage and display discussion](https://freetype.org/freetype2/docs/hinting/text-rendering-general.html).

The optional `run.py --cpfont` adapter also accepts V5 dense fonts, validates
its CRC, decodes wide glyphs and kerning, and applies the raster density before
CSS scale. It checks SD-font words with accents, ligatures, italic bearings,
negative tracking and kerning as well as individual capitals. Synthetic wide
rasters test dimensions above 255 and bearings outside signed 8-bit range.
The production SD loader and manager have separate host tests; the fixture
adapter does not substitute for those filesystem and cache checks.

V5 fonts can be generated with `fontconvert_sdcard.py --raster-density 2`.
Existing V4 fonts remain supported. See [the format specification](../../docs/cpfont-format.md)
for units, checksum and compatibility, and [the driver audit](DRIVER_AUDIT.md)
for the actual UC8279 X4 bitplane and baseline-restoration checks. Only firmware
with V5 support can read the new files; keep original font sources or V4 backups.


## Runtime fonts and v6 small caps

`run.py --cpfont /path/to/font.cpfont` now loads v4/v5/v6 data through the
production `SdCardFont` mmap path, including authored alternate glyphs and
kerning. It does not substitute a second file-format parser. Tests cover
small-caps and all-small-caps, including independently calculated boundary
spacing and ink bounds.

Add `--outline /path/to/font.ttf` (or `.otf`) to compile the actual vendored
FreeType and `OutlineFontFace` backend. The resulting `outline-font.png` shows
normal text, small caps and all-small-caps at 11.2, 14 and 22.4 points. No font
asset is copied into the repository. This catches integration failures that
comparing advances alone misses, including CFF outlines being scaled twice
when the PostScript hinter was absent from the module registry. The separate
`PublisherFontsTest` uses original geometric CFF/TrueType fixtures to compare
raster bounds and advances against known design units.

Use `--sanitize` for AddressSanitizer and UndefinedBehaviorSanitizer throughout
the renderer, SD loader, optional outline backend and display-driver audit.
The same host-versus-panel limitations still apply.

Dictionary cursor regressions also invert the production framebuffer at fractional scales
(0.525 and 0.75) with signed tracking in both built-in families. The 32 cases cover
all four orientations, overlapping padding, off-screen boxes and a row-tile seam.
Every output pixel must equal the original bit XOR the rectangle union, and a
second inversion must recover the original framebuffer byte-for-byte. This
preserves the exact original raster instead of repainting a suffix at a rounded
pen position. `selection.png` shows a synthetic dash-separated dictionary word selection.


## Fractional glyph positioning

Two-bit text in the EPUB/scaled glyph walk retains horizontal positions to 1/16
of a display pixel. The area filter combines the origin with the scaled bearing
before quantization. Native-size glyphs use an equivalent two-column filter.
This shifts stored coverage; it does not invent higher-resolution outlines for
legacy bitmap fonts or change their hinting. HD bitmap fonts retain their existing denser rasters. Runtime outline fonts can
rasterize at the final size and phase as described below. Native one-bit UI fonts
keep grid fitting.

The bounded scaled-glyph cache includes the horizontal phase in an existing
padding byte; its allocation budget is unchanged. Measuring, selection bounds,
BW, captured AA and staged AA all use the same position. Font advances and
kerning data are unchanged. Fractional word-edge coverage can add one pixel to
an ink box, so section caches are versioned to rebuild their layout.

`fractional.cpp` independently projects source-pixel rectangles onto the output
grid. It tests all 16 horizontal phases, four scales and orientations, all five
darkness settings, negative tracking/bearings, clipped output, cached/repeated
and uncached non-BMP glyphs, and both AA workflows. It also checks the original
Bookerly 10pt advances for “still” and emits `fractional-spacing.png` with original
sample text at four sizes. It detects cache aliases between phases and premature
rounding of a fractional source origin at coverage ties. These are host raster
checks; physical panel quality and page-turn timing still require device testing.


Scaled glyph advances and kerning now accumulate in a 16.16 pen before conversion
to the drawing phase or final integer width. Long odd-advance runs detect repeated
rounding bias; the test covers integer-only and antialiased fonts, synthetic caps,
negative kerning/tracking and selected-character geometry. The old per-glyph
rounding fails the retained-advance negative control. Exact ink measurements reuse
the walk's vertical bounds instead of looking up each glyph and raster again;
`test/text_ink_metrics` checks geometry and provider call counts separately.


## Final-size outlines and native TTF adapters

The optional outline run also builds `outline_quality.cpp`. Its independent
double-precision pen walk copies the final raster pixels directly into the documented
SDK planes, without using `TextRun`, `GlyphScale`, renderer clipping, or renderer
cache helpers. It checks 4/8/16 phase modes, all four orientations, left/top and
right/bottom clipping, negative bearings/tracking, combining marks, both small-cap
modes, scale changes, measured ink bounds, captured/staged equality, cache churn,
and face reopen. An advance-only comparison holds layout metrics fixed across
the three raster policies. Raw FreeType agreement has a separate backend test;
these cases check integration into the actual renderer. The reference sums documented FP32/Q16-rounded terms in double precision and separately bounds them against an unquantized pen. Framebuffer and final-width comparisons remain exact, including boundary cases; binary-scale long-run checks retain exact width assertions.

The run emits 18 `outline-quality-*.png` screens for 8/10/12pt, 4/8 phases, and
legacy quantized-bitmap projection, 8-bit 2x projection, and final-size LIGHT
hinting. `outline-comparison-q4.png` and `outline-comparison-q8.png` place those
policies in columns and point sizes in rows. `outline-detail-p8-q*.png` enlarges
the 8pt samples by exact pixel replication. Source, optical size, advances,
kerning, strings and baselines stay fixed within each comparison. These images
do not model the panel waveform, lighting, physical gray levels or refresh time.

Test a local four-style native TTF family through the production flash store:

```sh
python3 test/real_renderer/run.py . --out /path/to/ignored/output --sanitize \
  --native-fonts /path/Regular.ttf /path/Bold.ttf /path/Italic.ttf /path/BoldItalic.ttf
```

`native_outline.cpp` compares the adapter's actual pixels to a separately opened
outline face, including phase zero, all four styles, body and native pixel sizes,
unchanged legacy advances/kerning, captured/staged frames, and close/reload. Its
NOR wrapper releases the memory behind an unmapped style, while maintaining
coherent live index/log mappings. ASan therefore detects a stale style pointer
that an ordinary permanent RAM buffer could conceal. No font assets are copied
into the source tree.

## Embedded stack budget

Large host thread stacks can hide failures that reset the reader. The outline
and native-font `run.py` modes automatically run the host compiler guard. Before
a runtime font release, also run the actual-target and constrained-thread check:

```sh
python3 test/real_renderer/stack_budget.py \
  --out /tmp/witch-font-stack \
  --target-cc /path/to/xtensa-esp32s3-elf-gcc \
  /path/to/local-cff.otf /path/to/local-truetype.ttf
```

It compiles **all** vendored FreeType modules with `-Os -fstack-usage`, rejecting
individual frames over 4 KiB on the 64-bit host or 2 KiB on the optional actual
Xtensa compiler, plus unbounded dynamic frames. It checks that the CFF
interpreter, grayscale converter, and auto-hinter are present in the reports;
an absent compiler report cannot pass. `--static-only` runs the compiler guard
without font fixtures. Reports and exact commands are saved in `stack-usage.json`.

The runtime probe opens each streamed face and renders native/LIGHT, final-size/
oversampled glyphs at several sizes and phases on a 32 KiB pthread stack. An
8 KiB live stack reservation accounts for application callers. Because macOS
rounds the actual pthread allocation upward, stream and allocation callbacks
also enforce the requested 32 KiB boundary explicitly and report observed depth.
The probe exercises heap allocation, CFF accented glyphs, and cold caches.
This probe deliberately does not use sanitizers, whose stack instrumentation
would change that budget; run the normal renderer sanitizer suite separately.
The compiler bound is per function and the pthread is a different ABI, so neither
is a proof of every device call chain. Both supplement actual hardware navigation
and stack high-water checks. No private font is copied into the repository.
