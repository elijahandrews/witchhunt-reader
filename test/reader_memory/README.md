# Reader optional-AA memory lifecycle

`run.py` extracts the production EPUB exit prefix (through current-page restoration),
stage-1 failed-page-load recovery block, and complete content-render/restoration functions.
It compiles them with the production `BufferedGrayscalePlanes` allocator and a bounded
800000-byte host PSRAM shim. Device services, glyph rendering and driver workspace
allocation are stubbed; this test does not measure hardware timing or prove SDK behavior.

The checks verify:

- A pre-rendered page is restored using its existing 96000-byte mask allocation on exit,
  with no second pair allocated. The 529024-byte driver workspace is released early enough
  for the simulated 200000-byte page load to fit.
- Successful, failed and skipped exit restoration all release optional memory and disarm
  capture pointers, with no controller plane uploads.
- The first allocation-recovery pass frees both optional allocations and invalidates cached
  masks/handoff state before preserving the existing font/CSS/section retry behavior.
- An identical simulated page allocation then succeeds, and optional capture can resume
  after the page is safely loaded.

Run from the repository root:

```sh
python3 -B test/reader_memory/run.py --out /tmp/reader-memory --sanitize
```

`--case exit` and `--case oom` select individual regressions. The standard host CTest suite
registers the same checks without sanitizer flags as `ReaderOptionalAaMemory`.
