#include "RecentBooksActivity.h"

#include <Bitmap.h>
#include <BuildArena.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <string>

#include "../ActivityManager.h"
#include "../util/ConfirmationActivity.h"
#include "BookInfoActivity.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/ListRowTap.h"
#include "activities/reader/ReaderActivity.h"
#include "components/BookProgressPresentation.h"
#include "components/CoverGridLayout.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"

namespace {
std::string gridThumbPath(const std::string& coverBmpPath, int tw, int th) {
  return UITheme::getCoverThumbPath(coverBmpPath, tw, th);
}

// Where the grid lives on screen: the content rect and first-row offset from the active theme,
// plus the cell geometry CoverGridLayout derives from that space.
struct GridLayout {
  Rect content{};     // rect the whole screen body lives in
  int contentTop;     // y of the first row (below the header)
  int contentHeight;  // usable height from contentTop down
  CoverGridLayout::Layout cells;
};

// Whether the recents grid draws its one-line gesture hint below the last row.
//
// ONE question, asked in one place, because it drives two things that must agree: the
// hint itself and the vertical space reserved for it (bottomReserve below). They were
// separate `gpio.deviceIsX3()` tests until this was extracted, which is a latent bug —
// a board answering differently in the two spots either reserves a strip it never
// paints, or paints the hint over the bottom row of covers.
//
// It is still spelled by board name, and that is NOT right. The real question is
// whether a hint line fits under the grid; the original comment ("On X4 (taller
// screen) there is room") is not even self-consistent, since in portrait the X3 is the
// taller panel at 792x528 against the X4's 800x480. Resolving it means deriving the
// answer from the leftover height CoverGridLayout actually leaves, which changes what
// the C3 renders and therefore wants a device in hand.
//
// One thing it must NOT be converted to, having been tried: "does the board have Left
// and Right buttons", on the theory that the hint names Up/Left/Right combos and a
// board without those keys should not advertise them. Both X3 and X4 carry
// `left = right = PIN_UNASSIGNED` — they are XteinkAdcLadder boards whose Left/Right
// come off a resistor ladder, not GPIOs — so a pin-presence predicate reads false on
// the very boards that do draw the hint.
//
// Until then it is at least wrong in exactly one place instead of three.
bool gridShowsGestureHint() { return !gpio.deviceIsX3(); }

GridLayout computeGridLayout(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();

  GridLayout l{};
  l.content = UITheme::getContentRect(renderer, true, true);
  l.contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  l.contentHeight = l.content.height - l.contentTop - metrics.verticalSpacing;
  l.cells = CoverGridLayout::compute(RecentBooksActivity::gridInput(renderer));
  return l;
}
}  // namespace

CoverGridLayout::Input RecentBooksActivity::gridInput(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = UITheme::getContentRect(renderer, true, true);
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  // Reserve the strip the gesture-hint line occupies, on the boards that draw one; the scroll
  // arrows share it either way, hence the 12 px floor. Same predicate as the draw, so the two
  // cannot drift apart. Browse Files reserves the same strip without drawing the hint: its grid
  // must come out exactly like this one, or the two would make different thumbnails.
  return {.contentWidth = content.width,
          .contentHeight = content.height - contentTop - metrics.verticalSpacing,
          .bottomReserve = gridShowsGestureHint() ? 24 : 12,
          .maxCellHeight = GRID_MAX_CELL_HEIGHT,
          .maxCellWidth = GRID_MAX_CELL_WIDTH};
}

void RecentBooksActivity::loadRecentBooks() {
  recentBooks = RECENT_BOOKS.getBooks();
  // Cache each book's reading progress once so the grid badge avoids an SD read
  // per cell per repaint. Progress can only change by opening a book, which exits
  // this activity, so the cache stays valid for the activity's lifetime.
  bookProgress.assign(recentBooks.size(), -1);
  for (size_t i = 0; i < recentBooks.size(); i++) {
    bookProgress[i] = static_cast<int8_t>(BookProgressPresentation::readPercent(recentBooks[i]));
  }
}

