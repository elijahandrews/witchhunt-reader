# Single-pass text AA on X4 Pro

The text-AA implementation shipped locally as `2.40-singlepass2-x4pro` on 2026-11-08.
The user confirmed that the visible settling is gone. The preserved
`2.40-fastpages2-x4pro` image remains the rollback target. The application-only
installation preserved the bootloader, partition layout, settings and reading progress.
Measured page-turn latency and long-run ghosting remain unverified.

## Why this differs from earlier attempts

The earlier buffering experiment still performs a B/W activation followed by an
AA activation. Its CPU work can overlap the first waveform, but it cannot remove
that second visible settling phase. The October Direct experiment used a slow
image waveform. This implementation uses a dedicated **text** waveform and charge
history to present the complete page in one activation.

This is a selective port of `fix-display-updates` from:

- [Free-Ink/freeink-sdk, e6745c137f1dbe7aa0995184ce346030c900cea2](https://github.com/Free-Ink/freeink-sdk/tree/e6745c137f1dbe7aa0995184ce346030c900cea2).
- [crosspoint-reader/crosspoint-reader, 4209987985b99d0ec2e023b3ed993f5e6caca2c0](https://github.com/crosspoint-reader/crosspoint-reader/tree/4209987985b99d0ec2e023b3ed993f5e6caca2c0).

The SDK's `X4ProTextAa` selector/charge algorithm and the 245-byte
`kX4ProTextUc8279Zhx` bank are the ported pieces. Witch Hunt keeps its SDK fork,
image implementation and reader scheduling. The Picco's SSD2677 is a different
panel and does not run this waveform. No host test proves matching Picco speed.

## Eligibility and fallback

The new path requires Xteink X4 Pro, UC8279 **LUT68**, native 800×480 geometry,
PSRAM, non-inverted output, both framebuffers, text AA and **Fast AA** enabled,
and an EPUB page without images. It only runs after an established baseline and
for an effective FAST refresh. Other variants, images, periodic cleaning, manual
HALF/FULL overrides, missing memory and canceled AA retain the established path.

**Reader → Font → Fast AA** selects the single-pass text path. Turn it off to compare with
the older two-pass approach while keeping ordinary text AA enabled. The
existing `fastAntiAliasingV2` setting is used without a settings migration.
The new text waveform has black, **one gray**, and white; both previous AA gray
levels map to that one gray. It may change apparent edge weight. Image gray levels
are unchanged.

Fresh rendering captures AA masks during the existing glyph pass, including
status-bar and truncated-section chrome. Pre-rendering retains host masks beside
the exact BW generation; page/spine/generation identity and complete capture are
checked before use. Live chrome also updates these masks so white erasures cannot
reappear as gray fringes. No SPI upload happens during this text pre-render.

The explicit synchronous `displayTextAaOrBw` operation consumes a pending refresh
override once. It either submits the complete AA target or displays that same new
BW target. Only a successful AA submission swaps its host framebuffers and commits
charge history. The reader then skips the old AA overlay. A failed submission
retains the new target for the BW fallback; the normal overlay may run afterward.
Navigation is checked before submission, including after cached live chrome.
Once submission begins it is completed synchronously.

## Controller and memory details

Text selectors are `(high << 1) | low`: 00 black, 01 gray, 10 white hold, 11 white
clear. Unlike the existing image path, these planes are **not inverted** and the
text LUT registers stay in sequential order 0x20…0x24. The controller still gets
120 white padding rows followed by 480 visible rows. Warm text turns upload DTM1
and DTM2 and issue one DRF; they do not upload a B/W base or restore another plane
afterward. SPI clock and panel power/VCOM voltage registers are unchanged.

The upstream model applies doses +15, −2, −2, −20, median recentering, floor-rounded
998/1024 decay, and clipping to ±127. Charge history is committed only after BUSY
completion and invalidated on legacy physical operations or resynchronization.
An independent upstream comparison verifies the algorithm, not physical charge.

The workspace uses **529,024 bytes of PSRAM**: two 48,000-byte output selector
planes, 48,000 bytes of previous ink, 384,000 signed charge bytes and a 1,024-byte
histogram. It borrows immutable BW/mask inputs for the synchronous call, saving
48,000 bytes relative to upstream's retained base copy. The histogram moved from
the task stack to PSRAM. Allocation is lazy and reused, with a 128 KiB free-space
reserve and a largest-contiguous-block check; no internal-heap fallback. Reader
exit, framebuffer release or stage-1 page-load memory recovery frees it. Exit
keeps the mask pair until any cached page restoration is complete, avoiding a
free-and-reallocate cycle. The existing reusable mask pair adds 96,000
bytes, for **625,024 bytes of additional retained PSRAM** while both are allocated.

The text sequence changes undocumented register 0x2A. Its default is unknown.
A transition to the old display paths therefore powers off, resets the controller,
reinitializes its ordinary configuration and requires a clean optical/DTM baseline.
This conservative transition can flash or take longer when opening a menu, showing
an image or reaching a periodic clean. Consecutive text pages avoid the reset.
The hardware-configuration flag survives history invalidation and memory release.

UC BUSY waits retain the existing indefinite-wait behavior. The simulated
failed-completion test exercises a defensive reset branch; it does **not** prove
recovery from a physically stuck BUSY line. Controller reset restoration, gray
density, actual latency, ghosting and long-run charge behavior need physical tests.

## Verification

The release manifest under the ignored `.local-device/` directory records exact
source overlays, firmware/web hashes, build results and independent review results.
The new coverage includes:

- An 80-frame per-pixel selector/charge oracle with stable white areas, randomized
  ink, reset history, median/rounding checks and allocation failures.
- Independent comparison against pinned upstream across 100 full frames under
  address/undefined-behavior sanitizers; identical LUT68 bytes.
- Recording-bus tests for one DRF, plane polarity/padding, LUT ordering, no voltage
  writes, warm reuse, all major legacy exits, unsupported variants, failed
  completion and exact framebuffer ownership in dual/single-buffer builds.
- The unchanged 180-case legacy command fingerprint `004943a3dc8debeb`.
- Production renderer tests for captured versus staged masks across orientations,
  darkness and styles, plus one-pass/fallback swaps and refresh override routing.
- 213 checks using extracted production cached-page/screenshot functions, including
  generation/page/spine rejection, incomplete captures, live chrome, late navigation,
  failed AA and cadence. Removing either the generation guard or late-navigation
  check makes its negative control fail.
- Rapid consecutive navigation rejects stale cached-page handoffs, including
  forward/backward jumps before the next render. Cleanup waits for pending display
  work before reseeding controller RAM. Bounded-memory tests cover exit restoration
  and page-load recovery.
- Full host tests, the production renderer-to-driver wire audit, configured static
  analysis and a clean X4 Pro firmware build.

The complete EPUB activity is not a host test fixture; extracted production functions
cover the cached display and screenshot paths. The remaining generation/invalidation
call sites were independently reviewed. Device tests must exercise fresh and cached
turns, rapid navigation, cache misses, screenshots and overlays.

## Device comparison

Use a new application build only after the normal identity, live partition, scoped
state-backup and rollback checks in [Flashing X4 Pro](flashing-x4pro.md). The matching
web bundle is unchanged from fastpages2, so verified matching assets need no rewrite.
No dictionary or book copy is needed.

1. Keep the same book, font, size, darkness and lighting. Confirm the candidate
   version in Settings. Keep text AA on and enable Fast AA.
2. After the initial clean page, turn through plain-text pages. `CMD:STATE` reports
   `textAA=one-pass` only when the last displayed reader page used the new path;
   `textAA=legacy` means it did not. This diagnostic remains in compact-log builds.
   The command also includes private reading state; keep its captures local.
3. Compare cached forward turns and uncached/backward turns. Record button-to-final
   page timing with slow-motion video; software screenshots cannot show settling.
4. Turn Fast AA off and repeat on the same text. This preserves AA but selects the
   older two-pass path. Compare edge weight and ghosting as well as speed.
5. Exercise an image page, menu return, periodic clean, manual refresh, rapid turns,
   screenshot/overlay, inversion, sleep/wake, and at least 100 ordinary pages.
   Check that text remains dark and the white background clean.

If it is not a clear improvement, restore the known application and wait a week for
upstream progress. A weekly read-only branch watch is also recorded in the local task.
Do not tune voltages, disable cleaning, or change the panel variant to chase speed.
