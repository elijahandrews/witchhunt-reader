#pragma once

#include <Epub.h>
#include <Epub/BookSearchSession.h>

#include <atomic>
#include <memory>
#include <string>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderSearchActivity final : public Activity {
 public:
  EpubReaderSearchActivity(GfxRenderer& renderer, MappedInputManager& input, std::shared_ptr<Epub> epub)
      : Activity("EpubReaderSearch", renderer, input), epub_(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return session_ && !session_->done(); }
  ListRowTap::Result selectListRow(int index) override;

 private:
  std::shared_ptr<Epub> epub_;
  std::unique_ptr<BookSearchSession> session_;
  // One record, reused for row rendering/selection under the render lock. Its 247bytes
  // stay out of the small task stack, as does the once-allocated search session.
  BookSearchSession::Hit hit_;
  ButtonNavigator navigator_;
  std::string query_;
  std::atomic<int> selected_{0};
  int shownSection_ = -1;
  unsigned long lastProgressPaint_ = 0;
  bool invalidQuery_ = false;
  bool startFailed_ = false;
  void editQuery();
  void cancel();
  int rowCount() const;
  int pageItems() const;
};