// One unit of cover making for the grid -- one CoverThumbLoader step -- so the burst driver in
// render() can stop between units for input. True once every recent book's cover is resolved.
bool RecentBooksActivity::loadNextCover() {
  if (coverLoader.busy()) {
    const auto step = coverLoader.step();
    if (step == CoverThumbLoader::Step::Working) return false;
    RecentBook& book = recentBooks[nextCoverIndex];
    // Done is a cover written, or the placeholder of a book that has none: either way the stored
    // path is the canonical one. A transient failure stores none, so the next visit tries again.
    const std::string stored =
        step == CoverThumbLoader::Step::Done ? ReaderActivity::coverThumbPlaceholder(book.path) : std::string();
    // Under the render lock: this runs on the loop task, and renderGridCell() reads the same string.
    RenderLock lock(*this);
    RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, stored);
    book.coverBmpPath = stored;
    nextCoverIndex++;
    return false;
  }

  for (; nextCoverIndex < recentBooks.size(); nextCoverIndex++) {
    RecentBook& book = recentBooks[nextCoverIndex];
    if (!Storage.exists(book.path.c_str())) continue;
    // The grid thumbnail is source-agnostic -- the same "<bookCacheDir>/thumb_<W>x<H>.bmp" whether
    // the cover comes from a sidecar image or the embedded one -- and the loader checks it complete
    // and no larger than the slot, so a truncated or oversized thumb is made again.
    if (!coverLoader.complete(book.path)) {
      coverLoader.begin(book.path, coverScratch_.get());
      return false;
    }
    // Already present -- make sure the stored path is the canonical placeholder so a stale
    // "[HEIGHT].bmp" / raw-sidecar entry self-heals to the unified naming without a re-decode.
    const std::string placeholder = ReaderActivity::coverThumbPlaceholder(book.path);
    if (book.coverBmpPath != placeholder) {
      RenderLock lock(*this);
      RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, placeholder);
      book.coverBmpPath = placeholder;
    }
  }
  return true;
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  if (RECENT_BOOKS.pruneMissing()) {
    RECENT_BOOKS.saveToFile();
  }
  RECENT_BOOKS.refreshSidecarMetadata(static_cast<size_t>(RECENT_BOOKS.getCount()));

  loadRecentBooks();

  selectorIndex = 0;
  if (initialFocusIndex >= 0 && initialFocusIndex < static_cast<int>(recentBooks.size())) {
    selectorIndex = initialFocusIndex;
  }
  initialFocusIndex = -1;

  coversLoaded = false;
  firstRenderDone = false;
  nextCoverIndex = 0;
  prevSelectorIndex = -1;
  fullRedrawNeeded = true;
  openingBook = false;
  // The thumbnail the grid's cells take on this panel. The UI is always portrait and the theme only
  // changes in Settings, so it holds for as long as this screen is open.
  const CoverGridLayout::Layout cells = computeGridLayout(renderer).cells;
  const std::pair<int, int> gridThumb{cells.thumbWidth, cells.thumbHeight};
  coverLoader.configure(&gridThumb, 1, GRID_THUMB_CROP);

  requestUpdate();
}

void RecentBooksActivity::onExit() {
  // ActivityManager::exitActivity holds the render lock around onExit(). Returning the region also
  // closes any session in hand and releases the one book's metadata the cover memo still holds.
  returnLentBuffer(/*callerHoldsRenderLock=*/true);
  Epub::clearCoverMetadataMemo();
  Activity::onExit();
  recentBooks.clear();
  bookProgress.clear();
}

void RecentBooksActivity::switchViewMode(bool grid) {
  APP_STATE.recentBooksGridView = grid;
  APP_STATE.saveToFile();
  coversLoaded = false;
  firstRenderDone = false;
  nextCoverIndex = 0;
  prevSelectorIndex = -1;
  returnLentBuffer(/*callerHoldsRenderLock=*/false);
  fullRedrawNeeded = true;
  requestUpdate(true);
}

