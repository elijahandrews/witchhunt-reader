#include <FontCacheManager.h>
#include <FontDecompressor.h>
FontCacheManager::FontCacheManager(const std::map<int, EpdFontFamily> &a,
                                   const std::map<int, SdCardFont *> &b,
                                   const std::map<int, SdCardFont *> &c)
    : fontMap_(a), sdCardFonts_(b), sdCardFontAliases_(c) {}
void FontCacheManager::setFontDecompressor(FontDecompressor *d) {
  fontDecompressor_ = d;
}
bool FontCacheManager::isScanning() const { return false; }
void FontCacheManager::recordText(const char *, int, EpdFontFamily::Style) {}
void FontCacheManager::clearCache() {
  if (fontDecompressor_)
    fontDecompressor_->clearCache();
}
