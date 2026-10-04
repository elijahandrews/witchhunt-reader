#pragma once

#include <FontManifestParser.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

enum class FontManifestStatus : uint8_t { Ok, Invalid, UnsupportedVersion };

// Reads the downloaded font manifest into a list of families: the part the Font Manager
// (FontDownloadActivity) and the web font API (CrossPointWebServer) share. Each keeps its own family
// type; both have name, description, files (name, size, crc32, hasCrc32) and totalSize.
//
// The file is streamed through FontManifestParser a block at a time; a whole-document parse of the
// 24 KB manifest aborted the X3 for want of heap. Two passes: the first checks the document and
// counts its families, so nothing is built for a manifest that is rejected and the list is reserved
// once; the second builds the list. Each family is checked when it closes, as the whole-document
// parse checked it: its name, then its file names in order. The first that fails is logged and the
// family left out.
template <typename Family>
class FontManifestReader {
 public:
  using File = typename decltype(Family::files)::value_type;
  using FamilyNameCheck = bool (*)(const char* name);
  using FileNameCheck = bool (*)(const std::string& name);

  FontManifestReader(const char* logTag, FamilyNameCheck familyNameOk, FileNameCheck fileNameOk)
      : logTag(logTag), familyNameOk(familyNameOk), fileNameOk(fileNameOk) {}

  FontManifestReader(const FontManifestReader&) = delete;
  FontManifestReader& operator=(const FontManifestReader&) = delete;

  // Reads `file` from its start. On Ok, `families` and `baseUrl` hold the manifest; otherwise both
  // are left empty.
  FontManifestStatus read(HalFile& file, std::vector<Family>& families, std::string& baseUrl) {
    families.clear();
    baseUrl.clear();
    failureReason = nullptr;
    manifestVersion = 0;

    // About 1 KB, freed on return: StreamingJsonParser's 512-byte token buffer, the base URL and
    // one file name. Too big for the stack beside the read block.
    const auto parser = makeUniqueNoThrow<FontManifestParser>(callbacks());
    if (!parser) {
      LOG_ERR(logTag, "OOM: font manifest parser (%u bytes)", static_cast<unsigned>(sizeof(FontManifestParser)));
      failureReason = "out of memory";
      return FontManifestStatus::Invalid;
    }

    // Pass 1: is this a manifest we read, and how many families does it list?
    counting = true;
    familyCount = 0;
    if (!feedFile(file, *parser)) return FontManifestStatus::Invalid;
    manifestVersion = parser->version();
    // v1 (legacy, no crc32) and v2 (with crc32) are both read; the crc check is skipped per file when
    // the field is absent. See upstream PR #1904 and scripts/generate-font-manifest.py.
    if (manifestVersion != 1 && manifestVersion != 2) return FontManifestStatus::UnsupportedVersion;

    // Pass 2: build the families, into a list reserved for all of them, as the whole-document parse
    // reserved families.size() -- the ones that fail their checks included.
    counting = false;
    out = &families;
    families.reserve(familyCount);
    parser->reset();
    const bool built = feedFile(file, *parser);
    out = nullptr;
    if (!built) {
      families.clear();
      return FontManifestStatus::Invalid;
    }
    baseUrl = parser->baseUrl();
    return FontManifestStatus::Ok;
  }

  // Why read() returned Invalid, for the log.
  const char* failure() const { return failureReason; }
  // The manifest's version, once read() has checked the document; 0 if it gives none.
  int version() const { return manifestVersion; }

 private:
  bool fail(const char* why) {
    failureReason = why;
    return false;
  }

  // The whole file, from its start. False, with the reason set, when it cannot be read or is not a
  // complete manifest document.
  bool feedFile(HalFile& file, FontManifestParser& parser) {
    if (!file.seekSet(0)) return fail("read error");
    char block[512];  // one SD sector per read
    for (;;) {
      const int n = file.read(block, sizeof(block));
      if (n < 0) return fail("read error");
      if (n == 0) break;
      parser.feed(block, static_cast<size_t>(n));
    }
    if (parser.hasError()) return fail("not valid JSON");
    if (!parser.complete()) return fail("incomplete document");
    // A cut base URL would send every download to the wrong place.
    if (parser.baseUrlOverflow()) return fail("baseUrl too long");
    return true;
  }

  FontManifestCallbacks callbacks() {
    FontManifestCallbacks cb{};
    cb.ctx = this;
    cb.onFamilyBegin = [](void* ctx) { static_cast<FontManifestReader*>(ctx)->familyBegin(); };
    cb.onFamilyName = [](void* ctx, const char* v, size_t n) {
      static_cast<FontManifestReader*>(ctx)->familyName(v, n);
    };
    cb.onFamilyDescription = [](void* ctx, const char* v, size_t n) {
      static_cast<FontManifestReader*>(ctx)->familyDescription(v, n);
    };
    cb.onFile = [](void* ctx, const FontManifestFile& f) { static_cast<FontManifestReader*>(ctx)->fileEntry(f); };
    cb.onFamilyEnd = [](void* ctx) { static_cast<FontManifestReader*>(ctx)->familyEnd(); };
    return cb;
  }

  void familyBegin() {
    if (counting) {
      ++familyCount;
      return;
    }
    current = Family{};
    badFile = false;
    badFileName.clear();
  }

  void familyName(const char* value, const size_t len) {
    if (!counting) current.name.assign(value, len);
  }

  void familyDescription(const char* value, const size_t len) {
    if (!counting) current.description.assign(value, len);
  }

  void fileEntry(const FontManifestFile& f) {
    // Past a bad file the family is left out whole; only the first is reported.
    if (counting || badFile) return;
    std::string name(f.name, f.nameLen);
    if (f.nameOverflow || !fileNameOk(name)) {
      badFile = true;
      badFileName = std::move(name);
      return;
    }
    File entry;
    entry.name = std::move(name);
    entry.size = f.size;
    if (f.hasCrc32) {
      entry.crc32 = f.crc32;
      entry.hasCrc32 = true;
    }
    current.totalSize += entry.size;
    current.files.push_back(std::move(entry));
  }

  void familyEnd() {
    if (counting) return;
    if (!familyNameOk(current.name.c_str())) {
      LOG_ERR(logTag, "Manifest entry rejected, invalid family name: %s", current.name.c_str());
      return;
    }
    if (badFile) {
      LOG_ERR(logTag, "Manifest entry rejected, invalid file name in %s: %s", current.name.c_str(),
              badFileName.c_str());
      return;
    }
    out->push_back(std::move(current));
  }

  const char* logTag;
  FamilyNameCheck familyNameOk;
  FileNameCheck fileNameOk;

  bool counting = false;
  size_t familyCount = 0;
  std::vector<Family>* out = nullptr;
  Family current;
  bool badFile = false;
  std::string badFileName;

  const char* failureReason = nullptr;
  int manifestVersion = 0;
};