void RecentBooksActivity::removeSelectedBook() {
  if (recentBooks.empty() || selectorIndex >= static_cast<int>(recentBooks.size())) return;
  const std::string bookPath = recentBooks[selectorIndex].path;
  const std::string bookTitle = recentBooks[selectorIndex].title;
  auto handler = [this, bookPath](const ActivityResult& res) {
    if (!res.isCancelled) {
      LOG_DBG("RBA", "Removing from recent books: %s", bookPath.c_str());
      RECENT_BOOKS.removeBook(bookPath);
      loadRecentBooks();
      if (recentBooks.empty()) {
        selectorIndex = 0;
      } else if (selectorIndex >= static_cast<int>(recentBooks.size())) {
        selectorIndex = static_cast<int>(recentBooks.size()) - 1;
      }
      prevSelectorIndex = -1;
      fullRedrawNeeded = true;
      requestUpdate(true);
    }
  };
  std::string heading = tr(STR_REMOVE) + std::string("? ");
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, bookTitle), handler);
}

void RecentBooksActivity::showSelectedBookInfo() {
  if (recentBooks.empty() || selectorIndex >= static_cast<int>(recentBooks.size())) return;
  const std::string& path = recentBooks[selectorIndex].path;
  if (FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path)) {
    startActivityForResult(std::make_unique<BookInfoActivity>(renderer, mappedInput, path),
                           [this](const ActivityResult&) { requestUpdate(); });
  }
}

// Open the book under the selection. Shared by the Confirm button and by a tap on a cover or a
// row, so the two cannot drift: a long Confirm additionally arms a KOReader pull, a tap never
// does (touch has no press-type distinction here and a sync is not something to trigger by
// accident).
void RecentBooksActivity::openSelectedBook(const bool longPress) {
  if (recentBooks.empty() || selectorIndex < 0 || selectorIndex >= static_cast<int>(recentBooks.size())) return;
  const bool wantsSync = longPress && KOREADER_STORE.hasCredentials();
  const std::string& selectedPath = recentBooks[selectorIndex].path;
  const bool isEpubBook = FsHelpers::hasEpubExtension(selectedPath);
  LOG_DBG("RBA", "Selected recent book: %s (sync=%d epub=%d)", selectedPath.c_str(), wantsSync ? 1 : 0,
          isEpubBook ? 1 : 0);
  if (wantsSync && isEpubBook) {
    auto& sync = APP_STATE.koReaderSyncSession;
    sync.autoPullEpubPath = selectedPath;
    sync.postAction = KOReaderSyncPostAction::Reader;
    APP_STATE.saveToFile();
  }
  openingBook = true;
  ReturnHint hint;
  hint.target = ReturnTo::RecentBooks;
  hint.selectIndex = selectorIndex;
  activityManager.replaceWithReader(recentBooks[selectorIndex].path, std::move(hint));
}

// A tap on a cover (grid) or a row (list).
//
// Point-then-confirm, the same rule ActivityManager::dispatchListTap() applies to every other
// list: the first tap on a cover moves the selection to it, and only a tap on the cover that is
// already selected opens the book. The highlight moving IS the confirmation step -- opening a
// book on a single mis-tap costs a page load and a navigation back, which is the most expensive
// thing a stray finger can do on this screen.
//
// The two views resolve the hit differently, and neither re-derives geometry. The list view
// draws through GUI.drawList, so its rows are already published in ListTouchBand and
// mappedInput.listTouch() matches against what was painted. The grid draws its own cells, but
// their geometry comes from computeGridLayout() -- the same function the render calls -- so the
// inverse runs here against a fresh copy rather than against a recorded snapshot. That is
// strictly better for the grid: pageStartRow follows selectorIndex, which this task owns, so
// there is no render-task staleness to reason about at all.
bool RecentBooksActivity::handleBookTouch() {
  if (recentBooks.empty()) return false;

  int index = -1;
  MappedInputManager::RowTouch touch = MappedInputManager::RowTouch::None;

  if (APP_STATE.recentBooksGridView) {
    const GridLayout layout = computeGridLayout(renderer);
    const int cols = std::max(1, layout.cells.cols);
    const int visibleRows = std::max(1, layout.cells.rows);
    const int pageStartRow = (selectorIndex / cols / visibleRows) * visibleRows;
    const int itemCount = static_cast<int>(recentBooks.size());
    const auto hit = [&](const int x, const int y) {
      const int cell =
          CoverGridLayout::hitTest(layout.cells, layout.content.x, layout.contentTop, pageStartRow, itemCount, x, y);
      if (cell < 0) return false;
      index = cell;
      return true;
    };
    int x = 0;
    int y = 0;
    if (mappedInput.wasScreenTouchDown(x, y) && hit(x, y)) {
      touch = MappedInputManager::RowTouch::Down;
    } else if (mappedInput.wasScreenTapped(x, y) && hit(x, y)) {
      touch = MappedInputManager::RowTouch::Tap;
    }
  } else {
    touch = mappedInput.listTouch(index);
  }

  if (touch == MappedInputManager::RowTouch::None) return false;
  // Down is claimed but not acted on, so the same contact cannot also be read by anything else.
  // Acting on it would move the selection mid-contact and defeat the two-step below.
  if (touch == MappedInputManager::RowTouch::Down) return true;

  switch (ListRowTap::apply(index, static_cast<int>(recentBooks.size()), selectorIndex)) {
    case ListRowTap::Result::Rejected:
      return true;
    case ListRowTap::Result::Selected:
      requestUpdate();
      return true;
    case ListRowTap::Result::Activate:
      openSelectedBook(/*longPress=*/false);
      return true;
  }
  return true;
}

