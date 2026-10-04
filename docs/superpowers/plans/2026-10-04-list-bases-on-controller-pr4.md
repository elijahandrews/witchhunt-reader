# List bases on the controller (PR 4) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the three FreeInkUI list bases (`UiListActivity`, `MenuListActivity`, `TabbedUiListActivity`) and the ~15 screens built on them onto `ListController`. Paging becomes "a screenful" (the drawn window), tabbed lists get per-tab memory, and every one of these lists draws side Up/Down hints.

**Architecture:**
- **Paging:** `ListGrammar::page()` pages by the window the render drew (top row plus rows drawn) instead of a fixed page size.
- **Controller:** `ListController` stops holding an `int&` selection. It asks its `ListHost` for the selected row, to select one, to show one at the top of the screen, and for the published window.
- **Bases:** `UiListActivity` hosts the controller through small "position" hooks. Positions equal rows by default. `TabbedUiListActivity` puts its tab bar in front as position 0, and keeps one `ListNav` per tab, adapted from upstream crosspoint-reader.
- **What stays:** row rendering is unchanged (already FUI), and the file browser keeps its own input until PR 6.

**Tech Stack:** C++ (gnu++17, ESP-IDF Arduino), PlatformIO, FreeInkUI (`freeink-sdk/libs/ui`, unchanged), GoogleTest host suite (CMake + Ninja, MSYS2 UCRT64 gcc).

**Spec:** `docs/superpowers/specs/2026-10-03-list-input-harmonization-design.md`. Read §1 (R3 page is a screenful, R4, R5a), §2, §2b, §3, §6 step 4 and §8.

## Global Constraints

**The button scheme (§1):**
- **Up/Down short:** step one row (wraps); two quick taps (presses < 300 ms apart) jump a page.
- **Up/Down long:** first/last selectable row; on tabbed lists, previous/next tab.
- **Left/Right short:** step by default, or the screen's declared pair; declaring either side takes the step off both.
- **Left/Right long:** page; repeats every 500 ms while the physical key stays held.
- **Confirm:** short opens; long is the declared second action, otherwise the same as short.
- **Back:** short = the screen's back; long = Home.

**Paging and rendering:**
- **R3, page is a screenful:**
  - Forward selects the first row below the drawn window and scrolls so it is the top row; back mirrors it.
  - Paging clamps at the ends. On a list that fits one screen it goes to the first/last row.
  - The window is published by the render. It is never measured on the loop task.
  - `ListNav::top` is written only under `RenderLock`.
- **Swipe:** pages the selection exactly like a long Left/Right, never as an injected button.
- **Hints:** every migrated list draws both hint strips through `ListController::drawHints()`, with the side Up/Down boxes. Its content area reserves the side gutter: `UITheme::getContentRect(renderer, true, true)`.

**Input discipline:**
- List screens read button **events** only.
- Every state of a migrated screen reads events, never levels. A level-read press would reach the list again as an event: the spec §2 deferral rule.
- Blocking loops that read Back by level must call `buttonEvents.drain()` afterwards.

**Scope limits:**
- **No SDK change:** FreeInkUI stays byte-identical to upstream's.
- **No `std::function`** in declarations; nothing allocated per tick or per render; `ListController` stays ≤ 100 bytes (`static_assert`).

**Process:**
- **Flash:** this PR records its `default` delta. It must be flash-negative, or the PR description names the functional or RAM benefit it buys (§8).
- **Docs:** `USER_GUIDE.md` and `RELEASE_NOTES.md` are updated in this PR (Task 6).
- **Strings:** user-facing strings via `tr(STR_*)`. No new strings are expected. Never hand-run `scripts/gen_i18n.py`.
- **Formatting:** run `"/c/Program Files/LLVM/bin/clang-format.exe" -i <file>` on every touched C++ file.
- **Builds:**
  - Firmware builds run in **PowerShell** with `$env:PLATFORMIO_CORE_DIR = 'C:\pio'`.
  - Host tests run in **Git Bash** (`test/build`, Ninja).
  - Implementers build only `default`; the controller builds `x4pro` and `lilygo_t5s3` at the end.

## Review Focus

1. **A press read by level in a non-list state reaching the list afterwards as an event.**
   - The cases are the font picker's warm-up (Back cancels it) and the weather settings' city results (Back leaves them).
   - Expected: that Back acts once and does not also close the screen.
   - Pinned by Task 3 Step 6 and Task 4 Step 5, which move both to events, and by device items 3.4 and 4.5.
2. **Tab-bar focus on Settings and the reader menu.**
   - Back from a row returns to the bar. Back on the bar leaves: Settings saves and goes Home; the reader menu commits its overrides.
   - Confirm on the bar moves to the next tab.
   - Long Up/Down switches tab, and from a row lands on the new tab's remembered row.
   - Pinned by Task 5's position hooks and device items 5.1–5.6.
3. **Separators at a screen's edges in menus and Settings.**
   - Expected: stepping, paging and long Up/Down never select a section header.
   - Pinned by the grammar's `PageSkipsUnselectableRows` (Task 1), `isRowSelectable` (Tasks 4 and 5), and device item 4.3.
4. **Paging back from a short last screen.**
   - When the last screen shows fewer rows than a full screen, expected: Left-hold still moves back a full screen.
   - Pinned by `publishListWindow()` keeping the last full count (Task 3 Step 3) and device item 3.3.
5. **Wrapped two-line rows (menus, Settings) when paging.**
   - Expected: a page shows the next rows with none skipped, and the selection sits on the top row.
   - Pinned by `RowsOfDifferentHeightsPageByWhatWasDrawn` (Task 1) and device item 4.2.

---

## Before you start

