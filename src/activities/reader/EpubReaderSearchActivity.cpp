#include "EpubReaderSearchActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <new>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/themes/ListTouchBand.h"
#include "fontIds.h"

void EpubReaderSearchActivity::onEnter() {
  Activity::onEnter();
  editQuery();
}
void EpubReaderSearchActivity::onExit() {
  session_.reset();
  Activity::onExit();
}
void EpubReaderSearchActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
void EpubReaderSearchActivity::editQuery() {
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_FIND_IN_BOOK), query_,
                                                                 BookTextSearch::MAX_QUERY_BYTES, InputType::Text),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             if (!session_) cancel();
                             return;
                           }
                           RenderLock lock(*this);
                           query_ = std::get<KeyboardResult>(result.data).text;
                           session_.reset();
                           session_.reset(new (std::nothrow) BookSearchSession(epub_, query_));
                           invalidQuery_ = session_ && !session_->validQuery();
                           startFailed_ = !session_ || (!invalidQuery_ && !session_->begin());
                           if (invalidQuery_ || startFailed_) session_.reset();
                           selected_ = 0;
                           shownSection_ = -1;
                           lastProgressPaint_ = millis();
                           requestUpdate();
                         });
}
int EpubReaderSearchActivity::rowCount() const {
  return 1 + (session_ && session_->done() ? static_cast<int>(session_->count()) : 0);
}
int EpubReaderSearchActivity::pageItems() const {
  const auto rect = UITheme::getContentRect(renderer, true, true);
  return std::max(1, (rect.height - 125) / 58);
}
void EpubReaderSearchActivity::loop() {
  ButtonEventManager::ButtonEvent event;
  while (buttonEvents.consumeEvent(event)) {
    if (event.type != ButtonEventManager::PressType::Short) continue;
    if (event.button == MappedInputManager::Button::Back) {
      cancel();
      return;
    }
    if (event.button == MappedInputManager::Button::Confirm && (!session_ || session_->done())) {
      if (selected_ == 0) {
        editQuery();
      } else {
        RenderLock lock(*this);
        if (session_->readHit(static_cast<uint32_t>(selected_ - 1), hit_)) {
          setResult(BookSearchResult{hit_.spine, hit_.sourceOffset});
          finish();
        } else {
          startFailed_ = true;
          requestUpdate();
        }
      }
      return;
    }
  }
  if (session_ && !session_->done()) {
    RenderLock lock(*this);
    session_->step();
    // Hundreds of tiny spine items must not each force an e-ink refresh. Input and
    // the scan still advance every loop; only the progress repaint is throttled.
    const auto now = millis();
    if (session_->done() || (shownSection_ != session_->sectionsScanned() && now - lastProgressPaint_ >= 1000)) {
      shownSection_ = session_->sectionsScanned();
      lastProgressPaint_ = now;
      if (session_->done() && session_->count()) selected_ = 1;
      requestUpdate();
    }
    return;
  }
  navigator_.onNextList(selected_, rowCount(), [this] { requestUpdate(); }, pageItems());
  navigator_.onPreviousList(selected_, rowCount(), [this] { requestUpdate(); }, pageItems());
}
void EpubReaderSearchActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto rect = UITheme::getContentRect(renderer, true, true);
  renderer.drawCenteredText(UI_12_FONT_ID, rect.y + 12, tr(STR_FIND_IN_BOOK), true, EpdFontFamily::BOLD);
  const auto query = renderer.truncatedText(UI_10_FONT_ID, query_.c_str(), rect.width - 30);
  renderer.drawText(UI_10_FONT_ID, rect.x + 15, rect.y + 48, query.c_str());
  const bool searching = session_ && !session_->done();
  char status[160];
  if (searching) {
    snprintf(status, sizeof(status), "%s %d/%d", tr(STR_SEARCHING),
             std::min(session_->sectionsScanned() + 1, session_->sectionCount()), session_->sectionCount());
  } else if (startFailed_ || (session_ && session_->failed())) {
    snprintf(status, sizeof(status), "%s", tr(STR_BOOK_SEARCH_FAILED));
  } else if (invalidQuery_) {
    snprintf(status, sizeof(status), "%s", tr(STR_BOOK_SEARCH_ENTER_TEXT));
  } else if (session_ && session_->limited()) {
    snprintf(status, sizeof(status), tr(STR_BOOK_SEARCH_LIMIT), static_cast<unsigned long>(session_->count()));
  } else if (session_ && session_->failedSections()) {
    snprintf(status, sizeof(status), tr(STR_BOOK_SEARCH_PARTIAL), static_cast<unsigned long>(session_->count()),
             session_->failedSections());
  } else if (session_ && session_->count()) {
    snprintf(status, sizeof(status), tr(STR_BOOK_SEARCH_COUNT), static_cast<unsigned long>(session_->count()));
  } else {
    snprintf(status, sizeof(status), "%s", tr(STR_SEARCH_NO_MATCHES));
  }
  const auto statusLine = renderer.truncatedText(UI_10_FONT_ID, status, rect.width - 30);
  renderer.drawText(UI_10_FONT_ID, rect.x + 15, rect.y + 78, statusLine.c_str());
  if (searching) {
    ListTouchBand::recordUniformRows(rect.x, rect.width, rect.y + 115, 58, 0, 0);
  } else {
    const int rows = pageItems();
    const int first = (selected_ / rows) * rows;
    ListTouchBand::recordUniformRows(rect.x, rect.width, rect.y + 115, 58, first, std::min(rows, rowCount() - first));
    for (int i = first; i < rowCount() && i < first + rows; ++i) {
      const int y = rect.y + 115 + (i - first) * 58;
      const bool ink = i != selected_;
      if (!ink) renderer.fillRect(rect.x, y, rect.width, 58);
      if (i == 0) {
        renderer.drawText(UI_10_FONT_ID, rect.x + 15, y + 16, tr(STR_BOOK_SEARCH_AGAIN), ink);
        continue;
      }
      if (!session_->readHit(static_cast<uint32_t>(i - 1), hit_)) {
        renderer.drawText(UI_10_FONT_ID, rect.x + 15, y + 16, tr(STR_BOOK_SEARCH_FAILED), ink);
        continue;
      }
      const int toc = epub_->getTocIndexForSpineIndex(hit_.spine);
      std::string chapter;
      if (toc >= 0) chapter = epub_->getTocItem(toc).title;
      if (chapter.empty()) chapter = std::string(tr(STR_SECTION_PREFIX)) + std::to_string(hit_.spine + 1);
      chapter = std::to_string(i) + ". " + chapter;
      chapter = renderer.truncatedText(UI_10_FONT_ID, chapter.c_str(), rect.width - 30);
      renderer.drawText(UI_10_FONT_ID, rect.x + 15, y + 3, chapter.c_str(), ink, EpdFontFamily::BOLD);
      const auto snippet = renderer.truncatedText(UI_10_FONT_ID, hit_.snippet, rect.width - 30);
      renderer.drawText(UI_10_FONT_ID, rect.x + 15, y + 29, snippet.c_str(), ink);
    }
  }
  const bool pages = !searching && rowCount() > pageItems();
  const auto hints = mappedInput.mapHints(tr(STR_BACK), searching ? "" : tr(STR_SELECT),
                                          pages ? tr(STR_LIST_PAGE_PREV) : "", pages ? tr(STR_LIST_PAGE_NEXT) : "",
                                          searching ? "" : tr(STR_DIR_UP), searching ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, hints.front.btn1, hints.front.btn2, hints.front.btn3, hints.front.btn4);
  GUI.drawSideButtonHints(renderer, hints.side.up, hints.side.down);
  renderer.displayBuffer();
}
ListRowTap::Result EpubReaderSearchActivity::selectListRow(int index) {
  if (session_ && !session_->done()) return ListRowTap::apply(-1, 0, selected_);
  return ListRowTap::apply(index, rowCount(), selected_);
}
