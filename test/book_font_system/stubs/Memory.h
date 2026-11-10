#pragma once
#include <memory>
#include <new>
template<class T, class... Args> std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  return std::unique_ptr<T>(new(std::nothrow) T(std::forward<Args>(args)...));
}
