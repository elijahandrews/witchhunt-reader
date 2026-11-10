#pragma once
#if CROSSINK_SCALABLE_FONTS
#include <EpdFontFamily.h>
#include <HalScalableFont.h>

#include <array>
#include <cstdint>

// Additional publisher sizes borrow a resident native four-face family. Keep
// stable descriptors for at most 32 active sizes; preserve the two most recent
// families while layout measures an adjacent run or ruby annotation.
class ScalableSizeCache {
  struct Entry {
    uint16_t points = 0;
    int id = 0;
    uint32_t used = 0;
  };
  std::array<Entry, 32> entries_{};
  std::array<uint16_t, 32> failures_{};
  uint32_t sequence_ = 0;
  int recent_[2] = {};
  bool failureOverflow_ = false;
  void touch(Entry& entry) {
    entry.used = ++sequence_;
    if (recent_[0] != entry.id) {
      recent_[1] = recent_[0];
      recent_[0] = entry.id;
    }
  }
  template <class Renderer>
  void erase(Renderer& renderer, Entry& entry, HalScalableFont* const faces[4]) {
    renderer.removeFont(entry.id);
    for (unsigned style = 0; style < 4; ++style)
      if (faces[style]) faces[style]->releaseSize64(entry.points);
    for (auto& id : recent_)
      if (id == entry.id) id = 0;
    entry = {};
  }
  template <class Renderer>
  bool evict(Renderer& renderer, HalScalableFont* const faces[4]) {
    Entry* oldest = nullptr;
    for (auto& entry : entries_)
      if (entry.id && entry.id != recent_[0] && entry.id != recent_[1] && (!oldest || entry.used < oldest->used))
        oldest = &entry;
    if (!oldest) return false;
    erase(renderer, *oldest, faces);
    return true;
  }

 public:
  void retryFailures() {
    failures_.fill(0);
    failureOverflow_ = false;
  }
  template <class Renderer>
  void clear(Renderer& renderer, HalScalableFont* const faces[4]) {
    for (auto& entry : entries_)
      if (entry.id) erase(renderer, entry, faces);
    retryFailures();
    sequence_ = 0;
  }
  template <class Renderer>
  int resolve(Renderer& renderer, HalScalableFont* const faces[4], uint16_t points, int id) {
    if (!points || points > HalScalableFont::MaxPublisherPoints64 || !faces[0]) return 0;
    for (auto& entry : entries_)
      if (entry.id && entry.points == points) {
        touch(entry);
        return entry.id;
      }
    if (failureOverflow_) return 0;
    for (const auto failed : failures_)
      if (failed == points) return 0;
    Entry* slot = nullptr;
    do {
      for (auto& entry : entries_)
        if (!entry.id) {
          slot = &entry;
          break;
        }
    } while (!slot && evict(renderer, faces));
    if (slot && !renderer.getFontMap().count(id)) {
      for (;;) {
        const EpdFont* styles[4] = {};
        bool complete = true;
        for (unsigned style = 0; style < 4; ++style)
          if (faces[style]) {
            styles[style] = faces[style]->atSize64(points);
            if (!styles[style]) {
              complete = false;
              break;
            }
          }
        if (complete && styles[0]) {
          renderer.insertFont(id, EpdFontFamily(styles[0], styles[1], styles[2], styles[3]));
          *slot = {points, id, 0};
          touch(*slot);
          return id;
        }
        for (unsigned style = 0; style < 4; ++style)
          if (faces[style]) faces[style]->releaseSize64(points);
        if (!evict(renderer, faces)) break;
      }
    }
    renderer.noteFontFailure();
    for (auto& failed : failures_)
      if (!failed) {
        failed = points;
        return 0;
      }
    failureOverflow_ = true;
    return 0;
  }
};
#endif
