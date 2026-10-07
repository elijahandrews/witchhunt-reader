#include "EpubFontManager.h"

#include <CpFontFormat.h>
#include <Epub.h>
#include <Epub/css/CssFontCatalog.h>
#include <Epub/css/CssParser.h>
#include <HalStorage.h>
#include <Logging.h>
#include <OutlineFontFace.h>
#include <ReaderMono.h>
#include <WordTypography.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <new>

#if defined(ESP32)
#include <esp_heap_caps.h>
#endif

namespace {
uint32_t hashBytes(uint32_t hash, const void* bytes, size_t count) {
  const auto* p = static_cast<const uint8_t*>(bytes);
  while (count--) hash = (hash ^ *p++) * 16777619u;
  return hash;
}
uint32_t hashText(uint32_t hash, const std::string& text) { return hashBytes(hash, text.c_str(), text.size() + 1); }
bool supportedFormat(const std::string& format) {
  return format.empty() || format == "truetype" || format == "opentype" || format == "truetype-variations" ||
         format == "opentype-variations";
}
int weightRank(uint16_t found, uint16_t wanted) {
  if (found == wanted) return 0;
  if (wanted == 400) {
    if (found == 500) return 1;
    return found < 400 ? 100 + 400 - found : 1000 + found - 500;
  }
  return found > wanted ? found - wanted : 1000 + wanted - found;
}
}  // namespace

struct EpubFontManager::Source {
  std::string path, cachePath;
  uint32_t bytes = 0, hash = 0;
  bool ready = false;
  const uint8_t* memory = nullptr;
};
struct EpubFontManager::Face {
  EpubFontManager* owner = nullptr;
  bool loaded = false, memoryFailed = false, ioFailed = false;
  Source* source = nullptr;
  uint16_t points64 = 0, weight = 400;
  bool italic = false;
  FsFile file;
  OutlineFontFace outline;
  EpdFontData data{};
  EpdFont font{&data};
  static size_t read(void* ctx, uint32_t offset, uint8_t* buffer, size_t bytes) {
    auto& face = *static_cast<Face*>(ctx);
    const int count = face.file.seekSet(offset) ? face.file.read(buffer, bytes) : 0;
    if (count < 0 || static_cast<size_t>(count) != bytes) {
      if (!face.ioFailed && face.loaded) face.owner->noteFailure();
      face.ioFailed = true;
    }
    return count > 0 ? static_cast<size_t>(count) : 0;
  }
};
struct EpubFontManager::Family {
  std::string name;
  uint16_t points64 = 0;
  int id = 0;
  uint64_t lastUse = 0;
  bool retry = false;
  Face* faces[4] = {};
};

EpubFontManager::EpubFontManager(Epub& epub, GfxRenderer& renderer) : epub_(epub), renderer_(renderer) {
#if defined(ESP32)
  available_ = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) != 0;
#else
  available_ = true;
#endif
  prepare();
  renderer_.setBookFontResolver(this, &resolveCallback,
                                [](void* ctx) { return static_cast<EpubFontManager*>(ctx)->failureEpoch(); });
}
EpubFontManager::~EpubFontManager() {
  renderer_.clearBookFontResolver(this);
  for (const auto& family : families_)
    if (family->id) renderer_.removeFont(family->id);
  families_.clear();
  faces_.clear();  // allocation bank callbacks still refer to this live manager
  sources_.clear();
  // Face closes FreeType before closing its source file (member destruction order).
}

size_t EpubFontManager::memoryUsed() const { return bankUsed_; }

void EpubFontManager::noteFailure() {
  degraded_ = true;
  if (failureEpoch_ != UINT32_MAX) ++failureEpoch_;
}

void EpubFontManager::touch(Family& family) {
  family.lastUse = ++useSequence_;
  if (recentIds_[0] == family.id) return;
  recentIds_[1] = recentIds_[0];
  recentIds_[0] = family.id;
}