- [ ] **Branch:** `feat/list-bases-on-controller` (stacked on `feat/list-controller-opds`, PR #385). It already holds the amended spec (commits `b84833f36`, `571b649a8`).
- [ ] **Baseline the host suite** (Git Bash):

```bash
cd /c/_development/witchhunt-reader
cmake --build test/build -j 8 -- -k 0 > /tmp/hb.log 2>&1; grep FAILED /tmp/hb.log | grep -v epub_build_inventory
ctest --test-dir test/build -j 8 2>&1 | grep -E "tests passed|tests failed"
```

Expected: `100% tests passed out of 1168`. `epub_build_inventory` fails to build on Windows (`<dlfcn.h>`) and that is pre-existing.

- [ ] **Baseline `default` flash** (PowerShell):

```powershell
$env:PLATFORMIO_CORE_DIR = 'C:\pio'; & "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e default 2>&1 | Select-String -Pattern "Flash:|RAM:|SUCCESS|FAILED|error:" | Select-Object -Last 8
```

Expected: Flash 6229455 B, RAM 58264 B (the PR 3 head). Record it.

---

### Task 1: `ListGrammar` pages by the drawn window

The grammar's `page()` changes:
- **Today:** it is page-aligned to a fixed `pageRows`.
- **New:** it pages relative to the window the screen drew, `Rows::top` and `Rows::drawn` (spec R3). On a renderer with fixed pages, such as OPDS today, the result is identical.

**Files:**
- Modify: `src/util/ListGrammar.h` (the `Rows` struct and the `page()`/`fitsOnePage()` comments)
- Modify: `src/util/ListGrammar.cpp` (`step()`, `page()`, `fitsOnePage()`)
- Modify: `test/list_grammar/ListGrammarTest.cpp` (row helpers and the `Page*` tests)

**Interfaces:**
- Produces: `struct ListGrammar::Rows { int count = 0; int top = 0; int drawn = 1; Selectable selectable = nullptr; const void* ctx = nullptr; };`. The field `pageRows` is gone.
- `page(rows, from, direction)` keeps its signature. `fitsOnePage(rows)` and `completesDoubleTap(...)` keep theirs.

- [ ] **Step 1: Rewrite the row helpers and the paging tests**

In `test/list_grammar/ListGrammarTest.cpp`, replace the two helpers `plainRows` and `maskedRows` with:

```cpp
Rows plainRows(int count, int drawn, int top = 0) {
  Rows rows;
  rows.count = count;
  rows.top = top;
  rows.drawn = drawn;
  return rows;
}

Rows maskedRows(const Mask& mask, int drawn, int top = 0) {
  Rows rows;
  rows.count = static_cast<int>(mask.selectable.size());
  rows.top = top;
  rows.drawn = drawn;
  rows.selectable = &maskSelectable;
  rows.ctx = &mask;
  return rows;
}
```

Delete these tests, whose expectations assumed page alignment:
- `PageForwardGoesToTheNextPageAndClampsOnTheLast`
- `PageBackGoesToThePreviousPageAndClampsOnTheFirst`
- `PageOnOnePageListGoesToTheEnds`
- `PageTreatsANonPositivePageAsOneRow`
- `PageSkipsUnselectableRows`
- `PageAtAHeaderAtTheEndSettlesBesideIt`
- `OutOfRangeStartIsClampedFirst`

Add in their place:

```cpp
TEST(ListGrammarRows, PageForwardShowsTheRowBelowTheWindow) {
  EXPECT_EQ(ListGrammar::page(plainRows(50, 23, 0), 5, 1), 23);
  EXPECT_EQ(ListGrammar::page(plainRows(50, 23, 23), 30, 1), 46);
  EXPECT_EQ(ListGrammar::page(plainRows(50, 23, 46), 47, 1), 49);  // last screen: clamps on the last row
}

TEST(ListGrammarRows, PageBackShowsTheWindowAbove) {
  EXPECT_EQ(ListGrammar::page(plainRows(50, 23, 23), 30, -1), 0);
  EXPECT_EQ(ListGrammar::page(plainRows(50, 23, 27), 40, -1), 4);
  EXPECT_EQ(ListGrammar::page(plainRows(50, 23, 0), 5, -1), 0);  // first screen: clamps on the first row
}

TEST(ListGrammarRows, RowsOfDifferentHeightsPageByWhatWasDrawn) {
  // Wrapped titles: only 7 of 30 rows fit, and the screen shows rows 7-13.
  EXPECT_EQ(ListGrammar::page(plainRows(30, 7, 7), 9, 1), 14);
  EXPECT_EQ(ListGrammar::page(plainRows(30, 7, 7), 9, -1), 0);
}

TEST(ListGrammarRows, ASelectionOutsideTheWindowPagesFromItself) {
  // The window is a render old: the selection has already stepped below it.
  EXPECT_EQ(ListGrammar::page(plainRows(50, 23, 0), 23, 1), 46);
  EXPECT_EQ(ListGrammar::page(plainRows(50, 23, 0), 30, -1), 7);
}

TEST(ListGrammarRows, PageOnOnePageListGoesToTheEnds) {
  const Rows rows = plainRows(5, 23);
  EXPECT_EQ(ListGrammar::page(rows, 2, 1), 4);
  EXPECT_EQ(ListGrammar::page(rows, 2, -1), 0);
}

TEST(ListGrammarRows, PageTreatsANonPositiveWindowAsOneRow) {
  EXPECT_EQ(ListGrammar::page(plainRows(5, 0), 0, 1), 1);
}

TEST(ListGrammarRows, PageSkipsUnselectableRows) {
  // Rows 4 and 8 are headers; four rows fit on a screen.
  const Mask mask{{true, true, true, true, false, true, true, true, false, true}};
  EXPECT_EQ(ListGrammar::page(maskedRows(mask, 4, 0), 0, 1), 5);   // lands on header 4, walks on
  EXPECT_EQ(ListGrammar::page(maskedRows(mask, 4, 4), 5, 1), 9);   // lands on header 8, walks on
  EXPECT_EQ(ListGrammar::page(maskedRows(mask, 4, 8), 9, -1), 5);  // lands on header 4, stays on its screen
}

TEST(ListGrammarRows, PageAtAHeaderAtTheEndSettlesBesideIt) {
  const Mask mask{{true, true, true, true, true, false}};
  EXPECT_EQ(ListGrammar::page(maskedRows(mask, 3, 3), 3, 1), 4);
  const Mask leading{{false, true, true, true}};
  EXPECT_EQ(ListGrammar::page(maskedRows(leading, 2, 2), 3, -1), 1);
}

TEST(ListGrammarRows, OutOfRangeStartIsClampedFirst) {
  const Rows rows = plainRows(5, 2);
  EXPECT_EQ(ListGrammar::page(rows, 9, -1), 2);
  EXPECT_EQ(ListGrammar::page(rows, 9, 1), 4);
  EXPECT_EQ(ListGrammar::step(rows, 9, 1), 0);
  EXPECT_EQ(ListGrammar::step(rows, -3, -1), 4);
}
```

The other tests (step, first/last, empty, double-tap, commands, labels) stay as they are. Their `plainRows(count, n)` calls now mean "n rows drawn from row 0".

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build test/build --target ListGrammarTest 2>&1 | grep -E "error" | head -3
```

Expected: compile errors, because `Rows` has no member `top`/`drawn`.

- [ ] **Step 3: Change `Rows` and the comments in `src/util/ListGrammar.h`**

Replace the `Rows` struct:

```cpp
struct Rows {
  int count = 0;
  int pageRows = 1;  // rows the screen draws per page
  Selectable selectable = nullptr;
  const void* ctx = nullptr;
};
```

with:

```cpp
struct Rows {
  int count = 0;
  int top = 0;    // the first row the screen drew
  int drawn = 1;  // how many rows it drew from there: its window, the size of a page
  Selectable selectable = nullptr;
  const void* ctx = nullptr;
};
```

Replace the comment above `int page(...)` with:

```cpp
// A screenful back (-1) or forward (+1), relative to the window the screen drew: forward lands on
// the first row below it, back on the row a window above it. From a selection outside the window
// (the window is a render old) the page is measured from the selection instead. Clamped at the
// ends, so a list that fits one screen pages to its ends. A header at the target is passed over
// to the first selectable row of that page, or else the nearest one before it.
```

Replace the comment above `bool fitsOnePage(...)` with:

```cpp
// Whether every row fits in the drawn window, so there is no page to jump to.
```

- [ ] **Step 4: Implement in `src/util/ListGrammar.cpp`**

In the anonymous namespace, after `isSelectable`, add:

```cpp
int clampRow(const Rows& rows, const int row) { return row < 0 ? 0 : (row >= rows.count ? rows.count - 1 : row); }
```

In `step()`, replace `const int start = from < 0 ? 0 : (from >= rows.count ? rows.count - 1 : from);` with `const int start = clampRow(rows, from);`.

Replace the whole `page()` function with:

```cpp
int page(const Rows& rows, const int from, const int direction) {
  if (rows.count <= 0) return from;
  const int window = rows.drawn > 0 ? rows.drawn : 1;
  const int start = clampRow(rows, from);
  const int top = clampRow(rows, rows.top);
  // From inside the drawn window a page is the window's neighbour; from outside it (the window is a
  // render old) it is the same distance from the selection itself.
  const bool inWindow = start >= top && start < top + window;
  const int base = inWindow ? top : start;
  const int target = clampRow(rows, direction > 0 ? base + window : base - window);
  // Settle on the first selectable row at or after the target, so a header that opens a page
  // never pushes the selection onto another page; failing that, the nearest one before it.
  for (int row = target; row < rows.count; ++row) {
    if (isSelectable(rows, row)) return row;
  }
  for (int row = target - 1; row >= 0; --row) {
    if (isSelectable(rows, row)) return row;
  }
  return from;
}
```

Replace `fitsOnePage`:

```cpp
bool fitsOnePage(const Rows& rows) { return rows.count <= (rows.drawn > 0 ? rows.drawn : 1); }
```

- [ ] **Step 5: Run the grammar tests**

```bash
"/c/Program Files/LLVM/bin/clang-format.exe" -i src/util/ListGrammar.h src/util/ListGrammar.cpp test/list_grammar/ListGrammarTest.cpp
cmake --build test/build --target ListGrammarTest && ctest --test-dir test/build -R ListGrammar --output-on-failure 2>&1 | tail -3
```

Expected: `100% tests passed out of 35`. That is 33 minus 7 deleted plus 9 added.

`src/activities/ListController.cpp` still uses `pageRows` and will not compile in the firmware until Task 2. The host suite does not build it, so commit Tasks 1 and 2 back to back.

- [ ] **Step 6: Commit**

```bash
git add src/util/ListGrammar.h src/util/ListGrammar.cpp test/list_grammar/ListGrammarTest.cpp
git commit -m "feat(lists): a page is the screenful the render drew, not a fixed page size

ListGrammar::page() now pages relative to the window the screen drew (its
top row and how many rows it drew): forward lands on the first row below it,
back a window above, clamped at the ends. Rows of different heights (wrapped
titles, touch row heights) page by what is actually on screen; a selection
already outside the window pages from itself. On a renderer with fixed pages
the result is the old page-aligned one. Spec R3."
```

### Task 2: The controller asks its host for selection, window and page turns; OPDS adapts

**Files:**
- Replace: `src/activities/ListController.h` (full file below)
- Modify: `src/activities/ListController.cpp` (functions listed below)
- Modify: `src/activities/browser/OpdsBookBrowserActivity.h` (the two host structs, two atomics, two controller members)
- Modify: `src/activities/browser/OpdsBookBrowserActivity.cpp` (host method definitions, two `store()` lines in `render()`)

**Interfaces:**
- Consumes: `ListGrammar::Rows{count, top, drawn, selectable, ctx}` (Task 1).
- Produces:
  - `struct ListWindow { int top = 0; int drawn = 1; };`
  - `ListHost` virtuals:
    - pure: `int listCount() const`, `ListWindow listWindow() const`, `int listSelected() const`, `void listSelect(int row)`, `void onListActivate(int row, bool longPress)`, `void onListBack()`, `void onListHome()`
    - with defaults: `bool listSelectable(int) const` (true), `bool listActionAvailable(ListGrammar::Side, int) const` (true), `void listShowAtTop(int row)` (calls `listSelect(row)`), `void onListAction(ListGrammar::Side, int)`, `void onListTab(int)`, `void onListOtherEvent(const ButtonEventManager::ButtonEvent&)`
  - `ListController(MappedInputManager&, ButtonEventManager&, ListHost&, const ListDeclaration&)`, with `update()`, `page(int)`, `tapRow(int)`, `drawHints(GfxRenderer&, const char*, const char*) const` and the new `reset()`.
  - Removed: `ListHost::listPageRows()`, `ListHost::onListSelectionChanged()`, and the `int& selection` constructor parameter.

- [ ] **Step 1: Replace `src/activities/ListController.h`**

```cpp
#pragma once

#include <I18n.h>

#include <cstddef>
#include <cstdint>

#include "ButtonEventManager.h"
#include "ListRowTap.h"
#include "MappedInputManager.h"
#include "util/ListGrammar.h"

class GfxRenderer;

// The rows a screen drew in its last render, from its top row: what a page is (spec R3).
struct ListWindow {
  int top = 0;
  int drawn = 1;
};

// The screen's half of a ListController: what the controller asks of the list, and what it tells
// the screen. A screen with two lists (OPDS: catalog and format picker) gives each its own host.
class ListHost {
 public:
  virtual int listCount() const = 0;
  // The window the last render drew. Published by the render and read here on the loop task: never
  // measure from the renderer's live orientation here.
  virtual ListWindow listWindow() const = 0;
  virtual bool listSelectable(int /*row*/) const { return true; }
  // Whether a declared Left/Right action applies to `row` right now. Asked for declared sides only.
  virtual bool listActionAvailable(ListGrammar::Side /*side*/, int /*row*/) const { return true; }

  // The selected row, and moving it; listSelect() also asks for the repaint.
  virtual int listSelected() const = 0;
  virtual void listSelect(int row) = 0;
  // A page turn: select `row` and scroll so it is the top row on screen. A screen that lays its rows
  // out a page at a time already does that by selecting it, hence the default.
  virtual void listShowAtTop(int row) { listSelect(row); }

  virtual void onListActivate(int row, bool longPress) = 0;
  virtual void onListBack() = 0;
  virtual void onListHome() = 0;
  virtual void onListAction(ListGrammar::Side /*side*/, int /*row*/) {}
  virtual void onListTab(int /*direction*/) {}
  // A button the scheme does not use on a list (Power).
  virtual void onListOtherEvent(const ButtonEventManager::ButtonEvent& /*event*/) {}

 protected:
  ~ListHost() = default;
};

// A short Left or Right press the screen takes over from the default step.
struct ListAction {
  bool declared = false;
  StrId label = StrId::STR_DIR_UP;  // shown after the page glyph; unused when not declared
};

// What a list screen states once: its Left/Right pair, whether long Confirm has an action of its
// own, whether long Up/Down switch tabs.
struct ListDeclaration {
  ListAction left;
  ListAction right;
  bool confirmLong = false;
  bool tabbed = false;
};

// Runs one list's buttons, hint strips and touch through ListGrammar, so every list answers the
// same way (docs/superpowers/specs/2026-10-03-list-input-harmonization-design.md). A member of the
// screen, not a base class; it holds no heap and allocates nothing per tick or per render.
class ListController {
 public:
  ListController(MappedInputManager& input, ButtonEventManager& events, ListHost& host,
                 const ListDeclaration& declaration);

  // Call from the screen's loop() while this list is on screen. It is the only reader of button
  // events there; it reads events until the screen acts on one, then leaves the rest queued for
  // the next tick, where the screen's new state takes them in order. Activity transitions drain.
  void update();
  // A vertical swipe over the list (Activity::pageList): -1 back, +1 forward.
  void page(int direction);
  // A tap on a row (Activity::selectListRow).
  ListRowTap::Result tapRow(int row);
  // Draws the bottom and the side hint strips; `backLabel` and `confirmLabel` are the screen's own.
  void drawHints(GfxRenderer& renderer, const char* backLabel, const char* confirmLabel) const;
  // Forgets a hold-to-repeat and a half-made double-tap: call when the list is (re)entered.
  void reset();

 private:
  using Button = MappedInputManager::Button;

  MappedInputManager& input;
  ButtonEventManager& events;
  ListHost& host;
  ListDeclaration declaration;

  // Hold-to-repeat paging after a long Left/Right, while that key stays down.
  bool repeating = false;
  Button repeatButton = Button::Right;
  int8_t repeatDirection = 1;
  unsigned long repeatSinceMs = 0;

  // The last Up/Down tap that stepped, so a second tap close behind it can turn the pair into a
  // page jump. lastTapPressMs 0 means there is no tap to pair with.
  ListGrammar::Key lastTapKey = ListGrammar::Key::Down;
  unsigned long lastTapPressMs = 0;
  int rowBeforeTap = 0;

  ListGrammar::Shape shape() const;
  ListGrammar::Availability availability() const;
  ListGrammar::Rows rows() const;
  static bool keyFor(Button button, ListGrammar::Key& key);
  // Applies one command. False once the screen has acted (it may have left this list): stop
  // reading events until the next tick.
  bool apply(ListGrammar::Result result);
  void moveTo(int row);
  // A page turn to `row`: it becomes the top row on screen, unless that is already so.
  void showPage(int row);
  // A short or double Up/Down press: one step, or a page when it completes a double-tap.
  void tapVertical(ListGrammar::Key key, ListGrammar::Press press, unsigned long pressMs);
  void continuePageRepeat();
};

// The spec's budget for a controller per list screen.
static_assert(sizeof(ListController) <= 100, "ListController must stay under 100 bytes");
```

- [ ] **Step 2: Update `src/activities/ListController.cpp`**

Replace these functions with the versions below. Leave `keyFor`, `update`, `drawHints`, `shape` and the anonymous namespace unchanged.

```cpp
ListController::ListController(MappedInputManager& input, ButtonEventManager& events, ListHost& host,
                               const ListDeclaration& declaration)
    : input(input), events(events), host(host), declaration(declaration) {}

ListGrammar::Availability ListController::availability() const {
  const int selected = host.listSelected();
  ListGrammar::Availability a;
  a.left = declaration.left.declared && host.listActionAvailable(ListGrammar::Side::Left, selected);
  a.right = declaration.right.declared && host.listActionAvailable(ListGrammar::Side::Right, selected);
  return a;
}

ListGrammar::Rows ListController::rows() const {
  const ListWindow window = host.listWindow();
  ListGrammar::Rows r;
  r.count = host.listCount();
  r.top = window.top;
  r.drawn = window.drawn;
  r.selectable = &hostSelectable;
  r.ctx = &host;
  return r;
}

bool ListController::apply(const ListGrammar::Result result) {
  using ListGrammar::Command;
  switch (result.command) {
    case Command::None:
      return true;
    case Command::StepPrev:
    case Command::StepNext: {
      const auto r = rows();
      int row = host.listSelected();
      for (int i = 0; i < result.times; ++i)
        row = ListGrammar::step(r, row, result.command == Command::StepNext ? 1 : -1);
      moveTo(row);
      return true;
    }
    case Command::PagePrev:
    case Command::PageNext:
      showPage(ListGrammar::page(rows(), host.listSelected(), result.command == Command::PageNext ? 1 : -1));
      return true;
    case Command::First:
      moveTo(ListGrammar::first(rows()));
      return true;
    case Command::Last:
      moveTo(ListGrammar::last(rows()));
      return true;
    default:
      break;
  }

  // The screen may leave this list, so stop here and leave later presses queued
  // for the next tick, where the new state takes them in order.
  repeating = false;
  const int selected = host.listSelected();
  switch (result.command) {
    case Command::TabPrev:
      host.onListTab(-1);
      break;
    case Command::TabNext:
      host.onListTab(1);
      break;
    case Command::Activate:
      host.onListActivate(selected, false);
      break;
    case Command::ActivateLong:
      host.onListActivate(selected, true);
      break;
    case Command::Back:
      host.onListBack();
      break;
    case Command::Home:
      host.onListHome();
      break;
    case Command::LeftAction:
      host.onListAction(ListGrammar::Side::Left, selected);
      break;
    case Command::RightAction:
      host.onListAction(ListGrammar::Side::Right, selected);
      break;
    default:
      break;
  }
  return false;
}

void ListController::moveTo(const int row) {
  if (row == host.listSelected()) return;
  host.listSelect(row);
}

void ListController::showPage(const int row) {
  if (row == host.listSelected() && row == host.listWindow().top) return;
  host.listShowAtTop(row);
}

void ListController::tapVertical(const ListGrammar::Key key, const ListGrammar::Press press,
                                 const unsigned long pressMs) {
  const auto r = rows();
  const int selected = host.listSelected();
  const int direction = key == ListGrammar::Key::Down ? 1 : -1;

  // A Double event is a whole double-tap in one: it arrives only when the user bound a double
  // action to the page-turn keys and it fell through to the list.
  if (press == ListGrammar::Press::Double) {
    lastTapPressMs = 0;
    if (ListGrammar::fitsOnePage(r)) {
      moveTo(ListGrammar::step(r, ListGrammar::step(r, selected, direction), direction));
    } else {
      showPage(ListGrammar::page(r, selected, direction));
    }
    return;
  }

  // The first tap of the pair has already stepped. Page from where it started instead, so the pair
  // moves one page, not a page and a row. A third tap starts a new pair.
  if (ListGrammar::completesDoubleTap(key, pressMs, lastTapKey, lastTapPressMs, r)) {
    lastTapPressMs = 0;
    showPage(ListGrammar::page(r, rowBeforeTap, direction));
    return;
  }

  rowBeforeTap = selected;
  lastTapKey = key;
  lastTapPressMs = pressMs;
  moveTo(ListGrammar::step(r, selected, direction));
}

void ListController::continuePageRepeat() {
  if (!repeating) return;
  // An injected long press (a long tap on a hint box) never holds the live level, so it pages once.
  if (!input.isPressed(repeatButton)) {
    repeating = false;
    return;
  }
  const unsigned long now = millis();
  if (now - repeatSinceMs < kPageRepeatMs) return;
  repeatSinceMs = now;
  showPage(ListGrammar::page(rows(), host.listSelected(), repeatDirection));
}

void ListController::page(const int direction) {
  repeating = false;
  lastTapPressMs = 0;
  showPage(ListGrammar::page(rows(), host.listSelected(), direction));
}

ListRowTap::Result ListController::tapRow(const int row) {
  lastTapPressMs = 0;
  const int count = host.listCount();
  if (row >= 0 && row < count && !host.listSelectable(row)) return ListRowTap::Result::Rejected;
  int selected = host.listSelected();
  const auto result = ListRowTap::apply(row, count, selected);
  if (result == ListRowTap::Result::Selected) host.listSelect(selected);
  return result;
}

void ListController::reset() {
  repeating = false;
  lastTapPressMs = 0;
}
```

- [ ] **Step 3: Adapt the OPDS hosts**

In `OpdsBookBrowserActivity.h`:

1. In **both** `CatalogHost` and `FormatHost`:
   - delete the declarations `int listPageRows() const override;` and `void onListSelectionChanged() override;`;
   - add:

```cpp
    ListWindow listWindow() const override;
    int listSelected() const override;
    void listSelect(int row) override;
```

2. Next to the existing `std::atomic<int> catalogPageRows{1};` / `std::atomic<int> formatPageRows{1};`, add:

```cpp
  std::atomic<int> catalogTop{0};
  std::atomic<int> formatTop{0};
```

3. Change the two controller members to drop the selection argument:

```cpp
  ListController catalogList{mappedInput, buttonEvents, catalogHost, kCatalogDeclaration};
  ListController formatList{mappedInput, buttonEvents, formatHost, kFormatDeclaration};
```

In `OpdsBookBrowserActivity.cpp`, first delete the definitions of:
- `CatalogHost::listPageRows`
- `CatalogHost::onListSelectionChanged`
- `FormatHost::listPageRows`
- `FormatHost::onListSelectionChanged`

Then add:

```cpp
ListWindow OpdsBookBrowserActivity::CatalogHost::listWindow() const {
  ListWindow window;
  window.top = browser.catalogTop.load();
  window.drawn = browser.catalogPageRows.load();
  return window;
}

int OpdsBookBrowserActivity::CatalogHost::listSelected() const { return browser.selectorIndex; }

void OpdsBookBrowserActivity::CatalogHost::listSelect(const int row) {
  browser.selectorIndex = row;
  browser.requestUpdate();
}

ListWindow OpdsBookBrowserActivity::FormatHost::listWindow() const {
  ListWindow window;
  window.top = browser.formatTop.load();
  window.drawn = browser.formatPageRows.load();
  return window;
}

int OpdsBookBrowserActivity::FormatHost::listSelected() const { return browser.formatSelectorIndex; }

void OpdsBookBrowserActivity::FormatHost::listSelect(const int row) {
  browser.formatSelectorIndex = row;
  browser.requestUpdate();
}
```

In `render()`:
- in the catalog section, directly after `const auto pageStartIndex = selectorIndex / rowsPerPage * rowsPerPage;`, add `catalogTop.store(pageStartIndex);`;
- in the format-picker section, directly after `const int pageStartIndex = formatSelectorIndex / itemsPerPage * itemsPerPage;`, add `formatTop.store(pageStartIndex);`.

OPDS draws a page at a time, so the default `listShowAtTop` (select) is right for it.

- [ ] **Step 4: Format and build**

```bash
"/c/Program Files/LLVM/bin/clang-format.exe" -i src/activities/ListController.h src/activities/ListController.cpp src/activities/browser/OpdsBookBrowserActivity.h src/activities/browser/OpdsBookBrowserActivity.cpp
grep -rn "listPageRows\|onListSelectionChanged\|pageRows" src/activities/ListController.* src/activities/browser/OpdsBookBrowserActivity.* src/util/ListGrammar.*
```

Expected: no matches. Then build `default` (PowerShell command from "Before you start"). Expected: SUCCESS.

- [ ] **Step 5: Commit**

```bash
git add src/activities/ListController.h src/activities/ListController.cpp src/activities/browser/OpdsBookBrowserActivity.h src/activities/browser/OpdsBookBrowserActivity.cpp
git commit -m "refactor(lists): the controller asks its host for the selection, the window and page turns

ListController no longer holds an int& selection. Its ListHost reports the
selected row, selects one, shows one at the top of the screen (a page turn)
and publishes the window its last render drew. That is what lets ListNav-based
lists, whose selection is an atomic and whose viewport is the render task's,
and the tabbed lists, whose bar sits in front of row 0, use the controller.
OPDS publishes its page start alongside its page size."
```

### Task 3: `UiListActivity` runs its input through the controller

The pickers (Enum, Font, Dictionary, Keyboard layouts, Language) inherit all of it. The file browser keeps its own `navigateButtons()` and `handleCustomInput()` until PR 6.

**Files:**
- Replace: `src/activities/UiListActivity.h` (full file below)
- Modify: `src/activities/UiListActivity.cpp`
- Modify: `src/activities/home/FileBrowserActivity.h` (delete the `handleButtons()` override and its comment, lines ~130-138)
- Modify: `src/activities/settings/{EnumSelection,FontSelection,DictionarySelection,KeyboardLayouts,LanguageSelect}Activity.cpp` (content rect)
- Modify: `src/activities/settings/FontSelectionActivity.cpp` (warm-up reads events)

**Interfaces:**
- Consumes: Task 2's `ListHost`, `ListWindow`, `ListDeclaration`, `ListController`.
- Produces, as protected members of `UiListActivity`, for Tasks 4 and 5:
  - the constructor `UiListActivity(const char*, GfxRenderer&, MappedInputManager&, const ListDeclaration& = {})`;
  - `virtual bool isRowSelectable(int) const`;
  - `virtual const char* footerBackLabel() const`, `virtual const char* footerConfirmLabel() const`;
  - the position hooks `positionCount`, `selectedPosition`, `selectPosition`, `showPositionAtTop`, `positionWindow`, `isPositionSelectable`, `activatePosition`, `backFromPosition`, `switchTab`;
  - the helpers `Rect listContentRect() const`, `void drawListHints()`, `void publishListWindow()`, `ListWindow publishedWindow() const`, `void showRowAtTop(int)`, `void moveSelectionTo(int)`.
- Removed: `virtual bool handleButtons()`.

- [ ] **Step 1: Replace `src/activities/UiListActivity.h`**

```cpp
#pragma once

#include <atomic>
#include <functional>

#include "activities/Activity.h"
#include "activities/ListController.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

enum class SettingAction;
struct Rect;

class UiListActivity : public Activity, protected UiAppHost {
 public:
  void onEnter() override;
  // Stops touch routing before the activity goes away. Every screen here builds its UI in
  // onEnter() (resetUi() re-arms routing), so closing it on the way out is always right --
  // and doing it here means a subclass cannot forget. Two did it by hand; the rest did not.
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // What a tap on a row means. Overriding it here is what puts the FreeInkUI path and the
  // legacy ListTouchBand path on the same rule: onRowAction() used to apply ListRowTap
  // inline, so a subclass could override this (as MenuListActivity does, to decline
  // separators) and never be consulted for a tap it actually receives.
  ListRowTap::Result selectListRow(int index) override;

 protected:
  bool tryOpenSliderFor(SettingAction action, std::function<void()> onDone = {});

  static constexpr freeink::ui::ActionId ACTION_ROW = 1;
  static constexpr freeink::ui::ActionId ACTION_USER = 2;

  // `declaration` is the screen's Left/Right pair and long-press shape (ListController.h); the
  // default is the plain list the pickers and menus use.
  UiListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                 const ListDeclaration& declaration = {});

  virtual int listCount() const = 0;
  virtual void buildScreen(UiScreen& screen) = 0;
  virtual void activateIndex(int index) = 0;
  virtual freeink::ui::ListNav& activeNav() { return nav; }
  virtual bool handleCustomInput() { return false; }
  virtual void onBackButton() { finish(); }
  virtual void onSelectionChanged(int /*index*/) {}
  // False for rows the selection must step over, such as a section separator. Default: all rows.
  [[nodiscard]] virtual bool isRowSelectable(int /*index*/) const { return true; }
  // Runs once the UI has been laid out and drawn, before the footer and the
  // buffer swap. For anything that has to patch the framebuffer using a
  // rectangle only the layout pass knows -- see FontSelectionActivity, which
  // blits a cached preview strip here.
  virtual void afterUiRender() {}
  virtual int indexForActionValue(int16_t value) const { return value; }
  virtual const char* headerTitle() const { return nullptr; }
  virtual void drawChrome();
  virtual void drawFooter();
  // What drawFooter() puts on Back and Confirm. Left/Right and the side boxes come from the list's
  // declaration, through the controller.
  [[nodiscard]] virtual const char* footerBackLabel() const;
  [[nodiscard]] virtual const char* footerConfirmLabel() const;
  // Reads this list's buttons through the controller. The file browser overrides it with its own
  // until it moves to the controller (PR 6).
  virtual void navigateButtons();

  // --- The list as the button scheme sees it: one position per row, unless a subclass puts
  // something in front (TabbedUiListActivity's tab bar is position 0). ---
  [[nodiscard]] virtual int positionCount() const { return listCount(); }
  [[nodiscard]] virtual int selectedPosition() const;
  virtual void selectPosition(int position) { moveSelectionTo(position); }
  virtual void showPositionAtTop(int position) { showRowAtTop(position); }
  [[nodiscard]] virtual ListWindow positionWindow() const { return publishedWindow(); }
  [[nodiscard]] virtual bool isPositionSelectable(int position) const { return isRowSelectable(position); }
  virtual void activatePosition(int position, bool longPress);
  virtual void backFromPosition(int /*position*/) { onBackButton(); }
  virtual void switchTab(int /*direction*/) {}

  // The content area a list lays out in: room for the bottom hints AND the side Up/Down boxes.
  [[nodiscard]] Rect listContentRect() const;
  // Both hint strips for this list, with footerBackLabel() / footerConfirmLabel() on Back / Confirm.
  void drawListHints();
  // Call once a render pass has laid the list out: hands the drawn window to the input side.
  void publishListWindow();
  [[nodiscard]] ListWindow publishedWindow() const;

  void syncListViewport(UiScreen& screen, freeink::ui::ListProps& props, bool hasSubtitle = false);
  void moveSelectionTo(int index);
  // A page turn: select `row` and scroll so it is the top row on screen.
  void showRowAtTop(int row);

  freeink::ui::ListNav nav;
  // Used only by the file browser's own navigateButtons() until it moves to the controller (PR 6).
  ButtonNavigator buttonNavigator;

 private:
  // The ListController's view of this activity: it forwards to the position hooks above.
  struct ControllerHost final : ListHost {
    explicit ControllerHost(UiListActivity& list) : list(list) {}
    int listCount() const override { return list.positionCount(); }
    ListWindow listWindow() const override { return list.positionWindow(); }
    bool listSelectable(const int position) const override { return list.isPositionSelectable(position); }
    int listSelected() const override { return list.selectedPosition(); }
    void listSelect(const int position) override { list.selectPosition(position); }
    void listShowAtTop(const int position) override { list.showPositionAtTop(position); }
    void onListActivate(const int position, const bool longPress) override {
      list.activatePosition(position, longPress);
    }
    void onListBack() override { list.backFromPosition(list.selectedPosition()); }
    void onListHome() override { list.onGoHome(); }
    void onListTab(const int direction) override { list.switchTab(direction); }
    UiListActivity& list;
  };

  static void screenTrampoline(UiScreen& screen, void* user);
  static void rowActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  void onRowAction(int index);
  bool routeListTouch();

  // The window the last render drew (top row, rows drawn), for paging from the loop task.
  std::atomic<int> windowTop{0};
  std::atomic<int> windowDrawn{1};
  ControllerHost controllerHost{*this};
  ListController listController;
};
```

- [ ] **Step 2: Update `src/activities/UiListActivity.cpp`**

1. Add `#include <algorithm>` with the other system includes.

