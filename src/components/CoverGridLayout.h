#pragma once

// Geometry for a paged grid of book covers (the recent-books cover view).
//
// Nothing here is a fixed column or row count: both fall out of the space the caller hands in, so
// a higher-resolution panel yields MORE cells of the same comfortable size rather than the same
// few cells blown up. The caller supplies the content area it has after the header and button
// hints, plus the largest cover box worth drawing -- the full-size thumbnail's, since the draw path
// never upscales and a bigger box would only add empty frame.
//
// Rows come first, at full size. A page that would leave at least half a row empty takes one more
// row instead, its cells shrunk to fit (on a 540x960 panel: three rows of ~210 px covers rather
// than two rows of 240 and a fifth of the screen blank). Columns then follow from how wide a
// cover is at that height. The thumbnail is made at the cell's size (thumbWidth x thumbHeight), so
// a cover always draws 1:1: a dithered 1-bit image must never be resampled.
//
// Pure arithmetic: no renderer, no theme, no storage, so it is exercised on the host.
namespace CoverGridLayout {

// Visual constants of the grid itself. The label block holds two small-font lines (title, author).
inline constexpr int kMargin = 10;
inline constexpr int kLabelHeight = 36;
inline constexpr int kMinCellHeight = 96;

struct Input {
  int contentWidth = 0;   // width available to the grid
  int contentHeight = 0;  // height available below the header
  int bottomReserve = 0;  // strip at the bottom left free for hints / scroll arrows
  int maxCellHeight = 0;  // tallest cover box worth drawing (the full-size thumbnail's height + frame)
  int maxCellWidth = 0;   // widest cover box worth drawing (the full-size thumbnail's width + frame); 0: no cap
};

struct Layout {
  int cols = 1;
  int rows = 1;         // rows per page
  int cellWidth = 0;    // cover box width, 1 px frame included
  int cellHeight = 0;   // cover box height, 1 px frame included
  int rowStride = 0;    // cellHeight + label block + margin
  int labelWidth = 0;   // text width available under a cover
  int thumbWidth = 0;   // the stored thumbnail's box: what the cover is fitted into, drawn 1:1
  int thumbHeight = 0;  //   (the cell less its frame, no wider than the full-size thumbnail)
};

Layout compute(const Input& in);

// Which cell a point falls in, as an absolute item index, or -1 for a miss.
//
// The inverse of the caller's cell placement, and deliberately expressed in the same terms so
// the two cannot drift: `originX`/`originY` are the grid's top-left BEFORE the margin (i.e. the
// content rect's x and the content top, exactly what the render passes), `pageStartRow` is the
// first row on the visible page, and `itemCount` bounds the last partial row.
//
// The tappable cell is the cover box plus the label block under it -- what a reader sees as one
// entry -- but not the kMargin gutter after it, so a tap between two covers is a miss rather
// than a coin flip. Pure arithmetic, exercised on the host.
int hitTest(const Layout& l, int originX, int originY, int pageStartRow, int itemCount, int px, int py);

}  // namespace CoverGridLayout
