# Reading position during layout changes

Changing the font, size, line spacing, orientation, or reader viewport keeps the
first source text on the current page visible after reflow. A selected search
result becomes that position instead. Its vertical placement can change because
the new layout has different page boundaries.

Consecutive settings changes keep the same source position until the reader
navigates to another page. This prevents a size cycle from repeatedly choosing an
earlier first word. A second change while the section is still indexing also
retains the pending destination instead of taking the indexing screen's page-zero
cursor.

Positions use offsets in the chapter's inflated XHTML, with a character offset
for a word split over multiple lines or pages. Source tracking follows entities,
case transforms, normalization, inline markup, and layout splitting. Each cached
page records a small source summary; no per-character map is stored on the card.
If cached ranges overlap, a targeted rebuild identifies the page as the text is
laid out. Missing or unreadable source information uses a bounded page fallback;
it does not cause an unbounded rebuild loop.

This describes changes made while the book is open. Existing saved-progress files,
bookmarks, and KOReader synchronization retain their existing paragraph/page
precision. Changing global font settings while a book is closed is therefore
outside this fix. If a CSS change alters or removes the source text itself, the
anchor stays within the same source word where possible and otherwise falls back.

`SourceAnchorTest` exercises the production EPUB layout and cache code. The
synthetic feature EPUB includes search passages and two numbered long-paragraph
chapters for device checks: find `Marker 0048`, cycle sizes without turning a
page, then repeat after a deliberate page turn. Also change the size again while
indexing is still in progress. The selected passage should remain visible; after
a page turn, the next resize should preserve the newly selected page.
