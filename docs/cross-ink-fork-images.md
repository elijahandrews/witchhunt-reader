# EPUB image backend

The fork retains CrossInk's framebuffer writer, banded grayscale rendering and
four-byte-header PXC cache format. SVG illustrations use the bounded path
rasterizer from the user's Witch branch, adapted to native `HalFile` ownership
and `DirectPixelWriter`. The shared yxml tokenizer retains its MIT notice.

`ImageFormatDetector` recognizes PNG, JPEG and GIF signatures and a bounded SVG
root prefix. Its borrowed-file overload restores the original cursor; its
path overload closes before the decoder opens that path. A filename extension
is only a hint when the source has not been extracted. Memory-backed PNG/JPEG
callers select the decoder with the actual byte-buffer overload. GIF and SVG
remain file-backed. EPUB JPEG memory admission requires a baseline SOF0 header;
progressive JPEGs retain a seekable file for the full band decoder. Detection
does not substitute for decoder validation.

The SVG subset supports solid filled paths, groups, inherited fill rules,
viewBox scaling and the standard path commands, including curves and arcs. It
rejects unsupported elements, transforms, strokes, text and external resources.
Limits are 32 KiB of source, 8 KiB per path attribute, 1,536 edges, 64 paths,
depth 16, dimension 2,048 and at most 1,048,576 output pixels. Geometry is
validated in two passes before any framebuffer mutation. Rasterization uses a
single output row with 4×4 supersampling; it allocates no full-size image canvas.
Maximum geometry/crossing storage is approximately 36 KiB plus parser, path and
row scratch. Allocations are fallible and decoding yields during long input or
row loops. Optional cache-write failure removes the incomplete cache while
retaining the rendered result, matching native raster behavior.

Progressive JPEG illustrations now decode their AC scans instead of displaying
only an enlarged DC preview. The independent MIT `ProgressiveJpeg` library from
the user's branch indexes at most 32 luma scans and 16 Huffman tables, then
decodes one coefficient band at a time with a 512-byte input buffer. Reduced
IDCT supports 1/1, 1/2, 1/4 and 1/8 output. Native integration caps the workspace
at 384 KiB, prefers PSRAM while retaining its reserve, and keeps 32 KiB of
internal heap available if internal allocation is needed. Memory pressure can
reduce decode resolution; unsupported inputs or allocation failure can use the
native DC preview only before any full-decoder band has been emitted. An EOI
marker is required before publishing a finished image, so truncated early scans
cannot become a complete PXC cache. Synthetic tests compare against libjpeg luma,
exercise all scales, mutate malformed input and inject read/write failures.

`ImageBlock::warmCache(renderer, x, y, cancel, context)` returns `Ready`,
`Unavailable` or `Cancelled`. Cache-only PNG/JPEG/GIF/SVG decoding uses the same
pixels and Bayer origin as foreground rendering but never writes a framebuffer
plane, even for entirely off-screen images. Cancellation removes partial PXC
files and remains retryable. A completed cache clears the matching transient
image-failure record. The reader runs this on the existing render task after
display, warming the current page and five following pages, including a cached
next section, with input cancellation and bounded failed-attempt tracking. A
new cache for a visible placeholder schedules one redraw; cache hits do not
create a redraw loop.

Real PNG/JPEG/GIF tests exposed dependency undefined behavior on valid inputs.
Build hooks apply small, context-checked patches to the existing libraries:

- PNGdec 1.1.6: replace unaligned typed LZ copies with `memcpy` and an exact byte
  tail; never overwrite up to three bytes beyond the requested output span.
- JPEGDEC: read byte streams with `memcpy`, and guard four Huffman-prefix shifts
  when fewer than the required bits are available.
- AnimatedGIF: use alignment-safe byte loads and bounded scanline `memcpy`,
  including an unsigned high-byte shift on its bytewise fallback.

The dependency copyright/license text is unchanged. Patches are reapplied by
firmware and host builds, and repeated application is harmless. Tests use only
original synthetic runtime fixtures and actual production raster decoders.
They compare pixels and PXC bytes, cover all orientations and grayscale planes,
screen/strip clipping, night-mode midtones, malformed/oversized SVG, allocation
and storage faults, and duplicate file-reader prevention. The exact production
PNG copy block is also tested across alignments, overlap distances and short
tails. Reverting that block reproduces both the one-byte-tail overwrite and an
unaligned-load sanitizer failure.

EPUB admission now uses image bytes, so wrong or missing extensions reach the
same decoder pipeline. External SVG illustrations are admitted; inline SVG
wrappers admit their image children while suppressing unsupported vector text.
MathML `altimg` replaces its textual descendants only when the picture is
admitted; an unavailable or memory-rejected picture leaves the fallback text.
No-repeat background illustrations share the ordinary image dimension probe,
framebuffer-loan retry and content-selected extracted-file fallback, so SVG and
unusual raster headers receive the same admission behavior.

Tables retain the first admitted picture in each cell. Grid rows fit it below
the cell text, within the spanned column width and a 240-pixel height limit.
Rows that cannot fit the native grid keep the original picture in ordered
paragraph fallback. Table pictures participate in the same image pass,
grayscale masks, bounds, cache warming and cancellation as standalone pictures;
their raw source points survive the table cache. A completed footnote preview
cannot acquire an extra image page. Nested-table text remains skipped, as
in the donor engine. Section caches use version 109 / partial marker `0xDE`
and rebuild older derived layouts without changing durable reading state.
The PXC pixel format remains compatible. For a before/after decoder-quality
comparison of an already cached picture, remove only its derived `.pxc` file so
it is decoded again; preserve progress, reader preferences and reading stats.

Cover lookup scans bounded SVG/XHTML wrappers, then guide references and common
raster cover names when the named cover is missing. Actual raster bytes select
PNG/JPEG/GIF cover conversion. Pure vector covers have no new BMP conversion
path. Sparse EPUB 3 navigation may yield to a richer NCX; losing entries are
discarded before replacement. Href resolution prefers exact paths and permits
only unique basename fallbacks. Derived metadata caches use version 11 so old
cover/TOC decisions are rebuilt. Durable reading state is unchanged.

Focused host suites exercise the production converters and parser admission,
the real metadata cache/nav parser, and unchanged production cover-selection
blocks against generated archives. These checks do not establish physical
panel appearance, peak device heap or long-run cache timing.
