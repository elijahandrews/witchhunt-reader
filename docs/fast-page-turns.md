# Fast page turns investigation

The later [single-pass text AA implementation](single-pass-text-aa.md) was installed as
`2.40-singlepass2-x4pro` on 2026-11-08; the user confirmed that settling is gone.
The sections below preserve the earlier buffering-only investigation and its limitations.

Branch `fast-page-turns`, previous candidate `2.40-fastpages2-x4pro`, installed on the user’s X4 Pro
on 2026-11-08. Source
starts at Witch Hunt `9728b989` and SDK `e140f748`, with an uncommitted SDK patch as
well as reader changes. Both diffs are required to reproduce this candidate.

## Findings from the Picco comparison

The two-buffer helper was not the reason the Picco looked smoother. At the installed
CrossInk revision `b8feaec4d10ecb484ed80e326f01f2a3ea949331`, SDK
`684ea6073f209825bacf81459902298e9cf139b4` enables `supportsDeferredGrayscaleBase()`
only for **UC8179**. Neither the Picco's **SSD2677/SE0400NQW47** nor this X4 Pro's
**UC8279/LUT68** uses that helper. Board name alone cannot identify the panel.

The Picco driver runs a blocking B/W base, then a separate overlay activation using
its stock slot-5 grayscale table. A host audit of the exact driver confirms two
activations and a packed 96,000-byte upload. Its smooth appearance therefore does
not establish that it performs a single physical refresh. Default built-in Bitter
uses grayscale rasterization and AA; the monochrome SD-font shortcut does not explain
this user's reported setup.

