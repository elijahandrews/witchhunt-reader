This target compiles the production SD font manager, registry, loader, and font
classes with real temporary files. The renderer double records font registration,
base scale, aliases, and removal. The flash double models persistent entries, maps
backed by their actual bytes, capacity, and erase/write counters. It does not
exercise the ESP flash driver or draw pixels; the production pixel and panel-wire
tests live in `test/real_renderer`.

Run from a configured host test build:

```sh
cmake --build /tmp/witch-hunt-host-build --target SdFontManagerTest
/tmp/witch-hunt-host-build/sd_font_manager/SdFontManagerTest
```

The cases verify V4/V5 raster scales and composed size aliases; stale V4 and
changed V5 payload cache refresh; unchanged cache hits; read-only loading without
flash writes; oversized source fallback preserving an existing cache; and font
registration cleanup. All fixture text and family names are synthetic.
