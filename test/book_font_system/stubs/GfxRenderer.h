#pragma once
#include <HalScalableFont.h>
#include <map>
struct EpdFontFamily {
  const EpdFont* styles[4];
  EpdFontFamily(const EpdFont* a, const EpdFont* b=nullptr, const EpdFont* c=nullptr, const EpdFont* d=nullptr):styles{a,b,c,d} {}
};
class GfxRenderer {
  std::map<int,EpdFontFamily> fonts_;
 public:
  void noteFontFailure() {}
  uint8_t prepareGenericFontFamilies() { return 3; }
  void insertFont(int id,EpdFontFamily family) { fonts_.insert_or_assign(id,family); }
  void removeFont(int id) { fonts_.erase(id); }
  const auto& getFontMap() const { return fonts_; }
  void setBookFontResolver(void*, int(*)(void*,uint32_t,uint16_t),uint64_t(*)(void*)=nullptr,
      uint64_t(*)(void*)=nullptr,uint32_t(*)(void*,uint64_t)=nullptr,
      bool(*)(void*,uint64_t,uint64_t,uint32_t)=nullptr,bool(*)(void*)=nullptr) {}
};
