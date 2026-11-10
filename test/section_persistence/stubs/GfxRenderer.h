#pragma once

#include <cstdint>

class FontCacheManager;

class GfxRenderer {
 public:
  uint32_t failureEpoch = 0;
  bool pendingFontFailure = false;
  uint32_t fontFailureEpoch() const { return failureEpoch; }
  bool fontsNeedRetry() const { return pendingFontFailure; }
  uint64_t publisherUsage = 0;
  uint32_t publisherSignature = 0;
  mutable uint64_t restoredPublisherUsage = 0;
  uint64_t bookFontUsage() const { return publisherUsage; }
  uint64_t bookFontLoaded() const { return publisherUsage; }
  uint32_t bookFontSignature(uint64_t) const { return publisherSignature; }
  bool restoreBookFonts(uint64_t mask, uint64_t loaded, uint32_t signature) const {
    restoredPublisherUsage = mask;
    return loaded == mask && signature == publisherSignature;
  }
  FontCacheManager* getFontCacheManager() const { return nullptr; }
  bool isSdCardFont(int) const { return false; }
  bool releaseSdCardFontForLowMemory(int, bool = false) { return false; }
};
