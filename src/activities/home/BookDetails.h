#pragma once

#include <cstdint>
#include <string>

class BuildArena;

// What the book browser's Details view shows for a book: its title, author and series, as the
// book's own metadata (or its .opf sidecar) gives them rather than as the filename spells them.
//
// Getting them is cheap for a book that has been opened -- its book.bin starts with exactly these
// fields -- and costs a metadata-only OPF parse (~300 ms on the device) for one that has not. So a
// never-opened EPUB is parsed once and the result kept beside where its book.bin would be, as
// details.bin; after that every book on the card is a small file read away.
struct BookDetails {
  std::string title;
  std::string author;
  std::string series;
  std::string seriesIndex;
};

namespace BookDetailsCache {

// details.bin in a book's cache directory. Records the book's size, so a different book copied
// over the same name is not shown under the old one's title, and the stamp of its .opf sidecar
// (SidecarFiles::metadataStamp), so an edited sidecar is read again.
//
// Only a parse that succeeded is recorded. One that failed may have failed for want of memory, and
// recording it would show the book by its filename for good.
bool write(const std::string& path, uint32_t bookSize, uint32_t sidecarStamp, const BookDetails& details);
// False when the file is missing, from another format version, or recorded for a different book
// size or sidecar stamp. A bookSize of 0 means "not known" and skips that check.
bool read(const std::string& path, uint32_t bookSize, uint32_t sidecarStamp, BookDetails& out);

}  // namespace BookDetailsCache

namespace BookDetailsLookup {

// The answer without parsing anything: from details.bin, or -- for a TXT, Markdown or XTC book,
// which the view titles by filename -- from nothing at all. False only for an EPUB that needs
// parse() first.
bool cached(const std::string& bookPath, uint32_t bookSize, BookDetails& out);

// The slow path for an EPUB cached() could not answer: parse its OPF metadata and record the
// result in details.bin for next time. Always fills `out` -- with empty fields when parsing failed,
// which is then not recorded, so the next visit tries again. `scratch` holds the OPF inflate ring
// instead of the heap when given. True when the parse succeeded.
bool parse(const std::string& bookPath, uint32_t bookSize, BookDetails& out, BuildArena* scratch = nullptr);

}  // namespace BookDetailsLookup
