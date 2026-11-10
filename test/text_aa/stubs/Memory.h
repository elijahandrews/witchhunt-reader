#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
using HeapByteBuffer = std::unique_ptr<uint8_t[]>;
enum class MemoryPool { Psram };
struct Snapshot {
  size_t free, largest;
};
inline bool psramAvailable = true;
inline bool allocationFails = false;
inline Snapshot heapSnapshot{8 * 1024 * 1024, 8 * 1024 * 1024};
inline unsigned allocations = 0;
inline unsigned failAllocation = 0;
inline bool psramHeapAvailable() { return psramAvailable; }
inline Snapshot byteHeapSnapshot(MemoryPool) { return heapSnapshot; }
inline HeapByteBuffer makePsramByteBufferNoThrow(size_t n) {
  ++allocations;
  return allocationFails || allocations == failAllocation ? nullptr : std::make_unique<uint8_t[]>(n);
}
