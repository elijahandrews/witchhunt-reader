# Runtime outline backend host checks

Run from the repository root, providing local fixtures with Latin small caps, capital small caps and an fi ligature:

```sh
python3 test/outline_font/run.py /path/to/variable.ttf /path/to/static.otf /path/to/optical-variable.otf
python3 test/outline_font/run.py --sanitize /path/to/variable.ttf /path/to/static.otf /path/to/optical-variable.otf
```

The runner compiles the actual checked-in FreeType modules and `OutlineFontFace`, not a system font library. It checks memory/stream pixel and metric identity, real small-cap alternates, ligatures, pair kerning, layout-table allocation, optical-axis changes, borrowed empty-glyph behavior, malformed sources, excessive sizes, allocator exhaustion, shared-bank denial/recovery, each of the first 49 single allocation failures during real FreeType startup/face work, and release of all tracked face allocations. The final fixture should have an optical axis so that branch is exercised. No fixture font is copied into the repository. The tests do not cover hardware timing, physical display calibration, or all complex shaping. Variable GPOS deltas have the separate regression below.


`RasterFontTest` compares the production callback against an independent FreeType path that loads an outline, translates it explicitly, renders it, and scatters 8-bit coverage into final cells. It checks every signed phase for 4/8/16-step policies; FinalSize/Oversample2x; Light/Native/None hinting; small and fractional sizes; italic/negative bearings; empty glyphs; metadata and pixel-arena eviction; invalid inputs; 32 allocation-failure positions; borrowed bitmap lifetime; and unchanged legacy metrics, bitmaps, kerning and line height after rasterization. Cache hits must not allocate. All tracked payload must be released on close.

For arbitrary fonts without the original suite's small-cap/ligature requirements, use:

```sh
python3 test/outline_font/run.py --sanitize --raster-only --out /path/to/ignored/output /path/to/font.ttf
```

These are exact coverage/contract checks, not a claim about the physical panel's grayscale response or a universal preference for one hinting policy.

### Retained scratch and allocation-failure regression

Run `python test/outline_font/run_scratch.py --sanitize --out /tmp/outline-scratch [additional-fonts...]` using a Python environment with fontTools. The runner generates original CFF hint-mask/seac fixtures, builds the actual backend, and tests the bundled Bookerly plus any supplied CFF/TTF/CFF2 fonts. It covers nested-seac rejection, all first 100 allocation denials per font, cleanup, recovery, reuse of the large scratch blocks and concurrent independent faces. Font bytes remain outside source control; only the original fixture generator is checked in. `STACK_SKIP_FAILURES=1` is reserved for comparing ordinary bitmap/metric hashes with an older backend that has known allocation-failure defects.

### Variable GPOS and bounded parsing

Run with a Python environment containing fontTools:

```sh
python test/outline_font/run_variations.py --sanitize --out /tmp/outline-variations
python test/outline_font/run_variations.py --sanitize --out /tmp/outline-variations-local --real-font /path/to/variable-1000-upem.ttf
```

The runner generates an original geometric variable font with non-linear `avar` mapping and weight-dependent pairs. `make_variation_fixture.make(path)` is also the source for the synthetic EPUB's readable H/T spacing specimen; no downloaded font is embedded by the generator. The optional local fixture must have a weight axis, a GDEF variation store and 1000 units per em.

The suite compares every printable ASCII pair at twelve weights against fontTools' `VarStoreInstancer`, using normalized coordinates exported by independent public FreeType calls. Direct static instancing in fontTools quantizes those coordinates to F2DOT14, so it is deliberately not the oracle: that loses precision at half-unit kerning boundaries. Hand-calculated parser cases separately check multi-axis/region interpolation, signed rounding, PairSet versus PairPos offsets, extension lookups, device-only records, mixed short/long deltas, malformed indices/counts and work exhaustion. Full-face extreme LONG_WORDS fixtures verify saturation without overflow at minimum, middle and maximum supported sizes.

Each byte-prefix and byte-mutation case runs under ASan/UBSan. Memory and stream sources, exact/undersized table budgets, default-instance and layout-disabled options, every startup allocation denial, zero retained allocation leaks, no allocation during repeated pair lookup, and unchanged kerning after final-size rasterization are checked. The runner also runs the existing exact raster-reference suite on its ordinary fixtures. These checks verify software contracts, not physical display quality or hardware latency.
