#include "FileBrowserActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalCapabilities.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <SidecarFiles.h>
#include <Txt.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

#include "../ActivityManager.h"
#include "../ActivityResult.h"
#include "../reader/FinishedBookActivity.h"
#include "../util/BmpViewerActivity.h"
#include "../util/ConfirmationActivity.h"
#include "../util/KeyboardEntryActivity.h"
#include "BookDetails.h"
#include "BookInfoActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "FileContextMenuActivity.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "components/BookProgressPresentation.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

void FileBrowserActivity::onEnter() {
  model.load();
  int selectedIndex = 0;

  if (!focusName.empty()) {
    const size_t idx = model.findEntry(focusName);
    if (idx < model.entryCount()) {
      selectedIndex = static_cast<int>(idx);
    }
    focusName.clear();
  }

  RenderLock lock(*this);
  UiListActivity::onEnter();
  resetNavigation(selectedIndex);
}

void FileBrowserActivity::onExit() {
  UiListActivity::onExit();
  model.clear();
  clearDetails();
  detailsCache.shrink_to_fit();
}

void FileBrowserActivity::loop() {
  UiListActivity::loop();
  resolvePendingDetails();
}

// Browse Files with Details chosen. A card-wide search keeps filenames: its rows are paths, and
// the folder they sit in is what tells two same-named results apart.
bool FileBrowserActivity::detailsView() const {
  return model.getMode() == Mode::Books && SETTINGS.fileBrowserView == CrossPointSettings::BROWSER_VIEW_DETAILS &&
         !model.isDeepSearch();
}

void FileBrowserActivity::clearDetails() {
  detailsCache.clear();
  detailsNext = 0;
  detailsPending = false;
}

namespace {

std::string progressLabel(const int percent) {
  if (percent >= 100) return tr(STR_BOOK_FINISHED);
  if (percent > 0) return std::to_string(percent) + "%";
  return {};  // unread, or never opened
}

// "Author · Series #3", leaving out whichever part the book does not have.
std::string detailsSubtitle(const BookDetails& d) {
  std::string series = d.series;
  if (!series.empty() && !d.seriesIndex.empty()) series += " #" + d.seriesIndex;
  if (d.author.empty()) return series;
  if (series.empty()) return d.author;
  return d.author + " · " + series;
}

}  // namespace

// The row's details from memory, or else from the card without parsing anything. A never-opened
// EPUB comes back marked needsParse, titled by its filename until loop() has parsed it.
FileBrowserActivity::RowDetails& FileBrowserActivity::rowDetails(const std::string& path, const uint32_t size) {
  for (auto& row : detailsCache) {
    if (row.path == path) return row;
  }
  RowDetails row;
  row.path = path;
  row.size = size;
  BookDetails details;
  if (BookDetailsLookup::cached(path, size, details)) {
    row.title = std::move(details.title);
    row.subtitle = detailsSubtitle(details);
  } else {
    row.needsParse = true;
    detailsPending = true;
  }
  row.value = progressLabel(BookProgressPresentation::readPercent(path));

  if (detailsCache.capacity() < DETAILS_CACHE_CAPACITY) detailsCache.reserve(DETAILS_CACHE_CAPACITY);
  if (detailsCache.size() < DETAILS_CACHE_CAPACITY) {
    detailsCache.push_back(std::move(row));
    return detailsCache.back();
  }
  RowDetails& slot = detailsCache[detailsNext];
  detailsNext = (detailsNext + 1) % DETAILS_CACHE_CAPACITY;
  slot = std::move(row);
  return slot;
}

// Parses one never-opened EPUB per loop() pass -- ~300 ms each -- so input is read between them,
// and redraws once when the last one is done rather than once per book: every redraw is a panel
// refresh. The parse runs outside the render lock, and not beside a frame being composed, which
// shares the core and the card with it.
void FileBrowserActivity::resolvePendingDetails() {
  if (!detailsPending || renderer.isComposingFrame()) return;
  std::string path;
  uint32_t size = 0;
  {
    RenderLock lock(*this);
    const auto it =
        std::find_if(detailsCache.begin(), detailsCache.end(), [](const RowDetails& row) { return row.needsParse; });
    if (it == detailsCache.end()) {
      detailsPending = false;
      return;
    }
    path = it->path;
    size = it->size;
  }

  // Logged until the occasional first-visit reboot in Details is understood: what the heap held
  // going into a parse and coming out of it, and how long the parse took.
  const auto heapFree = [] { return static_cast<unsigned long>(esp_get_free_heap_size()); };
  const auto heapContig = [] {
    return static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT));
  };
  LOG_INF("FBR", "details parse %s: free=%lu contig=%lu minFree=%lu", path.c_str(), heapFree(), heapContig(),
          static_cast<unsigned long>(esp_get_minimum_free_heap_size()));
  const unsigned long parseStart = millis();
  BookDetails details;
  BookDetailsLookup::parse(path, size, details);
  LOG_INF("FBR", "details parse done in %lu ms (%s): free=%lu contig=%lu", millis() - parseStart,
          details.title.empty() ? "no title" : "ok", heapFree(), heapContig());

  bool done = false;
  {
    RenderLock lock(*this);
    for (auto& row : detailsCache) {
      if (row.path != path || !row.needsParse) continue;
      row.title = std::move(details.title);
      row.subtitle = detailsSubtitle(details);
      row.needsParse = false;
    }
    done = std::none_of(detailsCache.begin(), detailsCache.end(), [](const RowDetails& row) { return row.needsParse; });
    if (done) detailsPending = false;
  }
  if (done) requestUpdate();
}

