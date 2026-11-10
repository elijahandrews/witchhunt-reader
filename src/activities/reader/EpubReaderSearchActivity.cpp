#include "EpubReaderSearchActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>

#include "FilenameFontSystem.h"
#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "util/InputReleaseGuard.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;
}

EpubReaderSearchActivity::EpubReaderSearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   const std::shared_ptr<Epub>& epub, std::string query)
    : Activity("EpubReaderSearch", renderer, mappedInput),
      epub(epub),
      query(std::move(query)),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

int EpubReaderSearchActivity::getTotalItems() const {
  return session && session->done() ? static_cast<int>(session->count()) : 0;
}

void EpubReaderSearchActivity::onEnter() {
  Activity::onEnter();
  {
    RenderLock lock(*this);
    filenameFontSystem.ensureLoaded(renderer);
  }
  ignoreInitialUpRelease = mappedInput.isPhysicalPressed(MappedInputManager::Button::Up);
  ignoreInitialDownRelease = mappedInput.isPhysicalPressed(MappedInputManager::Button::Down);
  mappedInput.setReaderTouchscreenOverride(true);

  // epub is a required collaborator: the caller dereferences it before constructing
  // this activity, and loop()/render() use it unguarded on every frame.
  session = makeUniqueNoThrow<BookSearchSession>(epub, query);
  if (session) session->begin();
  updateStatus();
  selectorIndex = 0;
  topIndex = 0;
  visibleRows = 1;
  initialViewportPending = true;
  uiReady = false;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &EpubReaderSearchActivity::onRowEvent, this);
  app.setScreen(&EpubReaderSearchActivity::searchScreen, this);
  requestUpdate();
}

void EpubReaderSearchActivity::onExit() {
  session.reset();
  mappedInput.setReaderTouchscreenOverride(false);
  Activity::onExit();
}

void EpubReaderSearchActivity::updateStatus() {
  if (!session || session->failed()) {
    status = tr(STR_BOOK_SEARCH_FAILED);
    return;
  }
  if (!session->done()) {
    status = std::string(tr(STR_BOOK_SEARCH_SCANNING)) + " " + std::to_string(session->sectionsScanned()) + "/" +
             std::to_string(session->sectionCount());
  } else if (session->limited()) {
    status = std::to_string(session->count()) + " " + tr(STR_BOOK_SEARCH_LIMIT);
  } else if (session->failedSections()) {
    status = std::to_string(session->count()) + " " + tr(STR_BOOK_SEARCH_PARTIAL);
  } else {
    status = std::to_string(session->count()) + " " + tr(STR_BOOK_SEARCH_RESULTS);
  }
}

void EpubReaderSearchActivity::selectResult() {
  bool found = false;
  {
    RenderLock lock(*this);
    if (session && selectorIndex >= 0) found = session->readHit(selectorIndex, hit);
    if (found)
      setResult(BookSearchResult{hit.spine, hit.sourceOffset});
    else
      updateStatus();
  }
  if (found)
    finish();
  else
    requestUpdate();
}

void EpubReaderSearchActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<EpubReaderSearchActivity*>(user);
  if (event.value < 0 || event.value >= self->getTotalItems()) return;
  self->selectorIndex = event.value;
  self->app.clearTapFlash();
  self->pendingSelection = true;
}