2. Replace the constructor with:

```cpp
UiListActivity::UiListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                               const ListDeclaration& declaration)
    : Activity(name, renderer, mappedInput),
      UiAppHost(renderer),
      listController(mappedInput, buttonEvents, controllerHost, declaration) {}
```

3. In `onEnter()`, directly after `activeNav().reset();`, add:

```cpp
  listController.reset();
  windowTop = 0;
  windowDrawn = 1;
```

4. Delete the whole `UiListActivity::handleButtons()` function.

5. Replace `loop()` and `navigateButtons()` with:

```cpp
void UiListActivity::loop() {
  if (handleCustomInput()) return;
  if (routeListTouch()) return;

  // A swipe is a page, the same page as a long Left/Right (spec R3).
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    listController.page(swipe == MappedInputManager::SwipeDir::Up ? 1 : -1);
    return;
  }

  navigateButtons();
}

void UiListActivity::navigateButtons() { listController.update(); }
```

6. Replace `drawFooter()` with:

```cpp
void UiListActivity::drawFooter() { drawListHints(); }

const char* UiListActivity::footerBackLabel() const { return tr(STR_BACK); }

const char* UiListActivity::footerConfirmLabel() const { return tr(STR_SELECT); }

void UiListActivity::drawListHints() { listController.drawHints(renderer, footerBackLabel(), footerConfirmLabel()); }

Rect UiListActivity::listContentRect() const { return UITheme::getContentRect(renderer, true, true); }

int UiListActivity::selectedPosition() const {
  // activeNav() hands out a mutable reference; reading through it changes nothing.
  return const_cast<UiListActivity*>(this)->activeNav().selected;
}

void UiListActivity::activatePosition(const int position, bool /*longPress*/) {
  if (position >= 0 && position < listCount()) activateIndex(position);
}

void UiListActivity::showRowAtTop(const int row) {
  {
    // `top` belongs to the render task: take the lock rather than write it under a build in flight.
    // The follow flags go too, or the next build would pull the viewport back to a minimal scroll.
    RenderLock lock(*this);
    auto& current = activeNav();
    current.selected = row;
    current.followOnBuild = false;
    current.followPending = false;
    current.top = row;
  }
  onSelectionChanged(row);
  requestUpdate();
}

void UiListActivity::publishListWindow() {
  auto& current = activeNav();
  const int count = listCount();
  const int top = current.top;
  const int drawn = current.drawnRows > 0 ? current.drawnRows : 1;
  windowTop.store(top);
  // A short last screen keeps the last full screen's row count, so paging back from it moves a
  // whole screen rather than only as many rows as the tail had.
  windowDrawn.store(top + drawn >= count ? std::max(drawn, windowDrawn.load()) : drawn);
}

ListWindow UiListActivity::publishedWindow() const {
  ListWindow window;
  window.top = windowTop.load();
  window.drawn = windowDrawn.load();
  return window;
}
```

