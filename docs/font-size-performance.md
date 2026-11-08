# Font-size change performance

Bitmap font families can exceed the flash font partition even when individual
sizes fit. Previously, changing between two such sizes erased the font partition
and copied the newly selected size from SD each time. While a bitmap family is
loaded, size changes now reuse matching flash entries or load the requested file
directly from SD. The existing flash entry remains available when returning to
that size. Loading a different family or reopening a book can still populate the
flash cache; replacing the file at the same size still refreshes a stale entry.
TrueType size handling is unchanged.

An uncached size consequently uses SD for glyph data until a later normal load
caches it. This trades flash installation during a resize for SD reads during
rendering; the effect on physical page-turn latency needs device measurement.
The requested bitmap size and glyph metrics remain unchanged.

HD font checksums still cover every payload byte on each load. A read-only 1 KiB
CRC table replaces the bit-at-a-time calculation. SD validation batches reads in
an optional 4 KiB heap buffer, released before font metadata allocation. If that
allocation fails, validation continues with a 128-byte stack buffer. Corrupt and
short files are still rejected.

Returning to a cached EPUB layout also batches source-position range reads:
eight packed page records per transfer using 176 bytes of stack scratch. The
lookup still scans the whole chapter and rejects ambiguous ranges or read errors.
Reading positions, layout keys and cache formats are unchanged. A new size still
needs chapter reflow, so this does not eliminate all resizing latency.

Host checks cover repeated size cycles without additional flash writes, cached
size reuse, same-size file replacement, different-family installation, CRC
equivalence, corruption, allocation failure, and source-position ambiguity across
read batches. These verify mechanisms and correctness, not device timing.

For hardware validation, record the time from confirming a size until the same
passage is readable. Compare a cold size, a repeat size, and a return to the
original size in the same chapter; repeat near the chapter's end. Check that the
passage stays visible, inspect regular/bold/italic text, and compare subsequent
page turns at an SD-backed size. Repeat in portrait and landscape and monitor
serial heap/crash diagnostics when using an instrumented build. No cache deletion
or progress reset is needed.

Direct EPUB font-size actions, including the default pinch gestures, now show the
existing hourglass before saving the new size, loading fonts and rebuilding the
layout. The indicator uses an asynchronous refresh and follows Show Busy
Indicator and the existing measured-panel speed limit. A pinch at either size
limit does nothing. Menu selections keep their existing transition feedback.
If the displayed framebuffer is borrowed/unavailable or a clean refresh is
already pending, the extra overlay is skipped to avoid stale-page flashes or
consuming that refresh. Queued page previews and deferred text AA are canceled
before an acknowledged resize; the new layout replaces the hourglass.
