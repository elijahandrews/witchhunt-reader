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