7. In `render()`, between the rebuild loop and `afterUiRender();`, add `publishListWindow();`.

- [ ] **Step 3: The file browser loses its `handleButtons()` override**

In `src/activities/home/FileBrowserActivity.h`, delete the comment block that starts `// Back and Confirm belong to handleCustomInput() alone,` and the line `bool handleButtons() override { return false; }` below it. The base no longer has the hook, and the file browser already reads Back and Confirm only as events in `handleCustomInput()`.

- [ ] **Step 4: The pickers lay out with the side gutter**

In each of the five files listed below, inside `buildScreen()`, replace `UITheme::getContentRect(renderer, true, false)` with `listContentRect()`:
- `EnumSelectionActivity.cpp`
- `FontSelectionActivity.cpp`
- `DictionarySelectionActivity.cpp`
- `KeyboardLayoutsActivity.cpp`
- `LanguageSelectActivity.cpp`

- [ ] **Step 5: Build**

Format the touched files, then build `default`. Expected: SUCCESS. If a subclass fails with "marked override but does not override" for `handleButtons`, delete that override; it existed only in the file browser.

- [ ] **Step 6: The font picker's warm-up reads events**

In `src/activities/settings/FontSelectionActivity.cpp`, `handleCustomInput()`, replace:

```cpp
  if (!warmupActive) return false;
  // Back abandons the warm-up; the remaining families then build lazily, one
  // per cursor move, exactly as they would without it.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    LOG_DBG("FPRV", "Warm-up cancelled after %u of %u", static_cast<unsigned>(warmupDone),
            static_cast<unsigned>(warmupQueue.size()));
    finishWarmup();
    return true;
  }
  advanceWarmup();
  return true;  // the warm-up owns the screen until it finishes
```

