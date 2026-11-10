#pragma once
#include <I18n.h>

#include <memory>
#include <string>
#include <vector>
inline bool injectCancel = false, injectTouch = false, injectBackPress = false;
inline int tapX = 50, tapY = 740;
inline unsigned long fakeMillis = 0, delayCalls = 0;
inline unsigned long micros() { return fakeMillis * 1000; }
inline unsigned long millis() { return fakeMillis; }
inline void delay(unsigned ms) {
  ++delayCalls;
  fakeMillis += ms;
}
struct RenderLock {
  template <typename T>
  explicit RenderLock(T&) {}
};
struct Rect {
  int x, y, width, height;
};
struct GfxRenderer {
  std::vector<std::string> text, history;
  mutable std::string footer;
  int getScreenHeight() const { return 800; }
  void clearScreen() { text.clear(); }
  void displayBuffer() {}
  void drawCenteredText(int, int, const char* value) {
    text.emplace_back(value);
    history.emplace_back(value);
  }
};
struct MappedInputManager {
  enum class Button { Back };
  uint8_t backRawIndex = 0;
  void update() {}
  bool hasTouchHardware() const { return true; }
  bool wasPressed(Button) {
    const bool r = injectBackPress || injectCancel;
    injectBackPress = injectCancel = false;
    return r;
  }
  bool wasScreenTapped(int& x, int& y) {
    if (!injectTouch) return false;
    injectTouch = false;
    x = tapX;
    y = tapY;
    return true;
  }
  void suppressNextBackRelease() {}
  const char* withBackArrow(const char* value) { return value; }
  struct Labels {
    const char *btn1, *btn2, *btn3, *btn4;
  };
  Labels mapLabels(const char* a, const char* b, const char* c, const char* d) { return {a, b, c, d}; }
};
struct ActivityResult {
  bool isCancelled = false;
};
enum class HomeMenuItem { FILE_TRANSFER };
struct ActivityManager {
  void goHome(HomeMenuItem) {}
  void goToReader(const std::string&, bool) {}
};
inline ActivityManager activityManager;
class Activity {
 protected:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

 public:
  Activity(const char*, GfxRenderer& r, MappedInputManager& i) : renderer(r), mappedInput(i) {}
  virtual ~Activity() = default;
  virtual void onEnter() {}
  virtual void onExit() {}
  virtual void loop() {}
  virtual void render(RenderLock&&) {}
  virtual bool preventAutoSleep() { return false; }
  void requestUpdateAndWait() { render(RenderLock(*this)); }
  void requestUpdate(bool = false) { requestUpdateAndWait(); }
  template <typename T, typename F>
  void startActivityForResult(std::unique_ptr<T>, F f) {
    f(ActivityResult{});
  }
};
