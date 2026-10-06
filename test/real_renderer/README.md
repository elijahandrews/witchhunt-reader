# Local production-font renderer smoke loop

Run on macOS with Xcode command-line clang. This needs no network, device, CMake, SDL, extra Python packages, or firmware flash:

```sh
python3 test/real_renderer/run.py /absolute/path/to/witch-hunt --out /tmp/witch-render-output
```

The repository argument selects the production sources and font tables to compile.

The executable compiles production `GfxRenderer.cpp`, `EpdFont`, `EpdFontFamily`, `FontDecompressor`, `InflateReader`, uzlib, `GlyphFallback`, and `Utf8`. It includes the actual compressed Bookerly 14 and Noto Sans 14 generated tables from the selected repository. Widths and glyph pixels are not fabricated. The HAL display is an in-memory 800 × 480 device framebuffer, rotated by the production renderer to 480 × 800 portrait. The PNG contains the exact 1-bit framebuffer output.

The test draws 960 combinations of font, scale, tracking, style, and text. It checks the framebuffer's horizontal ink against the promised metrics, including proximity to both ends of the glyph rectangles and a strict measured-width requirement. This catches blank glyphs, end clipping, and drawing outside the promised width. Cases cover both families; scales 0.75, 1, and 1.2; tracking 0, +0.5, +1, and −0.5 px; regular, bold, italic, and bold italic, each with and without small caps; kerning, ligatures, combining acute marks, and digits. Generic family resolution checks reader, serif, and sans selection. The image labels each font, scale, and tracking row.

A separate oracle computes expected advance for `HHHH` from real glyph advance and kerning plus three tracking gaps, without calling the shared glyph walk. These 24 cases catch ignored tracking or scale. Spaces must retain positive advance and draw no pixels. The scaled `AV office fi` cases also catch the negative-bearing metric mismatch found during development.

These are bounded smoke checks, not an exhaustive proof. They do not test SD fonts, cache and prewarming policy, EPUB parser and page wiring, power and UI, grayscale refresh waveforms, or device panel output. Stubs replace hardware refresh, ESP memory and time, and unused SD font paths. `FontCacheManager` scan and prewarm orchestration is disabled; its decoder pointer still delegates to production `FontDecompressor`.

Host portability: `ScaledGlyphEntry` has 64-bit pointers on a host, so its device 16-byte size assertion fails. `run.py` copies the header to the output directory and changes only that assertion to `sizeof(void*) > 4 || sizeof(ScaledGlyphEntry) <= 16`. It copies `GfxRenderer.cpp` byte-identically to preserve quote-include adjacency. No production code is edited, and all glyph and rendering algorithms remain unchanged. The same conditional assertion is a proposed production portability patch that retains the 16-byte bound on 32-bit devices.

Artifacts in the output directory are `real_render`, `frame.pgm`, `frame.png`, copied renderer header and source, and `build-command.json`. The command file records the exact source paths and compiler flags. Re-run after every production change. A full EPUB and UI simulator is a separate integration stage.

During development, deliberate mutations that blanked all glyphs or ignored tracking were both rejected by these checks. The harness also caught a genuine one-pixel underestimate of scaled ligature ink, which was fixed in the production glyph walk.