void FileBrowserActivity::clearFileMetadata(const std::string& fullPath) {
  if (FsHelpers::hasEpubExtension(fullPath)) {
    Epub(fullPath, "/.crosspoint").clearCache();
    LOG_DBG("FileBrowser", "Cleared metadata cache for: %s", fullPath.c_str());
  } else if (FsHelpers::hasXtcExtension(fullPath)) {
    Xtc(fullPath, "/.crosspoint").clearCache();
    LOG_DBG("FileBrowser", "Cleared metadata cache for: %s", fullPath.c_str());
  }
}

// Iterative post-order traversal: clear book caches then delete files/dirs.
// Adapted from upstream PR #1892 (WuTofu) to handle our EPUB+XTC cache clearing.
bool FileBrowserActivity::removeDirRecursive(const std::string& fullPath) {
  auto file = Storage.open(fullPath.c_str());
  if (!file) {
    LOG_ERR("FBR", "Failed to open for removal: %s", fullPath.c_str());
    return false;
  }
  if (!file.isDirectory()) {
    file.close();
    clearFileMetadata(fullPath);
    return Storage.remove(fullPath.c_str());
  }
  file.close();

  constexpr size_t NAME_BUF = 500;
  char nameBuf[NAME_BUF];

  // Stack of (path, postOrder): postOrder=true means rmdir this path after its children.
  std::vector<std::pair<std::string, bool>> stack;
  stack.reserve(16);
  stack.push_back({fullPath, false});

  while (!stack.empty()) {
    auto [currentPath, postOrder] = std::move(stack.back());
    stack.pop_back();

    if (postOrder) {
      if (!Storage.rmdir(currentPath.c_str())) {
        LOG_ERR("FBR", "Failed to rmdir: %s", currentPath.c_str());
        return false;
      }
      continue;
    }

    auto dir = Storage.open(currentPath.c_str());
    if (!dir || !dir.isDirectory()) {
      LOG_ERR("FBR", "Failed to open dir: %s", currentPath.c_str());
      return false;
    }

    stack.push_back({currentPath, true});

    dir.rewindDirectory();
    for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      entry.getName(nameBuf, NAME_BUF);
      if (strcmp(nameBuf, ".") == 0 || strcmp(nameBuf, "..") == 0) continue;
      std::string entryPath = currentPath;
      if (entryPath.back() != '/') entryPath += '/';
      entryPath += nameBuf;
      const bool isDir = entry.isDirectory();
      entry.close();
      if (isDir) {
        stack.push_back({std::move(entryPath), false});
      } else {
        clearFileMetadata(entryPath);
        if (!Storage.remove(entryPath.c_str())) {
          LOG_ERR("FBR", "Failed to remove file: %s", entryPath.c_str());
          dir.close();
          return false;
        }
      }
    }
    dir.close();
  }
  return true;
}

bool FileBrowserActivity::handleCustomInput() {
  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.button == MappedInputManager::Button::Back) {
      if (ev.type == ButtonEventManager::PressType::Long) {
        if (model.getMode() == Mode::Books) {
          onGoHome();
          return true;
        }
        // PickFirmware: long Back = same as short Back (cancel / up dir)
      }
      // A search is the first thing Back undoes. Without this, a search that matched nothing in
      // the root folder leaves the screen empty with Home as the only way out, which throws away
      // where you were standing.
      if (model.isFiltered() || model.isDeepSearch()) {
        {
          RenderLock lock(*this);
          model.setFilter("");
          model.clearSearch();
          resetNavigation(0);
        }
        requestUpdate();
        return true;
      }
      if (ev.type == ButtonEventManager::PressType::Short || ev.type == ButtonEventManager::PressType::Long) {
        if (model.path() != "/") {
          std::string parent = model.path();
          while (parent.size() > 1 && parent.back() == '/') parent.pop_back();
          const std::string oldPath = parent;
          parent.replace(parent.find_last_of('/'), std::string::npos, "");
          // The rows are about to be replaced. Any contact still queued was aimed at the folder
          // we are leaving, and the list it would land in is a different one.
          mappedInput.flushTouchEvents();
          model.setPath(std::move(parent));  // empty -> "/"
          model.load();
          const auto pos = oldPath.find_last_of('/');
          const std::string dirName = oldPath.substr(pos + 1) + "/";
          const size_t idx = model.findEntry(dirName);
          resetNavigation((idx < model.entryCount()) ? static_cast<int>(idx) : 0);
          requestUpdate();
        } else if (model.getMode() != Mode::Books) {
          // At root in a picker: cancel back to caller.
          ActivityResult res;
          res.isCancelled = true;
          setResult(std::move(res));
          finish();
        } else {
          onGoHome();
        }
        return true;
      }
    }

    if (ev.button == MappedInputManager::Button::Confirm &&
        (ev.type == ButtonEventManager::PressType::Short || ev.type == ButtonEventManager::PressType::Long)) {
      if (confirmOpensOptions()) {
        openContextMenu();
        return true;
      }
      activateSelected(ev.type == ButtonEventManager::PressType::Long);
      return true;
    }

    // Picking a destination: the page-forward slot commits and the page-back slot makes a folder.
    // Those two are a real key on a board that has them and a tappable box on one that does not,
    // so both are reachable everywhere without a hold. A folders-only list is short enough that
    // losing the page buttons to them costs little, and the side keys and a swipe still scroll.
    if (model.getMode() == Mode::PickFolder && ev.type == ButtonEventManager::PressType::Short) {
      if (MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Right)) {
        ActivityResult res{FilePathResult{model.path()}};
        res.isCancelled = false;
        setResult(std::move(res));
        finish();
        return true;
      }
      if (MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Left)) {
        createFolderHere();
        return true;
      }
    }

    // Logical Left/Right page through the list, one screenful per press — the same thing they do in
    // the chapter selector, and the reason the context menu moved to a long press on Right. Paging
    // is driven from the event stream rather than ButtonNavigator: the navigator acts on the press
    // edge, which would page on the way into every long press.
    if (MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Right) &&
        ev.type == ButtonEventManager::PressType::Short && listPages()) {
      pageSelection(1);
      return true;
    }

    if (MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Left) &&
        ev.type == ButtonEventManager::PressType::Short && listPages()) {
      pageSelection(-1);
      return true;
    }

    // Options: a long press on the page-forward button, and a short press when the folder fits on
    // one screen and there is nothing to page. Either way the button hint says which one it is —
    // and it rides the same logical button, so rotating the device never separates the two.
    const bool optionsPress = (ev.type == ButtonEventManager::PressType::Long) ||
                              (ev.type == ButtonEventManager::PressType::Short && !listPages());
    if (model.getMode() != Mode::PickFolder &&
        MappedInputManager::isDirection(ev.button, MappedInputManager::Direction::Right) && optionsPress) {
      // Open the context menu for any selection. openContextMenu() shows
      // file-specific actions for supported files and the browser display
      // options (sort + visibility) for directories / unsupported types.
      openContextMenu();
      return true;
    }
  }
  return false;
}

