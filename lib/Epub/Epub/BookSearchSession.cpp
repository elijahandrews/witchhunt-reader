#include "BookSearchSession.h"

#include <algorithm>
#include <cstring>

#include "../Epub.h"

BookSearchSession::BookSearchSession(std::shared_ptr<Epub> epub, std::string_view query)
    : epub_(std::move(epub)),
      resultsPath_(epub_->getCachePath() + "/search-results.tmp"),
      zip_(epub_->getPath()),
      entry_(zip_),
      matcher_(query, match, this) {}

BookSearchSession::~BookSearchSession() { cancel(); }
int BookSearchSession::sectionCount() const { return epub_->getSpineItemsCount(); }
bool BookSearchSession::begin() {
  if (!validQuery() || sectionCount() <= 0 || !Storage.openFileForWrite("SEARCH", resultsPath_.c_str(), output_)) {
    failed_ = done_ = true;
    return false;
  }
  ownsResults_ = true;
  epub_->primeZip(zip_);
  return true;
}
void BookSearchSession::complete() {
  entry_.close();
  if (output_) output_.close();
  done_ = true;
  if (count_ && !Storage.openFileForRead("SEARCH", resultsPath_.c_str(), input_)) failed_ = true;
}
void BookSearchSession::cancel() {
  entry_.close();
  if (output_) output_.close();
  if (input_) input_.close();
  if (ownsResults_) {
    Storage.remove(resultsPath_.c_str());
    ownsResults_ = false;
  }
  done_ = true;
}
void BookSearchSession::step(size_t byteBudget) {
  if (done_) return;
  size_t consumed = 0;
  const uint32_t initialCount = count_;
  // One input chunk is the yield granularity; never hold an entire inflated chapter.
  do {
    if (!entry_.isOpen()) {
      if (spine_ >= sectionCount()) {
        complete();
        return;
      }
      ZipFile::FileStatSlim stat{};
      if (!epub_->getSpineItemStat(spine_, &stat) || !entry_.open(stat)) {
        ++failedSections_;
        ++spine_;
        return;  // Failed entries also yield, so a broken book remains cancellable.
      }
      epub_->adoptZipDetails(zip_);
      if (!matcher_.beginChapter()) {
        failed_ = true;
        complete();
        return;
      }
    }
    size_t produced = 0;
    bool end = false;
    bool ok = entry_.step(chunk_, sizeof(chunk_), &produced, &end);
    if (ok && produced) ok = matcher_.feed(chunk_, produced);
    consumed += produced;
    if (limited_ || failed_) {
      complete();
      return;
    }
    if (!ok || end) {
      // Finalize even a truncated entry so already found matches retain their snippets.
      const bool parsed = matcher_.finishChapter();
      if (!ok || !parsed) ++failedSections_;
      entry_.close();
      ++spine_;
      if (failed_ || limited_) complete();
      return;
    }
    if (!produced) {
      // A malformed ZIP must not spin forever without consuming input.
      ++failedSections_;
      entry_.close();
      ++spine_;
      return;
    }
  } while (consumed < byteBudget && count_ - initialCount < 64);
}
bool BookSearchSession::match(void* context, uint32_t source, const char* snippet) {
  return static_cast<BookSearchSession*>(context)->append(source, snippet);
}
bool BookSearchSession::append(uint32_t source, const char* snippet) {
  if (spine_ > UINT16_MAX) {
    failed_ = true;
    return false;
  }
  std::memset(record_, 0, sizeof(record_));
  record_[0] = static_cast<uint8_t>(spine_);
  record_[1] = static_cast<uint8_t>(spine_ >> 8);
  for (size_t i = 0; i < 4; ++i) record_[2 + i] = static_cast<uint8_t>(source >> (i * 8));
  std::memcpy(record_ + 6, snippet, std::min(std::strlen(snippet), BookTextSearch::SNIPPET_BYTES));
  if (output_.write(record_, sizeof(record_)) != sizeof(record_)) {
    failed_ = true;
    return false;
  }
  if (++count_ == MAX_MATCHES) {
    limited_ = true;
    return false;
  }
  return true;
}
bool BookSearchSession::readHit(uint32_t index, Hit& hit) {
  if (!done_ || index >= count_) return false;
  if (!input_ || !input_.seekSet(index * RECORD_BYTES) || input_.read(record_, sizeof(record_)) != sizeof(record_)) {
    failed_ = true;
    return false;
  }
  hit.spine = static_cast<uint16_t>(record_[0] | (record_[1] << 8));
  hit.sourceOffset = 0;
  for (size_t i = 0; i < 4; ++i) hit.sourceOffset |= static_cast<uint32_t>(record_[2 + i]) << (i * 8);
  std::memcpy(hit.snippet, record_ + 6, sizeof(hit.snippet));
  hit.snippet[BookTextSearch::SNIPPET_BYTES] = '\0';
  return hit.spine < sectionCount() && hit.sourceOffset != UINT32_MAX;
}
