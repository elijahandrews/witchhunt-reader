# Find text in an EPUB

Open the reader menu, choose **Navigation → Find in book**, and enter a word or
phrase with the existing keyboard. The search covers the current EPUB in reading
order. It matches case-insensitively, including Unicode case expansions such as
`Straße` / `STRASSE`, and ignores differences in whitespace, soft hyphens, and
invisible formatting characters. Inline formatting does not interrupt a phrase;
paragraph and line breaks act as spaces. Phrases do not cross spine-item boundaries.

The results show a chapter and surrounding text. Up/Down selects a result;
Left/Right moves a screenful when the list is long. Touch follows the configured
list activation preference. Select a result to open the page containing its
first character. **Search again** starts another query. Back returns to reading
without changing the current page, including while the search is running.

Results use raw XHTML character locations rather than rendered page numbers, so
they remain meaningful when the font size changes. The reader resolves those
locations through its current section layout.

The implementation reads small ZIP chunks and stores fixed-size result records
in a temporary file under the book cache. It holds neither the whole book nor a
list of every result in RAM. The file is removed when the search closes; a new
search replaces any file left by an interrupted shutdown. Queries are limited to
128 UTF-8 bytes; the first 10,000 matches are shown with an explicit limit message.
Missing or malformed sections and SD failures are reported instead of presenting
an incomplete scan as a successful complete search.

Search covers body text in the source EPUB. Head metadata, scripts, styles, SVG
contents, `hidden` elements, and inline `display:none` are excluded. External CSS
visibility rules are not evaluated by the search scanner, so text hidden only by
a stylesheet can still appear. Image pixels and equations stored only as images
are not searchable. Matching does not remove accents or perform general Unicode
normalization, and the reader currently opens the result page without highlighting
its words.

Host verification uses synthetic XHTML and EPUBs in `BookTextSearchTest.cpp`:

```sh
./build/epub_pipeline/EpubPipelineTest --gtest_filter='BookTextSearch.*:BookSearchSession.*'
```

The tests exercise source locations through entities and inline elements,
Unicode and chunk boundaries, overlapping matches, result paging, resource limits,
cancellation, malformed chapters, and cleanup. The same tests run under ASan/UBSan.