void EpubFontManager::collectFaces() {
  const auto referenced = [&](const Face* face) {
    if (face == allocatingFace_) return true;
    if (buildingFamily_)
      for (const auto* used : buildingFamily_->faces)
        if (used == face) return true;
    for (const auto& family : families_)
      for (const auto* used : family->faces)
        if (used == face) return true;
    return false;
  };
  faces_.erase(std::remove_if(faces_.begin(), faces_.end(), [&](const auto& face) { return !referenced(face.get()); }),
               faces_.end());
}

bool EpubFontManager::evictOne() {
  size_t victim = families_.size();
  for (size_t i = 0; i < families_.size(); ++i) {
    const auto& family = *families_[i];
    if (family.id == recentIds_[0] || family.id == recentIds_[1]) continue;
    bool allocating = false;
    for (const auto* face : family.faces)
      if (allocatingFace_ && face == allocatingFace_) allocating = true;
    if (allocating) continue;
    if (victim == families_.size() || family.lastUse < families_[victim]->lastUse) victim = i;
  }
  if (victim == families_.size()) return false;
  renderer_.removeFont(families_[victim]->id);
  families_.erase(families_.begin() + victim);
  collectFaces();
  return true;
}

bool EpubFontManager::claim(size_t bytes, Face* face) {
  if (bytes > MEMORY_LIMIT) return false;
  Face* saved = allocatingFace_;
  allocatingFace_ = face;
  while (bytes > MEMORY_LIMIT - bankUsed_ && evictOne()) {
  }
  allocatingFace_ = saved;
  if (bytes > MEMORY_LIMIT - bankUsed_) return false;
  bankUsed_ += bytes;
  return true;
}

void EpubFontManager::prepare() {
  const auto* css = epub_.getCssParser();
  if (!css) return;
  const auto& catalog = css->fontCatalog();
  if (preparedRevision_ == catalog.faceRevision()) return;
  preparedRevision_ = catalog.faceRevision();
  uint32_t signature = 2166136261u;
  for (const auto& face : catalog.faces()) {
    signature = hashText(signature, face.family);
    signature = hashBytes(signature, &face.weight, sizeof(face.weight));
    signature = hashBytes(signature, &face.style, sizeof(face.style));
    for (const auto& src : face.sources) {
      signature = hashText(signature, src.path);
      signature = hashText(signature, src.format);
    }
  }
  // Clearing/reloading identical rules between chapters must not rerasterize
  // every live face. A same-count replacement still changes this signature.
  if (catalogSignature_ == signature) return;
  catalogSignature_ = signature;
  // A stylesheet recompile changes face order and invalidates registrations.
  for (const auto& family : families_)
    if (family->id) renderer_.removeFont(family->id);
  families_.clear();
  faces_.clear();
  sources_.clear();
  recentIds_[0] = recentIds_[1] = 0;
  degraded_ = false;
  fingerprintReady_ = false;
  for (const auto& face : catalog.faces())
    for (const auto& src : face.sources)
      if (supportedFormat(src.format)) source(src.path);
  refreshFingerprint();
  fingerprintReady_ = true;
}

void EpubFontManager::refreshFingerprint() {
  const auto* css = epub_.getCssParser();
  if (!css) return;
  const uint32_t previous = fingerprint_;
  fingerprint_ = hashBytes(2166136261u, &available_, sizeof(available_));
  for (const auto& face : css->fontCatalog().faces()) {
    fingerprint_ = hashText(fingerprint_, face.family);
    fingerprint_ = hashBytes(fingerprint_, &face.weight, sizeof(face.weight));
    fingerprint_ = hashBytes(fingerprint_, &face.style, sizeof(face.style));
    for (const auto& src : face.sources) {
      fingerprint_ = hashText(fingerprint_, src.path);
      fingerprint_ = hashText(fingerprint_, src.format);
      uint32_t hash = 0;
      for (const auto& item : sources_)
        if (item->path == src.path && item->ready) {
          hash = item->hash;
          break;
        }
      fingerprint_ = hashBytes(fingerprint_, &hash, sizeof(hash));
    }
  }
  // A source can recover after BuildParams captured its unavailable fingerprint.
  // Invalidate that in-flight layout even when the original failure preceded setup.
  if (fingerprintReady_ && fingerprint_ != previous) noteFailure();
}

