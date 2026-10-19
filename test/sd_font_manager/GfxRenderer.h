#pragma once
// Registration-only renderer double: the actual manager and SD font loader are compiled.
#include <EpdFontFamily.h>
#include <FontLookup.h>

#include <map>
class SdCardFont;
class GfxRenderer {
 public:
  std::map<int, float> pointSizes;
  void registerFontPointSize(int id, float pt) { pointSizes[id] = pt; }
  FontResolverHook<EpdFontFamily> hook;
  void setFontResolver(decltype(hook.resolve) cb, void* ctx) { hook = {cb, ctx}; }
  void insertFont(int id, const EpdFontFamily& f) { fonts.insert_or_assign(id, f); }
  const EpdFontFamily* findFontFamily(int id) { return findOrResolveFont(fonts, scales, id, hook); }
  std::map<int, EpdFontFamily> fonts;
  std::map<int, float> scales;
  std::map<int, SdCardFont*> native, aliases;
  const auto& getFontMap() const { return fonts; }
  void registerSdCardFont(int id, SdCardFont* f) { native[id] = f; }
  void registerSdCardFontAlias(int id, SdCardFont* f) { aliases[id] = f; }
  void unregisterSdCardFont(int id) {
    native.erase(id);
    aliases.erase(id);
  }
  void clearSdCardFonts() {
    native.clear();
    aliases.clear();
  }
  void insertScaledFont(int id, const EpdFontFamily& f, float scale) {
    fonts.insert_or_assign(id, f);
    scales[id] = scale;
  }
  void removeFont(int id) {
    fonts.erase(id);
    scales.erase(id);
  }
  float fontBaseScale(int id) const { return scales.at(id); }
};