void EpubReaderSearchActivity::loop() {
  // A held side shortcut opens this list before its release. Keep that hold
  // from scrolling the new list or selecting a different chapter.
  if (InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Up, ignoreInitialUpRelease) ||
      InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Down,
                                               ignoreInitialDownRelease)) {
    return;
  }

  if (session && !session->done()) {
    bool repaint = false;
    {
      RenderLock lock(*this);
      session->step();
      repaint = session->done() || static_cast<uint32_t>(millis() - lastPaintAt) >= 1000;
      if (repaint) {
        updateStatus();
        lastPaintAt = millis();
      }
    }
    if (repaint) requestUpdate();
  }
  const int totalItems = getTotalItems();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput, safe);
  if (TouchHeaderBackButton::wasTapped(mappedInput, header) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      bool handled = false;
      bool repaint = false;
      {
        RenderLock lock(*this);
        handled = static_cast<bool>(app.route(snap));
        repaint = app.invalidated();
      }
      if (pendingSelection) {
        pendingSelection = false;
        selectResult();
        return;
      }
      if (repaint) requestUpdate();
      if (handled) return;
    }
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    RenderLock lock(*this);
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
    const int next = scrollListBy(topIndex, delta, visibleRows, totalItems);
    if (next != topIndex) {
      topIndex = next;
      if (totalItems > 0)
        selectorIndex = std::clamp(selectorIndex, topIndex, std::min(totalItems, topIndex + visibleRows) - 1);
      requestUpdate();
    }
    return;
  }

  if (totalItems == 0) return;
  const auto moveSelection = [this, totalItems](const int index) {
    RenderLock lock(*this);
    selectorIndex = index;
    topIndex = followListSelection(selectorIndex, topIndex, visibleRows, totalItems);
    requestUpdate();
  };
  buttonNavigator.onNextRelease(
      [this, totalItems, &moveSelection] { moveSelection(ButtonNavigator::nextIndex(selectorIndex, totalItems)); });
  buttonNavigator.onPreviousRelease(
      [this, totalItems, &moveSelection] { moveSelection(ButtonNavigator::previousIndex(selectorIndex, totalItems)); });
  buttonNavigator.onNextContinuous([this, totalItems, &moveSelection] {
    moveSelection(ButtonNavigator::nextPageIndex(selectorIndex, totalItems, visibleRows));
  });
  buttonNavigator.onPreviousContinuous([this, totalItems, &moveSelection] {
    moveSelection(ButtonNavigator::previousPageIndex(selectorIndex, totalItems, visibleRows));
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) selectResult();
}

void EpubReaderSearchActivity::searchScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<EpubReaderSearchActivity*>(user)->buildSearchScreen(screen);
}

void EpubReaderSearchActivity::buildSearchScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  setUiContentMargin(screen, renderer,
                     fui::Insets{static_cast<int16_t>(TouchHeaderBackButton::contentTop(renderer, mappedInput, safe.y)),
                                 static_cast<int16_t>(renderer.getScreenWidth() - safe.x - safe.width),
                                 static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height),
                                 static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  const auto statusRect = screen.takeTop(screen.theme().rowHeight);
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const int totalItems = getTotalItems();
  FilenameUiFontScope fonts(uiTarget, renderer);
  fui::ListProps props;
  props.labelText = screen.theme().bodyText;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = std::clamp<int>(rows, 1, RESULT_WINDOW_SIZE);
  topIndex = initialViewportPending ? followListSelection(selectorIndex, 0, visibleRows, totalItems)
                                    : scrollListBy(topIndex, 0, visibleRows, totalItems);
  initialViewportPending = false;

  const size_t drawCount =
      std::min({static_cast<size_t>(visibleRows), RESULT_WINDOW_SIZE, static_cast<size_t>(totalItems - topIndex)});
  for (size_t i = 0; i < drawCount; ++i) {
    if (session->readHit(topIndex + static_cast<int>(i), hit)) {
      labelWindow[i] = std::to_string(hit.spine + 1) + ". " + hit.snippet;
    } else {
      labelWindow[i] = tr(STR_BOOK_SEARCH_FAILED);
    }
    itemWindow[i] = fui::ListItem{};
    itemWindow[i].label = labelWindow[i].c_str();
    itemWindow[i].actionValue = static_cast<int16_t>(topIndex + static_cast<int>(i));
  }
  // readHit may discover an SD failure during this render; show it immediately.
  updateStatus();
  screen.target().text(statusRect, status.c_str(), screen.theme().smallText);

  props.items = itemWindow.data();
  props.count = static_cast<uint16_t>(drawCount);
  props.selectedIndex = static_cast<int16_t>(selectorIndex - topIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.topIndex = 0;
  screen.list(props);
  fui::drawListScrollIndicator(screen.target(), screen.body(), static_cast<size_t>(totalItems), visibleRows, topIndex,
                               screen.theme().listScrollWidth, screen.theme().listScrollSide,
                               screen.theme().listScrollInset);
}

void EpubReaderSearchActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput, safe);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_SEARCH_BOOK), true);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_SEARCH_BOOK), nullptr, true);
  }
  uiReady = false;
  renderUiApp(app, uiTarget);
  uiReady = true;
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  renderer.displayBuffer();
}
