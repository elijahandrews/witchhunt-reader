# Picco latency investigation

## Evidence (2026-11-09)

The owner reported delay before both page turns and menus changed. A serial
capture from the identified Picco reproduced the complaint on the installed
firmware. The panel identifies as SE0400NQW47 (SSD2677 path).

A fast black-and-white refresh took 346 ms, with about 90 ms power-on and
140 ms power-off. Text antialiasing then used a separate 117–118 ms overlay.
Between the completed black-and-white shutdown and the start of the overlay's
power-on, repeated reading updates spent approximately 1.1–1.8 seconds outside
those panel waits. This identifies substantial intervening work, but the
installed firmware does not log touch recognition or render-start timestamps;
it cannot assign the entire delay before the first visible change to one cause.
Private logs remain in the primary checkout's ignored device workspace.

## Changes

- `InputWake` now listens to the Picco TMA525C's host-owned data-ready interrupt,
  as it already did for GT911. This removes the wait for the next idle poll
  (up to 50 ms), including when the physical-button capture task is active.
  The ISR only signals existing static semaphores; decoding stays on the main
  task. Attachment failure retains timed polling. Other touch drivers keep
  their existing interrupt ownership.
- The scalable-font metric and pixel tables now use 2048 entries each, with
  four candidates per hashed set and bounded round-robin replacement. A glyph's
  identity still includes face/size ID, codepoint or shaped glyph ID, point size,
  fractional phase and scale. Rasterization and pixel quantization are unchanged.
  The existing 512 KiB pixel arena is unchanged. Table growth and replacement
  bytes add 74,752 bytes (73 KiB) to the existing persistent PSRAM allocation on
  ESP32-S3; no allocation occurs per lookup. The existing fallible allocation
  path remains in place. C3 firmware does not enable scalable fonts.

A host experiment using original synthetic prose, built-in Lexend at 11 pt,
character spacing 5 and word-local fractional positions drew 698 glyphs in
three consecutive passes:

| Cache | Metric misses per pass | Raster misses per pass |
| --- | --- | --- |
| Previous 512-entry direct mapping | 234 / 68 / 68 | 256 / 60 / 60 |
| 2048-entry four-way round-robin | 203 / 0 / 0 | 234 / 0 / 0 |

The resulting 480×800 coverage buffers were identical. These counts establish
less repeated font work for that workload, not a measured Picco speedup or a
promise of zero misses for every page. Ordinary bitmap settings labels do not
use the scalable-font cache.

## Validation

The input-wake regression fails against the previous code and passes with this
change, including captured/uncaptured input and interrupt attachment failure.
The SDK's Picco touch-protocol tests also pass. The integrated font regression
compares each page pixel with independent FreeType output and requires stable
cached pixel pointers on successive warm passes; the previous cache fails that
retention check. Adapter, synthetic caps/scales, variable-font and generic-font
tests pass under ASan/UBSan. Independent reviews of both changes are clean.

Both PlatformIO builds pass: Picco's application image is 6,009,504 bytes, with
122,612 bytes of static RAM; the C3 application image is 6,526,688 bytes, with
74,520 bytes of static RAM. These are build results, not installed-firmware or
hardware-performance results.

## Hardware verification

After an explicitly requested installation with a fresh device backup:

1. Wait several seconds, then tap a settings tab and swipe through reader menus.
   The first touch should be recognized without waiting for the idle poll.
2. Turn several ordinary text pages of the same book with the same font,
   spacing and antialiasing settings. Compare the delay before the first update
   and the black-and-white-to-gray interval against the captured baseline.
3. Check fractional glyph edges, mixed sizes/styles and publisher fonts for
   unchanged appearance; test touch after sleep and wake and while plugged in.

No EPUB cache reset is required: neither font output nor layout/cache formats
changed. Hardware confirmation is still required before calling the overall
sluggishness resolved. A separate missing-glyph fallback issue found during the
audit is not changed here.