void FileBrowserActivity::navigateButtons() {
  bool changed = false;
  {
    RenderLock lock(*this);
    const int previous = nav.selected;
    buttonNavigator.onNextList(
        ButtonNavigator::getStepNextButtons(), nav.selected, listCount(), [&changed] { changed = true; },
        nav.pageRowsFor(listCount()));
    buttonNavigator.onPreviousList(
        ButtonNavigator::getStepPreviousButtons(), nav.selected, listCount(), [&changed] { changed = true; },
        nav.pageRowsFor(listCount()));
    if (changed) {
      if (std::abs(nav.selected - previous) > 2) {
        nav.top = nav.selected;
      } else {
        nav.follow(listCount());
      }
    }
  }
  if (changed) requestUpdate();
}

void FileBrowserActivity::activateSelected(const bool longPress) {
  if (model.entryCount() == 0 || nav.selected < 0 || nav.selected >= listCount()) return;

  const std::string entry = model.entryName(static_cast<size_t>(nav.selected));
  if (entry.empty()) return;
  const bool isDirectory = entry.back() == '/';
  if (isDirectory) {
    if (longPress) return;
    std::string child = model.path();
    if (child.back() != '/') child += "/";
    child += entry.substr(0, entry.length() - 1);
    // As in the Back branch: drop contacts aimed at the folder we are leaving.
    mappedInput.flushTouchEvents();
    model.setPath(std::move(child));
    model.load();
    resetNavigation();
    requestUpdate();
    return;
  }
  if (model.getMode() == Mode::PickFirmware) {
    std::string cleanBasePath = model.path();
    if (cleanBasePath.back() != '/') cleanBasePath += "/";
    ActivityResult res{FilePathResult{cleanBasePath + entry}};
    res.isCancelled = false;
    setResult(std::move(res));
    finish();
    return;
  }
  // All files lists what the reader cannot open too. Selecting one of those offers what can be
  // done with it -- move it, remove it -- instead of handing the reader a file it would reject.
  if (model.getMode() == Mode::AllFiles && !FileBrowserModel::isOpenable(entry)) {
    openContextMenu();
    return;
  }

  std::string fullPath = model.entryFullPath(static_cast<size_t>(nav.selected));
  if (longPress && KOREADER_STORE.hasCredentials() && FsHelpers::hasEpubExtension(fullPath)) {
    auto& sync = APP_STATE.koReaderSyncSession;
    sync.autoPullEpubPath = fullPath;
    sync.postAction = KOReaderSyncPostAction::Reader;
    APP_STATE.saveToFile();
  }
  ReturnHint hint;
  hint.target = returnTarget();
  hint.path = model.path();
  hint.selectName = entry;
  activityManager.replaceWithReader(std::move(fullPath), std::move(hint));
}

void FileBrowserActivity::activateIndex(const int index) {
  app.clearTapFlash();
  nav.selected = index;
  activateSelected(false);
}

void FileBrowserActivity::resetNavigation(const int selected) {
  listTapActivation.reset();
  app.clearTapFlash();
  const int last = listCount() > 0 ? listCount() - 1 : 0;
  nav.reset(std::max(0, std::min(last, selected)));
}

// Rows one Left/Right press moves. drawList reports what the last render fit — which for wrapped
// rows is not a constant — and before the first render there is nothing to report yet.
int FileBrowserActivity::listPageSize() const { return nav.pageRowsFor(listCount()); }

// True when the folder is longer than one screen. When it is not, paging has nothing to do, so
// Right keeps its old short-press meaning (Options) instead of quietly stepping the selection.
bool FileBrowserActivity::listPages() const { return listCount() > listPageSize(); }

// Moves the selection a screenful, clamped at both ends.
//
// Deliberately relative, not ButtonNavigator::nextPageIndex: that snaps to index-aligned page
// boundaries, which is right for the fixed-height lists it serves but wrong here. Wrapped names
// make rows different heights, so page boundaries are not multiples of anything — snapping from
// index 39 with 8 rows on screen would land on 40 and look like the button moved one row.
//
// Moving the window too, rather than only the selection, is what makes it a page turn. The layout
// keeps the selection on screen but cannot tell a page jump from a step: landing one row past the
// last visible row looks identical either way, so it would scroll by one and put the selection on
// the bottom row — a "page" showing a single row the reader had not already seen. Anchoring the
// window to the new selection puts a full screen of new names above it instead.
void FileBrowserActivity::pageSelection(const int direction) {
  const int total = listCount();
  if (total <= 0) return;
  const int target = nav.selected + direction * listPageSize();
  nav.selected = std::max(0, std::min(total - 1, target));
  nav.top = nav.selected;
  nav.followPending = false;
  requestUpdate();
}

