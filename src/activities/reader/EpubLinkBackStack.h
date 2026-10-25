#pragma once

#include <cstddef>
#include <cstdint>

// The EPUB reader's link Back stack: where each followed link (footnote or not) was tapped, newest
// last, so Back returns there instead of closing the book. Kept beside progress.bin as
// linkstack.bin, little-endian:
//   version(1) depth(1), then depth x
//     [spine(2) page(2) pageCount(2) paragraph(2) source(4) character(2) rangeStart(4) rangeEnd(4)]
// Version 1 entries omit source/character/range and remain readable. UINT32_MAX means no source.
//
// It is persisted so a stack survives sleep and a reopen: the reader reopens on the page it was
// showing, which for a followed link is the destination, and without the stack Back on it closed
// the book. Entries keep the first source word/character on the caller's page, so Back returns
// precisely after a reopen or reflow even when one paragraph spans several pages. Legacy entries
// without source coordinates retain the exact saved page under the same layout, using their
// paragraph only if the chapter's page count changed. The reader shares this restoration path
// with deliberate-jump history. The optional original page interval is used only to recognize a
// backlink after reflow: the caller's marker and the page's first word can move to different pages.
//
// Ported from crosspoint-reader PR #3764 ("fix: reopen on the viewed page after following a link",
// suhsoohong / @suhsoohong): theirs is the idea of persisting the stack and the rule that a full
// stack drops its oldest entry. Their file holds spine and page only; this one also carries the
// paragraph anchor and page count, and is not consumed on open (see linkStackOnDisk_).
struct EpubLinkBackStack {
  struct Entry {
    int spineIndex = 0;
    int pageNumber = 0;
    int pageCount = 0;  // 0: unknown (recorded mid-build)
    uint16_t paragraphIndex = 0;
    bool hasParagraph = false;
    uint32_t sourceOffset = UINT32_MAX;
    uint16_t sourceCharacterOffset = 0;
    uint32_t sourceRangeStart = UINT32_MAX;
    uint32_t sourceRangeEnd = 0;
    bool hasSourceRange() const { return sourceRangeStart != UINT32_MAX && sourceRangeStart < sourceRangeEnd; }
  };

  static constexpr int kMaxDepth = 3;
  static constexpr uint8_t kVersion = 2;
  static constexpr size_t kEntrySize = 22;
  static constexpr size_t kMaxSize = 2 + kMaxDepth * kEntrySize;

  // `sessionDepth` counts the entries on top of the stack that were pushed in this reader session;
  // the ones below them were loaded from linkstack.bin, so it starts at 0 whatever was loaded. It
  // is what "the reader is on a detour right now" means to anything that is not Back -- the KOSync
  // push on sleep is skipped while it is non-zero -- and keeps that meaning exactly what it was
  // before the stack outlived the session: a link followed and not yet returned from, this session.
  // Only entries are persisted, never this count.

  // A full stack drops its OLDEST entry, so Back from the newest jump always returns to where that
  // jump was made. Refusing the push instead -- what the reader used to do -- sent Back from a
  // fourth jump to the origin of the third, skipping the page the reader had just left.
  static void push(Entry (&stack)[kMaxDepth], int& depth, int& sessionDepth, const Entry& origin) {
    if (depth >= kMaxDepth) {
      for (int i = 1; i < kMaxDepth; i++) stack[i - 1] = stack[i];
      depth = kMaxDepth - 1;
    }
    stack[depth++] = origin;
    // Session entries are always the newest, so the oldest one dropped above is a loaded one for
    // as long as any are left; once all are from this session the count simply stays at the depth.
    if (sessionDepth < depth) sessionDepth++;
  }

  // Removes the newest entry, which is a session one whenever any are left. The caller reads it at
  // stack[depth] afterwards.
  static void pop(int& depth, int& sessionDepth) {
    if (depth <= 0) return;
    depth--;
    if (sessionDepth > 0) sessionDepth--;
  }

