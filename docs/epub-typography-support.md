# EPUB typography in this fork

Enable the reader’s **embedded styles** setting to use publisher typography.
The selected reading font still supplies ordinary text when the EPUB does not
request an available embedded family. Books do not need to be converted to use
plain embedded OpenType fonts on the X4 Pro.

## Implemented

- Plain embedded TrueType, CFF OpenType and variable CFF2 fonts, loaded from local
  EPUB resources. Regular, bold, italic and bold italic use the declared faces.
  Ordered family/source fallbacks skip unavailable or unsupported fonts.
- Authored `smcp` and `c2sc` glyphs for `small-caps` and `all-small-caps`, including
  their kerning. Missing features use the reader’s synthetic small-cap fallback.
  Ordinary ligatures cannot hide lowercase letters before caps substitution.
- Variable `opsz` at the actual CSS display size, with `wght` and `ital` selection.
  Rasterization uses two source pixels per logical pixel; raster density does
  not accidentally double the optical-size coordinate. Fractional advances
  and coverage resampling retain authored spacing and thin strokes.
- Named families and `@font-face` in chapter-local head styles as well as external
  stylesheets. Chapter-local selectors remain scoped to that chapter; their
  specificity participates in the existing cascade.
- A bundled, licensed four-face monospace fallback for `monospace`, `pre`,
  `code`, `kbd` and `samp`. Preformatted whitespace preserves indentation,
  explicit newlines and eight-column tabs. `pre-wrap` can wrap at spaces without
  inserting prose hyphens into code identifiers.
- MathML `altimg` fallback through the existing image renderer. Without a usable
  fallback image, descendant text remains available instead of disappearing.

The runtime font engine uses PSRAM and a shared 3 MiB payload budget with bounded
least-recently-used eviction. Font extraction and allocation failures mark affected
pagination as degraded so the existing bounded retry policy can rebuild it.
Extracted font caches are checked against their ZIP entry CRC and repaired when
possible. Font contents participate in the pagination cache key. Devices without PSRAM use
existing fallback fonts.

## Page margins

**Settings → Reader → Spacing → Ignore publisher page margins** removes margin
and padding on EPUB `html` and `body` elements, including inline styles. It is
off by default. Enable it with **Embedded Style** still on to use the reader’s
**Reader Screen Margin** without adding the publisher’s outer page spacing.
The device’s bezel clearance and **Screen Edge Margin** still apply.

Paragraph indents, heading spacing, quotations, lists, and other nested block
spacing remain intact. A publisher’s margins on a wrapper `div` are retained;
this option does not guess which nested blocks represent an entire page. It
does not change TXT/Markdown layout. The reader builds a separate pagination
cache for the new layout as needed.

For one book, open the **reader menu → Settings → Ignore publisher page margins**
(also available in **Quick Overrides**). Choose **On**, **Off**, or **Default** to
follow the global setting. Returning to the book applies the change immediately
through the reader’s position-preserving reflow; reopening retains the choice.

**Extra Paragraph Spacing** now defaults to **Off**. This removes the reader’s
optional extra half-line gap while preserving publisher paragraph margins.
An existing saved preference is retained; it can be changed under
**Settings → Reader → Spacing**.

## SD fonts

Existing v4/v5 fonts remain supported. The converter’s `--small-caps` option writes
v6 fonts with true caps alternates and kerning; it requires `--raster-density 2`.
`--optical-size auto` instances an optical axis at each requested point size.
`--instance-styles` selects variable weight/italic instances; explicit `--axes`
and per-style overrides are available. Default conversion preserves the previous
format and behavior. See [the format specification](cpfont-format.md).

## Remaining limits

This is a bounded reader engine, not a complete browser EPUB renderer:

- Font fallback selects a usable family; it does not yet search the family stack
  separately for every missing character. The bundled monospace subset covers
  Latin-1 and common punctuation, not every script.
- Only plain local font resources are supported. Encrypted/obfuscated fonts,
  WOFF/WOFF2, remote fonts and arbitrary font containers are not implemented.
- Complex-script shaping, contextual substitutions, mark positioning, arbitrary
  OpenType feature settings and variable GPOS/GSUB deltas remain unsupported.
  Offline static instancing can bake variation changes for converted fonts.
- Runtime optical sizing is automatic; CSS `font-optical-sizing: none` and
  arbitrary `font-variation-settings` are not implemented. Numeric CSS weights
  still resolve through the reader’s normal/bold style model.
- CSS support retains the existing selector and layout limits, including complex
  selectors/pseudo-elements, row-spanning table cells, general vector SVG and
  native MathML typesetting. Hard preformatted line breaks inside table cells
  are not yet supported.
- Embedded fonts and local styles have explicit resource limits. Very large or
  malformed resources can fall back; firmware cannot promise pixel-identical
  rendering to desktop browsers or larger e-readers.

## Verification

The synthetic [typography EPUB](../test/epubs/README.md) contains original text
and original geometric TrueType/CFF outlines, four faces, caps, ligatures and
variable axes. A second EPUB exercises preformatted text and equation-image
fallbacks. No personal book text or commercial font assets are added by these
fixtures.

Host tests cover CSS/catalog persistence, chapter scope, layout, v6 decoding,
font-cache pressure and independent design-unit bounds. The
[production renderer harness](../test/real_renderer/README.md) renders actual
font pixels and validates grayscale planes against the actual X4 Pro driver.
AddressSanitizer and UndefinedBehaviorSanitizer check the new paths. Monitor
previews verify software output; the physical e-ink panel remains the final
appearance check.