// Display copy only. FileBrowserModel's entry names, and every path built from them, keep the
// raw directory-entry bytes, because FAT long-filename lookup is byte-exact: opening or
// deleting an NFD entry through an NFC-normalized name fails. Composing here fixes
// rendering -- the fonts carry precomposed Hangul syllables but no conjoining jamo, so a
// Korean filename written by macOS drew as blanks -- without touching what we hand to
// storage.
// Ported from crosspoint-reader PR #3036 (Sung-jin Brian Hong <serialx@serialx.net>).
std::string getFileName(std::string filename) {
  filename = utf8NfcNorm(std::move(filename));
  if (filename.back() == '/') {
    // Bracketed unconditionally. This used to skip the brackets when the theme reported
    // showsFileIcons(), on the assumption that a folder icon carried the distinction -- but
    // buildScreen() below fills in nothing but `label` and `actionValue`, so no row in this
    // browser has ever had an icon, and under Lyra (the one theme that answers true) folders
    // were left indistinguishable from files.
    filename.pop_back();
    return "[" + filename + "]";
  }
  if (SETTINGS.showFileExtensions) {
    return filename;
  }
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

void FileBrowserActivity::materializeListWindow() {
  const int total = listCount();
  windowFirst = static_cast<uint16_t>(std::max(0, std::min(nav.top, total)));
  windowCount = static_cast<uint16_t>(
      std::min(static_cast<size_t>(total - windowFirst), static_cast<size_t>(LIST_WINDOW_CAPACITY)));
  // Details are looked up for the rows that will be drawn, not the whole window: the window runs
  // past the bottom of the screen, and each lookup is a few small file reads.
  const bool details = detailsView();
  const int drawnEnd = nav.top + nav.pageRowsFor(total);
  std::string base = model.path();
  if (base.back() != '/') base += '/';
  for (uint16_t offset = 0; offset < windowCount; ++offset) {
    const uint16_t index = static_cast<uint16_t>(windowFirst + offset);
    const std::string entry = model.entryName(index);
    windowLabels[offset] = getFileName(entry);
    windowSubtitles[offset].clear();
    windowValues[offset].clear();
    if (details && !entry.empty() && entry.back() != '/' && index < drawnEnd) {
      const RowDetails& row = rowDetails(base + entry, model.entrySize(index));
      if (!row.title.empty()) windowLabels[offset] = row.title;
      windowSubtitles[offset] = row.subtitle;
      windowValues[offset] = row.value;
    }
    windowItems[offset] = {};
    windowItems[offset].label = windowLabels[offset].c_str();
    if (!windowSubtitles[offset].empty()) windowItems[offset].subtitle = windowSubtitles[offset].c_str();
    if (!windowValues[offset].empty()) windowItems[offset].value = windowValues[offset].c_str();
    windowItems[offset].actionValue = static_cast<int16_t>(index);
  }
}

void FileBrowserActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, true);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(contentRect.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (contentRect.x + contentRect.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (contentRect.y + contentRect.height)),
                  static_cast<int16_t>(contentRect.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (listCount() == 0) {
    fui::TextAreaProps empty;
    empty.text = model.getMode() == Mode::PickFirmware ? tr(STR_NO_BIN_FILES) : tr(STR_NO_FILES_FOUND);
    empty.style = screen.theme().bodyText;
    empty.showCaret = false;
    screen.textArea(empty);
    return;
  }

  fui::ListProps props;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  // A filename may wrap; a Details row is a title over a subtitle at a fixed two-line height.
  const bool details = detailsView();
  props.labelText.maxLines = details ? 1 : 3;
  syncListViewport(screen, props, /*hasSubtitle=*/details);
  materializeListWindow();
  props.items = windowItems.data();
  props.itemsWindowFirst = windowFirst;
  props.itemsWindowCount = windowCount;
  screen.list(props);
}

void FileBrowserActivity::drawChrome() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, true);
  const std::string destination = (model.path() == "/") ? std::string(tr(STR_SD_CARD)) : model.path();
  std::string folderName =
      (model.getMode() == Mode::PickFirmware) ? std::string(tr(STR_SELECT_FIRMWARE_FILE))
      : (model.getMode() == Mode::PickFolder)
          // Names the DESTINATION in full: "Move here" means the folder being browsed, never the
          // row under the highlight, and the first reading of it is the other way round.
          ? std::string(tr(STR_MOVE_TO_FOLDER)) + ": " + destination
          : (model.path() != "/") ? model.path().substr(model.path().rfind('/') + 1)
            // At the root, All files says which browser this is: the rows alone look like the book one.
            : (model.getMode() == Mode::AllFiles) ? std::string(tr(STR_ALL_FILES))
                                                  : std::string(tr(STR_SD_CARD));
  // A narrowed folder is indistinguishable from a small one unless the header says otherwise.
  if (model.isFiltered()) {
    folderName += ": \"" + model.filter() + "\"";
  }
  GUI.drawHeader(renderer, UITheme::getHeaderRect(renderer), folderName.c_str());
}

