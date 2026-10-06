# Generic families and letter spacing: implementation checkpoint

This branch adds CSS generic `serif`/`sans-serif` fallback, including inherited inline runs, alongside computed signed `letter-spacing` in em/rem/px/pt. Unknown named fonts and unsupported generic families remain unsupported; no embedded font files are loaded. The selected SD face serves as the serif preference, with a matched built-in sans face for contrast.

A word carries one compact family/spacing annotation through layout, hyphenation, bionic splitting and table retries. Uniform lines store one scalar; mixed lines store an optional aligned annotation array in the existing arena. Tracking is computed at the declaration and inherited as an absolute length. The same glyph walk measures and draws text, disables optional ligatures under nonzero tracking, preserves combining marks and uses the rasterizer's bearing convention. Table cells persist their actual line advance. Prewarming remains a scan without glyph-width measurement and respects the four built-in cache slots.

Synthetic EPUB cases cover family switches, bold/italic inline spans, nested inheritance, table cells, positive/negative tracking, resets, different-size inherited tracking, joined runs and long words. Mixed-family floated drop caps fall back to enlarged inline runs so distinct quote/letter formatting is preserved; inline size retains the renderer's existing 250% cap.

Remaining review items before this build is ready to flash:

- Float exclusion prediction still assumes a fixed base-font line height. Actual mixed family metrics can differ, so prediction must use compatible line-height bounds before enabling this build on the device.
- Empty-line spacing must resolve the active family when integrating the separately reviewed consecutive-break fix.
- This checkpoint is based on the initial casing/line-height patch. Integrate the later reviewed casing and line-height fixes, preserve both sets of synthetic fixture chapters, then repeat full independent review and firmware compilation.

Existing golden changes retain identical word sequences; newly honored family annotations and consistent scaled word-gap measurement account for the differences. The extended typography fixture adds new synthetic content.
