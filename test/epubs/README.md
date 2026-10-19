# Typography test EPUB

`make_test_css_typography.py` generates `test_css_typography.epub` using original
text and original geometric font outlines from `typography_font_fixture.py`.
It requires `fonttools`; no system fonts or downloaded commercial fonts are read.
The embedded specimens include identical designs in TrueType and CFF, four faces, a wider second family, an absent first
family with a usable fallback, true `smcp` and `c2sc`, an ordinary `fi` ligature,
and a variable font with `opsz`, `wght`, and `ital` axes. Only the specimen characters
H/N/T/I/F, their lowercase forms, and spaces use those diagnostic fonts.

```sh
python test/epubs/make_test_css_typography.py --output /tmp/typography.epub
python test/epubs/make_test_css_typography.py --output /tmp/typography.epub \
  --preview-dir /tmp/typography-preview
python -m unittest discover -s test/fontconvert_sdcard -p 'test_*.py'
```

The optional preview requires `freetype-py` and writes a four-style v6 cpfont plus
an expected-data JSON file. The tests independently parse that file, verify its
small-cap source maps and shared alternate IDs, check all EPUB manifest links,
assert the font designs differ, and verify deterministic regeneration.
Generated font pixels stay in the requested output directory.

On the device, the bold specimens should have heavier stems and the italic
specimens should lean. Ordinary small caps preserve full-size input capitals;
all-small-caps substitutes both input cases with the authored small-cap design.
The `fi` small-cap sample should resolve its two letters before ordinary
ligatures. The optical samples at 80%, 100%, 160%, and 200% exercise displayed
size separately from raster density; the large design has relatively finer
strokes and narrower letters. These diagnostic outlines test feature selection,
not the aesthetic quality of a finished reading typeface. CFF and TrueType
specimens must remain the same size; their known 1000-unit geometry provides an
independent check against accidental repeated raster scaling.

This fixture does not imply complete browser typography or shaping support.
The converted-font exporter currently supports single-glyph `smcp`/`c2sc`
substitutions and explicitly rejects contextual or conflicting language-specific
mappings. General complex-script shaping, arbitrary OpenType feature settings,
and the CSS `font-optical-sizing` opt-out are not covered. Runtime optical sizing
uses the actual display size automatically. Variable GPOS kerning is baked into
offline static instances; the current lean runtime backend does not apply its
variation deltas. Encrypted or obfuscated embedded-font resources and unsupported
font formats require a separate loader capability; this fixture uses plain TTF.
Do not interpret monitor previews as calibrated physical e-ink output.

`make_test_epub_features.py` generates `test_epub_features.epub` with original
preformatted spaces, tabs, blank lines, wrapping, small-cap inheritance, and
MathML `altimg` specimens. It also embeds two original fonts under the same CSS
family alias in different chapters, with different local rules, to exercise
chapter scope and cold-cache/reopen consistency. It uses the same `fonttools`
dependency. The pipeline tests include a separate generated archive with no
external stylesheet, an oversized head, and a long body that head discovery
must stop before reading.

Head styles retain the existing simple-selector resolver's limits. The bounded
head scan and local selector limits are reported as simplified content; local
rules never become book-wide selectors. MathML image fallback does not implement
mathematical typesetting or inline equation baseline placement. Preformatted
layout in table cells remains a separate layout limitation.


## Search and reading-position checks

`test_epub_features.epub` also includes original navigation specimens:

- Searching for `silver lantern` should find three passages, including split
  emphasis tags, uppercase text and a nonbreaking space.
- `copper & stone`, `CAFÉ STRASSE`, `interleaved`, `softhyphen` and `rain returns`
  exercise entities, Unicode case conversion, joined inline text, discretionary
  hyphens and whitespace normalization.
- Search for `marker 0048` to reach the middle of the long and nested paragraph
  chapters. Change font size from 14 to 20 to 10 and back to 14 without turning
  pages; the selected passage should remain visible. Repeat after a normal page
  turn to check that deliberate navigation updates the reading position.

These checks use original synthetic text. No personal library content is included.

## Publisher page margins

The existing **Media and image markers** chapter in `test_epub_features.epub`
has a one-em body margin and a separate hanging indent around its diamond marker.
Keep **Embedded Style** on. In the **reader menu → Settings**, change
**Ignore publisher page margins** to **On**, then return to the chapter. Ordinary
text should move outward to the reader's screen margin. The diamond and its
hanging paragraph should retain their relative positions, and the styled heading
should retain its size and alignment. **Off** restores the body inset; **Default**
follows **Settings → Reader → Spacing → Ignore publisher page margins**.
Other chapters without outer margins should keep their layout. Reopen the book
to check persistence, and open a second book to check that its choice is independent.
Repeat the toggle from **Quick Overrides** and check that the passage stays visible.


## Publisher layout specimens

The feature EPUB also contains original examples for these checks with embedded
styles enabled:

- **Rules and shaded blocks:** top/bottom rules, a pale background with padding,
  right-aligned labels and a paragraph that clears the float.
- **Generated decorations and first line:** generated heading brackets, an
  attribution prefix, first-line small capitals/tracking and optional ligatures.
  Change font size to check that only the first displayed line keeps its style.
- **Empty spacers and heading height:** authored empty gaps, zero-height elements
  and immediate-parent minimum-height/background inheritance.
- **SVG ornaments and percentage icons:** original curved paths and an elliptical
  ring, scaled within percentage-width wrappers.
- **Figures kept with captions:** a bounded illustration/caption group that moves
  together across a page boundary when it fits.
- **Hyphenation policies:** none/manual/automatic policies, consecutive-line
  limits and an invented URL with optional breaks that add no spaces or hyphens.

The fixture includes only invented text, geometric fonts and original graphics.


## Small text spacing

The **Small text spacing** chapter adds original short words and sentences at
80%, 100%, and 120%, italic, bold, small caps, combining accents, ligatures, and
fine positive/negative tracking. Use a 10pt reading font with antialiasing on.
Compare the i/l stems in “still” and “little” with the saved production-renderer
specimen. Check regular and HD fonts, then reopen the book and repeat at another
size. Host pixel checks live in `test/real_renderer/fractional.cpp`.