void FileBrowserActivity::drawFooter() {
  // Only the book browser leaves to Home; the others were opened from somewhere and go back there.
  const char* backLabel =
      (model.path() == "/") ? (model.getMode() == Mode::Books ? tr(STR_HOME) : tr(STR_BACK)) : tr(STR_BACK);
  const bool hasEntries = listCount() > 0;
  bool selectingFirmwareFile = false;
  if (model.getMode() == Mode::PickFirmware && hasEntries) {
    const std::string selectedEntry = model.entryName(static_cast<size_t>(nav.selected));
    selectingFirmwareFile = !selectedEntry.empty() && selectedEntry.back() != '/';
  }
  const char* confirmLabel = !hasEntries             ? ""
                             : selectingFirmwareFile ? tr(STR_SELECT)
                             : confirmOpensOptions() ? tr(STR_OPTIONS)
                                                     : tr(STR_OPEN);
  // The Options menu is available for every entry in Books mode and in the firmware picker, where
  // it is the only place a .bin can be removed (Books mode never lists one). The menu always
  // offers the browser display options (sort + visibility); supported files get
  // extra file-specific actions appended. So the hint shows for files and dirs alike.
  // Same gate as the Right press in handleCustomInput(), so the label and the key never disagree.
  const bool showOptionsHint = model.getMode() != Mode::PickFolder && hasEntries;
  // In a folder worth paging through, Left/Right are the page buttons and the hints say so —
  // Options is then the long press on Right. In a folder that fits on one screen there is nothing
  // to page, so the strip looks exactly as it always did.
  const bool pages = listPages();
  const char* prevLabel = (model.getMode() == Mode::PickFolder) ? tr(STR_NEW) : pages ? tr(STR_LIST_PAGE_PREV) : "";
  // In a folder small enough not to page, this slot carries Options. Where Confirm already
  // carries it that would draw the same word twice on one strip, and the second copy would sit on
  // a slot such a board has no key for.
  const char* nextLabel = (model.getMode() == Mode::PickFolder)         ? tr(STR_MOVE_HERE)
                          : pages                                       ? tr(STR_LIST_PAGE_NEXT)
                          : (showOptionsHint && !confirmOpensOptions()) ? tr(STR_OPTIONS)
                                                                        : "";
  // Paging is bound to logical Left/Right and stepping to logical Up/Down, so which physical pair
  // carries which — and therefore which hint strip each label belongs on — is the orientation's
  // business, not this screen's.
  const auto hints =
      mappedInput.mapHints(backLabel, confirmLabel, prevLabel, nextLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.front.btn1, hints.front.btn2, hints.front.btn3, hints.front.btn4);
  GUI.drawSideButtonHints(renderer, hints.side.up, hints.side.down);
}

void FileBrowserActivity::openContextMenu() {
  // If no file selected or a directory selected, show browser options only
  if (model.entryCount() == 0 || nav.selected < 0 || nav.selected >= listCount()) {
    showBrowserOptionsMenu();
    return;
  }

  const std::string entry = model.entryName(static_cast<size_t>(nav.selected));
  if (entry.empty() || entry.back() == '/') {
    showBrowserOptionsMenu(entry);
    return;
  }

  std::string cleanBase = model.path();
  if (cleanBase.back() != '/') cleanBase += "/";
  const std::string fullPath = cleanBase + entry;

  startActivityForResult(std::make_unique<FileContextMenuActivity>(
                             renderer, mappedInput, fullPath, model.getSortMode(), model.getSortDirection(),
                             /*offerDirectoryActions=*/false, model.isFiltered() || model.isDeepSearch(),
                             /*offerGoToFolder=*/model.isDeepSearch(),
                             /*offerFileManagement=*/managesFiles(),
                             /*offerViewChoice=*/model.getMode() == Mode::Books),
                         [this, fullPath, entry](const ActivityResult& res) {
                           if (res.isCancelled) {
                             requestUpdate();
                             return;
                           }
                           const auto* menuRes = std::get_if<MenuResult>(&res.data);
                           if (!menuRes) {
                             requestUpdate();
                             return;
                           }
                           handleContextMenuAction(menuRes->action, fullPath, entry, menuRes);
                         });
}

// Make a folder in the directory being browsed.
//
// The name goes through the same FAT sanitiser downloads use, so a name the card cannot hold is
// corrected rather than failing at mkdir with nothing to say.
void FileBrowserActivity::createFolderHere() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_FOLDER_NAME), "", 48, InputType::Text),
      [this](const ActivityResult& res) {
        const auto* kb = res.isCancelled ? nullptr : std::get_if<KeyboardResult>(&res.data);
        if (kb == nullptr || kb->text.empty()) {
          requestUpdate();
          return;
        }
        char safe[64];
        FsHelpers::sanitizePathComponentForFat32(kb->text.c_str(), safe, sizeof(safe));
        if (safe[0] == '\0') {
          requestUpdate();
          return;
        }
        std::string target = model.path();
        if (target.back() != '/') target += "/";
        target += safe;

        const bool made = Storage.mkdir(target.c_str(), /*pFlag=*/false);
        if (!made) {
          RenderLock lock(*this);
          renderer.setNextDisplayRefreshMode(HalDisplay::HALF_REFRESH);
          GUI.drawPopup(renderer, tr(STR_NEW_FOLDER_FAILED));
        } else {
          model.load();
          const size_t idx = model.findEntry(std::string(safe) + "/");
          resetNavigation((idx < model.entryCount()) ? static_cast<int>(idx) : 0);
        }
        requestUpdate();
      });
}

// Move a file into a folder the reader picks.
//
// A move on a FAT volume is a rename: the bytes never move, so this is instant whatever the size
// of the book and cannot leave half a file behind if power is lost. The cost is that it works
// within the one volume, which is all there is here.
//
// The book browser does not list a book's sidecars, so a book moved from there takes them along:
// left behind, its cover and metadata corrections would be lost to it and invisible where they
// stayed. All files lists them as files of their own, and moves exactly the one selected.
void FileBrowserActivity::moveToFolder(const std::string& fullPath, const std::string& entry) {
  const bool withSidecars = model.getMode() == Mode::Books;
  startActivityForResult(
      std::make_unique<FileBrowserActivity>(renderer, mappedInput, "/", std::string{}, Mode::PickFolder),
      [this, fullPath, entry, withSidecars](const ActivityResult& res) {
        const auto* picked = res.isCancelled ? nullptr : std::get_if<FilePathResult>(&res.data);
        if (picked == nullptr) {
          requestUpdate();
          return;
        }
        std::string target = picked->path;
        if (target.empty()) target = "/";
        if (target.back() != '/') target += "/";
        target += entry;

        const char* message = nullptr;
        if (target == fullPath) {
          message = tr(STR_MOVE_SAME_FOLDER);
        } else if (Storage.exists(target.c_str()) || (withSidecars && SidecarFiles::anyTargetTaken(fullPath, target))) {
          // Renaming onto an existing name is not ours to resolve silently, and FAT would not
          // report which of the two survived.
          message = tr(STR_MOVE_NAME_TAKEN);
        } else if (!Storage.rename(fullPath.c_str(), target.c_str())) {
          message = tr(STR_MOVE_FAILED);
        } else if (withSidecars && !SidecarFiles::moveAll(fullPath, target)) {
          // The book is already in its new folder; saying the move failed would be wrong.
          LOG_ERR("FBR", "Moved %s but not all of its sidecars", fullPath.c_str());
        }

        if (message != nullptr) {
          RenderLock lock(*this);
          renderer.setNextDisplayRefreshMode(HalDisplay::HALF_REFRESH);
          GUI.drawPopup(renderer, message);
        } else {
          clearFileMetadata(fullPath);
          clearDetails();
          model.load();
          resetNavigation(nav.selected);
        }
        requestUpdate();
      });
}

