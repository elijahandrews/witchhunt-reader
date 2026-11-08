#!/usr/bin/env python3
"""Compile real screenshot, reader-repair and SDK buffer functions with host I/O shims."""
import argparse
from pathlib import Path
import subprocess


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--negative-control', choices=('old-order', 'conditional-hook', 'stale-generation', 'late-cancel'))
    args = parser.parse_args()
    repo = args.repo.resolve()
    screenshot = (repo / 'src/util/ScreenshotUtil.cpp').read_text()
    reader = (repo / 'src/activities/reader/EpubReaderActivity.cpp').read_text()
    header = (repo / 'src/activities/reader/EpubReaderActivity.h').read_text()
    sdk = (repo / 'freeink-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp').read_text()
    request = function(screenshot, 'void ScreenshotUtil::request(')
    service = function(screenshot, 'void ScreenshotUtil::service(')
    restore = function(reader, 'void EpubReaderActivity::restoreCurrentPageToBufferIfPreRendered(')
    content = function(reader, 'uint16_t EpubReaderActivity::renderPageContentOnly(')
    status = function(reader, 'void EpubReaderActivity::renderStatusBar(')
    cached = function(reader, 'void EpubReaderActivity::displayPreRenderedPage(')
    hook = function(header, '  void prepareFramebufferForCapture() override')
    hook = hook.replace(' override', '')
    sync = function(sdk, 'void FreeInkDisplay::syncWriteBufferFromActive(')
    borrow = function(sdk, 'uint8_t* FreeInkDisplay::borrowSecondaryBuffer(')
    give_back = function(sdk, 'bool FreeInkDisplay::returnSecondaryBuffer(')
    if args.negative_control == 'old-order':
        pair = ('  if (renderer.hasSecondaryBuffer()) renderer.syncWriteBufferFromDisplayed();\n'
                '  activityManager.prepareFramebufferForCapture();')
        assert service.count(pair) == 1
        service = service.replace(pair, '  activityManager.prepareFramebufferForCapture();\n'
                                  '  if (renderer.hasSecondaryBuffer()) renderer.syncWriteBufferFromDisplayed();')
    if args.negative_control == 'conditional-hook':
        condition = '(!forCapture && !preRenderedPage.ready)'
        assert restore.count(condition) == 1
        restore = restore.replace(condition, '!preRenderedPage.ready')
    if args.negative_control == 'stale-generation':
        guard = 'capturedTextAaGeneration_ == preRenderedPage.textAaGeneration'
        assert guard in cached
        cached = cached.replace(guard, 'true')
    if args.negative_control == 'late-cancel':
        guard = 'const bool canceled = aaPreemptedByNavigation();'
        assert guard in cached
        cached = cached.replace(guard, 'const bool canceled = false;')
    code = r'''#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <ctime>
#include "components/themes/TapTargets.h"

template<typename T> auto makeUniqueNoThrow(size_t count) { return std::make_unique<T>(count); }
struct FontDecompressor {
  struct Stats { uint16_t fallbackOomGlyphs = 0; };
  Stats getStats() const { return {}; }
};
struct FontCacheManager {
  struct Scope { void endScanAndPrewarm() {} };
  void resetStats() {}
  Scope createPrewarmScope() { return {}; }
  const FontDecompressor* getDecompressor() { return nullptr; }
};

#define LOG_ERR(...) do {} while (0)
#define LOG_DBG(...) do {} while (0)
void checkHeapIntegrity(const char*) {}
struct HalDisplay { enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH }; };
static bool navigationPending = false;
static unsigned long nowMs = 1;
unsigned long millis() { return nowMs; }
struct RenderLock {
  static bool held;
  RenderLock() { assert(!held); held = true; }
  ~RenderLock() { held = false; }
  static bool peek() { return held; }
};
bool RenderLock::held = false;
class FreeInkDisplay {
 public:
  uint8_t a[4]{}, b[4]{};
  uint8_t* frameBuffer = a;
  uint8_t* frameBufferActive = b;
  uint8_t* _secondaryLent = nullptr;
  bool _redBaselineAuthoritative = false;
  size_t bufferSize = 4;
  void syncPendingAsync() {}
  void syncWriteBufferFromActive() const;
  uint8_t* borrowSecondaryBuffer(size_t* size);
  bool returnSecondaryBuffer();
};
// SDK_FUNCTIONS
class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };
  RenderMode mode = GRAYSCALE_MSB;
  FreeInkDisplay display;
  FontCacheManager fonts;
  unsigned grayBegins = 0, planeWrites = 0, overlays = 0;
  bool textSupport = false, acceptText = true, cancelDuringChrome = false;
  bool rejectErasedFringe = false, lastUsedAa = false;
  uint8_t* capLsb = nullptr; uint8_t* capMsb = nullptr;
  HalDisplay::RefreshMode lastMode = HalDisplay::FAST_REFRESH;
  bool supportsTextAa() const { return textSupport; }
  bool supportsDeferredGrayscaleBase() const { return false; }
  bool isErasingOpaqueGlyphs() const { return false; }
  void displayBuffer(HalDisplay::RefreshMode m) { ++displayCalls; lastMode = m; }
  void displayGrayscaleFrame(HalDisplay::RefreshMode m) { displayBuffer(m); }
  void triggerGrayscaleBaseAsync(HalDisplay::RefreshMode m) { displayBuffer(m); }
  bool displayTextAaOrBw(HalDisplay::RefreshMode m, const uint8_t* l, const uint8_t* h) {
    displayBuffer(m);
    if (rejectErasedFringe && l && h) assert(l[3] == 0 && h[3] == 0);
    return lastUsedAa = m == HalDisplay::FAST_REFRESH && l && h && acceptText;
  }
  HalDisplay::RefreshMode getLastRefreshMode() const { return lastMode; }
  uint8_t getLastDisplayModeByte() const { return 0; }
  struct GrayTimes { bool aborted = false; };
  template<class Render, class Abort> GrayTimes renderGrayscalePlanesSequential(Render, Abort) {
    ++overlays; return {};
  }
  template<class Render, class Abort> GrayTimes renderGrayscalePlanesInterleaved(Render, Abort, uint8_t*, uint8_t*) {
    ++overlays; return {};
  }
  void setRenderMode(RenderMode value) { mode = value; }
  FontCacheManager* getFontCacheManager() { return &fonts; }
  int getScreenHeight() const { return 800; }
  int getDisplayWidthBytes() const { return 4; }
  bool supportsGrayFrame() const { return !textSupport; }
  void setFastGrayscaleLut(bool) {}
  void beginGrayCapture(uint8_t* l, uint8_t* h) { ++grayBegins; capLsb = l; capMsb = h; }
  void endGrayCapture() { capLsb = capMsb = nullptr; }
  void copyGrayscaleLsbBuffers(uint8_t*) { ++planeWrites; }
  void copyGrayscaleMsbBuffers(uint8_t*) { ++planeWrites; }
  void clearScreen() { std::memset(getFrameBuffer(), 0, 4); }
  bool refreshPending = false;
  unsigned redraws = 0, chromeDraws = 0, syncs = 0, displayCalls = 0;
  bool isRefreshPending() const { return refreshPending; }
  bool hasSecondaryBuffer() const { return display.frameBufferActive; }
  void syncWriteBufferFromDisplayed() { ++syncs; display.syncWriteBufferFromActive(); }
  uint8_t* getFrameBuffer() { return display.frameBuffer; }
  uint32_t getBufferSize() const { return 4; }
  int getDisplayWidth() const { return 32; }
  int getDisplayHeight() const { return 1; }
  void getOrientedViewableTRBL(int* a, int* b, int* c, int* d) { *a = *b = *c = *d = 0; }
} renderer;
namespace WakeTrace { enum class Phase { PageVisible }; void mark(Phase) {} void logSummary() {} }
namespace CrossPointSettings {
constexpr int IMAGES_SUPPRESS = 0;
enum class STATUS_BAR_TITLE { NONE, CHAPTER_TITLE, BOOK_TITLE };
}
struct Settings {
  uint8_t screenMargin = 8;
  bool halfRefreshAfterImagePage = true;
  int getRefreshFrequency() const { return 15; }
  bool fastAntiAliasing = true, largeImagePlaceholder = true;
  bool touchReaderControls = true;
  bool statusBarPrintedPage = false, statusBarBattery = true, useClock = true, statusBarClock = true;
  CrossPointSettings::STATUS_BAR_TITLE statusBarTitle = CrossPointSettings::STATUS_BAR_TITLE::NONE;
} SETTINGS;
namespace ReaderUtils {
HalDisplay::RefreshMode nextRefreshCycleMode(int& remaining) {
  if (--remaining <= 0) { remaining = SETTINGS.getRefreshFrequency(); return HalDisplay::HALF_REFRESH; }
  return HalDisplay::FAST_REFRESH;
}
void displayWithRefreshCycle(GfxRenderer& r, int& remaining) { r.displayBuffer(nextRefreshCycleMode(remaining)); }
}
struct ImageBlock {
  static bool placeholderOnly;
  struct PlaceholderOnlyScope {
    PlaceholderOnlyScope() { assert(!placeholderOnly); placeholderOnly = true; }
    ~PlaceholderOnlyScope() { placeholderOnly = false; }
  };
};
bool ImageBlock::placeholderOnly = false;
namespace ChapterPageSpan { struct Display { int page = 2, total = 9; bool approximate = true; }; }
enum class SyncIndicator { None };
constexpr int STR_AUTO_TURN_ENABLED = 0, STR_UNNAMED = 1;
std::string tr(int) { return "label"; }
struct { unsigned getBatteryPercentage() const { return 86; } } powerManager;
namespace HalClock { bool isSynced() { return true; } time_t now() { return 7200; } }
struct Epub {
  float calculateProgress(int, float) const { return 0.4f; }
  int getTocIndexForSpineIndex(int) const { return -1; }
  struct TocItem { std::string title = "chapter"; };
  TocItem getTocItem(int) const { return {}; }
  std::string getTitle() const { return "book"; }
} epubInstance;
struct Rect { int x=0,y=0,width=0,height=0; };
struct Gui {
  void drawStatusBar(GfxRenderer& r, float, int, int, const std::string&, int, bool,
                     const std::string&, bool, bool, SyncIndicator, Rect* back = nullptr) const {
    if (back) *back=Rect{4,20,80,30};
    ++r.chromeDraws; r.getFrameBuffer()[3] = 99;
    if (r.capLsb) r.capLsb[3] = 0;
    if (r.capMsb) r.capMsb[3] = 0;
    if (r.cancelDuringChrome) navigationPending = true;
  }
} GUI;
namespace UITheme {
int getStatusBarTopHeight(bool) { return 2; }
int getStatusBarBottomHeight(bool) { return 3; }
int getStatusBarItemsHeight() { return 3; }
}
struct Page {
  uint8_t id;
  bool hasImages() const { return false; }
  bool renderTextOnly(GfxRenderer&, int, int, int, bool = false) const { return true; }
  void renderImagesFromGrayscaleCache(GfxRenderer&, int, int) const {}
  void render(GfxRenderer& r, int, int, int, bool force = true, bool mono = true) const {
    assert(r.getFrameBuffer()); ++r.redraws;
    std::memset(r.getFrameBuffer(), id, 4);
    if (r.capLsb) std::memset(r.capLsb, id, 4);
    if (r.capMsb) std::memset(r.capMsb, id, 4);
    r.getFrameBuffer()[1] = force;
    r.getFrameBuffer()[2] = (ImageBlock::placeholderOnly ? 1 : 0) | (mono ? 2 : 0);
  }
};
struct Section {
  int currentPage = 1;
  bool active = false, loadFails = false;
  int pageCount = 7;
  int estimatedTotalPages() const { return 9; }
  int getTocIndexForPage(int) const { return -1; }
  unsigned diskLoads = 0, buildLoads = 0;
  bool hasActiveBuild() const { return active; }
  std::unique_ptr<Page> loadPageFromSectionFile() {
    ++diskLoads; if (loadFails) return nullptr; return std::make_unique<Page>(Page{uint8_t(currentPage)});
  }
  std::unique_ptr<Page> loadPageFromActiveBuild(uint16_t page) {
    assert(page == currentPage); ++buildLoads; if (loadFails) return nullptr; return std::make_unique<Page>(Page{uint8_t(page)});
  }
};
class EpubReaderActivity {
 public:
  GfxRenderer& renderer;
  Section* section;
  struct { bool enabled=true; bool available() const { return enabled; } } jumpBack_;
  struct { bool hasTouch() const { return true; } } mappedInput;
  Epub* epub = &epubInstance;
  bool automaticPageTurnActive = false, forceLoadLargeImages = false;
  int currentSpineIndex = 1, pageTurnDuration = 60000;
  mutable int lastStatusBarPage = -11, lastStatusBarBattery = -12, lastStatusBarClockMinute = -13;
  mutable int lastStatusBarChapterTotal_ = -14;
  mutable bool lastStatusBarChapterApprox_ = false;
  struct BookmarkStore { bool has(uint16_t, uint16_t) const { return false; } } bookmarkStore;
  ChapterPageSpan::Display chapterPageDisplay() const { return {}; }
  std::string printedPageLabelFor(uint16_t) const { return {}; }
  int getImageOnlyPageYOffset(const Page&, int) { return 0; }
  int getEffectiveReaderFontId() const { return 1; }
  bool getEffectiveTextAntiAliasing() const { return true; }
  struct {
    bool ready = true; int spineIndex = 1, pageIndex = 1;
    bool incomplete = false; uint32_t textAaGeneration = 0;
  } preRenderedPage;
  uint32_t textAaGeneration_ = 0, capturedTextAaGeneration_ = 0;
  bool secondaryBufferDegraded_ = false, backgroundBuildThrough_ = false, lastTextAaPresented_ = false;
  bool pendingHalfRefreshAfterImagePage = false, pageHasPlaceholders = false;
  int pagesUntilFullRefresh = 15;
  HalDisplay::RefreshMode lastPageRefreshMode_ = HalDisplay::FAST_REFRESH;
  uint8_t lastPageDisplayModeByte_ = 0;
  struct {
    uint8_t low[4]{}, high[4]{}; bool available = true;
    bool ensure(size_t n) { assert(n == 4); return available; }
    uint8_t* lsb() { return low; } uint8_t* msb() { return high; }
  } bufferedGrayscalePlanes_;
  bool aaPreemptedByNavigation() const { return navigationPending; }
  bool usesDeferredAa() const { return false; }
  int getEffectiveImageRendering() const { return 1; }
  void publishPageLinkTargets(const Page&, int, int) {}
  void displayPreRenderedPage(const Page&, int, int, int, int);
  bool preRenderedPlanesStaged_ = true, pendingPreRender = true, usePreRenderedBuffer = true;
  EpubReaderActivity(GfxRenderer& r, Section& s) : renderer(r), section(&s) {}
  void restoreCurrentPageToBufferIfPreRendered(bool forCapture = false);
  // READER_HOOK
  uint16_t renderPageContentOnly(const Page&, int, int, int, int, bool forCapture = false);
  void renderStatusBar(bool recordDisplayed = true) const;
};
// READER_RESTORE
// READER_CONTENT
// READER_STATUS
// READER_CACHED
struct Manager {
  EpubReaderActivity* reader = nullptr;
  void prepareFramebufferForCapture() { if (reader) reader->prepareFramebufferForCapture(); }
} activityManager;
struct Capture {
  bool called = false, null = false, busy = false;
  uint8_t bytes[4]{};
} captured;
void send(const uint8_t* frame, uint32_t size, int width, int height, bool busy) {
  captured = {}; captured.called = true; captured.null = !frame; captured.busy = busy;
  if (frame) { assert(size == 4 && width == 32 && height == 1); std::memcpy(captured.bytes, frame, 4); }
}
class ScreenshotUtil {
 public:
  enum class Target { None, Card, Host };
  static void request(Target);
  static void service(GfxRenderer&, void (*)(const uint8_t*, uint32_t, int, int, bool));
  static void takeScreenshot(GfxRenderer& r) { send(r.getFrameBuffer(), 4, 32, 1, false); }
};
ScreenshotUtil::Target pendingTarget = ScreenshotUtil::Target::None;
unsigned long pendingDeadline = 0;
constexpr unsigned long kWaitMs = 5000;
// SCREENSHOT_FUNCTIONS
static unsigned checks = 0;
void check(bool valid, const char* why) {
  ++checks;
  if (!valid) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
void reset() {
  renderer.~GfxRenderer(); new (&renderer) GfxRenderer;
  SETTINGS = {}; navigationPending = false;
  captured = {}; activityManager.reader = nullptr; nowMs = 1; RenderLock::held = false;
  pendingTarget = ScreenshotUtil::Target::None;
}
void capture(ScreenshotUtil::Target target = ScreenshotUtil::Target::Host) {
  ScreenshotUtil::request(target); ScreenshotUtil::service(renderer, send);
}
int main() {
  for (int scenario = 0; scenario < 10; ++scenario) {
    reset(); Section section; EpubReaderActivity reader(renderer, section);
    renderer.textSupport = true; Page page{7};
    reader.renderPageContentOnly(page, 0, 0, 0, 0);
    check(reader.capturedTextAaGeneration_ != 0 && !renderer.capLsb && renderer.planeWrites == 0,
          "text masks captured without SPI and capture pointers disarmed");
    reader.preRenderedPage.textAaGeneration = reader.capturedTextAaGeneration_;
    reader.preRenderedPage.ready = false; // Real ready-to-display ownership handoff.
    if (scenario == 1) ++reader.preRenderedPage.textAaGeneration;
    if (scenario == 2) ++reader.preRenderedPage.pageIndex;
    if (scenario == 3) ++reader.preRenderedPage.spineIndex;
    if (scenario == 4) reader.preRenderedPage.incomplete = true;
    if (scenario == 5) renderer.cancelDuringChrome = true;
    if (scenario == 6) reader.pendingHalfRefreshAfterImagePage = true;
    if (scenario == 7) renderer.acceptText = false;
    if (scenario == 8) reader.pagesUntilFullRefresh = 1;
    if (scenario == 9) {
      const auto generation = reader.textAaGeneration_;
      reader.renderPageContentOnly(Page{9}, 0, 0, 0, 0);
      check(reader.textAaGeneration_ != generation, "replacement capture changes the generation");
    }
    renderer.rejectErasedFringe = true;
    reader.displayPreRenderedPage(page, 0, 0, 0, 0);
    check(renderer.displayCalls == 1 && reader.lastTextAaPresented_ == (scenario == 0),
          "only matching complete current-generation capture gets one-pass AA");
    check(renderer.getFrameBuffer()[0] == (scenario == 9 ? 9 : 7) && renderer.getFrameBuffer()[3] == 99,
          "fallback preserves BW target and live chrome");
    check(renderer.overlays == (scenario == 0 || scenario == 5 ? 0u : 1u),
          "success and late navigation skip replay; other fallbacks retain AA");
    check(reader.pagesUntilFullRefresh == (scenario == 6 || scenario == 8 ? 15 : 14),
          "cadence consumed once even on fallback");
    check(reader.capturedTextAaGeneration_ == 0 && !renderer.capLsb, "consumed masks cannot be reused");
  }
  // Software screenshots must invalidate a retained one-pass capture even when
  // reloading the displayed page fails, and must not start a new gray capture.
  for (bool fail : {false, true}) {
    reset(); Section section; EpubReaderActivity reader(renderer, section);
    renderer.textSupport = true; reader.renderPageContentOnly(Page{7}, 0, 0, 0, 0);
    section.loadFails = fail; const auto begins = renderer.grayBegins;
    reader.restoreCurrentPageToBufferIfPreRendered(true);
    check(reader.capturedTextAaGeneration_ == 0 && renderer.grayBegins == begins,
          "screenshot invalidates retained one-pass masks without capturing new ones");
  }
  for (bool activeBuild : {false, true}) for (bool staleSecondary : {true, false}) {
    reset(); Section section; section.active = activeBuild; EpubReaderActivity reader(renderer, section);
    activityManager.reader = &reader;
    std::memset(renderer.display.a, 2, 4); std::memset(renderer.display.b, 1, 4);
    if (staleSecondary) {
      // Actual SDK borrow/return reproduces Background-B destroying the displayed copy.
      size_t size; auto* scratch = renderer.display.borrowSecondaryBuffer(&size);
      assert(size == 4); std::memset(scratch, 87, 4); assert(renderer.display.returnSecondaryBuffer());
      check(renderer.display.frameBufferActive[0] == 2, "returned secondary is next page");
    }
    for (int repeat = 0; repeat < 3; ++repeat) {
      captured = {}; capture(repeat == 2 ? ScreenshotUtil::Target::Card : ScreenshotUtil::Target::Host);
      check(captured.called && !captured.null && !captured.busy, "capture succeeds");
      check(captured.bytes[0] == 1, "capture is logical current page, including repeated stale-secondary case");
      check(captured.bytes[3] == 99, "current status chrome restored exactly once");
      check(renderer.redraws == unsigned(repeat + 1) && renderer.chromeDraws == renderer.redraws,
            "one explicit redraw per reader capture");
      check(section.currentPage == 1 && renderer.displayCalls == 0, "no navigation or display refresh");
      check(reader.lastStatusBarPage == -11 && reader.lastStatusBarBattery == -12 &&
                reader.lastStatusBarClockMinute == -13 && reader.lastStatusBarChapterTotal_ == -14 &&
                !reader.lastStatusBarChapterApprox_, "capture does not acknowledge displayed chrome");
      check(renderer.mode == GfxRenderer::BW && renderer.grayBegins == 0 && renderer.planeWrites == 0,
            "capture stays BW without gray-plane allocation or hardware staging");
      check(captured.bytes[1] == 0 && captured.bytes[2] == (activeBuild ? 3 : 2),
            "capture preserves completed or in-progress image policy");
      check(!reader.preRenderedPage.ready && !reader.preRenderedPlanesStaged_ &&
                !reader.pendingPreRender && !reader.usePreRenderedBuffer, "stale pre-render flags discarded");
    }
    check(activeBuild ? section.buildLoads == 3 && !section.diskLoads : section.diskLoads == 3 && !section.buildLoads,
          "active and completed section loaders remain distinct");
  }
  for (bool active : {false, true}) for (bool placeholders : {false, true}) for (bool forced : {false, true}) {
    reset(); Section section; section.active = active; EpubReaderActivity reader(renderer, section);
    reader.forceLoadLargeImages = forced; SETTINGS.largeImagePlaceholder = placeholders;
    activityManager.reader = &reader; capture();
    check(captured.bytes[1] == (!active && (forced || !placeholders)), "large-image setting and one-page override preserved");
    check(captured.bytes[2] == (active ? 3 : 2) && !ImageBlock::placeholderOnly,
          "active-build placeholder scope ends after BW render");
  }
  for (bool active : {false, true}) {
    reset(); Section section; section.active = active; section.loadFails = true;
    EpubReaderActivity reader(renderer, section); activityManager.reader = &reader; capture();
    check(!reader.preRenderedPage.ready && !reader.preRenderedPlanesStaged_ &&
              !reader.pendingPreRender && !reader.usePreRenderedBuffer,
          "failed page load cannot retain pre-render ownership after sync");
    check(!renderer.redraws && !renderer.chromeDraws && !renderer.planeWrites, "failed load draws nothing");
  }
  for (bool borrowed : {false, true}) {
    reset(); Section section; EpubReaderActivity reader(renderer, section); activityManager.reader = &reader;
    if (borrowed) { size_t size; renderer.display.borrowSecondaryBuffer(&size); }
    else renderer.display.frameBufferActive = nullptr;
    std::memset(renderer.display.a, 2, 4); capture();
    check(captured.bytes[0] == 1 && captured.bytes[3] == 99 && renderer.syncs == 0,
          "borrowed or absent secondary still captures current page");
  }
  reset(); Section section; EpubReaderActivity reader(renderer, section); activityManager.reader = &reader;
  reader.preRenderedPage.ready = false; reader.restoreCurrentPageToBufferIfPreRendered();
  check(!renderer.redraws && !section.diskLoads, "normal exit helper remains conditional");
  reader.preRenderedPage.ready = true; reader.restoreCurrentPageToBufferIfPreRendered();
  check(renderer.redraws == 1 && renderer.chromeDraws == 0, "normal exit behavior preserved");
  check(renderer.grayBegins == 1 && renderer.planeWrites == 2 && renderer.getFrameBuffer()[1] == 1,
        "ordinary content helper retains its pre-render gray/image policy");
  TapTargets::Recorder::Builder existing; existing.add(1,1,2,2,9);
  TapTargets::screenActions().record(existing);
  reader.renderStatusBar(false);
  check(TapTargets::screenActions().hitTest(1,1)==9 && TapTargets::screenActions().hitTest(5,21)==-1,
        "screenshot-only chrome does not replace displayed footer targets");
  reader.renderStatusBar();
  check(TapTargets::screenActions().hitTest(5,21)==0 && TapTargets::screenActions().hitTest(1,1)==-1,
        "displayed footer records its button");
  reader.jumpBack_.enabled=false; reader.renderStatusBar();
  check(!TapTargets::screenActions().hasTargets(), "expired history clears displayed footer targets");
  check(reader.lastStatusBarPage == 2 && reader.lastStatusBarBattery == 86 &&
            reader.lastStatusBarClockMinute == 120 && reader.lastStatusBarChapterTotal_ == 9 &&
            reader.lastStatusBarChapterApprox_, "ordinary status render still records displayed chrome");
  for (bool secondary : {false, true}) {
    reset(); std::memset(renderer.display.a, 7, 4); std::memset(renderer.display.b, 9, 4);
    if (!secondary) renderer.display.frameBufferActive = nullptr;
    capture();
    check(captured.bytes[0] == (secondary ? 9 : 7) && renderer.redraws == 0, "non-reader sync without redraw");
  }
  reset(); Section nullSection; EpubReaderActivity nullReader(renderer, nullSection);
  activityManager.reader = &nullReader; renderer.display.frameBuffer = nullptr;
  capture(); check(captured.called && captured.null && !captured.busy && !renderer.redraws && !nullSection.diskLoads,
                   "released canvas preserves host null-frame error path");
  reset(); RenderLock::held = true; capture();
  check(!captured.called && pendingTarget == ScreenshotUtil::Target::Host, "locked frame remains pending");
  RenderLock::held = false; ScreenshotUtil::service(renderer, send);
  check(captured.called, "pending capture resumes after lock release");
  reset(); renderer.refreshPending = true; capture();
  check(!captured.called, "pending refresh is not captured");
  nowMs = 5001; ScreenshotUtil::service(renderer, send);
  check(captured.called && captured.busy && captured.null, "deadline keeps busy response");
  std::printf("%u production cached-AA generation/cancellation/cadence and screenshot checks passed\n", checks);
}
'''
    code = code.replace('// SDK_FUNCTIONS', sync + '\n' + borrow + '\n' + give_back)
    code = code.replace('// READER_HOOK', hook)
    code = code.replace('// READER_RESTORE', restore)
    code = code.replace('// READER_CONTENT', content)
    code = code.replace('// READER_STATUS', status)
    code = code.replace('// READER_CACHED', cached)
    code = code.replace('// SCREENSHOT_FUNCTIONS', request + '\n' + service)
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / 'probe.cpp').write_text(code)
    command = ['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-g',
               '-I'+str(repo / 'src'), str(output / 'probe.cpp'), '-o', str(output / 'probe')]
    if args.sanitize:
        command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(command, check=True)
    result = subprocess.run([str(output / 'probe')], capture_output=True, text=True)
    (output / 'run.log').write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end='')
    if args.negative_control:
        assert result.returncode != 0, 'Mutated production capture unexpectedly passed'
        print('Negative control rejected:', args.negative_control)
    else:
        assert result.returncode == 0, 'Production screenshot regression failed'


if __name__ == '__main__':
    main()
