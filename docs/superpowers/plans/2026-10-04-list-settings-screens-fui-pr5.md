# List PR 5: the settings and network list screens onto FreeInkUI and the controller — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the eight screens that still draw their rows with the theme's `GUI.drawList` and read buttons themselves onto `UiListActivity`. Each then renders with FreeInkUI's `list()` and takes input through `ListController`. The screens:

- network mode
- OPDS server list
- OPDS server settings
- status bar settings
- weather city results
- finished book
- reading-stats book list
- font download

**Architecture:**

- Each screen becomes a `UiListActivity` subclass:
  - `listCount` / `buildScreen` / `activateIndex`, plus the optional hooks;
  - its own `loop()`, `render()`, `ButtonNavigator` and `selectListRow` are deleted.
- **Upstream ports:** where upstream crosspoint-reader has a FreeInkUI version (develop @ cdac66ffe), the row building is ported from it with attribution. Our features win where upstream lacks them.
- **Rows:**
  - fixed `ListItem` arrays for small fixed lists;
  - `rowProvider` for lists built from loaded data (no per-render string copies).
- **Non-list states** (download progress, error, complete) read button events in `handleCustomInput()`. Blocking calls drain the event queue afterwards.

**Tech Stack:** C++17 / ESP32 (C3 + S3), PlatformIO, FreeInkUI (`freeink-sdk/libs/ui/FreeInkUI`), GoogleTest host suite (`test/`).

**Spec:** `docs/superpowers/specs/2026-10-03-list-input-harmonization-design.md` — §1 (the scheme, R3 as revised), §2b (rendering, upstream hybrid, rows on demand), §3 (screen mapping), §6 item 5, §8 (costs).

## Global Constraints

### The scheme

The bases already implement it; screens never re-implement it.

- **Up/Down:**
  - Step; two quick taps jump a page.
  - Hold: first / last row.
- **Left/Right:**
  - Short: step. No screen in this PR declares a pair.
  - Hold: page. A page turn moves the screen by the drawn window and keeps the selection on its line.
- **Confirm:** short opens. Long Confirm does the same, since no screen here declares `confirmLong`.
- **Back:**
  - Short: `onBackButton()`.
  - Long: `homeFromList()` — Back while a reader is on the stack, otherwise Home.
  - A screen whose Back commits something overrides `homeFromList()` so Home never discards it.
- **Touch:**
  - Swipe pages.
  - Row taps follow the user's tap setting (the base's `selectListRow`).
  - The hint boxes are buttons: tap = press, long tap = hold.

### Layout

- All layout derives from `listContentRect()`: both hint strips are reserved.
- Headers use `listHeaderRect()` or the base `drawChrome()`.
- Subtitle lists call `syncListViewport(screen, props, /*hasSubtitle=*/true)`.

### Rows

- `rowProvider` for loaded or long lists. Fixed member arrays only for ≤ 12 fixed rows.
- No `std::vector<std::string>` of row copies; no `std::function`.
- Nothing allocated per tick or per render.
- Row sources the render task reads are mutated on the loop task only under `RenderLock`.

### Input

- Every state of a migrated screen reads button **events**, never levels.
- Blocking calls are followed by `buttonEvents.drain()`.

### Upstream ports

- Attribution: a source comment and a commit-message line, `Adapted from upstream crosspoint-reader's <File> (develop @ cdac66ffe, <path>)`.
- Not ported:
  - `OptionPopup`: it reads levels and costs about 300 lines of flash, so Confirm keeps cycling values.
  - Upstream's FontDownload groups, string arena, mandatory CRC and abort-deletes.
  - Any upstream input code.

### Flash

- Each task records its `default` delta against the PR 4 head: Flash 6,228,461 B / RAM 58,280 B.
- The PR must be flash-negative (§8). The guaranteed saving is the `std::function` row thunks and the per-screen `loop()`/`render()` bodies.
- `BaseTheme::drawList` / `LyraTheme::drawList` stay until their last callers move (PRs 6–7).

### Heap

- `UiAppHost` (`FreeInkApp<24,6>` + `ThemeTokens`) joins each migrated activity's heap object.
- FontDownload's download path is TLS-heap-sensitive, so Task 8 carries a device heap gate with a revert fallback.

### Strings

- `tr(STR_*)` only. No new strings are planned.

### Builds

- `default` per task: PowerShell, `$env:PLATFORMIO_CORE_DIR = 'C:\pio'; & C:\pio\penv\Scripts\pio.exe run -e default`. Never clean.
- The host suite runs in Git Bash.
- The controller builds `x4pro` and `lilygo_t5s3` at the end.
- clang-format every touched C++ file.

### Docs

`USER_GUIDE.md` and `RELEASE_NOTES.md` are updated in Task 9.

## Review Focus

1. **A press made during a blocking call** (font download in progress, manifest fetch, Wi-Fi hand-off) replaying into the list afterwards.
   - Expected: Back during a download cancels it once, and the family list then reacts to nothing stale.
   - Pinned by Task 8's event pump and `drain()` steps, and by device item 8.x "Back during a download".
2. **A row source changing under a render in flight:**
   - the OPDS store reload after the editor returns or a server is deleted;
   - `editServer` strings written by keyboard results;
   - font families swapped in from SD;
   - the reading-stats row cache refilled.
   - Expected: no stale or garbled row and no crash.
   - Pinned by the `RenderLock` steps in Tasks 3, 4, 7 and 8.
3. **Reading-stats rows outrunning their cache** after a page turn or a two-tap jump on a long history.
   - Expected: rows draw their titles, never "…" left on screen.
   - Pinned by the host tests for the pure cache-range function (Task 7) and device item 7.x.
4. **Long Back on a screen whose Back commits something** (font download after a delete, OPDS editor, finished book).
   - Expected: nothing is lost, and the finished-book flow still marks the book finished.
   - Pinned by each task's `homeFromList()` decision and its device item.
5. **Landscape and the side hint strip** on screens with their own chrome: the status bar preview, the finished-book cover panel, the OPDS editor's hint band.
   - Expected: the strip never covers chrome.
   - Pinned by `listContentRect()`-derived layout in Tasks 2, 4 and 6 and their landscape device items.

---

## Before you start

