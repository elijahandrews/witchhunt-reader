#pragma once
#include <BuildArena.h>
#include <I18n.h>
#include <PngToBmpConverter.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "CoverThumbLoader.h"
#include "RecentBooksStore.h"
#include "activities/reader/ReaderActivity.h"
#include "components/CoverGridLayout.h"

struct Rect;

class RecentBooksActivity final : public Activity {
 public:
  // The full-size thumbnail's box. The cover is FITTED inside it (GRID_THUMB_CROP false), whole: a
  // 2:3 cover comes out about 160x240, a square one 196x196. Filling the box and cropping, as the
  // carousel does, cut a third off the top and bottom of every ordinary cover.
  //
  // The grids make their thumbnail at the size their cells come out at (CoverGridLayout's
  // thumbWidth x thumbHeight), never larger than this: a dithered 1-bit image must never be
  // resampled. On the X3 and X4 that is this box exactly, so the finished-book screen, which always
  // uses this box, shares the grid's file there. Both numbers are in the cache filename, so changing
  // either regenerates every grid cover.
  static constexpr int GRID_THUMB_WIDTH = 196;
  static constexpr int GRID_THUMB_HEIGHT = 240;
  static constexpr bool GRID_THUMB_CROP = false;
  // Largest cover box the grid will draw: the full-size thumbnail plus the 1 px frame on each side.
  // Raising the height means raising GRID_THUMB_HEIGHT (every cover regenerates), and is bounded by
  // what the label block leaves free.
  //
  // Columns and rows are NOT constants: CoverGridLayout derives both from the panel and the theme
  // metrics, so a higher-resolution device gets more cells instead of a handful of oversized ones --
  // 2x2 on the 480 px-wide X4/X3 panels, 3x3 of slightly smaller cells on a 540x960 one.
  static constexpr int GRID_MAX_CELL_HEIGHT = GRID_THUMB_HEIGHT + 2;
  static constexpr int GRID_MAX_CELL_WIDTH = GRID_THUMB_WIDTH + 2;

  // The space the cover grid has on this panel and theme -- the one input every cover grid lays
  // itself out from (Recent Books and Browse Files' Covers view), so they come out alike and one
  // thumbnail file per book serves both.
  static CoverGridLayout::Input gridInput(const GfxRenderer& renderer);

 private:
  int selectorIndex = 0;
  int initialFocusIndex = -1;  // applied once in onEnter(), then cleared

  std::vector<RecentBook> recentBooks;
  // The store stays loaded while this screen is open: it prunes and refreshes on entry and writes
  // covers and removals back (see HomeActivity::recentsHold).
  std::optional<RecentBooksStore::Hold> recentsHold;
  // Reading-progress percent per recent book (parallel to recentBooks), cached so
  // the grid badge doesn't re-read progress.bin from SD on every cell repaint.
  // -1 = not started / unknown. Refreshed whenever recentBooks is (re)loaded.
  std::vector<int8_t> bookProgress;

  // Lazy cover loading state for grid view
  bool coversLoaded = false;
  bool firstRenderDone = false;
  size_t nextCoverIndex = 0;

  // Makes the grid thumbnail of the book at nextCoverIndex, one step per loadNextCover() call, in
  // the borrowed secondary framebuffer (coverScratch_) while covers are being made.
  uint8_t* lentRegion_ = nullptr;
  std::unique_ptr<BuildArena> coverScratch_;
  CoverThumbLoader coverLoader;  // after coverScratch_, so its sessions go first

  // Partial selection repaint: track previous index so we only redraw two cells
  int prevSelectorIndex = -1;
  bool fullRedrawNeeded = true;

  // Set once Confirm commits to opening a book. Suppresses any further grid
  // selection repaint so the highlight can't visibly jump during the
  // transition into the reader.
  bool openingBook = false;

  void loadRecentBooks();
  // One unit of cover making (one CoverThumbLoader step) per call. True once every cover is resolved.
  bool loadNextCover();
  void generateCovers();
  bool lendForCovers();
  void returnLentBuffer(bool callerHoldsRenderLock);

  void switchViewMode(bool grid);
  void removeSelectedBook();
  void showSelectedBookInfo();

  // Draws a single grid cell (used for both full render and partial selection update).
  void renderGridCell(int index, bool selected, int cellX, int cellY, const CoverGridLayout::Layout& cells);

  void renderListView(RenderLock&&);
  void renderGridView(RenderLock&&);
  // The button hints and the gesture line both views end with.
  void drawHints(const Rect& contentRect, bool hasBooks);
  // Columns currently on screen — derived from the panel size and theme metrics, not a constant.
  int gridColumns() const;
  // Open the book under the selection. Shared by Confirm and by a tap, so the two agree.
  void openSelectedBook(bool longPress);
  // Tap on a cover (grid) or a row (list): Down moves the selection, Tap opens. Returns true
  // when the touch was consumed. Inert on non-touch boards.
  bool handleBookTouch();

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int focusIndex = -1)
      : Activity("RecentBooks", renderer, mappedInput), initialFocusIndex(focusIndex) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // The borrowed framebuffer goes back before any other screen opens on top of this one.
  void startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) override;
};
