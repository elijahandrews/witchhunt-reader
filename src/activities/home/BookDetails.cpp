#include "BookDetails.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <SidecarFiles.h>

namespace {
std::string detailsPath(const Epub& epub) { return epub.getCachePath() + "/details.bin"; }
}  // namespace

namespace BookDetailsLookup {

// details.bin only, never book.bin. Epub::loadForMetadata() reads book.bin when it is current but
// quietly falls back to the ~300 ms OPF parse when it is from an older cache version -- which after
// a firmware update is every book on the card -- and this runs while a page is being drawn.
bool cached(const std::string& bookPath, const uint32_t bookSize, BookDetails& out) {
  if (!FsHelpers::hasEpubExtension(bookPath)) {
    out = {};
    return true;
  }
  const Epub epub(bookPath, "/.crosspoint");
  const std::string path = detailsPath(epub);
  // Most misses are books never seen before: answer those without hashing the sidecar.
  if (!Storage.exists(path.c_str())) return false;
  return BookDetailsCache::read(path, bookSize, SidecarFiles::metadataStamp(bookPath), out);
}

bool parse(const std::string& bookPath, const uint32_t bookSize, BookDetails& out, BuildArena* scratch) {
  out = {};
  Epub epub(bookPath, "/.crosspoint");
  // Fast when the book has been opened (book.bin), an OPF parse when not; the .opf sidecar is
  // applied either way.
  if (!epub.loadForMetadata(scratch)) return false;
  out.title = epub.getTitle();
  out.author = epub.getAuthor();
  out.series = epub.getSeries();
  out.seriesIndex = epub.getSeriesIndex();
  epub.setupCacheDir();
  BookDetailsCache::write(detailsPath(epub), bookSize, SidecarFiles::metadataStamp(bookPath), out);
  return true;
}

}  // namespace BookDetailsLookup
