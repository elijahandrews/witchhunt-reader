# CrossPoint Reader — Durable Context

Keep this file focused on repo-specific gotchas that are worth reusing in future sessions.

## FreeInk SDK

Refer to https://freeink.org/llms.txt for guidance.

## Simulator

- Simulator patches belong in the adjacent `crossink-simulator` repo.
- The local simulator env is `simulator`; use `x4-pro-simulator` for touch/PSRAM paths. The companion HAL changes described in `docs/epub-parity.md` are pinned in `platformio.ini`; no personal override is needed.
- `CROSSPOINT_SIM_USE_NATIVE_DECODERS` enables the real PNG/JPEG/GIF libraries; the old decoder-stub limitation no longer applies to these environments.
- Known simulator limits:
  - The local display adapter uses ordinary refresh; it cannot validate physical single-pass text antialiasing or panel timing.
  - The local storage adapter declines trusted content-hash scans because host files can change outside HAL observation; live Pluto synchronization needs hardware verification.
  - `esp_deep_sleep_start()` is a no-op in simulator.
  - `HalStorage` uses POSIX file access under `./fs_` and allows multiple readers, unlike real hardware.

## Real Hardware / Storage

- SdFat on hardware allows only one open reader per file path at a time. If a fallback needs to reopen the same file, close the first handle before reopening.
- `HalStorage::openFileForWrite` must retain the SDK's `O_RDWR | O_CREAT | O_TRUNC` contract: incremental EPUB layout seeks and reads completed pages through the same handle before closing it. Host storage doubles must enforce access modes.

## Rendering / Reader Pipeline

- `lib/Epub/Epub/Page.cpp`: images must render only in `GfxRenderer::BW`; grayscale passes are text anti-aliasing passes only.
- `Page::forEachImage` includes table-cell pictures. Use it for image bounds, decoding, masks and warming instead of scanning only `TAG_PageImage` elements.
- Derived font IDs can be evicted. Persist the stable reader ID, family token and exact point size, then resolve at use time.
- Kindle EPUBs may contain paired high-res and old-Kindle fallback images. `ChapterHtmlSlimParser` should skip `<img>` nodes with `data-AmznRemoved-M8` to avoid duplicate stacked images.
- After image/layout pipeline changes that affect cached EPUB output, clear the affected `.crosspoint/epub_<hash>/` cache if behavior looks stale.

## UI Consistency

- Use FreeInkUI SDK components and input routing for list-style screens where possible. Row rendering, touch targets,
  hit testing, and pagination should share the same FreeInkUI list configuration instead of custom touch scaling.

## Heap Baselines (X4 hardware, SD card font)

- A normal resume-into-partial reading session runs at ~85-90KB free / ~49KB maxAlloc by
  the first watermark crossing (Epub metadata + x-locations + resident glyph caches).
  Do not read mid-range heap numbers as session degradation without checking the scenario.
- SD-font section builds cost ~38-50KB at cold start; the 4-style advance-table prewarm
  (~30KB incl. 16KB contiguous scratch) dominates and is skipped below 80KB free.

## Misc Repo Gotchas

- POSIX TZ signs are inverted from ISO 8601 in `TimeStore::applyTimezone()`: `"UTC-1"` means UTC+1.
- `LyraTheme::drawHeader()` does not call `BaseTheme::drawHeader()`, so header changes in the base theme must be duplicated in Lyra if needed.
