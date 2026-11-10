#pragma once
#include <mutex>
using SemaphoreHandle_t = std::mutex*;
constexpr int portMAX_DELAY = 0;
inline SemaphoreHandle_t xSemaphoreCreateMutex() {
  static std::mutex mutex;
  return &mutex;
}
inline void xSemaphoreTake(SemaphoreHandle_t p, int) { p->lock(); }
inline void xSemaphoreGive(SemaphoreHandle_t p) { p->unlock(); }
