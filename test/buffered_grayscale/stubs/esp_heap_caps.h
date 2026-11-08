#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
constexpr uint32_t MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
namespace fakepsram {
inline size_t freeBytes = 8 * 1024 * 1024, largest = 8 * 1024 * 1024, allocated = 0;
inline int attempts = 0;
inline bool fail = false;
inline uint32_t lastCaps = 0;
inline void reset() {
  freeBytes = largest = 8 * 1024 * 1024;
  attempts = 0;
  fail = false;
  lastCaps = 0;
}
}  // namespace fakepsram
inline size_t heap_caps_get_free_size(uint32_t caps) {
  fakepsram::lastCaps = caps;
  return fakepsram::freeBytes;
}
inline size_t heap_caps_get_largest_free_block(uint32_t) { return fakepsram::largest; }
inline void* heap_caps_malloc(size_t bytes, uint32_t caps) {
  ++fakepsram::attempts;
  fakepsram::lastCaps = caps;
  if (fakepsram::fail) return nullptr;
  auto* p = std::malloc(bytes);
  if (p) ++fakepsram::allocated;
  return p;
}
inline void heap_caps_free(void* p) {
  if (p) {
    --fakepsram::allocated;
    std::free(p);
  }
}
