#pragma once
#include <EpdFontFamily.h>

#include <cstdint>
#include <map>
class GfxRenderer {
  std::map<int, EpdFontFamily> fonts_;
  uint8_t mask_ = 0;

 public:
  unsigned failures = 0;
  void noteFontFailure() { ++failures; }
  void removeFont(int id) { fonts_.erase(id); }
  void insertFont(int id, EpdFontFamily font) { fonts_.insert_or_assign(id, font); }
  const auto& getFontMap() const { return fonts_; }
  uint8_t genericFontMask() const { return mask_; }
  void setMask(uint8_t mask) { mask_ = mask; }
  int getFontIdForSize64(int id, uint16_t points) const { return getFontIdForSize(id, (points + 32) / 64); }
  int getFontIdForSize(int id, uint8_t points) const {
    const auto it = fonts_.find(id);
    if (it == fonts_.end() || !points) return id;
    const auto* base = it->second.getData();
    for (const auto& [candidate, family] : fonts_)
      if (family.getData()->sizeFamily == base->sizeFamily && family.getData()->pointSize == points) return candidate;
    return id;
  }
};
