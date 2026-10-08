# Experimental page-turn antialiasing

This work lives on `fix/page-turn-pop-in`. The default branch, `master`, keeps
CSS, font coverage, search and reading-position fixes without the prepared-AA
experiments. `stable/without-pop-in` preserves the initial rollback point;
`backup/css9-before-pop-in-split` preserves the previously flashed source.

## Reader controls

Open the reader menu's Settings tab. This experimental branch defaults both
options to **On** and remembers an explicit **Off** across restarts:

- **Single-refresh AA (experimental)** changes how eligible text pages reach the
  panel. On uses the existing SDK Direct quality waveform; Off uses BW followed
  by the established grayscale overlay. The setting is offered on the tested
  X4 Pro UC8279/LUT68 hardware regardless of temporary framebuffer loans.
- **Pre-render next page (experimental)** controls advance page preparation.
  Turning it off still lets the current page capture its grayscale pixels for
  Direct. It does not disable background chapter layout or text search.

Neither switch changes layout or the reading position. Changing a switch clears
prepared page state before repainting the current page. All four combinations
are useful comparisons; next-page preparation is not required for Direct.

Text antialiasing must be enabled. Unsupported panels, inverted output, pages
with images, unavailable framebuffer storage, and preempted page turns keep the
existing compatible path. Explicit manual clearing also keeps its requested
refresh behavior. This experiment does not change panel voltages or LUT data.

## What one refresh does and does not mean

The current overlay path exposes a BW page before its gray edges arrive.
Preparing the next page removes repeated glyph work but cannot combine those
physical activations. Direct instead uploads two complete absolute planes and
issues one grayscale activation, without a preceding BW activation.

One activation can still include visible black/white phases. The SDK selects its
longer image-quality waveform, so a turn may flash more and take longer. Equal
logical coverage does not prove equal settled gray tones under different
waveforms. The first ordinary BW/menu refresh after Direct also uses the SDK's
additional recovery sequence. Optical quality, ghosting and perceived latency
must be compared on the actual device.

The previous page stays visible until the new complete page is ready. Advance
pre-rendering can reduce that preparation wait in either mode, at the cost of
background work and retained memory. Direct reuses the existing two 48,000-byte
PSRAM masks; it does not allocate another pair.

## Evidence from other firmware

The inspected readers do not establish a fast, nonflashing single-refresh text
solution for this exact panel:

- [XPoint's reading policy](https://github.com/Belphemur/XPoint/blob/53e49ca817805d9506ea4223d5e3ab2607de0274/src/activities/reader/EpubReaderActivity.cpp#L3859)
  retains sparse overlay AA for text and reserves Direct for images/sleep.
- [CrossInk issue 683](https://github.com/uxjulia/CrossInk/issues/683) records delayed
  AA on UC8279/LUT68 using a 120 fps video. Its
  [reader path](https://github.com/uxjulia/CrossInk/blob/9914146eeae7b46b300f475a16c32426fc02ec1f/src/activities/reader/EpubReaderActivity.cpp#L7450)
  still uses a base refresh followed by AA.
- [CrossPoint issue 3548](https://github.com/crosspoint-reader/crosspoint-reader/issues/3548)
  reports a quality-bank regression from 228 ms to 987 ms of gray BUSY time,
  with visible flashing. Both compared logs still include a 559 ms base pass:
  this is evidence about the waveform, not a Direct-only total-turn benchmark.
  [The tester confirmed](https://github.com/crosspoint-reader/crosspoint-reader/issues/3548#issuecomment-5669606187)
  that restoring text AA fixed reading while Direct worked for sleep images.
- [Witchhunt's newer combined path](https://github.com/jpirnay/witchhunt-reader/blob/45652a0dc314ec61d4c98084ce4d37d8e5f7a4dc/src/activities/reader/EpubReaderActivity.cpp#L5281)
  identifies T5S3 as the supported panel; that capability is not interchangeable
  with X4 Pro Direct.

These findings justify a reversible comparison, not a claim of a proven optical
improvement. Keep this work separate from master for at most one or two device
iterations before deciding whether it is worth retaining.

## Verification

`test/real_renderer/run.py` checks production glyph masks and the renderer/HAL
handoff. `driver_audit.py` runs those selectors through the actual pinned UC8279
source, including gate offset and byte inversion, and counts activations.
`direct_hal_audit.py` compiles the production HAL methods together with the
complete SDK facade and drivers against the SDK's recording bus. It exercises
successive Direct frames, storage loans, unsupported configurations, inversion,
BW/menu recovery and sleep transitions. Host timings do not model panel latency.

On the device, compare the same font, size and darkness with each menu
combination. Check the first page, repeated forward/back turns, menu return,
font resizing and an illustration-to-text transition. Record both time until
the page first appears and time until it settles, plus the final font weight
and any residual previous-page marks. The synthetic EPUB's Small font strokes
chapter provides original text for the appearance comparison.