EpubFontManager::Source* EpubFontManager::source(const std::string& path) {
  for (const auto& item : sources_)
    if (item->path == path) return item.get();
  if (!available_) return nullptr;
  size_t bytes = 0;
  // Missing, empty and unsupported oversized members are deterministic fallback.
  if (!epub_.getItemSize(path, &bytes) || !bytes || bytes > 8 * 1024 * 1024) return nullptr;
  if (sources_.size() >= 128) {
    noteFailure();
    return nullptr;
  }
  auto item = std::unique_ptr<Source>(new (std::nothrow) Source);
  if (!item) {
    noteFailure();
    return nullptr;
  }
  item->path = path;
  // The source ZIP fingerprint already invalidates this book's complete cache
  // when any member changes, including a same-length replacement font.
  const uint32_t nameHash = hashText(2166136261u, path);
  char suffix[40];
  snprintf(suffix, sizeof(suffix), "/embedded-font-%08x-%u.bin", static_cast<unsigned>(nameHash),
           static_cast<unsigned>(sources_.size()));
  item->cachePath = epub_.getCachePath() + suffix;
  uint32_t expectedCrc = 0;
  if (!epub_.getItemCrc32(path, &expectedCrc)) {
    noteFailure();
    return nullptr;
  }
  // Verify cached bytes against the authored ZIP payload, not just their size.
  // A bad/missing cache gets one fresh extraction, then the same streamed check.
  for (unsigned attempt = 0; attempt < 2 && !item->ready; ++attempt) {
    if (attempt && !epub_.extractItemToFile(path, item->cachePath)) break;
    FsFile file;
    if (!Storage.openFileForRead("EPFONT", item->cachePath, file) || file.fileSize() != bytes) continue;
    uint8_t buffer[512];
    size_t remaining = bytes;
    uint32_t hash = 2166136261u, crc = 0xffffffffu;
    while (remaining) {
      const size_t count = std::min(remaining, sizeof(buffer));
      if (file.read(buffer, count) != static_cast<int>(count)) break;
      hash = hashBytes(hash, buffer, count);
      crc = cpfont::crc32Update(crc, buffer, count);
      remaining -= count;
    }
    if (!remaining && (crc ^ 0xffffffffu) == expectedCrc) {
      item->hash = hash;
      item->bytes = static_cast<uint32_t>(bytes);
      item->ready = true;
    }
    file.close();
  }
  if (!item->ready) Storage.remove(item->cachePath.c_str());
  // Failed extraction/I/O may recover; do not memoize an unavailable source.
  if (!item->ready) {
    noteFailure();
    return nullptr;
  }
  sources_.push_back(std::move(item));
  refreshFingerprint();
  return sources_.back().get();
}

EpubFontManager::Source* EpubFontManager::monoSource(uint8_t style) {
  const std::string key = "@reader-mono:" + std::to_string(style);
  for (const auto& item : sources_)
    if (item->path == key) return item.get();
  auto item = std::unique_ptr<Source>(new (std::nothrow) Source);
  if (!item) {
    noteFailure();
    return nullptr;
  }
  size_t bytes = 0;
  item->memory = readerMono::data(style, &bytes);
  item->bytes = static_cast<uint32_t>(bytes);
  item->path = key;
  item->ready = true;
  sources_.push_back(std::move(item));
  return sources_.back().get();
}

