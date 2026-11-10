#include "EpubLinkReturnState.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>

namespace EpubLinkReturnState {
namespace {
constexpr size_t ENTRY_BYTES = 16;
constexpr size_t PREVIOUS_ENTRY_BYTES = 10;
constexpr size_t HEADER_BYTES = 4;
void write16(uint8_t* p, uint16_t value) {
  p[0] = value & 0xff;
  p[1] = value >> 8;
}
uint16_t read16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
void write32(uint8_t* p, uint32_t value) {
  write16(p, value);
  write16(p + 2, value >> 16);
}
uint32_t read32(const uint8_t* p) {
  return static_cast<uint32_t>(read16(p)) | (static_cast<uint32_t>(read16(p + 2)) << 16);
}
}  // namespace
std::optional<ReadingProgress> readingProgress(const Position (&positions)[MAX_DEPTH], const int depth,
                                               const bool transientPreview, const ReaderProgressSaveDebouncer& progress,
                                               const int lastSavedSpineIndex, const int lastSavedPageCount) {
  if (transientPreview && depth > 0) {
    const auto& origin = positions[resumeDepth(depth, true)];
    const uint32_t originKey =
        (static_cast<uint32_t>(origin.spineIndex) << 16) | static_cast<uint16_t>(origin.pageNumber);
    const int pageCount = progress.lastObservedPosition() == originKey
                              ? static_cast<int>(progress.lastObservedMetadata())
                              : (lastSavedSpineIndex == origin.spineIndex ? std::max(0, lastSavedPageCount) : 0);
    return ReadingProgress{origin.spineIndex, origin.pageNumber, origin.pageCount ? origin.pageCount : pageCount,
                           origin.visibleTextOffset, origin.sourceAnchor};
  }
  if (!progress.hasPending()) return std::nullopt;
  const uint32_t position = progress.lastObservedPosition();
  return ReadingProgress{static_cast<int>(position >> 16), static_cast<int>(position & 0xffffU),
                         static_cast<int>(progress.lastObservedMetadata())};
}

void push(Position (&positions)[MAX_DEPTH], int& depth, const Position origin) {
  if (depth == MAX_DEPTH) {
    for (int i = 1; i < MAX_DEPTH; ++i) positions[i - 1] = positions[i];
    --depth;
  }
  positions[depth++] = origin;
}

int resumeDepth(const int depth, const bool transientPreview) {
  return transientPreview && depth > 0 ? depth - 1 : depth;
}

bool save(const std::string& cachePath, const Position (&positions)[MAX_DEPTH], const int depth,
          const bool transientPreview) {
  const std::string path = cachePath + "/links.bin";
  const int savedDepth = resumeDepth(depth, transientPreview);
  if (savedDepth == 0) return !Storage.exists(path.c_str()) || Storage.remove(path.c_str());
  if (savedDepth < 0 || savedDepth > MAX_DEPTH) return false;

  uint8_t data[HEADER_BYTES + MAX_DEPTH * ENTRY_BYTES] = {'L', 'R', 3, static_cast<uint8_t>(savedDepth)};
  for (int i = 0; i < savedDepth; ++i) {
    const auto& pos = positions[i];
    if (pos.spineIndex < 0 || pos.spineIndex > UINT16_MAX || pos.pageNumber < 0 || pos.pageNumber > UINT16_MAX) {
      LOG_ERR("ERS", "Link return position out of range");
      return false;
    }
    uint8_t* p = data + HEADER_BYTES + i * ENTRY_BYTES;
    write16(p, pos.spineIndex);
    write16(p + 2, pos.pageNumber);
    write16(p + 4, pos.pageCount);
    write32(p + 6, pos.visibleTextOffset);
    write32(p + 10, pos.sourceAnchor.sourceOffset);
    write16(p + 14, pos.sourceAnchor.characterOffset);
  }
  FsFile file;
  if (!Storage.openFileForWrite("ERS", path, file)) return false;
  const size_t size = HEADER_BYTES + savedDepth * ENTRY_BYTES;
  bool saved = file.write(data, size) == size;
  if (saved) saved = file.sync();
  const bool closed = file.close();
  saved = saved && closed;
  if (!saved) {
    LOG_ERR("ERS", "Failed to write link return stack");
    if (!Storage.remove(path.c_str())) LOG_ERR("ERS", "Failed to remove incomplete link stack");
  }
  return saved;
}

bool load(const std::string& cachePath, Position (&positions)[MAX_DEPTH], int& depth, const int spineCount) {
  depth = 0;
  const std::string path = cachePath + "/links.bin";
  if (!Storage.exists(path.c_str())) return true;
  uint8_t data[HEADER_BYTES + MAX_DEPTH * ENTRY_BYTES];
  FsFile file;
  bool valid = false;
  int loadedDepth = 0;
  size_t headerBytes = 1;
  size_t entryBytes = 4;
  if (Storage.openFileForRead("ERS", path, file)) {
    const size_t size = file.size();
    if (size >= 5 && size <= sizeof(data) && file.read(data, size) == static_cast<int>(size)) {
      const bool versioned = data[0] == 'L' && data[1] == 'R' && (data[2] == 2 || data[2] == 3);
      headerBytes = versioned ? HEADER_BYTES : 1;
      entryBytes = versioned ? (data[2] == 3 ? ENTRY_BYTES : PREVIOUS_ENTRY_BYTES) : 4;
      loadedDepth = versioned ? data[3] : data[0];
      valid = loadedDepth >= 1 && loadedDepth <= MAX_DEPTH && size == headerBytes + loadedDepth * entryBytes;
      for (int i = 0; valid && i < loadedDepth; ++i) {
        const uint8_t* p = data + headerBytes + i * entryBytes;
        valid = read16(p) < spineCount;
      }
    }
    if (!file.close()) valid = false;
  }
  // Consume even malformed records, after closing the reader. A later clean
  // exit rewrites the current stack; a crash must not resurrect old entries.
  if (!Storage.remove(path.c_str())) {
    LOG_ERR("ERS", "Failed to consume link return stack");
    return false;
  }
  if (!valid) {
    LOG_ERR("ERS", "Ignoring invalid link return stack");
    return false;
  }
  for (int i = 0; i < loadedDepth; ++i) {
    const uint8_t* p = data + headerBytes + i * entryBytes;
    positions[i] = {read16(p), read16(p + 2)};
    if (entryBytes >= PREVIOUS_ENTRY_BYTES) {
      positions[i].pageCount = read16(p + 4);
      positions[i].visibleTextOffset = read32(p + 6);
      if (entryBytes == ENTRY_BYTES) positions[i].sourceAnchor = {read32(p + 10), read16(p + 14)};
    }
  }
  depth = loadedDepth;
  LOG_DBG("ERS", "Loaded link stack, depth %d", depth);
  return true;
}
}  // namespace EpubLinkReturnState
