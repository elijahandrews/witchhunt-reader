# EPUB typography audit and CSS patch

The CSS audit selects features from a private local EPUB collection. Source book metadata and text are not included in this repository.

## Implemented in 2.37-css1

- `text-transform: uppercase`, `lowercase`, and `none`, including explicit `inherit`, `unset`, and `initial`. Applies to external and inline CSS, nested blocks and inline tags, links, tables, and floated drop caps. Transformation happens after entity decoding and before measurement and pagination. Embedded Style OFF keeps source case.
- UTF-8 default full Unicode case mappings, including accented Latin characters and expansions such as sharp-s to SS. Tables are generated from Python 3.13's Unicode 15.1.0 database. Context-sensitive Greek final sigma and language-specific Turkic/Lithuanian casing are not implemented. `capitalize`, `full-width`, and `full-size-kana` are not implemented.
- Block `line-height`: retain the existing parser's unitless, em, and percentage interpretation, persist it in the CSS cache, inherit it through containers, and apply it during pagination. `normal`/`initial` reset spacing; `inherit`/`unset` resolve from the parent. `<br>` preserves the paragraph's spacing. This uses the existing reader normalization against a nominal 1.5 body line height and clamp of 0.7–2.0; it is not an unrestricted browser line-box implementation. Inline-only line-height and table-grid row line-height remain limited.
- CSS and section cache versions are bumped so existing EPUBs rebuild automatically. No EPUB conversion is needed.

## CSS coverage

The local audit was used to select representative CSS patterns. Book titles, author names, filenames, and source passages are omitted; repository fixtures contain synthetic text.

| Pattern | Evidence | Status |
| --- | --- | --- |
| Small uppercase date | 75% size and uppercase on a span or paragraph | Uppercase now applied; percentage sizing already supported |
| Small-cap reset | Lowercase combined with small caps | Lowercase supported |
| Inherited paragraph spacing | Body line height with chapter styles inheriting it | Inherited block line height reaches layout |
| Extract spacing | Container styles with multiple em line-height overrides | Block line spacing reaches layout |
| Generic family fallback | Serif body with sans-serif headings and inline spans; named lists ending in generic families | Font-family switching is being implemented |
| Chapter-number tracking | 0.1em letter spacing on headings and inline spans | Letter spacing is being implemented |

## Other declared gaps

The inventory also found descendant/child/adjacent-sibling selectors (including `p+p`), `::first-letter`, generic monospace, borders, `white-space: pre-wrap`, maximum image dimensions, `break-inside`/avoid behavior, widows/orphans, and richer list markers. A declaration's presence does not prove that its selector matches an element. These have not been marked fixed. In particular, this patch does not promise Kobo-equivalent EPUB fidelity or embedded font loading.

## Local validation

The CMake host harness runs the production EPUB parser, CSS cache and pagination code against deterministic display/metric stubs. It checks cold/warm cache equivalence, casing, mixed overrides, Unicode expansion, Embedded Style OFF, and exact relative line advances. This is a local execution of the rendering pipeline, not an ESP32-S3 or e-ink hardware emulator. The PlatformIO `x4pro` environment is compiled separately before flashing.

`test/epubs/make_test_css_typography.py` deterministically creates the on-device **Witch CSS Style Test** and the regression fixture. Its text is original test material. The reader's existing `Books/Licorice EPUB Style Test.epub` is updated in place; its title is now Witch CSS Style Test.

The four changed existing golden fixtures retain identical word sequences. Their y positions, pagination and page-relative metadata change because line height is now honored. The new typography fixture has its own golden plus independent behavioral assertions. Review regressions cover empty sibling spacing, direct container text, consecutive line breaks, nested drop-cap case scopes, expanding mappings, and UTF-8 sequences split across SAX callbacks.

The local EPUB collection parsed and paginated without truncation or CSS fallback. This is a parser/layout check using stub metrics, not a guarantee of pixel fidelity or device memory behavior.

Specification reference: [CSS Text Level 3, text-transform](https://www.w3.org/TR/css-text-3/#text-transform-property).
