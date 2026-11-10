#pragma once
#include <optional>

#include "EpubLinkReturnState.h"

// One recent deliberate jump, separate from the nested footnote-return stack.
// Ordinary reading expires it after five page turns; taking it never bounces.
class ReaderJumpBack {
 public:
  struct Entry {
    EpubLinkReturnState::Position position;
    bool followedLink = false;
  };
  void remember(EpubLinkReturnState::Position position, bool followedLink = false) {
    entry_ = Entry{position, followedLink};
    remaining_ = 6;
  }
  bool available() const { return entry_.has_value(); }
  void pageTurn() {
    if (remaining_ && --remaining_ == 0) clear();
  }
  void clear() {
    entry_.reset();
    remaining_ = 0;
  }
  std::optional<Entry> take() {
    auto result = entry_;
    clear();
    return result;
  }

 private:
  std::optional<Entry> entry_;
  uint8_t remaining_ = 0;
};
