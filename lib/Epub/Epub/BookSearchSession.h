#pragma once

#include <HalStorage.h>
#include <ZipFile.h>

#include <memory>
#include <string>

#include "BookTextSearch.h"

class Epub;

// Incremental full-book scan. Matches live in a disposable SD file, not an in-memory
// vector. Only the scanner buffers and a single result record are resident in RAM.
class BookSearchSession {
 public:
  static constexpr uint32_t MAX_MATCHES = 10000;
  struct Hit {
    uint16_t spine = 0;
    uint32_t sourceOffset = 0;
    char snippet[BookTextSearch::SNIPPET_BYTES + 1]{};
  };

  BookSearchSession(std::shared_ptr<Epub> epub, std::string_view query);
  ~BookSearchSession();
  bool begin();
  void step(size_t byteBudget = 4096);
  void cancel();
  bool readHit(uint32_t index, Hit& hit);
  bool done() const { return done_; }
  bool failed() const { return failed_; }
  bool limited() const { return limited_; }
  uint32_t count() const { return count_; }
  int failedSections() const { return failedSections_; }
  int sectionsScanned() const { return spine_; }
  int sectionCount() const;
  bool validQuery() const { return matcher_.validQuery(); }

 private:
  static constexpr size_t RECORD_BYTES = 6 + BookTextSearch::SNIPPET_BYTES + 1;
  std::shared_ptr<Epub> epub_;
  std::string resultsPath_;
  ZipFile zip_;
  ZipFile::EntryReader entry_;
  BookTextSearch matcher_;
  FsFile output_;
  FsFile input_;
  uint8_t chunk_[256]{};
  uint8_t record_[RECORD_BYTES]{};
  uint32_t count_ = 0;
  int spine_ = 0;
  int failedSections_ = 0;
  bool done_ = false;
  bool failed_ = false;
  bool limited_ = false;

  bool append(uint32_t source, const char* snippet);
  void complete();
  static bool match(void* context, uint32_t source, const char* snippet);
};
