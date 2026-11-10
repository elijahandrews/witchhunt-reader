#pragma once
#include <HalStorage.h>
#include <cstdint>
#include <map>
#include <string>
#include <cstring>
#include <FontLoadError.h>
namespace freeink::font { struct FtFont { enum class HintingMode {Light}; struct RenderOptions { HintingMode hinting; }; }; }
struct EpdFontData { uint16_t points64 = 0; uint8_t style = 0; uint32_t source = 0; };
struct EpdFont { EpdFontData value; const EpdFontData* data = &value; };
class HalScalableFont {
  uint32_t identity_ = 0;
  bool live_ = false, variable_ = false;
  FontLoadError error_ = FontLoadError::None;
  std::map<uint32_t, EpdFont> sizes_;
 public:
  static constexpr size_t MaxPublisherFileBytes = 8*1024*1024;
  enum class FileMode {Publisher};
  inline static int live = 0, maximumLive = 100, opens = 0;
  inline static int failedStyle = -1;
  inline static size_t descriptors = 0;
  ~HalScalableFont() { if(live_) --live; descriptors -= sizes_.size(); }
  bool openMemory(const uint8_t* data, size_t bytes, const freeink::font::FtFont::RenderOptions&) {
    if(live >= maximumLive) { error_ = FontLoadError::Memory; return false; }
    if(!bytes || !data[0]) { error_ = FontLoadError::Unsupported; return false; }
    identity_ = data[0]; variable_ = bytes > 1 && data[1] == 1;
    ++live; ++opens; live_ = true; return true;
  }
  bool openFile(const char* path, size_t, const freeink::font::FtFont::RenderOptions& options, FileMode) {
    const auto& bytes = Storage.bytes(path); return openMemory(bytes.data(), bytes.size(), options);
  }
  bool configurePublisherStyle(uint8_t) { return true; }
  FontLoadError lastLoadError() const { return error_; }
  uint32_t fingerprint() const { return identity_; }
  static uint32_t renderingRevision() { return 12; }
  bool copyFamilyName(char* name, size_t capacity) { if(capacity < 8) return false; std::strcpy(name,"fixture"); return true; }
  void releaseSize64(uint16_t points) { for(unsigned style=0;style<4;++style) descriptors -= sizes_.erase(uint32_t(points)*4+style); }
  const EpdFont* atSize64(uint16_t points, uint8_t style) {
    if(style == failedStyle) return nullptr;
    if(!variable_) style = 0;
    const auto key = uint32_t(points)*4 + style;
    if (!sizes_.count(key)) ++descriptors;
    auto& font = sizes_[key]; font.value = {points,style,identity_}; return &font;
  }
};
