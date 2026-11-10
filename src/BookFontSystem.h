#pragma once

#if CROSSINK_SCALABLE_FONTS
#include <Epub/css/CssFontCatalog.h>
#include <HalScalableFont.h>

#include <array>
#include <memory>
#include <string>

class Epub;
class GfxRenderer;

// Native publisher ownership: bounded shared sources and borrowed size/style
// descriptors live until their registrations are removed under the render lock.
class BookFontSystem {
 public:
  void ensure(Epub& epub, GfxRenderer& renderer, bool enabled);
  void open(Epub& epub, GfxRenderer& renderer);
  void close();
  int resolve(uint32_t family, uint8_t points);
  // Positive registered ID, zero reader fallback, or exact genericFont sentinel.
  int resolve64(uint32_t family, uint16_t points64);
  bool hasLoadedFamily(uint32_t family) const;
  bool copyFamilyName(uint32_t family, char* name, size_t capacity);
  uint64_t requestedFaces() const { return requestedFaces_; }
  uint64_t loadedFaces() const;
  uint32_t faceSignature(uint64_t mask) const;
  bool restoreFaces(uint64_t mask, uint64_t loaded, uint32_t signature);
  uint32_t signature() const { return signature_; }
  uint32_t failureEpoch() const { return failureEpoch_; }
  bool hasTemporaryFailures() const {
    if (monoTemporary_ || catalogTemporary_) return true;
    for (const auto& state : states_)
      if (state.temporary) return true;
    return false;
  }
  // One explicit retry opportunity per foreground build/recovery, never per word.
  void retryUnavailableFonts();

 private:
  static constexpr size_t MaxFaces = CssFontCatalog::MAX_FACES;
  static constexpr size_t MaxSources = 32;
  static constexpr size_t MaxRegistrations = 32;
  struct FaceState {
    uint32_t fingerprint[4] = {};
    uint8_t unsupported = 0;
    uint8_t temporary = 0;
  };
  struct Source {
    std::string path;
    std::unique_ptr<HalScalableFont> font;
  };
  struct Registration {
    uint32_t family = 0;
    uint16_t points64 = 0;
    int id = 0;
    std::array<int8_t, 4> sources{{-1, -1, -1, -1}};
    uint32_t lastUse = 0;
    uint32_t retryGeneration = 0;
    bool retry = false;
  };
  std::array<FaceState, MaxFaces> states_{};
  std::array<Source, MaxSources> sources_{};
  std::array<Registration, MaxRegistrations> registrations_{};
  std::array<bool, MaxSources> buildingSources_{};
  int recentIds_[2] = {};
  GfxRenderer* renderer_ = nullptr;
  Epub* book_ = nullptr;
  const CssFontCatalog* catalog_ = nullptr;
  uint32_t signature_ = 0;
  uint32_t failureEpoch_ = 0;
  uint32_t retryGeneration_ = 1;
  uint32_t useSequence_ = 0;
  uint64_t requestedFaces_ = 0;
  uint8_t monoTemporary_ = 0;
  bool catalogTemporary_ = false;
  bool loadCatalog();
  void noteFailure();
  void touch(Registration& registration);
  void erase(Registration& registration);
  void collectSources();
  bool evictOne();
  int loadSource(const std::string& path, int monoStyle, bool& temporary);
  int resolveNamed(uint32_t family, uint16_t points64);
  int selectFace(uint32_t family, uint8_t style, size_t& descriptor);
  void recordFace(size_t index, uint8_t style, const HalScalableFont& font);
  static int fontId(uint32_t family, uint16_t points64);
};
extern BookFontSystem bookFontSystem;
#endif
