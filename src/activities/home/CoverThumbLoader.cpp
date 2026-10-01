#include "CoverThumbLoader.h"

#include <HalStorage.h>
#include <Logging.h>

#include <utility>

#include "components/UITheme.h"

CoverThumbLoader::~CoverThumbLoader() { reset(); }

std::string CoverThumbLoader::thumbPath(const std::string& bookPath) const {
  return UITheme::getCoverThumbPath(ReaderActivity::coverThumbPlaceholder(bookPath), width_, height_);
}

void CoverThumbLoader::begin(const std::string& bookPath, BuildArena* scratch) {
  reset();
  book_ = bookPath;
  scratch_ = scratch;
}

void CoverThumbLoader::reset() {
  // Sessions first: each may hold blocks of the scratch region and open files.
  jpeg_.reset();
  extract_.reset();
  png_.reset();
  pngFiles_.close();
  book_.clear();
  scratch_ = nullptr;
  phase_ = Phase::Start;
  afterSessionFailure_ = false;
  jpegSessionFailed_ = false;
}

CoverThumbLoader::Step CoverThumbLoader::finish(const Step result) {
  reset();
  return result;
}

CoverThumbLoader::Step CoverThumbLoader::step() {
  if (book_.empty()) return Step::Failed;

  switch (phase_) {
    case Phase::Jpeg: {
      const auto status = jpeg_->continueSteps(1);
      if (status == CoverThumbSession::Status::Running) return Step::Working;
      jpeg_.reset();
      if (status == CoverThumbSession::Status::Done) return finish(Step::Done);
      // The session removed its partial output. One more go as the one-shot conversion.
      LOG_ERR("CTL", "Sliced JPEG cover failed for %s - retrying one-shot", book_.c_str());
      jpegSessionFailed_ = true;
      phase_ = Phase::Start;
      return Step::Working;
    }

    case Phase::Extract: {
      const auto status = extract_->continueStep(4096);
      if (status == ReaderActivity::CoverExtractSession::Status::Running) return Step::Working;
      extract_.reset();
      // Done: cover.img is cached now, so the next attempt can start the PNG decode on it.
      if (status == ReaderActivity::CoverExtractSession::Status::Error) {
        LOG_ERR("CTL", "Cover extract failed for %s", book_.c_str());
        afterSessionFailure_ = true;
      }
      phase_ = Phase::Start;
      return Step::Working;
    }

    case Phase::Png: {
      constexpr uint32_t ROWS_PER_STEP = 6;
      const auto status = png_->continueRows(ROWS_PER_STEP);
      if (status == PngDecodeSession::Status::Running) return Step::Working;
      pngFiles_.close();
      png_.reset();
      if (status == PngDecodeSession::Status::Done) return finish(Step::Done);
      LOG_ERR("CTL", "PNG cover decode failed for %s", book_.c_str());
      Storage.remove(thumbPath(book_).c_str());  // the partial BMP
      return finish(Step::Failed);
    }

    case Phase::Start:
      break;
  }

  const std::string thumb = thumbPath(book_);
  if (ReaderActivity::isCoverThumbComplete(thumb, width_, height_)) return finish(Step::Done);

  const std::pair<int, int> size{width_, height_};
  std::unique_ptr<CoverThumbSession> sliced;
  const ThumbResult res =
      ReaderActivity::ensureCoverThumbs(book_, &size, 1, scratch_, jpegSessionFailed_ ? nullptr : &sliced);
  if (sliced) {
    jpeg_ = std::move(sliced);
    phase_ = Phase::Jpeg;
    return Step::Working;
  }
  if (res == ThumbResult::Ok) return finish(Step::Done);

  // Only a TRANSIENT failure may walk the session ladder. A structural absence is permanent --
  // re-extracting the same entry yields the same undecodable bytes (RecentBooksActivity has the
  // livelock this avoids on an EPUB whose "cover.png" is really an AVIF).
  if (res == ThumbResult::TransientFail && !afterSessionFailure_) {
    png_ = ReaderActivity::beginPngThumbSession(book_, width_, height_, pngFiles_, scratch_);
    if (png_) {
      phase_ = Phase::Png;
      return Step::Working;
    }
    extract_ = ReaderActivity::beginCoverExtractSession(book_, scratch_);
    if (extract_) {
      phase_ = Phase::Extract;
      return Step::Working;
    }
  }

  // Nothing more to try. A book with no extractable cover gets the placeholder, so no screen opens
  // it again to rediscover that -- but not one with a sidecar image, which is a real source that
  // only failed this time (a tight heap, say).
  const bool permanent = res == ThumbResult::StructurallyAbsent || !afterSessionFailure_;
  if (permanent && ReaderActivity::sidecarCoverPath(book_).empty() && ReaderActivity::writeCoverPlaceholderBmp(thumb)) {
    LOG_DBG("CTL", "No extractable cover for %s - wrote placeholder", book_.c_str());
    return finish(Step::Done);
  }
  return finish(Step::Failed);
}