void RecentBooksActivity::loop() {
  const bool gridView = APP_STATE.recentBooksGridView;
  const int listSize = static_cast<int>(recentBooks.size());

  // Ahead of the button queue: a tap that opens a book replaces this activity, and draining
  // queued button events into a screen that is going away serves nobody.
  if (handleBookTouch()) return;

  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    // Confirm short/long: open book (long = KOReader sync for EPUBs)
    if (ev.button == MappedInputManager::Button::Confirm &&
        (ev.type == ButtonEventManager::PressType::Short || ev.type == ButtonEventManager::PressType::Long)) {
      openSelectedBook(ev.type == ButtonEventManager::PressType::Long);
      return;
    }

    // Back short: go home
    if (ev.button == MappedInputManager::Button::Back && ev.type == ButtonEventManager::PressType::Short) {
      onGoHome();
      return;
    }

    // Up short: navigate (row up in grid, previous in list)
    if (MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Up) &&
        ev.type == ButtonEventManager::PressType::Short) {
      if (!recentBooks.empty()) {
        if (gridView) {
          selectorIndex = std::max(0, selectorIndex - gridColumns());
        } else {
          selectorIndex = ButtonNavigator::previousIndex(selectorIndex, listSize);
        }
        requestUpdate();
      }
      continue;
    }

    // Down short: navigate (row down in grid, next in list)
    if (MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Down) &&
        ev.type == ButtonEventManager::PressType::Short) {
      if (!recentBooks.empty()) {
        if (gridView) {
          selectorIndex = std::min(listSize - 1, selectorIndex + gridColumns());
        } else {
          selectorIndex = ButtonNavigator::nextIndex(selectorIndex, listSize);
        }
        requestUpdate();
      }
      continue;
    }

    // Up long: toggle between list and grid view
    if (MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Up) &&
        ev.type == ButtonEventManager::PressType::Long) {
      switchViewMode(!gridView);
      return;
    }

    // Left short: column left in grid, previous in list
    if (MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Left) &&
        ev.type == ButtonEventManager::PressType::Short) {
      if (!recentBooks.empty()) {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, listSize);
        requestUpdate();
      }
      continue;
    }

    // Right short: column right in grid, next in list
    if (MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Right) &&
        ev.type == ButtonEventManager::PressType::Short) {
      if (!recentBooks.empty()) {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, listSize);
        requestUpdate();
      }
      continue;
    }

    // Left long: remove selected book (both views)
    if (MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Left) &&
        ev.type == ButtonEventManager::PressType::Long) {
      removeSelectedBook();
      return;
    }

    // Right long: show book info (both views)
    if (MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Right) &&
        ev.type == ButtonEventManager::PressType::Long) {
      showSelectedBookInfo();
      return;
    }
  }

  if (gridView && firstRenderDone && !coversLoaded && !openingBook) generateCovers();
}

