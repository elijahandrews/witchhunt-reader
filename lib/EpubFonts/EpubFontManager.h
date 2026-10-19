#pragma once

#include <GfxRenderer.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class Epub;
class CssFontCatalog;

// Owns publisher faces for one open book. Register only while this book owns
// the renderer; destroy after its foreground/background page work has stopped.
class EpubFontManager {
 public:
  EpubFontManager(Epub& epub, GfxRenderer& renderer);
  ~EpubFontManager();
  EpubFontManager(const EpubFontManager&) = delete;
  EpubFontManager& operator=(const EpubFontManager&) = delete;
  uint32_t fingerprint();
  bool degraded() const { return degraded_; }
  uint32_t failureEpoch() const { return failureEpoch_; }
  size_t memoryUsed() const;
  GfxRenderer::TextFont resolve(int baseFont, uint8_t family, float scale);

 private:
  struct Source;
  struct Face;
  struct Family;
  Epub& epub_;
  GfxRenderer& renderer_;
  std::vector<std::unique_ptr<Source>> sources_;
  std::vector<std::unique_ptr<Face>> faces_;
  std::vector<std::unique_ptr<Family>> families_;
  uint32_t fingerprint_ = 0;
  uint32_t preparedRevision_ = 0;
  uint32_t catalogSignature_ = 0;
  bool available_ = false;
  bool fingerprintReady_ = false;
  bool degraded_ = false;
  uint32_t failureEpoch_ = 0;
  size_t bankUsed_ = 0;
  uint64_t useSequence_ = 0;
  int recentIds_[2] = {};
  Family* buildingFamily_ = nullptr;
  Face* allocatingFace_ = nullptr;
  static constexpr size_t MEMORY_LIMIT = 3 * 1024 * 1024;
  static constexpr size_t MAX_FACES = 32;
  static constexpr size_t MAX_FAMILIES = 32;
  void prepare();
  void refreshFingerprint();
  bool evictOne();
  void collectFaces();
  void touch(Family& family);
  bool claim(size_t bytes, Face* face);
  void noteFailure();
  Source* source(const std::string& path);
  Source* monoSource(uint8_t style);
  Face* openFace(Source& source, uint16_t points64, uint16_t weight, bool italic);
  Family* family(const std::string& name, uint16_t points64, const CssFontCatalog& catalog);
  static GfxRenderer::TextFont resolveCallback(void* ctx, int baseFont, uint8_t family, float scale);
};
