#pragma once
#include <cstddef>
namespace MemoryBudget {
inline bool canAllocateInternal(size_t, size_t, size_t) { return true; }
}
