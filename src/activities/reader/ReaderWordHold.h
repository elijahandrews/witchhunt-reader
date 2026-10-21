#pragma once

#include <cstdint>

// The SDK owns duration, slop and multi-touch cancellation. This binds a live
// tap candidate to the displayed page, including when the loop missed the
// sampler's brief press edge. No extra timer or per-page allocation is needed.
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
    // A shorter duration is a new contact even if the loop missed its release.
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

  void reset() {
    armed_ = false;
    tracking_ = false;
  }

 private:
  Page page_{};
  uint32_t lastHeldMs_ = 0;
  bool armed_ = false;
  bool tracking_ = false;
};
