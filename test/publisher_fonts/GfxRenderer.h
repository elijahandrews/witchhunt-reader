#pragma once
#include <EpdFontFamily.h>
#include <WordTypography.h>

#include <map>
// Font registration/resolver spy. Font metrics, glyphs, shaping and rasterization
// are provided by the production manager and outline backend.
class GfxRenderer {
 public:
  struct TextFont {
    int fontId;
    float scale;
  };
  using Resolver = TextFont (*)(void*, int, uint8_t, float);
  std::map<int, EpdFontFamily> fonts;
  std::map<int, float> scales, points;
  void* resolverContext = nullptr;
  Resolver resolver = nullptr;
  uint32_t (*failureGetter)(void*) = nullptr;
  unsigned removals = 0;
  void setBookFontResolver(void* context, Resolver callback, uint32_t (*failure)(void*) = nullptr) {
    resolverContext = context;
    resolver = callback;
    failureGetter = failure;
  }
  void clearBookFontResolver(void* context) {
    if (context == resolverContext) {
      resolverContext = nullptr;
      resolver = nullptr;
      failureGetter = nullptr;
    }
  }
  const auto& getFontMap() const { return fonts; }
  void insertScaledFont(int id, const EpdFontFamily& family, float scale) {
    fonts.insert_or_assign(id, family);
    scales[id] = scale;
  }
  void removeFont(int id) {
    fonts.erase(id);
    scales.erase(id);
    points.erase(id);
    ++removals;
  }
  void registerFontPointSize(int id, float size) { points[id] = size; }
  float fontPointSize(int id) const {
    auto found = points.find(id);
    return found == points.end() ? 14.0f : found->second;
  }
  TextFont resolveGenericTextFont(int base, uint8_t family) const {
    return {family == wordTypography::SansSerif ? 2 : base, 1.0f};
  }
};
