# Screenshot capture regression

Run from the repository root:

```sh
python3 test/serial_screenshot/run.py --out /tmp/witch-screenshot --sanitize
python3 test/serial_screenshot/run.py --out /tmp/witch-screenshot-old-order --sanitize --negative-control old-order
python3 test/serial_screenshot/run.py --out /tmp/witch-screenshot-repeated --sanitize --negative-control conditional-hook
```

The harness extracts and compiles the production screenshot service, reader capture hook, page-content helper and status-bar renderer. It also uses the SDK's actual secondary-buffer sync/borrow/return functions. Small host shims supply page loads, font-prewarm I/O, image policy observations and framebuffer bytes.

The checks cover repeated captures when a returned secondary buffer contains the next page, absent/borrowed secondary buffers, completed and active section builds, placeholder policies, status chrome without changing last-displayed metadata, BW-only capture without gray-plane writes, failed page loads, null transfer-mode framebuffers, non-reader screens, pending refreshes and render locks. Positive controls retain ordinary pre-render staging and status bookkeeping. Both negative controls must fail at the current-page-pixel assertion; the conditional-repair control retains chrome and fails on the second capture.

This is a control-flow and ownership regression, not an image-decoder, font-rasterizer or physical e-ink test. The card destination is stubbed after capture preparation, so its existing BMP writer and confirmation-border refresh are outside the harness. Device verification should capture a freshly opened synthetic page twice before any page turn, then capture next/back and compare the page content with `STATE`.
