#pragma once

#include <BuildArena.h>
#include <FileIndex.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../UiListActivity.h"
#include "BookRowResolver.h"
#include "FileBrowserModel.h"
#include "RecentBooksStore.h"
#include "components/CoverGridLayout.h"
#include "components/themes/BaseTheme.h"

// CoverThumbLoader.h needs ReaderActivity.h, which includes this file: hence a pointer to it.
class CoverThumbLoader;

class FileBrowserActivity final : public UiListActivity {
 public:
  // Books = the book browser opened from Home; AllFiles = every file, opened from Settings;
  // PickFirmware / PickFolder = pickers that return a path via ActivityResult. Owned by the model,
  // which needs it to filter; aliased here so callers keep naming it FileBrowserActivity::Mode.
  using Mode = FileBrowserModel::Mode;

 private:
  void clearFileMetadata(const std::string& fullPath);
  bool removeDirRecursive(const std::string& fullPath);
  void openContextMenu();
  void handleContextMenuAction(int action, const std::string& fullPath, const std::string& entry,
                               const struct MenuResult* menuRes = nullptr);
  void doMarkAsRead(const std::string& fullPath);
  void doSetAsSleepCover(const std::string& fullPath);
  void doDeleteCache(const std::string& fullPath, const std::string& entry);
  void doRemove(const std::string& fullPath, const std::string& entry, bool isDirectory);

  static constexpr size_t LIST_WINDOW_CAPACITY = 24;
  std::array<std::string, LIST_WINDOW_CAPACITY> windowLabels;
  std::array<std::string, LIST_WINDOW_CAPACITY> windowSubtitles;
  std::array<std::string, LIST_WINDOW_CAPACITY> windowValues;
  std::array<freeink::ui::ListItem, LIST_WINDOW_CAPACITY> windowItems;
  uint16_t windowFirst = 0;
  uint16_t windowCount = 0;

  // What is in the folder and in what order. Everything about enumeration, the SD index and
  // sorting lives there; this class decides what a row means and where a tap goes next.
  FileBrowserModel model;

  std::string focusName;  // entry to select on first load (e.g. the file just returned from)

  // What a book row says instead of its filename, in the Details list and the Covers grid alike.
  BookRowResolver bookRows;

  bool detailsView() const;
  bool coversView() const;

  // Covers view: the same rows as a grid of covers, laid out by CoverGridLayout exactly as the
  // recent-books grid is, so the two share a thumbnail size and every cover either one has made.
  // Navigation stays the list's -- next/previous cell, a page per Left/Right -- and the page on
  // screen is the one holding the selection.
  struct CoverGrid {
    Rect content{};
    int top = 0;  // y of the first row, below the header
    CoverGridLayout::Layout cells{};
    int perPage = 1;
  };
  CoverGrid coverGrid() const;
  void afterUiRender() override;
  void drawCoverGrid();
  void drawCoverCell(int index, int x, int y, const CoverGridLayout::Layout& cells, bool selected,
                     const std::string& base);
  enum class CoverThumb : uint8_t { Drawn, NoCover, Missing };
  CoverThumb drawCoverThumb(const std::string& bookPath, int x, int y, int tw, int th, Rect& drawn);
  void drawTitleCard(const std::string& title, const Rect& card);
  void drawFolderCard(const Rect& card, int books);
  bool handleCoverTouch();

  // Making the covers the page on screen lacks. The render task flags a cell drawn without its
  // thumbnail; loop() then works through that page's books one at a time in short bursts, in the
  // borrowed secondary framebuffer as the Home carousel does, and redraws as each cover lands.
  std::unique_ptr<CoverThumbLoader> coverLoader;
  std::atomic<bool> coverWork{false};
  // Books whose cover failed during this visit: not retried until the browser is entered again.
  std::vector<std::string> coverFailed;
  uint8_t* lentRegion = nullptr;
  std::unique_ptr<BuildArena> coverScratch;
  void generateCovers();
  bool lendForCovers();
  void returnLentBuffer(bool callerHoldsRenderLock);

  [[nodiscard]] int listPageSize() const;
  [[nodiscard]] bool listPages() const;
  void pageSelection(int direction);
  void createFolderHere();
  void moveToFolder(const std::string& fullPath, const std::string& entry);
  bool confirmOpensOptions() const;
  bool managesFiles() const;
  ReturnTo returnTarget() const;
  void showBrowserOptionsMenu(const std::string& dirEntry = {});
  void activateSelected(bool longPress);
  void resetNavigation(int selected = 0);
  // Search: prompt for a query, then narrow the folder to the names containing it.
  void startSearch(bool everywhere);
  void applyFilter(const std::string& query);
  void goToResultFolder();
  void materializeListWindow();

 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/",
                               std::string focusName = {}, Mode mode = Mode::Books);
  ~FileBrowserActivity() override;
  void onEnter() override;
  void onExit() override;
  void loop() override;
  // The borrowed framebuffer goes back before any other screen opens on top of this one.
  void startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) override;

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  // Back and Confirm belong to handleCustomInput() alone, which classifies them through
  // ButtonEventManager because this screen means different things by a short and a long press
  // (Back: up one folder vs. leave the browser; Confirm: open vs. sync-then-open). The base
  // implementation acts on the raw press-down EDGE, which arrives a tick before the Short event
  // that the same physical press later produces -- so leaving it in place ran BOTH handlers for
  // one press: Confirm entered a folder on the press and then activated row 0 of the folder it
  // had just entered on the release, and Back finished the activity before the short-press
  // up-one-folder branch could ever be reached.
  bool handleButtons() override { return false; }
  int indexForActionValue(int16_t value) const override { return static_cast<uint16_t>(value); }
  void drawChrome() override;
  void drawFooter() override;
  void navigateButtons() override;
};
