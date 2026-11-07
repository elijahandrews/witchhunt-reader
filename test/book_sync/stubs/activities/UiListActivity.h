#pragma once
#include <I18n.h>

#include <memory>
#include <string>
#include <vector>
inline bool injectCancel = false;
inline unsigned long millis() {
  static unsigned long time = 0;
  return ++time;
}
inline void delay(unsigned) {}
struct RenderLock {
  template <typename T>
  explicit RenderLock(T&) {}
};
struct GfxRenderer {
  std::vector<std::string> text;
  int getScreenHeight() const { return 800; }
  void drawCenteredText(int, int, const char* value) { text.emplace_back(value); }
};
struct MappedInputManager {
  enum class Button { Back };
  void update() {}
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
  virtual void activateIndex(int) = 0;
  virtual void buildScreen(UiScreen&) = 0;
  virtual void drawChrome() {}
  void layoutListArea(UiScreen&) {}
  void requestUpdateAndWait() {
    renderer.text.clear();
    drawChrome();
  }
  void requestUpdate(bool = false) { requestUpdateAndWait(); }
  void finish() {}
  template <typename T, typename F>
  void startActivityForResult(std::unique_ptr<T>, F fn) {
    fn(ActivityResult{});
  }
};
