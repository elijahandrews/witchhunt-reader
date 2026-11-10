# CSS parity cache and ownership

This describes the CSS model, cache contract and native chapter integration.
`docs/epub-parity.md` tracks the complete EPUB port and its final integration
checks; isolated parser evidence is not a firmware or physical-display result.

The native `CXC` CSS cache version is **25**. Each rule keeps the native selector
index and context/border representation. The original 102-byte payload is
followed by 47 explicit bytes: extended flags (u32), line-height multiplier (f32),
white-space/float (u8 each), visibility booleans (3 bytes), three length values
(5 bytes each), background luminance (u16), avoid/inherit booleans (2 bytes),
clear/hyphens/ligatures (3 bytes), hyphen limits (3 bytes), background/pseudo
resource tokens (u32 each), and background-repeat (u8). The resulting payload is
149 bytes; compiler bitfield layout is never written to disk. `CssPropertyFlags`
uses an explicit 64-bit storage budget. Version 24 is invalidated.

After rules, a bounded resource table stores literal pseudo content and resolved
background paths. Tokens are hashes of the resource kind and exact bytes. The
font catalog follows in checksummed `CFNT` version 2 form; the final u32 points
at the resource/catalog trailer. Rule and catalog publication uses the native
write-temporary/validate/replace flow. Additional inline stacks and chapter-local
faces use two alternating checksummed catalog snapshots. A snapshot is accepted
only if it extends the catalog embedded in the current CSS cache. Named tokens
and generic-family tokens remain native u32 values; the donor's u8 stack IDs are
not used.

The catalog retains at most 250 stacks, 64 faces, 16 members per stack, eight
sources per face, and 16KiB of text. Names, paths, and source hints have separate
bounds. New cold-path STL allocations use conservative internal-memory
preflights; parser scratch uses fallible owned allocation (1KiB selector plus
4KiB declaration), released before layout. Cache inspection streams font and
resource records without hydrating their containers. `clear()` releases rule
arenas and resources but retains the book's font catalog so inline stack tokens
remain valid. An explicit book CSS rebuild resets it.

`doc_css/index.bin` is a completion marker for chapter `<head><style>` caches.
Its version byte, u16 spine count, one flags byte per spine, and u32 checksum
cover the complete scan. Each local cache uses the native CSS25 codec. Fonts are
catalogued before local declarations are resolved, so a declaration can precede
its `@font-face`. Local names use a document prefix; unrelated fallback members
retain their original identities. The index is published only after every head,
local cache, and shared font catalog has completed. Failed writes or partial
heads leave no completion index and are retried on the next open.

As in the Witch Hunt reference, linked manifest stylesheets retain the book-wide
merge. Only definite print-only references are excluded, and an incomplete
inventory keeps all sheets for that attempt. `.media_retry` plus an incomplete
cache status forces a later healthy scan. Local `<style media=...>` uses the same
screen-media decision. This is not viewport media-query evaluation. Identical
linked CSS is deduplicated only within the same archive directory, because font
and image URL resolution depends on that directory.


## Native chapter integration

Chapter layout consumes a document overlay and retains the native selector,
border, table and RTL behavior. A missing required local-style index fails the
build; recoverable CSS or font fallback leaves a layout-retry marker, so a later
healthy open can rebuild rather than permanently reuse degraded pages. Embedded
styles disabled does not require the document index.

The bounded HTML parser supplies original byte provenance. NFC composition and
case expansion retain original character ordinals through word splits. Generated
before/after text is source-less, and queued IDs bind to actually emitted spans
rather than the maximum source offset on a multi-column page. Partial markup
recovery also releases a captured right-floated value back to its parent flow.

Whitespace modes preserve the reference's pre/pre-wrap distinction, eight-column
tabs, CRLF normalization and paragraph-scoped nowrap. Computed line-height and
hyphenation policies follow block and inline scopes, with explicit resets. Exact
publisher sizes, script sizes, first-line typography and computed letter spacing
use the font backend's fractional-size APIs, while layout retains the stable
reader font identity across dynamic-font cache eviction.

Layout bounds are explicit: 32 inherited-property scopes, 32 container-width
scopes, four float scopes, four pending background illustrations and eight
pseudo scopes. Right-text capture stops at 64 words or 8 KiB of input text. Its
measurement clone is admitted only after a fresh allocation budget check and
otherwise uses normal flow. A floating image remains within one viewport and
leaves at least half the text width. A no-repeat background illustration follows
the reference's block-after-content behavior and requires both original sides
to be at least 96 pixels; this does not implement tiled CSS backgrounds. Gray
boxes use the native page compositor, with explicit nesting layers and white
child backgrounds. Keep-together groups use one retained prior page and the
native page/marker/footnote limits.

Parser-owned source origins use 804 bytes and the streaming input buffer uses
1 KiB. The fallible arena sizes its initial slab for both bounded style stacks
and target-ABI alignment, with a minimum of 4 KiB, so slight stack growth does
not reserve a second unused slab. Both incremental and full section builds
allocate the parser on the heap. Arrays and metadata remain bounded; no
parser-sized object is added to the firmware task stack.

## Focused verification

The production Chapter test target covers the CSS/layout interactions, source
anchors, floated and background images, table pictures, preview stopping,
malformed markup and inheritance resets under ASan/UBSan. CSS parity and native
arena targets cover cache round trips, corruption, partial input and allocation
failure. The document-style and media harnesses execute the production Epub
methods with synthetic archives, including cold/warm caches, interrupted writes,
relative-resource aliases, missing metadata and retry after memory pressure.
Deliberate regressions are tested in temporary source copies: removing the
catalog hydration budget, using the parent size for script tracking, dropping a
wrapped source probe, bypassing end-of-preview cleanup, losing a malformed
float's parent, or reverting to an undersized arena each make the corresponding
checks fail.
