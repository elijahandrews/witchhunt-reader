The dark-mode audit compiles the production HAL inversion methods, complete FreeInk display facade, and UC8279 driver unchanged against the SDK's recording bus:

```sh
python3 test/dark_mode/run.py . --out /tmp/witch-dark-mode-hal --sanitize
python3 test/real_renderer/run.py . --out /tmp/witch-dark-mode-renderer --sanitize
```

The HAL audit checks logical framebuffer preservation, inverted wire pixels, repeated page turns, pending refresh completion, secondary-buffer loans, power-off, and restored AA capability. It runs with dual and single buffers and with/without PSRAM build flags. A sparse four-coverage pattern passes through the actual facade and driver in both polarities. The audit verifies the wire selectors, native gray LUT, unchanged caller-owned snapshots, controller baseline, and the next page. Missing, repeated, null, reordered, custom-LUT and factory-mode dark AA passes must not activate a refresh. A polarity change must cancel an incomplete pass.

The renderer audit checks real text and icon pixels in four orientations, full refreshes on polarity changes, ordinary refreshes afterward, and window/power behavior. Its synthetic light/dark images model logical pixel coverage; the native LUT68 bank has identical drive sequences for its two middle selectors, so these images are not a claim of four distinct physical shades.

Inverted AA is enabled only for the X4 Pro UC8279/LUT68 configuration with the existing baseline buffer available. Other panels retain the monochrome dark-mode fallback. The feature uses native two-stage text AA, with complemented selectors and reversed gray-edge source drive; it does not enable the separate Direct grayscale experiment. Pulse durations, voltage settings, VCOM, and endpoint waveforms stay unchanged. See the SDK's inverted-AA documentation and host tests for the waveform derivation and capability guards.

These host checks validate pixels and controller transactions. Optical ghosting and readability still require the physical panel.
