# Runtime outline backend host checks

Run from the repository root, providing local fixtures with Latin small caps, capital small caps and an fi ligature:

```sh
python3 test/outline_font/run.py /path/to/variable.ttf /path/to/static.otf /path/to/optical-variable.otf
python3 test/outline_font/run.py --sanitize /path/to/variable.ttf /path/to/static.otf /path/to/optical-variable.otf
```

The runner compiles the actual checked-in FreeType modules and `OutlineFontFace`, not a system font library. It checks memory/stream pixel and metric identity, real small-cap alternates, ligatures, pair kerning, layout-table allocation, optical-axis changes, borrowed empty-glyph behavior, malformed sources, excessive sizes, allocator exhaustion, shared-bank denial/recovery, each of the first 49 single allocation failures during real FreeType startup/face work, and release of all tracked face allocations. The final fixture should have an optical axis so that branch is exercised. No fixture font is copied into the repository. The tests do not cover hardware timing, physical display calibration, all complex shaping, or variable GPOS deltas.
