#pragma once
inline bool locked = false;
struct RenderLock {
  RenderLock() { locked = true; }
  ~RenderLock() { locked = false; }
};
