# Generic families and letter spacing

This branch adds CSS generic `serif`/`sans-serif` fallback, including inherited inline runs, alongside computed signed `letter-spacing` in em/rem/px/pt. Unknown named fonts and unsupported generic families remain unsupported; no embedded font files are loaded. The selected SD face serves as the serif preference, with a matched built-in sans face for contrast.

A word carries one compact family/spacing annotation through layout, hyphenation, bionic splitting and table retries. Uniform lines store one scalar; mixed lines store an optional aligned annotation array in the existing arena. Tracking is computed at the declaration and inherited as an absolute length. The same glyph walk measures and draws text, disables optional ligatures under nonzero tracking, preserves combining marks and uses the rasterizer's bearing convention. Table cells persist their actual line advance. Prewarming remains a scan without glyph-width measurement and respects the four built-in cache slots.

Synthetic EPUB cases cover family switches, bold/italic inline spans, nested inheritance, table cells, positive/negative tracking, resets, different-size inherited tracking, joined runs and long words. Mixed-family floated drop caps fall back to enlarged inline runs so distinct quote/letter formatting is preserved; inline size retains the renderer's existing 250% cap.

Float exclusion now uses exact line indices from the greedy breaker for paragraphs containing floats. A single pass computes resolved run-height bounds: uniform families get exact geometry, while mixed-family lines use conservative cumulative bounds so text cannot enter an image. A mixed paragraph may remain narrow slightly longer than an exact line-by-line browser layout. Paragraphs without floats retain the existing dynamic-programming breaker.

Blank separators capture their own resolved family height and CSS spacing when created, so the next element's family cannot change the pending gap. Reviewed case-transform, expanding Unicode, and streaming boundary fixes are integrated. Independent combined review found no remaining actionable defects.

The local production renderer harness in `test/real_renderer` uses the actual font decoder and glyph drawing code. It checks all four faces of both built-in families, small caps, scaling, and signed tracking, and exports a PNG. The optional local `.cpfont` fixture also checks SD font bitmaps and grayscale-plane output. Reduced glyphs use area coverage so 75% uppercase text and small caps retain thin strokes. Hardware, SD-font loading, refresh waveforms, and complete UI behavior are outside that harness.

Existing golden changes retain identical word sequences; newly honored family annotations and consistent scaled word-gap measurement account for the differences. The extended typography fixture adds new synthetic content.
