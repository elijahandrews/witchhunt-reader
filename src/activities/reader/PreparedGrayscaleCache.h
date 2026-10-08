#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// RAM-only masks; no controller writes are performed by this cache. Allocation
// policy belongs to the reader, which supplies a PSRAM-only allocator.
class PreparedGrayscaleCache {
 public:
  struct Key {
    const void* section = nullptr;
    int spine = -1, page = -1, font = 0, left = 0, top = 0;
    uint8_t orientation = 0, darkness = 0;
    bool operator==(const Key& other) const {
      return section == other.section && spine == other.spine && page == other.page && font == other.font &&
             left == other.left && top == other.top && orientation == other.orientation && darkness == other.darkness;
    }
  };
  using Allocate = void* (*)(size_t);
  using Free = void (*)(void*);

  PreparedGrayscaleCache() = default;
  ~PreparedGrayscaleCache() { release(); }
  PreparedGrayscaleCache(const PreparedGrayscaleCache&) = delete;
  PreparedGrayscaleCache& operator=(const PreparedGrayscaleCache&) = delete;

  // A failed or cancelled capture never leaves the prior masks usable. Same-size
  // captures reuse both allocations and clear all pixels before the new draw.
  uint32_t begin(size_t bytes, const Key& key, Allocate allocate, Free free) {
    invalidate();
    if (!bytes || !allocate || !free) return 0;
    if (bytes != bytes_) release();
    if (!lsb_) {
      auto* lsb = static_cast<uint8_t*>(allocate(bytes));
      auto* msb = lsb ? static_cast<uint8_t*>(allocate(bytes)) : nullptr;
      if (!msb) {
        if (lsb) free(lsb);
        return 0;
      }
      lsb_ = lsb;
      msb_ = msb;
      bytes_ = bytes;
      free_ = free;
    }
    memset(lsb_, 0, bytes_);
    memset(msb_, 0, bytes_);
    key_ = key;
    capturing_ = true;
    return generation_;
  }
  bool commit(uint32_t token) {
    if (!token || token != generation_ || !capturing_) return false;
    capturing_ = false;
    ready_ = true;
    return true;
  }
  bool matches(const Key& key) const { return ready_ && key_ == key; }
  void invalidate() {
    ready_ = capturing_ = false;
    if (++generation_ == 0) ++generation_;
  }
  void release() {
    invalidate();
    if (free_) {
      if (lsb_) free_(lsb_);
      if (msb_) free_(msb_);
    }
    lsb_ = msb_ = nullptr;
    free_ = nullptr;
    bytes_ = 0;
  }
  uint8_t* lsb() const { return lsb_; }
  uint8_t* msb() const { return msb_; }

 private:
  uint8_t* lsb_ = nullptr;
  uint8_t* msb_ = nullptr;
  Free free_ = nullptr;
  size_t bytes_ = 0;
  Key key_;
  uint32_t generation_ = 1;
  bool capturing_ = false, ready_ = false;
};
