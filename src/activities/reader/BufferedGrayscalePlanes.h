#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
#include <esp_heap_caps.h>
#endif

// CrossInk's DeferredGrayscale keeps both AA planes in reusable PSRAM while
// the base waveform runs. One contiguous allocation holds our two planes:
// 96 KB at 800x480, too large for a task stack or scarce internal RAM.
// No internal-RAM fallback; C3 and low-memory readers keep the existing path.
class BufferedGrayscalePlanes {
  struct Free {
    void operator()(uint8_t* p) const {
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
      heap_caps_free(p);
#else
      (void)p;
#endif
    }
  };
  std::unique_ptr<uint8_t[], Free> storage_;
  size_t planeBytes_ = 0;

 public:
  bool ensure(size_t planeBytes) {
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
    constexpr size_t reserve = 128 * 1024;
    if (!planeBytes || planeBytes > (std::numeric_limits<size_t>::max() - reserve) / 2) return false;
    if (storage_ && planeBytes_ == planeBytes) return true;
    reset();
    const size_t bytes = 2 * planeBytes;
    constexpr uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    if (heap_caps_get_free_size(caps) < bytes + reserve || heap_caps_get_largest_free_block(caps) < bytes) return false;
    storage_.reset(static_cast<uint8_t*>(heap_caps_malloc(bytes, caps)));
    if (!storage_) return false;
    planeBytes_ = planeBytes;
    return true;
#else
    (void)planeBytes;
    return false;
#endif
  }
  void reset() {
    storage_.reset();
    planeBytes_ = 0;
  }
  uint8_t* lsb() const { return storage_.get(); }
  uint8_t* msb() const { return storage_ ? storage_.get() + planeBytes_ : nullptr; }
};
