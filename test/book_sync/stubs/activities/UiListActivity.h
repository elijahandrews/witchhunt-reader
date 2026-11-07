#pragma once
#include <I18n.h>

#include <memory>
#include <string>
#include <vector>

#include "components/themes/ButtonHintStrip.h"
inline bool injectCancel = false;
inline bool injectTouch = false, injectBackPress = false;
inline int tapX = 50, tapY = 780;
inline unsigned long fakeMillis = 0, delayCalls = 0;
inline unsigned long millis() { return fakeMillis; }
inline void delay(unsigned ms) {
  ++delayCalls;
  fakeMillis += ms;
}
namespace touchtransform {
enum Orientation { Portrait, Landscape };
}
struct RenderLock {
  template <typename T>
  explicit RenderLock(T&) {}
};
struct GfxRenderer {
  std::vector<std::string> text;
  std::vector<std::string> history;
  std::string footer;
  int getScreenHeight() const { return 800; }
  void drawCenteredText(int, int, const char* value) {
    text.emplace_back(value);
    history.emplace_back(value);
  }
};
struct MappedInputManager {
  enum class Button { Back };
  uint8_t backRawIndex = 0;
  void update() {}
  bool wasPressed(Button) {
    bool result = injectBackPress;
    injectBackPress = false;
    return result;
  }
  uint8_t rawIndex(Button) const { return backRawIndex; }
  bool wasScreenTappedIn(touchtransform::Orientation orientation, int& x, int& y) {
    if (!injectTouch || orientation != touchtransform::Portrait) return false;
    injectTouch = false;
    x = tapX;
    y = tapY;
    return true;
  }
};
struct ButtonEventManager {
  struct ButtonEvent {
    MappedInputManager::Button button;
  };
  void update() {}
  void drain() {}
  bool consumeEvent(ButtonEvent& event) {
    if (!injectCancel) return false;
    injectCancel = false;
    event.button = MappedInputManager::Button::Back;
    return true;
  }
};
struct ActivityResult {
  bool isCancelled = false;
};
struct UiScreen {};
class UiListActivity {
 protected:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  ButtonEventManager buttonEvents;

 public:
  UiListActivity(const char*, GfxRenderer& r, MappedInputManager& i) : renderer(r), mappedInput(i) {}
  virtual ~UiListActivity() = default;
  virtual void onEnter() {}
  virtual void onExit() {}
  virtual bool usesWifi() const { return false; }
  virtual bool preventAutoSleep() { return false; }
  virtual int listCount() const = 0;
  virtual const char* headerTitle() const = 0;
  virtual const char* footerConfirmLabel() const = 0;
  virtual const char* footerBackLabel() const { return "back"; }
  virtual void activateIndex(int) = 0;
  virtual void buildScreen(UiScreen&) = 0;
  virtual void drawChrome() {}
  void layoutListArea(UiScreen&) {}
  void requestUpdateAndWait() {
    renderer.text.clear();
    drawChrome();
    renderer.footer = footerBackLabel();
    ButtonHintStrip::Strip strip;
    strip.y = 760;
    strip.height = 40;
    for (int i = 0; i < 4; ++i) {
      strip.x[i] = i * 100;
      strip.width[i] = 100;
    }
    strip.active[mappedInput.rawIndex(MappedInputManager::Button::Back)] = true;
    ButtonHintStrip::record(strip);
  }
  void requestUpdate(bool = false) { requestUpdateAndWait(); }
  void finish() {}
  template <typename T, typename F>
  void startActivityForResult(std::unique_ptr<T>, F fn) {
    fn(ActivityResult{});
  }
};