- [ ] **Branch:** `feat/list-settings-screens-fui`, stacked on PR 4 (`feat/list-bases-on-controller`, PR #387, head `b44579413`). When #387 merges, retarget this PR to `master` before merging it.
- [ ] **Baseline the host suite** (Git Bash):

```bash
cd /c/_development/witchhunt-reader
cmake --build test/build -j 8 -- -k 0 > "$TEMP/hb.log" 2>&1; grep FAILED "$TEMP/hb.log" | grep -v epub_build_inventory
ctest --test-dir test/build -j 8 2>&1 | grep -E "tests passed|tests failed"
```

Expected: `100% tests passed out of 1177`. `epub_build_inventory` fails to build on Windows (`<dlfcn.h>`); that failure is pre-existing.

- [ ] **Baseline `default`** (PowerShell):

```powershell
$env:PLATFORMIO_CORE_DIR = 'C:\pio'; & C:\pio\penv\Scripts\pio.exe run -e default 2>&1 | Select-String -Pattern "^(RAM|Flash):|SUCCESS|FAILED|error"
```

Expected: Flash 6228461 B, RAM 58280 B.

---

## Decisions taken while planning (for the reviewer)

### Kept or settled

- **Network-mode icons stay.** Task 1 adds `networkModeIcons.h`, our five 32 px icons turned upright for FreeInkUI (byte-checked against the `drawIcon` arrays).
  - Cost: +512 B flash. The theme still uses the old arrays on Home.
  - Text-only rows would save the 512 B but lose the icons Lyra shows today.
- **No `OptionPopup`.** The OPDS filename format and the status-bar enum rows keep cycle-on-Confirm.
- **OPDS server picker** keeps today's rows: no upstream "Add Server" row.
  - Long Back there uses the base `homeFromList()`, as every list does.
  - A stale return hint left by `goToBrowserWithSearch()` can make it land in the file browser rather than Home. That ActivityManager quirk predates this PR and is noted for later.
- **OPDS row store:** rebuilt at the start of each build on the render task (pointer assignments, no allocation).
  - The reason: the activity pop path runs the result handler outside the render lock, so rows built only on reload could point into reallocated store strings.

### Behaviour and layout changes

- **OPDS editor popup:** `BaseTheme` gains `PopupShip::Caller`, so a popup drawn in `drawFooter()` ships once with the frame. Upstream's version ships twice, the second time a stale buffer.
- **Finished book:**
  - `homeFromList()` always acts as Back. The screen also opens from the file browser's "mark as read", with no reader below.
  - The next-book cover is decoded once per frame in `afterUiRender()`, not on every rebuild pass.
- **Weather city results:**
  - They become a child list screen, `WeatherCityResultsActivity`. It reports Wi-Fi use.
  - A search with no matches shows `STR_NO_ENTRIES` instead of returning silently.
- **Reading stats:**
  - The row cache follows the selection and the drawn window, chosen by a pure `ListRowCache` function with 12 host tests.
  - Task 7 also overrides `showPositionPage()` to check coverage against the page turn's own top.

### Font download

- **Back during a download** reads events and drains afterwards.
  - On the T5 S3 the Back hint box still cannot cancel a running download (hint taps are dispatched by the loop, which the download holds). This is unchanged from today.
- **Heap gate:** Task 8 carries a device heap gate on an X3/X4.
  - Measured: the contiguous heap at the TLS handshake.
  - Pass: it drops by no more than `sizeof(UiAppHost)` + 1 KB, as logged.
  - If no board is attached, the gate is PENDING and blocks the merge.
  - If it fails, Task 8 is reverted.

---

### Task 1: `NetworkModeSelectionActivity` onto `UiListActivity`

**Files:**
- Create: `src/components/icons/networkModeIcons.h` (full file below)
- Replace: `src/activities/network/NetworkModeSelectionActivity.h` (full file below)
- Replace: `src/activities/network/NetworkModeSelectionActivity.cpp` (full file below)
- Unchanged callers: `src/activities/network/CrossPointWebServerActivity.cpp`. It constructs the activity with `(renderer, mappedInput)` in three places and reads `NetworkModeResult` / `isCancelled`.

**Interfaces:**

- **Consumes** (each checked against `src/activities/UiListActivity.h`):

  | Member | Declaration in the base | Access | Use here |
  |---|---|---|---|
  | ctor | `UiListActivity(const char* name, GfxRenderer&, MappedInputManager&, const ListDeclaration& = {})` | protected | `("NetworkModeSelection", renderer, mappedInput)`, default declaration |
  | `listCount` | `virtual int listCount() const = 0;` | protected | override, inline, returns `MENU_ITEM_COUNT` |
  | `buildScreen` | `virtual void buildScreen(UiScreen& screen) = 0;` | protected | override |
  | `activateIndex` | `virtual void activateIndex(int index) = 0;` | protected | override |
  | `onBackButton` | `virtual void onBackButton() { finish(); }` | protected | override → `onCancel()` |
  | `headerTitle` | `virtual const char* headerTitle() const` | protected | override → `tr(STR_FILE_TRANSFER)`; the base `drawChrome()` draws it at `listHeaderRect()` |
  | `listContentRect` | `[[nodiscard]] Rect listContentRect() const;` | protected | content margins |
  | `syncListViewport` | `void syncListViewport(UiScreen&, freeink::ui::ListProps&, bool hasSubtitle = false);` | protected | called with `/*hasSubtitle=*/true` |
  | `nav` | `freeink::ui::ListNav nav;` (`selected` is `std::atomic<int>`) | protected | `nav.selected = index` in `activateIndex` |
  | `ACTION_ROW` | `static constexpr freeink::ui::ActionId ACTION_ROW = 1;` | protected | `props.action` |
  | `app` | `UiApp app;` (via `protected UiAppHost`) | protected | `app.clearTapFlash()` |

  - Not overridden; the base versions apply: `onEnter()`, `onExit()`, `loop()`, `render()`, `selectListRow()`, `homeFromList()`, `drawFooter()`, `footerBackLabel()` (`STR_BACK`) and `footerConfirmLabel()` (`STR_SELECT`).
  - From `Activity`: `bool usesWifi() const` (public virtual), overridden. Also `setResult()` and `finish()`.
  - `ActivityManager::goToSerialTransfer()` and `goToUsbDrive()` (`ActivityManager.h:145-146`).
  - FreeInkUI fields set, checked against `freeink-sdk/libs/ui/FreeInkUI/include/components/lists/list.h` and `FreeInkUICore.h`:
    - `ListItem`: `label`, `subtitle`, `icon` (a `BitmapRef`), `actionValue` (`int16_t`).
    - `ListProps`: `items`, `count` (`uint16_t`), `action`, `inputMask` (`fui::InputTouch`), `labelText`, `subtitleText` (`TextStyle`, with `.maxLines`).
    - `BitmapRef`: `data`, `width`, `height` (`uint16_t`), `format = BitmapFormat::Mask1`; `progmem` keeps its default of `true`.
    - `Insets{top, right, bottom, left}`.
- **Produces:**
  - The public API is unchanged:
    - the `explicit NetworkModeSelectionActivity(GfxRenderer&, MappedInputManager&)` ctor, now defined out of line;
    - `usesWifi()`;
    - `void onModeSelected(NetworkMode)` and `void onCancel()`;
    - `enum class NetworkMode { JOIN_NETWORK, CONNECT_CALIBRE, CREATE_HOTSPOT, USB_SERIAL, USB_DRIVE }`, byte-identical (`ActivityResult.h:78` forward-declares it).
  - New public member: `static constexpr int MENU_ITEM_COUNT = 4;`.
  - Removed overrides (the base supplies them): `onEnter`, `onExit`, `loop`, `render`, `selectListRow`.
  - New header `components/icons/networkModeIcons.h`. It defines five `static constexpr uint8_t[128]` arrays: `networkModeIconWifi`, `networkModeIconLibrary`, `networkModeIconHotspot`, `networkModeIconUsb` and `networkModeIconTransfer`.
- **Behaviour, preserved:**
  - **Rows:** the same four, in the same order, with the same `StrId` labels and descriptions. The per-board fourth row (`USB_DRIVE` with `FREEINK_CAP_USB_MSC`, else `USB_SERIAL`) keeps the same `#if` block.
  - **Confirm:** `menuModes[index]` gives the same mode the old `selectedIndex` if-chain gave (0→JOIN_NETWORK, 1→CONNECT_CALIBRE, 2→CREATE_HOTSPOT, 3→USB_MODE). `USB_SERIAL` → `goToSerialTransfer()` and `USB_DRIVE` → `goToUsbDrive()`, neither with a result. Otherwise `onModeSelected()` sets `NetworkModeResult` and calls `finish()`.
  - **Back:** `onBackButton()` → `onCancel()`, which returns a cancelled result. The parent's handler calls its `onGoHome()`, as before.
  - **Selection** starts at row 0, because the base `onEnter()` resets `nav` (the old code set `selectedIndex = 0`).
  - **Header:** same title, `STR_FILE_TRANSFER`. **Footer:** Back / Select, as before.
  - **`usesWifi()`** still returns true.
- **Behaviour, deliberate changes:**
  - **Buttons:** the list scheme from the base.
    - Left/Right step.
    - A double tap on Up/Down jumps a page; a long Up/Down goes to the first or last row.
    - A long Left/Right pages.
    - **Long Back** = `homeFromList()`. With no reader on the stack that is this activity's `onGoHome()` → `returnFromChild()`, the same destination the short Back reaches through the parent's cancel handler. With a reader on the stack it is `onBackButton()` → `onCancel()`. So a long Back never lands somewhere a short Back would not.
  - **Hints:** the side Up/Down hint boxes are now drawn (`drawListHints()` via the default `drawFooter()`). The header and the list sit inside `listContentRect()`, so neither runs under the side strip. The old screen used `getContentRect(true, false)`.
  - **Row drawing:** FreeInkUI replaces the theme's `drawList`.
    - Descriptions wrap to two lines in small text instead of being cut to one.
    - Labels are body text, up to two lines (as in our other FUI pickers; upstream leaves the label style unset).
    - The selection look is FUI's.
  - **Icons:** they now show on every theme (Classic drew none) and are upright in every orientation (drawIcon is portrait-only).
  - **Row tap:** handled by the base's `selectListRow()` plus the user's touch-list-activation setting, as on every FUI list. The old override applied `ListRowTap` to `selectedIndex` itself.
  - **Heap:** while the screen is open it holds the base's `FreeInkApp` and 4 `ListItem`s (about 60 B each) instead of a `ButtonNavigator`. All of it is freed when the screen pops, which happens before the parent's result handler turns the radio on (`ActivityManager.cpp`: `exitActivity()` runs before `handler(pendingResult)`).

- [ ] **Step 1: Create `src/components/icons/networkModeIcons.h`**

```cpp
#pragma once
#include <cstdint>

// The File Transfer menu's row icons, as FreeInkUI list rows draw them: 32 x 32, rows top to
// bottom, 4 bytes a row, MSB first, bit 0 = ink (BitmapFormat::Mask1, the Icon.h layout).
//
// They are the pictures in wifi.h, library.h, hotspot.h, usb.h and transfer.h, pixel for pixel.
// Those arrays are stored pre-rotated for GfxRenderer::drawIcon's portrait transform, which a
// BitmapRef cannot take (FreeInkUIGfxRenderer.h, GfxRendererTarget::bitmap): each array here is
// its drawIcon twin turned upright -- logical pixel (x, y) is the drawIcon array's row 31 - x,
// column y. FreeInkUI draws them pixel by pixel through the renderer, so they are upright in every
// orientation. A build keeps only the four its menu lists, 128 bytes each.
static constexpr uint8_t networkModeIconWifi[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC0, 0x03, 0xFF, 0xFF, 0x00, 0x00, 0x7F,
    0xF8, 0x1F, 0xF0, 0x1F, 0xF0, 0xFF, 0xFE, 0x0F, 0xE3, 0xFF, 0xFF, 0x83, 0xC7, 0xFF, 0xFF, 0xE3,
    0xDF, 0xFC, 0x1F, 0xF3, 0xFF, 0xE0, 0x03, 0xFF, 0xFF, 0x01, 0x00, 0xFF, 0xFE, 0x1F, 0xF0, 0x7F,
    0xFC, 0x7F, 0xFC, 0x3F, 0xFC, 0xFF, 0xFF, 0x3F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF8, 0x1F, 0xFF,
    0xFF, 0xE0, 0x07, 0xFF, 0xFF, 0xC3, 0x87, 0xFF, 0xFF, 0xEF, 0xE3, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

static constexpr uint8_t networkModeIconLibrary[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0, 0x01, 0x8F, 0xFF,
    0xE0, 0x00, 0x07, 0xFF, 0xE7, 0x3C, 0x67, 0xFF, 0xE7, 0x3C, 0xE3, 0xFF, 0xE7, 0x3C, 0xF3, 0xFF,
    0xE7, 0x3C, 0xF3, 0xFF, 0xE7, 0x3C, 0x71, 0xFF, 0xE7, 0x3C, 0x79, 0xFF, 0xE7, 0x3C, 0x39, 0xFF,
    0xE7, 0x3C, 0x3C, 0xFF, 0xE7, 0x3C, 0x3C, 0xFF, 0xE7, 0x3C, 0x9C, 0xFF, 0xE7, 0x3C, 0x9E, 0x7F,
    0xE7, 0x3C, 0x9E, 0x7F, 0xE7, 0x3C, 0xCE, 0x7F, 0xE7, 0x3C, 0xCF, 0x3F, 0xE7, 0x3C, 0xCF, 0x3F,
    0xE7, 0x3C, 0xE7, 0x1F, 0xE7, 0x3C, 0xE7, 0x9F, 0xE7, 0x3C, 0xE3, 0x9F, 0xE7, 0x3C, 0xF3, 0xCF,
    0xE7, 0x3C, 0xF3, 0xCF, 0xE7, 0x3C, 0xF1, 0xCF, 0xE7, 0x3C, 0xF9, 0x8F, 0xE0, 0x01, 0xF8, 0x1F,
    0xF0, 0x01, 0xFC, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

static constexpr uint8_t networkModeIconHotspot[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF8, 0xFF, 0xFF, 0x1F, 0xF1, 0xFF, 0xFF, 0x8F,
    0xF1, 0xFF, 0xFF, 0xCF, 0xE3, 0xDF, 0xFF, 0xC7, 0xE7, 0x9F, 0xF9, 0xE7, 0xC7, 0x1F, 0xF8, 0xE3,
    0xCF, 0x3E, 0x7C, 0xF3, 0xCE, 0x38, 0x1C, 0x73, 0xCE, 0x79, 0x9E, 0x73, 0xCE, 0x73, 0xCE, 0x73,
    0xCE, 0x73, 0xCE, 0x73, 0xCE, 0x79, 0x9E, 0x73, 0xCE, 0x38, 0x1C, 0x73, 0xCF, 0x3E, 0x7C, 0xF3,
    0xC7, 0x1F, 0xF8, 0xE3, 0xE7, 0x9F, 0xF9, 0xE7, 0xE3, 0xFF, 0xFB, 0xC7, 0xF3, 0xFF, 0xFF, 0x8F,
    0xF1, 0xFF, 0xFF, 0x8F, 0xF8, 0xFF, 0xFF, 0x1F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

static constexpr uint8_t networkModeIconUsb[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x87,
    0xFF, 0xFF, 0xFE, 0x07, 0xFF, 0xFF, 0xFE, 0x07, 0xFF, 0xFF, 0xFE, 0x07, 0xFF, 0xE1, 0xFE, 0x0F,
    0xFF, 0xE0, 0xFC, 0x0F, 0xFF, 0xE0, 0xF8, 0xFF, 0xFF, 0xC0, 0xF1, 0xFF, 0xFF, 0x81, 0xE3, 0xFF,
    0xFF, 0x0F, 0xC7, 0xFF, 0xFE, 0x1F, 0x8F, 0xBF, 0xFC, 0x3F, 0x1F, 0x1F, 0xF8, 0x7E, 0x3E, 0x0F,
    0xF8, 0xFC, 0x7E, 0x0F, 0xFC, 0x70, 0xFC, 0x1F, 0xFC, 0x70, 0x78, 0x3F, 0xFE, 0x20, 0x10, 0xFF,
    0xFE, 0x00, 0x01, 0xFF, 0xFE, 0x0F, 0x83, 0xFF, 0xFF, 0x1F, 0xE7, 0xFF, 0xFE, 0x1F, 0xFF, 0xFF,
    0xF0, 0x7F, 0xFF, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF,
    0xE1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

static constexpr uint8_t networkModeIconTransfer[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF1, 0xFF, 0xFF, 0xFF,
    0xE0, 0x7F, 0xFF, 0xFF, 0xE0, 0x1F, 0xFF, 0xFF, 0xF2, 0x07, 0xFF, 0xFF, 0xF3, 0x81, 0xFF, 0xFF,
    0xF3, 0xE0, 0x7F, 0xFF, 0xF9, 0xF8, 0x1F, 0xFF, 0xF9, 0xFF, 0x03, 0xFF, 0xF8, 0xFF, 0xC0, 0xFF,
    0xFC, 0xFF, 0xF0, 0x3F, 0xFC, 0xFF, 0xFC, 0x0F, 0xFE, 0x7F, 0xFF, 0x07, 0xFE, 0x00, 0x00, 0x03,
    0xFE, 0x00, 0x00, 0x03, 0xFC, 0x7F, 0xFF, 0x83, 0xFC, 0x7F, 0xFC, 0x1F, 0xFC, 0xFF, 0xF0, 0x7F,
    0xF8, 0xFF, 0xC1, 0xFF, 0xF9, 0xFF, 0x07, 0xFF, 0xF1, 0xFC, 0x1F, 0xFF, 0xF1, 0xF0, 0x7F, 0xFF,
    0xF3, 0xC1, 0xFF, 0xFF, 0xE3, 0x07, 0xFF, 0xFF, 0xE0, 0x3F, 0xFF, 0xFF, 0xE0, 0xFF, 0xFF, 0xFF,
    0xF3, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};
```

- [ ] **Step 2: Check the icon bytes against the drawIcon arrays**

Write the script below to your scratchpad as `check_icons.py`, using the Write tool rather than a heredoc. Then run `python <scratchpad>/check_icons.py`.

```python
import re
import sys

ICONS = "C:/_development/witchhunt-reader/src/components/icons/"
NEW = sys.argv[1] if len(sys.argv) > 1 else ICONS + "networkModeIcons.h"
PAIRS = [
    ("wifi.h", "WifiIcon", "networkModeIconWifi"),
    ("library.h", "LibraryIcon", "networkModeIconLibrary"),
    ("hotspot.h", "HotspotIcon", "networkModeIconHotspot"),
    ("usb.h", "UsbIcon", "networkModeIconUsb"),
    ("transfer.h", "TransferIcon", "networkModeIconTransfer"),
]


def parse(text, name):
    m = re.search(name + r"\[\]\s*=\s*\{(.*?)\};", text, re.S)
    return [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", m.group(1))]


new_text = open(NEW).read()
bad = 0
for legacy_file, legacy_name, new_name in PAIRS:
    src = parse(open(ICONS + legacy_file).read(), legacy_name)
    want = []
    for row in range(32):
        for byte in range(4):
            v = 0
            for k in range(8):
                col = byte * 8 + k
                stored_row = 31 - col  # drawIcon keeps logical column `col` in stored row 31-col
                v = (v << 1) | ((src[stored_row * 4 + row // 8] >> (7 - row % 8)) & 1)
            want.append(v)
    ok = parse(new_text, new_name) == want
    bad += not ok
    print(new_name, "match" if ok else "MISMATCH")
sys.exit(1 if bad else 0)
```

Expected: five lines ending in `match`, exit code 0.

The transform comes from `GfxRenderer::drawIcon`, which calls `display.drawImageTransparent(bitmap, y, getScreenWidth() - width - x, height, width)`, together with the Portrait mapping physical row = `displayHeight - 1 - x`. The arrays were also checked by eye against upstream's Lucide `icon_usb_32` and `icon_wifi_32` (upstream `src/components/icons/listIcons.h`); the orientation matches.

- [ ] **Step 3: Replace `src/activities/network/NetworkModeSelectionActivity.h`**

```cpp
#pragma once

#include "activities/UiListActivity.h"

// USB_SERIAL and USB_DRIVE are mutually exclusive in the menu: a board that can
// present itself as a USB mass-storage device has no use for the serial
// file-transfer protocol, so it offers the drive instead (see the menu tables
// in the .cpp). Both enumerators exist in every build; only the listing differs.
enum class NetworkMode { JOIN_NETWORK, CONNECT_CALIBRE, CREATE_HOTSPOT, USB_SERIAL, USB_DRIVE };

/**
 * NetworkModeSelectionActivity presents the user with a choice:
 * - "Join a Network" - Connect to an existing WiFi network (STA mode)
 * - "Connect to Calibre" - Use Calibre wireless device transfers
 * - "Create Hotspot" - Create an Access Point that others can connect to (AP mode)
 * - "USB Drive" or "USB Transfer" - whichever USB transfer this board has, opened directly
 *   (no result comes back)
 *
 * The onModeSelected callback is called with the user's choice.
 * The onCancel callback is called if the user presses back.
 *
 * Adapted from upstream crosspoint-reader's NetworkModeSelectionActivity (develop @ cdac66ffe,
 * src/activities/network/NetworkModeSelectionActivity.cpp). Ours lists the fourth row on every
 * board (upstream only with USB mass storage) and keeps the serial transfer, the direct USB
 * hand-offs and usesWifi().
 */
class NetworkModeSelectionActivity final : public UiListActivity {
 public:
  explicit NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  // Every board lists four rows; the fourth is its USB transfer method.
  static constexpr int MENU_ITEM_COUNT = 4;

  bool usesWifi() const override { return true; }

  void onModeSelected(NetworkMode mode);
  void onCancel();

 private:
  int listCount() const override { return MENU_ITEM_COUNT; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override { onCancel(); }
  const char* headerTitle() const override;

  // Label, subtitle and icon never change, so the rows are built once, in the constructor.
  freeink::ui::ListItem rowItems[MENU_ITEM_COUNT]{};
};
```

- [ ] **Step 4: Replace `src/activities/network/NetworkModeSelectionActivity.cpp`**

```cpp
#include "NetworkModeSelectionActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/icons/networkModeIcons.h"

namespace fui = freeink::ui;

namespace {
// The fourth row is the USB transfer method this board actually has. Boards
// that can act as a USB mass-storage device (FREEINK_CAP_USB_MSC) show "USB
// Drive"; the rest keep the serial protocol.
#if FREEINK_CAP_USB_MSC
constexpr NetworkMode USB_MODE = NetworkMode::USB_DRIVE;
constexpr StrId USB_MODE_LABEL = StrId::STR_USB_DRIVE;
constexpr StrId USB_MODE_DESC = StrId::STR_USB_DRIVE_DESC;
constexpr const uint8_t* USB_MODE_ICON = networkModeIconUsb;
#else
constexpr NetworkMode USB_MODE = NetworkMode::USB_SERIAL;
constexpr StrId USB_MODE_LABEL = StrId::STR_USB_TRANSFER;
constexpr StrId USB_MODE_DESC = StrId::STR_USB_TRANSFER_DESC;
constexpr const uint8_t* USB_MODE_ICON = networkModeIconTransfer;
#endif

constexpr int ROWS = NetworkModeSelectionActivity::MENU_ITEM_COUNT;
constexpr NetworkMode menuModes[ROWS] = {NetworkMode::JOIN_NETWORK, NetworkMode::CONNECT_CALIBRE,
                                         NetworkMode::CREATE_HOTSPOT, USB_MODE};
constexpr StrId menuItems[ROWS] = {StrId::STR_JOIN_NETWORK, StrId::STR_CALIBRE_WIRELESS, StrId::STR_CREATE_HOTSPOT,
                                   USB_MODE_LABEL};
constexpr StrId menuDescs[ROWS] = {StrId::STR_JOIN_DESC, StrId::STR_CALIBRE_DESC, StrId::STR_HOTSPOT_DESC,
                                   USB_MODE_DESC};
constexpr const uint8_t* menuIcons[ROWS] = {networkModeIconWifi, networkModeIconLibrary, networkModeIconHotspot,
                                            USB_MODE_ICON};

// A 32 px icon from networkModeIcons.h as the bitmap a list row draws (Mask1: bit 0 = ink).
fui::BitmapRef rowIcon(const uint8_t* bits) {
  fui::BitmapRef icon;
  icon.data = bits;
  icon.width = 32;
  icon.height = 32;
  icon.format = fui::BitmapFormat::Mask1;
  return icon;
}
}  // namespace

NetworkModeSelectionActivity::NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("NetworkModeSelection", renderer, mappedInput) {
  for (int i = 0; i < MENU_ITEM_COUNT; i++) {
    fui::ListItem item;
    item.label = I18N.get(menuItems[i]);
    item.subtitle = I18N.get(menuDescs[i]);
    item.icon = rowIcon(menuIcons[i]);
    item.actionValue = static_cast<int16_t>(i);
    rowItems[i] = item;
  }
}

const char* NetworkModeSelectionActivity::headerTitle() const { return tr(STR_FILE_TRANSFER); }

void NetworkModeSelectionActivity::activateIndex(const int index) {
  // Every choice leaves this screen; a lingering tap flash would gray an unrelated element on the
  // next screen's first render.
  app.clearTapFlash();
  nav.selected = index;
  const NetworkMode mode = menuModes[index];

  // Neither USB mode needs WiFi or the web server, so hand off directly here
  // instead of routing the result back through the WiFi-centric
  // CrossPointWebServerActivity. The WiFi modes still return to that owner.
  if (mode == NetworkMode::USB_SERIAL) {
    activityManager.goToSerialTransfer();
    return;
  }
  if (mode == NetworkMode::USB_DRIVE) {
    activityManager.goToUsbDrive();
    return;
  }
  onModeSelected(mode);
}

void NetworkModeSelectionActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Below the header, inside the room listContentRect() leaves for the bottom and side hints.
  const Rect contentRect = listContentRect();
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(contentRect.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (contentRect.x + contentRect.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (contentRect.y + contentRect.height)),
                  static_cast<int16_t>(contentRect.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems;
  props.count = static_cast<uint16_t>(MENU_ITEM_COUNT);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 2;
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  screen.list(props);
}

void NetworkModeSelectionActivity::onModeSelected(NetworkMode mode) {
  setResult(NetworkModeResult{mode});
  finish();
}

void NetworkModeSelectionActivity::onCancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
```

Notes for the implementer:
- `activateIndex` does not range-check `index`. Both ways in already do: `UiListActivity::activatePosition()` checks `position < listCount()`, and `rowActionTrampoline()` checks `index < listCount()`.
- No `RenderLock` is taken. `rowItems` is written only in the constructor, before the activity is pushed, and is read-only afterwards.

- [ ] **Step 5: Check that nothing of the old input or drawing is left**

```bash
cd /c/_development/witchhunt-reader && grep -n "wasPressed\|wasReleased\|isPressed\|ButtonNavigator\|buttonNavigator\|GUI.drawList\|drawButtonHints\|mapLabels\|selectListRow\|selectedIndex" src/activities/network/NetworkModeSelectionActivity.h src/activities/network/NetworkModeSelectionActivity.cpp
```

Expected: no output.

- [ ] **Step 6: Format** (Git Bash)

```bash
cd /c/_development/witchhunt-reader && for f in src/components/icons/networkModeIcons.h src/activities/network/NetworkModeSelectionActivity.h src/activities/network/NetworkModeSelectionActivity.cpp; do "/c/Program Files/LLVM/bin/clang-format.exe" -i "$f"; done
```

Re-run Step 2's script after formatting. clang-format may re-wrap the arrays; the check reads the bytes, not the layout. Expected: still five `match`.

- [ ] **Step 7: Build `default`** (PowerShell, never clean)

```powershell
$env:PLATFORMIO_CORE_DIR = 'C:\pio'; & C:\pio\penv\Scripts\pio.exe run -e default 2>&1 | Select-String -Pattern "^(RAM|Flash):|SUCCESS|FAILED|error"
```

Expected: `SUCCESS`. Record the Flash and RAM bytes against the PR 4 head (Flash 6228461 B, RAM 58280 B) or against the previous task's figure.

The research measured 1,740 B of this screen's own symbols (`render` 518, `loop` 306). The port removes `render`, `loop` and `selectListRow` and adds the constructor, `buildScreen`, `activateIndex` and 512 B of icon rodata. Expect roughly flash-neutral, and report the number either way. RAM (static) should not move, because the icon arrays are `constexpr` and live in flash.

- [ ] **Step 8: Host suite** (Git Bash)

```bash
cd /c/_development/witchhunt-reader && cmake --build test/build -j 8 -- -k 0 > "$TEMP/hb.log" 2>&1; grep FAILED "$TEMP/hb.log" | grep -v epub_build_inventory
cd /c/_development/witchhunt-reader && ctest --test-dir test/build -j 8 2>&1 | grep -E "tests passed|tests failed"
```

Expected: no `FAILED` lines, and `100% tests passed, 0 tests failed out of 1177`. This task adds no host tests; the activity is not in the host build.

- [ ] **Step 9: Commit**

```bash
cd /c/_development/witchhunt-reader && git add src/components/icons/networkModeIcons.h src/activities/network/NetworkModeSelectionActivity.h src/activities/network/NetworkModeSelectionActivity.cpp
cd /c/_development/witchhunt-reader && git commit -m "feat(lists): the network mode picker runs on the list base

File Transfer's mode picker becomes a UiListActivity: FreeInkUI rows with
the description as a subtitle of up to two lines, the list button scheme
(Left/Right step, long Up/Down to the ends, long Back home) and both hint
strips, with room left for the side boxes. The four rows, the direct USB
drive and serial transfer hand-offs, usesWifi() and the cancel result are
unchanged.

The row icons are our own 32 px icons turned upright
(components/icons/networkModeIcons.h): FreeInkUI draws a bitmap in logical
orientation, and drawIcon's pre-rotated arrays cannot be wrapped. They now
show on every theme and in every orientation.

Adapted from upstream crosspoint-reader's NetworkModeSelectionActivity (develop @ cdac66ffe, src/activities/network/NetworkModeSelectionActivity.cpp)."
```

- [ ] **Device checklist for this task:**
  1. **1.1 X4 (buttons), layout.** Home → File Transfer.
     - Four rows: Join a Network, Connect to Calibre, Create Hotspot, and USB Transfer (X3/X4) or USB Drive (boards with USB mass storage).
     - Each row has an upright icon (wifi arcs above the dot; the USB trident's plug at the lower left), a body-text label and a small-text description of at most two lines.
     - The side Up/Down boxes are drawn, and neither the header nor the rows run under them.
     - Footer: Back / Select.
  2. **1.2 X4 movement.**
     - Down steps through all four rows and wraps to the first; Up wraps back.
     - Right steps down and Left steps up.
     - A long Down lands on the last row and a long Up on the first.
     - A double tap on Down, and a long Right, move by the drawn window. With one screen of rows that is the last row; nothing breaks.
  3. **1.3 X4 Confirm, row 1.** Confirm on Join a Network opens the Wi-Fi list. Back from it returns to this picker with row 0 selected.
  4. **1.4 X4 Confirm, rows 2 and 3.** Confirm on Connect to Calibre opens the Calibre screen; Back returns here. Confirm on Create Hotspot starts the hotspot screen.
  5. **1.5 X4 Confirm, USB row.** Confirm on USB Transfer opens the serial transfer screen directly. There is no Wi-Fi and no reboot on exit.
  6. **1.6 X4 Back.** Short Back leaves to the screen File Transfer was opened from (Home). Reopen it: a long Back lands on the same screen. Neither reboots: Wi-Fi was never on.
  7. **1.7 Landscape**, if the UI orientation setting reaches this screen. The icons stay upright, and the list sits clear of the hint strips in both landscapes.
  8. **1.8 T5 S3 (touch), hint boxes.**
     - Tapping the Down/Up side boxes steps; a long tap on Down goes to the last row.
     - Tapping the Confirm box opens the selected mode, and tapping the Back box leaves.
     - A long tap on the Back box goes to the same place as Back.
  9. **1.9 T5 S3, swipe and rows.**
     - A swipe up or down on the list pages without error (one screen, so the selection moves at most to the end).
     - A row tap selects, or opens, according to Settings → touch list activation. A tap on USB Drive opens the USB drive screen.
  10. **1.10 X4 Pro** (S3, buttons and touch, if available). The fourth row is USB Drive with the USB icon, and Confirm opens the USB drive screen.

---

### Task 2: `StatusBarSettingsActivity` onto `UiListActivity`

**Files:**
- Replace: `src/activities/settings/StatusBarSettingsActivity.h` (full file below)
- Replace: `src/activities/settings/StatusBarSettingsActivity.cpp` (full file below)
- Unchanged caller: `src/activities/settings/SettingActionDispatch.cpp:48`, which runs `std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput)`.

**Interfaces:**

- **Consumes** (each checked against `src/activities/UiListActivity.h`):

  | Member | Declaration in the base | Access | Use here |
  |---|---|---|---|
  | ctor | `UiListActivity(const char*, GfxRenderer&, MappedInputManager&, const ListDeclaration& = {})` | protected | `("StatusBarSettings", renderer, mappedInput)` |
  | `onEnter` | `void onEnter() override;` | public | override, calls `UiListActivity::onEnter()` first |
  | `listCount` | `virtual int listCount() const = 0;` | protected | override → `rowCount` |
  | `buildScreen` | `virtual void buildScreen(UiScreen& screen) = 0;` | protected | override |
  | `activateIndex` | `virtual void activateIndex(int index) = 0;` | protected | override, cycles and saves |
  | `headerTitle` | `virtual const char* headerTitle() const` | protected | override → `tr(STR_CUSTOMISE_STATUS_BAR)` |
  | `footerConfirmLabel` | `[[nodiscard]] virtual const char* footerConfirmLabel() const;` | protected | override → `tr(STR_TOGGLE)` |
  | `afterUiRender` | `virtual void afterUiRender() {}` | protected | override, draws the preview |
  | `listContentRect` | `[[nodiscard]] Rect listContentRect() const;` | protected | list margins and the preview band |
  | `syncListViewport` | `void syncListViewport(UiScreen&, freeink::ui::ListProps&, bool hasSubtitle = false);` | protected | default `false` (single-line rows) |
  | `nav`, `ACTION_ROW`, `app` | as in Task 1 | protected | `nav.selected = index`, `props.action`, `app.clearTapFlash()` |

  - Not overridden: `loop`, `render`, `selectListRow`, `onBackButton` (default `finish()`), `homeFromList`, `drawChrome`, `drawFooter` and `footerBackLabel`.
  - `render()` calls the hooks in this order (`UiListActivity.cpp:213-226`): `drawChrome` → `renderUi` (×≤8) → `publishListWindow` → **`afterUiRender`** → `drawFooter` → `displayBuffer`. So the preview is drawn once, after the last rebuild pass, onto a frame no later clear touches.
  - FreeInkUI fields set:
    - `ListItem`: `label`, `value` (`const char*`), `toggle`, `toggleChecked` and `actionValue`. `value` is ignored when `toggle` is set (list.h:25-28).
    - `ListProps`: `items`, `count`, `action`, `inputMask`, `valueInset` (`int16_t`, port of upstream's 8), and `labelText` (`bodyText`, `maxLines = 2`). `valueText` is left unset, so it resolves to the theme's `smallText` (`FreeInkApp.h:266-267`), as in `SettingsActivity`.
- **Produces:**
  - The ctor signature is unchanged (`explicit StatusBarSettingsActivity(GfxRenderer&, MappedInputManager&)`, inline).
  - New public member: `static constexpr int MAX_STATUS_BAR_ITEMS = 12;`, static_asserted against `statusBarItems[]`.
  - Removed: `onExit`, `loop`, `render`, `selectListRow`, `handleSelection` and the members `buttonNavigator` / `selectedIndex`; the unused FreeRTOS includes go too.
- **Behaviour, preserved:**
  - **The table:** `statusBarItems[]`, `enumItem` / `toggleItem`, `visibleItem()` and `visibleItemCount()` are unchanged. Only one comment changes: toggles are now "switch", not "Show/Hide". The clock rows still drop out while `SETTINGS.useClock` is off.
  - **The onEnter clamp loop:** unchanged, and it still runs before the first render. `onEnter` returns before the manager notifies the render task (`ActivityManager.cpp`: `onEnter()` runs in the pending-activity branch; the `xTaskNotify` comes at the end of `loop()`).
  - **Confirm:** cycles `field = (field + 1) % valueCount` and calls `SETTINGS.saveToFile()` on every press, as before. This keeps today's write-per-press against `.skills/SKILL.md` §8 "SPIFFS write throttling", as instructed.
  - **Repaint after a change:** `activateIndex` calls `requestUpdate()`. The controller's Confirm path calls `activateIndex()` and returns, so without this the row would show the old value (see the comment in `SettingsActivity::activateIndex`).
  - **Header and footer:** the header title and the Confirm label (Toggle, via `footerConfirmLabel()`) are unchanged.
  - **Preview drawing:** `drawPreviewProgressBar`, `drawPreviewStatusItems` and their constants are moved verbatim. The preview is still a "Preview" label plus a 78 px box under the list, redrawn on every render from live `SETTINGS`.
  - **Selection:** the old `onEnter` clamp of `selectedIndex` is gone. The base `onEnter()` resets the selection to row 0, and the screen is a new instance on every visit (`make_unique` in `SettingActionDispatch`), so it started at 0 before too.
- **Behaviour, deliberate changes:**
  - **Toggle rows** (chapter/page count, printed page, book %, battery, clock) draw a switch instead of the words Show/Hide. Checked = the old "Show" (value 1).
  - **Enum rows** show their value in FUI's value slot: theme small text, inset 8 px, with no Lyra value pill (spec §2b drops `highlightValue`).
  - **Labels:** body text up to two lines, where the old `drawList` truncated to one. This deliberately differs from upstream's `smallText` labels, to match our `SettingsActivity`.
  - **Buttons and hints:** the list button scheme (as in Task 1), and the side Up/Down hint boxes are now drawn. Long Back = Home, or Back while a reader is on the stack. Nothing is lost: every change was already saved on Confirm.
  - **Preview placement:** the band is reserved at the bottom of `listContentRect()`.
    - The box spans `content.width - 20` from `content.x + 10`, no longer the full screen width. On X4 portrait it ends at the side strip; on X3 it clears the strips on both sides.
    - The label is at `content.x + contentSidePadding`.
    - Vertically: label, half a spacing, box, then one `verticalSpacing` to the hints. The old layout left 2.5 spacings below the box.
  - **Heap while open:** the base's `FreeInkApp` plus 12 `ListItem`s (about 56-60 B each, ≈700 B) in the activity object, instead of a `ButtonNavigator`. No `std::string` per row and none per render, except the preview's existing truncated title.

- [ ] **Step 1: Replace `src/activities/settings/StatusBarSettingsActivity.h`**

```cpp
#pragma once

#include "activities/UiListActivity.h"

// Reader status bar configuration activity.
//
// Adapted from upstream crosspoint-reader's StatusBarSettingsActivity (develop @ cdac66ffe,
// src/activities/settings/StatusBarSettingsActivity.cpp): the FreeInkUI row list built from a fixed
// ListItem array, with on/off items as switches. The items themselves (statusBarItems[] in the
// .cpp), cycling the other items on Confirm, and the boxed preview are ours: upstream has a
// smaller item set, edits values in a popup and previews with the real status bar, which cannot
// show our upper/lower bars, items position, printed page or clock position.
class StatusBarSettingsActivity final : public UiListActivity {
 public:
  explicit StatusBarSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("StatusBarSettings", renderer, mappedInput) {}

  // Every entry of statusBarItems[] (the .cpp static_asserts the match). The clock rows are the
  // only ones that can drop out.
  static constexpr int MAX_STATUS_BAR_ITEMS = 12;

  void onEnter() override;

 private:
  int listCount() const override { return rowCount; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  [[nodiscard]] const char* footerConfirmLabel() const override;
  // Draws the preview into the band buildScreen() keeps free under the list.
  void afterUiRender() override;

  // The rows this visit shows. The clock rows drop out while the clock is off, and that cannot
  // change while this screen is open (it is set in Settings > Clock).
  int rowCount = 0;
  // Labels and action values are set once in onEnter(); buildScreen() refreshes each row's value or
  // switch from SETTINGS. Every string is an I18N pointer, so nothing is allocated per row.
  freeink::ui::ListItem rowItems[MAX_STATUS_BAR_ITEMS]{};
};
```

- [ ] **Step 2: Replace `src/activities/settings/StatusBarSettingsActivity.cpp`**

```cpp
#include "StatusBarSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
const StrId progressBarNames[] = {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE};
const StrId progressBarThicknessNames[] = {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM,
                                           StrId::STR_PROGRESS_BAR_THICK};
const StrId titleNames[] = {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE};
const StrId statusItemsPositionNames[] = {StrId::STR_TOP, StrId::STR_BOTTOM};
const StrId clockPositionNames[] = {StrId::STR_ALIGN_LEFT, StrId::STR_ALIGN_RIGHT};

// One menu row. Editing a status-bar option means: cycle `field` through `valueCount` values and
// display its current value. Rows with an enum-style set of choices provide `valueNames` (indexed by
// the field value); rows with no `valueNames` are on/off toggles, drawn as a switch.
//
// The whole menu is this single table. Adding, removing, or reordering a row is a one-line edit here —
// there is no parallel index bookkeeping to keep in sync. Rows with `requiresClock` are skipped when
// the clock feature is off, so the visible list compacts without any index remapping.
struct StatusBarItem {
  StrId label;
  uint8_t CrossPointSettings::* field;
  uint8_t valueCount;
  uint8_t defaultValue;     // value to reset to if the stored one is out of range
  const StrId* valueNames;  // nullptr → on/off switch
  bool requiresClock;
};

template <size_t N>
constexpr StatusBarItem enumItem(StrId label, uint8_t CrossPointSettings::* field, const StrId (&names)[N],
                                 uint8_t defaultValue, bool requiresClock = false) {
  return {label, field, static_cast<uint8_t>(N), defaultValue, names, requiresClock};
}
constexpr StatusBarItem toggleItem(StrId label, uint8_t CrossPointSettings::* field, bool requiresClock = false) {
  return {label, field, 2, 1, nullptr, requiresClock};
}

const StatusBarItem statusBarItems[] = {
    enumItem(StrId::STR_STATUS_ITEMS_POSITION, &CrossPointSettings::statusBarItemsPosition, statusItemsPositionNames,
             CrossPointSettings::STATUS_BAR_ITEMS_POSITION::STATUS_BAR_ITEMS_BOTTOM),
    toggleItem(StrId::STR_CHAPTER_PAGE_COUNT, &CrossPointSettings::statusBarChapterPageCount),
    toggleItem(StrId::STR_PRINTED_PAGE_NUMBER, &CrossPointSettings::statusBarPrintedPage),
    toggleItem(StrId::STR_BOOK_PROGRESS_PERCENTAGE, &CrossPointSettings::statusBarBookProgressPercentage),
    enumItem(StrId::STR_TITLE, &CrossPointSettings::statusBarTitle, titleNames,
             CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE),
    toggleItem(StrId::STR_BATTERY, &CrossPointSettings::statusBarBattery),
    toggleItem(StrId::STR_CLOCK, &CrossPointSettings::statusBarClock, /*requiresClock=*/true),
    enumItem(StrId::STR_CLOCK_POSITION, &CrossPointSettings::statusBarClockPosition, clockPositionNames,
             CrossPointSettings::STATUS_BAR_CLOCK_POSITION::STATUS_BAR_CLOCK_LEFT, /*requiresClock=*/true),
    enumItem(StrId::STR_UPPER_PROGRESS_BAR, &CrossPointSettings::statusBarUpperProgressBar, progressBarNames,
             CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS),
    enumItem(StrId::STR_UPPER_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarUpperProgressBarThickness,
             progressBarThicknessNames, CrossPointSettings::STATUS_BAR_PROGRESS_BAR_THICKNESS::PROGRESS_BAR_NORMAL),
    enumItem(StrId::STR_LOWER_PROGRESS_BAR, &CrossPointSettings::statusBarLowerProgressBar, progressBarNames,
             CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS),
    enumItem(StrId::STR_LOWER_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarLowerProgressBarThickness,
             progressBarThicknessNames, CrossPointSettings::STATUS_BAR_PROGRESS_BAR_THICKNESS::PROGRESS_BAR_NORMAL),
};
static_assert(sizeof(statusBarItems) / sizeof(statusBarItems[0]) ==
                  static_cast<size_t>(StatusBarSettingsActivity::MAX_STATUS_BAR_ITEMS),
              "keep StatusBarSettingsActivity::MAX_STATUS_BAR_ITEMS in sync with statusBarItems[]");

// Map a visible row index (clock rows omitted when the clock is off) to its entry in statusBarItems.
const StatusBarItem& visibleItem(int visibleIndex) {
  int seen = 0;
  for (const auto& item : statusBarItems) {
    if (item.requiresClock && !SETTINGS.useClock) {
      continue;
    }
    if (seen == visibleIndex) {
      return item;
    }
    ++seen;
  }
  return statusBarItems[0];  // out-of-range guard; callers clamp the index first
}

int visibleItemCount() {
  return static_cast<int>(
      std::count_if(std::begin(statusBarItems), std::end(statusBarItems),
                    [](const StatusBarItem& item) { return !item.requiresClock || SETTINGS.useClock; }));
}

// Retained for the progress-bar preview drawing below, which references specific enum cardinalities.
constexpr int PROGRESS_BAR_ITEMS = 3;

constexpr int previewHorizontalInset = 10;
constexpr int previewHeight = 78;
constexpr int previewInnerMargin = 4;
constexpr int previewBatteryInset = 2;  // matches the battery's inset from the margin in the real bar
constexpr int statusItemGap = 8;        // gap between adjacent status items, as in BaseTheme::drawStatusBar

// The band under the list that the preview owns: its label, the box, and a spacing above and below.
int previewBandHeight(const GfxRenderer& renderer, const ThemeMetrics& metrics) {
  return renderer.getLineHeight(UI_10_FONT_ID) + previewHeight + metrics.verticalSpacing * 2;
}

void drawPreviewProgressBar(const GfxRenderer& renderer, const Rect& rect, const uint8_t progressBar,
                            const uint8_t thickness, const bool topEdge) {
  if (progressBar == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS) {
    return;
  }

  const int percent = progressBar == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::BOOK_PROGRESS ? 75 : 25;
  const int barHeight = UITheme::getProgressBarHeight(progressBar, thickness);
  const int y = topEdge ? rect.y + previewInnerMargin : rect.y + rect.height - previewInnerMargin - barHeight;
  const int barWidth = (rect.width - previewInnerMargin * 2) * percent / 100;
  renderer.fillRect(rect.x + previewInnerMargin, y, barWidth, barHeight);
}

void drawPreviewStatusItems(const GfxRenderer& renderer, const Rect& rect, const ThemeMetrics& metrics) {
  const bool hasProgressText = SETTINGS.statusBarChapterPageCount || SETTINGS.statusBarBookProgressPercentage;
  const bool hasTitle = SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE;
  const bool hasStatusItems = hasProgressText || hasTitle || SETTINGS.statusBarBattery ||
                              SETTINGS.statusBarPrintedPage || (SETTINGS.useClock && SETTINGS.statusBarClock);
  if (!hasStatusItems) {
    return;
  }

  const bool statusItemsAtTop =
      SETTINGS.statusBarItemsPosition == CrossPointSettings::STATUS_BAR_ITEMS_POSITION::STATUS_BAR_ITEMS_TOP;
  const int adjacentProgressHeight = statusItemsAtTop
                                         ? UITheme::getProgressBarHeight(SETTINGS.statusBarUpperProgressBar,
                                                                         SETTINGS.statusBarUpperProgressBarThickness)
                                         : UITheme::getProgressBarHeight(SETTINGS.statusBarLowerProgressBar,
                                                                         SETTINGS.statusBarLowerProgressBarThickness);
  const int statusItemsHeight = UITheme::getStatusBarItemsHeight();
  const int textY = statusItemsAtTop
                        ? rect.y + previewInnerMargin + adjacentProgressHeight + 4
                        : rect.y + rect.height - previewInnerMargin - adjacentProgressHeight - statusItemsHeight + 4;

  const bool showBatteryPercentage =
      SETTINGS.statusBarBattery &&
      SETTINGS.hideBatteryPercentage == CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_NEVER;
  const bool showClock = SETTINGS.useClock && SETTINGS.statusBarClock;
  const int previewClockWidth = showClock ? renderer.getTextWidth(SMALL_FONT_ID, "00:00") : 0;
  const bool clockOnRight =
      SETTINGS.statusBarClockPosition == CrossPointSettings::STATUS_BAR_CLOCK_POSITION::STATUS_BAR_CLOCK_RIGHT;

  // Left cluster: battery, then the clock when it is left-positioned. Reserving the battery's
  // *measured* width (icon + percentage) is what keeps the clock off the percentage text —
  // estimating it is what made the preview overlap (issue #214).
  const int leftClusterX = rect.x + previewInnerMargin + previewBatteryInset;
  int leftClusterWidth = 0;
  if (SETTINGS.statusBarBattery) {
    GUI.drawBatteryLeft(renderer, Rect{leftClusterX, textY, metrics.batteryWidth, metrics.batteryHeight},
                        showBatteryPercentage);
    leftClusterWidth = BaseTheme::statusBarBatteryWidth(renderer, metrics, showBatteryPercentage);
  }

  // Right-aligned zone: the printed ("physical") page label sits to the LEFT of the device page
  // counter as a parenthesised hint, matching BaseTheme::drawStatusBar. Example label "(vii)".
  const char* printedLabel = SETTINGS.statusBarPrintedPage ? "(vii)" : "";
  const int printedLabelWidth = *printedLabel ? renderer.getTextWidth(SMALL_FONT_ID, printedLabel) : 0;
  const int printedLabelGap = printedLabelWidth > 0 && hasProgressText ? 8 : 0;

  int progressTextWidth = 0;
  const int rightEdge = rect.x + rect.width - previewInnerMargin - 2;
  if (hasProgressText) {
    char progressStr[32] = "";
    if (SETTINGS.statusBarChapterPageCount && SETTINGS.statusBarBookProgressPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%d/%d  %d%%", 8, 32, 75);
    } else if (SETTINGS.statusBarBookProgressPercentage) {
      snprintf(progressStr, sizeof(progressStr), "%d%%", 75);
    } else {
      snprintf(progressStr, sizeof(progressStr), "%d/%d", 8, 32);
    }

    const int progressStrWidth = renderer.getTextWidth(SMALL_FONT_ID, progressStr);
    progressTextWidth = progressStrWidth + printedLabelGap + printedLabelWidth;
    renderer.drawText(SMALL_FONT_ID, rightEdge - progressStrWidth, textY, progressStr);
    if (printedLabelWidth > 0) {
      renderer.drawText(SMALL_FONT_ID, rightEdge - progressStrWidth - printedLabelGap - printedLabelWidth, textY,
                        printedLabel);
    }
  } else if (printedLabelWidth > 0) {
    progressTextWidth = printedLabelWidth;
    renderer.drawText(SMALL_FONT_ID, rightEdge - printedLabelWidth, textY, printedLabel);
  }

  // Clock goes at whichever end it is configured for, mirroring BaseTheme::drawStatusBar: just
  // past the battery on the left, or just past the progress text on the right.
  int rightClusterWidth = progressTextWidth;
  if (showClock) {
    int clockX;
    if (clockOnRight) {
      rightClusterWidth += (rightClusterWidth > 0 ? statusItemGap : 0) + previewClockWidth;
      clockX = rightEdge - rightClusterWidth;
    } else {
      clockX = leftClusterX + leftClusterWidth + statusItemGap;
      leftClusterWidth += statusItemGap + previewClockWidth;
    }
    renderer.drawText(SMALL_FONT_ID, clockX, textY, "00:00");
  }

  if (!hasTitle) {
    return;
  }

  const char* title = SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE
                          ? tr(STR_EXAMPLE_BOOK)
                          : tr(STR_EXAMPLE_CHAPTER);
  const int leftReserve = leftClusterWidth > 0 ? previewBatteryInset + leftClusterWidth + statusItemGap : 6;
  const int rightReserve = rightClusterWidth > 0 ? rightClusterWidth + 18 : 6;
  const int titleAreaWidth = rect.width - previewInnerMargin * 2 - leftReserve - rightReserve;
  if (titleAreaWidth <= 0) {
    return;
  }

  std::string previewTitle = renderer.truncatedText(SMALL_FONT_ID, title, titleAreaWidth);
  const int titleWidth = renderer.getTextWidth(SMALL_FONT_ID, previewTitle.c_str());
  renderer.drawText(SMALL_FONT_ID, rect.x + previewInnerMargin + leftReserve + (titleAreaWidth - titleWidth) / 2, textY,
                    previewTitle.c_str());
}
}  // namespace

void StatusBarSettingsActivity::onEnter() {
  UiListActivity::onEnter();

  // Clamp status bar settings in case of corrupt/migrated data: every field must hold a valid value
  // index (0..valueCount-1). A stray value would index past its valueNames array when rendered.
  for (const auto& item : statusBarItems) {
    if (SETTINGS.*item.field >= item.valueCount) {
      SETTINGS.*item.field = item.defaultValue;
    }
  }

  // The rows this visit shows. Labels and action values never change while the screen is open;
  // buildScreen() fills in the values.
  rowCount = visibleItemCount();
  for (int i = 0; i < rowCount; ++i) {
    rowItems[i] = {};
    rowItems[i].label = I18N.get(visibleItem(i).label);
    rowItems[i].actionValue = static_cast<int16_t>(i);
  }
}

const char* StatusBarSettingsActivity::headerTitle() const { return tr(STR_CUSTOMISE_STATUS_BAR); }

// Every row changes in place on Confirm: a switch flips, a value moves to the next one.
const char* StatusBarSettingsActivity::footerConfirmLabel() const { return tr(STR_TOGGLE); }

void StatusBarSettingsActivity::activateIndex(const int index) {
  // The row repaints with its new value; a lingering tap flash would gray it.
  app.clearTapFlash();
  nav.selected = index;
  const StatusBarItem& item = visibleItem(index);
  SETTINGS.*item.field = static_cast<uint8_t>((SETTINGS.*item.field + 1) % item.valueCount);
  SETTINGS.saveToFile();
  // Nothing else repaints: the controller's Confirm path calls activateIndex() and returns.
  requestUpdate();
}

void StatusBarSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Below the header, inside the room listContentRect() leaves for both hint strips, and above the
  // preview band afterUiRender() draws into.
  const Rect contentRect = listContentRect();
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(contentRect.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (contentRect.x + contentRect.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (contentRect.y + contentRect.height) +
                                       previewBandHeight(renderer, metrics)),
                  static_cast<int16_t>(contentRect.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Labels were set in onEnter(); the values track SETTINGS, so they are refreshed on every pass.
  // A value is an I18N pointer and a switch is two flags: nothing is allocated.
  for (int i = 0; i < rowCount; ++i) {
    const StatusBarItem& item = visibleItem(i);
    const uint8_t value = SETTINGS.*item.field;
    auto& row = rowItems[i];
    row.toggle = item.valueNames == nullptr;
    row.toggleChecked = row.toggle && value != 0;
    row.value = row.toggle ? nullptr : I18N.get(item.valueNames[value]);
  }

  fui::ListProps props;
  props.items = rowItems;
  props.count = static_cast<uint16_t>(rowCount);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;  // air between the value and the row edge
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void StatusBarSettingsActivity::afterUiRender() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = listContentRect();
  const int labelHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int bandTop = content.y + content.height - previewBandHeight(renderer, metrics);

  const int labelY = bandTop + metrics.verticalSpacing / 2;
  renderer.drawText(UI_10_FONT_ID, content.x + metrics.contentSidePadding, labelY, tr(STR_PREVIEW));
  const Rect previewRect{content.x + previewHorizontalInset, labelY + labelHeight + metrics.verticalSpacing / 2,
                         content.width - previewHorizontalInset * 2, previewHeight};
  renderer.drawRect(previewRect.x, previewRect.y, previewRect.width, previewRect.height);
  drawPreviewProgressBar(renderer, previewRect, SETTINGS.statusBarUpperProgressBar,
                         SETTINGS.statusBarUpperProgressBarThickness, true);
  drawPreviewProgressBar(renderer, previewRect, SETTINGS.statusBarLowerProgressBar,
                         SETTINGS.statusBarLowerProgressBarThickness, false);
  drawPreviewStatusItems(renderer, previewRect, metrics);
}
```

Notes for the implementer:
- **Geometry.** The band is `labelH + 78 + 2·vs`. Inside it, the label sits at `bandTop + vs/2` and the box at `bandTop + vs + labelH`. So the box ends at `bandBottom − vs`, which is the bottom of `listContentRect()`, minus one spacing. The list's bottom inset is exactly the band plus the hint strip, so no row is drawn under the preview.
- **Tasks:** `rowItems` is written in `onEnter()` (loop task, before the first render) and in `buildScreen()` (render task, under the render's lock). The loop task never writes it after `onEnter()`. `SETTINGS.*field` is a `uint8_t` the loop task writes and the render reads, the same as before the port. A render that races a Confirm draws one stale frame, and the `requestUpdate()` that follows repaints it.
- **Unused constant:** `PROGRESS_BAR_ITEMS` was already unused; it is kept to keep the diff to the port.

- [ ] **Step 3: Check that nothing of the old input or drawing is left**

```bash
cd /c/_development/witchhunt-reader && grep -n "wasPressed\|wasReleased\|isPressed\|ButtonNavigator\|buttonNavigator\|GUI.drawList\|drawButtonHints\|mapLabels\|selectListRow\|selectedIndex\|STR_SHOW\|freertos" src/activities/settings/StatusBarSettingsActivity.h src/activities/settings/StatusBarSettingsActivity.cpp
```

Expected: no output.

- [ ] **Step 4: Format** (Git Bash)

```bash
cd /c/_development/witchhunt-reader && for f in src/activities/settings/StatusBarSettingsActivity.h src/activities/settings/StatusBarSettingsActivity.cpp; do "/c/Program Files/LLVM/bin/clang-format.exe" -i "$f"; done
```

- [ ] **Step 5: Build `default`** (PowerShell, never clean)

```powershell
$env:PLATFORMIO_CORE_DIR = 'C:\pio'; & C:\pio\penv\Scripts\pio.exe run -e default 2>&1 | Select-String -Pattern "^(RAM|Flash):|SUCCESS|FAILED|error"
```

Expected: `SUCCESS`. Record Flash and RAM against Task 1's figures.

The research measured 2,724 B for this screen; `render` is 1,780 of it, of which about 1.4 KB is the preview. The port removes `loop` (258), the `std::function` row thunk (106) and the `ButtonNavigator` calls, and adds `buildScreen`, `activateIndex` and `afterUiRender`. The preview moves unchanged. Expect a small decrease, and report the number either way.

- [ ] **Step 6: Host suite** (Git Bash)

```bash
cd /c/_development/witchhunt-reader && cmake --build test/build -j 8 -- -k 0 > "$TEMP/hb.log" 2>&1; grep FAILED "$TEMP/hb.log" | grep -v epub_build_inventory
cd /c/_development/witchhunt-reader && ctest --test-dir test/build -j 8 2>&1 | grep -E "tests passed|tests failed"
```

Expected: no `FAILED` lines, and `100% tests passed, 0 tests failed out of 1177`. No new tests. `test/list_row_tap/ListRowTapTest.cpp` names this screen only in a comment (line 111) and is unaffected.

- [ ] **Step 7: Commit**

```bash
cd /c/_development/witchhunt-reader && git add src/activities/settings/StatusBarSettingsActivity.h src/activities/settings/StatusBarSettingsActivity.cpp
cd /c/_development/witchhunt-reader && git commit -m "feat(lists): the status bar settings run on the list base

The status bar screen becomes a UiListActivity with FreeInkUI rows: on/off
items are switches instead of Show/Hide text, the other items show their
value and still cycle on Confirm, saving each change as before. The list
follows the list button scheme and draws both hint strips; the preview keeps
its band under the list, inside the room left for the side boxes. The item
table, the clock-row filter, the corrupt-value clamp and the preview drawing
are unchanged.

Adapted from upstream crosspoint-reader's StatusBarSettingsActivity (develop @ cdac66ffe, src/activities/settings/StatusBarSettingsActivity.cpp)."
```

- [ ] **Device checklist for this task:**
  1. **2.1 X4 (buttons), layout.** Settings → Reader → Customise status bar.
     - The header reads Customise Status Bar.
     - There are ten rows with the clock off, or twelve with Settings → Clock on (Clock and Clock position appear after Battery).
     - The five on/off rows show a switch; the other rows show their value at the right edge in small text.
     - The "Preview" label and its box sit under the list, and no row is drawn over or under them.
     - The side Up/Down boxes are drawn, and neither the box nor the rows run under them.
     - Footer: Back / Toggle.
  2. **2.2 X4 toggle rows.** Confirm on Battery flips the switch, and the preview's battery appears or disappears on the same frame. Repeat for Chapter page count, Printed page number and Book progress percentage: the preview's text at the right changes to 8/32, 75 %, (vii) and so on.
  3. **2.3 X4 enum rows.**
     - Confirm on Title cycles Book → Chapter → Hide → Book, and the preview title follows.
     - Upper and lower progress bar and thickness cycle, and the bars in the box follow.
     - Status items position Top/Bottom moves the text row inside the box.
  4. **2.4 X4 persistence.** Change two rows, short Back, then reopen: both values are kept, and the selection starts on the first row. Change one, then long Back: Home. Reopen from Settings: the change is kept.
  5. **2.5 X4 movement.**
     - Up/Down step and wrap. Left/Right step.
     - A long Up/Down goes to the first or last row.
     - A long Right pages when the list is longer than one screen (in landscape, or at a large UI font), and the selection keeps its line.
  6. **2.6 Clock rows.** With the clock on, Confirm on Clock position moves "00:00" between the left and right clusters in the preview, without overlapping the battery percentage (#214).
  7. **2.7 Landscape**, if the UI orientation reaches this screen.
     - The preview band stays above the bottom side strip (landscape clockwise), the list pages, and the box fits the content width.
     - On X3 (if available), the box clears the side strips on both sides in portrait.
  8. **2.8 Opened from the reader menu**, if Settings is reachable there with a book open. A long Back acts as Back and returns to Settings, not Home.
  9. **2.9 T5 S3 (touch), hint boxes.**
     - Tapping the Confirm box toggles the selected row, and the preview updates.
     - Tapping the side boxes steps; a long tap on the Down box goes to the last row.
     - Tapping the Back box leaves; a long tap on the Back box goes Home.
  10. **2.10 T5 S3, swipe and rows.**
      - A row tap selects or toggles the row according to the touch-list-activation setting. A tap on a switch row's switch toggles it the same way (the whole row is the target).
      - A swipe up or down pages when there is more than one screen.
      - The preview box is never a tap target: a tap there does nothing.

### Task 3: `OpdsServerListActivity` onto `UiListActivity`

The OPDS server list (Settings → OPDS, and Home's OPDS picker) moves from `Activity` with `GUI.drawList` and `ButtonNavigator` onto `UiListActivity`. Its rows are now drawn by FreeInkUI and its buttons are read through the `ListController`. The row building is ported from upstream: `rebuildRowItems()` with `const char*` into `OPDS_STORE`, the folder/format subtitles taken live from `SETTINGS`, and `centeredText` for an empty list. Our input-side features stay.

What happens to each existing behaviour:
- **Rows: unchanged.**
  - Settings mode: servers, then "Add Server", "Download folder" (subtitle: the folder or "SD root") and "Filename format" (subtitle: the current format).
  - Picker mode: servers only, with **no "Add Server" row**. Upstream's picker row is not ported (ruling: no behaviour change in a list migration).
  - A server row shows the name, or the URL when unnamed. The URL is the subtitle only when the server is named.
- **Selection on entry:** row 0, as today. The base `onEnter()` resets `nav`.
- **Confirm: unchanged.**
  - Picker: `replaceActivity(OpdsBookBrowserActivity(server, initialQuery_))`, keeping our search hand-off.
  - Server row: the editor at that index. "Add Server": the editor with -1.
  - "Download folder": the keyboard; the result is normalized, copied and saved.
  - "Filename format": Confirm **cycles** the format and saves. No `OptionPopup` (constraint: it reads levels and costs about 300 lines of flash).
- **After the editor returns:** reload the store and **clamp** the selection to the new row count (ours). Upstream resets it to 0, which loses the user's place; not ported.
- **Back: unchanged.** Settings mode: `finish()`. Picker mode: `activityManager.goHome()` (ours has no `goHome(HomeMenuItem)` overload, so upstream's is not ported).
- **Long Back (new, from the scheme): base `homeFromList()`, not overridden.** Back commits nothing: the folder and format are saved when changed, and the editor saves each field as it is entered. See the first OPEN item for the picker's stale-hint case.
- **Navigation (deliberate, per spec):**
  - Up/Down and Left/Right step.
  - Two quick Up/Down taps jump a page. A held Up/Down goes to the ends.
  - A held Left/Right or a swipe pages.
  - Both hint strips are drawn, including the side Up/Down boxes.
- **Layout (deliberate, same as every migrated list):**
  - The list lays out inside `listContentRect()`, and the header sits at `listHeaderRect()`.
  - On a board with side hints, both are narrower than today's full-width ones.
  - The empty-picker message is centred in the list body rather than at half screen height.
- **Touch:** row taps go through FreeInkUI and the base `selectListRow`. They follow the same `touchListActivation` preference that ActivityManager applied to the legacy `GUI.drawList` band (ActivityManager.cpp:820), so tap behaviour does not change.
- **Render-owned data:**
  - `OPDS_STORE.loadFromFile()` runs under `RenderLock` (onEnter and the editor's result handler).
  - The folder keyboard result and the format cycle write `SETTINGS` under `RenderLock`, then save outside it.
  - `rowItems_` is written only by the render task (see OPEN).
- **Heap:**
  - Today: about 12 `std::string` copies per render through two `std::function` lambdas.
  - Now: none per render.
  - The row array is fixed: `MAX_SERVERS + 3` = 11 `ListItem`s inside the activity object. `OpdsServerStore` caps servers at 8 on load (JsonSettingsIO.cpp:761) and on add (OpdsServerStore.cpp:81), and `serverRows()` clamps again.
- **Not ported:** `OptionPopup`, `makeUniqueNoThrow`, the picker's "Add Server", `goHome(HomeMenuItem::OPDS_BROWSER)`, and upstream's `std::vector<ListItem>` (a fixed array: at most 11 rows).

**Files:**
- Replace: `src/activities/settings/OpdsServerListActivity.h` (full file below)
- Replace: `src/activities/settings/OpdsServerListActivity.cpp` (full file below)

**Interfaces:**
- Consumes, from `src/activities/UiListActivity.h` (checked against the file at b44579413):
  - the protected constructor `UiListActivity(const char* name, GfxRenderer&, MappedInputManager&, const ListDeclaration& = {})`, called with the default declaration;
  - `public: void onEnter() override` (called first in ours);
  - the protected pure virtuals `int listCount() const`, `void buildScreen(UiScreen&)` and `void activateIndex(int)`;
  - the protected virtuals `void onBackButton()` and `const char* headerTitle() const`;
  - the protected non-virtual helpers `Rect listContentRect() const`, `void syncListViewport(UiScreen&, freeink::ui::ListProps&, bool hasSubtitle = false)` (passed `true`) and `void moveSelectionTo(int)`;
  - the protected member `freeink::ui::ListNav nav` (`nav.selected` is `std::atomic<int>`);
  - `static constexpr freeink::ui::ActionId ACTION_ROW`;
  - `app.clearTapFlash()` (`UiAppHost::app`, a public member of the protected base).
  - **Not overridden:** `loop()`, `render()`, `selectListRow()`, `homeFromList()`, `drawFooter()`, `drawChrome()`.
- Consumes, from FreeInkUI:
  - `ListItem::label`, `subtitle` and `actionValue` (`components/lists/list.h:13-33`);
  - `ListProps::items`, `count`, `action` and `inputMask` (`list.h:51-90`), and `freeink::ui::InputTouch`;
  - `Screen::setContentMarginFromScreen(Insets)`, `spacer(int16_t)`, `centeredText(const char*, TextStyle)`, `list(ListProps)` and `theme().bodyText` (`FreeInkApp.h:71, 113, 568`).
- Consumes from the app: `OpdsServerStore::MAX_SERVERS`, `getCount()`, `getServers()`, `getServer(size_t)` and `loadFromFile()`; `OpdsSettingsActivity(GfxRenderer&, MappedInputManager&, int serverIndex)` (unchanged by Task 4).
- Produces: nothing new. The public constructor `OpdsServerListActivity(GfxRenderer&, MappedInputManager&, bool pickerMode = false, std::string initialQuery = {})` is unchanged, so `ActivityManager.cpp:506, 515` and `SettingActionDispatch.cpp:56` compile as they are.

- [ ] **Step 1: Replace `src/activities/settings/OpdsServerListActivity.h`**

```cpp
#pragma once

#include <string>

#include "OpdsServerStore.h"
#include "activities/UiListActivity.h"

// Adapted from upstream crosspoint-reader's OpdsServerListActivity (develop @ cdac66ffe,
// src/activities/settings/OpdsServerListActivity.cpp).
//
// The list of configured OPDS servers. Settings mode lists the servers, then "Add Server",
// "Download folder" and "Filename format"; a server row opens its editor. Picker mode (from Home)
// lists the servers only, and a server row opens the OPDS browser on that server, carrying the
// search query it was opened with.
class OpdsServerListActivity final : public UiListActivity {
 public:
  explicit OpdsServerListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool pickerMode = false,
                                  std::string initialQuery = {});

  void onEnter() override;

 private:
  // Settings mode appends "Add Server", "Download folder" and "Filename format" to the servers.
  static constexpr int SETTINGS_ROWS = 3;
  static constexpr int MAX_ROWS = static_cast<int>(OpdsServerStore::MAX_SERVERS) + SETTINGS_ROWS;

  int listCount() const override { return getItemCount(); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Picker mode backs out to Home rather than finishing.
  void onBackButton() override;
  const char* headerTitle() const override;

  int serverRows() const;
  int getItemCount() const;
  int rebuildRowItems();
  void reloadServers();
  void handleSelection(int index);

  bool pickerMode = false;
  std::string initialQuery_;
  // The rows: pointers into OPDS_STORE's strings, SETTINGS and the string table. rebuildRowItems()
  // refills them at the start of every build, so only the render task writes them, and they can
  // never point into a store the server editor has changed since they were built. At most
  // MAX_SERVERS + 3 rows, so a fixed array and no heap.
  freeink::ui::ListItem rowItems_[MAX_ROWS]{};
};
```

- [ ] **Step 2: Replace `src/activities/settings/OpdsServerListActivity.cpp`**

```cpp
#include "OpdsServerListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "OpdsSettingsActivity.h"
#include "activities/ActivityManager.h"
#include "activities/browser/OpdsBookBrowserActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "util/OpdsFilename.h"

namespace fui = freeink::ui;

namespace {
// Normalizes a user-typed folder: trims spaces, "" => SD root, otherwise a
// single leading '/' and no trailing '/'. Cold path (runs once per edit).
std::string normalizeFolder(std::string v) {
  while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
  while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
  if (v.empty()) return "";
  if (v.front() != '/') v.insert(v.begin(), '/');
  while (v.size() > 1 && v.back() == '/') v.pop_back();
  if (v == "/") return "";  // a bare slash is SD root, same as empty
  return v;
}

// Label shown for the current OPDS filename format in the list subtitle.
StrId opdsFormatLabel(uint8_t format) {
  switch (format) {
    case static_cast<uint8_t>(OpdsFilenameFormat::TitleAuthor):
      return StrId::STR_FMT_TITLE_AUTHOR;
    case static_cast<uint8_t>(OpdsFilenameFormat::TitleOnly):
      return StrId::STR_FMT_TITLE;
    default:
      return StrId::STR_FMT_AUTHOR_TITLE;
  }
}

fui::ListItem makeRow(const char* label, const char* subtitle, const int actionValue) {
  fui::ListItem item;
  item.label = label;
  item.subtitle = subtitle;
  item.actionValue = static_cast<int16_t>(actionValue);
  return item;
}
}  // namespace

OpdsServerListActivity::OpdsServerListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const bool pickerMode, std::string initialQuery)
    : UiListActivity("OpdsServerList", renderer, mappedInput),
      pickerMode(pickerMode),
      initialQuery_(std::move(initialQuery)) {}

int OpdsServerListActivity::serverRows() const {
  // The store never holds more than MAX_SERVERS (loading and adding both stop there); the clamp
  // keeps rowItems_ in bounds whatever opds.json says.
  return static_cast<int>(std::min(OPDS_STORE.getCount(), OpdsServerStore::MAX_SERVERS));
}

int OpdsServerListActivity::getItemCount() const {
  // Picker mode lists the servers only; settings mode adds its three rows after them.
  return serverRows() + (pickerMode ? 0 : SETTINGS_ROWS);
}

void OpdsServerListActivity::onEnter() {
  UiListActivity::onEnter();
  // Reload from disk in case servers were added/removed by a subactivity or the web UI.
  reloadServers();
}

void OpdsServerListActivity::reloadServers() {
  // The rows point into the store's strings, and a reload replaces every one of them: it must not
  // overlap a build.
  RenderLock lock(*this);
  OPDS_STORE.loadFromFile();
}

// Fills rowItems_ from OPDS_STORE and SETTINGS; returns the row count. Called by buildScreen() on
// the render task, which holds the render lock for the whole pass, so the store cannot reload under
// it. Pointer assignments only: nothing is copied or allocated.
int OpdsServerListActivity::rebuildRowItems() {
  const auto& servers = OPDS_STORE.getServers();
  const int serverCount = serverRows();
  int count = 0;
  for (int i = 0; i < serverCount; i++) {
    const OpdsServer& server = servers[static_cast<size_t>(i)];
    // Primary label: server name (falling back to URL if unnamed).
    // Subtitle: the URL, only when the name is set.
    rowItems_[count++] = makeRow(server.name.empty() ? server.url.c_str() : server.name.c_str(),
                                 server.name.empty() ? nullptr : server.url.c_str(), i);
  }
  if (pickerMode) return count;

  rowItems_[count++] = makeRow(tr(STR_ADD_SERVER), nullptr, serverCount);
  rowItems_[count++] =
      makeRow(tr(STR_OPDS_DOWNLOAD_FOLDER),
              SETTINGS.opdsDownloadFolder[0] ? SETTINGS.opdsDownloadFolder : tr(STR_OPDS_SD_ROOT), serverCount + 1);
  rowItems_[count++] = makeRow(tr(STR_OPDS_FILENAME_FORMAT), I18N.get(opdsFormatLabel(SETTINGS.opdsFilenameFormat)),
                               serverCount + 2);
  return count;
}

void OpdsServerListActivity::onBackButton() {
  if (pickerMode) {
    activityManager.goHome();
  } else {
    finish();
  }
}

const char* OpdsServerListActivity::headerTitle() const { return tr(STR_OPDS_SERVERS); }

void OpdsServerListActivity::activateIndex(const int index) {
  nav.selected = index;
  // Activation opens an editor/browser or repaints a new value; a lingering
  // flash would gray an unrelated row.
  app.clearTapFlash();
  handleSelection(index);
  requestUpdate();
}

void OpdsServerListActivity::handleSelection(const int index) {
  const int serverCount = serverRows();

  if (pickerMode) {
    // Picker mode: selecting a server navigates to the OPDS browser
    if (index < serverCount) {
      const auto* server = OPDS_STORE.getServer(static_cast<size_t>(index));
      if (server) {
        activityManager.replaceActivity(
            std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, *server, initialQuery_));
      }
    }
    return;
  }

  // Index layout: [servers 0..serverCount-1], [Add Server], [Download folder], [Filename format].
  if (index == serverCount + 1) {
    auto folderHandler = [this](const ActivityResult& result) {
      if (result.isCancelled) return;
      const auto& kb = std::get<KeyboardResult>(result.data);
      const std::string norm = normalizeFolder(kb.text);
      {
        // The folder row's subtitle points at this buffer while a build draws it.
        RenderLock lock(*this);
        strncpy(SETTINGS.opdsDownloadFolder, norm.c_str(), sizeof(SETTINGS.opdsDownloadFolder) - 1);
        SETTINGS.opdsDownloadFolder[sizeof(SETTINGS.opdsDownloadFolder) - 1] = '\0';
      }
      SETTINGS.saveToFile();
      requestUpdate();
    };
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_OPDS_DOWNLOAD_FOLDER),
                                                std::string(SETTINGS.opdsDownloadFolder), 63, InputType::Text),
        folderHandler);
    return;
  }

  // "Filename format": Confirm cycles through the available formats.
  if (index == serverCount + 2) {
    {
      // The format row's subtitle is chosen from this value while a build draws it.
      RenderLock lock(*this);
      SETTINGS.opdsFilenameFormat =
          static_cast<uint8_t>((SETTINGS.opdsFilenameFormat + 1) % static_cast<uint8_t>(OpdsFilenameFormat::Count));
    }
    SETTINGS.saveToFile();
    requestUpdate();
    return;
  }

  // A server row opens its editor; "Add Server" opens the editor on a new server.
  auto resultHandler = [this](const ActivityResult&) {
    // The editor saved (or deleted) on its own. Reload, and keep the user's place, clamped in case
    // the server under the selection was deleted.
    reloadServers();
    const int itemCount = getItemCount();
    moveSelectionTo(itemCount > 0 ? std::min(nav.selected.load(), itemCount - 1) : 0);
  };
  startActivityForResult(
      std::make_unique<OpdsSettingsActivity>(renderer, mappedInput, index < serverCount ? index : -1),
      resultHandler);
}

void OpdsServerListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = listContentRect();
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(contentRect.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (contentRect.x + contentRect.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (contentRect.y + contentRect.height)),
                  static_cast<int16_t>(contentRect.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const int count = rebuildRowItems();
  if (count == 0) {
    screen.centeredText(tr(STR_NO_SERVERS), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // buttons go through the ListController
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  screen.list(props);
}
```

- [ ] **Step 3: Format**

From the repo root in Git Bash:

```bash
"/c/Program Files/LLVM/bin/clang-format.exe" -i src/activities/settings/OpdsServerListActivity.h src/activities/settings/OpdsServerListActivity.cpp
grep -nE "wasPressed|wasReleased|isPressed|ButtonNavigator|buttonNavigator|GUI\.drawList|mapLabels|drawButtonHints|selectListRow|render\(RenderLock|void loop\(|std::vector" src/activities/settings/OpdsServerListActivity.h src/activities/settings/OpdsServerListActivity.cpp
```

Expected: the grep prints nothing.

- [ ] **Step 4: Build `default`**

In PowerShell:

```powershell
$env:PLATFORMIO_CORE_DIR = 'C:\pio'; & C:\pio\penv\Scripts\pio.exe run -e default 2>&1 | Select-String -Pattern "^(RAM|Flash):|SUCCESS|FAILED|error"
```

Expected: `SUCCESS`. Record Flash and RAM against the previous task's figures (PR 4 head baseline: Flash 6228461 B, RAM 58280 B). Flash should drop: the old `render` (586 B), `loop` (296 B) and `std::function` row lambdas go, while `buildScreen`, `rebuildRowItems` and `activateIndex` are added. If the compiler reports `OpdsServerStore::MAX_SERVERS` as ODR-used without a definition (it should not; C++17 makes `static constexpr` members inline), change the `std::min` in `serverRows()` to `std::min<size_t>(OPDS_STORE.getCount(), size_t{OpdsServerStore::MAX_SERVERS})`.

- [ ] **Step 5: Host suite**

```bash
cmake --build test/build -j 8 -- -k 0 > "$TEMP/hb.log" 2>&1; grep FAILED "$TEMP/hb.log" | grep -v epub_build_inventory
ctest --test-dir test/build -j 8 2>&1 | grep -E "tests passed|tests failed"
```

Expected: the first grep prints nothing; `100% tests passed, 0 tests failed out of 1177`. Neither file is in a host test target, so the count does not change.

- [ ] **Step 6: Commit**

```bash
git add src/activities/settings/OpdsServerListActivity.h src/activities/settings/OpdsServerListActivity.cpp
git commit -m "feat(lists): the OPDS server list follows the list button scheme

The OPDS server list, in Settings and as Home's server picker, is now a
UiListActivity. Up/Down and Left/Right step, a held Up/Down jumps to the
ends, a held Left/Right or a swipe pages, and a long Back goes Home. Both
hint strips are drawn, and the list leaves room for the side boxes. FreeInkUI
draws the rows from a fixed array. Each build refills it with pointers into
the server store and the settings, so nothing is copied per render. The store
reloads under the render lock, and the folder and format edits write under it.

Kept from ours: the search query handed to the catalog, Confirm cycling the
filename format, the selection clamp after the editor returns, and a picker
without an Add Server row.

Adapted from upstream crosspoint-reader's OpdsServerListActivity (develop @ cdac66ffe, src/activities/settings/OpdsServerListActivity.cpp)."
```

- [ ] **Device checklist for this task:**

X4 (buttons):
1. **3.1** Settings → OPDS servers, with two servers (one named, one unnamed):
   - The named server shows its name with the URL as subtitle. The unnamed one shows its URL with no subtitle.
   - Then come "Add Server", "Download folder" (with its folder or "SD root") and "Filename format" (with its format).
   - The header reads "OPDS Servers".
2. **3.2** Up/Down and Left/Right each step one row. Hold Down: the last row ("Filename format"). Hold Up: the first server.
3. **3.3** Confirm on "Filename format": the subtitle cycles through the three formats. Leave and re-enter: the last one is kept.
4. **3.4** Confirm on "Download folder":
   - Type `books/`: the subtitle shows `/books`.
   - Open it again and clear it: the subtitle shows "SD root".
   - Open it again and press Back in the keyboard: nothing changes.
5. **3.5** Confirm on the second server, rename it, Back: the list shows the new name, and the selection is still on that server.
6. **3.6** Confirm on the last server → Delete server: the list returns without that server, and the selection is on "Add Server" (not on row 0).
7. **3.7** "Add Server" → enter a name → Back: the new server sits above "Add Server", and the selection is on it.
8. **3.8** Back returns to Settings. Long Back goes Home.
9. **3.9** Picker, two or more servers:
   - Home → OPDS browser: servers only, with no "Add Server", folder or format rows.
   - Confirm opens the catalog for that server. Back goes Home. Long Back goes Home.
10. **3.10** Picker with a search, two or more servers: Home → a book card's "search author". Pick a server: the catalog opens on the search results for that author.
11. **3.11** Empty picker (only if Home still offers the OPDS browser with no servers): "No OPDS servers configured" is centred in the list area. Back goes Home.

T5 S3 (touch):

12. **3.12** Tap a row with "Touch list activation" set to select-first: the first tap selects and the second opens. Set to immediately: one tap opens.
13. **3.13** Tap the Back hint box: Back. Long-tap it: Home. Tap the Confirm box on "Filename format": it cycles. Tap the side Down box: one step. Long-tap the side Down box: the last row.
14. **3.14** With enough servers for more than one screen: a swipe up pages, and the selection moves with the page. With a list that fits on one screen, a swipe leaves the screen as it is.
15. **3.15** The side Up/Down boxes do not cover the rows or the header.

### Task 4: `OpdsSettingsActivity` onto `UiListActivity`

The OPDS server editor moves from `Activity` with `GUI.drawList` and `ButtonNavigator` onto `UiListActivity`. Ported from upstream:
- the fixed `fieldRowItems[5]`, with labels set once in the constructor and value pointers refreshed on every build;
- `valueInset = 8`, and labels in small text with up to two lines;
- the URL hint in a band taken with `screen.takeTop(tabBarHeight)`.

Kept from ours: URL validation with its popup, the `MAX_*_LENGTH` limits, the Password and Url keyboards, `addServer` returning the index, and Delete without confirmation.

**How the popup gets into the frame.** Three facts from the code decide the design:
- `BaseTheme::drawPopup` (BaseTheme.cpp:958-980) and `LyraTheme::drawPopup` (LyraTheme.cpp:839-862) both end in `shipPopup()` (BaseTheme.cpp:950-956). That always ships the frame: `displayBuffer()`, or `triggerDisplayAsync()` for `PopupShip::Async`.
- `GfxRenderer::displayBuffer()` swaps the buffers inside `display.displayBuffer()` (lib/GfxRenderer/GfxRenderer.cpp:3031-3036). After a ship, the write buffer holds an older frame.
- `UiListActivity::render()` (UiListActivity.cpp:213-226) always ends in `renderer.displayBuffer()`, after `drawFooter()`.

Today's own `render()` (OpdsSettingsActivity.cpp:227-236) avoids a double ship by calling either `drawPopup(..., false)` or `displayBuffer()`, never both.

Upstream's `drawFooter()` override calls `GUI.drawPopup(renderer, msg)` with the defaults. That gets two things wrong:
- `overlayDisplayedFrame=true` re-seeds the write buffer from the displayed frame, which throws away the frame this render just composed.
- It then ships, and the base's `displayBuffer()` ships a second time from the stale post-swap buffer.

So upstream's version is not ported. A `render()` override would have to copy the base's eight-pass rebuild loop, `publishListWindow()` and `afterUiRender()`, and that copy drifts the next time the base changes (PR 4 added `publishListWindow()` there). The constraints also forbid it.

The plan instead adds one ship mode, `PopupShip::Caller`: `drawPopup` composes the box and does not ship. `drawFooter()` draws the hints, then the popup with `overlayDisplayedFrame=false` and `PopupShip::Caller`. The base's single `displayBuffer()` then ships list, hints and popup together:
- The frame is shipped once.
- It is the frame this pass composed, so it is never stale.
- The z-order is today's: list, then hints, then popup.

What happens to each existing behaviour:
- **Rows: unchanged.**
  - Name, URL, Username, Password, plus "Delete server" for an existing server: 4 or 5 rows.
  - Values: the text, "Not Set" when empty, and `******` for a password.
  - After the first successful save of a new server, the Delete row and the "OPDS Browser" title appear, as today.
- **Selection on entry:** row 0, as today.
- **Keyboards: unchanged.** Same titles. Limits `OpdsServerStore::MAX_NAME_LENGTH`, `MAX_URL_LENGTH`, `MAX_USERNAME_LENGTH` and `MAX_PASSWORD_LENGTH`. Types `InputType::Url` (prefilled `https://` when empty) and `InputType::Password`. Each accepted result saves at once.
- **URL validation: unchanged.**
  - `OpdsServerValidation::normalizeUrl` runs on the result. A rejected URL is not stored and shows "Enter a valid OPDS URL".
  - That text is the existing hard-coded English literal. No new string.
  - The popup stays on every repaint until a save succeeds, as today. No key dismisses it.
- **Save failure: unchanged.** "Error: General failure" stays until the next save succeeds. This covers both a failed save and a failed delete.
- **Delete: unchanged.** No confirmation; `removeServer`, then `finish()`.
- **Back:** `finish()` (base default). **Long Back: base `homeFromList()`, not overridden.** Every edit is already saved when its keyboard closes, and delete is immediate, so Back commits nothing. Going Home skips the list's result handler, which is harmless because the list is destroyed.
- **Popup state:** the `std::string popupMessage` becomes `bool invalidUrlPopup`. Same message and lifetime, no heap.
- **Render-owned data:**
  - `editServer`'s strings (read through `fieldRowItems[].value`), `isNewServer` (row count and title), `showSaveError` and `invalidUrlPopup` are written on the loop task only under `RenderLock`. This covers keyboard results, `saveServer()`, the delete failure and `onEnter()`.
  - The SD writes (`addServer` / `updateServer` / `removeServer`) run outside the lock. They read `editServer`, which only the loop task writes.
  - This closes a race that both today's code and upstream have.
- **Deliberate visual changes (upstream's render):**
  - The URL hint is FreeInkUI small text in its band. Today it is `GUI.drawSubHeader`'s UI font with a rule under it.
  - Row labels are drawn in small text, the value's size, with up to two lines and an 8 px value inset.
  - The header sits at `listHeaderRect()`, and the list inside `listContentRect()`, as on every migrated list.
- **Navigation (deliberate, per spec):** the list scheme and both hint strips, as in Task 3.

**Files:**
- Modify: `src/components/themes/BaseTheme.h` (add `PopupShip::Caller`, comments)
- Modify: `src/components/themes/BaseTheme.cpp` (`shipPopup()` returns early for `Caller`; `LyraTheme::drawPopup` already ships through it, so `LyraTheme.cpp` is unchanged)
- Replace: `src/activities/settings/OpdsSettingsActivity.h` (full file below)
- Replace: `src/activities/settings/OpdsSettingsActivity.cpp` (full file below)

**Interfaces:**
- Consumes, from `src/activities/UiListActivity.h` (checked against the file at b44579413):
  - the protected constructor (default declaration), and `public: void onEnter() override` (called first);
  - the protected pure virtuals `int listCount() const`, `void buildScreen(UiScreen&)` and `void activateIndex(int)`;
  - the protected virtuals `const char* headerTitle() const` and `void drawFooter()` (base version called first);
  - the protected helpers `Rect listContentRect() const` and `void syncListViewport(UiScreen&, freeink::ui::ListProps&, bool hasSubtitle = false)` (default `false`; these rows have no subtitle);
  - `freeink::ui::ListNav nav`, `ACTION_ROW` and `app.clearTapFlash()`.
  - **Not overridden:** `loop()`, `render()`, `selectListRow()`, `homeFromList()`, `onBackButton()`, `drawChrome()`.
- Consumes, from FreeInkUI:
  - `ListItem::label`, `value` and `actionValue`;
  - `ListProps::items`, `count`, `action`, `inputMask`, `valueInset` (`list.h:113`) and `labelText` (`TextStyle`, with `maxLines` at FreeInkUICore.h:538);
  - `Screen::takeTop(int16_t height, int16_t gap = 0)` returning `freeink::ui::Rect` (FreeInkApp.h:82);
  - `Screen::target().text(Rect, const char*, TextStyle)` (FreeInkUICore.h:740), `Rect::inset(Insets)` (FreeInkUICore.h:106), and `theme().headerSidePadding` / `theme().smallText` (FreeInkUICore.h:674, 686);
  - `Screen::spacer` and `setContentMarginFromScreen`.
- Consumes from the app:
  - `OpdsServerStore::addServer` (returns `std::optional<size_t>`), `updateServer`, `removeServer` and `getServer`;
  - `OpdsServerValidation::normalizeUrl(const std::string&)` (returns `std::optional<std::string>`);
  - `GUI.drawPopup(const GfxRenderer&, const char*, bool overlayDisplayedFrame, PopupShip)`.
- Produces: `PopupShip::Caller` in `BaseTheme.h`. A later screen that must put a popup into the frame a `UiListActivity::render()` ships uses it from its `drawFooter()`. The public constructor `OpdsSettingsActivity(GfxRenderer&, MappedInputManager&, int serverIndex = -1)` is unchanged, which is what Task 3 consumes.

- [ ] **Step 1: `PopupShip::Caller` in `src/components/themes/BaseTheme.h`**

Replace:

```cpp
// How drawPopup() ships the frame it just composed.
enum class PopupShip : uint8_t {
  Blocking,  // displayBuffer(): return once the panel has finished painting the box.
  Async,     // triggerDisplayAsync(): return while the panel is still painting, so the caller can
             // start the slow work the popup is announcing. The caller then owes the panel a
             // GfxRenderer::finishDisplayAsync() before it next writes the framebuffer, touches
             // the display, or frees a framebuffer — releaseSecondaryBuffer() in particular does
             // NOT drain a refresh in flight, and X3 re-reads the frame after the waveform.
};
```

with:

```cpp
// How drawPopup() ships the frame it just composed.
enum class PopupShip : uint8_t {
  Blocking,  // displayBuffer(): return once the panel has finished painting the box.
  Async,     // triggerDisplayAsync(): return while the panel is still painting, so the caller can
             // start the slow work the popup is announcing. The caller then owes the panel a
             // GfxRenderer::finishDisplayAsync() before it next writes the framebuffer, touches
             // the display, or frees a framebuffer — releaseSecondaryBuffer() in particular does
             // NOT drain a refresh in flight, and X3 re-reads the frame after the waveform.
  Caller,    // Not shipped: the box is only drawn into the write buffer, and the caller's own
             // displayBuffer() ships it with the rest of the frame. For a popup drawn inside a
             // render that ships itself, such as UiListActivity::render() via drawFooter(): a ship
             // here would swap the buffers, and the render's displayBuffer() would then ship the
             // stale one. Pass overlayDisplayedFrame=false with it, since the caller composed the frame.
};
```

In the same file, replace:

```cpp
  // ship=Async returns while the panel paints; see PopupShip for what the caller then owes.
  virtual Rect drawPopup(const GfxRenderer& renderer, const char* message, bool overlayDisplayedFrame = true,
```

with:

```cpp
  // ship=Async returns while the panel paints; see PopupShip for what the caller then owes.
  // ship=Caller draws without shipping; the caller's displayBuffer() ships the frame.
  virtual Rect drawPopup(const GfxRenderer& renderer, const char* message, bool overlayDisplayedFrame = true,
```

and replace:

```cpp
  // Ships the frame a drawPopup() override just composed, blocking or not. One place so the two
  // popup looks cannot drift on the part that isn't a look at all.
```

with:

```cpp
  // Ships the frame a drawPopup() override just composed, blocking or not, or leaves it to the
  // caller (PopupShip::Caller). One place so the two popup looks cannot drift on the part that
  // isn't a look at all.
```

- [ ] **Step 2: `shipPopup()` in `src/components/themes/BaseTheme.cpp`**

Replace:

```cpp
void BaseTheme::shipPopup(const GfxRenderer& renderer, const PopupShip ship) {
  if (ship == PopupShip::Async) {
```

with:

```cpp
void BaseTheme::shipPopup(const GfxRenderer& renderer, const PopupShip ship) {
  if (ship == PopupShip::Caller) return;  // drawn only; the caller's displayBuffer() ships it
  if (ship == PopupShip::Async) {
```

`LyraTheme::drawPopup` and `BaseTheme::drawBusyIndicator` both end in `shipPopup()`, so they honour `Caller` with no further change. No code switches over `PopupShip` (the callers are `ActivityManager.cpp:1050` and `SleepActivity.cpp:1036`, both `Async`), so no `-Wswitch` fallout.

- [ ] **Step 3: Replace `src/activities/settings/OpdsSettingsActivity.h`**

```cpp
#pragma once

#include <string>

#include "OpdsServerStore.h"
#include "activities/UiListActivity.h"

// Adapted from upstream crosspoint-reader's OpdsSettingsActivity (develop @ cdac66ffe,
// src/activities/settings/OpdsSettingsActivity.cpp).
/**
 * Edit screen for a single OPDS server.
 * Shows Name, URL, Username, Password fields and a Delete option.
 * Used for both adding new servers and editing existing ones.
 */
class OpdsSettingsActivity final : public UiListActivity {
 public:
  /**
   * @param serverIndex Index into OpdsServerStore, or -1 for a new server
   */
  explicit OpdsSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int serverIndex = -1);

  void onEnter() override;

 private:
  int listCount() const override { return getMenuItemCount(); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  // The hints, then the popup (if one is up) over the finished frame, which the base render ships.
  void drawFooter() override;

  int getMenuItemCount() const;
  void handleSelection(int index);
  // Stores one keyboard result in an editServer field, then saves.
  void commitField(std::string& field, const std::string& text);
  bool saveServer();
  // The message of the popup this frame carries, or nullptr.
  const char* activePopup() const;

  int serverIndex;
  // Read by the render task: editServer's strings through fieldRowItems[].value, isNewServer for
  // the title and the row count, and the two popup flags. The loop task writes them only under
  // RenderLock.
  OpdsServer editServer;
  bool isNewServer = false;
  bool showSaveError = false;
  bool invalidUrlPopup = false;

  // Row storage: at most 5 rows (Name/URL/Username/Password + Delete, see
  // BASE_ITEMS in the .cpp), so a fixed-capacity array avoids any heap
  // allocation for the row list. Labels are set once in the constructor
  // (they never change); buildScreen() only refreshes the value pointers,
  // which point at editServer's own fields (no new strings built).
  static constexpr int MAX_MENU_ITEMS = 5;
  freeink::ui::ListItem fieldRowItems[MAX_MENU_ITEMS]{};
};
```

- [ ] **Step 4: Replace `src/activities/settings/OpdsSettingsActivity.cpp`**

```cpp
#include "OpdsSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <memory>
#include <optional>

#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
// Editable fields: Name, URL, Username, Password.
// Existing servers also show a Delete option (BASE_ITEMS + 1).
constexpr int BASE_ITEMS = 4;
constexpr char INVALID_OPDS_URL_MESSAGE[] = "Enter a valid OPDS URL";
}  // namespace

OpdsSettingsActivity::OpdsSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const int serverIndex)
    : UiListActivity("OpdsSettings", renderer, mappedInput), serverIndex(serverIndex) {
  static_assert(BASE_ITEMS + 1 == MAX_MENU_ITEMS, "Name, URL, Username, Password and Delete");
  // Labels never change (unlike the values, which track editServer's fields
  // live), so they're set once here rather than every buildScreen() call.
  static constexpr StrId fieldNames[BASE_ITEMS] = {StrId::STR_SERVER_NAME, StrId::STR_OPDS_SERVER_URL,
                                                   StrId::STR_USERNAME, StrId::STR_PASSWORD};
  for (int i = 0; i < BASE_ITEMS; i++) {
    fieldRowItems[i].label = I18N.get(fieldNames[i]);
    fieldRowItems[i].actionValue = static_cast<int16_t>(i);
  }
  fieldRowItems[BASE_ITEMS].label = tr(STR_DELETE_SERVER);
  fieldRowItems[BASE_ITEMS].actionValue = static_cast<int16_t>(BASE_ITEMS);
}

int OpdsSettingsActivity::getMenuItemCount() const {
  return isNewServer ? BASE_ITEMS : BASE_ITEMS + 1;  // +1 for Delete
}

void OpdsSettingsActivity::onEnter() {
  UiListActivity::onEnter();

  // All of this is read by the render task; set it under the lock so a pass that starts before
  // onEnter() returns never sees a half-copied server.
  RenderLock lock(*this);
  isNewServer = (serverIndex < 0);
  showSaveError = false;
  invalidUrlPopup = false;

  if (!isNewServer) {
    // Edit flow: copy the selected server into local editable state.
    // Changes are persisted field-by-field through saveServer().
    const auto* server = OPDS_STORE.getServer(static_cast<size_t>(serverIndex));
    if (server) {
      editServer = *server;
    } else {
      // Server was deleted between navigation and entering this screen — treat as new
      isNewServer = true;
      serverIndex = -1;
    }
  }
}

void OpdsSettingsActivity::activateIndex(const int index) {
  nav.selected = index;
  // Activation opens a keyboard or leaves the screen; a lingering flash would
  // gray an unrelated row.
  app.clearTapFlash();
  handleSelection(index);
}

bool OpdsSettingsActivity::saveServer() {
  // The store copies editServer, which only this task writes, so the save itself runs outside the
  // render lock (it is SD I/O). What the render task reads changes under the lock below.
  bool success = false;
  std::optional<size_t> insertedIndex;

  if (isNewServer) {
    // Create flow: first save inserts a new server record into the multi-server store.
    insertedIndex = OPDS_STORE.addServer(editServer);
    success = insertedIndex.has_value();
    if (!success) {
      LOG_ERR("OPS", "Failed to add OPDS server");
    }
  } else {
    // Edit flow: update the same server entry in-place.
    success = OPDS_STORE.updateServer(static_cast<size_t>(serverIndex), editServer);
    if (!success) {
      LOG_ERR("OPS", "Failed to update OPDS server at index %d", serverIndex);
    }
  }

  {
    RenderLock lock(*this);
    if (insertedIndex) {
      // After the first successful save, promote to an existing server so
      // subsequent field edits update in-place rather than creating duplicates.
      isNewServer = false;
      serverIndex = static_cast<int>(*insertedIndex);
    }
    showSaveError = !success;
    if (success) invalidUrlPopup = false;
  }
  if (!success) {
    requestUpdate();
  }

  return success;
}

void OpdsSettingsActivity::commitField(std::string& field, const std::string& text) {
  {
    // The render task reads this string through fieldRowItems[].value, and the assignment may
    // reallocate it.
    RenderLock lock(*this);
    field = text;
  }
  saveServer();
  requestUpdate();
}

void OpdsSettingsActivity::handleSelection(const int index) {
  // Each field edit is saved immediately so partially configured servers
  // survive navigation and power-loss scenarios.
  if (index == 0) {
    // Server Name
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) commitField(editServer.name, std::get<KeyboardResult>(result.data).text);
    };
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SERVER_NAME), editServer.name,
                                                OpdsServerStore::MAX_NAME_LENGTH, InputType::Text),
        handler);
  } else if (index == 1) {
    // Server URL
    const std::string prefillUrl = editServer.url.empty() ? "https://" : editServer.url;
    auto handler = [this](const ActivityResult& result) {
      if (result.isCancelled) return;
      const auto normalizedUrl = OpdsServerValidation::normalizeUrl(std::get<KeyboardResult>(result.data).text);
      if (!normalizedUrl) {
        {
          RenderLock lock(*this);
          invalidUrlPopup = true;
        }
        requestUpdate();
        return;
      }
      {
        RenderLock lock(*this);
        invalidUrlPopup = false;
        editServer.url = *normalizedUrl;
      }
      saveServer();
      requestUpdate();
    };
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_OPDS_SERVER_URL), prefillUrl,
                                                OpdsServerStore::MAX_URL_LENGTH, InputType::Url),
        handler);
  } else if (index == 2) {
    // Username
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) commitField(editServer.username, std::get<KeyboardResult>(result.data).text);
    };
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_USERNAME), editServer.username,
                                                OpdsServerStore::MAX_USERNAME_LENGTH, InputType::Text),
        handler);
  } else if (index == 3) {
    // Password
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) commitField(editServer.password, std::get<KeyboardResult>(result.data).text);
    };
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_PASSWORD), editServer.password,
                                                OpdsServerStore::MAX_PASSWORD_LENGTH, InputType::Password),
        handler);
  } else if (index == BASE_ITEMS && !isNewServer) {
    // Delete flow is only available for existing servers. No confirmation.
    if (!OPDS_STORE.removeServer(static_cast<size_t>(serverIndex))) {
      LOG_ERR("OPS", "Failed to remove OPDS server at index %d", serverIndex);
      {
        RenderLock lock(*this);
        showSaveError = true;
      }
      requestUpdate();
      return;
    }
    finish();
  }
}

void OpdsSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = listContentRect();
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(contentRect.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (contentRect.x + contentRect.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (contentRect.y + contentRect.height)),
                  static_cast<int16_t>(contentRect.x)});

  // URL hint where the old sub-header band sat.
  const fui::Rect band = screen.takeTop(static_cast<int16_t>(metrics.tabBarHeight));
  const int16_t pad = screen.theme().headerSidePadding;
  screen.target().text(band.inset(fui::Insets{0, pad, 0, pad}), tr(STR_CALIBRE_URL_HINT), screen.theme().smallText);
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // fieldRowItems' labels/actionValue were set once in the constructor; only
  // the live value pointers (pointing at editServer's own fields, no new
  // strings built) need refreshing here.
  fieldRowItems[0].value = editServer.name.empty() ? tr(STR_NOT_SET) : editServer.name.c_str();
  fieldRowItems[1].value = editServer.url.empty() ? tr(STR_NOT_SET) : editServer.url.c_str();
  fieldRowItems[2].value = editServer.username.empty() ? tr(STR_NOT_SET) : editServer.username.c_str();
  fieldRowItems[3].value = editServer.password.empty() ? tr(STR_NOT_SET) : "******";

  fui::ListProps props;
  props.items = fieldRowItems;
  props.count = static_cast<uint16_t>(getMenuItemCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // buttons go through the ListController
  props.valueInset = 8;               // air between the value and the row edge
  // Label at the value's font size: both sides of the row read as one unit.
  // maxLines=2 also marks the style caller-owned (see textStyleUnset).
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

const char* OpdsSettingsActivity::headerTitle() const {
  // Reuse STR_OPDS_BROWSER as the "edit existing server" title.
  // New server creation uses STR_ADD_SERVER.
  return isNewServer ? tr(STR_ADD_SERVER) : tr(STR_OPDS_BROWSER);
}

const char* OpdsSettingsActivity::activePopup() const {
  if (invalidUrlPopup) return INVALID_OPDS_URL_MESSAGE;
  if (showSaveError) return tr(STR_ERROR_GENERAL_FAILURE);
  return nullptr;
}

void OpdsSettingsActivity::drawFooter() {
  UiListActivity::drawFooter();
  // On top of the finished frame, hints included. This runs inside UiListActivity::render(), whose
  // own displayBuffer() follows, so the popup must neither ship (that would swap the buffers, and
  // the render's ship would then show the stale one) nor re-seed from the displayed frame (that
  // would discard the frame this pass just composed).
  const char* message = activePopup();
  if (message) GUI.drawPopup(renderer, message, /*overlayDisplayedFrame=*/false, PopupShip::Caller);
}
```

- [ ] **Step 5: Format**

From the repo root in Git Bash:

```bash
"/c/Program Files/LLVM/bin/clang-format.exe" -i src/components/themes/BaseTheme.h src/components/themes/BaseTheme.cpp src/activities/settings/OpdsSettingsActivity.h src/activities/settings/OpdsSettingsActivity.cpp
grep -nE "wasPressed|wasReleased|isPressed|ButtonNavigator|buttonNavigator|GUI\.drawList|mapLabels|drawButtonHints|selectListRow|render\(RenderLock|void loop\(|displayBuffer|popupMessage" src/activities/settings/OpdsSettingsActivity.h src/activities/settings/OpdsSettingsActivity.cpp
grep -n "PopupShip::Caller" src/activities/settings/OpdsSettingsActivity.cpp src/components/themes/BaseTheme.cpp
```

Expected:
- The first grep prints nothing.
- The second prints exactly two lines: the `drawFooter()` call and the `shipPopup()` early return.

- [ ] **Step 6: Build `default`**

In PowerShell:

```powershell
$env:PLATFORMIO_CORE_DIR = 'C:\pio'; & C:\pio\penv\Scripts\pio.exe run -e default 2>&1 | Select-String -Pattern "^(RAM|Flash):|SUCCESS|FAILED|error"
```

Expected: `SUCCESS`. Record Flash and RAM against Task 3's figures. Flash should drop: the old `render` (744 B), `loop` (252 B) and `std::function` row lambdas go, offset by `buildScreen`, `drawFooter`, `commitField` and one compare in `shipPopup`.

- [ ] **Step 7: Host suite**

```bash
cmake --build test/build -j 8 -- -k 0 > "$TEMP/hb.log" 2>&1; grep FAILED "$TEMP/hb.log" | grep -v epub_build_inventory
ctest --test-dir test/build -j 8 2>&1 | grep -E "tests passed|tests failed"
```

Expected: the first grep prints nothing; `100% tests passed, 0 tests failed out of 1177`. Neither `BaseTheme.*` nor the OPDS activities are in a host test target.

- [ ] **Step 8: Commit**

```bash
git add src/components/themes/BaseTheme.h src/components/themes/BaseTheme.cpp src/activities/settings/OpdsSettingsActivity.h src/activities/settings/OpdsSettingsActivity.cpp
git commit -m "feat(lists): the OPDS server editor follows the list button scheme

The OPDS server editor is now a UiListActivity, with the list button scheme
and both hint strips. FreeInkUI draws the rows from a fixed array whose
values point into the server being edited. Keyboard results write those
strings, and the title and popup state, under the render lock; the SD save
runs outside it. The Calibre URL hint is drawn in a band the screen takes
above the list.

The invalid-URL and save-failure popups are drawn over the finished frame in
drawFooter() with a new PopupShip::Caller mode, which draws without shipping.
UiListActivity's own displayBuffer() therefore ships list, hints and popup
once. Upstream's drawFooter() popup would have re-seeded from the displayed
frame and shipped twice.

Kept from ours: URL validation and its popup, the field length limits, the
URL and password keyboards, the index addServer returns, and Delete without
confirmation.

Adapted from upstream crosspoint-reader's OpdsSettingsActivity (develop @ cdac66ffe, src/activities/settings/OpdsSettingsActivity.cpp)."
```

- [ ] **Device checklist for this task:**

X4 (buttons), with the serial log open:
1. **4.1** Settings → OPDS servers → an existing server:
   - The header reads "OPDS Browser".
   - "For Calibre, add /opds to your URL" sits in a band under the header, in small text.
   - Rows: Name, URL, Username, Password with their values on the right (empty ones "Not Set", the password `******`), then "Delete server".
2. **4.2** "Add Server":
   - The header reads "Add Server" and there are 4 rows (no Delete).
   - Enter a name. The row shows it, the header turns to "OPDS Browser", and "Delete server" appears.
3. **4.3** URL row: with an empty URL the keyboard opens in URL mode, prefilled `https://`.
   - Enter text that `normalizeUrl` rejects (e.g. `notaurl`): "Enter a valid OPDS URL" appears over the list. The list and hints are intact around it, and the previous screen does not flash.
   - The serial log shows **one** `Time = ... from clearScreen to displayBuffer` line for that frame, with no second `n/a` line.
   - Press Down: the popup stays and the selection moves (as today).
   - Enter a valid URL: the popup goes, and the row shows the normalized URL.
4. **4.4** Password row: the keyboard masks input. After Confirm, the row shows `******`.
5. **4.5** Save failure: with 8 servers already configured, "Add Server" → enter a name. "Error: General failure" appears. Back returns to the list, which has no 9th server.
6. **4.6** "Delete server": the screen closes at once, with no confirmation. The list no longer shows the server.
7. **4.7** Up/Down and Left/Right step. Hold Down: "Delete server". Hold Up: Name.
8. **4.8** Back returns to the server list. Long Back goes Home. Re-open Settings → OPDS servers: every field entered before the long Back is kept.

T5 S3 (touch):

9. **4.9** Tap a row: it opens its keyboard, following the "Touch list activation" setting. Tap the Back hint box: Back. Long-tap it: Home.
10. **4.10** Tap the side Down box: one step. Long-tap it: the last row. A swipe leaves the 5-row list as it is.
11. **4.11** Repeat 4.3 on T5 S3: the popup frame shows once, with no second refresh and no flash of an older frame.
12. **4.12** The side Up/Down boxes cover neither the URL hint band nor the values.

### Task 5: The weather city search results become a child `UiListActivity`

The results stop being a second mode of the weather menu. They become their own screen, `WeatherCityResultsActivity`, a small picker shaped like `LanguageSelectActivity` and `DictionarySelectionActivity`. Upstream has no weather feature, so no code is ported.

**Verified before writing this task:**
- **A push from inside a result handler is supported.**
  - `ActivityManager.cpp:326-347` moves the handler out of the activity before calling it ("to avoid the case where handler calling another startActivityForResult()"). The loop then `continue`s into the Push branch (`:378-404`).
  - Precedents for the same nesting: the weather menu's keyboard handler already starts the Wi-Fi picker (`WeatherSettingsActivity.cpp:165`), and `SettingsActivity.cpp:272-279` starts an activity from a result handler.
- **The blocking search leaves no stale presses.** It runs inside the handler, so presses made during it are dropped twice: by the `buttonEvents.drain()` after the handler (`ActivityManager.cpp:339`) and by the one after the push (`:401`). No extra drain is needed.
- **The menu keeps its selection.** The menu is not re-entered when the child pops (`ActivityManager.cpp:318-347`: no `onEnter()` on the popped-to activity), and the child has its own `ListNav`. So the menu's selection stays on Location, and #342 stays fixed by construction.
- **`STR_NO_ENTRIES` exists** (`english.yaml:332`, "No entries found"). `OpdsBookBrowserActivity` still uses it after this task.
- **`MenuListActivity` has no `render()` of its own.** Its rows are drawn by `drawMenuList(rect)`, and only the subclass knows the rect. So `WeatherSettingsActivity::render()` stays. `MenuListActivity::loop()` is `UiListActivity::loop()`, so the weather menu's `loop()` override goes. Its `onEnter()`, `onBackPressed()` and `selectListRow()` overrides go too: they existed only for the results mode.
- **The child returns `usesWifi() == true`.** Today the results show inside `WeatherSettingsActivity`, which returns true. Without it, `KOReaderSyncWorker::radioStillOursToTearDown()` (`KOReaderSyncWorker.cpp:98-100`) could treat the radio as its own and turn it off while the list is on screen.

**Deliberate behaviour changes** (for Task 9's notes):
1. **Empty results:** a search with no matches opens the list screen showing "No entries found". There is no Confirm hint and Back returns. Today the menu just reappears with nothing said.
2. **Hints:** the results list draws the side Up/Down boxes and the front `« Up` / `» Down` labels, and its rows narrow for the side gutter. Today it draws the old bottom strip only.
3. **Buttons:** the results list follows the scheme. Left/Right step; hold Left/Right pages; hold Up/Down goes to the first/last row; long Back goes Home; a swipe pages. Today only Up/Down stepped, and every other press was dropped.
4. **Row tap:** rows take taps through FreeInkUI, with the user's tap preference. Today the legacy `ListTouchBand` handled them.
5. **Labels:** a long "name, region, country" label wraps to a second line instead of being cut.

**Files:**
- Create: `src/activities/weather/WeatherCityResultsActivity.h`
- Create: `src/activities/weather/WeatherCityResultsActivity.cpp`
- Replace: `src/activities/weather/WeatherSettingsActivity.h` (full file below)
- Modify: `src/activities/weather/WeatherSettingsActivity.cpp`:
  - includes;
  - delete `onEnter()`, `loop()`, `onBackPressed()` and `selectListRow()`;
  - `launchCitySearch()` plus the new `showCityResults()`;
  - the results branch of `render()`.

**Interfaces:**
- Consumes, from `src/activities/UiListActivity.h` at the PR 4 head:
  - the constructor `UiListActivity(const char*, GfxRenderer&, MappedInputManager&, const ListDeclaration& = {})`, with the default declaration;
  - the pure virtuals `int listCount() const`, `void buildScreen(UiScreen&)` and `void activateIndex(int)`;
  - the overrides `const char* headerTitle() const`, `[[nodiscard]] const char* footerConfirmLabel() const`, `void drawChrome()` and `void onBackButton()`. All are protected virtual and are overridden as private, as `LanguageSelectActivity` does;
  - the helpers `Rect listContentRect() const`, `Rect listHeaderRect() const` and `void syncListViewport(UiScreen&, freeink::ui::ListProps&, bool = false)`;
  - `ACTION_ROW`, `app.clearTapFlash()`, and `Activity::usesWifi() const`.
- Consumes, from FreeInkUI (`components/lists/list.h`):
  - `ListProps::count` (`uint16_t`) and `ListProps::rowProvider` (`void (*)(void*, uint16_t, ListItem&)`, `list.h:81`);
  - `ListProps::rowProviderCtx` (`void*`), `ListProps::action`, `ListProps::inputMask` and `ListProps::labelText.maxLines`;
  - `ListItem::label` (`const char*`) and `ListItem::actionValue` (`int16_t`).
  - `list()` gives each provider call a fresh `ListItem scratch;` (`list.h:599-601`) and reads it before asking for the next row, so one member char buffer is enough. `actionValue` must be set: it is the value a tap reports (`list.h:734`).
- Consumes, from the weather library:
  - `WeatherClient::searchCity(const std::string&) -> std::vector<GeocodingResult>`;
  - `GeocodingResult{name, country, admin1, latitude, longitude}`;
  - `WEATHER_SETTINGS.setLocation(float, float, const std::string&)` and `saveToFile()`.
- Produces:
  - `WeatherCityResultsActivity(GfxRenderer&, MappedInputManager&, std::vector<GeocodingResult>)`. Confirm on a row finishes with a non-cancelled `ActivityResult`; Back finishes with `isCancelled = true`.
  - `WeatherSettingsActivity::showCityResults(const std::string&)` (private).
- Removed from `WeatherSettingsActivity`:
  - the overrides `onEnter()`, `loop()`, `selectListRow(int)` and `onBackPressed()`;
  - the members `searchResults`, `showingSearchResults`, `resultIndex`, `resultsNavigator` and `searchQuery`.

- [ ] **Step 1: Create `src/activities/weather/WeatherCityResultsActivity.h`**

```cpp
#pragma once

#include <WeatherData.h>

#include <cstdint>
#include <vector>

#include "activities/UiListActivity.h"

// The places a weather city search matched (at most five: the geocoding query asks for that many).
// Confirm stores the picked place as the weather location and returns; Back returns without
// changing it. An empty search opens it too, so it can say that nothing matched.
class WeatherCityResultsActivity final : public UiListActivity {
 public:
  WeatherCityResultsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                             std::vector<GeocodingResult> results);

  // Opened right after the search with the radio still up for the weather flow, as the weather
  // menu that opens it is: the KOReader sync worker must not take the radio down under it.
  bool usesWifi() const override { return true; }

 private:
  int listCount() const override { return static_cast<int>(cities.size()); }
  const char* headerTitle() const override;
  [[nodiscard]] const char* footerConfirmLabel() const override;
  void drawChrome() override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Back cancels: nothing is stored.
  void onBackButton() override;
  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);

  std::vector<GeocodingResult> cities;
  // The row the list is laying out, as "name, admin1, country". Written by provideRow() on the
  // render task; the list reads it before it asks for the next row.
  char rowLabel[160] = {};
};
```

- [ ] **Step 2: Create `src/activities/weather/WeatherCityResultsActivity.cpp`**

```cpp
#include "WeatherCityResultsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WeatherSettingsStore.h>

#include <cstdio>
#include <utility>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

WeatherCityResultsActivity::WeatherCityResultsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       std::vector<GeocodingResult> results)
    : UiListActivity("WeatherCityResults", renderer, mappedInput), cities(std::move(results)) {}

const char* WeatherCityResultsActivity::headerTitle() const { return tr(STR_WEATHER_SEARCH_RESULTS); }

// Nothing to pick on an empty search, so Confirm shows no hint; Back still returns.
const char* WeatherCityResultsActivity::footerConfirmLabel() const {
  return cities.empty() ? "" : UiListActivity::footerConfirmLabel();
}

void WeatherCityResultsActivity::drawChrome() {
  UiListActivity::drawChrome();
  if (!cities.empty()) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = listContentRect();
  const Rect header = listHeaderRect();
  const int top = header.y + header.height + metrics.verticalSpacing;
  const int bottom = content.y + content.height;
  renderer.drawCenteredText(UI_10_FONT_ID, top + (bottom - top) / 2, tr(STR_NO_ENTRIES));
}

void WeatherCityResultsActivity::provideRow(void* ctx, const uint16_t index, fui::ListItem& item) {
  auto* self = static_cast<WeatherCityResultsActivity*>(ctx);
  const GeocodingResult& city = self->cities[index];
  // "name, admin1, country", leaving out the parts the geocoder did not return.
  snprintf(self->rowLabel, sizeof(self->rowLabel), "%s%s%s%s%s", city.name.c_str(), city.admin1.empty() ? "" : ", ",
           city.admin1.c_str(), city.country.empty() ? "" : ", ", city.country.c_str());
  item.label = self->rowLabel;
  item.actionValue = static_cast<int16_t>(index);
}

void WeatherCityResultsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = listContentRect();
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(contentRect.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (contentRect.x + contentRect.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (contentRect.y + contentRect.height)),
                  static_cast<int16_t>(contentRect.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  // No rows to lay out: drawChrome() says that nothing matched.
  if (cities.empty()) return;

  fui::ListProps props;
  props.count = static_cast<uint16_t>(cities.size());
  props.rowProvider = &WeatherCityResultsActivity::provideRow;
  props.rowProviderCtx = this;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void WeatherCityResultsActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  const GeocodingResult& city = cities[static_cast<size_t>(index)];
  WEATHER_SETTINGS.setLocation(city.latitude, city.longitude, city.name + ", " + city.country);
  WEATHER_SETTINGS.saveToFile();
  ActivityResult picked;  // not cancelled: the weather menu repaints its Location row
  setResult(std::move(picked));
  finish();
}

void WeatherCityResultsActivity::onBackButton() {
  ActivityResult cancelled;
  cancelled.isCancelled = true;
  setResult(std::move(cancelled));
  finish();
}
```

Notes for the implementer:
- **Long Back:** no `homeFromList()` override. Back commits nothing here, and no reader is below the weather settings, so long Back going Home is the scheme.
- **The stored location name stays "name, country"**, without admin1, exactly as `WeatherSettingsActivity.cpp:53` does today.

- [ ] **Step 3: Replace `src/activities/weather/WeatherSettingsActivity.h`**

```cpp
#pragma once

#include <string>

#include "../MenuListActivity.h"

/**
 * Settings submenu for weather configuration.
 * Supports city search via geocoding, manual lat/lon entry, and unit selection.
 * The search's matches open as their own list, WeatherCityResultsActivity.
 */
class WeatherSettingsActivity final : public MenuListActivity {
 public:
  explicit WeatherSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : MenuListActivity("WeatherSettings", renderer, mappedInput) {
    buildMenuItems();
  }

  bool usesWifi() const override { return true; }
  // MenuListActivity draws its rows through drawMenuList(rect) and has no render() of its own:
  // the rect, under this screen's header, is only known here.
  void render(RenderLock&&) override;

 private:
  void buildMenuItems();
  void onActionSelected(int index) override;
  std::string getItemValueString(int index) const override;
  void onSettingToggled(int index) override;

  void launchCitySearch();
  // Searches for `query` and opens the matches as their own list.
  void showCityResults(const std::string& query);
  void launchLatitudeEntry();
  void launchLongitudeEntry();
};
```

- [ ] **Step 4: `WeatherSettingsActivity.cpp`, includes**

Replace:

```cpp
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
```

with:

```cpp
#include "MappedInputManager.h"
#include "WeatherCityResultsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
```

(`fontIds.h` served only the results branch's `UI_10_FONT_ID`.)

- [ ] **Step 5: `WeatherSettingsActivity.cpp`, delete `onEnter()` and `loop()`**

Replace:

```cpp
void WeatherSettingsActivity::onEnter() {
  MenuListActivity::onEnter();
  showingSearchResults = false;
}

void WeatherSettingsActivity::loop() {
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

  MenuListActivity::loop();
}

std::string WeatherSettingsActivity::getItemValueString(int index) const {
```

with:

```cpp
std::string WeatherSettingsActivity::getItemValueString(int index) const {
```

- [ ] **Step 6: `WeatherSettingsActivity.cpp`, `onBackPressed()`, `selectListRow()`, and one search path**

Replace:

```cpp
void WeatherSettingsActivity::onBackPressed() {
  if (showingSearchResults) {
    showingSearchResults = false;
    requestUpdate();
    return;
  }
  finish();
}

ListRowTap::Result WeatherSettingsActivity::selectListRow(const int index) {
  if (!showingSearchResults) return MenuListActivity::selectListRow(index);
  return ListRowTap::apply(index, static_cast<int>(searchResults.size()), resultIndex);
}

void WeatherSettingsActivity::launchCitySearch() {
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_WEATHER_SEARCH_CITY), "",
                                                                 64, InputType::Text),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) return;

                           const auto& kb = std::get<KeyboardResult>(result.data);
                           if (kb.text.empty()) return;

                           if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
                             startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                                                    [this, query = kb.text](const ActivityResult& wifiResult) {
                                                      if (wifiResult.isCancelled) return;
                                                      searchResults = WeatherClient::searchCity(query);
                                                      showingSearchResults = !searchResults.empty();
                                                      resultIndex = 0;
                                                      requestUpdate();
                                                    });
                           } else {
                             searchResults = WeatherClient::searchCity(kb.text);
                             showingSearchResults = !searchResults.empty();
                             resultIndex = 0;
                             requestUpdate();
                           }
                         });
}
```

with:

```cpp
void WeatherSettingsActivity::launchCitySearch() {
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_WEATHER_SEARCH_CITY), "",
                                                                 64, InputType::Text),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) return;

                           const auto& kb = std::get<KeyboardResult>(result.data);
                           if (kb.text.empty()) return;

                           if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
                             startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                                                    [this, query = kb.text](const ActivityResult& wifiResult) {
                                                      if (!wifiResult.isCancelled) showCityResults(query);
                                                    });
                             return;
                           }
                           showCityResults(kb.text);
                         });
}

// The search is a blocking request on the loop task, as it always was; the matches then open as
// their own list, also when there are none, so an empty search says so. This runs inside a result
// handler (the keyboard's, or the Wi-Fi picker's). ActivityManager moves a handler out before
// running it, so starting another activity from one is supported, the same way the keyboard handler
// above starts the Wi-Fi picker.
void WeatherSettingsActivity::showCityResults(const std::string& query) {
  startActivityForResult(
      std::make_unique<WeatherCityResultsActivity>(renderer, mappedInput, WeatherClient::searchCity(query)),
      [this](const ActivityResult&) { requestUpdate(); });  // the Location row shows a pick
}
```

- [ ] **Step 7: `WeatherSettingsActivity.cpp`, `render()` loses the results branch**

Replace:

```cpp
  if (showingSearchResults) {
    closeRouting();
    GUI.drawHeader(renderer, listHeaderRect(), tr(STR_WEATHER_SEARCH_RESULTS));

    const int contentTop = contentRect.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight =
        contentRect.height - (metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2);

    if (searchResults.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight / 2, tr(STR_NO_ENTRIES));
    } else {
      GUI.drawList(
          renderer, Rect(contentRect.x, contentTop, contentRect.width, contentHeight),
          static_cast<int>(searchResults.size()), resultIndex,
          [this](int index) {
            const auto& r = searchResults[index];
            std::string label = r.name;
            if (!r.admin1.empty()) label += ", " + r.admin1;
            if (!r.country.empty()) label += ", " + r.country;
            return label;
          },
          nullptr, nullptr, nullptr, true);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  GUI.drawHeader(renderer, listHeaderRect(), tr(STR_WEATHER_SETTINGS));
```

with:

```cpp
  GUI.drawHeader(renderer, listHeaderRect(), tr(STR_WEATHER_SETTINGS));
```

Then check that nothing of the old mode is left:

```bash
cd /c/_development/witchhunt-reader
grep -n "showingSearchResults\|searchResults\|resultIndex\|resultsNavigator\|searchQuery\|drawList\|mapLabels\|closeRouting\|selectListRow\|onBackPressed\|::loop\|::onEnter" src/activities/weather/WeatherSettingsActivity.h src/activities/weather/WeatherSettingsActivity.cpp
```

Expected: no matches.

- [ ] **Step 8: Format**

```bash
cd /c/_development/witchhunt-reader
for f in src/activities/weather/WeatherCityResultsActivity.h src/activities/weather/WeatherCityResultsActivity.cpp src/activities/weather/WeatherSettingsActivity.h src/activities/weather/WeatherSettingsActivity.cpp; do "/c/Program Files/LLVM/bin/clang-format.exe" -i "$f"; done
```

- [ ] **Step 9: Build `default`**

```powershell
$env:PLATFORMIO_CORE_DIR = 'C:\pio'; & C:\pio\penv\Scripts\pio.exe run -e default 2>&1 | Select-String -Pattern "^(RAM|Flash):|SUCCESS|FAILED|error"
```

Expected: SUCCESS. Record Flash and RAM, and the delta against the previous task's numbers.
- Research B §D estimates the net at about -0.5 to +0.5 KB. Removed: the results half of `render()` (~0.4 KB), the `loop()` branch (~0.3 KB), `selectListRow`, the duplicate search callback, and the 224 B of `GUI.drawList` row thunks. Added: one small class and its vtable.
- If it comes out positive, name it in the report. The function it buys is the empty-results screen plus the scheme on this list.
- If the compiler reports "marked 'override', but does not override", recheck the signature against `UiListActivity.h`. They are listed under Interfaces above.

- [ ] **Step 10: Host suite**

```bash
cd /c/_development/witchhunt-reader
cmake --build test/build -j 8 -- -k 0 > "$TEMP/hb.log" 2>&1; grep FAILED "$TEMP/hb.log" | grep -v epub_build_inventory
ctest --test-dir test/build -j 8 2>&1 | grep -E "tests passed|tests failed"
```

Expected: no FAILED lines; `100% tests passed out of 1177`. The host suite compiles no activity, so the count does not change.

- [ ] **Step 11: Commit**

```bash
cd /c/_development/witchhunt-reader
git add src/activities/weather/WeatherCityResultsActivity.h src/activities/weather/WeatherCityResultsActivity.cpp src/activities/weather/WeatherSettingsActivity.h src/activities/weather/WeatherSettingsActivity.cpp
git commit -m "feat(lists): the weather city results are a list screen of their own

A city search's matches open as WeatherCityResultsActivity, a small
UiListActivity child, instead of a second mode inside the weather menu. The
list follows the list button scheme (Left/Right step, hold Left/Right to
page, hold Up/Down for the ends, a swipe pages), draws both hint strips, and
takes row taps through FreeInkUI. Confirm stores the place as before; Back
returns without changing it. A search that matches nothing now opens the
list too and says 'No entries found' instead of silently showing the menu
again. The weather menu loses its results branch, its own loop(),
onEnter(), onBackPressed() and selectListRow(), and the second copy of the
search call."
```

- [ ] **Device checklist for this task:**
  1. **5.1 X4:**
     - Do: open Settings → Weather Settings → Location, then search a real city (for example "Springfield").
     - Expect: the "Search Results" list opens. Rows read "name, region, country", and a long one wraps to a second line. The side Up/Down boxes are drawn, and the front boxes read `« Up` / `» Down`.
  2. **5.2 X4:**
     - Do: step with Up/Down and with Left/Right; double-tap Down; hold Down; hold Up; hold Right; hold Left.
     - Expect: Up/Down and Left/Right step and wrap. With five rows the list fits one screen, so a double-tap of Down moves two rows. Hold Down or hold Right selects the last row; hold Up or hold Left selects the first.
  3. **5.3 X4:**
     - Do: press Confirm on a row.
     - Expect: back on the weather menu, with no extra press needed. The Location row shows "name, country" and the selection is still on Location (#342). Latitude and Longitude show the new coordinates.
  4. **5.4 X4:**
     - Do: on the results press Back; then search again and long-press Back.
     - Expect: Back returns to the weather menu with Location unchanged. Long Back goes Home.
  5. **5.5 X4:**
     - Do: search a nonsense string ("qqqzzzxx"), then press Confirm, then Back.
     - Expect: the list screen shows "No entries found", with no Confirm hint and the side boxes still drawn. Confirm does nothing. Back returns to the weather menu.
  6. **5.6 X4, Wi-Fi off:**
     - Do: Location → type a city → Wi-Fi picker → connect. Then repeat and cancel the Wi-Fi picker.
     - Expect: after connecting, the results open (a push from a nested result handler). After cancelling, the weather menu returns with no results screen.
  7. **5.7 X4:**
     - Do: open the weather display (landscape) → Confirm opens the settings → search.
     - Expect: the results list draws in portrait like the menu. Back out to the weather display: it is landscape again.
  8. **5.8 T5S3:**
     - Do: on the results, tap the Back box; long-tap the Back box; tap and long-tap the side Up/Down boxes; swipe up and down; tap a row.
     - Expect: the Back box tap returns to the weather menu, and its long tap goes Home. The side boxes step, and a long tap jumps to the first/last row. A swipe pages (with five rows, to the last/first row). A row tap selects or sets the location, per the touch-activation setting. On the empty-results screen only Back does anything.

### Task 6: `FinishedBookActivity` onto `UiListActivity`

`FinishedBookActivity` (src/activities/reader/FinishedBookActivity.*) keeps its result protocol, its `BookFinished::` helpers and `launchFinishedBookFlow` unchanged. What moves:
- The three per-tick vectors become a fixed action array (`actions_[6]` plus a count) read by a `rowProvider`.
- The three toggles become switches.
- The header, the two text lines and the next-book panel become chrome above the list.
- Input goes through the controller.

This follows upstream's `EndOfBookOptions` (fixed row store built once, `InputTouch`, list margin below the panel) in pattern only. That class is an in-reader overlay with different rows and level-based input, so no code is taken from it.

**Verified before writing this task:**
- **The header comment's claim is stale.** It says Up/Down/Left/Right never arrive as events while this screen is on the reader stack. It is false:
  - `main.cpp:1859-1865`: Up and Down have no global mapping (`actionFor` returns `BTN_DEFAULT`). They always fall through to the activity.
  - `main.cpp:2041`: an event falls through when its action is `BTN_DEFAULT` or reader-scoped while `!activityManager.currentIsReaderActivity()`. With this screen on top, `currentIsReaderActivity()` is false (`ActivityManager.h:209-213`). So reader-scoped bindings fall through too: the defaults short Left/Right = `BTN_DEFAULT`, and long Left/Right = `BTN_PREV/NEXT_SECTION` (`CrossPointSettings.h:843-869`, reader-scoped per `CrossPointSettings.cpp:58-80`).
  - Only a user-bound *global* action (Go Home, Sleep, refresh, bookmarks, light, touch toggle, Ignore) is consumed. That is R7, by design.
  - `ListController.cpp:117` drops the `PageBack`/`PageForward` names and steps on their Up/Down aliases.
  - `ButtonEventManager`'s double-press wait asks the same `currentIsReaderActivity()` (`main.cpp:1299`).
  - The comment and the `ButtonNavigator` path go.
- **The render pass structure** (`UiListActivity.cpp:213-226`): `clearScreen`, `drawChrome`, `renderUi`, then up to 8 more rounds of `clearScreen` + `drawChrome` + `renderUi` while the nav asks for a rebuild. After that come `publishListWindow`, `afterUiRender` (once), `drawFooter` and `displayBuffer`.
  - So `drawChrome()` only lays the panel out and draws text. The cover bitmap is opened and drawn in `afterUiRender()`, once per frame.
  - The panel needs the cover's width to place the text. The loop task reads the BMP header once, when the preview loads, and stores the width and height.
- **`homeFromList()` alone is not enough.** `UiListActivity.cpp:167-173` does route it to `onBackButton()` when a reader is anywhere on the stack (`ActivityManager.cpp:676-680`). That covers the reader launches (`EpubReaderActivity.cpp:1122/2578/3709`, `XtcReaderActivity.cpp:195`, `LineReaderActivity.cpp:236`).
  - But the screen has a second launch site with **no reader below**: the file browser's "mark as read" (`FileBrowserActivity.cpp:1471`). There the base would call `onGoHome()` and skip the result handler, and with it the move-to-/COMPLETED and forget switches.
  - Back commits here, so the screen overrides `homeFromList()` to `onBackButton()` unconditionally.
- **The metadata hook is `handleCustomInput()`, returning false.**
  - `onSelectionChanged` fires only when the selection moves, so the preview would never load on a screen the user just reads.
  - `handleCustomInput()` runs at the top of every `loop()`. It starts the load only on a tick where `buttonEvents.isGestureInFlight()` is false: no key held, no double-tap window open, no event queued. That is the rule `EpubReaderActivity.cpp:1875` uses for uninterruptible loop-task work.
  - So the SD read never sits in front of a press. Presses made during it become events on the next `ButtonEventManager::update()` and step the list normally. They were made on this screen, so they are not stale, and no `drain()` follows.
- **Toggles read and write `SETTINGS.*` directly**, as `MenuListActivity`'s toggle rows do. The three mirror booleans always equal the settings (copied in `onEnter`, written together on each flip), so they go.
- **The next-book strings are now written under `RenderLock`.** The render task reads them (the row label and the panel). Today they are written without the lock.

**Deliberate behaviour changes** (for Task 9's notes):
1. **Switches:** the move-to-/COMPLETED, forget and KOReader-sync rows draw a switch instead of the words "ON"/"OFF".
2. **Hints:** both hint strips are drawn, the side Up/Down boxes included, with front `« Up` / `» Down` labels. The text, panel and list narrow for the side gutter.
3. **Buttons:** the scheme applies.
   - Hold Up/Down: first/last row.
   - Hold Left/Right: page (on this one-screen list, the first/last row).
   - Double-tap Up/Down: page.
   - Long Confirm = Confirm.
   - **Long Back = Back** (the GoHome result, so the book is still credited as finished and the switches still apply), from the reader and from the file browser.
   - A swipe pages.
   - Today only short Back/Confirm counted, and steps came from the press log.
4. **Row tap:** taps go through FreeInkUI with the user's tap preference. Today the legacy band handled them.
5. **Layout:**
   - The header sits `topPadding` lower (`listHeaderRect()`, like every other list).
   - The list now reaches down to the hint strip. Today it stopped one hint-strip height short, because it subtracted `buttonHintsHeight` from a rect that already excluded it.
   - The values ("« Home", "Open", "Search") draw in FreeInkUI's value slot without the Lyra value pill.

**Files:**
- Replace: `src/activities/reader/FinishedBookActivity.h` (full file below; the `BookFinished` declarations are unchanged)
- Modify: `src/activities/reader/FinishedBookActivity.cpp`: everything from the `FinishedBookActivity` constructor (line 567) to the end of the file is replaced. The `BookFinished::` helpers above it are untouched.

**Interfaces:**
- Consumes, from `src/activities/UiListActivity.h` at the PR 4 head:
  - the constructor (default declaration);
  - the pure virtuals `int listCount() const`, `void buildScreen(UiScreen&)` and `void activateIndex(int)`;
  - the overrides `const char* headerTitle() const`, `void drawChrome()`, `void afterUiRender()`, `bool handleCustomInput()`, `void onBackButton()` and `void homeFromList()`. All are protected virtual and are overridden as private;
  - the helpers `listContentRect() const`, `listHeaderRect() const` and `syncListViewport(UiScreen&, freeink::ui::ListProps&, bool = false)`;
  - `ACTION_ROW` and `app.clearTapFlash()`.
- Consumes, from FreeInkUI:
  - `ListProps::count`, `rowProvider` (`void (*)(void*, uint16_t, ListItem&)`), `rowProviderCtx`, `action`, `inputMask` and `labelText.maxLines`;
  - `ListItem::label`, `value` (`const char*`), `toggle` and `toggleChecked` (`bool`), and `actionValue` (`int16_t`).
- Consumes, elsewhere:
  - `ButtonEventManager::isGestureInFlight() const` and `RenderLock(Activity&)`;
  - `UITheme::getCoverThumbPath(std::string, int, int)`, `Bitmap(FsFile&)` / `parseHeaders()` / `getWidth()` / `getHeight()`, and `GfxRenderer::drawBitmap(const Bitmap&, int, int, int, int)` / `wrappedText(...)`.
- Produces (unchanged for callers):
  - the constructor `FinishedBookActivity(GfxRenderer&, MappedInputManager&, std::string, std::string, std::string = {}, bool = false)`;
  - `MenuResult.action = FinishedBookAction` on every non-cancelled finish;
  - `BookFinished::launchFinishedBookFlow`, `findNextBookInDirectory`, `moveFinishedBookToCompleted` and `FinishedBookAction`.
- The class is now `final` and derives from `UiListActivity`. The `loop()`, `render()` and `selectListRow()` overrides are gone: the base's serve.

- [ ] **Step 1: Replace `src/activities/reader/FinishedBookActivity.h`**

```cpp
#pragma once

#include <cstdint>
#include <string>

#include "CrossPointState.h"
#include "activities/UiListActivity.h"

namespace BookFinished {
std::string findNextBookInDirectory(const std::string& currentBookPath, const std::string& currentBookSeries,
                                    const std::string& currentBookSeriesIndex);

bool moveFinishedBookToCompleted(const std::string& currentBookPath, std::string& outMovedPath);

enum class FinishedBookAction {
  Stay = 0,
  GoHome = 1,
  OpenNextBook = 2,
  SearchOpdsForAuthor = 3,
};

// Launches the finished-book menu on top of `host` and handles its result:
// credits a finish to the reading-stats session, applies the move-to-/COMPLETED
// and remove-from-recents settings, then navigates home or to the next book.
// On cancel/stay the host gets a requestUpdate() to re-render its last page.
// The caller is responsible for persisting reading progress beforehand (the
// progress formats differ per reader).
// `onMenuClosed` (optional, with `onMenuClosedCtx`) runs first in the result
// handler regardless of outcome — readers use it to clear a "menu is open"
// flag. Plain function pointer + context instead of std::function per the
// project callback convention.
// `onSyncToKOReader` (optional, with `onSyncToKOReaderCtx`), when non-null, offers a "sync
// progress to KOReader" toggle (only when KOReader credentials are also configured) alongside
// the move-to-/COMPLETED and forget-book toggles. Unlike those two, applying it isn't a plain
// settings write: it's invoked in place of navigating away directly, with the book's current
// path (which may already be the moved-to-/COMPLETED path — the move/forget settings are still
// applied first, "in parallel" rather than replaced), a KOReaderSyncPostAction describing which
// of Go Home / Open Next / Search OPDS was picked, and that action's target (next-book path or
// OPDS author, empty for Go Home). The callback owns pushing progress and, once its own reboot
// completes, performing that action itself (EpubReaderActivity is the only caller that supplies
// one, since KOReader sync is EPUB-only).
// It returns whether it actually took over: the sync path REPLACES this flow's own navigation, so
// a callback that declines (no credentials, no live Epub to read a position from) must say so or
// the user is left sitting on a dead screen with nothing having happened. False means "I did not
// navigate", and the picked action is performed here as if the toggle had been off.
void launchFinishedBookFlow(Activity& host, GfxRenderer& renderer, MappedInputManager& mappedInput,
                            const std::string& bookPath, const std::string& series, const std::string& seriesIndex,
                            const std::string& author = {}, void (*onMenuClosed)(void*) = nullptr,
                            void* onMenuClosedCtx = nullptr,
                            bool (*onSyncToKOReader)(void*, const std::string& bookPath, KOReaderSyncPostAction,
                                                     const std::string& target) = nullptr,
                            void* onSyncToKOReaderCtx = nullptr);
}  // namespace BookFinished

// What to do now that the book is finished. Pushed by a reader when the last page is turned (see
// launchFinishedBookFlow) and by the file browser's "mark as read". The rows are actions (Home,
// open the next book, search OPDS for the author) and switches (move to /COMPLETED, forget the
// book, sync to KOReader) that the result handler applies once an action is picked. Above the
// list sit the header, two lines of text and a preview of the next book.
class FinishedBookActivity final : public UiListActivity {
 public:
  FinishedBookActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string currentBookPath,
                       std::string nextBookPath, std::string currentBookAuthor = {},
                       bool koReaderSyncAvailable = false);

  void onEnter() override;

 private:
  // The rows that can appear, in display order. Which ones do depends on the book and the settings;
  // rebuildRows() keeps those that apply in actions_.
  enum class Row : uint8_t { GoHome, OpenNext, SearchOpds, ToggleMoveToCompleted, ToggleForget, ToggleSyncToKOReader };
  static constexpr int kMaxRows = 6;

  int listCount() const override { return actionCount_; }
  const char* headerTitle() const override;
  void drawChrome() override;
  void buildScreen(UiScreen& screen) override;
  void afterUiRender() override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  // Back means "done with this book": a GoHome result, which credits the finish and applies the
  // switches. It never cancels.
  void onBackButton() override;
  // Long Back is Back here, also when no reader is below (the file browser's "mark as read"),
  // where going Home directly would skip the switches.
  void homeFromList() override;

  void rebuildRows();
  void loadNextBookPreview();
  int layoutNextBookPreview(const Rect& content, int top);
  void finishWith(BookFinished::FinishedBookAction action);
  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);

  std::string currentBookPath_;
  std::string nextBookPath_;
  std::string currentBookAuthor_;
  // Read by the render task (the next-book row and the panel above the list); written on the loop
  // task, under RenderLock, once the preview has loaded.
  std::string nextBookTitle_;
  std::string nextBookAuthor_;
  std::string nextBookSeries_;
  std::string nextBookCoverPath_;
  int coverWidth_ = 0;  // the cover bitmap's own size; 0 = no cover to show
  int coverHeight_ = 0;
  // "Search OPDS for author: <author>", built with the rows.
  std::string opdsLabel_;
  Row actions_[kMaxRows]{};
  uint8_t actionCount_ = 0;
  // Laid out by drawChrome() on every pass and read in the same frame: the list's top edge by
  // buildScreen(), the cover's place by afterUiRender().
  int listTop_ = 0;
  int coverX_ = 0;
  int coverY_ = 0;
  int coverW_ = 0;
  int coverH_ = 0;
  bool nextBookAvailable_ = false;
  bool nextBookMetadataLoaded_ = false;
  bool koReaderSyncAvailable_ = false;
};
```

- [ ] **Step 2: `FinishedBookActivity.cpp`, cut the old activity**

Check where the old activity starts, then delete from there to the end of the file:

```bash
cd /c/_development/witchhunt-reader
f=src/activities/reader/FinishedBookActivity.cpp
sed -n '565p;567p' "$f"
```

Expected output, exactly two lines:

```
}  // namespace BookFinished
FinishedBookActivity::FinishedBookActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
```

Only if both match:

```bash
sed -i '567,$d' "$f"
tail -n 3 "$f"
```

Expected: the file now ends with `}  // namespace BookFinished` followed by one empty line.

- [ ] **Step 3: `FinishedBookActivity.cpp`, append the new activity**

Append this at the end of the file, after `}  // namespace BookFinished` and its blank line. Use the editor, not a heredoc. The includes at the top of the file already cover everything used here: `Bitmap.h`, `HalStorage.h`, `I18n.h`, `OpdsServerStore.h`, `components/UITheme.h` and `fontIds.h`.

```cpp
namespace fui = freeink::ui;

FinishedBookActivity::FinishedBookActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           std::string currentBookPath, std::string nextBookPath,
                                           std::string currentBookAuthor, bool koReaderSyncAvailable)
    : UiListActivity("FinishedBook", renderer, mappedInput),
      currentBookPath_(std::move(currentBookPath)),
      nextBookPath_(std::move(nextBookPath)),
      currentBookAuthor_(std::move(currentBookAuthor)),
      nextBookAvailable_(!nextBookPath_.empty()),
      nextBookMetadataLoaded_(nextBookPath_.empty()),
      koReaderSyncAvailable_(koReaderSyncAvailable) {
  // Until the preview has loaded, the next-book row names the file.
  if (nextBookAvailable_) nextBookTitle_ = getFilename(nextBookPath_);
}

void FinishedBookActivity::onEnter() {
  rebuildRows();
  UiListActivity::onEnter();
}

// The rows that apply, in display order. None of the conditions changes while the screen is open,
// so this runs once on entry and again, under the lock, when the preview arrives; it allocates only
// the OPDS label, once.
void FinishedBookActivity::rebuildRows() {
  int count = 0;
  actions_[count++] = Row::GoHome;
  if (nextBookAvailable_) actions_[count++] = Row::OpenNext;
  if (!currentBookAuthor_.empty() && OPDS_STORE.hasServers()) {
    opdsLabel_.assign(tr(STR_SEARCH_OPDS_FOR_AUTHOR));
    opdsLabel_ += ": ";
    opdsLabel_ += currentBookAuthor_;
    actions_[count++] = Row::SearchOpds;
  }
  if (!pathIsInCompleted(currentBookPath_)) actions_[count++] = Row::ToggleMoveToCompleted;
  actions_[count++] = Row::ToggleForget;
  // A switch, not an action, so it composes with whichever of Home / next book / OPDS search is
  // picked instead of replacing it -- see launchFinishedBookFlow.
  if (koReaderSyncAvailable_) actions_[count++] = Row::ToggleSyncToKOReader;
  actionCount_ = static_cast<uint8_t>(count);
}

const char* FinishedBookActivity::headerTitle() const { return tr(STR_FINISHED_BOOK_HEADER); }

void FinishedBookActivity::provideRow(void* ctx, const uint16_t index, fui::ListItem& item) {
  const auto* self = static_cast<const FinishedBookActivity*>(ctx);
  item.actionValue = static_cast<int16_t>(index);
  switch (self->actions_[index]) {
    case Row::GoHome:
      item.label = tr(STR_GO_BACK_TO_HOME);
      item.value = tr(STR_HOME);
      break;
    case Row::OpenNext:
      // The panel above shows the next book's author, series and cover; the row names the book.
      item.label = self->nextBookTitle_.empty() ? tr(STR_OPEN_NEXT_BOOK) : self->nextBookTitle_.c_str();
      item.value = tr(STR_OPEN);
      break;
    case Row::SearchOpds:
      item.label = self->opdsLabel_.c_str();
      item.value = tr(STR_SEARCH);
      break;
    case Row::ToggleMoveToCompleted:
      item.label = tr(STR_MOVE_FINISHED_TO_COMPLETED);
      item.toggle = true;
      item.toggleChecked = SETTINGS.moveFinishedBooksToCompleted != 0;
      break;
    case Row::ToggleForget:
      item.label = tr(STR_FORGET_BOOK);
      item.toggle = true;
      item.toggleChecked = SETTINGS.removeFinishedBooksFromRecents != 0;
      break;
    case Row::ToggleSyncToKOReader:
      item.label = tr(STR_KO_SYNC_FINISHED_BOOK);
      item.toggle = true;
      item.toggleChecked = SETTINGS.syncFinishedBookToKOReader != 0;
      break;
  }
}

void FinishedBookActivity::activateIndex(const int index) {
  if (index < 0 || index >= actionCount_) return;
  switch (actions_[index]) {
    case Row::GoHome:
      finishWith(BookFinished::FinishedBookAction::GoHome);
      return;
    case Row::OpenNext:
      finishWith(BookFinished::FinishedBookAction::OpenNextBook);
      return;
    case Row::SearchOpds:
      finishWith(BookFinished::FinishedBookAction::SearchOpdsForAuthor);
      return;
    case Row::ToggleMoveToCompleted:
      SETTINGS.moveFinishedBooksToCompleted = SETTINGS.moveFinishedBooksToCompleted ? 0 : 1;
      break;
    case Row::ToggleForget:
      SETTINGS.removeFinishedBooksFromRecents = SETTINGS.removeFinishedBooksFromRecents ? 0 : 1;
      break;
    case Row::ToggleSyncToKOReader:
      SETTINGS.syncFinishedBookToKOReader = SETTINGS.syncFinishedBookToKOReader ? 0 : 1;
      break;
  }
  // A switch: the result handler applies it once an action is picked. Saved now, as before, so it
  // is also what the next finished book opens with.
  SETTINGS.saveToFile();
  requestUpdate();
}

void FinishedBookActivity::onBackButton() { finishWith(BookFinished::FinishedBookAction::GoHome); }

void FinishedBookActivity::homeFromList() { onBackButton(); }

void FinishedBookActivity::finishWith(const BookFinished::FinishedBookAction action) {
  app.clearTapFlash();
  MenuResult menuResult;
  menuResult.action = static_cast<int>(action);
  ActivityResult result(menuResult);
  setResult(std::move(result));
  finish();
}

// The next book's preview is read from SD here, on the loop task, once: an OPF parse and maybe a
// cover conversion, which nothing interrupts. It starts only on a tick with no press in flight or
// queued, so it never sits in front of one; presses made during it reach the list on the next tick.
// Returning false hands the tick to the list as usual.
bool FinishedBookActivity::handleCustomInput() {
  if (!nextBookMetadataLoaded_ && !buttonEvents.isGestureInFlight()) loadNextBookPreview();
  return false;
}

void FinishedBookActivity::loadNextBookPreview() {
  const auto metadata = loadNextBookMetadata(nextBookPath_);
  // The panel places the text beside the cover, so it needs the cover's size on every layout pass.
  // Read the header once, here, rather than open the file on the render task each pass.
  std::string coverPath;
  int coverWidth = 0;
  int coverHeight = 0;
  if (!metadata.coverPath.empty()) {
    coverPath = UITheme::getCoverThumbPath(metadata.coverPath, kFinishedBookCoverMaxWidth, kFinishedBookCoverHeight);
    HalFile coverFile = Storage.open(coverPath.c_str());
    if (coverFile) {
      Bitmap bmp(coverFile);
      if (bmp.parseHeaders() == BmpReaderError::Ok && bmp.getWidth() > 0 && bmp.getHeight() > 0) {
        coverWidth = bmp.getWidth();
        coverHeight = bmp.getHeight();
      }
      coverFile.close();
    }
  }
  {
    // The render task reads all of these: the next-book row's label and the panel.
    RenderLock lock(*this);
    nextBookTitle_ = metadata.title.empty() ? getFilename(nextBookPath_) : metadata.title;
    nextBookAuthor_ = metadata.author;
    nextBookSeries_ = metadata.series;
    nextBookCoverPath_ = coverWidth > 0 ? std::move(coverPath) : std::string();
    coverWidth_ = coverWidth;
    coverHeight_ = coverHeight;
    nextBookMetadataLoaded_ = true;
    rebuildRows();
  }
  requestUpdate();
}

// Runs on every layout pass (up to nine a frame): text and placement only, no SD access. The cover
// itself is drawn once, in afterUiRender().
void FinishedBookActivity::drawChrome() {
  UiListActivity::drawChrome();  // the header

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = listContentRect();
  const Rect header = listHeaderRect();
  const int textX = content.x + metrics.contentSidePadding;
  const int lineGap = renderer.getLineHeight(UI_10_FONT_ID);

  int y = header.y + header.height + metrics.verticalSpacing;
  renderer.drawText(UI_10_FONT_ID, textX, y, tr(STR_FINISHED_BOOK_HEADER_LINE1), true, EpdFontFamily::REGULAR);
  y += lineGap + 4;
  renderer.drawText(UI_10_FONT_ID, textX, y, tr(STR_FINISHED_BOOK_HEADER_LINE2), true, EpdFontFamily::REGULAR);
  y += lineGap + 4;

  coverW_ = 0;
  coverH_ = 0;
  if (nextBookAvailable_) {
    renderer.drawText(UI_12_FONT_ID, textX, y, tr(STR_NEXT_BOOK_HEADER), true, EpdFontFamily::BOLD);
    y += lineGap + metrics.verticalSpacing;
    y = layoutNextBookPreview(content, y) + metrics.verticalSpacing;
  }
  listTop_ = y;
}

// The next book's panel: the cover on the left, title / author / series wrapped beside it. Returns
// the panel's bottom edge. The cover is only placed here.
int FinishedBookActivity::layoutNextBookPreview(const Rect& content, const int top) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int left = content.x + metrics.contentSidePadding;
  const int contentBottom = content.y + content.height;
  const int contentWidth = content.width - 2 * metrics.contentSidePadding;
  // Leave room under the panel for about three rows of the list.
  const int previewHeight =
      std::max(0, std::min(kFinishedBookCoverHeight, contentBottom - top -
                                                         3 * (renderer.getLineHeight(UI_12_FONT_ID) + 8) -
                                                         metrics.verticalSpacing));
  const int previewWidth = std::min(contentWidth / 2, kFinishedBookCoverMaxWidth);

  int textX = left;
  if (coverWidth_ > 0 && coverHeight_ > 0 && previewHeight > 0) {
    int height = previewHeight;
    int width = coverWidth_ * height / coverHeight_;
    if (width > previewWidth) {
      width = previewWidth;
      height = coverHeight_ * width / coverWidth_;
    }
    if (width > 0 && height > 0) {
      coverX_ = left;
      coverY_ = top;
      coverW_ = width;
      coverH_ = height;
      textX = left + width + metrics.contentSidePadding;
    }
  }

  const int textWidth = content.x + content.width - metrics.contentSidePadding - textX;
  int infoY = top;
  if (!nextBookTitle_.empty()) {
    for (const auto& line : renderer.wrappedText(UI_12_FONT_ID, nextBookTitle_.c_str(), textWidth, 3)) {
      renderer.drawText(UI_12_FONT_ID, textX, infoY, line.c_str(), true, EpdFontFamily::BOLD);
      infoY += renderer.getLineHeight(UI_12_FONT_ID);
    }
    infoY += 4;
  }
  if (!nextBookAuthor_.empty()) {
    for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, nextBookAuthor_.c_str(), textWidth, 3)) {
      renderer.drawText(UI_10_FONT_ID, textX, infoY, line.c_str(), true);
      infoY += renderer.getLineHeight(UI_10_FONT_ID);
    }
    infoY += 4;
  }
  if (!nextBookSeries_.empty()) {
    for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, nextBookSeries_.c_str(), textWidth, 2)) {
      renderer.drawText(UI_10_FONT_ID, textX, infoY, line.c_str(), true);
      infoY += renderer.getLineHeight(UI_10_FONT_ID);
    }
  }
  return std::max(top + previewHeight, infoY);
}

void FinishedBookActivity::buildScreen(UiScreen& screen) {
  const Rect content = listContentRect();
  // The list starts under the panel drawChrome() laid out on this same pass.
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(listTop_),
                  static_cast<int16_t>(renderer.getScreenWidth() - (content.x + content.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (content.y + content.height)),
                  static_cast<int16_t>(content.x)});

  fui::ListProps props;
  props.count = actionCount_;
  props.rowProvider = &FinishedBookActivity::provideRow;
  props.rowProviderCtx = this;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  // One line per row, as before: up to six rows share the screen with the preview panel, and what a
  // second line would carry (the next book's author and series) is already in the panel.
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 1;
  syncListViewport(screen, props);
  screen.list(props);
}

// Once per frame, after the layout passes: drawChrome() placed the cover, this draws it.
void FinishedBookActivity::afterUiRender() {
  if (coverW_ <= 0 || coverH_ <= 0) return;
  HalFile coverFile = Storage.open(nextBookCoverPath_.c_str());
  if (!coverFile) return;
  Bitmap bmp(coverFile);
  if (bmp.parseHeaders() == BmpReaderError::Ok) renderer.drawBitmap(bmp, coverX_, coverY_, coverW_, coverH_);
  coverFile.close();
}
```

Then check that the old input path and its helpers are gone:

```bash
cd /c/_development/witchhunt-reader
grep -n "ButtonNavigator\|buttonNavigator\|RowModel\|buildRowModel\|drawList\|mapLabels\|selectListRow\|selectedIndex_\|nextBookName_\|never arrive as events" src/activities/reader/FinishedBookActivity.h src/activities/reader/FinishedBookActivity.cpp
```

Expected: no matches.

- [ ] **Step 4: Format**

```bash
cd /c/_development/witchhunt-reader
for f in src/activities/reader/FinishedBookActivity.h src/activities/reader/FinishedBookActivity.cpp; do "/c/Program Files/LLVM/bin/clang-format.exe" -i "$f"; done
```

- [ ] **Step 5: Build `default`**

```powershell
$env:PLATFORMIO_CORE_DIR = 'C:\pio'; & C:\pio\penv\Scripts\pio.exe run -e default 2>&1 | Select-String -Pattern "^(RAM|Flash):|SUCCESS|FAILED|error"
```

Expected: SUCCESS. Record Flash and RAM, and the delta against Task 5's numbers.
- Expect a saving of roughly 1-2 KB.
  - Removed (per Research B §D): `render` 1,626 B, `loop` 1,610 B, `buildRowModel` 808 B, `selectListRow`, and the 148 B of `drawList` row thunks.
  - Added: `drawChrome` + `layoutNextBookPreview`, `buildScreen`, `provideRow`, `activateIndex`, `loadNextBookPreview`, and a `UiListActivity` vtable.
- Four translation units include this header: `EpubReaderActivity.cpp`, `XtcReaderActivity.cpp`, `LineReaderActivity.cpp` and `FileBrowserActivity.cpp`. They now pull in `UiListActivity.h`. A clash there (an ambiguous `Rect` or a macro) shows up as an error in one of them. Fix it at the include site, not by dropping the base.

- [ ] **Step 6: Host suite**

```bash
cd /c/_development/witchhunt-reader
cmake --build test/build -j 8 -- -k 0 > "$TEMP/hb.log" 2>&1; grep FAILED "$TEMP/hb.log" | grep -v epub_build_inventory
ctest --test-dir test/build -j 8 2>&1 | grep -E "tests passed|tests failed"
```

Expected: no FAILED lines; `100% tests passed out of 1177`.

- [ ] **Step 7: Commit**

```bash
cd /c/_development/witchhunt-reader
git add src/activities/reader/FinishedBookActivity.h src/activities/reader/FinishedBookActivity.cpp
git commit -m "feat(lists): the finished-book screen follows the list button scheme

FinishedBookActivity moves onto UiListActivity. Its rows come from a fixed
action array through a rowProvider, instead of three vectors of strings
rebuilt on every loop tick, and the move, forget and KOReader-sync rows are
switches rather than ON/OFF text. The header, the two lines and the
next-book panel are chrome above the list; the cover's size is read once
when the preview loads, and the bitmap is drawn once per frame after the
layout passes. Up/Down/Left/Right reach this screen as events (main.cpp
lets reader-scoped actions through when the reader is not on top), so the
ButtonNavigator path and the comment claiming otherwise go. Back still
finishes with GoHome, and long Back does the same, from the reader and from
the file browser's mark-as-read. The next book's preview still loads on the
loop task, on a tick with no press pending, and its strings are now written
under the render lock."
```

- [ ] **Device checklist for this task:**
  1. **6.1 X4, portrait:**
     - Setup: an EPUB with a next book in its folder, an OPDS server configured, KOReader credentials set.
     - Do: turn past the last page, and press Down right away.
     - Expect: the header, the two lines and "Next book to read:" appear, with the next book's filename in its row. Within about a second the cover, title, author and series appear and the row shows the title. The early Down press steps the list; it is not lost and not doubled.
  2. **6.2 X4:**
     - Do: step with Up/Down and Left/Right; hold Down; hold Up; hold Right; hold Left; double-tap Down.
     - Expect: steps wrap. Hold Down or hold Right selects the last row; hold Up or hold Left selects the first (the list fits one screen). A double-tap of Down moves two rows.
  3. **6.3 X4:**
     - Do: Confirm on Move to /COMPLETED, Forget and Sync to KOReader.
     - Expect: each flips a switch (no ON/OFF words). The new state is still there the next time a book is finished (saved).
  4. **6.4 X4:**
     - Do: with Move and Forget on, press Back. Repeat with another book and long-press Back.
     - Expect: Back goes Home; the book is in /COMPLETED, gone from recents, and counted finished in Reading Stats. Long Back does the same (not a bare Home).
  5. **6.5 X4:**
     - Do: Confirm on "« Home", on the next-book row ("Open"), and on the OPDS row ("Search").
     - Expect: Home; the next book opens; the OPDS search for the author opens. With KOReader sync on, the sync runs first, then the picked action.
  6. **6.6 X4, bindings:**
     - Do: with the defaults (long Left/Right = previous/next section), hold Left and Right on this screen. Then bind long Left to Go Home in the button settings and hold Left.
     - Expect: with the defaults they page (first/last row). With long Left bound to Go Home it goes Home (R7).
  7. **6.7 Landscape (both rotations):**
     - Do: finish a book in a landscape reader.
     - Expect: the header, text and panel stay clear of the hint strips; the bottom/side strips are on the correct edges for the rotation. At least the first rows are visible and stepping scrolls the list. The cover is not drawn over the list.
     - Also try a next book with a long title, author and series. The list may shrink to one or two rows (OPEN above); every row must stay reachable.
  8. **6.8 File browser:**
     - Do: open a book's options → Mark as read.
     - Expect: the screen opens with no KOReader row and no OPDS row. Back goes Home with the switches applied. Long Back does the same.
  9. **6.9 T5S3, in a reader:**
     - Do: tap the Back hint box; long-tap the Back box; tap the Confirm box; tap and long-tap the side Up/Down boxes; swipe up and down; tap a switch row; tap the next-book row.
     - Expect: the Back box tap and long tap both run the Home flow (switches applied). The Confirm box activates the selected row. The side boxes step, and a long tap jumps to the first/last row. A swipe pages to the last/first row. A switch row flips, immediately or on a second tap per the touch-activation setting. The next-book row opens that book.
  10. **6.10 X3:**
      - Do: repeat 6.1 with a large EPUB open.
      - Expect: no allocation failure or abort in the serial log when the screen opens (it now holds a `UiAppHost` while the reader is below; OPEN above). Note the free-heap line if the log prints one.

### Task 7: ReadingStatsBookListActivity onto UiListActivity

The book list moves from `Activity` + `GUI.drawList` + `ButtonNavigator` onto `UiListActivity` with a row provider. The page-aligned row cache becomes a cache that follows the selection and the drawn window. A pure, host-tested helper (`util/ListRowCache.h`) decides which rows to keep.

**Files:**
- Create: `src/util/ListRowCache.h` (header-only, pure)
- Create: `test/list_row_cache/CMakeLists.txt`, `test/list_row_cache/ListRowCacheTest.cpp`
- Modify: `test/CMakeLists.txt` (one `add_subdirectory` line)
- Replace: `src/activities/settings/ReadingStatsBookListActivity.h` and `.cpp` (full files below)

**Interfaces:**
- **Consumes.** Each item was checked against the file it comes from.
  - From `src/activities/UiListActivity.h`, all protected unless marked:
    - the constructor `UiListActivity(const char* name, GfxRenderer&, MappedInputManager&, const ListDeclaration& = {})`;
    - the public `void onEnter() override`;
    - the pure virtuals `virtual int listCount() const = 0`, `virtual void buildScreen(UiScreen& screen) = 0` and `virtual void activateIndex(int index) = 0`;
    - the overridable hooks `virtual const char* headerTitle() const`, `[[nodiscard]] virtual const char* footerConfirmLabel() const` (its base returns `tr(STR_SELECT)`), `virtual bool handleCustomInput()`, `virtual void onSelectionChanged(int)`, `virtual void showPositionPage(int position, int topPosition)` (default: `showRowPage(position, topPosition)`, which calls `onSelectionChanged(row)`) and `virtual freeink::ui::ListNav& activeNav()`;
    - the helpers `[[nodiscard]] Rect listContentRect() const`, `[[nodiscard]] ListWindow publishedWindow() const` and `void syncListViewport(UiScreen&, freeink::ui::ListProps&, bool hasSubtitle = false)`;
    - `static constexpr freeink::ui::ActionId ACTION_ROW`;
    - `app.clearTapFlash()`, reached through the protected base `UiAppHost`.
  - `ListWindow { int top; int drawn; }` (`src/activities/ListController.h:16`). `publishedWindow().drawn` is 1 from `UiListActivity::onEnter()` (`resetPublishedWindow()`) until the first render publishes.
  - From FreeInkUI (`freeink-sdk/libs/ui/FreeInkUI/include/components/lists/list.h`):
    - `ListProps::rowProvider` is `void (*)(void* ctx, uint16_t index, ListItem& item)` (line 81) and `rowProviderCtx` is `void*` (line 82). `list()` calls the provider once per laid-out row into a stack scratch item, and reads that row before the next call (lines 600-603).
    - The row is registered with `item.actionValue` (`frame.hit(…, item.actionValue, …)`), so the provider must set it.
    - `ListItem::label`, `subtitle` and `value` are `const char*`.
    - `ListNav::selected` is a `std::atomic<int>`; `ListNav::requestSelection(int)`.
    - On the screen: `setContentMarginFromScreen(Insets)`, `spacer(int16_t)`, `centeredText(const char*, TextStyle)`, `theme().bodyText` and `list(const ListProps&)` (`FreeInkApp.h`).
  - `int utf8SafeTruncateBuffer(const char* buf, int len)` (`lib/Utf8/Utf8.h:16`).
  - `READING_STATS.querySummary` and `queryBooksAt` are unchanged. `kEntryCount = 100` caps the list at 100 books.
- **Produces** (no later task depends on these):
  - `src/util/ListRowCache.h`:
    - `struct ListRowCache::Range { int first; int last; bool holds(const Range&) const; }`
    - `Range ListRowCache::needed(int count, int selected, int windowTop, int windowDrawn)`
    - `Range ListRowCache::toDecode(int count, int selected, int windowTop, int windowDrawn)`
  - `ReadingStatsBookListActivity`'s public surface is unchanged: the constructor and `onEnter()`. `ReadingStatsActivity.cpp:75` does not change.
- **The design.** The loop task knows the selection and a window: the last render's published window, or the top a page turn has just set.
  - `needed()` is the window the next render draws:
    - the given window while the selection is inside it;
    - otherwise the window the follow scrolls to, which puts the selection on its first line (it moved up) or its last line (it moved down).
  - `toDecode()` is that window plus one window either side, clipped to the list.
  - The rows are re-read only when `needed()` is not held. Stepping then refills about once a screen, and paging about every other page. Both rates are pinned by tests.
  - Rows are still decoded on the loop task and swapped in under `RenderLock`.
- **Deliberate behaviour changes** (user-visible, for the docs task):
  1. Buttons follow the list scheme:
     - short Left/Right steps (it paged a screen);
     - holding Left/Right pages, keeping the selection on its line;
     - a double-tap of Up/Down pages;
     - holding Up/Down jumps to the first or last book;
     - a long Back goes Home (this screen is never above a reader);
     - a swipe pages the selection.
  2. Both hint strips are drawn, including the side Up/Down boxes. The list and the header sit inside the side gutter, so the list is slightly narrower on the X3/X4.
  3. The reading time is drawn in FreeInkUI's value slot. The Lyra value pill is gone (spec §2b).
  4. FreeInkUI draws as many rows as fit. The old `ListTouchBand::kMaxRows` cap is gone.
  5. The decoded cache holds up to three screens. It holds 20 rows when the list opens, against one page (about 8) before.
  6. If the history changes twice in a row while the list reads it (a stale read, a re-read of the order, and another stale read), the rows show blank. Before, the read was retried on the next navigation. Now it is not retried on every tick. The next window outside those rows reads afresh.
  7. On the empty list the Confirm label is blank and the side boxes are drawn. Before, only Back was shown.

- [ ] **Step 1: Write the failing test**

These values were checked by building this exact test against the header in Step 3 with the repo's gtest (12/12 pass).

Why the page top must be passed (OPEN item 1): paging forward with the last render's window and the selection on line 3, the cache misses the new screen's tail on page 3. `needed` is `[20, 28)`, which is held by the cache `[4, 28)`, but the screen drawn is `[24, 32)`. Passing the page's own top is what makes `PagingRefillsAboutEveryOtherPage` below hold.

Create `test/list_row_cache/CMakeLists.txt`:

```cmake
add_executable(ListRowCacheTest
  ListRowCacheTest.cpp
)

target_include_directories(ListRowCacheTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ListRowCacheTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ListRowCacheTest)
```

Create `test/list_row_cache/ListRowCacheTest.cpp`:

```cpp
// Which rows a list that decodes them from storage keeps in memory (src/util/ListRowCache.h, used by
// the reading-stats book list).
//
// The cache must hold every row the next render draws, whatever moved the window: a step the follow
// scrolls after, a page turn that keeps the selection on its line, or nothing at all. And it must
// not refill on every press: each refill reads the rows from the SD card on the loop task.

#include <gtest/gtest.h>

#include <algorithm>

#include "util/ListRowCache.h"

namespace {

using ListRowCache::Range;

void expectRange(const Range& range, const int first, const int last) {
  EXPECT_EQ(range.first, first);
  EXPECT_EQ(range.last, last);
}

}  // namespace

TEST(ListRowCache, EmptyListHoldsNothing) {
  expectRange(ListRowCache::needed(0, 0, 0, 8), 0, 0);
  expectRange(ListRowCache::toDecode(0, 0, 0, 8), 0, 0);
}

TEST(ListRowCache, SelectionOnScreenNeedsTheWindow) {
  // 100 books, eight drawn from row 40, the selection on row 43.
  expectRange(ListRowCache::needed(100, 43, 40, 8), 40, 48);
}

TEST(ListRowCache, DecodesTheWindowAndOneWindowEitherSide) {
  expectRange(ListRowCache::toDecode(100, 43, 40, 8), 32, 56);
}

TEST(ListRowCache, StepPastTheBottomNeedsTheWindowTheFollowScrollsTo) {
  // Down from the last line of [40, 48): the follow puts row 48 on the last line.
  expectRange(ListRowCache::needed(100, 48, 40, 8), 41, 49);
  expectRange(ListRowCache::toDecode(100, 48, 40, 8), 33, 57);
}

TEST(ListRowCache, StepPastTheTopNeedsTheWindowTheFollowScrollsTo) {
  // Up from the first line of [40, 48): the follow puts row 39 on the first line.
  expectRange(ListRowCache::needed(100, 39, 40, 8), 39, 47);
  expectRange(ListRowCache::toDecode(100, 39, 40, 8), 31, 55);
}

TEST(ListRowCache, ClampsToTheList) {
  expectRange(ListRowCache::toDecode(100, 2, 0, 8), 0, 16);
  expectRange(ListRowCache::toDecode(100, 97, 92, 8), 84, 100);
  // A list shorter than a screen is decoded whole.
  expectRange(ListRowCache::needed(5, 2, 0, 8), 0, 5);
  expectRange(ListRowCache::toDecode(5, 2, 0, 8), 0, 5);
}

TEST(ListRowCache, UnmeasuredWindowCountsAsOneRow) {
  expectRange(ListRowCache::needed(100, 10, 10, 0), 10, 11);
  expectRange(ListRowCache::toDecode(100, 10, 10, 0), 9, 12);
  expectRange(ListRowCache::toDecode(100, 10, 10, -3), 9, 12);
}

TEST(ListRowCache, StaleIndexesAreClampedIntoTheList) {
  // The list shrank under a selection and a window from before (a book was removed).
  expectRange(ListRowCache::needed(10, 25, 20, 8), 9, 10);
  expectRange(ListRowCache::toDecode(10, 25, 20, 8), 1, 10);
  expectRange(ListRowCache::needed(10, -4, -2, 8), 0, 8);
}

// Whatever the inputs: the rows decoded hold the rows needed, lie inside the list and come to at
// most three windows; the rows needed hold the selection.
TEST(ListRowCache, DecodeHoldsWhatIsNeededAndStaysSmall) {
  for (int count = 1; count <= 30; ++count) {
    for (int drawn = 1; drawn <= 10; ++drawn) {
      for (int top = 0; top < count; ++top) {
        for (int sel = 0; sel < count; ++sel) {
          const Range needed = ListRowCache::needed(count, sel, top, drawn);
          const Range decode = ListRowCache::toDecode(count, sel, top, drawn);
          ASSERT_TRUE(decode.holds(needed)) << count << " " << sel << " " << top << " " << drawn;
          ASSERT_GE(decode.first, 0);
          ASSERT_LE(decode.last, count);
          ASSERT_LE(decode.last - decode.first, 3 * drawn);
          ASSERT_LE(needed.first, sel);
          ASSERT_GT(needed.last, sel);
        }
      }
    }
  }
}

// Stepping down a 100-row list one row at a time: every screen drawn is held, and the cache is
// refilled about once a screen, never on every step.
TEST(ListRowCache, SteppingDownRefillsAboutOncePerScreen) {
  constexpr int kCount = 100;
  constexpr int kDrawn = 8;
  Range held = ListRowCache::toDecode(kCount, 0, 0, kDrawn);
  int top = 0;
  int refills = 0;
  for (int sel = 1; sel < kCount; ++sel) {
    if (!held.holds(ListRowCache::needed(kCount, sel, top, kDrawn))) {
      held = ListRowCache::toDecode(kCount, sel, top, kDrawn);
      ++refills;
    }
    // The render: the follow scrolls the least it must to show the selection.
    if (sel >= top + kDrawn) top = sel - kDrawn + 1;
    ASSERT_TRUE(held.holds(Range{top, std::min(kCount, top + kDrawn)})) << "row " << sel;
  }
  EXPECT_GE(refills, 1);
  EXPECT_LE(refills, kCount / kDrawn);
}

TEST(ListRowCache, SteppingUpRefillsAboutOncePerScreen) {
  constexpr int kCount = 100;
  constexpr int kDrawn = 8;
  int top = kCount - kDrawn;
  Range held = ListRowCache::toDecode(kCount, kCount - 1, top, kDrawn);
  int refills = 0;
  for (int sel = kCount - 2; sel >= 0; --sel) {
    if (!held.holds(ListRowCache::needed(kCount, sel, top, kDrawn))) {
      held = ListRowCache::toDecode(kCount, sel, top, kDrawn);
      ++refills;
    }
    if (sel < top) top = sel;
    ASSERT_TRUE(held.holds(Range{top, std::min(kCount, top + kDrawn)})) << "row " << sel;
  }
  EXPECT_GE(refills, 1);
  EXPECT_LE(refills, kCount / kDrawn);
}

// Paging forward with the selection kept on its line, the activity passing the page's own top:
// each page is held before its render, and the cache is refilled about every other page.
TEST(ListRowCache, PagingRefillsAboutEveryOtherPage) {
  constexpr int kCount = 100;
  constexpr int kDrawn = 8;
  constexpr int kLine = 3;
  Range held = ListRowCache::toDecode(kCount, kLine, 0, kDrawn);
  int refills = 0;
  int pages = 0;
  for (int top = kDrawn; top < kCount; top += kDrawn) {
    ++pages;
    const int sel = std::min(top + kLine, kCount - 1);
    if (!held.holds(ListRowCache::needed(kCount, sel, top, kDrawn))) {
      held = ListRowCache::toDecode(kCount, sel, top, kDrawn);
      ++refills;
    }
    ASSERT_TRUE(held.holds(Range{top, std::min(kCount, top + kDrawn)})) << "page at " << top;
  }
  EXPECT_LE(refills, (pages + 1) / 2);
}
```

In `test/CMakeLists.txt`, directly after the line `add_subdirectory(list_row_tap)`, add:

```cmake
add_subdirectory(list_row_cache)
```

- [ ] **Step 2: Run it and watch it fail** (Git Bash)

```bash
cmake --build test/build --target ListRowCacheTest 2>&1 | grep -m1 "ListRowCache.h"
```

Expected: `fatal error: util/ListRowCache.h: No such file or directory`. The CMake re-configure that the new `add_subdirectory` triggers runs first; that is expected.

- [ ] **Step 3: Write `src/util/ListRowCache.h`**

```cpp
#pragma once

#include <algorithm>

// Which rows of a list to keep decoded, for a screen that reads its rows from storage a few at a
// time instead of holding them all (ReadingStatsBookListActivity). Pure, so the host suite pins it
// (test/list_row_cache/).
//
// The loop task knows the selection and a window: the one the last render published (top row, rows
// drawn), or the top a page turn has just set. The next render draws that window while the
// selection is in it. Otherwise the selection moved off it by steps, and the follow scrolls the
// least it can: the selection lands on the window's first line (it went up) or its last (it went
// down). Either way the next window holds the selection.
namespace ListRowCache {

// Rows [first, last).
struct Range {
  int first = 0;
  int last = 0;

  [[nodiscard]] bool holds(const Range& other) const { return other.first >= first && other.last <= last; }
};

namespace detail {

// The top of the window the next render draws, and the rows it draws (at least one). `count` > 0.
inline int nextTop(const int count, const int selected, const int windowTop, const int drawn) {
  const int sel = std::clamp(selected, 0, count - 1);
  const int top = std::clamp(windowTop, 0, count - 1);
  if (sel < top) return sel;
  if (sel >= top + drawn) return sel - drawn + 1;
  return top;
}

}  // namespace detail

// The rows the next render draws.
inline Range needed(const int count, const int selected, const int windowTop, const int windowDrawn) {
  if (count <= 0) return {};
  const int drawn = std::max(1, windowDrawn);
  const int top = detail::nextTop(count, selected, windowTop, drawn);
  return {top, std::min(count, top + drawn)};
}

// The rows to decode once needed() is no longer held: that window and one window either side,
// clipped to the list. Stepping then refills about once a screen, and a page turn about every
// other page, rather than on every press.
inline Range toDecode(const int count, const int selected, const int windowTop, const int windowDrawn) {
  if (count <= 0) return {};
  const int drawn = std::max(1, windowDrawn);
  const int top = detail::nextTop(count, selected, windowTop, drawn);
  return {std::max(0, top - drawn), std::min(count, top + 2 * drawn)};
}

}  // namespace ListRowCache
```

- [ ] **Step 4: Run the test and watch it pass**

```bash
cmake --build test/build --target ListRowCacheTest 2>&1 | tail -1
test/build/list_row_cache/ListRowCacheTest.exe | tail -1
```

Expected: `[  PASSED  ] 12 tests.`

- [ ] **Step 5: Replace `src/activities/settings/ReadingStatsBookListActivity.h`**

```cpp
#pragma once

#include <cstdint>
#include <vector>

#include "ReadingStats.h"
#include "activities/UiListActivity.h"

// Phase-2 sub-screen: scrollable list of all books with recorded reading time.
// Sorted by total time descending so the most-read books are easiest to reach.
// Selecting a row pushes ReadingStatsBookDetailActivity.
//
// The history is never loaded: one scan of the stats file gives the order by time (8 bytes a
// book), and only the rows around the window on screen are decoded, each read at its offset
// (util/ListRowCache.h decides which).
class ReadingStatsBookListActivity final : public UiListActivity {
 public:
  explicit ReadingStatsBookListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("ReadingStatsBookList", renderer, mappedInput) {}

  void onEnter() override;

 private:
  // Until the first render publishes how many rows it drew, decode as if this many did. Opening on
  // the first row that decodes twice as many (the window and the one after it): more than any
  // panel shows, so the opening screen is fully decoded.
  static constexpr int kOpeningRows = 10;

  int listCount() const override { return static_cast<int>(index_.size()); }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  [[nodiscard]] const char* footerConfirmLabel() const override;
  // The per-tick coverage check: a render can draw a window the rows were not decoded for.
  bool handleCustomInput() override;
  void onSelectionChanged(int index) override;
  // A page turn knows the top the next render starts from; it hands it to onSelectionChanged().
  void showPositionPage(int position, int topPosition) override;

  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);

  // Rows are decoded on the loop task and swapped in under RenderLock; the render only reads them.
  std::vector<ReadingStatsStore::IndexEntry> index_;
  uint32_t indexSeq_ = 0;  // the history's generation index_ was taken at
  std::vector<BookReadingStats> rows_;
  int rowsFirst_ = 0;
  // The top a page turn in progress has set, for the onSelectionChanged() it calls; -1 otherwise.
  int pageTop_ = -1;
  // Render task only: the row the provider is filling. The list reads a row's strings before it
  // asks for the next row, so one buffer of each serves every row.
  char labelBuf_[128] = {};
  char valueBuf_[24] = {};

  void rebuildIndex();
  // Decodes the rows the next render draws, and a window either side, unless they are held.
  void ensureRowsFor(int selected, int windowTop);
  // The rows the last render drew, or kOpeningRows before any render has published.
  int windowRows() const;
  // Rows [first, first + page) of the index; false when the history changed since it was taken.
  bool readRows(int first, int page, std::vector<BookReadingStats>& rows) const;
  const BookReadingStats* rowAt(int index) const;
};
```

- [ ] **Step 6: Replace `src/activities/settings/ReadingStatsBookListActivity.cpp`**

`readRows()` and `rowAt()` are byte-for-byte the current ones. `formatDuration()` keeps its format strings and writes into a buffer instead of returning a `std::string`.

```cpp
#include "ReadingStatsBookListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>

#include "ReadingStatsBookDetailActivity.h"
#include "components/UITheme.h"
#include "util/ListRowCache.h"

namespace fui = freeink::ui;

namespace {

// Same compact format as the main stats screen ("1h 05m", "3m 07s", "42s") — kept inline rather
// than shared via a header to keep this slice's footprint small. If a third caller appears we'll
// lift it into a util.
void formatDuration(const uint32_t totalSeconds, char* out, const size_t outSize) {
  const uint32_t h = totalSeconds / 3600;
  const uint32_t m = (totalSeconds % 3600) / 60;
  const uint32_t s = totalSeconds % 60;
  if (h > 0) {
    snprintf(out, outSize, "%uh %02um", h, m);
  } else if (m > 0) {
    snprintf(out, outSize, "%um %02us", m, s);
  } else {
    snprintf(out, outSize, "%us", s);
  }
}

}  // namespace

void ReadingStatsBookListActivity::onEnter() {
  UiListActivity::onEnter();
  rebuildIndex();
  // The list opens on its first row, before any render has published a window.
  ensureRowsFor(0, 0);
}

const char* ReadingStatsBookListActivity::headerTitle() const { return tr(STR_READING_STATS_BOOK_LIST); }

const char* ReadingStatsBookListActivity::footerConfirmLabel() const {
  return index_.empty() ? "" : UiListActivity::footerConfirmLabel();
}

void ReadingStatsBookListActivity::rebuildIndex() {
  ReadingStatsStore::Summary summary;
  if (READING_STATS.querySummary(summary, /*withIndex=*/true) != ReadingStatsStore::ReadResult::Ok) {
    summary.byTime.clear();
  }
  {
    RenderLock lock(*this);
    index_ = std::move(summary.byTime);
    indexSeq_ = summary.seq;
    rows_.clear();
    rowsFirst_ = 0;
  }
  // Keep the selection on a row that still exists: the detail screen may have removed the last
  // book. The selection is atomic, and the next build follows it.
  auto& current = activeNav();
  const int last = std::max(0, static_cast<int>(index_.size()) - 1);
  if (current.selected.load() > last) current.requestSelection(last);
  requestUpdate();
}

int ReadingStatsBookListActivity::windowRows() const {
  // UiListActivity::onEnter() resets the published window to one row; a render replaces it.
  const int drawn = publishedWindow().drawn;
  return drawn > 1 ? drawn : kOpeningRows;
}

bool ReadingStatsBookListActivity::readRows(const int first, const int page,
                                            std::vector<BookReadingStats>& rows) const {
  const int last = std::min(first + page, static_cast<int>(index_.size()));
  const auto count = static_cast<size_t>(std::max(0, last - first));
  const auto result = READING_STATS.queryBooksAt(index_, static_cast<size_t>(first), count, indexSeq_, rows);
  if (result == ReadingStatsStore::ReadResult::Stale) return false;
  if (result != ReadingStatsStore::ReadResult::Ok) rows.assign(count, BookReadingStats{});
  for (BookReadingStats& row : rows) row.days.clear();  // a row shows title, author, time and the finished mark
  return true;
}

// Loop task only. `windowTop` is where the next render starts as far as the loop task knows: the
// top the last render published, or the top a page turn has just set.
void ReadingStatsBookListActivity::ensureRowsFor(const int selected, const int windowTop) {
  if (index_.empty()) return;
  const int drawn = windowRows();
  const ListRowCache::Range held{rowsFirst_, rowsFirst_ + static_cast<int>(rows_.size())};
  if (held.holds(ListRowCache::needed(static_cast<int>(index_.size()), selected, windowTop, drawn))) return;

  ListRowCache::Range range = ListRowCache::toDecode(static_cast<int>(index_.size()), selected, windowTop, drawn);
  std::vector<BookReadingStats> rows;
  if (!readRows(range.first, range.last - range.first, rows)) {
    // The history changed under the list. Take the order again, once.
    rebuildIndex();
    if (index_.empty()) return;
    range = ListRowCache::toDecode(static_cast<int>(index_.size()), activeNav().selected.load(), windowTop, drawn);
    if (!readRows(range.first, range.last - range.first, rows)) {
      // Still changing. Hold blank rows rather than read again on every tick; the next window
      // that leaves them reads afresh.
      rows.assign(static_cast<size_t>(range.last - range.first), BookReadingStats{});
    }
  }
  {
    RenderLock lock(*this);
    rows_ = std::move(rows);
    rowsFirst_ = range.first;
  }
  requestUpdate();
}

const BookReadingStats* ReadingStatsBookListActivity::rowAt(const int index) const {
  const int at = index - rowsFirst_;
  return at >= 0 && at < static_cast<int>(rows_.size()) ? &rows_[static_cast<size_t>(at)] : nullptr;
}

bool ReadingStatsBookListActivity::handleCustomInput() {
  // The first frame, a follow that scrolled further than the selection change predicted, or a
  // window that grew (rows without an author fit more): the render drew rows the loop task never
  // decoded, as "…". Decode them here, on the loop task; the render only reads. When the rows are
  // held this is a range compare.
  ensureRowsFor(activeNav().selected.load(), publishedWindow().top);
  return false;
}

void ReadingStatsBookListActivity::onSelectionChanged(const int index) {
  ensureRowsFor(index, pageTop_ >= 0 ? pageTop_ : publishedWindow().top);
}

void ReadingStatsBookListActivity::showPositionPage(const int position, const int topPosition) {
  // The published window is the last render's. A page turn moves the screen by a whole window, so
  // checking the rows against the old one would miss the new screen's far end on every other page.
  pageTop_ = topPosition;
  UiListActivity::showPositionPage(position, topPosition);
  pageTop_ = -1;
}

void ReadingStatsBookListActivity::activateIndex(const int index) {
  const BookReadingStats* row = rowAt(index);
  if (row == nullptr || row->docId.empty()) return;  // not decoded, or a book the history no longer has
  app.clearTapFlash();
  startActivityForResult(std::make_unique<ReadingStatsBookDetailActivity>(renderer, mappedInput, row->docId),
                         [this](const ActivityResult&) {
                           // The detail screen may have removed its book: read the order again,
                           // keeping the selection on the same row where there still is one.
                           rebuildIndex();
                           ensureRowsFor(activeNav().selected.load(), publishedWindow().top);
                         });
}

void ReadingStatsBookListActivity::provideRow(void* ctx, const uint16_t index, fui::ListItem& item) {
  auto* self = static_cast<ReadingStatsBookListActivity*>(ctx);
  item.actionValue = static_cast<int16_t>(index);
  const BookReadingStats* row = self->rowAt(index);
  if (row == nullptr) {
    // Not decoded yet: the loop task's next check reads it and repaints.
    item.label = "…";
    return;
  }
  // Title is the primary label; fall back to docId so a row without metadata is still
  // recognizable. Finished books get a leading checkmark so completions stand out.
  const std::string& name = row->title.empty() ? row->docId : row->title;
  if (row->finishedCount > 0) {
    const int written = snprintf(self->labelBuf_, sizeof(self->labelBuf_), "✓ %s", name.c_str());
    if (written >= static_cast<int>(sizeof(self->labelBuf_))) {
      // Cut on a character boundary, never inside a multi-byte one.
      self->labelBuf_[utf8SafeTruncateBuffer(self->labelBuf_, static_cast<int>(sizeof(self->labelBuf_)) - 1)] = '\0';
    }
    item.label = self->labelBuf_;
  } else {
    item.label = name.c_str();
  }
  // The author, when known; without one the row is a single line.
  if (!row->author.empty()) item.subtitle = row->author.c_str();
  formatDuration(row->totalSeconds, self->valueBuf_, sizeof(self->valueBuf_));
  item.value = self->valueBuf_;
}

void ReadingStatsBookListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = listContentRect();
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(contentRect.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (contentRect.x + contentRect.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (contentRect.y + contentRect.height)),
                  static_cast<int16_t>(contentRect.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (index_.empty()) {
    screen.centeredText(tr(STR_READING_STATS_NO_DATA), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.rowProvider = &ReadingStatsBookListActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(index_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  screen.list(props);
}
```

Thread notes for the reviewer:
- `index_`, `rows_` and `rowsFirst_` are written only on the loop task, always under `RenderLock`. Each write is followed by `requestUpdate()`, from `rebuildIndex()` and `ensureRowsFor()`.
- The render reads them inside `render(RenderLock&&)`, through `listCount()`, `provideRow()`, `buildScreen()` and `footerConfirmLabel()`.
- `labelBuf_` and `valueBuf_` are touched only by the render task.
- The selection moves only through `ListNav::requestSelection()`.
- `onSelectionChanged()` and `showPositionPage()` run only on the loop task, from `ListController`, the row-tap route and the swipe in `UiListActivity::loop()`.
- Loop order in `UiListActivity::loop()`:
  1. `handleCustomInput()`, the per-tick check against the published window;
  2. then `routeListTouch()`, the swipe and `navigateButtons()`, which reach `moveSelectionTo()` or `showRowPage()` and through them `onSelectionChanged()`.
- So a selection change always decodes before the `requestUpdate()` it triggers, and that update is not sent to the render task until `ActivityManager::loop()` ends.

- [ ] **Step 7: Check that nothing of the old list is left**

```bash
grep -n "GUI.drawList\|ListTouchBand\|pageItems\|wasPressed\|buttonNavigator\|selectedIndex\|drawButtonHints\|mapLabels" src/activities/settings/ReadingStatsBookListActivity.h src/activities/settings/ReadingStatsBookListActivity.cpp
```

Expected: no output.

- [ ] **Step 8: Format** (Git Bash)

```bash
"/c/Program Files/LLVM/bin/clang-format.exe" -i src/util/ListRowCache.h test/list_row_cache/ListRowCacheTest.cpp src/activities/settings/ReadingStatsBookListActivity.h src/activities/settings/ReadingStatsBookListActivity.cpp
git diff --stat
```

Expected: the code above is already clang-format clean, so formatting changes nothing.

- [ ] **Step 9: Build `default`** (PowerShell)

```powershell
$env:PLATFORMIO_CORE_DIR = 'C:\pio'; & C:\pio\penv\Scripts\pio.exe run -e default 2>&1 | Select-String -Pattern "^(RAM|Flash):|SUCCESS|FAILED|error"
```

Expected: `SUCCESS`. Record Flash and RAM against the previous task's numbers (PR 4 head: Flash 6228461 B, RAM 58280 B).
- About 732 B of `std::function` thunks and `render`/`loop` go away; `buildScreen`, the provider and a `UiListActivity` vtable come back.
- Expect a small net drop. Record whatever it is.

- [ ] **Step 10: Host suite** (Git Bash)

```bash
cmake --build test/build -j 8 -- -k 0 > "$TEMP/hb.log" 2>&1; grep FAILED "$TEMP/hb.log" | grep -v epub_build_inventory
ctest --test-dir test/build -j 8 2>&1 | grep -E "tests passed|tests failed"
```

Expected:
- the `grep FAILED` line prints nothing;
- `100% tests passed, 0 tests failed out of 1189`, which is 1177 + 12 `ListRowCache` tests (see OPEN item 5).

- [ ] **Step 11: Commit**

```bash
git add src/util/ListRowCache.h test/list_row_cache/CMakeLists.txt test/list_row_cache/ListRowCacheTest.cpp test/CMakeLists.txt src/activities/settings/ReadingStatsBookListActivity.h src/activities/settings/ReadingStatsBookListActivity.cpp
git commit -m "feat(lists): reading-stats book list on the list base

The book list moves onto UiListActivity. Rows come from a row provider over
the decoded rows, with the finished mark and the reading time formatted into
fixed buffers, and the buttons, hints and touch follow the list scheme:
Left/Right step, hold Left/Right to page, hold Up/Down for the ends, long Back
goes Home, a swipe pages.

The row cache follows the selection and the drawn window instead of a
theme-sized page. ListRowCache decides which rows to hold: the window the next
render draws, plus one window either side. It is host-tested in
test/list_row_cache. Rows are still decoded on the loop task and swapped in
under the render lock. A selection change decodes before its repaint, a page
turn passes its own top, and a per-tick check catches a window the render
moved. pageItems() and the ListTouchBand clamp go."
```

- [ ] **Device checklist for this task** (X4 buttons; T5S3 touch, where tapping a hint box is a press and a long tap is a hold). Use a history of at least 20 books, at least one of them marked finished.
  1. **7.1** Open Reading stats → All books. The opening screen shows every row decoded: titles, authors and times, with no "…" row. The header sits inside the side gutter, and the side Up/Down boxes are drawn.
  2. **7.2** Step Down past the bottom row, one press at a time, to the end of the list. The list scrolls by one row, and no "…" row ever appears. Step back Up to the top the same way.
  3. **7.3** Hold Right: one page per second while held. The selection keeps its line, and no page shows a "…" row, including the last (short) page. Hold Left back to the top.
  4. **7.4** Double-tap Down: a page. Hold Down: the last book. Hold Up: the first book. A short Right or Left steps one row.
  5. **7.5** Confirm opens the book's detail. Delete the book there and come back. The list re-reads with one fewer row, and the selection is on the same line, or on the new last row when the deleted book was last.
  6. **7.6** A finished book shows "✓ " before its title. At 480 px width, a long finished title plus a "12h 05m" value both stay readable: the label wraps or truncates, and the value is not clipped.
  7. **7.7** Empty history (fresh device, or with the stats file moved away): "No reading recorded yet" is centred. Confirm does nothing; Back leaves.
  8. **7.8** Long Back goes Home.
  9. **7.9** T5S3:
     - A row tap selects or opens, per the touch-activation setting.
     - A swipe up pages, and the selection moves with it, with no "…" frame.
     - The side boxes step, and a long tap on the side Down box selects the last book.
     - In landscape, reopen the list: the opening screen is fully decoded.

### Task 8: FontDownloadActivity's family list onto UiListActivity

The family list moves onto `UiListActivity` with a row provider over `families_`. The status screens (Wi-Fi, loading, downloading, complete, error) stay drawn as today, through `drawChrome()` and `drawFooter()` instead of a `render()` override, and their input reads events. Only upstream's list skeleton is ported.

**Files:**
- Replace: `src/activities/settings/FontDownloadActivity.h` (full file below)
- Modify: `src/activities/settings/FontDownloadActivity.cpp` (edits E1-E11 below; line numbers are the current file's, 991 lines)

**Interfaces:**
- **Consumes.** Each item was checked against the file it comes from.
  - From `src/activities/UiListActivity.h`:
    - the protected constructor `UiListActivity(const char*, GfxRenderer&, MappedInputManager&, const ListDeclaration& = {})`;
    - the public `void onEnter() override` and `void onExit() override`. `onExit()` runs `closeRouting()` and then `Activity::onExit()`; this task calls it before the restart.
    - the pure virtuals `virtual int listCount() const = 0`, `virtual void buildScreen(UiScreen&) = 0` and `virtual void activateIndex(int) = 0`. The base `activatePosition()` calls `activateIndex()` only for `0 <= position < listCount()`.
    - the overridable hooks `virtual const char* headerTitle() const`, `virtual bool handleCustomInput()` (`loop()` calls it first and returns if it returns true), `virtual void drawChrome()` (the default draws `GUI.drawHeader(renderer, listHeaderRect(), headerTitle())`), `virtual void drawFooter()` (default `drawListHints()`) and `[[nodiscard]] virtual const char* footerConfirmLabel() const`;
    - the helpers `void drawListHints()`, `[[nodiscard]] Rect listContentRect() const` and `void syncListViewport(UiScreen&, freeink::ui::ListProps&, bool hasSubtitle = false)`;
    - `virtual freeink::ui::ListNav& activeNav()`, the protected member `freeink::ui::ListNav nav` and `ACTION_ROW`;
    - `app.clearTapFlash()`, through the protected base `UiAppHost`.
  - `UiListActivity::render()`, which this task does not override, runs:
    1. `clearScreen`;
    2. `drawChrome`;
    3. `renderUi`, with up to 8 rebuild passes, each repeating `drawChrome`. A pass is repeated only while the list asks for a rebuild, so non-list states draw once.
    4. `publishListWindow`;
    5. `afterUiRender`;
    6. `drawFooter`;
    7. `displayBuffer`.
  - `FreeInkApp::render()` does not clear the target (`clearBeforePaint_` is false; nothing in `src/` calls `setClearColor`), so the status text that `drawChrome()` draws survives `renderUi()`.
  - From FreeInkUI `list.h`: `ListProps::rowProvider`, which is `void (*)(void* ctx, uint16_t index, ListItem& item)` (line 81), plus `rowProviderCtx`, `count`, `action`, `inputMask` and `valueInset`. `ListItem::label`, `subtitle` and `value` are `const char*`, and `actionValue` is `int16_t`. `ListNav::requestSelection(int)` and the atomic `ListNav::selected`.
  - From `src/ButtonEventManager.h`: `void update()`, `bool consumeEvent(ButtonEvent& out)`, `void drain()` and `struct ButtonEvent { Button button; PressType type; unsigned long pressMs; }`.
    - An activity reaches it as `buttonEvents`, the protected `ButtonEventManager&` in `Activity.h:24`, bound to `globalButtonEvents()`. That is how `FontSelectionActivity::handleCustomInput()` and `OpdsBookBrowserActivity.cpp:879` reach it.
    - `drain()` also flushes the sampler's raw edges and queued touch events (`ButtonEventManager.cpp:116-134`).
  - From `src/MappedInputManager.h`: `void update() const` (line 115, which is `gpio.update()`) and `mapLabels(...)`. Its order with `buttonEvents.update()` matches `main.cpp:1527-1528` (`gpio.update(); buttonEventManager.update();`).
  - `::UiAppHost` (`src/components/UiAppHost.h`), used only in `sizeof`.
- **Produces:** nothing that another task uses.
  - `FontDownloadActivity`'s public surface: the constructor, `onEnter`, `onExit`, `usesWifi`, `preventAutoSleep` and `skipLoopDelay`.
  - Its own `loop`, `render` and `selectListRow` overrides are gone; the base's run.
  - The class becomes `final`.
- **Deliberate behaviour changes** (user-visible, for the docs task):
  1. In the family list, buttons follow the list scheme:
     - short Left/Right steps (it paged);
     - holding Left/Right pages;
     - a double-tap of Up/Down pages;
     - holding Up/Down jumps to the first or last row;
     - a long Back goes Home, which reboots through `onExit()`, like Back;
     - a swipe pages the selection.
  2. Both hint strips are drawn, including the side Up/Down boxes. The header and list sit inside the side gutter (the header was full width). The empty list shows a blank Confirm label next to the side boxes.
  3. Row values ("Installed", "Update available", "Resume") are drawn in FreeInkUI's value slot. The Lyra value pill is gone.
  4. Cancelling a transfer: Back now cancels when the press completes, that is on release (a Short event), or after a 1 s hold (a Long event). Before, it cancelled on the press edge. Any kind of Back press cancels, even one with a configured global action.
  5. Presses and taps made during a transfer or the manifest load are dropped once it returns (`drain()`). Before:
     - they replayed into the list as stale steps or a stale Confirm;
     - a Back-hint tap on a touch board closed the screen;
     - a Power press slept the device after the transfer.
  6. The COMPLETE and ERROR screens read Back and Confirm as events, so they act on release, not on press.
  7. A cancelled transfer returns to the list only after the stashed families are restored. Before, the state flipped to the list while it was still empty, and a render in that gap showed "No fonts available".
- **HEAP GATE (Step 17):**
  - The activity object grows by `sizeof(UiAppHost)` (a `FreeInkApp<24,6>` and a `ThemeTokens`, held by value) for as long as the screen is open, the TLS handshake included.
  - The gate measures the contiguous heap at the first font file's handshake before and after this change.
  - **If it fails, revert this task's commit and keep FontDownload on its current base for a later PR.**

- [ ] **Step 1: Replace `src/activities/settings/FontDownloadActivity.h`**

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "FontInstaller.h"
#include "activities/UiListActivity.h"
#include "network/HttpDownloader.h"

#ifndef FONT_MANIFEST_URL
#define FONT_MANIFEST_URL "https://raw.githubusercontent.com/jpirnay/witchhunt-reader/master/assets/sd-fonts/fonts.json"
#endif

// Adapted from upstream crosspoint-reader's FontDownloadActivity
// (develop @ cdac66ffe, src/activities/settings/FontDownloadActivity.cpp).
// Only the family list's FreeInkUI skeleton is theirs. The stash-to-SD download, resume, the v1
// manifest, the delete prompt and the reboot on exit are ours; their groups, string arena,
// mandatory CRC and delete-on-abort are not taken.
class FontDownloadActivity final : public UiListActivity {
 public:
  explicit FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  bool usesWifi() const override { return true; }
  void onExit() override;
  bool preventAutoSleep() override { return state_ == LOADING_MANIFEST || state_ == DOWNLOADING; }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    LOADING_MANIFEST,
    FAMILY_LIST,
    DOWNLOADING,
    COMPLETE,
    ERROR,
  };

  struct ManifestFile {
    std::string name;
    size_t size = 0;
    uint32_t crc32 = 0;
    bool hasCrc32 = false;  // false = legacy v1 manifest, fall back to size-only check
  };

  struct ManifestFamily {
    std::string name;
    std::string description;
    // `styles` was once parsed here but never rendered — dropped to avoid
    // ArduinoJson string allocations that fragmented the heap before the
    // first TLS download. Resurrect if a UI surfaces style names.
    std::vector<ManifestFile> files;
    size_t totalSize = 0;
    bool installed = false;
    bool hasUpdate = false;
    // True iff a leftover __staging dir from a previous interrupted download
    // exists for this not-yet-installed family — the next confirm will resume
    // rather than restart.
    bool hasResumableDownload = false;
  };

  State state_ = WIFI_SELECTION;
  FontInstaller fontInstaller_;

  // HTTP/TLS session shared across all files of a single downloadFamily()
  // call. Each family install pays the TLS handshake once (on its first
  // file); subsequent files reuse the open keep-alive connection.
  // NOT shared with the manifest fetch — holding the TLS context open
  // through the JSON parse aborts on the ~36 KB contiguous allocation
  // collision with ArduinoJson's working memory.
  HttpDownloader::Session httpSession_;

  std::string baseUrl_;
  // Read by the render task (listCount(), the row provider, the confirm label): replaced or
  // changed on the loop task only under RenderLock.
  std::vector<ManifestFamily> families_;

  enum class PendingFontAction {
    None,
    Download,
    Delete,
  };

  size_t currentFileIndex_ = 0;
  size_t currentFileTotal_ = 0;
  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  int downloadingFamilyIndex_ = 0;
  // Cached during downloadFamily() before families_ is stashed to SD, so the
  // render path can show the family name and decide the Retry/Resume label
  // without touching families_ (which is empty during the download).
  std::string downloadingFamilyName_;
  bool downloadingFamilyHasResumable_ = false;
  PendingFontAction pendingErrorAction_ = PendingFontAction::None;
  std::string errorMessage_;
  bool cancelRequested_ = false;
  int previousActionCount_ = 0;
  // What the two action rows offer, counted by refreshRowTotals() so that listCount() and the row
  // provider never walk families_, and the rows' labels ("Download all (12.3 MB)") they point at.
  int uninstalledCount_ = 0;
  int updateCount_ = 0;
  char downloadAllLabel_[64] = {};
  char updateAllLabel_[64] = {};
  int lastProgressPercent_ = -1;
  unsigned long lastProgressUpdateMs_ = 0;

  // --- The family list (UiListActivity) ---
  int listCount() const override { return state_ == FAMILY_LIST ? listItemCount() : 0; }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Every state but the family list: COMPLETE and ERROR read their own events, the others none.
  bool handleCustomInput() override;
  // The header in every state, and the status screens' text outside the family list.
  void drawChrome() override;
  // The list's hint strips in the family list; Back and Retry/Resume on the status screens.
  void drawFooter() override;
  [[nodiscard]] const char* footerConfirmLabel() const override;
  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);

  void onWifiSelectionComplete(bool success);
  bool fetchAndParseManifest();
  // Download a single family by its index into families_. Internally stashes
  // families_ to SD so the TLS handshake runs on a defragmented heap; the
  // selected family's state mutations (installed/hasUpdate/hasResumableDownload)
  // are merged back into families_ on return.
  void downloadFamily(int familyIdx);
  // Internal: the body of downloadFamily after the stash. Operates only on
  // the local family copy and constants like familyIdx; never touches
  // families_ (which is empty during this call).
  void downloadFamilyImpl(ManifestFamily& family, int familyIdx);
  void downloadAll();
  void updateAll();
  // The transfer's input pump: true once Back has been pressed, read as an event.
  bool backPressedDuringTransfer();

  // Persist families_ to /fonts_families.bin and clear the in-memory vector.
  // Used to free the ~10 KB of scattered std::string allocations that fragment
  // the heap enough to break the TLS handshake during font downloads.
  bool stashFamiliesToSd();
  // Read /fonts_families.bin into `out`. Returns true on success. The caller swaps it into
  // families_ under RenderLock.
  bool restoreFamiliesFromSd(std::vector<ManifestFamily>& out);
  bool hasDownloadCandidates() const { return uninstalledCount_ > 0; }
  bool hasUpdateCandidates() const { return updateCount_ > 0; }
  int actionCount() const { return (hasDownloadCandidates() ? 1 : 0) + (hasUpdateCandidates() ? 1 : 0); }
  bool isDownloadAllRow(const int row) const { return hasDownloadCandidates() && row == 0; }
  bool isUpdateAllRow(const int row) const { return hasUpdateCandidates() && row == (hasDownloadCandidates() ? 1 : 0); }
  // The family a list row shows; -1 for an action row or a row past the list.
  int familyIndexFromList(const int listIndex) const {
    const int familyIndex = listIndex - actionCount();
    return familyIndex >= 0 && familyIndex < static_cast<int>(families_.size()) ? familyIndex : -1;
  }
  int listItemCount() const { return families_.empty() ? 0 : static_cast<int>(families_.size()) + actionCount(); }
  // Recounts the action rows and formats their labels. Call under RenderLock after families_ or a
  // family's installed / hasUpdate flag changes.
  void refreshRowTotals();
  // Loop task only: moves the selection through the nav when the action rows come or go.
  void syncSelectedIndexForNewActionCount();

  void promptDeleteFamily(int familyIndex);
  void deleteFamilyAtIndex(int familyIndex);

  static void formatSize(size_t bytes, char* out, size_t outSize);
};
```

Equivalence of the cached counts with the deleted O(n) helpers:
- `uninstalledCount_ > 0` equals the old `hasDownloadCandidates()` (any `!installed`).
- `updateCount_ > 0` equals the old `hasUpdateCandidates()` (any `installed && hasUpdate`). That is why the update branch in `refreshRowTotals()` is an `else if` after `!installed`.
- The two byte sums equal the old `totalUninstalledSize()` and `totalUpdateSize()`.

- [ ] **Step 2 (E1): Includes and constructor**

In `FontDownloadActivity.cpp`, replace:

```cpp
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

FontDownloadActivity::FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FontDownload", renderer, mappedInput), fontInstaller_(sdFontSystem.registry()) {}
```

with:

```cpp
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace fui = freeink::ui;

FontDownloadActivity::FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("FontDownload", renderer, mappedInput), fontInstaller_(sdFontSystem.registry()) {}
```

- [ ] **Step 3 (E2): `onEnter()` and `onExit()`**

Replace both functions, which currently run from line 28 (`void FontDownloadActivity::onEnter() {`) to the closing brace of `onExit()` at line 60, with:

```cpp
void FontDownloadActivity::onEnter() {
  UiListActivity::onEnter();
  // The heap gate for this screen (PR 5 Task 8): the activity now carries a UiAppHost, and the
  // TLS handshake below is the allocation that cannot afford it.
  LOG_DBG("FONT", "Activity %u B, of which UiAppHost %u B", static_cast<unsigned>(sizeof(*this)),
          static_cast<unsigned>(sizeof(::UiAppHost)));

  // Free the heap the WiFi stack needs before it is brought up, not after -
  // association itself is the allocation-heavy step, well ahead of TLS. Matters
  // most here because SettingsActivity is still on the stack below us with its
  // per-category SettingInfo vectors resident, fragmenting the heap.
  // WifiSelectionActivity sets WIFI_STA itself, so no radio work happens here.
  trimMemoryForNetworkSession(renderer, "FONT");

  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void FontDownloadActivity::onExit() {
  // Routing closes first (UiListActivity::onExit()); the restart below never returns.
  UiListActivity::onExit();

  // Always silentRestart on exit, regardless of WiFi state. Even if a deep
  // error path turned WiFi off, we still did expensive network/TLS work and
  // the heap is fragmented past the point a normal session can recover (see
  // [project-font-download-heap-stash]). A reboot here gives the next
  // activity a pristine heap.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  silentRestart();
}
```

- [ ] **Step 4 (E3): `onWifiSelectionComplete()`**

This is the Wi-Fi callback path. It is reached from `onEnter()` when Wi-Fi is already up, and otherwise from the `WifiSelectionActivity` result handler. Replace the whole function (currently lines 62-90) with:

```cpp
void FontDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  {
    RenderLock lock(*this);
    state_ = LOADING_MANIFEST;
  }
  requestUpdateAndWait();

  // Re-trim: the status screens rendered since onEnter can have repopulated the
  // font cache. Idempotent - the secondary buffer is already gone by now.
  trimMemoryForNetworkSession(renderer, "FONT");

  const bool loaded = fetchAndParseManifest();
  // The fetch held the loop task. Nothing pressed meanwhile may reach the list.
  buttonEvents.drain();
  if (!loaded) {
    RenderLock lock(*this);
    state_ = ERROR;
    return;
  }

  {
    RenderLock lock(*this);
    refreshRowTotals();
    previousActionCount_ = actionCount();
    state_ = FAMILY_LIST;
  }
  requestUpdate();
}
```

The old `selectedIndex_ = 0;` has no replacement: `UiListActivity::onEnter()` reset the nav to row 0, and no list input runs before this point. `fetchAndParseManifest()` itself is unchanged. It fills `families_` while `state_ == LOADING_MANIFEST`, and in that state `listCount()` returns 0 and the render never reads `families_`.

- [ ] **Step 5 (E4): Clear the stash under the lock**

In `stashFamiliesToSd()`, replace:

```cpp
  // Free the in-memory representation now that it's safely on disk.
  families_.clear();
  families_.shrink_to_fit();
```

with:

```cpp
  // Free the in-memory representation now that it's safely on disk. Under the lock: the render
  // task reads families_ through listCount() and the row provider.
  {
    RenderLock lock(*this);
    families_.clear();
    families_.shrink_to_fit();
  }
```

- [ ] **Step 6 (E5): `restoreFamiliesFromSd()` reads into a local**

Replace the whole function (currently lines 310-363) with the following. The body is the current one with `families_` replaced by `out`.

```cpp
bool FontDownloadActivity::restoreFamiliesFromSd(std::vector<ManifestFamily>& out) {
  FsFile file;
  if (!Storage.openFileForRead("FONT", FAMILIES_STASH_PATH, file)) {
    LOG_ERR("FONT", "Stash file missing");
    return false;
  }

  uint32_t magic = 0;
  uint32_t count = 0;
  bool ok = readU32(file, magic) && magic == FAMILIES_STASH_MAGIC && readU32(file, count);
  if (ok) {
    out.clear();
    out.reserve(count);
    for (uint32_t i = 0; i < count && ok; i++) {
      ManifestFamily fam;
      ok = ok && readStr(file, fam.name);
      ok = ok && readStr(file, fam.description);
      uint32_t totalSize = 0;
      ok = ok && readU32(file, totalSize);
      fam.totalSize = totalSize;
      uint8_t flags = 0;
      ok = ok && readU8(file, flags);
      fam.installed = (flags & 1) != 0;
      fam.hasUpdate = (flags & 2) != 0;
      fam.hasResumableDownload = (flags & 4) != 0;
      uint8_t fileCount = 0;
      ok = ok && readU8(file, fileCount);
      fam.files.reserve(fileCount);
      for (uint8_t j = 0; j < fileCount && ok; j++) {
        ManifestFile fl;
        ok = ok && readStr(file, fl.name);
        uint32_t fsize = 0, fcrc = 0;
        ok = ok && readU32(file, fsize);
        fl.size = fsize;
        ok = ok && readU32(file, fcrc);
        fl.crc32 = fcrc;
        uint8_t hasCrc = 0;
        ok = ok && readU8(file, hasCrc);
        fl.hasCrc32 = hasCrc != 0;
        fam.files.push_back(std::move(fl));
      }
      if (ok) out.push_back(std::move(fam));
    }
  }
  file.close();
  if (!ok) {
    LOG_ERR("FONT", "Stash read failed (magic=%08x count=%u)", magic, count);
    return false;
  }
  // Keep the stash file around so a crash mid-download can still recover.
  // It gets overwritten on next stash and is harmless if stale.
  LOG_DBG("FONT", "Restored %zu families from stash", out.size());
  return true;
}
```

- [ ] **Step 7 (E7): Cached totals and the selection rewrite**

Replace everything from `size_t FontDownloadActivity::totalUninstalledSize() const {` (currently line 399) through the closing brace of `bool FontDownloadActivity::hasUpdateCandidates() const` (line 449). That span covers `totalUninstalledSize`, `totalUpdateSize`, `syncSelectedIndexForNewActionCount`, `hasDownloadCandidates` and `hasUpdateCandidates`. Replace it with:

```cpp
void FontDownloadActivity::refreshRowTotals() {
  size_t uninstalledBytes = 0;
  size_t updateBytes = 0;
  uninstalledCount_ = 0;
  updateCount_ = 0;
  for (const auto& family : families_) {
    if (!family.installed) {
      ++uninstalledCount_;
      uninstalledBytes += family.totalSize;
    } else if (family.hasUpdate) {
      ++updateCount_;
      updateBytes += family.totalSize;
    }
  }
  char size[32];
  formatSize(uninstalledBytes, size, sizeof(size));
  snprintf(downloadAllLabel_, sizeof(downloadAllLabel_), "%s (%s)", tr(STR_DOWNLOAD_ALL), size);
  formatSize(updateBytes, size, sizeof(size));
  snprintf(updateAllLabel_, sizeof(updateAllLabel_), "%s (%s)", tr(STR_UPDATE_ALL), size);
}

// The action rows come and go with what is left to download or update, and every family row
// shifts with them. Keep the selection on the same family, or on the first family when the
// selected action row went. The selection is the nav's: requested from the loop task, never
// written by the render.
void FontDownloadActivity::syncSelectedIndexForNewActionCount() {
  const int currentActionCount = actionCount();
  if (currentActionCount == previousActionCount_) {
    return;
  }

  auto& current = activeNav();
  const int selected = current.selected.load();
  int newIndex = selected;
  if (selected >= previousActionCount_) {
    newIndex = selected - previousActionCount_ + currentActionCount;
  } else if (selected >= currentActionCount) {
    newIndex = currentActionCount;
  }
  newIndex = std::min(newIndex, std::max(0, listItemCount() - 1));

  previousActionCount_ = currentActionCount;
  if (newIndex != selected) current.requestSelection(newIndex);
}
```

The labels fit: the longest translation of `STR_DOWNLOAD_ALL` (Ukrainian) is about 30 bytes, and `" (1023.9 MB)"` adds 12, well inside 64. `syncSelectedIndexForNewActionCount()` now has three callers, all on the loop task:
- `downloadFamily()`;
- `deleteFamilyAtIndex()`;
- through `previousActionCount_`, the manifest load.

The calls from `render()` and `loop()` are gone with those functions.

- [ ] **Step 8 (E8): `downloadFamily()` drains, then swaps the restore in under the lock**

Replace the whole function (currently lines 451-509) with:

```cpp
void FontDownloadActivity::downloadFamily(int familyIdx) {
  if (familyIdx < 0 || familyIdx >= static_cast<int>(families_.size())) {
    LOG_ERR("FONT", "downloadFamily: invalid index %d (size %zu)", familyIdx, families_.size());
    return;
  }

  // Snapshot the target family by value, then stash + free families_ so the
  // ~10 KB of scattered std::string allocations don't fragment the heap
  // during the TLS handshake. Render-path caches (downloadingFamilyName_,
  // downloadingFamilyHasResumable_) cover the family-name and Resume-label
  // accesses that previously read families_ during DOWNLOADING/ERROR.
  ManifestFamily family = families_[familyIdx];

  cancelRequested_ = false;
  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    downloadingFamilyName_ = family.name;
    downloadingFamilyHasResumable_ = family.hasResumableDownload;
    downloadingFamilyIndex_ = familyIdx;
    currentFileIndex_ = 0;
    currentFileTotal_ = family.files.size();
    fileProgress_ = 0;
    fileTotal_ = 0;
  }
  requestUpdateAndWait();

  if (!stashFamiliesToSd()) {
    RenderLock lock(*this);
    state_ = ERROR;
    pendingErrorAction_ = PendingFontAction::Download;
    errorMessage_ = "Failed to stash manifest";
    return;
  }

  // Run the actual download with families_ empty (defragmented heap).
  downloadFamilyImpl(family, familyIdx);
  // The transfer held the loop task. Its progress callback read Back as an event and dropped every
  // other press; clear whatever is still half-made, so nothing pressed during the transfer reaches
  // the list as a step or a second Confirm.
  buttonEvents.drain();

  // Restore families_ regardless of success/error/abort outcome, then merge
  // back the mutations the impl made on the local family copy. Without the
  // restored manifest the activity can't render the family list, so a failed
  // restore is fatal — drop to ERROR rather than continuing with empty state.
  // Read outside the lock, swapped in under it: the render task reads families_.
  std::vector<ManifestFamily> restored;
  const bool restoredOk = restoreFamiliesFromSd(restored);

  RenderLock lock(*this);
  // Update cached render state from the impl's mutations.
  downloadingFamilyHasResumable_ = family.hasResumableDownload;
  if (!restoredOk) {
    state_ = ERROR;
    pendingErrorAction_ = PendingFontAction::Download;
    errorMessage_ = "Failed to restore manifest";
    return;
  }
  families_ = std::move(restored);
  if (familyIdx < static_cast<int>(families_.size())) {
    families_[familyIdx].installed = family.installed;
    families_[familyIdx].hasUpdate = family.hasUpdate;
    families_[familyIdx].hasResumableDownload = family.hasResumableDownload;
  }
  refreshRowTotals();
  syncSelectedIndexForNewActionCount();
  // A cancelled transfer returns to the list now that the list is back.
  if (cancelRequested_) state_ = FAMILY_LIST;
}
```

`RenderLock` is a plain mutex (`ActivityManager.cpp:1119-1127`). Nothing called while the tail lock is held takes it again: `refreshRowTotals()`, `syncSelectedIndexForNewActionCount()` and `ListNav::requestSelection()` (atomic) do not. Callers request their repaint after the function returns: `activateIndex()` and the ERROR retry call `requestUpdateAndWait()`, and `downloadAll()` and `updateAll()` set COMPLETE under their own lock afterwards.

- [ ] **Step 9 (E9): The transfer reads Back as an event**

Inside `downloadFamilyImpl()`, make the three changes below.

(a) Replace:

```cpp
        httpSession_, url, stagedPath, [this](unsigned int downloaded, unsigned int total) {
          mappedInput.update();
          fileProgress_ = downloaded;
```

with:

```cpp
        httpSession_, url, stagedPath, [this](unsigned int downloaded, unsigned int total) {
          fileProgress_ = downloaded;
```

(b) Replace:

```cpp
          return !mappedInput.wasPressed(MappedInputManager::Button::Back);
        });

    if (result == HttpDownloader::ABORTED) {
      LOG_INF("FONT", "Download cancelled: %s", file.name.c_str());
      // Keep staging dir so the next launch can resume.
      Storage.remove(stagedPath);
      family.hasResumableDownload = !family.installed;
      cancelRequested_ = true;
      RenderLock lock(*this);
      state_ = FAMILY_LIST;
      return;
    }
```

with:

```cpp
          return !backPressedDuringTransfer();
        });

    if (result == HttpDownloader::ABORTED) {
      LOG_INF("FONT", "Download cancelled: %s", file.name.c_str());
      // Keep staging dir so the next launch can resume.
      Storage.remove(stagedPath);
      family.hasResumableDownload = !family.installed;
      // downloadFamily() returns to the list once families_ is restored; until then the list has
      // nothing to show.
      cancelRequested_ = true;
      return;
    }
```

(c) Add the pump between the end of `downloadFamilyImpl()` and `promptDeleteFamily()`. Replace:

```cpp
  RenderLock lock(*this);
  state_ = COMPLETE;
}

void FontDownloadActivity::promptDeleteFamily(int familyIndex) {
```

with:

```cpp
  RenderLock lock(*this);
  state_ = COMPLETE;
}

// The transfer holds the loop task, so main.cpp's loop is not turning presses into events while
// it runs. Pump them here in main.cpp's order (sample the keys, then classify the edges) and
// report Back, whatever kind of press it was. Every other press is dropped, and downloadFamily()
// drains what is left once the transfer returns.
bool FontDownloadActivity::backPressedDuringTransfer() {
  mappedInput.update();
  buttonEvents.update();
  bool back = false;
  ButtonEventManager::ButtonEvent event;
  while (buttonEvents.consumeEvent(event)) {
    if (event.button == MappedInputManager::Button::Back) back = true;
  }
  return back;
}

void FontDownloadActivity::promptDeleteFamily(int familyIndex) {
```

- Why the abort no longer sets FAMILY_LIST here: the impl runs with `families_` stashed.
- What the drains cover:
  - the download: Step 8;
  - the manifest fetch: Step 4;
  - the Wi-Fi callback path: Step 4 again, since `onWifiSelectionComplete()` is that callback (`ActivityManager` also drains after every result handler, `ActivityManager.cpp:339`).
- The ERROR retry goes through `downloadFamily()`, so it drains as well.

- [ ] **Step 10 (E10): `deleteFamilyAtIndex()` changes the flags under the lock**

Replace the whole function (currently lines 748-781) with:

```cpp
void FontDownloadActivity::deleteFamilyAtIndex(int familyIndex) {
  if (familyIndex < 0 || familyIndex >= static_cast<int>(families_.size())) return;

  const auto result = fontInstaller_.deleteFamily(families_[familyIndex].name.c_str());
  if (result == FontInstaller::Error::OK) {
    fontInstaller_.refreshRegistry();
    {
      RenderLock lock(*this);
      families_[familyIndex].installed = false;
      families_[familyIndex].hasUpdate = false;
      refreshRowTotals();
      syncSelectedIndexForNewActionCount();
      pendingErrorAction_ = PendingFontAction::None;
      errorMessage_.clear();
      state_ = FAMILY_LIST;
    }
    requestUpdate();
    return;
  }

  std::string message = "Failed to delete font";
  if (result == FontInstaller::Error::INVALID_FAMILY_NAME) {
    message = "Invalid font family";
  }

  RenderLock lock(*this);
  state_ = ERROR;
  downloadingFamilyIndex_ = familyIndex;
  pendingErrorAction_ = PendingFontAction::Delete;
  errorMessage_ = message;
}
```

The old trailing clamp (`if (selectedIndex_ >= listItemCount()) …`) is gone. A delete changes the row count only through the action rows, and `syncSelectedIndexForNewActionCount()` clamps that case. `ListNav::syncToProps()` also clamps a selection past a shrunken list.

- [ ] **Step 11 (E11): The list hooks and the status screens replace `confirmButtonLabel`, `loop`, `formatSize`, `render` and `selectListRow`**

Replace everything from `std::string FontDownloadActivity::confirmButtonLabel() const {` (currently line 783) to the end of the file (line 991), including the `// --- Input handling ---` and `// --- Rendering ---` comments, with:

```cpp
// --- The family list ---

const char* FontDownloadActivity::headerTitle() const { return tr(STR_FONT_MANAGER); }

const char* FontDownloadActivity::footerConfirmLabel() const {
  if (state_ != FAMILY_LIST || families_.empty()) return "";
  const int selected = nav.selected.load();
  if (isDownloadAllRow(selected)) return tr(STR_DOWNLOAD);
  if (isUpdateAllRow(selected)) return tr(STR_UPDATE);
  const int familyIndex = familyIndexFromList(selected);
  if (familyIndex < 0) return "";
  const auto& family = families_[familyIndex];
  if (family.installed && !family.hasUpdate) return tr(STR_DELETE);
  if (family.hasUpdate) return tr(STR_UPDATE);
  if (family.hasResumableDownload) return tr(STR_RESUME);
  return tr(STR_DOWNLOAD);
}

void FontDownloadActivity::activateIndex(const int index) {
  if (state_ != FAMILY_LIST) return;
  // Activation starts a download or opens the delete prompt; a lingering tap flash would gray an
  // unrelated row once the list comes back.
  app.clearTapFlash();
  if (isDownloadAllRow(index)) {
    downloadAll();
  } else if (isUpdateAllRow(index)) {
    updateAll();
  } else {
    const int familyIndex = familyIndexFromList(index);
    if (familyIndex < 0) return;
    const auto& family = families_[familyIndex];
    if (family.installed && !family.hasUpdate) {
      promptDeleteFamily(familyIndex);
      return;
    }
    downloadFamily(familyIndex);
  }
  requestUpdateAndWait();
}

bool FontDownloadActivity::handleCustomInput() {
  // The family list is the base's: its controller, its touch rows, its swipe.
  if (state_ == FAMILY_LIST) return false;

  ButtonEventManager::ButtonEvent event;
  while (buttonEvents.consumeEvent(event)) {
    const bool back = event.button == MappedInputManager::Button::Back;
    const bool confirm = event.button == MappedInputManager::Button::Confirm;
    if ((state_ == COMPLETE && (back || confirm)) || (state_ == ERROR && back)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      requestUpdate();
      return true;
    }
    if (state_ == ERROR && confirm) {
      if (downloadingFamilyIndex_ >= 0 && downloadingFamilyIndex_ < static_cast<int>(families_.size())) {
        if (pendingErrorAction_ == PendingFontAction::Delete) {
          deleteFamilyAtIndex(downloadingFamilyIndex_);
        } else {
          downloadFamily(downloadingFamilyIndex_);
        }
        requestUpdateAndWait();
      } else {
        {
          RenderLock lock(*this);
          state_ = FAMILY_LIST;
        }
        requestUpdate();
      }
      return true;
    }
    // Anything else is dropped: Wi-Fi selection and the manifest load take no presses, and a
    // transfer reads its own Back (backPressedDuringTransfer()).
  }
  return true;
}

void FontDownloadActivity::provideRow(void* ctx, const uint16_t index, fui::ListItem& item) {
  const auto* self = static_cast<const FontDownloadActivity*>(ctx);
  const int row = index;
  item.label = "";
  item.actionValue = static_cast<int16_t>(row);
  if (self->isDownloadAllRow(row)) {
    item.label = self->downloadAllLabel_;
    return;
  }
  if (self->isUpdateAllRow(row)) {
    item.label = self->updateAllLabel_;
    return;
  }
  const int familyIndex = self->familyIndexFromList(row);
  if (familyIndex < 0) return;
  const ManifestFamily& family = self->families_[familyIndex];
  item.label = family.name.c_str();
  if (!family.description.empty()) item.subtitle = family.description.c_str();
  if (family.hasUpdate) {
    item.value = tr(STR_UPDATE_AVAILABLE);
  } else if (family.installed) {
    item.value = tr(STR_INSTALLED);
  } else if (family.hasResumableDownload) {
    item.value = tr(STR_RESUME);
  }
}

void FontDownloadActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = listContentRect();
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(contentRect.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (contentRect.x + contentRect.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (contentRect.y + contentRect.height)),
                  static_cast<int16_t>(contentRect.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Only the family list lays out here: drawChrome() draws the other states, and families_ is
  // stashed to SD while a transfer runs.
  if (state_ != FAMILY_LIST) return;
  if (families_.empty()) {
    screen.centeredText(tr(STR_NO_FONTS_AVAILABLE), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.rowProvider = &FontDownloadActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(listItemCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;  // air between the status and the row edge
  syncListViewport(screen, props, /*hasSubtitle=*/true);
  screen.list(props);
}

// --- The status screens ---

void FontDownloadActivity::formatSize(const size_t bytes, char* out, const size_t outSize) {
  if (bytes >= 1024 * 1024) {
    snprintf(out, outSize, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(out, outSize, "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(out, outSize, "%zu B", bytes);
  }
}

void FontDownloadActivity::drawChrome() {
  UiListActivity::drawChrome();  // the header, through headerTitle(), in every state
  // The family list is buildScreen()'s; Wi-Fi selection shows its own screen on top of this one.
  if (state_ == FAMILY_LIST || state_ == WIFI_SELECTION) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING_MANIFEST) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING_FONT_LIST));
  } else if (state_ == DOWNLOADING) {
    // families_ is stashed to SD during downloadFamily(); read the cached
    // name instead of indexing families_.
    std::string statusText = std::string(tr(STR_DOWNLOADING)) + " " + downloadingFamilyName_ + " (" +
                             std::to_string(currentFileIndex_ + 1) + "/" + std::to_string(currentFileTotal_) + ")";
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, statusText.c_str());

    float progress = 0;
    if (fileTotal_ > 0) {
      progress = static_cast<float>(fileProgress_) / static_cast<float>(fileTotal_);
    }

    int barY = centerY + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, barY, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(progress * 100), 100);

    int percentY = barY + metrics.progressBarHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, percentY,
                              (std::to_string(static_cast<int>(progress * 100)) + "%").c_str());
  } else if (state_ == COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_FONT_INSTALLED), true, EpdFontFamily::BOLD);
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_FONT_INSTALL_FAILED), true,
                              EpdFontFamily::BOLD);
    if (!errorMessage_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, errorMessage_.c_str());
    }
  }
}

void FontDownloadActivity::drawFooter() {
  if (state_ == FAMILY_LIST) {
    drawListHints();
    return;
  }
  // Wi-Fi selection and the manifest load take no presses.
  if (state_ == WIFI_SELECTION || state_ == LOADING_MANIFEST) return;

  const char* confirmLabel = "";
  if (state_ == ERROR) {
    // Use the cached value: families_ may have just been restored (post-impl)
    // or still empty (if the failure was in the stash itself); either way the
    // cache reflects the last update from the download attempt.
    const bool canResume = pendingErrorAction_ == PendingFontAction::Download && downloadingFamilyHasResumable_;
    confirmLabel = canResume ? tr(STR_RESUME) : tr(STR_RETRY);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
```

What stays and what changes in the screens:
- The status screens draw the same text, progress bar and hints as today's `render()`. The only differences: the header is now `listHeaderRect()`, and the hints come from `drawFooter()`.
- `mapLabels`/`drawButtonHints` remain only on the non-list screens. The family list uses `drawListHints()`.
- `downloadAll()`, `updateAll()`, `fetchAndParseManifest()`, `stashFamiliesToSd()` (apart from E4), `promptDeleteFamily()` and the rest of `downloadFamilyImpl()` are unchanged.

- [ ] **Step 12: Check that nothing of the old list and level reads is left**

```bash
grep -n "selectedIndex_\|buttonNavigator_\|wasPressed\|GUI.drawList\|confirmButtonLabel\|totalUninstalledSize\|totalUpdateSize\|isDownloadAllSelected\|isUpdateAllSelected\|selectListRow\|::render(\|::loop()\|mapLabels(tr(STR_BACK), confirmLabel.c_str()" src/activities/settings/FontDownloadActivity.h src/activities/settings/FontDownloadActivity.cpp
```

Expected: no output.

- [ ] **Step 13: Format** (Git Bash)

```bash
"/c/Program Files/LLVM/bin/clang-format.exe" -i src/activities/settings/FontDownloadActivity.h src/activities/settings/FontDownloadActivity.cpp
```

The header and the new functions above are already clang-format clean.

- [ ] **Step 14: Build `default`** (PowerShell)

```powershell
$env:PLATFORMIO_CORE_DIR = 'C:\pio'; & C:\pio\penv\Scripts\pio.exe run -e default 2>&1 | Select-String -Pattern "^(RAM|Flash):|SUCCESS|FAILED|error"
```

Expected: `SUCCESS`. Record Flash and RAM against Task 7's numbers.
- 418 B of `drawList` thunks and the `loop()`/`render()` bodies go.
- `buildScreen`, the provider, `drawChrome`, `drawFooter`, `handleCustomInput` and a `UiListActivity` vtable come back.
- Expect roughly flat. Record the number.

- [ ] **Step 15: Host suite** (Git Bash)

```bash
cmake --build test/build -j 8 -- -k 0 > "$TEMP/hb.log" 2>&1; grep FAILED "$TEMP/hb.log" | grep -v epub_build_inventory
ctest --test-dir test/build -j 8 2>&1 | grep -E "tests passed|tests failed"
```

Expected: the same count as after Task 7, `100% tests passed, 0 tests failed out of 1189`. This task adds no host test: everything it changes is activity code, which the host suite cannot build.

- [ ] **Step 16: Commit**

```bash
git add src/activities/settings/FontDownloadActivity.h src/activities/settings/FontDownloadActivity.cpp
git commit -m "feat(lists): font manager's family list on the list base

The family list moves onto UiListActivity with a row provider over the
manifest families. The Download all / Update all rows are formatted into
fixed buffers and their counts cached, so the provider and the row count
never walk the families. Buttons, hints and touch follow the list scheme, and
the Confirm label still follows the selected row (Download, Update, Delete,
Resume). The action-row selection shift goes through the nav from the loop
task, never from the render.

The status screens (loading, downloading, complete, error) keep their drawing
and read button events. The in-transfer cancel pumps events and stops on
Back instead of reading Back by level, and every blocking call (the transfer,
the manifest fetch) drains the events it left behind. The families are
stashed and restored under the render lock. A cancelled transfer returns to
the list only once they are back.

Stash-to-SD, resume, the v1 manifest, the delete prompt and the reboot on
exit are unchanged; upstream's groups, string arena, mandatory CRC and
delete-on-abort are not taken. The activity logs its size and its UiAppHost's
on entry for the TLS heap check.

Adapted from upstream crosspoint-reader's FontDownloadActivity (develop @ cdac66ffe, src/activities/settings/FontDownloadActivity.cpp)."
```

- [ ] **Step 17: HEAP GATE (device, X3 preferred, X4 acceptable; serial on COM6 at 115200)**

This task's activity grows by `sizeof(UiAppHost)` on a TLS-heap-sensitive path. Measure the contiguous heap at the TLS handshake for the first font file, before and after the change, on the same board, along the same path.

**The log lines to compare** (`default` builds with `LOG_LEVEL=2`, so `LOG_DBG` lines print):
- **Primary.** The first `[DBG] [HTTP] Heap free: <F>, largest block: <M>` after the line `[DBG] [FONT] Stashed families_ to /fonts_families.bin and cleared in-memory copy`.
  - It comes from `logPreCallContext()` (`src/network/HttpDownloader.cpp:66-67`), which runs before every session GET. The first GET after the stash opens the session, so it precedes that family's TLS handshake.
  - `<M>` is `heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)`, the number the stash design exists to protect.
- **Handshake success.** `[DBG] [TLS] negotiated <host>: …` follows (`lib/SecureNet/src/SecureClient.cpp:314`). A failure shows `[DBG] [TLS] wolfSSL_connect(…) failed: err=…` instead.
- **Secondary.** The manifest fetch's handshake trough, `[DBG] [HTTP] Phase done … handshakeMinLargest=<L>` (`HttpDownloader.cpp:160-162`), logged once per screen entry.
- **After build only.** `[DBG] [FONT] Activity <A> B, of which UiAppHost <S> B`.

Procedure:
1. **Baseline (before).** Build the parent's two files and flash them:

   ```powershell
   git checkout HEAD~1 -- src/activities/settings/FontDownloadActivity.h src/activities/settings/FontDownloadActivity.cpp
   $env:PLATFORMIO_CORE_DIR = 'C:\pio'; & C:\pio\penv\Scripts\pio.exe run -e default -t upload --upload-port COM6
   ```

   - Start a serial capture to a file.
   - From Home: Settings → Reader → Font Manager. Join Wi-Fi if asked. Pick one family F that is not installed (the smallest one shown), and Confirm. Wait for "Font installed".
   - Record `<M>`, `<F>`, `<L>`, and that the `negotiated` line is present.
   - Back to the list, Confirm on F and confirm the delete, so that F downloads again in the next run. Back (the device reboots).
2. **Restore this task's files and flash them:**

   ```powershell
   git checkout HEAD -- src/activities/settings/FontDownloadActivity.h src/activities/settings/FontDownloadActivity.cpp
   $env:PLATFORMIO_CORE_DIR = 'C:\pio'; & C:\pio\penv\Scripts\pio.exe run -e default -t upload --upload-port COM6
   ```

   `git status` must show a clean tree again.
3. **After.** Repeat the same path and the same family F, starting from a fresh boot. Record `<M>`, `<F>`, `<L>`, `<A>`, `<S>` and the `negotiated` line.
4. **Acceptance.** All of the following must hold:
   - the after-run's `negotiated` line is present after the stash, and the screen reaches "Font installed";
   - `M_before − M_after ≤ S + 1024`, the UiAppHost size plus 1 KB for allocator rounding and run-to-run noise;
   - `L_before − L_after ≤ S + 1024` likewise.

   If a difference is within 1 KB of its limit, repeat both runs once and compare the means.
5. **Fallback if any acceptance check fails:**
   - `git revert --no-edit <this task's commit>`;
   - FontDownload stays on its current base (`Activity` + `drawList`) and moves in a later PR;
   - note in the PR description that it is deferred, with the measured numbers.
6. Record in the report: board, `M`, `F` and `L` before and after, `A`, `S`, pass or fail.

- [ ] **Device checklist for this task** (X4 buttons; T5S3 touch, where tapping a hint box is a press and a long tap is a hold).
  1. **8.1** Settings → Reader → Font Manager. After Wi-Fi, the list shows:
     - "Download all (N MB)" while any family is not installed;
     - "Update all (N MB)" while any installed family has an update;
     - then the families, each with its description as a subtitle and "Installed", "Update available", "Resume" or nothing as its value.
     The header sits inside the side gutter, and the side boxes are drawn.
  2. **8.2** Step through the rows. The Confirm hint follows the row: Download (Download all), Update (Update all), Delete (an installed family), Update (one with an update), Resume (a half-downloaded one), Download (the rest). A short Right or Left steps; holding Right pages; holding Down goes to the last family.
  3. **8.3** Download one family: the progress screen shows "Downloading <name> (i/n)", a bar and a percentage, with Back as the only hint. "Font installed" follows. Back or Confirm returns to the list: the family reads "Installed", and the "Download all" size shrank.
  4. **8.4** Start a download and press Back briefly. The transfer stops once the button is released, and the list returns with "Resume" on that family and the selection unchanged. Repeat, holding Back: it stops after about 1 s.
  5. **8.5** Start a download and press Down three times and Confirm once while it runs. Once it completes and you go back to the list, the selection has not moved and nothing else started.
  6. **8.6** Download all with the selection on that row. When everything is installed, the "Download all" row disappears and the selection sits on the first family, not past the list.
  7. **8.7** Confirm on an installed family → the delete prompt → confirm. The family reads empty, the "Download all" row appears, and the selection stays on the same family (it moved down one row with it).
  8. **8.8** Error path: switch the access point off during a download. The error screen offers Resume (or Retry); Confirm retries, and Back returns to the list.
  9. **8.9** T5S3 / X4 Pro:
     - a row tap selects or opens, per the setting;
     - a swipe pages;
     - on the complete and error screens, the Back and Confirm hint boxes work.
     - During a transfer: on the X4 Pro the home key should cancel (record the result). On the T5S3 the Back hint box is expected not to cancel (OPEN item 2), and the tap must not close the screen after the transfer.
  10. **8.10** Back from the list reboots, as before. A long Back from the list goes Home, also through the reboot.
  11. **8.11** Step 17's heap gate passed, with its numbers recorded.

### Task 9: Docs, measurements and the full suite

**Files:**
- Modify: `USER_GUIDE.md` ("Moving through lists"; §3.7 *Customise Status Bar*; §3.7 *Weather Settings*)
- Modify: `RELEASE_NOTES.md` (`## Unreleased` → `### Lists`)

**Interfaces:**
- Consumes: the behaviour built by Tasks 1–8, as each task's report records it. If a task was reverted (Task 8's heap gate), leave its screen out of the text below.
- Produces: nothing code-facing.

- [ ] **Step 1: User guide, "Moving through lists"**

Replace the qualifier paragraph:

```markdown
The firmware's lists are moving to one set of buttons, a screen at a time. So far these work this
way: the **OPDS catalog**, **Settings** and the **reader menu**, the option lists and pickers they
open (fonts, dictionaries, language, keyboard layouts, long option lists such as the time zone),
and the menus (file options, Home's More, clock, KOReader sync, weather settings, quick overrides).
Other lists still behave as their own sections describe.
```

with:

```markdown
The firmware's lists are moving to one set of buttons, a screen at a time. So far these work this
way: the **OPDS catalog** and the **OPDS servers** with their settings, **Settings** and the
**reader menu**, the option lists and pickers they open (fonts, dictionaries, language, keyboard
layouts, long option lists such as the time zone), the menus (file options, Home's More, clock,
KOReader sync, weather settings, quick overrides), **File Transfer**'s choice of mode, **Customise
Status Bar**, the **font manager**, **Reading Stats**' book list, the weather city search results
and the screen shown when you finish a book. Other lists still behave as their own sections
describe.
```

If Task 8 was reverted, delete ", the **font manager**" from the new text.

- [ ] **Step 2: User guide, §3.7 Customise Status Bar**

In the **Customise Status Bar** bullet (it begins `- **Customise Status Bar**: Opens a submenu to configure every element of the reading status bar individually:`), append after its last sentence:

```markdown
 On/off items are switches; the others step to their next value with **Confirm**. The preview under the list shows the result.
```

- [ ] **Step 3: User guide, §3.7 Weather Settings**

Replace `- **Weather Settings**: Configure the Open-Meteo weather panel shown on the Home Screen.` with:

```markdown
- **Weather Settings**: Configure the Open-Meteo weather panel shown on the Home Screen. **Location** searches for a city: pick one of the matches with **Confirm**, or press **Back** to keep the current location. A search with no matches says so instead of returning silently.
```

- [ ] **Step 4: Release notes**

Under `## Unreleased` → `### Lists`, add after the existing bullets:

```markdown
- **More screens move the same way:** File Transfer's mode choice, the OPDS servers and their settings, Customise Status Bar, the font manager, Reading Stats' book list, the weather city search results and the finished-book screen. They show the side Up/Down hints, page with a hold of Left/Right or a swipe, and keep the selection's line when they do.
- **On/off settings on Customise Status Bar and the finished-book screen are switches** instead of "Show"/"Hide" and "On"/"Off" words.
- **A weather city search with no matches now says so** instead of returning to the settings without a word.
```

If Task 8 was reverted, delete "the font manager, " from the first bullet.

- [ ] **Step 5: Full host suite, flash, commit**

```bash
cmake --build test/build -j 8 -- -k 0 > "$TEMP/hb.log" 2>&1; grep FAILED "$TEMP/hb.log" | grep -v epub_build_inventory
ctest --test-dir test/build -j 8 2>&1 | grep -E "tests passed|tests failed"
```

Expected: `100% tests passed out of 1189` (1177 + Task 7's 12 `ListRowCache` tests; adjust if a task's report says otherwise).

Build `default` and record Flash/RAM against the PR 4 head (6228461 B / 58280 B). Then:

```bash
git add USER_GUIDE.md RELEASE_NOTES.md
git commit -m "docs: the settings and network list screens follow the list button scheme

Moving through lists names the screens that now follow it; Customise Status Bar
documents its switches and the weather settings the city results; release notes
for the moved screens, the switches and the no-matches message."
```

- [ ] **Step 6: PR description inputs** (for the controller, not committed)

Report the `default` Flash/RAM delta for the whole PR. If flash is not negative, list what the PR buys:
- every list here gets the side Up/Down boxes, two-tap paging, swipe paging and paging that keeps the selection's line;
- no level-read input left on these screens;
- rows built on demand instead of copied per render.
