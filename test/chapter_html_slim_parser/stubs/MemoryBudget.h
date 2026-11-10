#pragma once

#include <cstdint>

namespace MemoryBudget {
struct Snapshot {
  uint32_t freeHeap = UINT32_MAX;
  uint32_t maxAllocHeap = UINT32_MAX;
};
inline constexpr uint32_t EPUB_TEXT_LAYOUT_MIN_FREE = 0;
inline Snapshot snapshot() { return {}; }
inline bool textLayoutHeapAllowed = true;
inline bool hasHeapForEpubTextLayoutStart(Snapshot) { return textLayoutHeapAllowed; }
inline bool heapAllowed = true;
inline bool hasHeap(Snapshot, uint32_t, uint32_t) { return heapAllowed; }
inline bool shouldReleaseSdFontCachesForEpubInlineImage(Snapshot) { return false; }
inline bool imageAllowed = true;
inline bool hasHeapForEpubInlineImage(const char*, const char*) { return imageAllowed; }
inline bool hasHeapForOptimizerPxcImage(const char*, const char*) { return true; }
}  // namespace MemoryBudget
