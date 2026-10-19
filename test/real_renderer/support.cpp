#include <FontCacheManager.h>
#include <FontDecompressor.h>

#include <cstdlib>
FontCacheManager::FontCacheManager(const std::map<int, EpdFontFamily>& a, const std::map<int, SdCardFont*>& b,
                                   const std::map<int, SdCardFont*>& c)
    : fontMap_(a), sdCardFonts_(b), sdCardFontAliases_(c) {}
void FontCacheManager::setFontDecompressor(FontDecompressor* d) { fontDecompressor_ = d; }
namespace rendererTest {
bool scanning = false;
int recorded = 0;
void setScanning(bool value) {
  scanning = value;
  recorded = 0;
}
int recordedCount() { return recorded; }
}  // namespace rendererTest
bool FontCacheManager::isScanning() const { return rendererTest::scanning; }
void FontCacheManager::recordText(const char* text, int, EpdFontFamily::Style) {
  if (!text) std::abort();  // Production recordText requires a non-null string.
  ++rendererTest::recorded;
}
void FontCacheManager::clearCache() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
}
