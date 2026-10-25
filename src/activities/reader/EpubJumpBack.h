#pragma once

#include "EpubLinkBackStack.h"

// One recent deliberate jump per open book. Independent of the persisted link
// stack: chapter/search jumps must not change link-detour sync behavior. No heap
// allocation or writes on page turns; normal reading expires the affordance.
class EpubJumpBack {
 public:
  struct Target {
    EpubLinkBackStack::Entry position;
    bool followedLink = false;
  };
  static constexpr uint8_t kPageTurnGrace = 5;

  bool available() const { return turnsLeft_ != 0; }
  void remember(const EpubLinkBackStack::Entry& position, bool followedLink = false) {
    target_ = {position, followedLink};
    turnsLeft_ = kPageTurnGrace + 1;
  }
  // Available on the landing page and after five ordinary turns, gone on the sixth.
  void pageTurned() {
    if (turnsLeft_) --turnsLeft_;
  }
  void clear() { turnsLeft_ = 0; }
  bool take(Target& out) {
    if (!available()) return false;
    out = target_;
    clear();
    return true;
  }

 private:
  Target target_{};
  uint8_t turnsLeft_ = 0;
};
static_assert(sizeof(EpubJumpBack) <= 48, "Keep jump-back state bounded on device and host");
