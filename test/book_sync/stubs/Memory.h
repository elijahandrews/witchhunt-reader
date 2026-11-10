#pragma once
#include <memory>
inline unsigned allocationCalls = 0, failAllocationAt = 0;
template <typename T, typename... A>
std::unique_ptr<T> makeUniqueNoThrow(A&&... a) {
  if (++allocationCalls == failAllocationAt) return nullptr;
  return std::make_unique<T>(std::forward<A>(a)...);
}
