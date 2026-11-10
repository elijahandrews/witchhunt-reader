#pragma once

#include <Epub.h>
#include <Epub/SourceAnchor.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>
#include <optional>
#include <string>

#include "ProgressRecord.h"
#include "WitchProgressImport.h"
#include "util/InPlaceFileWrite.h"

namespace EpubReaderUtils {

struct Progress {
  int spineIndex = 0;
  int pageNumber = 0;
  int pageCount = 0;
  bool hasPageCount = false;
  uint32_t visibleTextOffset = 0;
  bool hasVisibleTextOffset = false;
  bool importedFromWitch = false;
  uint16_t legacyParagraph = UINT16_MAX;
  uint32_t legacySourceOffset = UINT32_MAX;
  uint16_t legacySourceCharacter = 0;
  SourceAnchor sourceAnchor;
};

inline progress_record::Slot readProgressSlot(const char* moduleName, const std::string& path) {
  if (!Storage.exists(path.c_str())) return {};
  FsFile file;
  if (!Storage.openFileForRead(moduleName, path, file)) return {};
  uint8_t data[progress_record::MAX_RECORD_BYTES] = {};
  const size_t size = file.fileSize();
  const size_t wanted = std::min(size, sizeof(data));
  const int n = file.read(data, wanted);
  file.close();
  if (n < 0 || static_cast<size_t>(n) != wanted) return {};
  return progress_record::decode(data, size);
}

inline bool copyProgress(const progress_record::Slot& slot, Progress& progress) {
  if (slot.kind == progress_record::SlotKind::Invalid) return false;
  const auto& f = slot.fields;
  progress = {f.spineIndex,        f.pageNumber == UINT16_MAX ? 0 : f.pageNumber,
              f.pageCount,         f.hasPageCount,
              f.visibleTextOffset, f.hasVisibleTextOffset};
  progress.sourceAnchor = {f.sourceOffset, f.sourceCharacter};
  return true;
}

// Read one explicit file (used when importing a legacy snapshot).
inline bool readProgressFile(const char* moduleName, const std::string& path, Progress& progress) {
  return copyProgress(readProgressSlot(moduleName, path), progress);
}

inline bool loadProgressPath(const std::string& path, Progress& progress, const char* moduleName = "ERS") {
  const auto primary = readProgressSlot(moduleName, path);
  const auto backup = readProgressSlot(moduleName, path + ".bak");
  const auto newest = progress_record::newest(primary, backup);
  return newest != progress_record::NONE &&
         copyProgress(newest == progress_record::PRIMARY ? primary : backup, progress);
}

inline bool loadProgress(const Epub& epub, Progress& progress, const char* moduleName = "ERS") {
  const std::string path = epub.getCachePath() + "/progress.bin";
  if (loadProgressPath(path, progress, moduleName)) return true;
  // A damaged new-format save must not resurrect an older Witch position.
  if (Storage.exists(path.c_str()) || Storage.exists((path + ".bak").c_str())) return false;
  FsFile file;
  if (!Storage.openFileForRead(moduleName, epub.getWitchCachePath() + "/progress.bin", file)) return false;
  uint8_t data[witch_progress::MAX_BYTES] = {};
  const size_t size = file.fileSize();
  const int read = size <= sizeof(data) ? file.read(data, size) : -1;
  const bool closed = file.close();
  witch_progress::Position legacy;
  if (!closed || read < 0 || static_cast<size_t>(read) != size || !witch_progress::decode(data, size, legacy))
    return false;
  progress = {legacy.spine,
              legacy.page == UINT16_MAX ? 0 : legacy.page,
              legacy.pageCount,
              size >= 6,
              0,
              false,
              true,
              legacy.paragraph,
              legacy.sourceOffset,
              legacy.sourceCharacter};
  return true;
}

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
inline bool saveProgress(Epub& epub, int spineIndex, int pageNumber, int pageCount,
                         const std::optional<uint32_t> visibleTextOffset = std::nullopt, SourceAnchor sourceAnchor = {}) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  const std::string path = epub.getCachePath() + "/progress.bin";
  const std::string backupPath = path + ".bak";
  const auto primary = readProgressSlot("ERS", path);
  const auto backup = readProgressSlot("ERS", backupPath);
  progress_record::Fields fields{static_cast<uint16_t>(spineIndex), static_cast<uint16_t>(pageNumber),
                                 static_cast<uint16_t>(pageCount),  true,
                                 visibleTextOffset.value_or(0),     visibleTextOffset.has_value()};
  fields.sourceOffset = sourceAnchor.sourceOffset;
  fields.sourceCharacter = sourceAnchor.characterOffset;
  const auto newest = progress_record::newest(primary, backup);
  if (newest != progress_record::NONE &&
      progress_record::sameFields(newest == progress_record::PRIMARY ? primary.fields : backup.fields, fields))
    return true;
  uint8_t record[progress_record::MAX_RECORD_BYTES];
  const size_t recordSize = progress_record::encodeWithSource(fields, progress_record::nextSeq(primary, backup), record);
  const auto& target = progress_record::target(primary, backup) == progress_record::PRIMARY ? path : backupPath;
  return writeFileInPlace("ERS", target.c_str(), record, recordSize);
}

}  // namespace EpubReaderUtils
