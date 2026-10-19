#pragma once
// The real manager and TTF store run against bounded simulated NOR on the host.
#include "SimNorFlash.h"
class EspNorFlash final : public NorFlash {
  SimNorFlash flash_{8 * 1024 * 1024};

 public:
  bool begin() { return true; }
  uint32_t size() const override { return flash_.size(); }
  bool read(uint32_t p, void* d, uint32_t n) const override { return flash_.read(p, d, n); }
  bool program(uint32_t p, const void* d, uint32_t n) override { return flash_.program(p, d, n); }
  bool erase(uint32_t p, uint32_t n) override { return flash_.erase(p, n); }
  const uint8_t* map(uint32_t p, uint32_t n) override { return flash_.map(p, n); }
  void unmap(const uint8_t* p) override { flash_.unmap(p); }
  uint32_t freeMapPages() const override { return flash_.freeMapPages(); }
};
