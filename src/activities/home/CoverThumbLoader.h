#pragma once

#include <PngToBmpConverter.h>

#include <memory>
#include <string>

#include "Epub/CoverThumbSession.h"
#include "activities/reader/ReaderActivity.h"

class BuildArena;

// Makes one book's cover thumbnail at one size, a small unit of work per step().
//
// The ladder is the one the Home carousel and the Recent Books grid already walk, through the same
// ReaderActivity entry points; this is it as a reusable piece rather than a third copy inlined into
// a screen's loop:
//   1. a thumbnail already complete on the card -- done;
//   2. ensureCoverThumbs(), which converts a sidecar image, an XTC or a TXT cover in one go, or for
//      an embedded JPEG hands back a sliced decode (Home's CoverThumbSession);
//   3. on a transient failure, a sliced PNG decode, or first a sliced ZIP extraction of a large
//      embedded PNG and then the decode;
//   4. a book with no cover at all gets the 1x1 placeholder BMP, so it is never re-opened to find
//      that out again -- unless a sidecar image exists, which is a real source worth a retry.
//
// The caller decides how long to keep stepping: a time budget, input waiting, a frame being drawn.
// Every session is resumable, so stopping between steps loses nothing.
//
// `scratch`, when given, holds the inflate rings and decoder buffers instead of the heap -- the
// screen's borrowed secondary framebuffer. reset() before that region goes back to the display.
class CoverThumbLoader {
 public:
  enum class Step : uint8_t {
    Working,  // more to do; call step() again
    Done,     // the thumbnail (or the no-cover placeholder) is complete on the card
    Failed,   // no thumbnail this time; worth another try another day
  };

  CoverThumbLoader(int width, int height) : width_(width), height_(height) {}
  ~CoverThumbLoader();
  CoverThumbLoader(const CoverThumbLoader&) = delete;
  CoverThumbLoader& operator=(const CoverThumbLoader&) = delete;

  // Where the thumbnail of `bookPath` lives at this loader's size.
  std::string thumbPath(const std::string& bookPath) const;

  // Start on `bookPath`, abandoning whatever book was in hand.
  void begin(const std::string& bookPath, BuildArena* scratch);
  Step step();
  // Abandon the book in hand and close every session and file it had open.
  void reset();

  const std::string& book() const { return book_; }
  bool busy() const { return !book_.empty(); }

 private:
  enum class Phase : uint8_t { Start, Jpeg, Extract, Png };

  Step finish(Step result);

  int width_;
  int height_;
  std::string book_;
  BuildArena* scratch_ = nullptr;
  Phase phase_ = Phase::Start;
  // A sliced attempt failed for this book: the next one-shot attempt may not start another
  // session, and its own failure is then final for now (as RecentBooksActivity::loadNextCover).
  bool afterSessionFailure_ = false;
  // The sliced JPEG decode failed: retry once as the one-shot conversion (as HomeActivity does).
  bool jpegSessionFailed_ = false;
  std::unique_ptr<CoverThumbSession> jpeg_;
  std::unique_ptr<ReaderActivity::CoverExtractSession> extract_;
  std::unique_ptr<PngDecodeSession> png_;
  ReaderActivity::PngThumbFiles pngFiles_;
};