// True when Confirm should open the entry's menu rather than the entry itself.
//
// Only on a board with no Back or Confirm key. There, Confirm exists solely as a tap -- on a
// capacitive Home key, or on the hint box -- and every route this screen has to its menu is a
// HOLD: of logical Right, or of a hint box. Neither is a gesture that hardware can make, so
// Delete, Info, Move to folder, New Folder and the sort options have no reachable home at all.
// Opening a file still does: a tap on the row selects it and a second tap opens it, through
// activateIndex() rather than through this button.
//
// A board with the keys keeps Confirm as Open, and its menu one hold of Right away.
bool FileBrowserActivity::confirmOpensOptions() const {
  return managesFiles() && !HalCapabilities::hasBackAndConfirmButtons();
}

// The two browsers a reader organises the card from, rather than picks something in. Both get
// New folder, Move to folder and Remove; the pickers keep to what their pick needs.
bool FileBrowserActivity::managesFiles() const {
  return model.getMode() == Mode::Books || model.getMode() == Mode::AllFiles;
}

// Where the reader comes back to: the browser it was opened from, in the mode it was in.
ReturnTo FileBrowserActivity::returnTarget() const {
  return model.getMode() == Mode::AllFiles ? ReturnTo::AllFiles : ReturnTo::FileBrowser;
}

// Asks for a query, then either narrows this folder or walks the whole card for it.
//
// One prompt rather than a live filter: on a folder large enough to be worth searching the model
// is reading names off the card to match them, which is the right price for pressing Search and
// the wrong one for every letter typed.
void FileBrowserActivity::startSearch(const bool everywhere) {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, everywhere ? tr(STR_SEARCH_ALL) : tr(STR_SEARCH),
                                              model.filter(), 64, InputType::Text),
      [this, everywhere](const ActivityResult& result) {
        if (result.isCancelled) {
          requestUpdate();
          return;
        }
        const auto& kb = std::get<KeyboardResult>(result.data);
        if (!everywhere) {
          applyFilter(kb.text);
          return;
        }
        // The walk reads the card and takes a moment on a full one, so say so
        // rather than looking frozen.
        {
          RenderLock lock(*this);
          GUI.drawPopup(renderer, tr(STR_SEARCHING));
          renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
          model.searchEverywhere(kb.text);
          resetNavigation(0);
        }
        requestUpdate();
      });
}

// Applies a query (or "" to clear) and puts the selection somewhere sensible: a search that
// matches nothing still shows the folder's own empty state rather than a stale row.
// Leaves the results and opens the folder the selected one actually lives in, with it selected.
// A card-wide search tells you where a book is; this is how you go and stand there.
void FileBrowserActivity::goToResultFolder() {
  if (!model.isDeepSearch() || nav.selected < 0) return;
  const std::string folder = model.resultFolder(static_cast<size_t>(nav.selected));
  const std::string full = model.entryFullPath(static_cast<size_t>(nav.selected));
  if (folder.empty()) return;
  const size_t slash = full.rfind('/');
  focusName = (slash == std::string::npos) ? full : full.substr(slash + 1);
  mappedInput.flushTouchEvents();
  {
    RenderLock lock(*this);
    model.setPath(folder);  // also ends the search
    model.load();
    const size_t idx = model.findEntry(focusName);
    focusName.clear();
    resetNavigation(idx < model.entryCount() ? static_cast<int>(idx) : 0);
  }
  requestUpdate();
}

void FileBrowserActivity::applyFilter(const std::string& query) {
  {
    RenderLock lock(*this);
    model.setFilter(query);
    resetNavigation(0);
  }
  requestUpdate();
}

void FileBrowserActivity::showBrowserOptionsMenu(const std::string& dirEntry) {
  // Resolved now rather than in the handler: the menu cannot change the selection while it is
  // open, but reading it back afterwards would be a dependency on that staying true.
  const bool isDir = !dirEntry.empty();
  std::string dirPath = model.path();
  if (isDir) {
    if (dirPath.back() != '/') dirPath += "/";
    dirPath += dirEntry.substr(0, dirEntry.length() - 1);
  }
  startActivityForResult(std::make_unique<FileContextMenuActivity>(
                             renderer, mappedInput, "", model.getSortMode(), model.getSortDirection(), isDir,
                             model.isFiltered() || model.isDeepSearch(), /*offerGoToFolder=*/false,
                             /*offerFileManagement=*/managesFiles(),
                             /*offerViewChoice=*/model.getMode() == Mode::Books),
                         [this, isDir, dirPath, dirEntry](const ActivityResult& res) {
                           if (res.isCancelled) {
                             requestUpdate();
                             return;
                           }
                           const auto* menuRes = std::get_if<MenuResult>(&res.data);
                           if (!menuRes) {
                             requestUpdate();
                             return;
                           }
                           const auto chosen = static_cast<FileContextMenuActivity::Action>(menuRes->action);
                           if (chosen == FileContextMenuActivity::Action::Open && isDir) {
                             activateSelected(false);
                             return;
                           }
                           if (chosen == FileContextMenuActivity::Action::Remove && isDir) {
                             doRemove(dirPath, dirEntry.substr(0, dirEntry.length() - 1), /*isDirectory=*/true);
                             return;
                           }
                           handleContextMenuAction(menuRes->action, "", "", menuRes);
                         });
}