EpubFontManager::Face* EpubFontManager::openFace(Source& src, uint16_t points64, uint16_t weight, bool italic) {
  for (const auto& face : faces_)
    if (face->source == &src && face->points64 == points64 && !face->memoryFailed && !face->ioFailed &&
        (!face->outline.hasWeightAxis() || face->weight == weight) &&
        (!face->outline.hasItalicAxis() || face->italic == italic))
      return face.get();
  if (!src.ready) return nullptr;
  collectFaces();
  while (faces_.size() >= MAX_FACES && evictOne()) {
  }
  if (faces_.size() >= MAX_FACES) {
    noteFailure();
    return nullptr;
  }
  auto face = std::unique_ptr<Face>(new (std::nothrow) Face);
  if (!face) {
    noteFailure();
    return nullptr;
  }
  face->owner = this;
  if (!src.memory && !Storage.openFileForRead("EPFONT", src.cachePath, face->file)) {
    noteFailure();
    return nullptr;
  }
  OutlineFontFace::Options options;
  options.opticalPoints = points64 / 64.0f;
  options.logicalPx = options.opticalPoints * 150.0f / 72.0f;
  options.weight = weight;
  options.italic = italic;
  options.memoryLimit = 1024 * 1024;
  options.allocationContext = face.get();
  options.claimMemory = [](void* ctx, size_t bytes) {
    auto* f = static_cast<Face*>(ctx);
    return f->owner->claim(bytes, f);
  };
  options.releaseMemory = [](void* ctx, size_t bytes) {
    auto* f = static_cast<Face*>(ctx);
    f->owner->bankUsed_ -= bytes;
  };
  options.memoryFailure = [](void* ctx) {
    auto* f = static_cast<Face*>(ctx);
    if (!f->memoryFailed && f->loaded) f->owner->noteFailure();
    f->memoryFailed = true;
  };
  bool opened = false;
  do {
    face->memoryFailed = false;
    opened = src.memory ? face->outline.openMemory(src.memory, src.bytes, options)
                        : face->outline.openStream(&Face::read, face.get(), src.bytes, options);
  } while (!opened && face->memoryFailed && evictOne());
  if (!opened) {
    if (face->memoryFailed || face->ioFailed) noteFailure();
    return nullptr;
  }
  const auto metrics = face->outline.lineMetrics();
  face->data.advanceY = static_cast<uint8_t>(metrics.advanceY);
  face->data.ascender = metrics.ascender;
  face->data.descender = metrics.descender;
  face->data.is2Bit = true;
  face->data.outline = &OutlineFontFace::callbacks();
  face->data.outlineCtx = &face->outline;
  face->source = &src;
  face->points64 = points64;
  face->weight = weight;
  face->italic = italic;
  face->loaded = true;
  faces_.push_back(std::move(face));
  return faces_.back().get();
}

