#pragma once
#include <Epub.h>
#include <Epub/BookSearchSession.h>
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <array>
#include <atomic>
#include <memory>
#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderSearchActivity final : public Activity {
  using UiApp = freeink::ui::FreeInkApp<20, 4>;
  static constexpr size_t RESULT_WINDOW_SIZE = 20;

  std::shared_ptr<Epub> epub;
  std::string query;
  std::unique_ptr<BookSearchSession> session;
  BookSearchSession::Hit hit;
  uint32_t lastPaintAt = 0;
  std::string status;
  ButtonNavigator buttonNavigator;
  bool ignoreInitialUpRelease = false;
  bool ignoreInitialDownRelease = false;
  int selectorIndex = 0;
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;
  int topIndex = 0;
  bool initialViewportPending = true;
  bool pendingSelection = false;
  std::array<std::string, RESULT_WINDOW_SIZE> labelWindow{};
  std::array<freeink::ui::ListItem, RESULT_WINDOW_SIZE> itemWindow{};

  static void searchScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildSearchScreen(UiApp::ScreenType& screen);
  void selectResult();
  void updateStatus();

  int getTotalItems() const;

 public:
  EpubReaderSearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::shared_ptr<Epub>& epub,
                           std::string query);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return session && !session->done(); }
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
};
