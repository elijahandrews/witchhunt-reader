#pragma once
#include <cstdint>

// Bind a touch contact to the page it began on. A delayed loop tick must not
// turn a hold from the old page into a lookup on a newly rendered page.
class ReaderWordHold {
 public:
  struct Page {
    int spine;
    int page;
    uint32_t shownAt;
    uint8_t orientation;
    bool operator==(const Page&) const = default;
  };
  void observe(bool candidate, bool ready, const Page& page, uint32_t now, uint32_t heldMs) {
    if (!candidate) {
      reset();
      return;
    }
    if (!tracking_ || heldMs < lastHeldMs_) {
      page_ = page;
      armed_ = ready && heldMs <= static_cast<uint32_t>(now - page.shownAt);
      tracking_ = true;
    }
    lastHeldMs_ = heldMs;
    if (!ready || !(page_ == page)) armed_ = false;
  }
  bool consume(const Page& page) {
    const bool valid = armed_ && page_ == page;
    armed_ = false;
    return valid;
  }
  void reset() { armed_ = tracking_ = false; }

 private:
  Page page_{};
  uint32_t lastHeldMs_ = 0;
  bool armed_ = false;
  bool tracking_ = false;
};
