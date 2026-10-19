# Production renderer to UC8279 X4 Pro driver regression

The normal `run.py` renderer smoke loop also runs this test. To run it separately:

```sh
python3 test/real_renderer/driver_audit.py /absolute/path/to/witch-hunt --out /tmp/witch-driver-audit
```

The first binary compiles the production renderer and actual compressed Bookerly 14 glyphs. It draws staged and captured grayscale masks at 0.75, 1, and 1.2 scale for all five darkness settings, checks both paths agree, and checks cold and warm scaled caches agree. A synthetic four-stripe glyph independently covers raw values 0, 1, 2, and 3. Each stripe has a constant 4×4 interior, sampled away from boundaries after scaling. Sixty cases check expected coverage-to-tone behavior, the documented overlay code, and recovered BW base. Fixture files carry the actual BW, LSB, and MSB bytes from the renderer.

The second binary compiles the actual `Uc8279X4Driver.cpp` unchanged with a recording `EpdBus`. It feeds all 60 production-renderer staged and captured fixtures into that driver. Every visible wire byte must match the expected bitwise-inverted absolute selector. Both uploads must contain 120 leading white gate rows, giving the panel's full 600-gate stream. The actual sparse AA LUT68 bank must be selected. After the grayscale activation, both controller planes must match the original BW base byte-for-byte, including the gate padding. The driver runs on an X4 Pro profile with controller variant 0x68.

The independently specified tone oracle preserves all four source levels for Normal, pushes dark fringe to black for Dark, pushes both fringes to black for Extra Dark and Maximum, and maps both fringes to light for Lighter. The SDK overlay contract is `(LSB, MSB)`: black/white=00, light=01, dark=11. UC8279X4 and UC8179 fold masks into absolute selectors as `plane0 = base | lsb`, `plane1 = plane0 ^ msb`, then recover `base = plane0 & plane1`. The test exercises the actual driver implementations of these operations and their transport, rather than only testing those equations.

The SDK API boundary agrees across the other reviewed drivers: UC8179 uses the same explicit fold; SSD1677 and the X3 UC8253/UC8279 drivers advertise overlay masks and send them to their panel LUTs; LgfxEpdDriver decodes 01 as light and 11 as dark; PaperMono merges both masks into its single available gray tone. Their physical LUT outputs are outside this regression.

Driver copies remain byte-identical to the selected SDK. A copied renderer header changes only its device-specific cache size assertion to accommodate host pointers. GPIO, BUSY waits, memory allocation, and SPI transport are host shims. This does not simulate physical e-ink response, voltage, temperature, contrast, ghosting, or optical calibration. Maximum is tested for glyph coverage and selectors; whether a caller skips the grayscale waveform is a separate reader integration decision.

`driver-build-commands.json` records exact compile commands and source paths. Binary fixtures and executables remain in the output directory; no book files or fonts are copied into the repository.