  // A link that lands back where an earlier one was followed from -- a note's "Return", a backlink
  // to the marker the reader tapped -- is a return, not one more detour. Answers the depth the stack
  // goes back to: the newest entry on `spine` whose original source interval overlaps the
  // destination page's interval (or exact page when no interval was captured), which is dropped
  // with everything above it, as Back would have. -1 when there is none. The newest entry itself is
  // never matched: it was pushed for this very link, and is where it was tapped.
  //
  // Both ranges must be unambiguous page intervals from Section. If an origin has a range,
  // unavailable/nonoverlapping source evidence never falls back to a coincidentally equal page.
  //
  // Based on crosspoint-reader PR #3799 (Ruslan Sukharev, @skhrv), whose FootnoteHistory unwinds the
  // same way; theirs matches any page of the origin's spine when the link crosses spines.
  static int returnDepth(const Entry (&stack)[kMaxDepth], const int depth, const int spine, const int page,
                         const uint32_t sourceFirst = UINT32_MAX, const uint32_t sourceEnd = 0) {
    for (int i = depth - 2; i >= 0; i--) {
      if (stack[i].spineIndex != spine) continue;
      if (stack[i].hasSourceRange()) {
        if (sourceFirst != UINT32_MAX && sourceFirst < sourceEnd && stack[i].sourceRangeStart < sourceEnd &&
            sourceFirst < stack[i].sourceRangeEnd)
          return i;
      } else if (stack[i].pageNumber == page) {
        return i;
      }
    }
    return -1;
  }

  // Bytes written to `out`; 0 for an empty stack, which is stored as no file at all.
  static size_t encode(const Entry (&stack)[kMaxDepth], const int depth, uint8_t (&out)[kMaxSize]) {
    if (depth <= 0 || depth > kMaxDepth) return 0;
    out[0] = kVersion;
    out[1] = static_cast<uint8_t>(depth);
    for (int i = 0; i < depth; i++) {
      uint8_t* p = out + 2 + i * kEntrySize;
      put16(p, stack[i].spineIndex);
      put16(p + 2, stack[i].pageNumber);
      put16(p + 4, stack[i].pageCount);
      // 0 is the paragraph LUT's "none" (Section::getParagraphIndexForPage), never an anchor.
      put16(p + 6, stack[i].hasParagraph ? stack[i].paragraphIndex : 0);
      put16(p + 8, stack[i].sourceOffset & 0xFFFF);
      put16(p + 10, stack[i].sourceOffset >> 16);
      put16(p + 12, stack[i].sourceOffset == UINT32_MAX ? 0 : stack[i].sourceCharacterOffset);
      put32(p + 14, stack[i].hasSourceRange() ? stack[i].sourceRangeStart : UINT32_MAX);
      put32(p + 18, stack[i].hasSourceRange() ? stack[i].sourceRangeEnd : 0);
    }
    return 2 + depth * kEntrySize;
  }

  // The stored depth, with `stack` filled; 0 when the file is anything but a whole stack this book
  // can use -- another version, a size that is not exactly the depth's, or an entry naming a spine
  // the book no longer has. All or nothing: a stack missing an entry would send Back to the wrong
  // origin. `stack` is left untouched when the answer is 0.
  static int decode(const uint8_t* data, const size_t size, const int spineCount, Entry (&stack)[kMaxDepth]) {
    if (size < 2 || (data[0] != 1 && data[0] != kVersion)) return 0;
    const size_t entrySize = data[0] == 1 ? 8 : kEntrySize;
    const int depth = data[1];
    if (depth < 1 || depth > kMaxDepth || size != 2 + depth * entrySize) return 0;
    for (int i = 0; i < depth; i++) {
      if (get16(data + 2 + i * entrySize) >= spineCount) return 0;
    }
    for (int i = 0; i < depth; i++) {
      const uint8_t* p = data + 2 + i * entrySize;
      Entry e;
      e.spineIndex = get16(p);
      e.pageNumber = get16(p + 2);
      e.pageCount = get16(p + 4);
      e.paragraphIndex = get16(p + 6);
      e.hasParagraph = e.paragraphIndex != 0;
      if (data[0] == kVersion) {
        e.sourceOffset = get16(p + 8) | (static_cast<uint32_t>(get16(p + 10)) << 16);
        e.sourceCharacterOffset = e.sourceOffset == UINT32_MAX ? 0 : get16(p + 12);
        e.sourceRangeStart = get32(p + 14);
        e.sourceRangeEnd = get32(p + 18);
        if (!e.hasSourceRange()) {
          e.sourceRangeStart = UINT32_MAX;
          e.sourceRangeEnd = 0;
        }
      }
      stack[i] = e;
    }
    return depth;
  }

 private:
  static void put32(uint8_t* out, uint32_t value) {
    put16(out, value & 0xFFFF);
    put16(out + 2, value >> 16);
  }
  static uint32_t get32(const uint8_t* in) { return get16(in) | (static_cast<uint32_t>(get16(in + 2)) << 16); }
  static void put16(uint8_t* out, const int value) {
    out[0] = static_cast<uint8_t>(value & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  }
  static uint16_t get16(const uint8_t* in) { return static_cast<uint16_t>(in[0] | (in[1] << 8)); }
};