with:

```cpp
  if (!warmupActive) return false;
  // Back abandons the warm-up; the remaining families then build lazily, one per cursor move,
  // exactly as they would without it. Read as an event like every other press on this list: a
  // Back read here by level would reach the list afterwards as an event and close the screen.
  // Other presses are dropped while the warm-up owns the screen.
  ButtonEventManager::ButtonEvent event;
  while (buttonEvents.consumeEvent(event)) {
    if (event.button == MappedInputManager::Button::Back) {
      LOG_DBG("FPRV", "Warm-up cancelled after %u of %u", static_cast<unsigned>(warmupDone),
              static_cast<unsigned>(warmupQueue.size()));
      finishWarmup();
      return true;
    }
  }
  advanceWarmup();
  return true;  // the warm-up owns the screen until it finishes
```

- [ ] **Step 7: Format, build, commit**

```bash
"/c/Program Files/LLVM/bin/clang-format.exe" -i src/activities/UiListActivity.h src/activities/UiListActivity.cpp src/activities/home/FileBrowserActivity.h src/activities/settings/EnumSelectionActivity.cpp src/activities/settings/FontSelectionActivity.cpp src/activities/settings/DictionarySelectionActivity.cpp src/activities/settings/KeyboardLayoutsActivity.cpp src/activities/settings/LanguageSelectActivity.cpp
```

Build `default`. Expected: SUCCESS. Record the flash.

