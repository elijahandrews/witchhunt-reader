#pragma once

#include <cstdint>
#include <cstring>

// A digest of a completed local hash index, retained only through an explicitly
// armed software restart. POD deliberately has no constructor/member initializers:
// the HAL places it in RTC_NOINIT memory. Never infer trust from SD timestamps.
struct ContentHashCacheSeal {
  uint32_t magic;
  uint32_t restartArmed;
  uint32_t bytes;
  uint8_t digest[32];

  static constexpr uint32_t MAGIC = 0x42534335;
  static constexpr uint32_t MAX_BYTES = 512 + 10000 * 32;
  void invalidate() { magic = restartArmed = 0; }
  bool valid() const { return magic == MAGIC && bytes >= 512 && bytes <= MAX_BYTES && (bytes - 512) % 32 == 0; }
  void boot(bool softwareReset) {
    if (!softwareReset || restartArmed != MAGIC || !valid()) invalidate();
    restartArmed = 0;  // A later, unrelated reset cannot inherit this permission.
  }
  void set(const uint8_t* hash, uint32_t length) {
    invalidate();
    bytes = length;
    memcpy(digest, hash, sizeof(digest));
    magic = MAGIC;
  }
  void armRestart() { restartArmed = valid() ? MAGIC : 0; }
};
