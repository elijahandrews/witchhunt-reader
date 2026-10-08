The dark-mode audit compiles the production HAL inversion methods, complete FreeInk display facade, and UC8279 driver unchanged against the SDK's recording bus:

```sh
python3 test/dark_mode/run.py . --out /tmp/witch-dark-mode-hal --sanitize
python3 test/real_renderer/run.py . --out /tmp/witch-dark-mode-renderer --sanitize
```

The HAL audit checks logical framebuffer preservation, inverted wire pixels, repeated page turns, pending refresh completion, secondary-buffer loans, power-off, rejected gray passes, and restored AA capability. It runs with dual and single buffers and with/without PSRAM build flags. The renderer audit checks real text and icon pixels in four orientations, full refreshes on polarity changes, ordinary refreshes afterward, and window/power behavior. It saves synthetic light/dark page images.

These host checks validate pixels and controller transactions. Optical ghosting and readability still require the physical panel.