void RecentBooksActivity::render(RenderLock&& lock) {
  // Confirm has committed to opening a book; the reader is taking over the
  // screen. Skip any grid repaint so the selection highlight can't visibly
  // jump to a stale buffer position during the transition.
  if (openingBook) return;

  if (!APP_STATE.recentBooksGridView) {
    renderListView(std::move(lock));
    return;
  }

  renderGridView(std::move(lock));
  firstRenderDone = true;  // loop() starts on the covers once the grid is on screen
}

// Cover making for the grid, in bursts of CoverThumbLoader steps from loop() -- not from render(),
// where it used to run holding the render lock and decoding on the heap. In the borrowed secondary
// framebuffer, as Home and Browse Files do it: on the heap a large progressive cover took free heap
// to 4 KB (X3, 2026-10-01), one failed allocation short of an abort. A step only writes SD files, so
// the grid is redrawn when a cover lands, not per step; a press ends the burst, and the next pass
// carries on where it stood.
void RecentBooksActivity::generateCovers() {
  if (renderer.isComposingFrame() || mappedInput.hasPendingInput()) return;
  lendForCovers();                  // without the lend the decoders fall back to the heap, as before
  HalPowerManager::Lock fullSpeed;  // this runs between presses, when the governor drops the clock
  constexpr uint32_t COVER_SLICE_BUDGET_MS = 150;
  const size_t startIdx = nextCoverIndex;
  const uint32_t deadline = millis() + COVER_SLICE_BUDGET_MS;
  while (true) {
    if (loadNextCover()) {  // every cover resolved
      coversLoaded = true;
      returnLentBuffer(/*callerHoldsRenderLock=*/false);
      fullRedrawNeeded = true;
      requestUpdate();
      return;
    }
    if (nextCoverIndex != startIdx) {  // a book's cover just landed: show it
      fullRedrawNeeded = true;
      requestUpdate();
      return;
    }
    if (mappedInput.hasPendingInput() || renderer.isComposingFrame() ||
        static_cast<int32_t>(millis() - deadline) >= 0) {
      return;
    }
  }
}

// The lend and the return are Browse Files' (FileBrowserActivity::lendForBackgroundWork): before
// the lend the write buffer is brought up to the frame on the panel and, on the X4, RED RAM is seeded
// with it, which single-buffer fast diff requires.
bool RecentBooksActivity::lendForCovers() {
  if (lentRegion_ != nullptr) return true;
  if (!renderer.hasSecondaryBuffer()) return false;
  RenderLock lock(*this);
  renderer.syncWriteBufferFromDisplayed();
  if (!renderer.isX3()) renderer.syncRedRamFromFrameBuffer();
  size_t size = 0;
  uint8_t* region = renderer.borrowSecondaryBuffer(&size);
  if (region == nullptr) return false;
  coverScratch_ = makeUniqueNoThrow<BuildArena>(region, size);
  if (!coverScratch_ || !coverScratch_->valid()) {
    coverScratch_.reset();
    renderer.returnSecondaryBuffer();  // cannot fail: the region never entered the heap
    return false;
  }
  lentRegion_ = region;
  renderer.setSingleBufferFastDiff(true);
  LOG_INF("RBA", "Lent secondary framebuffer for covers (%u bytes, free=%lu)", static_cast<unsigned>(size),
          static_cast<unsigned long>(esp_get_free_heap_size()));
  return true;
}