EpubFontManager::Family* EpubFontManager::family(const std::string& name, uint16_t points64,
                                                 const CssFontCatalog& catalog) {
  for (size_t i = 0; i < families_.size(); ++i) {
    auto& cached = *families_[i];
    if (cached.name != name || cached.points64 != points64) continue;
    bool failed = cached.retry;
    for (const auto* face : cached.faces)
      if (face && (face->memoryFailed || face->ioFailed)) failed = true;
    if (!failed) {
      touch(cached);
      return &cached;
    }
    // A later glyph allocation may have failed after registration. Retry a
    // fresh engine on the next resolve instead of keeping a poisoned family.
    renderer_.removeFont(cached.id);
    families_.erase(families_.begin() + i);
    collectFaces();
    break;
  }
  while (families_.size() >= MAX_FAMILIES && evictOne()) {
  }
  if (families_.size() >= MAX_FAMILIES) {
    noteFailure();
    return nullptr;
  }
  auto result = std::unique_ptr<Family>(new (std::nothrow) Family);
  if (!result) {
    noteFailure();
    return nullptr;
  }
  result->name = name;
  result->points64 = points64;
  buildingFamily_ = result.get();
  const uint32_t startEpoch = failureEpoch_;
  const EpdFont* styles[4] = {};
  for (uint8_t style = 0; style < 4; ++style) {
    const bool italic = (style & 2) != 0;
    const uint16_t weight = (style & 1) ? 700 : 400;
    if (name.empty()) {
      auto* src = monoSource(style);
      auto* face = src ? openFace(*src, points64, weight, italic) : nullptr;
      if (face) {
        styles[style] = &face->font;
        result->faces[style] = face;
      }
      continue;
    }
    std::vector<size_t> candidates;
    for (size_t i = 0; i < catalog.faces().size(); ++i)
      if (catalog.faces()[i].family == name) candidates.push_back(i);
    const auto rank = [&](const CssFontCatalog::Face& face) {
      const bool faceItalic = face.style != CssFontCatalog::Style::Normal;
      const int styleRank = faceItalic == italic ? (face.style == CssFontCatalog::Style::Oblique ? 10000 : 0) : 20000;
      return styleRank + weightRank(face.weight, weight);
    };
    std::sort(candidates.begin(), candidates.end(), [&](size_t a, size_t b) {
      const int ra = rank(catalog.faces()[a]), rb = rank(catalog.faces()[b]);
      return ra == rb ? a > b : ra < rb;
    });
    for (size_t index : candidates) {
      const auto& candidate = catalog.faces()[index];
      for (const auto& entry : candidate.sources) {
        if (!supportedFormat(entry.format)) continue;
        auto* src = source(entry.path);
        if (!src) continue;
        auto* face = openFace(*src, points64, weight, italic);
        if (face) {
          styles[style] = &face->font;
          result->faces[style] = face;
          break;
        }
      }
      if (styles[style]) break;
    }
  }
  if (styles[0]) {
    uint32_t hash = hashText(fingerprint_, name);
    hash = hashBytes(hash, &points64, sizeof(points64));
    // A separate positive namespace; collisions never replace an existing font.
    const int id = static_cast<int>(0x60000000u | (hash & 0x0fffffffu));
    if (renderer_.getFontMap().count(id) == 0) {
      renderer_.insertScaledFont(id, EpdFontFamily(styles[0], styles[1], styles[2], styles[3]), 0.5f);
      renderer_.registerFontPointSize(id, points64 / 64.0f);
      result->id = id;
    }
  }
  result->retry = failureEpoch_ != startEpoch;
  buildingFamily_ = nullptr;
  if (!result->id) {
    collectFaces();
    return nullptr;
  }
  families_.push_back(std::move(result));
  touch(*families_.back());
  return families_.back().get();
}

uint32_t EpubFontManager::fingerprint() {
  prepare();
  return fingerprint_;
}
GfxRenderer::TextFont EpubFontManager::resolveCallback(void* ctx, int base, uint8_t family, float scale) {
  return static_cast<EpubFontManager*>(ctx)->resolve(base, family, scale);
}
GfxRenderer::TextFont EpubFontManager::resolve(int baseFont, uint8_t familyId, float scale) {
  if (familyId < CssFontCatalog::FIRST_NAMED_ID && familyId != wordTypography::Monospace)
    return renderer_.resolveGenericTextFont(baseFont, familyId);
  const auto* css = epub_.getCssParser();
  if (!css) return {baseFont, 1.0f};
  prepare();
  const auto& catalog = css->fontCatalog();
  const auto* stack = catalog.stack(familyId);
  if (!stack && familyId != wordTypography::Monospace) return {baseFont, 1.0f};
  const float points = renderer_.fontPointSize(baseFont);
  const float requested = points * scale;
  if (!std::isfinite(requested) || requested <= 0 || requested > 1023)
    return renderer_.resolveGenericTextFont(baseFont, stack ? stack->fallback : wordTypography::SansSerif);
  const uint16_t points64 = static_cast<uint16_t>(std::max<long>(1, std::lround(requested * 64)));
  if (familyId == wordTypography::Monospace) {
    auto* font = available_ ? family("", points64, catalog) : nullptr;
    if (font && font->id) return {font->id, points * 64.0f / points64};
    return renderer_.resolveGenericTextFont(baseFont, wordTypography::SansSerif);
  }
  for (const auto& name : stack->families) {
    if (name.generic != wordTypography::Reader) return resolve(baseFont, name.generic, scale);
    auto* font = available_ ? family(name.name, points64, catalog) : nullptr;
    if (font && font->id) return {font->id, points * 64.0f / points64};
  }
  return renderer_.resolveGenericTextFont(baseFont, stack->fallback);
}