void FileBrowserActivity::handleContextMenuAction(int action, const std::string& fullPath, const std::string& entry,
                                                  const MenuResult* menuRes) {
  using Action = FileContextMenuActivity::Action;
  const Action actionEnum = static_cast<Action>(action);

  if (actionEnum == Action::Search) {
    startSearch(/*everywhere=*/false);
    return;
  }
  if (actionEnum == Action::SearchAll) {
    startSearch(/*everywhere=*/true);
    return;
  }
  if (actionEnum == Action::ClearSearch) {
    {
      RenderLock lock(*this);
      model.clearSearch();
    }
    applyFilter("");
    return;
  }
  if (actionEnum == Action::GoToFolder) {
    goToResultFolder();
    return;
  }
  if (actionEnum == Action::NewFolder) {
    createFolderHere();
    return;
  }
  if (actionEnum == Action::MoveTo) {
    moveToFolder(fullPath, entry);
    return;
  }

  // Display options: apply sort + visibility state returned from the menu.
  if (actionEnum == Action::DisplayOptionsChanged) {
    if (!menuRes) {
      requestUpdate();
      return;
    }
    model.setSort(static_cast<CrossPointSettings::FILE_SORT_MODE>(menuRes->sortMode),
                  static_cast<CrossPointSettings::FILE_SORT_DIRECTION>(menuRes->sortDirection));

    // Hidden-files visibility changes the set of entries, so reload from disk.
    const bool hiddenChanged = (SETTINGS.showHiddenFiles != menuRes->showHiddenFiles);
    SETTINGS.showHiddenFiles = menuRes->showHiddenFiles;
    SETTINGS.showFileExtensions = menuRes->showFileExtensions;
    // Only Browse Files offers the view; the other browsers hand back what they were given.
    if (model.getMode() == Mode::Books) SETTINGS.fileBrowserView = menuRes->browserView;
    SETTINGS.saveToFile();
    // A row with no title of its own is titled by its filename, whose extension just may have
    // changed.
    clearDetails();

    // Re-apply ordering. The SD index is built for a specific sort mode, so when it's
    // active any sort change must re-open it (open() rebuilds on a mode mismatch);
    // loadFiles() does that. Hidden-files visibility changes the entry set, so it also
    // needs a full reload. Only the in-RAM small-folder case can re-sort in place.
    if (hiddenChanged || model.usesIndex()) {
      model.load();  // re-enumerate + (for the index) rebuild/reopen with the new mode
    } else {
      model.resort();
    }
    // Keep the selection in range after a reorder/reload.
    resetNavigation(nav.selected);
    requestUpdate();
    return;
  }

  // File-specific actions (require fullPath)
  switch (actionEnum) {
    case Action::Open: {
      ReturnHint hint;
      hint.target = returnTarget();
      hint.path = model.path();
      hint.selectName = entry;
      activityManager.replaceWithReader(std::string(fullPath), std::move(hint));
      return;
    }
    case Action::FetchAndOpen: {
      if (KOREADER_STORE.hasCredentials() && FsHelpers::hasEpubExtension(fullPath)) {
        auto& sync = APP_STATE.koReaderSyncSession;
        sync.autoPullEpubPath = fullPath;
        sync.postAction = KOReaderSyncPostAction::Reader;
        APP_STATE.saveToFile();
      }
      ReturnHint hint;
      hint.target = returnTarget();
      hint.path = model.path();
      hint.selectName = entry;
      activityManager.replaceWithReader(std::string(fullPath), std::move(hint));
      return;
    }
    case Action::MarkAsRead:
      doMarkAsRead(fullPath);
      return;
    case Action::Info:
      startActivityForResult(std::make_unique<BookInfoActivity>(renderer, mappedInput, fullPath),
                             [this](const ActivityResult&) { requestUpdate(); });
      return;
    case Action::DeleteCache:
      doDeleteCache(fullPath, entry);
      return;
    case Action::SetAsSleepCover:
      doSetAsSleepCover(fullPath);
      return;
    case Action::Remove:
      doRemove(fullPath, entry, false);
      return;
    default:
      requestUpdate();
      return;
  }
}