The X4 Pro has different panel waveforms and uploads two padded 60,000-byte planes.
[CrossInk issue 683](https://github.com/uxjulia/CrossInk/issues/683) reports the same
visible two-stage effect on UC8279/LUT68. This is evidence against treating smooth
Picco output as a firmware-only difference. Current Picco AA timings and optical
settling have not been measured, so the physical contribution remains unquantified.

## Transferable software change

There is also a real API difference. CrossInk calls `displayGrayscaleBase(FAST)`:
on a warm UC8279 this selects the driver's existing stock `prebw_mid` transition.
Witch Hunt's fresh-page `triggerDisplayAsync()` instead calls `displayStart()` directly,
which bypasses that choice and performs an ordinary OTP B/W refresh, with an inverted
target re-drive after AA. The previously blocking cached-page path already selected
the stock AA-aware transition.

This candidate splits the **existing UC8279 AA-aware base** into start and finish,
following CrossInk's UC8179 structure. It keeps the existing command order, LUT bytes,
voltage data, baseline restoration and power policy. Consecutive AA cached turns retain their prior
base sequence; eligible fresh turns select the AA-aware sequence too. After an
intervening B/W frame or cancelled AA, the explicit API may also select this
transition where the old cached dispatch selected ordinary DU. Initial,
full/half, unsupported, inverted and missing-secondary-buffer cases keep safe fallbacks.
The facade swaps exactly once and retains the submitted B/W page until completion.

While that base waveform runs, the reader renders both AA masks into one reusable
96,000-byte PSRAM allocation. Allocation reserves 128 KiB for other work and never
falls back to internal RAM. The old LSB-only path remains available when the new
capability or storage is unavailable. Images and single-push/deferred panels keep
their existing rendering. Activity exit frees the scratch storage.

The masks are uploaded only after the base completes. Navigation is checked during
rendering and after each upload; cancellation restores the B/W baseline without a
gray activation. Rendered AA masks and the separate AA waveform are unchanged. Physical settling
can still differ because the base history changes.
This is a software improvement candidate, not a demonstrated match for Picco speed.

## What happened in the earlier experiment

The earlier `fix/page-turn-pop-in` branch captured both masks during B/W rendering
and cached them for the next page, using the same 96,000-byte budget. It removed more
repeated CPU work than this simpler overlap path. Saved device logs prove cache hits;
there is no recorded optical verdict establishing that the implementation failed.

For two cached turns, the logs record about 129–134 ms for upload, 230–231 ms of actual
AA BUSY time, and further controller RAM restoration. The full post-base software
work was about 530 ms. RAM restoration does not activate the glass, so that is not a
530 ms optical-delay measurement. Upload plus AA BUSY was roughly 360 ms even with
the masks already prepared; buffering cannot remove those stages.

The October `2.38-aa1` experiment (`e7571891`) also offered Direct mode. It used the
SDK's long image-quality waveform: one recorded fresh turn took 1,224 ms, including
991 ms in the display waveform. One activation did not mean a quick, nonflashing
transition. Neither its final appearance nor a cached Direct turn was recorded.

Newer upstream one-pass **text** AA is a different design, with a charge-history map
and dedicated text waveforms. It is not the Direct image mode tried in October, nor
the path running on this Picco. Reports show promise on UC8279 but also image tone
regressions and power-state concerns. Porting that waveform stack would be a separate
experiment with different gray-level and memory tradeoffs; see the newer one-pass candidate linked above.

## Verification and device comparison

The production-renderer harness compares all plane pixels with the established path
across four orientations, five darkness settings and four font styles under ASan/UBSan.
It checks render/upload ordering, cancellation, restored renderer pointers and B/W
content. Upload-cancellation regressions failed before the review fix. Allocation
tests cover reuse, fragmentation, failure, invalid sizes and non-PSRAM builds.

The SDK recording-bus tests compare blocking and split output across 180 scenarios:
three supported variants, initial/warm/after-AA/forced-clear/after-Direct state,
FAST/HALF/FULL, power-off policy, and completion/cancellation followed by ordinary B/W.
The same fixture is run against the untouched pinned SDK to detect shared refactoring
errors. Facade tests poison the write buffer after submission, then finish via wait,
buffer borrowing/release, inversion or another refresh, verifying the original B/W
frame supplies the baseline. Unsupported and single-buffer modes fall back safely.

For a later authorized device comparison, use the same synthetic EPUB page, font,
size and darkness on the installed baseline and this exact candidate. Compare a
fresh/backward turn, a cached forward turn after idle, rapid navigation, periodic
cleanup, images and menu return. Record first visible text and fully settled text
separately from input readiness. `grayscaleBaseAsync`, `Inline AA buffered=1`, and
`Cached AA buffered=1` logs identify the new path. Existing panel BUSY logs separate
the base and AA waveform durations from CPU work. Physical latency, ghosting and
settled tones remain unverified until that comparison.

## First device result (2026-11-08)

The application-only flash passed independent verification, protected-flash comparison,
and boot/version checks. The user reports noticeable settling remains; after comparison,
it may be a hair faster but is largely the same. No camera timings were recorded.
This is not a successful elimination of the two-stage appearance. It is consistent
with the existing second grayscale activation and its upload/waveform cost remaining.

The next investigation must distinguish a missed eligibility gate from the inherent
cost of two activations. The installed compact log profile cannot establish from the
boot capture whether each tested page used buffered AA. Upstream one-pass text AA
remains a separate waveform/charge-history experiment, with power-state/fallback risks
to audit before any port or hardware installation. Do not claim a single-pass text
implementation was tested in this release.

### Follow-up source review

The user’s supplied AA/font settings do not disable ordinary FAST-page overlap.
A second independent review found no large artificial BUSY sleep. The panel still
requires base completion, AA upload and a separate grayscale activation.

After the base waveform, the current driver restores DTM1, then overwrites DTM1 with
the first AA plane and uploads DTM2 with the second: three padded 60,000-byte transfers.
At the board’s configured 10 MHz this is a calculated 144 ms of wire time before the
AA activation, excluding overhead and the AA waveform itself. Cached pages also
regenerate both masks and their opaque replay; work exceeding the base interval remains
additive. Three post-AA RAM writes delay completion/input readiness but do not activate
the panel again.

Deferring the immediately overwritten DTM1 restore is a concrete smaller optimization
candidate (one 60,000-byte transfer), but cancellation and non-AA completion still need
a valid baseline. It has not been implemented or hardware-tested. Eliminating the second
activation requires a different one-pass text waveform, with a separate review of charge
history, power-state restoration, image fallback and allocation failures.

## Pinned code references

- [CrossInk helper](https://github.com/uxjulia/CrossInk/blob/b8feaec4d10ecb484ed80e326f01f2a3ea949331/src/activities/reader/DeferredGrayscale.h)
- [Picco panel driver](https://github.com/jpirnay/freeink-sdk/blob/684ea6073f209825bacf81459902298e9cf139b4/libs/display/FreeInkDisplay/src/driver/Ssd2677Driver.cpp)
- [UC8179 capability](https://github.com/jpirnay/freeink-sdk/blob/684ea6073f209825bacf81459902298e9cf139b4/libs/display/FreeInkDisplay/src/driver/Uc8179Driver.h)
- [Built-in scalable fonts](https://github.com/uxjulia/CrossInk/blob/b8feaec4d10ecb484ed80e326f01f2a3ea949331/lib/ScalableFont/ScalableBuiltins.cpp)
- [Newer one-pass text report](https://github.com/crosspoint-reader/crosspoint-reader/issues/4344)
