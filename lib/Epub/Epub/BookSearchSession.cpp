#include "BookSearchSession.h"

#include <Epub.h>

#include <algorithm>
#include <atomic>
#include <cstring>

BookSearchSession::BookSearchSession(std::shared_ptr<Epub> epub, std::string_view query)
    : epub_(std::move(epub)), zip_(epub_->getPath()), matcher_(query, match, this) {}

BookSearchSession::~BookSearchSession() { cancel(); }
int BookSearchSession::sectionCount() const { return epub_->getSpineItemsCount(); }
bool BookSearchSession::begin() {
  if (ownsResults_) return false;
  if (!validQuery() || sectionCount() <= 0) {
    failed_ = done_ = true;
    return false;
  }
  // Each live session owns exactly one file. Exclusive creation also protects a
  // surviving file after reboot or a counter wrap; never truncate another scan.
  static std::atomic<uint32_t> nextId{0};
  for (unsigned attempt = 0; attempt < 32; ++attempt) {
    resultsPath_ = epub_->getCachePath() + "/search-" + std::to_string(nextId.fetch_add(1)) + ".tmp";
    output_ = Storage.open(resultsPath_.c_str(), O_WRONLY | O_CREAT | O_EXCL);
    if (output_) {
      ownsResults_ = true;
      return true;
    }
    if (!Storage.exists(resultsPath_.c_str())) break;
  }
  failed_ = done_ = true;
  return false;
}
void BookSearchSession::complete() {
  entry_.reset();
  if (output_) {
    if (!output_.sync()) failed_ = true;
    if (!output_.close()) failed_ = true;
  }
  done_ = true;
  if (count_ && !Storage.openFileForRead("SEARCH", resultsPath_.c_str(), input_)) failed_ = true;
}
void BookSearchSession::cancel() {
  entry_.reset();
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
  if (!entry_) {
    if (spine_ >= sectionCount()) {
      complete();
      return;
    }
    const auto item = epub_->getSpineItem(spine_);
    entry_ = zip_.openFileStream(item.href.c_str(), 256);
    if (!entry_) {
      ++failedSections_;
      ++spine_;
      return;
    }
    if (!matcher_.beginChapter()) {
      failed_ = true;
      complete();
      return;
    }
  }
  class Sink final : public Print {
   public:
    explicit Sink(BookTextSearch& matcher) : matcher_(matcher) {}
    size_t write(uint8_t value) override { return write(&value, 1); }
    size_t write(const uint8_t* bytes, size_t length) override { return matcher_.feed(bytes, length) ? length : 0; }

   private:
    BookTextSearch& matcher_;
  } sink(matcher_);
  constexpr size_t inputChunkBytes = 256;
  constexpr uint32_t matchesPerTurn = 64;
  const uint32_t initialCount = count_;
  size_t remaining = std::max<size_t>(1, byteBudget);
  do {
    const size_t before = entry_->produced();
    const auto status = entry_->pump(sink, std::min(remaining, inputChunkBytes));
    const size_t produced = entry_->produced() - before;
    if (limited_ || failed_) {
      complete();
      return;
    }
    if (status != ZipStreamStatus::More || produced == 0) {
      const bool parsed = matcher_.finishChapter();
      if (status == ZipStreamStatus::Error || !parsed || (status == ZipStreamStatus::More && produced == 0)) {
        ++failedSections_;
      }
      entry_.reset();
      ++spine_;
      if (failed_ || limited_ || spine_ >= sectionCount()) complete();
      return;
    }
    remaining -= std::min(remaining, produced);
    // Match callbacks write SD records. Check between bounded input chunks so
    // highly repetitive text cannot spend a whole turn writing thousands.
  } while (remaining > 0 && count_ - initialCount < matchesPerTurn);
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