void FileBrowserActivity::doMarkAsRead(const std::string& fullPath) {
  std::string cachePath;
  uint8_t data[7] = {0};
  size_t dataLen = 0;

  if (FsHelpers::hasEpubExtension(fullPath)) {
    Epub epub(fullPath, "/.crosspoint");
    epub.setupCacheDir();
    cachePath = epub.getCachePath();
    // 7-byte EPUB progress: spine(2) + page(2) + pageCount(2) + percent(1)
    data[6] = 100;
    dataLen = 7;
  } else if (FsHelpers::hasXtcExtension(fullPath)) {
    Xtc xtc(fullPath, "/.crosspoint");
    xtc.setupCacheDir();
    cachePath = xtc.getCachePath();
    // 5-byte XTC progress: page(4) + percent(1)
    data[4] = 100;
    dataLen = 5;
  } else if (FsHelpers::hasTxtExtension(fullPath) || FsHelpers::hasMarkdownExtension(fullPath)) {
    Txt txt(fullPath, "/.crosspoint");
    txt.setupCacheDir();
    cachePath = txt.getCachePath();
    // 7-byte TXT progress: page(2) + offset(4) + percent(1)
    data[6] = 100;
    dataLen = 7;
  } else {
    return;
  }

  FsFile f;
  if (!Storage.openFileForWrite("FBR", cachePath + "/progress.bin", f)) {
    LOG_ERR("FBR", "Failed to write progress for mark-as-read: %s", fullPath.c_str());
    return;
  }
  f.write(data, dataLen);
  f.close();
  LOG_INF("FBR", "Marked as read: %s", fullPath.c_str());

  // Series/index/author unknown without loading — findNextBook falls back to alphabetical order.
  const std::string nextBookPath = BookFinished::findNextBookInDirectory(fullPath, {}, {});
  startActivityForResult(std::make_unique<FinishedBookActivity>(renderer, mappedInput, fullPath, nextBookPath),
                         [this, fullPath, nextBookPath](const ActivityResult& result) {
                           if (result.isCancelled) {
                             requestUpdate();
                             return;
                           }
                           const auto& menuResult = std::get<MenuResult>(result.data);
                           if (menuResult.action == static_cast<int>(BookFinished::FinishedBookAction::GoHome)) {
                             if (SETTINGS.moveFinishedBooksToCompleted) {
                               std::string movedPath;
                               BookFinished::moveFinishedBookToCompleted(fullPath, movedPath);
                             }
                             if (SETTINGS.removeFinishedBooksFromRecents) {
                               RECENT_BOOKS.removeBook(fullPath);
                             }
                             onGoHome();
                             return;
                           }
                           if (menuResult.action == static_cast<int>(BookFinished::FinishedBookAction::OpenNextBook) &&
                               !nextBookPath.empty()) {
                             if (SETTINGS.moveFinishedBooksToCompleted) {
                               std::string movedPath;
                               BookFinished::moveFinishedBookToCompleted(fullPath, movedPath);
                             }
                             if (SETTINGS.removeFinishedBooksFromRecents) {
                               RECENT_BOOKS.removeBook(fullPath);
                             }
                             ReturnHint hint;
                             hint.target = returnTarget();
                             hint.path = model.path();
                             activityManager.replaceWithReader(nextBookPath, std::move(hint));
                             return;
                           }
                           // Stay — apply side effects then reload the list (file may have moved to /COMPLETED).
                           if (SETTINGS.moveFinishedBooksToCompleted) {
                             std::string movedPath;
                             BookFinished::moveFinishedBookToCompleted(fullPath, movedPath);
                           }
                           if (SETTINGS.removeFinishedBooksFromRecents) {
                             RECENT_BOOKS.removeBook(fullPath);
                           }
                           clearDetails();  // its row now says Finished
                           model.load();
                           resetNavigation(nav.selected);
                           requestUpdate(true);
                         });
}

void FileBrowserActivity::doSetAsSleepCover(const std::string& fullPath) {
  if (FsHelpers::hasBmpExtension(fullPath)) {
    // BMP: use the shared helper that just does a file copy + settings update.
    const bool success = BmpViewerActivity::setBmpFileAsSleepScreen(fullPath);
    {
      RenderLock lock(*this);
      const char* msg = success ? tr(STR_SLEEP_SCREEN_SET) : tr(STR_FAILED_TO_SET_SLEEP_SCREEN);
      // drawPopup ships the frame; preseed its refresh mode to HALF instead of shipping twice.
      renderer.setNextDisplayRefreshMode(HalDisplay::HALF_REFRESH);
      GUI.drawPopup(renderer, msg);
    }
    requestUpdate();
  } else {
    // JPG/PNG: must render to framebuffer — open the image viewer so the user can use its Set Sleep button.
    ReturnHint hint;
    hint.target = returnTarget();
    hint.path = model.path();
    hint.selectName = model.entryName(static_cast<size_t>(nav.selected));
    activityManager.replaceWithReader(std::string(fullPath), std::move(hint));
  }
}

void FileBrowserActivity::doDeleteCache(const std::string& fullPath, const std::string& entry) {
  startActivityForResult(std::make_unique<ConfirmationActivity>(
                             renderer, mappedInput, tr(STR_DELETE_CACHE) + std::string("?"), utf8NfcNorm(entry)),
                         [this, fullPath](const ActivityResult& res) {
                           if (!res.isCancelled) {
                             clearFileMetadata(fullPath);
                             clearDetails();  // its progress went with the cache
                             LOG_INF("FBR", "Cache deleted for: %s", fullPath.c_str());
                           }
                           requestUpdate();
                         });
}

// As with a move: deleting a book from the book browser deletes the sidecars that browser never
// showed, and deleting a file from All files deletes that file alone.
void FileBrowserActivity::doRemove(const std::string& fullPath, const std::string& entry, bool isDirectory) {
  const bool withSidecars = !isDirectory && model.getMode() == Mode::Books;
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput,
                                                                tr(STR_DELETE) + std::string("? "), utf8NfcNorm(entry)),
                         [this, fullPath, isDirectory, withSidecars](const ActivityResult& res) {
                           if (!res.isCancelled) {
                             LOG_DBG("FBR", "Attempting to delete: %s", fullPath.c_str());
                             bool deleted;
                             if (isDirectory) {
                               deleted = removeDirRecursive(fullPath);
                             } else {
                               clearFileMetadata(fullPath);
                               deleted = Storage.remove(fullPath.c_str());
                               if (deleted && withSidecars && !SidecarFiles::removeAll(fullPath)) {
                                 LOG_ERR("FBR", "Deleted %s but not all of its sidecars", fullPath.c_str());
                               }
                             }
                             if (deleted) {
                               LOG_DBG("FBR", "Deleted successfully");
                               clearDetails();
                               model.load();
                               resetNavigation(nav.selected);
                               requestUpdate(true);
                             } else {
                               LOG_ERR("FBR", "Failed to delete: %s", fullPath.c_str());
                               requestUpdate();
                             }
                           } else {
                             requestUpdate();
                           }
                         });
}

int FileBrowserActivity::listCount() const {
  return static_cast<int>(std::min(model.entryCount(), static_cast<size_t>(std::numeric_limits<uint16_t>::max())));
}