```bash
git add src/activities/UiListActivity.h src/activities/UiListActivity.cpp src/activities/home/FileBrowserActivity.h src/activities/settings/EnumSelectionActivity.cpp src/activities/settings/FontSelectionActivity.cpp src/activities/settings/DictionarySelectionActivity.cpp src/activities/settings/KeyboardLayoutsActivity.cpp src/activities/settings/LanguageSelectActivity.cpp
git commit -m "feat(lists): UiListActivity screens follow the list button scheme

The pickers (enum, font, dictionary, keyboard layouts, language) now read
their buttons through the ListController: Up/Down and Left/Right step,
holding Left/Right pages a screenful, holding Up/Down jumps to the ends, a
swipe pages the selection. Both hint strips are drawn, the side Up/Down boxes
included, and the list makes room for them. A page turn sets the viewport
under the render lock; the render publishes the window it drew. The font
picker's warm-up now reads Back as an event, so the Back that ends it cannot
also close the screen. The file browser keeps its own input until PR 6."
```

- [ ] **Device checklist for this task** (run with Task 6's build; listed here so the reviewer can check the code against it):
  1. **3.1** Language picker: Up/Down and Left/Right step. A double-tap of Down jumps a screen. Hold Down: last row. Hold Up: first row. Back leaves without changing the language.
  2. **3.2** Timezone (86-row enum): hold Right pages a screen and keeps paging, stopping on the last row. Hold Left pages back.
  3. **3.3** On the timezone list's last screen, hold Left once: the previous **full** screen shows.
  4. **3.4** Font picker with missing previews: during the warm-up press Back. The warm-up stops, the list stays open, and a second Back leaves.
  5. **3.5** T5S3/X4 Pro: on a picker the side Up/Down boxes are visible and tappable. A swipe up pages, and the selection moves with the page.
  6. **3.6** File browser: unchanged behaviour. Left/Right page, a long Right gives Options, a swipe pages (now moving the selection).

### Task 4: `MenuListActivity` and its seven screens

**Files:**
- Modify: `src/activities/MenuListActivity.h` and `.cpp`
- Modify: `src/activities/home/HomeMoreActivity.cpp`, `src/activities/home/FileContextMenuActivity.cpp`, `src/activities/settings/SettingsSubmenuActivity.cpp`, `src/activities/settings/ClockSettingsActivity.cpp`, `src/activities/settings/KOReaderSettingsActivity.cpp`, `src/activities/reader/QuickOverridesActivity.cpp`, `src/activities/weather/WeatherSettingsActivity.cpp` (`render()` hints and content rect; Weather also `loop()`)

**Interfaces:**
- Consumes: Task 3's `isRowSelectable`, `listContentRect()`, `drawListHints()`, `publishListWindow()`, and `navigateButtons()` defaulting to the controller.
- Produces: `MenuListActivity::isRowSelectable(int) const override`. `handleNavigation()` is removed.

- [ ] **Step 1: `MenuListActivity.h`**

Delete the declarations of `handleNavigation()` (and its comment) and `navigateButtons() override`. Add in the protected section:

```cpp
  // Separators are not rows the selection can rest on.
  [[nodiscard]] bool isRowSelectable(int index) const override;
```

- [ ] **Step 2: `MenuListActivity.cpp`**

Replace `initMenuList()` with:

```cpp
void MenuListActivity::initMenuList() {
  // Never open on a separator: step on to the next row that can hold the selection.
  const int count = static_cast<int>(menuItems.size());
  for (int tried = 0; tried < count && !isRowSelectable(selectedIndex); ++tried) {
    selectedIndex = (selectedIndex + 1) % count;
  }
}

bool MenuListActivity::isRowSelectable(const int index) const {
  return index >= 0 && index < static_cast<int>(menuItems.size()) && !menuItems[index].isSeparator;
}
```

Delete `handleNavigation()` and `navigateButtons()`.

In `drawMenuList()`, after the rebuild `for` loop, add `publishListWindow();`.

- [ ] **Step 3: The six plain menu screens**

In the `render()` of each file below:
1. Replace `UITheme::getContentRect(renderer, true, false)` with `listContentRect()`.
2. Replace the two lines `const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));` and `GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);` with `drawListHints();`.

The files:
- `HomeMoreActivity.cpp` (`render`, lines ~37 and ~46-47)
- `SettingsSubmenuActivity.cpp` (~93, ~102-103)
- `ClockSettingsActivity.cpp` (~92, ~122-123)
- `KOReaderSettingsActivity.cpp` (~211, ~220-221)
- `QuickOverridesActivity.cpp` (~242, ~253-254)

In `FileContextMenuActivity.cpp` `render()`:
- Replace `UITheme::getContentRect(renderer, true, true)` with `listContentRect()`.
- Replace the hint block with `drawListHints();`. The block runs from `const auto hints = mappedInput.mapHints(` through `GUI.drawSideButtonHints(renderer, hints.side.up, hints.side.down);`, including any comment lines between.

- [ ] **Step 4: Weather settings, menu branch**

In `WeatherSettingsActivity.cpp`:
1. In `render()`, replace `const Rect contentRect = UITheme::getContentRect(renderer, true, false);` with `const Rect contentRect = listContentRect();`.
2. In the **menu** branch, directly after `drawMenuList(...)`, replace the two hint lines with `drawListHints();`.

The city-results branch keeps its own `mapLabels` hints until PR 5.

- [ ] **Step 5: Weather settings, the results list reads events**

In `WeatherSettingsActivity::loop()`, replace the `if (showingSearchResults) { ... }` block with:

```cpp
  if (showingSearchResults) {
    // This list moves to the ListController in PR 5. Until then its Back and Confirm are read as
    // events, never levels, so neither reaches the settings menu again once the results close;
    // the steps below come from the press log, and their events are dropped here.
    ButtonEventManager::ButtonEvent event;
    while (buttonEvents.consumeEvent(event)) {
      if (event.button == MappedInputManager::Button::Back) {
        showingSearchResults = false;
        requestUpdate();
        return;
      }
      if (event.button == MappedInputManager::Button::Confirm) {
        if (resultIndex >= 0 && resultIndex < static_cast<int>(searchResults.size())) {
          const auto& result = searchResults[resultIndex];
          WEATHER_SETTINGS.setLocation(result.latitude, result.longitude, result.name + ", " + result.country);
          WEATHER_SETTINGS.saveToFile();
          showingSearchResults = false;
          requestUpdate();
        }
        return;
      }
    }

    resultsNavigator.onNextList(resultIndex, static_cast<int>(searchResults.size()), [this] { requestUpdate(); });
    resultsNavigator.onPreviousList(resultIndex, static_cast<int>(searchResults.size()), [this] { requestUpdate(); });
    return;
  }
```

- [ ] **Step 6: Format, build, commit**

```bash
for f in src/activities/MenuListActivity.h src/activities/MenuListActivity.cpp src/activities/home/HomeMoreActivity.cpp src/activities/home/FileContextMenuActivity.cpp src/activities/settings/SettingsSubmenuActivity.cpp src/activities/settings/ClockSettingsActivity.cpp src/activities/settings/KOReaderSettingsActivity.cpp src/activities/reader/QuickOverridesActivity.cpp src/activities/weather/WeatherSettingsActivity.cpp; do "/c/Program Files/LLVM/bin/clang-format.exe" -i "$f"; done
grep -rn "handleNavigation\|mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP)" src/activities/MenuListActivity.* src/activities/home/HomeMoreActivity.cpp src/activities/home/FileContextMenuActivity.cpp src/activities/settings/SettingsSubmenuActivity.cpp src/activities/settings/ClockSettingsActivity.cpp src/activities/settings/KOReaderSettingsActivity.cpp src/activities/reader/QuickOverridesActivity.cpp
```

Expected: no matches. (Weather's results branch is not in that list on purpose.) Build `default`. Expected: SUCCESS.

```bash
git add src/activities/MenuListActivity.h src/activities/MenuListActivity.cpp src/activities/home/HomeMoreActivity.cpp src/activities/home/FileContextMenuActivity.cpp src/activities/settings/SettingsSubmenuActivity.cpp src/activities/settings/ClockSettingsActivity.cpp src/activities/settings/KOReaderSettingsActivity.cpp src/activities/reader/QuickOverridesActivity.cpp src/activities/weather/WeatherSettingsActivity.cpp
git commit -m "feat(lists): the menu lists follow the list button scheme

MenuListActivity drops its own navigation: settings submenus, the file
options menu, Home's More, clock, KOReader, quick overrides and the weather
menu now step on Left/Right as well as Up/Down (Left/Right used to page under
'Up'/'Down' labels), page on a held Left/Right and skip separators everywhere.
Both hint strips are drawn and the lists make room for the side boxes. The
render no longer runs nav.follow() from the loop task. The weather city
results read Back and Confirm as events until they move in PR 5."
```

- [ ] **Device checklist for this task:**
  1. **4.1** A settings submenu longer than one screen: Right steps one row (it used to page), and hold Right pages.
  2. **4.2** On a menu whose rows wrap to two lines, hold Right repeatedly: no row is skipped between screens, and the selection lands on the top row.
  3. **4.3** A submenu with section headers: step, page and hold Up/Down never land on a header.
  4. **4.4** File options menu: Back still commits display-option changes. Quick overrides: Back still applies the overrides.
  5. **4.5** Weather settings → city search → results: Back returns to the weather menu and stays there (no second Back). Confirm on a result sets it.

### Task 5: `TabbedUiListActivity`: the tab bar as position 0, per-tab memory; Settings and the reader menu

**Files:**
- Replace: `src/activities/TabbedUiListActivity.h` and `.cpp` (full files below)
- Modify: `src/activities/settings/SettingsActivity.h` and `.cpp`
- Modify: `src/activities/reader/EpubReaderMenuActivity.h` and `.cpp`

**Interfaces:**
- Consumes: Task 3's position hooks, `showRowAtTop`, `publishedWindow`, `moveSelectionTo`, `listContentRect`, `publishListWindow`, `footerConfirmLabel`, `isRowSelectable`, the `ListDeclaration` constructor argument.
- Produces: `TabbedUiListActivity` with `selectTab(int)`, `focusTabs()`, `tabsFocused()`, `selectedTab()`, `buildTabBar(UiScreen&)`, per-tab `activeNav()`.
- Removed: `handleButtons()`, `navigateButtons()`, `stepSelection()`, and the `isRowSelectable` declaration (now in `UiListActivity`).

- [ ] **Step 1: Replace `src/activities/TabbedUiListActivity.h`**

```cpp
#pragma once

#include <vector>

#include "activities/UiListActivity.h"

// A UiListActivity whose list is split across a row of tabs at the top of the screen.
//
//   * The tab bar is a focus position ABOVE row 0. The list button scheme sees it as position 0 and
//     row i as position i + 1; activeNav().selected == -1 means the bar holds focus.
//   * Up/Down step through bar and rows as one range; a long Up/Down switches to the previous / next
//     tab (spec R4). Left/Right step too, and a long Left/Right pages.
//   * Confirm on the bar advances to the next tab; on a row it activates the row.
//   * Back on a row returns focus to the bar; on the bar it leaves (onBackFromTabs()).
//   * A tap on a tab selects it; a tap on a row follows the usual ListRowTap rule.
//   * Each tab keeps its own ListNav, so it remembers its row and scroll position. A switch made
//     from the bar (Confirm, a tap, a long press) stays on the bar; a long Up/Down from a row lands
//     on the new tab's remembered row, or its bar if it has none.
//
// Per-tab ListNav storage adapted from upstream crosspoint-reader's UiTabListActivity
// (develop @ cdac66ffe, src/activities/UiTabListActivity.cpp).
class TabbedUiListActivity : public UiListActivity {
 protected:
  TabbedUiListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput);

  // Most tabs any one screen may show. Only bounds the stack array buildTabBar() composes from.
  static constexpr int MAX_TABS = 8;

  // --- supplied by the subclass ---------------------------------------------------------
  [[nodiscard]] virtual int tabCount() const = 0;
  [[nodiscard]] virtual const char* tabLabel(int slot) const = 0;
  // The selected tab changed: point listCount()/buildScreen() at the new tab's rows. Called
  // before the repaint.
  virtual void onTabSelected(int /*slot*/) {}
  // Back pressed while the bar holds focus. Default leaves the screen.
  virtual void onBackFromTabs() { finish(); }
  // Height the bar takes off the top of the screen.
  [[nodiscard]] virtual int16_t tabBarHeight() const { return 54; }
  // Last word on the composed props -- text style, icons, layout. The focus-dependent selected
  // styling is set before this runs and is meant to survive it.
  virtual void customizeTabBar(UiScreen& /*screen*/, freeink::ui::TabBarProps& /*props*/) {}

  // --- provided to the subclass ---------------------------------------------------------
  [[nodiscard]] int selectedTab() const { return selectedTabSlot; }
  // True while the bar, rather than a row, holds focus.
  [[nodiscard]] bool tabsFocused() { return activeNav().selected < 0; }
  // Move to a tab with focus on its bar (a tap on a tab, Confirm on the bar). Out-of-range slots
  // are ignored.
  void selectTab(int slot);
  // Take focus off the rows and back to the bar.
  void focusTabs();
  // Compose the bar and consume it from the top of the screen. Call FIRST from buildScreen(),
  // before laying out the list.
  void buildTabBar(UiScreen& screen);

  void onEnter() override;
  freeink::ui::ListNav& activeNav() override;
  ListRowTap::Result selectListRow(int index) override;

  // The bar is position 0, row i is position i + 1.
  [[nodiscard]] int positionCount() const override { return listCount() + 1; }
  [[nodiscard]] int selectedPosition() const override;
  void selectPosition(int position) override;
  void showPositionAtTop(int position) override;
  [[nodiscard]] ListWindow positionWindow() const override;
  [[nodiscard]] bool isPositionSelectable(int position) const override;
  void activatePosition(int position, bool longPress) override;
  void backFromPosition(int position) override;
  void switchTab(int direction) override;

 private:
  static void tabActionTrampoline(const freeink::ui::ActionEvent& event, void* user);
  // Make `slot` the active tab; `barFocus` puts the focus on its bar, otherwise the tab opens
  // where it was left.
  void enterTab(int slot, bool barFocus);

  int selectedTabSlot = 0;
  std::vector<freeink::ui::ListNav> tabNavs;
};
```

- [ ] **Step 2: Replace `src/activities/TabbedUiListActivity.cpp`**

Keep `buildTabBar()` exactly as it is today; copy its body over unchanged. The rest:

```cpp
#include "TabbedUiListActivity.h"

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"

namespace fui = freeink::ui;

namespace {
// A tabbed list switches tabs on a long Up/Down instead of jumping to its ends (spec R4).
ListDeclaration tabbedDeclaration() {
  ListDeclaration declaration;
  declaration.tabbed = true;
  return declaration;
}
}  // namespace

TabbedUiListActivity::TabbedUiListActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity(name, renderer, mappedInput, tabbedDeclaration()) {}

void TabbedUiListActivity::onEnter() {
  // One ListNav per tab, so each remembers its own row and scroll position; all open on the bar.
  // Sized before the base onEnter(), which resets the active one.
  tabNavs.assign(static_cast<size_t>(std::max(1, tabCount())), fui::ListNav{});
  for (auto& tab : tabNavs) tab.reset(-1);
  UiListActivity::onEnter();
  app.on(ACTION_USER, &TabbedUiListActivity::tabActionTrampoline, this);
  // Open on the bar, not on a row: the first thing a reader does here is pick a tab, and
  // starting with row 0 highlighted invites a Confirm that toggles a setting they never chose.
  activeNav().reset(-1);
}

fui::ListNav& TabbedUiListActivity::activeNav() {
  if (tabNavs.empty()) return nav;  // before onEnter() sized them
  const auto slot = static_cast<size_t>(selectedTabSlot);
  return tabNavs[slot < tabNavs.size() ? slot : 0];
}

void TabbedUiListActivity::tabActionTrampoline(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<TabbedUiListActivity*>(user);
  self->app.clearTapFlash();
  self->selectTab(event.value);
}

void TabbedUiListActivity::selectTab(const int slot) { enterTab(slot, true); }

void TabbedUiListActivity::enterTab(const int slot, const bool barFocus) {
  if (slot < 0 || slot >= tabCount()) return;
  selectedTabSlot = slot;
  onTabSelected(slot);
  if (barFocus) {
    focusTabs();
    return;
  }
  listTapActivation.reset();
  requestUpdate();
}

void TabbedUiListActivity::focusTabs() {
  activeNav().selected = -1;
  listTapActivation.reset();
  requestUpdate();
}

// buildTabBar(): unchanged, copied from the current file.

int TabbedUiListActivity::selectedPosition() const {
  // activeNav() hands out a mutable reference; reading through it changes nothing.
  return const_cast<TabbedUiListActivity*>(this)->activeNav().selected + 1;
}

void TabbedUiListActivity::selectPosition(const int position) {
  // requestSelection() rather than writing the viewport here: `top` belongs to the render task,
  // and the deferred pull resolves it in syncToProps -- including the bar (selection -1), which
  // snaps the viewport to the top.
  activeNav().requestSelection(position - 1);
  if (position > 0) onSelectionChanged(position - 1);
  listTapActivation.reset();
  requestUpdate();
}

void TabbedUiListActivity::showPositionAtTop(const int position) {
  if (position <= 0) {
    selectPosition(0);
    return;
  }
  showRowAtTop(position - 1);
  listTapActivation.reset();
}

ListWindow TabbedUiListActivity::positionWindow() const {
  const ListWindow rows = publishedWindow();
  ListWindow window;
  // The bar sits in front of row 0, so the first screen shows it as well.
  window.top = rows.top == 0 ? 0 : rows.top + 1;
  window.drawn = rows.top == 0 ? rows.drawn + 1 : rows.drawn;
  return window;
}

bool TabbedUiListActivity::isPositionSelectable(const int position) const {
  return position == 0 || isRowSelectable(position - 1);
}

void TabbedUiListActivity::activatePosition(const int position, bool /*longPress*/) {
  if (position == 0) {
    // Confirm on the bar moves to the next tab.
    selectTab((selectedTabSlot + 1) % std::max(1, tabCount()));
    return;
  }
  if (position - 1 < listCount()) activateIndex(position - 1);
}

void TabbedUiListActivity::backFromPosition(const int position) {
  if (position == 0) {
    onBackFromTabs();
  } else {
    focusTabs();
  }
}

void TabbedUiListActivity::switchTab(const int direction) {
  const int count = tabCount();
  if (count <= 0) return;
  enterTab(((selectedTabSlot + direction) % count + count) % count, tabsFocused());
}

ListRowTap::Result TabbedUiListActivity::selectListRow(const int index) {
  if (index >= 0 && index < listCount() && !isRowSelectable(index)) return ListRowTap::Result::Rejected;
  return ListRowTap::apply(index, listCount(), activeNav().selected);
}
```

- [ ] **Step 3: `SettingsActivity`**

In `SettingsActivity.h`, replace `void drawFooter() override;` with:

```cpp
  [[nodiscard]] const char* footerConfirmLabel() const override;
```

Keep the `isRowSelectable(...) const override;` declaration as is; it now overrides `UiListActivity`'s.

In `SettingsActivity.cpp`:

1. In `enterCategory()`, delete the comment `// -1 is the tab bar: switching category always hands focus back to the bar, ...` (two lines) and the line `nav.reset(-1);`. Per-tab memory now lives in the base, which focuses the bar on a tab switch from the bar.
2. Replace the whole `drawFooter()` function with:

```cpp
const char* SettingsActivity::footerConfirmLabel() const {
  // Confirm means "next tab" while the bar holds focus and "toggle" on a row, so the hint names
  // the category it would move to rather than a generic label.
  return selectedPosition() == 0 ? I18N.get(categoryNames[(selectedTab() + 1) % categoryCount]) : tr(STR_TOGGLE);
}
```

3. In `render()`:
   - replace `nav.consumeRebuildNeeded()` with `activeNav().consumeRebuildNeeded()`;
   - directly before `drawFooter();`, add `publishListWindow();`.
4. In `buildScreen()`, replace `UITheme::getContentRect(renderer, true, false)` with `listContentRect()`. Leave `drawChrome()`'s rect alone: it only sizes the header.
5. Search the file for any remaining bare `nav.` and change each to `activeNav().`.
6. In the comment inside `activateIndex()` that says `Confirm path in UiListActivity::handleButtons() calls activateIndex() and`, replace `UiListActivity::handleButtons()` with `the list controller`.

- [ ] **Step 4: `EpubReaderMenuActivity`**

In `EpubReaderMenuActivity.h`:
- delete the line `freeink::ui::ListNav& activeNav() override { return tabNav[activeTabIndex()]; }`;
- delete the member `std::array<freeink::ui::ListNav, MENU_TAB_COUNT> tabNav;`;
- replace `void drawFooter() override;` with `[[nodiscard]] const char* footerConfirmLabel() const override;`.

In `EpubReaderMenuActivity.cpp`:

1. In `onEnter()`, delete the two comment lines and the line `for (auto& tab : tabNav) tab.reset(-1);`. The base now gives every tab its own nav, opening on the bar.
2. Replace `drawFooter()` with:

```cpp
const char* EpubReaderMenuActivity::footerConfirmLabel() const {
  // Confirm means "next tab" while the bar holds focus and "select" on a row, so the hint names
  // the tab it would move to, as the settings screen does.
  return selectedPosition() == 0 ? tabLabel((selectedTab() + 1) % tabCount()) : tr(STR_SELECT);
}
```

3. In `buildScreen()`, replace `UITheme::getContentRect(renderer, true, false)` with `listContentRect()`. Leave the second use, in the progress-line drawing, unchanged.

- [ ] **Step 5: Format, build, commit**

```bash
for f in src/activities/TabbedUiListActivity.h src/activities/TabbedUiListActivity.cpp src/activities/settings/SettingsActivity.h src/activities/settings/SettingsActivity.cpp src/activities/reader/EpubReaderMenuActivity.h src/activities/reader/EpubReaderMenuActivity.cpp; do "/c/Program Files/LLVM/bin/clang-format.exe" -i "$f"; done
grep -n "tabNav\b\|tabNav\[\|handleButtons\|stepSelection\|nav\.reset(-1)" src/activities/TabbedUiListActivity.* src/activities/settings/SettingsActivity.* src/activities/reader/EpubReaderMenuActivity.*
```

Expected: no matches. Build `default`. Expected: SUCCESS.

```bash
git add src/activities/TabbedUiListActivity.h src/activities/TabbedUiListActivity.cpp src/activities/settings/SettingsActivity.h src/activities/settings/SettingsActivity.cpp src/activities/reader/EpubReaderMenuActivity.h src/activities/reader/EpubReaderMenuActivity.cpp
git commit -m "feat(lists): Settings and the reader menu follow the list button scheme, with per-tab memory

TabbedUiListActivity runs through the ListController with the tab bar as
position 0: Up/Down walk bar and rows, a long Up/Down switches tab, Left/Right
step and page on a hold, Confirm on the bar moves to the next tab and Back on
a row returns to the bar, as before. Each tab now keeps its own ListNav, so a
tab remembers its row and scroll position (Settings forgot it on every switch);
per-tab storage adapted from upstream crosspoint-reader's UiTabListActivity
(develop @ cdac66ffe). Both screens draw the side Up/Down hints."
```

- [ ] **Device checklist for this task:**
  1. **5.1** Settings opens on the tab bar. Confirm cycles tabs with the hint naming the next tab. Down enters the first row.
  2. **5.2** Settings, on a row: hold Down switches to the next tab and lands on its remembered row (the bar the first time). Hold Up switches back to the row you left.
  3. **5.3** Settings: Back from a row returns to the bar. Back on the bar saves and goes Home.
  4. **5.4** Settings: Up from the first row lands on the bar. Up on the bar wraps to the last row.
  5. **5.5** Reader menu, from a book: the same tab rules. Back on the bar returns to the book with the overrides applied.
  6. **5.6** Settings → Display: rows with section headers. Hold Right pages, and no header is ever selected.

### Task 6: Docs, measurements and the full suite

**Files:**
- Modify: `USER_GUIDE.md` ("Moving through lists", §3.7 intro, §4 *System Navigation*)
- Modify: `RELEASE_NOTES.md` (`## Unreleased`)

- [ ] **Step 1: User guide**

1. In "Moving through lists", replace:

```markdown
The firmware's lists are moving to one set of buttons, a screen at a time. So far the **OPDS
catalog** works this way; other lists still behave as their own sections describe.
```

with:

```markdown
The firmware's lists are moving to one set of buttons, a screen at a time. So far these work this
way: the **OPDS catalog**, **Settings** and the **reader menu**, the option lists and pickers they
open (fonts, dictionaries, language, keyboard layouts, long option lists such as the time zone),
and the menus (file options, Home's More, clock, KOReader sync, weather, quick overrides). Other
lists still behave as their own sections describe.
```

2. In the same section's table, replace the Up/Down row with:

```markdown
| **Up / Down** | Move one row; tap twice quickly to jump a page | Jump to the first / last row (in Settings and the reader menu: switch to the previous / next tab) |
```

3. Under `### 3.7 Settings`, after the line `The Settings screen allows you to configure the device's behavior.`, add:

```markdown

The settings are grouped into tabs. The screen opens on the tab bar: **Confirm** moves to the next
tab, **Down** enters its list, and **Back** on a row returns to the bar (on the bar it saves and
returns Home). From anywhere in a list, **hold Up / Down** to switch to the previous / next tab;
each tab remembers where you were. Moving within a tab follows
**[Moving through lists](#moving-through-lists)**.
```

4. In §4 *System Navigation*, in the **Reader Menu** bullet, replace `While the tab bar is selected, **Confirm** moves to the next tab, and its button hint names that tab.` with `While the tab bar is selected, **Confirm** moves to the next tab, and its button hint names that tab. From anywhere in the list, **hold Up / Down** to switch tabs; each tab remembers where you were.`

- [ ] **Step 2: Release notes**

Under `## Unreleased`, add a `### Lists` heading if absent, then:

```markdown
- **Settings, the reader menu, the pickers and the menus move the same way as the OPDS catalog.** Up/Down and Left/Right move one row; hold Left/Right to page a screenful (Left/Right used to page at once under "Up" / "Down" labels in the menus); hold Up/Down to jump to the first / last row, or in Settings and the reader menu to switch tabs; tap Up or Down twice quickly to jump a page. A swipe now moves the selection with the page. The side Up/Down hints appear on all of these lists, so the T5 S3 can always move up.
- **Each tab in Settings and the reader menu remembers where you were**, so switching tabs and back no longer starts you at the top.
- **Fix: Back during the font previews' first-time preparation no longer also closes the font list**, and Back from weather city results no longer also leaves the weather settings.
```

- [ ] **Step 3: Full host suite, flash, commit**

```bash
cmake --build test/build -j 8 -- -k 0 > /tmp/hb.log 2>&1; grep FAILED /tmp/hb.log | grep -v epub_build_inventory
ctest --test-dir test/build -j 8 2>&1 | grep -E "tests passed|tests failed"
```

Expected: `100% tests passed out of 1170` (1168 − 7 + 9). Build `default` and record Flash/RAM against the baseline (6229455 B / 58264 B).

```bash
git add USER_GUIDE.md RELEASE_NOTES.md
git commit -m "docs: Settings, menus and pickers follow the list button scheme

Moving through lists names the screens that now follow it; Settings and the
reader menu document tabs on a long Up/Down and per-tab memory; release notes
for the behaviour changes and the two Back fixes."
```

- [ ] **Step 4: PR description inputs** (for the controller, not committed)

Collect the `default` Flash delta.
- **If it is not negative,** list the benefits this PR buys (spec §8):
  - the side Up/Down hints on ~15 screens;
  - per-tab memory;
  - event-based input on every list base, which removes the press-edge/Short double-dispatch class (memory note *UiListActivity double dispatch*);
  - page-as-screenful;
  - the two Back fixes.
- **Also include** the device checklists 3.x, 4.x and 5.x above.