// Whatever holds a block of the region goes first. onExit() runs under the render lock
// ActivityManager already holds; taking it again would deadlock, hence the flag.
void RecentBooksActivity::returnLentBuffer(const bool callerHoldsRenderLock) {
  if (lentRegion_ == nullptr && !coverLoader.busy()) return;
  const auto doReturn = [this] {
    coverLoader.reset();
    if (lentRegion_ == nullptr) return;
    coverScratch_.reset();
    renderer.returnSecondaryBuffer();  // cannot fail: the region never entered the heap
    renderer.setSingleBufferFastDiff(false);
    lentRegion_ = nullptr;
    Epub::clearCoverMetadataMemo();
    LOG_INF("RBA", "Returned secondary framebuffer after covers (free=%lu contig=%lu)",
            static_cast<unsigned long>(esp_get_free_heap_size()),
            static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
  };
  if (callerHoldsRenderLock) {
    doReturn();
  } else {
    RenderLock lock(*this);
    doReturn();
  }
}

void RecentBooksActivity::startActivityForResult(std::unique_ptr<Activity>&& activity,
                                                 ActivityResultHandler resultHandler) {
  returnLentBuffer(/*callerHoldsRenderLock=*/false);
  Activity::startActivityForResult(std::move(activity), std::move(resultHandler));
}

void RecentBooksActivity::renderListView(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, true);

  GUI.drawHeader(renderer, UITheme::getHeaderRect(renderer), tr(STR_MENU_RECENT_BOOKS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = contentRect.height - contentTop - metrics.verticalSpacing;

  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, contentRect.x + metrics.contentSidePadding, contentTop + 20,
                      tr(STR_NO_RECENT_BOOKS));
  } else {
    GUI.drawList(
        renderer, Rect{contentRect.x, contentTop, contentRect.width, contentHeight},
        static_cast<int>(recentBooks.size()), selectorIndex, [this](int index) { return recentBooks[index].title; },
        [this](int index) {
          const auto& book = recentBooks[index];
          if (!book.author.empty() && !book.series.empty()) return book.author + "\n" + book.series;
          if (!book.series.empty()) return book.series;
          return book.author;
        },
        [this](int index) { return UITheme::getFileIcon(recentBooks[index].path); });
  }

  if (gridShowsGestureHint()) {
    const int hintY = contentRect.y + contentRect.height - metrics.verticalSpacing - 14;
    const std::string hint = std::string(tr(STR_DIR_UP)) + "+L: " + tr(STR_VIEW_GRID) + "/" + tr(STR_VIEW_LIST) +
                             "   " + tr(STR_DIR_LEFT) + "+L: " + tr(STR_REMOVE) + "   " + tr(STR_DIR_RIGHT) +
                             "+L: " + tr(STR_INFO);
    renderer.drawText(SMALL_FONT_ID, contentRect.x + metrics.contentSidePadding, hintY, hint.c_str());
  }

  const bool hasBooks = !recentBooks.empty();
  const auto hints =
      mappedInput.mapHints(tr(STR_HOME), hasBooks ? tr(STR_OPEN) : "", "", "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.front.btn1, hints.front.btn2, hints.front.btn3, hints.front.btn4);
  GUI.drawSideButtonHints(renderer, hints.side.up, hints.side.down);

  renderer.displayBuffer();
}

void RecentBooksActivity::renderGridCell(int index, bool selected, int cellX, int cellY,
                                         const CoverGridLayout::Layout& cells) {
  const int tw = cells.cellWidth;
  const int th = cells.cellHeight;
  const int labelW = cells.labelWidth;
  const auto& book = recentBooks[index];
  const int labelY = cellY + th + 3;
  const int cellFillHeight = th + CoverGridLayout::kLabelHeight + 3;

  if (selected) {
    renderer.fillRect(cellX, cellY, tw, cellFillHeight);
  } else {
    // Clear to white before redrawing (needed when deselecting)
    renderer.fillRect(cellX, cellY, tw, cellFillHeight, false);
  }
  // The frame goes round what the cell shows: the whole cell for the loading text or a coverless
  // book, but tight round a cover -- a fitted cover (GRID_THUMB_CROP) is narrower than the cell.
  Rect frame{cellX, cellY, tw, th};

  if (!book.coverBmpPath.empty()) {
    const std::string thumbPath = gridThumbPath(book.coverBmpPath, cells.thumbWidth, cells.thumbHeight);
    FsFile file;
    bool thumbDrawn = false;
    if (Storage.openFileForRead("RBA", thumbPath, file)) {
      Bitmap bmp(file);
      // Skip a truncated thumbnail (interrupted write): its header parses but readNextRow() fails
      // partway, leaving a half-drawn cover and a GFX error. The validity check above regenerates it.
      if (bmp.parseHeaders() == BmpReaderError::Ok && bmp.isComplete()) {
        const int imgW = bmp.getWidth();
        const int imgH = bmp.getHeight();
        const int innerW = tw - 2;
        const int innerH = th - 2;
        if (imgW > 0 && imgH > 0) {
          // Mirror drawBitmap1Bit scale = min(maxW/imgW, maxH/imgH) to get rendered size,
          // then center the image within the frame.
          const float scaleX = static_cast<float>(innerW) / imgW;
          const float scaleY = static_cast<float>(innerH) / imgH;
          // Cap at 1.0: never upscale (drawBitmap1Bit also won't upscale beyond maxW/maxH).
          const float scale = std::min(1.0f, std::min(scaleX, scaleY));
          const int rendW = static_cast<int>(imgW * scale);
          const int rendH = static_cast<int>(imgH * scale);
          const int offsetX = std::max(1, (tw - rendW) / 2);
          const int offsetY = std::max(1, (th - rendH) / 2);
          // Pre-clear only the exact rendered image area; the black selection background
          // shows through in the surrounding space.
          renderer.fillRect(cellX + offsetX, cellY + offsetY, rendW, rendH, false);
          renderer.drawBitmap1Bit(bmp, cellX + offsetX, cellY + offsetY, rendW, rendH);
          thumbDrawn = true;
          // Not round the 1x1 placeholder of a book with no cover: that cell stays an empty box.
          if (imgW > 1 && imgH > 1) frame = Rect{cellX + offsetX - 1, cellY + offsetY - 1, rendW + 2, rendH + 2};
        }
      }
      file.close();
    }
    if (!thumbDrawn) {
      // Thumbnail not yet generated — clear interior and show loading label
      renderer.fillRect(cellX + 1, cellY + 1, tw - 2, th - 2, false);
      const char* loadingText = tr(STR_LOADING);
      const int textW = renderer.getTextWidth(SMALL_FONT_ID, loadingText);
      const int textH = renderer.getLineHeight(SMALL_FONT_ID);
      renderer.drawText(SMALL_FONT_ID, cellX + (tw - textW) / 2, cellY + (th - textH) / 2, loadingText, true);
    }
  } else {
    // No cover — clear the whole interior so the placeholder looks clean.
    renderer.fillRect(cellX + 1, cellY + 1, tw - 2, th - 2, false);
  }

  renderer.drawRect(frame.x, frame.y, frame.width, frame.height, !selected);

  // Reading-progress overlay on the cover: bottom-edge bar while in progress,
  // folded corner when finished, nothing for unread books.
  const int progressPercent = (index >= 0 && index < static_cast<int>(bookProgress.size())) ? bookProgress[index] : -1;
  BookProgressPresentation::drawIndicator(renderer, frame, progressPercent);

  // Label: title line 1, author line 2; white text on black for selected, black on white otherwise
  const bool black = !selected;
  std::string titleStr = renderer.truncatedText(SMALL_FONT_ID, book.title.c_str(), labelW);
  renderer.drawText(SMALL_FONT_ID, cellX + 2, labelY, titleStr.c_str(), black);
  if (!book.author.empty()) {
    std::string authorStr = renderer.truncatedText(SMALL_FONT_ID, book.author.c_str(), labelW);
    renderer.drawText(SMALL_FONT_ID, cellX + 2, labelY + 17, authorStr.c_str(), black);
  }
}

// Column count for row-wise navigation. Recomputed rather than cached: it depends on the theme
// metrics, which the settings screen can change while this activity is on the stack.
int RecentBooksActivity::gridColumns() const { return computeGridLayout(renderer).cells.cols; }

void RecentBooksActivity::renderGridView(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const GridLayout layout = computeGridLayout(renderer);
  const Rect contentRect = layout.content;
  const int contentTop = layout.contentTop;
  const int contentHeight = layout.contentHeight;
  const int margin = CoverGridLayout::kMargin;
  const int cols = layout.cells.cols;
  const int tw = layout.cells.cellWidth;
  const int cellHeight = layout.cells.rowStride;
  const int visibleRows = layout.cells.rows;
  const int totalRows = (static_cast<int>(recentBooks.size()) + cols - 1) / cols;
  const int selectedRow = selectorIndex / cols;
  const int pageStartRow = (selectedRow / visibleRows) * visibleRows;
  const int startIndex = pageStartRow * cols;

  auto cellPos = [&](int i, int& cx, int& cy) {
    const int row = (i / cols) - pageStartRow;
    const int col = i % cols;
    cx = contentRect.x + margin + col * (tw + margin);
    cy = contentTop + row * cellHeight;
  };
  LOG_DBG("RBA", "Render grid: sel=%d prev=%d start=%d pageStartRow=%d visibleRows=%d totalRows=%d", selectorIndex,
          prevSelectorIndex, startIndex, pageStartRow, visibleRows, totalRows);
  // Partial fast path: only the selection changed within the same page
  const int prevPage = prevSelectorIndex >= 0 ? (prevSelectorIndex / cols / visibleRows) : -1;
  const int curPage = selectedRow / visibleRows;
  if (!fullRedrawNeeded && prevSelectorIndex >= 0 && prevSelectorIndex != selectorIndex && prevPage == curPage) {
    // The write framebuffer holds the frame from two refreshes ago (displayBuffer()
    // swaps buffers), which still shows an older selection. Resync it to the
    // displayed frame before patching just the two affected cells; without this,
    // the stale highlight ships back to the panel and multiple cells appear selected.
    LOG_DBG("RBA", "Partial grid redraw: sel=%d prev=%d", selectorIndex, prevSelectorIndex);
    renderer.syncWriteBufferFromDisplayed();
    int cx, cy;
    cellPos(prevSelectorIndex, cx, cy);
    renderGridCell(prevSelectorIndex, false, cx, cy, layout.cells);
    cellPos(selectorIndex, cx, cy);
    renderGridCell(selectorIndex, true, cx, cy, layout.cells);
    prevSelectorIndex = selectorIndex;
    renderer.displayBuffer();
    return;
  }

  // Full redraw
  fullRedrawNeeded = false;
  prevSelectorIndex = selectorIndex;

  renderer.clearScreen();

  GUI.drawHeader(renderer, UITheme::getHeaderRect(renderer), tr(STR_MENU_RECENT_BOOKS));

  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, contentRect.x + metrics.contentSidePadding, contentTop + 20,
                      tr(STR_NO_RECENT_BOOKS));
    const auto labels = mappedInput.mapLabels(tr(STR_HOME), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int endIndex = std::min(startIndex + visibleRows * cols, static_cast<int>(recentBooks.size()));
  LOG_DBG("RBA", "Full grid redraw: sel=%d prev=%d", selectorIndex, prevSelectorIndex);
  for (int i = startIndex; i < endIndex; i++) {
    int cx, cy;
    cellPos(i, cx, cy);
    renderGridCell(i, i == selectorIndex, cx, cy, layout.cells);
  }

  // Scroll arrows when content spans multiple pages
  if (totalRows > visibleRows) {
    constexpr int arrowSize = 6;
    const int centerX = contentRect.x + contentRect.width / 2;
    if (pageStartRow > 0) {
      const int arrowY = contentTop + 2;
      for (int j = 0; j < arrowSize; ++j) {
        const int half = arrowSize - 1 - j;
        renderer.drawLine(centerX - half, arrowY + j, centerX + half, arrowY + j);
      }
    }
    if (pageStartRow + visibleRows < totalRows) {
      const int arrowY = contentTop + contentHeight - arrowSize - 2;
      for (int j = 0; j < arrowSize; ++j) {
        renderer.drawLine(centerX - j, arrowY + j, centerX + j, arrowY + j);
      }
    }
  }

  if (gridShowsGestureHint()) {
    const int hintY = contentRect.y + contentRect.height - metrics.verticalSpacing - 14;
    const std::string hint = std::string(tr(STR_DIR_UP)) + "+L: " + tr(STR_VIEW_GRID) + "/" + tr(STR_VIEW_LIST) +
                             "   " + tr(STR_DIR_LEFT) + "+L: " + tr(STR_REMOVE) + "   " + tr(STR_DIR_RIGHT) +
                             "+L: " + tr(STR_INFO);
    renderer.drawText(SMALL_FONT_ID, contentRect.x + metrics.contentSidePadding, hintY, hint.c_str());
  }

  const auto hints = mappedInput.mapHints(tr(STR_HOME), tr(STR_OPEN), "", "", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.front.btn1, hints.front.btn2, hints.front.btn3, hints.front.btn4);
  GUI.drawSideButtonHints(renderer, hints.side.up, hints.side.down);

  renderer.displayBuffer();
}
