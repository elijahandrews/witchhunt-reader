# One button scheme for every list — design

> **Status.** Agreed in conversation on 2026-10-03, section by section; this document is the
> written form for review before an implementation plan. Started from issue **#374** (OPDS
> front buttons labelled "Up"/"Down" that did nothing). The interim fix on
> `fix/374-opds-front-buttons` (commits `e8d974a7e`, `6e45c08fd`, unpushed) is superseded by
> this design and is not to be merged; #374 is closed by the OPDS pilot (PR 3 below).
>
> **Amended 2026-10-04** (after PRs 1–3 opened as #383 / #384 / #385): row rendering is now in
> scope (§2b). Lists move onto FreeInkUI's `list()` wherever that brings a functional or memory
> benefit, porting upstream crosspoint-reader's FUI screens where upstream has them, with no SDK
> change. A page is now a screenful (R3), and a swipe moves the selection with the page.

## Problem

There are about 36 list screens and five ways of binding buttons to them. The same physical
button means different things depending on which base class a screen happens to use:

| Pattern | Screens | Up / Down | Left / Right | Hold |
|---|---|---|---|---|
| `ButtonNavigator::onNextList` default | `MenuListActivity` family, chapter/TOC lists, OPDS format picker, 8 older settings/network screens | step | page | Up/Down 1.5 s → ends; Left/Right repeat page |
| Step-only navigator + own actions | Bookmarks, Starred pages, Wi-Fi, OPDS catalog, file browser | step | Rename/Delete, Forget/Rescan, Search/Info, Options | per screen |
| `UiListActivity` base | Enum, Font, Dictionary, Keyboard-layout, Language pickers | step (on release) | step | any arrow 0.5 s → repeat page |
| `TabbedUiListActivity` | Settings, reader menu | step | step | any arrow → switch tab |
| Hand-rolled | Footnotes, KOReader sync result, Home list, … | varies | varies | varies |

What that costs the reader:

- **Labels lie.** About a dozen screens label front Left/Right "Up"/"Down" while those buttons
  page once the list is longer than a screen; OPDS labelled them "Up"/"Down" while they did nothing (#374); the file context menu
  pages under blank labels, which on touch boards makes the boxes untappable.
- **"Page" is not a page.** Eight screens pass no page size and page by a hard-coded 10 rows,
  snapped to multiples of 10, while the theme draws 15–22 rows; a "page" often stays on the
  same screen.
- **Holds are unreachable by touch.** Every hold feature reads the global held time
  (`MappedInputManager::getHeldTime`), which an injected press never sets, so a long tap on a
  hint box — the only way to hold Left/Right/Back/Confirm on the X4 Pro and the T5S3 — triggers
  none of them.
- **A 300 ms lag on every Left/Right press outside the reader.** The default double-press
  bindings on Left/Right (`PAGE_BACK_10` / `PAGE_FORWARD_10`) are reader-scoped, but
  `ButtonEventManager::hasDoubleAction` still makes every Short wait out the double window, and
  two quick presses become a `Double` that lists ignore — the file browser pages zero times.
- **A swipe fires actions.** `ActivityManager::dispatchListSwipe` injects a raw Left/Right press
  because no screen overrides `Activity::pageList()`. On Bookmarks and Starred pages a swipe up
  **deletes the selected entry without confirmation**; on Wi-Fi it rescans, on OPDS it opens
  Info or Search, on the button-remap wizard it is captured as an assignment.
- **The OPDS catalog cannot page at all**, though a feed holds up to 2000 entries.
- **Unlocked render-state writes.** `MenuListActivity::handleNavigation` calls `nav.follow()`
  and the file browser writes `nav.top` from the loop task; `ListNav` documents both as
  render-owned.

The device matrix the scheme has to serve:

| | X3 / X4 | X4 Pro | LilyGo T5S3 |
|---|---|---|---|
| Up / Down | physical | physical | Down physical; **no Up** |
| Left / Right | physical | hint boxes only | hint boxes only |
| Back / Confirm | physical | Home key: hold / tap; hint boxes | Home key: hold / tap; hint boxes |
| Hold of Left/Right/Back/Confirm | physical | hint-box long tap only | hint-box long tap only |
| Touch | — | yes | yes |

## Goals

1. One button scheme for every single-column selectable list, identical across screens.
2. Hint labels generated from the same declaration as the behaviour, so they cannot drift.
3. Every action reachable on every board: by physical key on X3/X4, by hint-box tap or long
   tap on the X4 Pro and the T5S3.
4. One input implementation for lists (`ListGrammar` + `ListController`) in place of five.
5. The user guide describes the scheme once and every screen that changed.
6. One row renderer: every list drawn by FreeInkUI's `list()` where that brings a functional or
   memory benefit, with the legacy `drawList` implementations deleted.
7. Less divergence from upstream crosspoint-reader: a screen upstream already draws with FUI is
   ported from upstream rather than re-invented.
8. The series ends with less flash than `master` had before PR 1.

## Non-goals

- **Upstream's input model.** Upstream's list bases step on the press edge, page on any held
  arrow with wrap-around, open a row on the first tap and draw no hint boxes on touch boards.
  That conflicts with §1 by design (the T5S3 is not in upstream's model), so their input code
  is not adopted; only their rendering is (§2b).
- **Two-dimensional screens** keep geometric arrows: the file browser's cover grid, the Home
  carousel layout, the keyboard, the dictionary word selector, the Wi-Fi yes/no prompts, value
  pickers (slider, printed-page input). The full list with reasons is in §3.
- **The SDK.** No FreeInkUI change. Our pin's `libs/ui` is byte-identical to upstream's
  (checked 2026-10-04: ours `c6e1d2b`, upstream `aef1a6c`), which is what lets upstream's FUI
  screens port without an SDK bump; adding SDK features would end that.
- **Reader controls** (page turns, chapter skip) and the global button-action settings keep
  their current meaning.
- **Delete without confirmation** on Bookmarks and Starred pages is a separate issue. This
  design removes the swipe that triggers it by accident; it does not add a confirmation.

## Design

### 1. The scheme (normative)

| Button | Short press | Long press (1 s) |
|---|---|---|
| **Up / Down** | step one row; two quick taps jump a page | first / last selectable row; **on tabbed lists**, previous / next tab |
| **Left / Right** | step one row (**default pair**), or the screen's **declared pair** | page back / forward; repeats every 500 ms while still held |
| **Confirm** | open / select | the screen's second action if declared, otherwise the same as short |
| **Back** | the screen's back | Home |

Home never discards a change: a screen whose Back commits something goes Home through its own way
out (Settings saves; the reader menu runs its Go Home action), and while a book is open below the
list, or on the file options menu, a long Back is Back.

| Touch | Effect |
|---|---|
| Tap a row | point-then-confirm via `ListRowTap`, unchanged |
| Swipe up / down over the list | page forward / back — the same page as a long Left/Right, selection included, called directly, never as an injected button |
| Tap / long tap a hint box | the same as a short / long press of that button (unchanged mechanism) |

Rules:

- **R1 — Step** moves to the next/previous selectable row and wraps at the ends, as today.
- **R2 — The declared pair.** A screen may replace the short Left/Right pair, as a unit, with
  `none / action`, `action / none` or `action / action`. Declaring either side overloads the
  pair: **neither side steps any more**. The declaration is made once per screen; an action
  that does not apply to the selected row or the current feed (no search link, a folder rather
  than a book, an unsaved network) makes that side **none** for the moment — it never falls
  back to stepping. A button therefore never changes between moving and acting as the
  selection moves.
- **R3 — Page is a screenful.** The render publishes the window it drew: its top row and how
  many rows it drew. A page turn moves the screen by the window the last render drew (forward:
  the new screen starts with the first row below it; back: one window above it), never past the
  ends of the list, and the selection keeps its line on the screen. Where the screen cannot move
  any further, the selection goes to the first / last row. From a selection outside the window
  (the window is a render old) the page is measured from the selection, which lands on the new
  top row. A header the selection lands on is passed over to the next selectable row, else the
  nearest one before it. A header directly above the new top opens the new screen. Rows may
  differ in height (wrapped titles, touch row heights), so a window is whatever was drawn, not a
  fixed count. A screen that lays rows out a page at a time (OPDS) pages between page
  boundaries, and a tab bar above the rows is not a line of the page. Hold-repeat needs the
  live key level, so on the X4 Pro and the T5S3 a long tap pages once.
  (Revised after X4 testing: the selection used to land on the new screen's top row.)
- **R4 — Long Up / Down** jumps to the first / last selectable row. On a tabbed list (Settings,
  reader menu) it switches to the previous / next tab instead, wrapping; tabbed lists use the
  default Left/Right pair.
- **R5 — Events, not levels.** All six buttons act on `ButtonEventManager` events: Short at
  release, Long while held at `LONG_PRESS_MS` (1 s). Each event carries the time its key went
  down on the sampler's clock (`ButtonEvent::pressMs`). This is lossless (events are classified
  from the sampler's edge queue) and treats a physical key and an injected long tap
  identically. The jump to the ends moves from 1.5 s to 1 s.
- **R5a — Double-tap Up/Down pages.** Two taps of the same Up/Down key whose presses land
  within `DOUBLE_WINDOW_MS` (300 ms) jump a page, as `ButtonNavigator` did on `master`. The
  first tap steps at once (single steps never wait); the second undoes that step and pages from
  where the pair started, so the pair moves exactly one page. On a list that fits one page the
  two taps stay two steps. The window is measured between the presses, not between the loop
  ticks that handle them, so a redraw between the taps does not break the pair. A `Double`
  event (only when a double action is bound to the page-turn keys and falls through) is the
  same double-tap; elsewhere a `Double` counts as one press (an action, Confirm, Back) or as
  two steps on the default Left/Right pair.
- **R6 — No needless double-press wait.** `ButtonEventManager::hasDoubleAction(btn)` returns
  false when the key's configured double action is reader-scoped
  (`CrossPointSettings::isReaderScopedAction`) and no reader activity is on top. The lookup
  of "is a reader on top" is injected (a function pointer set from `main.cpp`), so the button
  manager does not depend on `ActivityManager`.
- **R7 — Global bindings win.** An event consumed by a user-bound global action never reaches
  the list (existing behaviour of the dispatch in `main.cpp`). So a user who binds a global
  action to long Left loses hold-to-page on lists; documented, not worked around.
- **R8 — Labels.** Resolved through `MappedInputManager::mapHints`, so they follow the
  orientation:
  - Front Left / Right: the page glyph first, then the short-press label —
    `« Up` / `» Down` (default pair), `« Search` / `» Info` (declared), `«` / `»` alone for a
    side that is none at the moment. Hint boxes truncate at the end ("Downlo…"), so the glyph
    goes first to survive long and translated labels. «/» consistently means "hold to page".
  - Side Up / Down: `Up` / `Down`, **drawn and active on every list**, so a none front box
    never leaves the T5S3 without a way to step. List content rects reserve the side gutter.
  - Confirm and Back: the screen's labels (Open, Select, Download, Home, …).
- **R9 — Known limitation.** Up/Down are the same physical keys as PageBack/PageForward. A
  user-bound global *Short* action on PageBack/PageForward and the list step both act on one
  press. Rare configuration; documented only.

### 2. Architecture

```
ButtonEventManager ──events──▶ ListController ──command──▶ screen callbacks
                                  │   ▲                      (activate, back, actions)
                                  ▼   │
                              ListGrammar (pure)  ◀── declaration + list state
                                  │
                                  └──▶ hint labels ──▶ mapHints ──▶ GUI.drawButtonHints /
                                                                  drawSideButtonHints
touch: dispatchListTap ─▶ selectListRow ─▶ ListController::tapRow
       dispatchListSwipe ─▶ pageList     ─▶ ListController::page
```

**`ListGrammar`** (`src/util/ListGrammar.{h,cpp}`) — pure logic, no Arduino, no hardware.

- `ListCommand commandFor(Button, PressType, const ListDeclaration&, const ListState&)` returns
  one of: `StepPrev`, `StepNext`, `PagePrev`, `PageNext`, `First`, `Last`, `TabPrev`, `TabNext`,
  `Activate`, `ActivateLong`, `Back`, `Home`, `LeftAction`, `RightAction`, `None`.
- `ListLabels labelsFor(const ListDeclaration&, const ListState&)` returns the six labels.
- Index arithmetic: `stepIndex`, `pageIndex` (clamping; relative to the drawn window, R3), `firstSelectable`,
  `lastSelectable`, all taking an optional selectable predicate. Replaces the list half of
  `ButtonNavigator`'s static helpers.
- Both outputs derive from the one declaration; host tests pin them together.

**`ListDeclaration`** — what a screen states once, as a `static constexpr` value:

```cpp
struct ListAction {
  bool declared;                                 // short Left/Right runs a screen action
  StrId label;                                   // short-press label, e.g. STR_SEARCH
};
struct ListDeclaration {
  ListAction left;                               // neither declared = default pair (step)
  ListAction right;
  bool confirmLong;                              // long Confirm has an action of its own
  bool tabbed;                                   // long Up/Down switch tabs
};
```

The screen implements the virtual `ListHost` interface rather than registering function-pointer
slots with a `ctx`: `listCount`, `listWindow`, `listLeadPositions`, `listPagesAligned`,
`listSelectable`, `listActionAvailable`, `listSelected`, `listSelect`, `listShowPage(row, top)`,
`onListActivate(row, longPress)`, `onListBack`, `onListHome`, `onListAction`, `onListTab` and
`onListOtherEvent`. A screen with two lists gives
each its own host. A virtual interface costs one vtable pointer, not a `std::function` (heap
discipline).

**List state** — per tick, the controller asks the host for the row count, the window its last
render drew (top row and row count), the selectable predicate, and whether each declared action is
available on the selected row. `listWindow()` returns what the screen's last render published: it
is called on the loop task, and the renderer's live orientation must not be measured there.

**`ListController`** (`src/activities/ListController.{h,cpp}`) — a member of each list screen,
not a base class.

- `update()`, called from the screen's `loop()`: the **only** consumer of button events on a
  list screen. Walks the pending events, maps each through `ListGrammar`, applies movement, runs
  callbacks, and stops at the first event the screen acts on, leaving later presses for the next
  tick. Every state of a list screen must therefore read events, never levels. Matches Up/Down and
  discards their PageBack/PageForward alias events. Events the grammar does not use go to an
  optional `onOtherEvent` callback (Footnotes uses Power to select).
- Hold-repeat for Left/Right paging after a Long, while `isPressed` stays true.
- `page(int dir)` for swipes, `tapRow(int row)` for row taps (wraps `ListRowTap`).
- `drawHints(renderer, backLabel, confirmLabel)` composes the labels into stack buffers per call
  (no allocation per render) and draws both strips.
- Selection access: the host owns the selection (`listSelected()` / `listSelect()`); a page turn
  asks it to show a page (`listShowPage(row, top)`, which takes the RenderLock to set
  `ListNav::top`). The loop task never writes render-owned `ListNav` fields otherwise.
- Under 100 bytes per instance.

**Base classes.** `UiListActivity`, `MenuListActivity` and `TabbedUiListActivity` each hold a
`ListController`; their own `navigateButtons` / `handleButtons` go. Subclasses that customised
input (the file browser) express it as a declaration instead.

**Touch hooks.** `ListController` backs `Activity::selectListRow` and `Activity::pageList` for
the screen that owns it. Once every list screen overrides `pageList`, the injected-button
fallback in `dispatchListSwipe` is deleted. `UiListActivity`'s own swipe handling stops
scrolling the viewport and pages the selection, like every other list.

**`ButtonNavigator`.** Its list functions (`onNextList` / `onPreviousList`, `onListNav`,
`onListPageNav`, the press-log double-tap logic) are deleted once the last list has moved.
`onPressAndContinuous` and friends stay for the slider, keyboard and frontlight panel.

### 2b. Rendering on `fui::list` (hybrid with upstream)

**Rule.** A list moves its row drawing onto FreeInkUI's `list()` + `ListNav` when that brings a
functional benefit (touch rows, wrapping, one paging model) or a memory benefit (less flash or
heap). Its input always goes through `ListController` (§2); upstream's input code is not taken.

**Source of the render code.** Where upstream crosspoint-reader (`upstream/develop`, pinned per
PR to the commit ported from) already draws a screen with FUI, that screen's row building is
ported from upstream, attributed per the repo's porting convention (source comment + commit
message; `Co-authored-by` only where the result is mainly theirs). Where upstream has no FUI
version, the screen follows the closest upstream pattern.

| Our screen | Render source |
|---|---|
| NetworkModeSelection, OpdsServerList, OpdsSettings, StatusBarSettings, FontDownload, Wi-Fi, ButtonRemap, KOReaderSync | upstream's FUI version of the same screen |
| EPUB / XTC chapter lists | upstream's FUI chapter selectors; MD TOC follows the EPUB one |
| StarredPages | upstream `EpubReaderBookmarksActivity` |
| FinishedBook | upstream `EndOfBookOptions` |
| OPDS catalog + format picker | upstream's `CatalogActivity`-based OPDS screen, evaluated at that PR |
| GlobalBookmarks, ReadingStatsBookList, Footnotes, Weather city list | ours, following the closest upstream pattern |
| UiList / MenuList / Tabbed families, file browser | already FUI; input only |
| Home, list layout | stays drawn by the theme (a themed launcher; no memory benefit); buttons via the controller |
| Home carousel, cover grid, keyboard, dictionary word select | not lists; unchanged |

**No SDK additions; follow upstream's workarounds instead:**
- Chapter levels are indented with leading spaces in the label, as upstream does.
- The Lyra "value pill" (`highlightValue`) is dropped; values draw in FUI's normal value slot.
- A page turn's top and selection (R3) are applied app-side under `RenderLock` (setting
  `ListNav::top` and requesting the selection; the build's follow then scrolls the least it must
  to keep the selection visible), as both file browsers already do; the input task never writes
  render-owned fields without the lock.

**Rows on demand.** A migrated screen supplies rows through `ListProps::rowProvider` rather than
a materialized window of `ListItem`s and `std::string`s, so an open list does not hold ~2–3 KB
of row copies on the heap. A small fixed list (≤ a dozen rows, e.g. a settings submenu) may
keep a materialized array where that is simpler and no larger.

**Borrowed from upstream:** per-tab selection memory for tabbed lists (one `ListNav` per tab,
as upstream's `UiTabListActivity`), the `onRowLongPress` hook, `rowProvider` usage as in their
file browser.

**Deleted once the last caller moves:** `BaseTheme::drawList` and `LyraTheme::drawList` with
their `std::function` row callbacks. The `ListTouchBand` path stays while the Home carousel
records a band.

**Viewport.** `ListNav`'s own follow keeps the selection visible while stepping; paging (R3)
sets the top explicitly. The render publishes the drawn window for the input side (a
render-published atomic, never a loop-task measurement — see `listWindow()` in §2).

### 3. Screen mapping

| Screen | Left / Right short | Long Confirm | Notes |
|---|---|---|---|
| `MenuListActivity` family: file context menu, Home "More", settings submenus, clock, quick overrides, KOReader settings, weather menu and city results | default | — | |
| Pickers: Enum, Font, Dictionary, Keyboard layouts, Language | default | — | |
| Settings, reader menu | default | — | `tabbed` |
| OPDS server list, OPDS settings, network mode, status bar, reading-stats book list, font download, finished book | default | — | |
| Chapter / TOC lists (EPUB, Markdown, XTC), footnotes, KOReader sync result | default | — | footnotes: Power via `onOtherEvent` |
| OPDS format picker | default | — | |
| **OPDS catalog** | Search (feed has a search link) / Info (book selected) | — | closes #374; gains paging |
| **Bookmarks, Starred pages** | Rename / Delete | — | |
| **Wi-Fi networks** | Options (saved network; opens the forget prompt) / Rescan | — | |
| **File browser**, list views | none / Options | KOReader pull, then open (Books mode) | Back: parent folder; cover grid out of scope |
| **File browser**, folder picker | New folder / Move here | — | |
| Home, list layout | default | — | buttons only; rows stay drawn by the theme (§2b) |

**Every screen that keeps a selection, and what happens to it** (complete as of 2026-10-04,
from a search of `src/activities` for list renderers, navigators and selection indexes):

| Screen | Buttons → `ListController` | Rows → `fui::list` |
|---|---|---|
| OPDS catalog + format picker | PR 3 (done) | PR 7 |
| Settings, reader menu (tabbed) | PR 4 | already FUI |
| Enum, Font, Dictionary, Keyboard-layout, Language pickers | PR 4 | already FUI |
| File context menu, Home "More", settings submenus, Clock, Quick overrides, KOReader settings, Weather menu | PR 4 | already FUI |
| NetworkMode, OPDS server list, OPDS settings, Status bar, Font download, Finished book, Reading-stats book list, Weather city list | PR 5 | PR 5 |
| Starred pages, Global bookmarks, Wi-Fi network list | PR 6 | PR 6 |
| File browser list views (Books, All files, Recents, firmware picker, folder picker) | PR 6 | already FUI |
| Home, list layout | PR 6 | **no** — a themed launcher; moving it brings no memory benefit |
| EPUB / XTC chapter lists, Markdown TOC, Footnotes, KOReader sync result | PR 7 | PR 7 |
| Button-remap wizard | **no** — it exists to capture whichever physical button is pressed next; only its `pageList` is a no-op (PR 1) | PR 7, so `drawList` can be deleted |

Not lists, so outside this design:

| Screen | Why |
|---|---|
| Home carousel layout, file-browser cover grid | 2-D grids: Left/Right move within a row, Up/Down between rows |
| Keyboard, dictionary word selection | 2-D cursors (key grid, words on a page) |
| Wi-Fi save / forget prompts, confirmation dialog | horizontal 2–3-button dialogs |
| Frontlight panel, slider picker, printed-page input | value pickers: Left/Right change a value |
| Dictionary definition, system information, button / gesture overviews, book info, reading-stats dashboard | read-only pagers; nothing is selected |

### 4. User-visible changes

Each of these goes into `USER_GUIDE.md` and `RELEASE_NOTES.md` in the PR that makes it:

- Menus and settings lists: front Left/Right **step** on a short press (they paged); hold to
  page.
- Pickers: holding an arrow no longer repeats paging on all four; hold Left/Right to page,
  hold Up/Down to jump to the ends.
- Settings and the reader menu: tabs switch with a **long Up/Down** (any held arrow did).
- File browser: **short Right opens Options** (it was a hold, or a short press only on a
  one-screen folder); hold Right pages; short Left does nothing.
- Bookmarks, Starred pages, Wi-Fi, OPDS: Left/Right actions unchanged; hold now pages; a
  swipe pages instead of triggering the action.
- OPDS catalog: can be paged (hold Left/Right or swipe); front buttons show `«` / `»` when
  Search or Info does not apply.
- Jump to the first / last row: 1 s instead of 1.5 s, and reachable by a long tap on the side
  hint boxes.
- Every list shows the side Up/Down hint boxes (a slightly narrower list on X3/X4).
- Lists no longer lag 300 ms behind Left/Right presses.

### 5. User documentation (deliverable)

User documentation is part of the implementation, not a follow-up: **each PR that changes
behaviour updates `USER_GUIDE.md` in the same PR**, and the series is not done until all of
the following hold.

- A new subsection **"Moving through lists"** under §1 Hardware Overview, after Button Layout:
  the scheme table from §1 of this design in reader terms, how hints show it (`«`/`»` = hold to
  page, `short` label after the glyph), and the per-device row (on the X4 Pro and T5S3 tap or
  long-tap the hint boxes; the side boxes are the T5S3's Up).
- Per-screen sections then describe only their declared pair and anything special, linking to
  that subsection instead of restating the arrows:
  - §1 Hardware Overview: the T5S3 note ("paging backward … by tapping the scroll bar or
    swiping") — replace with the side hint box and hold/swipe paging.
  - §3.3 Browse Files: *Navigate List* and *Options menu* bullets; *Move to folder* (New /
    Move here on short Left/Right).
  - §3.4 Recent Books: the keys sentence.
  - §3.7 Settings, and §4 *System Navigation* (reader menu): tabs on long Up/Down.
  - §3.7.5 OPDS: browsing keys (Search / Info, paging) — the section covers servers only
    today, so this adds a short "Browsing a catalog" paragraph.
  - §5.2 Touch: swipe pages; remove the stale **Tap the scroll bar** and **Swipe right from the
    left edge → Back** rows (neither exists in the firmware); fix the T5S3 note under the table.
  - §5.7 Per-device differences: hint boxes as the Left/Right/Up keys.
  - §6 Chapter Selection: Up/Down step, hold Left/Right to page.
- `RELEASE_NOTES.md`: one entry per PR describing the change in reader terms.
- Developer docs: a new `docs/list-input.md` (the scheme, how to declare a list, the
  `ListController` contract); `docs/touch-gestures.md` stale rows removed; the class comments of
  `TabbedUiListActivity` and the OPDS format picker corrected.
- No new translatable strings are expected: labels are composed from existing `StrId`s plus
  the `«` / `»` glyphs. If one is needed it is added to the English YAML and regenerated per
  `docs/i18n.md` (never a hand-run `gen_i18n.py` outside the build).

### 6. Rollout

One concern per PR; each builds `default`, `x4pro` and `lilygo_t5s3` and passes the host suite.

1. **Swipe fix.** `pageList()` overrides that consume the swipe without effect on Bookmarks,
   Starred pages, Wi-Fi, OPDS and the button-remap wizard. Paging by swipe on the first four
   arrives when each moves to `ListController` (OPDS in PR 3, the rest in PR 6); none of them
   pages by button today either. Standalone; ships first. User guide: §5.2 swipe row.
2. **Double-press wait (R6).** Standalone.
3. **`ListGrammar` + `ListController` + hints, piloted on OPDS** (catalog and format picker).
   Closes #374. User guide: new "Moving through lists" subsection, §3.7.5.
4. **The FUI bases onto the controller.** `UiListActivity`, `MenuListActivity`,
   `TabbedUiListActivity` (with per-tab selection memory) drive input through `ListController`
   with a `ListNav` selection adapter; R3 page-as-screenful and swipe-as-page land here, and
   `ListGrammar::page` takes the drawn window. About 15 screens follow through their bases; the
   file browser keeps its own input until PR 6. User guide: §3.7, §4.
5. **The settings and network `drawList` screens onto FUI + the controller:** NetworkMode,
   OpdsServerList, OpdsSettings, StatusBarSettings, FontDownload, ReadingStatsBookList,
   FinishedBook, the weather city list — ported from upstream where it has them. User guide: §3.7.
6. **Screens with declared pairs, and Home:** GlobalBookmarks, StarredPages, Wi-Fi onto FUI +
   the controller; file browser list views and folder picker onto the controller; Home's list
   layout onto the controller (rows stay theme-drawn). User guide: §3.1, §3.3, §3.4.
7. **The custom painters:** chapter/TOC lists, footnotes, KOReader sync, the OPDS rendering
   and the button-remap list onto FUI. User guide: §6.
8. **Cleanup:** delete `drawList` (Base + Lyra), `ButtonNavigator`'s list functions and the
   `dispatchListSwipe` injection fallback; `docs/list-input.md`, `docs/touch-gestures.md`,
   §5.2/§5.7 final pass.

### 7. Testing

- **Host (new `test/list_grammar/`):** every button × press type × declaration shape (default,
  none/action, action/none, action/action, tabbed, long-Confirm) × list shape (empty, one page,
  several pages, selectable predicate with headers at the ends) → expected command **and**
  expected labels. Index arithmetic: wrap on step, clamp on page, paging by the drawn window, first/last
  selectable.
- **Host:** R6 — the double-wait decision, extracted into a pure function if
  `ButtonEventManager` cannot run in the host suite.
- **Device, per PR:** a checklist for the screens it touches:
  - X3 / X4: short and long on all six buttons, hold-repeat paging.
  - T5S3 / X4 Pro: hint-box tap and long tap, side boxes, swipe, row tap.
  - Lists opened from the reader (chapter list, reader menu, quick overrides) in portrait and
    both landscapes.

### 8. Costs

- **Flash.** The C3 partition is about 95 % full. PR 3 may add at most ~2 KB (it added
  2,282 B with the double-tap restore). Every later PR records its measured delta and must be
  flash-negative, or name the function or RAM benefit it buys instead. The series must end
  below `master`'s size before PR 1. Measured targets (2026-10-04 symbol sizes): ~15.5 KB of
  per-screen input methods, ~2.4 KB of navigator callback lambdas, ~1.7 KB of `ButtonNavigator`
  list code, 2.5 KB of `drawList`, ~3 KB of `drawList` row-callback thunks.
- **Heap.** No `std::function`; labels composed in stack buffers per draw; under 100 bytes
  per controller; nothing allocated per tick or per render; rows built on demand
  (`rowProvider`) rather than held in per-screen windows.

## Open questions

- **Delete confirmation** on Bookmarks / Starred pages: separate issue to file once PR 1 has
  removed the accidental swipe trigger.
- **Label width.** `« ` plus a long translated label may still truncate in a hint box; check
  the longest translations of Search, Rename, Delete, Options, Forget, Rescan during PR 3/6 and
  shorten in the YAML if needed.
