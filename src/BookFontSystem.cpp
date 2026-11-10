#include "BookFontSystem.h"
#if CROSSINK_SCALABLE_FONTS
#include <Epub.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <ReaderMono.h>

#include <algorithm>
#include <cstring>

BookFontSystem bookFontSystem;
namespace {
uint32_t mix(uint32_t hash, uint32_t value) { return (hash ^ value) * 16777619u; }
bool supportedFormat(const std::string& format) {
  return format.empty() || format == "truetype" || format == "opentype" || format == "truetype-variations" ||
         format == "opentype-variations";
}
int weightRank(uint16_t found, uint16_t wanted) {
  if (found == wanted) return 0;
  if (wanted == 400) return found == 500 ? 1 : found < 400 ? 500 - found : 500 + found;
  return found > wanted ? found - wanted : 1000 + wanted - found;
}
}  // namespace
int BookFontSystem::fontId(uint32_t family, uint16_t points64) {
  return int(0x60000000u | (mix(mix(2166136261u, family), points64) & 0x0fffffffu));
}
void BookFontSystem::close() {
  if (renderer_) {
    renderer_->setBookFontResolver(nullptr, nullptr);
    for (auto& registration : registrations_)
      if (registration.id) renderer_->removeFont(registration.id);
  }
  for (auto& registration : registrations_) registration = Registration{};
  for (auto& source : sources_) source = Source{};
  for (auto& state : states_) state = FaceState{};
  buildingSources_.fill(false);
  recentIds_[0] = recentIds_[1] = 0;
  signature_ = failureEpoch_ = useSequence_ = 0;
  retryGeneration_ = 1;
  requestedFaces_ = 0;
  monoTemporary_ = 0;
  catalogTemporary_ = false;
  renderer_ = nullptr;
  book_ = nullptr;
  catalog_ = nullptr;
}
void BookFontSystem::ensure(Epub& epub, GfxRenderer& renderer, bool enabled) {
  if (!enabled) {
    close();
    return;
  }
  if (book_ != &epub) open(epub, renderer);
}
void BookFontSystem::open(Epub& epub, GfxRenderer& renderer) {
  close();
  renderer_ = &renderer;
  book_ = &epub;
  loadCatalog();
  renderer.setBookFontResolver(
      this,
      [](void* c, uint32_t family, uint16_t points) {
        return static_cast<BookFontSystem*>(c)->resolve64(family, points);
      },
      [](void* c) { return static_cast<BookFontSystem*>(c)->requestedFaces(); },
      [](void* c) { return static_cast<BookFontSystem*>(c)->loadedFaces(); },
      [](void* c, uint64_t mask) { return static_cast<BookFontSystem*>(c)->faceSignature(mask); },
      [](void* c, uint64_t mask, uint64_t loaded, uint32_t signature) {
        return static_cast<BookFontSystem*>(c)->restoreFaces(mask, loaded, signature);
      },
      [](void* c) { return static_cast<BookFontSystem*>(c)->hasTemporaryFailures(); });
}
bool BookFontSystem::loadCatalog() {
  auto* parser = book_ ? book_->getCssParser() : nullptr;
  const auto* candidate = parser ? &parser->fontCatalog() : nullptr;
  catalogTemporary_ = parser && !parser->fontCatalogReady();
  catalog_ = !catalogTemporary_ ? candidate : nullptr;
  signature_ = mix(mix(2166136261u, HalScalableFont::renderingRevision()), renderer_->prepareGenericFontFamilies());
  if (catalog_) signature_ = mix(signature_, catalog_->signature());
  if (catalogTemporary_) noteFailure();
  return !catalogTemporary_;
}
void BookFontSystem::noteFailure() {
  if (renderer_) renderer_->noteFontFailure();
  if (failureEpoch_ != UINT32_MAX) ++failureEpoch_;
}
void BookFontSystem::retryUnavailableFonts() {
  if (catalogTemporary_) loadCatalog();
  if (++retryGeneration_ == 0) retryGeneration_ = 1;
  for (auto& state : states_) state.temporary = 0;
  monoTemporary_ = 0;
}
void BookFontSystem::touch(Registration& registration) {
  registration.lastUse = ++useSequence_;
  if (recentIds_[0] != registration.id) {
    recentIds_[1] = recentIds_[0];
    recentIds_[0] = registration.id;
  }
}
void BookFontSystem::collectSources() {
  for (size_t i = 0; i < MaxSources; ++i) {
    if (buildingSources_[i]) continue;
    bool referenced = false;
    for (const auto& registration : registrations_)
      if (registration.id && std::find(registration.sources.begin(), registration.sources.end(), int8_t(i)) !=
                                 registration.sources.end()) {
        referenced = true;
        break;
      }
    if (!referenced) sources_[i] = Source{};
  }
}
void BookFontSystem::erase(Registration& registration) {
  renderer_->removeFont(registration.id);
  for (auto& id : recentIds_)
    if (id == registration.id) id = 0;
  const auto removed = registration;
  registration = Registration{};
  for (const auto source : removed.sources) {
    if (source < 0 || !sources_[source].font || buildingSources_[source]) continue;
    bool borrowed = false;
    for (const auto& other : registrations_)
      if (other.id && other.points64 == removed.points64 &&
          std::find(other.sources.begin(), other.sources.end(), source) != other.sources.end()) {
        borrowed = true;
        break;
      }
    if (!borrowed) sources_[source].font->releaseSize64(removed.points64);
  }
  collectSources();
}
bool BookFontSystem::evictOne() {
  Registration* victim = nullptr;
  for (auto& item : registrations_) {
    if (!item.id || item.id == recentIds_[0] || item.id == recentIds_[1]) continue;
    if (!victim || item.lastUse < victim->lastUse) victim = &item;
  }
  if (!victim) return false;
  erase(*victim);
  return true;
}
int BookFontSystem::loadSource(const std::string& path, int monoStyle, bool& temporary) {
  for (size_t i = 0; i < MaxSources; ++i)
    if (sources_[i].font && sources_[i].path == path) return int(i);
  size_t slot = MaxSources;
  do {
    for (size_t i = 0; i < MaxSources; ++i)
      if (!sources_[i].font) {
        slot = i;
        break;
      }
  } while (slot == MaxSources && evictOne());
  if (slot == MaxSources) {
    temporary = true;
    noteFailure();
    return -1;
  }
  size_t bytes = 0;
  const uint8_t* memory = nullptr;
  std::string cachePath;
  if (monoStyle >= 0) {
    memory = readerMono::data(uint8_t(monoStyle), &bytes);
  } else {
    const int present = book_->getFontItemSize(path, &bytes);
    if (present < 0) {
      temporary = true;
      noteFailure();
      return -1;
    }
    if (!present || !bytes || bytes > HalScalableFont::MaxPublisherFileBytes) return -1;
    cachePath = book_->getCachePath() + "/publisher-source-" + std::to_string(slot);
    HalFile file;
    if (!Storage.openFileForWrite("BKF", cachePath, file)) {
      temporary = true;
      noteFailure();
      return -1;
    }
    const bool extracted = book_->readFontContentsValidatedToStream(path, file, 1024) && file.size() == bytes;
    const bool closed = file.close();
    if (!extracted || !closed) {
      Storage.remove(cachePath.c_str());
      temporary = true;
      noteFailure();
      return -1;
    }
  }
  freeink::font::FtFont::RenderOptions options;
  options.hinting = freeink::font::FtFont::HintingMode::Light;
  for (;;) {
    // One fallible owner per admitted source. Font bytes, tables and descriptors
    // use the HAL's bounded PSRAM pools; no duplicate variable-style source file.
    auto font = makeUniqueNoThrow<HalScalableFont>();
    bool opened =
        font && (memory ? font->openMemory(memory, bytes, options)
                        : font->openFile(cachePath.c_str(), bytes, options, HalScalableFont::FileMode::Publisher));
    opened = opened && font->configurePublisherStyle(0);
    if (opened) {
      sources_[slot].path = path;
      sources_[slot].font = std::move(font);
      return int(slot);
    }
    const auto error = font ? font->lastLoadError() : FontLoadError::Memory;
    font.reset();
    if (error == FontLoadError::Memory && evictOne()) continue;
    temporary = error == FontLoadError::Memory || error == FontLoadError::Read;
    if (temporary) noteFailure();
    if (!cachePath.empty()) Storage.remove(cachePath.c_str());
    LOG_ERR("BKF", "Publisher source unavailable: %s", path.c_str());
    return -1;
  }
}
void BookFontSystem::recordFace(size_t index, uint8_t style, const HalScalableFont& font) {
  auto& previous = states_[index].fingerprint[style];
  const uint32_t current = font.fingerprint();
  if (previous && previous != current) noteFailure();
  previous = current ? current : 1;
}
int BookFontSystem::selectFace(uint32_t family, uint8_t style, size_t& descriptor) {
  descriptor = MaxFaces;
  if (family == genericFont::Monospace) {
    if (monoTemporary_ & (1u << style)) return -1;
    bool temporary = false;
    const int result = loadSource("@reader-mono:" + std::to_string(style), style, temporary);
    if (temporary) monoTemporary_ |= 1u << style;
    return result;
  }
  if (!catalog_) return -1;
  std::array<uint8_t, MaxFaces> candidates{};
  size_t count = 0;
  const auto& faces = catalog_->faces();
  for (size_t i = 0; i < std::min(faces.size(), MaxFaces); ++i)
    if (faces[i].family == family) candidates[count++] = uint8_t(i);
  const auto rank = [&](const CssFontCatalog::Face& face) {
    const bool italic = face.style != CssFontCatalog::Style::Normal;
    return (italic == bool(style & 2) ? face.style == CssFontCatalog::Style::Oblique ? 10000 : 0 : 20000) +
           weightRank(face.weight, style & 1 ? 700 : 400);
  };
  std::sort(candidates.begin(), candidates.begin() + count, [&](uint8_t a, uint8_t b) {
    const int ra = rank(faces[a]), rb = rank(faces[b]);
    return ra != rb ? ra < rb : faces[a].declarationOrder > faces[b].declarationOrder;
  });
  for (size_t n = 0; n < count; ++n) {
    const size_t index = candidates[n];
    requestedFaces_ |= uint64_t(1) << index;
    auto& state = states_[index];
    const auto& face = faces[index];
    for (size_t source = 0; source < std::min(face.sources.size(), size_t(8)); ++source) {
      const uint8_t bit = uint8_t(1u << source);
      if ((state.unsupported | state.temporary) & bit || !supportedFormat(face.sources[source].format)) continue;
      bool temporary = false;
      const int loaded = loadSource(face.sources[source].path, -1, temporary);
      if (loaded >= 0) {
        descriptor = index;
        return loaded;
      }
      if (temporary)
        state.temporary |= bit;
      else
        state.unsupported |= bit;
    }
  }
  return -1;
}
int BookFontSystem::resolveNamed(uint32_t family, uint16_t points64) {
  for (auto& item : registrations_)
    if (item.id && item.family == family && item.points64 == points64) {
      if (item.retry && item.retryGeneration != retryGeneration_) {
        erase(item);
        break;
      }
      touch(item);
      return item.id;
    }
  Registration* registration = nullptr;
  do {
    for (auto& item : registrations_)
      if (!item.id) {
        registration = &item;
        break;
      }
  } while (!registration && evictOne());
  if (!registration) {
    noteFailure();
    return 0;
  }
  const int id = fontId(family, points64);
  if (renderer_->getFontMap().count(id)) {
    LOG_ERR("BKF", "Publisher registration ID collision");
    return 0;
  }
  const uint32_t before = failureEpoch_;
  const EpdFont* styles[4] = {};
  Registration candidate;
  std::array<size_t, 4> descriptors{{MaxFaces, MaxFaces, MaxFaces, MaxFaces}};
  for (uint8_t style = 0; style < 4; ++style) {
    const int source = selectFace(family, style, descriptors[style]);
    if (source < 0) continue;
    buildingSources_[source] = true;
    candidate.sources[style] = int8_t(source);
    styles[style] = sources_[source].font->atSize64(points64, style);
    if (!styles[style]) {
      if (descriptors[style] < MaxFaces)
        states_[descriptors[style]].temporary = 0xff;
      else if (family == genericFont::Monospace)
        monoTemporary_ |= 1u << style;
      noteFailure();
      break;
    }
  }
  bool complete = styles[0] != nullptr;
  for (unsigned style = 0; style < 4; ++style)
    if (candidate.sources[style] >= 0 && !styles[style]) complete = false;
  if (complete) {
    renderer_->insertFont(id, EpdFontFamily(styles[0], styles[1], styles[2], styles[3]));
    candidate.family = family;
    candidate.points64 = points64;
    candidate.id = id;
    candidate.retry = failureEpoch_ != before || monoTemporary_;
    for (const auto& state : states_) candidate.retry = candidate.retry || state.temporary;
    for (uint8_t style = 0; style < 4; ++style)
      if (descriptors[style] < MaxFaces)
        recordFace(descriptors[style], style, *sources_[candidate.sources[style]].font);
    candidate.retryGeneration = retryGeneration_;
    *registration = candidate;
    touch(*registration);
  }
  if (!complete) {
    // An existing registration can keep a source alive after a partial size
    // attempt. Release those candidate descriptors unless a published family
    // still borrows this exact source and size.
    for (const auto source : candidate.sources) {
      if (source < 0 || !sources_[source].font) continue;
      bool borrowed = false;
      for (const auto& other : registrations_)
        if (other.id && other.points64 == points64 &&
            std::find(other.sources.begin(), other.sources.end(), source) != other.sources.end()) {
          borrowed = true;
          break;
        }
      if (!borrowed) sources_[source].font->releaseSize64(points64);
    }
  }
  buildingSources_.fill(false);
  collectSources();
  return complete ? id : 0;
}
int BookFontSystem::resolve(uint32_t family, uint8_t points) { return resolve64(family, uint16_t(points) * 64); }
int BookFontSystem::resolve64(uint32_t family, uint16_t points64) {
  if (!renderer_ || !family || !points64) return 0;
  if (family == genericFont::Serif || family == genericFont::SansSerif) return genericFont::resolverResult(family);
  if (family == genericFont::Monospace) return resolveNamed(family, points64);
  if (catalog_)
    if (const auto* stack = catalog_->stack(family)) {
      for (const auto& member : stack->families) {
        if (member.generic) return resolve64(member.generic, points64);
        if (const int id = resolveNamed(member.token, points64)) return id;
      }
      return genericFont::resolverResult(stack->fallback);
    }
  return resolveNamed(family, points64);
}
bool BookFontSystem::hasLoadedFamily(uint32_t family) const {
  const auto loaded = [&](uint32_t named) {
    for (const auto& item : registrations_)
      if (item.id && item.family == named) return true;
    return false;
  };
  if (catalog_)
    if (const auto* stack = catalog_->stack(family)) {
      for (const auto& member : stack->families) {
        if (member.generic) return false;
        if (loaded(member.token)) return true;
      }
      return false;
    }
  return loaded(family);
}
bool BookFontSystem::copyFamilyName(uint32_t family, char* name, size_t capacity) {
  if (!name || !capacity) return false;
  name[0] = '\0';
  const auto copy = [&](uint32_t named) {
    for (const auto& item : registrations_)
      if (item.id && item.family == named && item.sources[0] >= 0)
        return sources_[item.sources[0]].font->copyFamilyName(name, capacity);
    return false;
  };
  if (catalog_)
    if (const auto* stack = catalog_->stack(family)) {
      for (const auto& member : stack->families) {
        if (member.generic) return false;
        if (copy(member.token)) return true;
      }
      return false;
    }
  return copy(family);
}
uint64_t BookFontSystem::loadedFaces() const {
  uint64_t mask = 0;
  for (size_t i = 0; i < MaxFaces; ++i)
    for (const auto value : states_[i].fingerprint)
      if (value) {
        mask |= uint64_t(1) << i;
        break;
      }
  return mask;
}
uint32_t BookFontSystem::faceSignature(uint64_t mask) const {
  if (!mask) return 0;
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < MaxFaces; ++i)
    if (mask & (uint64_t(1) << i))
      for (const auto value : states_[i].fingerprint) hash = mix(hash, value);
  return hash;
}
bool BookFontSystem::restoreFaces(uint64_t mask, uint64_t loaded, uint32_t signature) {
  const size_t count = catalog_ ? catalog_->faces().size() : 0;
  const uint64_t valid = count >= MaxFaces ? UINT64_MAX : (uint64_t(1) << count) - 1;
  if ((mask & ~valid) || (loaded & ~mask)) return false;
  for (uint64_t group : {loaded, mask & ~loaded})
    for (size_t i = 0; i < count; ++i)
      if (group & (uint64_t(1) << i)) resolveNamed(catalog_->faces()[i].family, 12 * 64);
  return (loadedFaces() & mask) == loaded && faceSignature(mask) == signature;
}
#endif
