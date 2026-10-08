# Prepared text antialiasing on X4 Pro

With text antialiasing enabled, compatible UC8279 X4 Pro readers capture the
black-and-white page and its two grayscale masks together. The next-page
pre-render retains those masks in PSRAM, so showing that page does not need to
scan, warm, and repaint its glyphs again for smoothing. Fresh text pages also
capture their masks during the initial draw.

The display still uses its existing black-and-white refresh followed by the same
grayscale overlay. No waveform, voltage, contrast, or tone mapping is changed.
This removes repeated rendering work; it does not eliminate the panel's physical
grayscale refresh. There is no new setting.

One reusable pair occupies 96,000 bytes of PSRAM (about 94 KiB). Allocation is
explicitly restricted to PSRAM and never falls back to internal RAM. Images,
unsupported panels, unavailable secondary buffers, and allocation failures retain
the existing rendering path. The cache is released when the reader activity exits.

Prepared masks belong to a particular section, page, font, orientation, darkness,
and content position. Navigation, overlays, layout changes, and discarded
pre-renders invalidate them. A forward turn can consume the prepared next page;
its live status bar updates both the BW pixels and retained masks. Controller RAM
is not touched during preparation. Upload starts only after that page's BW refresh
finishes. A cancelled upload restores the existing BW/controller baseline without
triggering the grayscale waveform.

## Verification

`PreparedGrayscaleCacheTest` covers allocation reuse, partial allocation failure,
replacement failure, cancellation, and mismatched page/layout identities.
`test/real_renderer/run.py` uses the production renderer to compare captured and
replayed page pixels across all orientations, darkness levels, and several scales.
It checks live chrome over glyph fringes, exact mask uploads, and cancellation at
every upload boundary. The actual UC8279 driver audit verifies wire encoding,
LUT68, unchanged BW baselines, and subsequent page turns after cancelled uploads.
Existing glyph, scaling, and overlap checks cover the local SD fonts too.

The harness reports initial BW preparation and total preparation separately.
Host timings are diagnostic only: device latency must be measured on the reader.
Serial logs distinguish `Prepared AA fresh` and `Prepared AA cached`, with original
preparation, upload, grayscale refresh, and cleanup times. Existing page-render
logs provide the surrounding latency and pre-render hit/miss information.

For a device check, use the synthetic **Witch EPUB Features Test** already on the
card. Find `Marker 0048`, pause to allow the next page to prepare, then turn forward.
Compare with a backward turn and a rapid sequence of turns. Repeat after resizing,
rotating, opening a menu, and using an image page. Font edges and background should
match the previous firmware; a prepared forward turn should avoid the glyph-replay
work after the BW page appears. Hardware timing and physical appearance require a
post-flash check; host tests do not simulate ink settling.

The experimental branch also offers a Direct grayscale comparison and a switch
for next-page preparation; see [page-turn antialiasing](page-turn-antialiasing.md).
