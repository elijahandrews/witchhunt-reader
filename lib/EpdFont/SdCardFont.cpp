#include "SdCardFont.h"

#include <Arduino.h>  // ESP.getMaxAllocHeap() for the prewarm arena retry
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>
#include <new>

#include "CpFontFormat.h"

static_assert(sizeof(EpdGlyph) == 16, "EpdGlyph must be 16 bytes to match .cpfont file layout");
static_assert(sizeof(EpdUnicodeInterval) == 12, "EpdUnicodeInterval must be 12 bytes to match .cpfont file layout");
static_assert(sizeof(EpdKernClassEntry) == 3, "EpdKernClassEntry must be 3 bytes to match .cpfont file layout");
static_assert(sizeof(EpdLigaturePair) == 8, "EpdLigaturePair must be 8 bytes to match .cpfont file layout");

// FNV-1a hash for content-based font ID generation
static constexpr uint32_t FNV_OFFSET = 2166136261u;
static constexpr uint32_t FNV_PRIME = 16777619u;

static uint32_t fnv1a(const uint8_t* data, size_t len, uint32_t hash = FNV_OFFSET) {
  for (size_t i = 0; i < len; i++) {
    hash ^= data[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

// .cpfont magic bytes
static constexpr char CPFONT_MAGIC[8] = {'C', 'P', 'F', 'O', 'N', 'T', '\0', '\0'};
static constexpr uint32_t HEADER_SIZE = 32;
static constexpr uint32_t STYLE_TOC_ENTRY_SIZE = 32;
// Working headroom left outside the mini bitmap arena's single contiguous block, for the
// allocations prewarmStyle still makes after it.
static constexpr uint32_t PREWARM_MAX_ALLOC_RESERVE = 4 * 1024;

// Helper to read little-endian values from byte buffer
static inline uint16_t readU16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static inline int16_t readI16(const uint8_t* p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); }
static inline uint32_t readU32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

SdCardFont::~SdCardFont() { freeAll(); }

// --- Per-style free/cleanup ---

void SdCardFont::freeStyleMiniData(PerStyle& s) {
  delete[] s.miniIntervals;
  s.miniIntervals = nullptr;
  delete[] s.miniGlyphs;
  s.miniGlyphs = nullptr;
  delete[] s.miniBitmap;
  s.miniBitmap = nullptr;
  s.miniIntervalCount = 0;
  s.miniGlyphCount = 0;
  s.miniMode = PerStyle::MiniMode::NONE;
  // Clear dangling pointers in miniData and stubData (kern data points to freed mini arrays)
  s.miniData.kernLeftClasses = nullptr;
  s.miniData.kernRightClasses = nullptr;
  s.miniData.kernLeftCodepoints = nullptr;  // SD fonts use the packed class maps, not the split ones
  s.miniData.kernLeftClassIds = nullptr;
  s.miniData.kernRightCodepoints = nullptr;
  s.miniData.kernRightClassIds = nullptr;
  s.miniData.kernMatrix = nullptr;
  s.miniData.kernMatrixWide = nullptr;
  s.miniData.kernRowOffsets = nullptr;  // SD fonts are always dense; see EpdFontData::kernRowOffsets
  s.miniData.kernSparseCols = nullptr;
  s.miniData.kernSparseValues = nullptr;
  s.miniData.kernLeftEntryCount = 0;
  s.miniData.kernRightEntryCount = 0;
  s.miniData.kernLeftClassCount = 0;
  s.miniData.kernRightClassCount = 0;
  s.stubData.kernLeftClasses = nullptr;
  s.stubData.kernRightClasses = nullptr;
  s.stubData.kernLeftCodepoints = nullptr;  // SD fonts use the packed class maps, not the split ones
  s.stubData.kernLeftClassIds = nullptr;
  s.stubData.kernRightCodepoints = nullptr;
  s.stubData.kernRightClassIds = nullptr;
  s.stubData.kernMatrix = nullptr;
  s.stubData.kernMatrixWide = nullptr;
  s.stubData.kernRowOffsets = nullptr;
  s.stubData.kernSparseCols = nullptr;
  s.stubData.kernSparseValues = nullptr;
  s.stubData.kernLeftEntryCount = 0;
  s.stubData.kernRightEntryCount = 0;
  s.stubData.kernLeftClassCount = 0;
  s.stubData.kernRightClassCount = 0;
  s.onDemandMissCount = 0;
  s.onDemandMissLogged = 0;
  // NOTE: reportedMissCount is intentionally NOT reset here. The merge path
  // calls freeStyleMiniData() to swap mini buffers, and resetting the miss
  // tracker every paragraph would re-spam the log for the same 4 missing cps.
  // It IS reset by freeStyleAll() (font teardown) and clearAccumulation()
  // (section boundary), which are the right granularity for "forget what
  // we've reported".
  freeStyleMiniKern(s);
  memset(&s.miniData, 0, sizeof(s.miniData));
  s.epdFont.data = &s.stubData;
}

void SdCardFont::freeStyleKernLigatureData(PerStyle& s) {
  if (metadataOwned_) {
    // Kern class tables: heap-owned only for pure SD fonts (mmapDataBase_ == nullptr).
    // For mmap fonts they alias flash (EpdKernClassEntry is fully packed — safe).
    if (!mmapDataBase_) {
      delete[] s.kernLeftClasses;
      delete[] s.kernRightClasses;
    }
    // Ligature pairs are always heap-owned when metadataOwned_ is true
    // (EpdLigaturePair contains uint32_t, copied to heap in loadFromMmap).
    delete[] s.ligaturePairs;
  }
  s.kernLeftClasses = nullptr;
  s.kernClassesLoaded = false;
  s.kernRightClasses = nullptr;
  s.ligaturePairs = nullptr;
  s.ligLoaded = false;
  // Clear dangling pointers in EpdFontData structs
  s.stubData.ligaturePairs = nullptr;
  s.stubData.ligaturePairCount = 0;
  s.miniData.ligaturePairs = nullptr;
  s.miniData.ligaturePairCount = 0;
}

void SdCardFont::freeStyleMiniKern(PerStyle& s) {
  delete[] s.miniKernLeftClasses;
  s.miniKernLeftClasses = nullptr;
  delete[] s.miniKernRightClasses;
  s.miniKernRightClasses = nullptr;
  delete[] s.miniKernMatrix;
  s.miniKernMatrix = nullptr;
  s.miniKernLeftEntryCount = 0;
  s.miniKernRightEntryCount = 0;
  s.miniKernLeftClassCount = 0;
  s.miniKernRightClassCount = 0;
  s.miniKernReady = false;
}

void SdCardFont::freeStyleAll(PerStyle& s) {
  freeStyleMiniData(s);
  s.reportedMissCount = 0;
  if (metadataOwned_ && s.intervalsOwner < 0) {
    delete[] s.fullIntervals;
  }
  s.fullIntervals = nullptr;
  s.intervalsOwner = -1;
  freeStyleKernLigatureData(s);
  s.present = false;
  s.caps = {};
}

// --- Global free/cleanup ---

void SdCardFont::freeAll() {
  clearOverflow();
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    freeStyleAll(styles_[i]);
  }
  styleCount_ = 0;
  contentHash_ = 0;
  rasterDensity_ = 1;
  fileSize_ = 0;
  loaded_ = false;
  metadataOwned_ = true;
  mmapDataBase_ = nullptr;
}

void SdCardFont::unloadMetadata() {
  if (!loaded_) return;
  // For mmap-sourced fonts the heap copies can be dropped (they will be re-copied
  // from flash by reloadMetadata) — but since the mmap data is always accessible
  // and re-copying is cheap, keep the no-op behaviour: the heap copies stay live
  // and there is nothing to reload. Phase lifecycle overhead is zero.
  if (mmapDataBase_) {
    LOG_DBG("SDCF", "unloadMetadata: mmap font — no-op (%s)", filePath_);
    return;
  }
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    auto& s = styles_[i];
    if (!s.present) continue;
    // Free fullIntervals and kern/lig tables — keeps file offsets and header counts intact.
    // A borrowed table is the owner's to free; the borrower keeps intervalsOwner for the reload.
    if (s.intervalsOwner < 0) {
      delete[] s.fullIntervals;
    }
    s.fullIntervals = nullptr;
    freeStyleKernLigatureData(s);
    // Mini data is NOT freed — it is managed per-section and is already empty at the
    // chapter boundary where this is called.
  }
  LOG_DBG("SDCF", "Metadata unloaded: %s", filePath_);
}

bool SdCardFont::reloadMetadata() {
  if (!loaded_) return false;
  if (mmapDataBase_) {
    LOG_DBG("SDCF", "reloadMetadata: mmap font — no-op (%s)", filePath_);
    return true;
  }

  FsFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "reloadMetadata: failed to open %s", filePath_);
    return false;
  }

  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    auto& s = styles_[i];
    if (!s.present) continue;

    // A borrower re-points at its owner, which has a lower index and so was reloaded first.
    if (s.intervalsOwner >= 0) {
      s.fullIntervals = styles_[s.intervalsOwner].fullIntervals;
      continue;
    }

    // Re-read fullIntervals using stored file offset
    if (!s.fullIntervals) {
      s.fullIntervals = new (std::nothrow) EpdUnicodeInterval[s.header.intervalCount];
      if (!s.fullIntervals) {
        LOG_ERR("SDCF", "reloadMetadata: failed to alloc intervals for style %u", i);
        file.close();
        return false;
      }
      if (!file.seekSet(s.intervalsFileOffset)) {
        LOG_ERR("SDCF", "reloadMetadata: failed to seek intervals for style %u", i);
        file.close();
        return false;
      }
      const size_t sz = s.header.intervalCount * sizeof(EpdUnicodeInterval);
      if (file.read(reinterpret_cast<uint8_t*>(s.fullIntervals), sz) != static_cast<int>(sz)) {
        LOG_ERR("SDCF", "reloadMetadata: failed to read intervals for style %u", i);
        file.close();
        return false;
      }
    }
  }

  file.close();

  // Kern/lig tables are lazy-loaded on the next prewarm call — no need to reload eagerly.
  // The existing loadStyleKernLigatureData() path handles this transparently.
  LOG_DBG("SDCF", "Metadata reloaded: %s", filePath_);
  return true;
}

void SdCardFont::clearOverflow() {
  for (uint32_t i = 0; i < OVERFLOW_CAPACITY; i++) {
    if (!overflow_[i].occupied) {
      continue;
    }
    delete[] overflow_[i].bitmap;
    overflow_[i].bitmap = nullptr;
    overflow_[i].codepoint = 0;
    overflow_[i].styleIdx = 0;
    overflow_[i].occupied = false;
  }
  overflowCount_ = 0;
  overflowNext_ = 0;
}

// --- Per-style kern/ligature ---

void SdCardFont::applyKernLigaturePointers(const PerStyle& s, EpdFontData& data, bool mapped) const {
  // Layout on mmap fonts can use the full flash tables without a RAM copy.
  // SD uses the bounded glyph cache's mini matrix, including cumulative layout
  // glyphs so measuring an earlier word remains valid after the cache grows.
  data.kernLeftClasses = mapped ? s.kernLeftClasses : s.miniKernLeftClasses;
  data.kernRightClasses = mapped ? s.kernRightClasses : s.miniKernRightClasses;
  // Packed form, as stored in the .cpfont; the split arrays are built-in only.
  data.kernLeftCodepoints = nullptr;
  data.kernLeftClassIds = nullptr;
  data.kernRightCodepoints = nullptr;
  data.kernRightClassIds = nullptr;
  const auto* matrix =
      mapped ? mmapDataBase_ + s.kernMatrixFileOffset : reinterpret_cast<const uint8_t*>(s.miniKernMatrix);
  data.kernMatrix = rasterDensity_ == 2 ? nullptr : reinterpret_cast<const int8_t*>(matrix);
  data.kernMatrixWide = rasterDensity_ == 2 ? matrix : nullptr;
  // The .cpfont format stores a dense matrix and is mapped in place, so SD fonts never use the
  // sparse form the built-in fonts switched to. Set explicitly rather than relying on the
  // caller's initialisation: getKerning() picks the representation by which pointer is non-null.
  data.kernRowOffsets = nullptr;
  data.kernSparseCols = nullptr;
  data.kernSparseValues = nullptr;
  data.kernLeftEntryCount = mapped ? s.header.kernLeftEntryCount : s.miniKernLeftEntryCount;
  data.kernRightEntryCount = mapped ? s.header.kernRightEntryCount : s.miniKernRightEntryCount;
  data.kernLeftClassCount = mapped ? s.header.kernLeftClassCount : s.miniKernLeftClassCount;
  data.kernRightClassCount = mapped ? s.header.kernRightClassCount : s.miniKernRightClassCount;
  // Ligatures are small (typically < 1KB) so they stay resident.
  data.ligaturePairs = s.ligaturePairs;
  data.ligaturePairCount = s.header.ligaturePairCount;
}

bool SdCardFont::loadStyleKernLigatureData(PerStyle& s, bool ligatureOnly) {
  // The initial ligature expansion pass needs only substitutions. A layout
  // caller that requests kerning later loads class maps and the cache's mini
  // matrix too; otherwise measured and rendered word widths would disagree.
  const bool wantKern = !ligatureOnly && s.header.kernLeftEntryCount > 0;
  const bool wantLig = s.header.ligaturePairCount > 0;

  const bool kernDone = !wantKern || s.kernClassesLoaded;
  const bool ligDone = !wantLig || s.ligLoaded;
  if (kernDone && ligDone) return true;

  FsFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont for kern/lig: %s", filePath_);
    return false;
  }

  if (wantKern && !s.kernClassesLoaded) {
    // Load only the small class-lookup tables (~3KB each). The full matrix
    // (~36KB contiguous for Literata) is built per-page from SD in
    // buildMiniKernMatrix().
    EpdKernClassEntry* newLeft = new (std::nothrow) EpdKernClassEntry[s.header.kernLeftEntryCount];
    EpdKernClassEntry* newRight = new (std::nothrow) EpdKernClassEntry[s.header.kernRightEntryCount];

    if (!newLeft || !newRight) {
      delete[] newLeft;
      delete[] newRight;
      LOG_ERR("SDCF", "Failed to allocate kern classes (%u+%u bytes)", s.header.kernLeftEntryCount * 3u,
              s.header.kernRightEntryCount * 3u);
      file.close();
      return false;
    }

    if (!file.seekSet(s.kernLeftFileOffset)) {
      delete[] newLeft;
      delete[] newRight;
      LOG_ERR("SDCF", "Failed to seek to kern data");
      file.close();
      return false;
    }
    size_t leftSz = s.header.kernLeftEntryCount * sizeof(EpdKernClassEntry);
    size_t rightSz = s.header.kernRightEntryCount * sizeof(EpdKernClassEntry);
    if (file.read(reinterpret_cast<uint8_t*>(newLeft), leftSz) != static_cast<int>(leftSz) ||
        file.read(reinterpret_cast<uint8_t*>(newRight), rightSz) != static_cast<int>(rightSz)) {
      delete[] newLeft;
      delete[] newRight;
      LOG_ERR("SDCF", "Failed to read kern classes");
      file.close();
      return false;
    }
    s.kernLeftClasses = newLeft;
    s.kernRightClasses = newRight;
    s.kernClassesLoaded = true;
  }

  if (wantLig && !s.ligLoaded) {
    EpdLigaturePair* newLig = new (std::nothrow) EpdLigaturePair[s.header.ligaturePairCount];
    if (!newLig) {
      LOG_ERR("SDCF", "Failed to allocate ligature pairs");
      file.close();
      return false;
    }
    if (!file.seekSet(s.ligatureFileOffset)) {
      delete[] newLig;
      LOG_ERR("SDCF", "Failed to seek to ligature data");
      file.close();
      return false;
    }
    size_t sz = s.header.ligaturePairCount * sizeof(EpdLigaturePair);
    if (file.read(reinterpret_cast<uint8_t*>(newLig), sz) != static_cast<int>(sz)) {
      delete[] newLig;
      LOG_ERR("SDCF", "Failed to read ligature pairs");
      file.close();
      return false;
    }
    s.ligaturePairs = newLig;
    s.ligLoaded = true;

    // Make ligatures visible to the stub (used when no mini data built yet).
    // Kern stays nullptr on the stub — it is only wired in miniData via
    // applyKernLigaturePointers() after buildMiniKernMatrix() runs.
    s.stubData.ligaturePairs = s.ligaturePairs;
    s.stubData.ligaturePairCount = s.header.ligaturePairCount;
  }

  file.close();
  LOG_DBG("SDCF", "Kern/lig loaded: kernL=%u kernR=%u ligs=%u ligOnly=%d",
          s.kernClassesLoaded ? s.header.kernLeftEntryCount : 0u,
          s.kernClassesLoaded ? s.header.kernRightEntryCount : 0u, s.ligLoaded ? s.header.ligaturePairCount : 0u,
          ligatureOnly);
  return true;
}

// --- Per-page mini kern matrix ---

// Local copy of EpdFont.cpp's lookupKernClass (that one is file-static there).
// Returns the 1-based class ID for `cp`, or 0 if the codepoint has no kerning class.
static uint8_t miniLookupKernClass(const EpdKernClassEntry* entries, uint16_t count, uint32_t cp) {
  if (!entries || count == 0 || cp > 0xFFFF) return 0;
  const auto target = static_cast<uint16_t>(cp);
  const auto* end = entries + count;
  const auto it =
      std::lower_bound(entries, end, target, [](const EpdKernClassEntry& e, uint16_t v) { return e.codepoint < v; });
  return (it != end && it->codepoint == target) ? it->classId : 0;
}

// Build a small per-page kern matrix containing ONLY the (leftClass, rightClass)
// pairs reachable from codepoints in the current text. Class IDs are renumbered
// to a dense 1..N range so the resulting matrix is usedLeft × usedRight (typical
// Latin page: ~25×25 bytes) instead of the font's full ~180×200 (~36KB).
//
// Correctness: EpdFont::getKerning only touches `kernLeftClasses` /
// `kernRightClasses` / `kernMatrix` / the count fields — we swap all of them to
// the mini versions together in applyKernLigaturePointers, so a codepoint not
// on this page simply returns class 0 (no kerning), which was the pre-existing
// behavior for any codepoint outside the kern classes.
bool SdCardFont::buildMiniKernMatrix(PerStyle& s, const uint32_t* codepoints, uint32_t cpCount, HalFile& file) {
  freeStyleMiniKern(s);
  if (!s.kernLeftClasses || !s.kernRightClasses || s.header.kernLeftEntryCount == 0 ||
      s.header.kernRightEntryCount == 0) {
    return true;  // font has no kern classes — nothing to build
  }

  // 4× 256-byte scratch arrays: heap-allocated as one block to avoid blowing
  // the activity task's 8 KB stack when this runs deep in the parser → layout
  // → prewarm call chain (especially when invoked 4× per paragraph, once per
  // style).
  // Layout: [usedLeft 256][usedRight 256][leftRenumber 256][rightRenumber 256]
  //         [newToOldLeft 256][newToOldRight 256] = 1536 bytes total
  std::unique_ptr<uint8_t[]> scratch(new (std::nothrow) uint8_t[6 * 256]());
  if (!scratch) {
    LOG_ERR("SDCF", "Failed to allocate kern scratch (1536 bytes)");
    return false;
  }
  uint8_t* const base = scratch.get();
  uint8_t* usedLeft = base + 0 * 256;
  uint8_t* usedRight = base + 1 * 256;
  uint8_t* leftRenumber = base + 2 * 256;
  uint8_t* rightRenumber = base + 3 * 256;
  uint8_t* newToOldLeft = base + 4 * 256;
  uint8_t* newToOldRight = base + 5 * 256;

  // Step 1: mark used left/right classes (class IDs are uint8_t — 0 means none).
  for (uint32_t i = 0; i < cpCount; i++) {
    uint8_t lc = miniLookupKernClass(s.kernLeftClasses, s.header.kernLeftEntryCount, codepoints[i]);
    if (lc) usedLeft[lc] = 1;
    uint8_t rc = miniLookupKernClass(s.kernRightClasses, s.header.kernRightEntryCount, codepoints[i]);
    if (rc) usedRight[rc] = 1;
  }

  // Step 2: build renumber maps (oldClassId -> newClassId, 1-based) and
  // reverse maps (newClassId -> oldClassId) for the SD read step.
  uint8_t numLeft = 0, numRight = 0;
  for (int i = 1; i < 256; i++) {
    if (usedLeft[i]) {
      numLeft++;
      leftRenumber[i] = numLeft;
      newToOldLeft[numLeft] = static_cast<uint8_t>(i);
    }
    if (usedRight[i]) {
      numRight++;
      rightRenumber[i] = numRight;
      newToOldRight[numRight] = static_cast<uint8_t>(i);
    }
  }
  if (numLeft == 0 || numRight == 0) {
    return true;  // no kern pairs applicable on this page
  }

  // Step 3: count how many codepoint→classId entries the mini class tables need.
  // Each resident class table has one entry per kerned codepoint in the page.
  uint16_t miniLeftCount = 0;
  uint16_t miniRightCount = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    if (miniLookupKernClass(s.kernLeftClasses, s.header.kernLeftEntryCount, codepoints[i]) != 0) miniLeftCount++;
    if (miniLookupKernClass(s.kernRightClasses, s.header.kernRightEntryCount, codepoints[i]) != 0) miniRightCount++;
  }

  // Step 4: allocate the three mini buffers. The matrix is <1KB in practice
  // (<30 × <30 × 1 byte) so fragmentation is a non-issue.
  const uint32_t entryBytes = rasterDensity_ == 2 ? 2u : 1u;
  const uint32_t miniRowBytes = static_cast<uint32_t>(numRight) * entryBytes;
  const uint32_t matrixBytes = static_cast<uint32_t>(numLeft) * miniRowBytes;
  s.miniKernLeftClasses = new (std::nothrow) EpdKernClassEntry[miniLeftCount];
  s.miniKernRightClasses = new (std::nothrow) EpdKernClassEntry[miniRightCount];
  s.miniKernMatrix = new (std::nothrow) int8_t[matrixBytes];
  if (!s.miniKernLeftClasses || !s.miniKernRightClasses || !s.miniKernMatrix) {
    LOG_ERR("SDCF", "Failed to allocate mini kern (%u+%u+%u bytes)", miniLeftCount * 3u, miniRightCount * 3u,
            matrixBytes);
    freeStyleMiniKern(s);
    return false;
  }

  // Step 5: populate mini class tables. `codepoints` is already sorted (see
  // prewarm()) so the output is sorted by codepoint — required for binary
  // search in lookupKernClass during render.
  uint16_t lIdx = 0, rIdx = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    uint32_t cp = codepoints[i];
    if (cp > 0xFFFF) continue;  // kern class entries are uint16_t
    uint8_t lc = miniLookupKernClass(s.kernLeftClasses, s.header.kernLeftEntryCount, cp);
    if (lc) {
      s.miniKernLeftClasses[lIdx].codepoint = static_cast<uint16_t>(cp);
      s.miniKernLeftClasses[lIdx].classId = leftRenumber[lc];
      lIdx++;
    }
    uint8_t rc = miniLookupKernClass(s.kernRightClasses, s.header.kernRightEntryCount, cp);
    if (rc) {
      s.miniKernRightClasses[rIdx].codepoint = static_cast<uint16_t>(cp);
      s.miniKernRightClasses[rIdx].classId = rightRenumber[rc];
      rIdx++;
    }
  }

  // Step 6: populate the mini matrix rows.
  //
  // Mmap path: the full kern matrix is directly pointer-accessible in flash.
  // Read each needed row via pointer arithmetic — no heap buffer, no seeks.
  //
  // SD path: read the matrix in fixed-size 4 KB chunks sweeping forward through
  // the file. This bounds the heap spike at KERN_CHUNK_BYTES instead of the full
  // matrix size (~28 KB for Literata). Falls back to per-row reads if the chunk
  // buffer allocation fails.
  const uint32_t rowBytes = static_cast<uint32_t>(s.header.kernRightClassCount) * entryBytes;

  // Build a sorted-by-oldL mapping so we sweep the source forward.
  // Reuse the usedLeft/usedRight scratch slots — both are fully consumed after
  // step 1 and not read again. newToOldLeft/Right (slots 4 & 5) must stay intact.
  uint8_t* sortedOldL = base + 0 * 256;
  uint8_t* sortedNewL = base + 1 * 256;
  {
    uint8_t k = 0;
    for (int oldL = 1; oldL < 256; oldL++) {
      for (uint16_t newL = 1; newL <= numLeft; newL++) {
        if (newToOldLeft[newL] == static_cast<uint8_t>(oldL)) {
          sortedOldL[k] = static_cast<uint8_t>(oldL);
          sortedNewL[k] = newL;
          k++;
          break;
        }
      }
    }
  }

  if (mmapDataBase_) {
    // Mmap fast-path: kern matrix is at a fixed offset in the flash mapping.
    // Each row holds byte entries in V4, two-byte little-endian entries in V5.
    // Direct pointer read — no heap allocation, no SD I/O.
    const int8_t* matrixBase = reinterpret_cast<const int8_t*>(mmapDataBase_ + s.kernMatrixFileOffset);
    for (uint8_t k = 0; k < numLeft; k++) {
      const uint8_t oldL = sortedOldL[k];
      const uint8_t newL = sortedNewL[k];
      const int8_t* srcRow = matrixBase + (oldL - 1u) * rowBytes;
      int8_t* miniRow = s.miniKernMatrix + (newL - 1u) * miniRowBytes;
      for (uint16_t newR = 1; newR <= numRight; newR++) {
        memcpy(miniRow + (newR - 1u) * entryBytes, srcRow + (newToOldRight[newR] - 1u) * entryBytes, entryBytes);
      }
    }
    LOG_DBG("SDCF", "Built mini kern (mmap): %u×%u=%u bytes", numLeft, numRight, matrixBytes);
  } else {
    // SD path: chunked sweep.
    static constexpr uint32_t KERN_CHUNK_BYTES = 4096;
    std::unique_ptr<int8_t[]> chunkBuf(new (std::nothrow) int8_t[KERN_CHUNK_BYTES]);
    if (chunkBuf) {
      uint32_t chunkStart = UINT32_MAX;
      uint32_t chunkEnd = 0;
      uint32_t chunkSeeks = 0;
      for (uint8_t k = 0; k < numLeft; k++) {
        const uint8_t oldL = sortedOldL[k];
        const uint8_t newL = sortedNewL[k];
        const uint32_t rowFileOff = s.kernMatrixFileOffset + (oldL - 1u) * rowBytes;
        const uint32_t rowFileEnd = rowFileOff + rowBytes;
        if (rowFileOff < chunkStart || rowFileEnd > chunkEnd) {
          chunkStart = rowFileOff;
          uint32_t toRead = KERN_CHUNK_BYTES;
          const uint32_t matrixEnd =
              s.kernMatrixFileOffset + static_cast<uint32_t>(s.header.kernLeftClassCount) * rowBytes;
          if (chunkStart + toRead > matrixEnd) toRead = matrixEnd - chunkStart;
          if (!file.seekSet(chunkStart)) {
            LOG_ERR("SDCF", "Failed to seek to kern chunk at %u", chunkStart);
            freeStyleMiniKern(s);
            return false;
          }
          if (file.read(reinterpret_cast<uint8_t*>(chunkBuf.get()), toRead) != static_cast<int>(toRead)) {
            LOG_ERR("SDCF", "Failed to read kern chunk (%u bytes)", toRead);
            freeStyleMiniKern(s);
            return false;
          }
          chunkEnd = chunkStart + toRead;
          chunkSeeks++;
        }
        const int8_t* srcRow = chunkBuf.get() + (rowFileOff - chunkStart);
        int8_t* miniRow = s.miniKernMatrix + (newL - 1u) * miniRowBytes;
        for (uint16_t newR = 1; newR <= numRight; newR++) {
          memcpy(miniRow + (newR - 1u) * entryBytes, srcRow + (newToOldRight[newR] - 1u) * entryBytes, entryBytes);
        }
      }
      LOG_DBG("SDCF", "Built mini kern (chunked %uB): %u×%u=%u bytes, %u seeks", KERN_CHUNK_BYTES, numLeft, numRight,
              matrixBytes, chunkSeeks);
    } else {
      // Fallback: one seek + one row read per used left class.
      LOG_DBG("SDCF", "Built mini kern (per-row fallback): %u rows", numLeft);
      std::unique_ptr<int8_t[]> rowBuf(new (std::nothrow) int8_t[rowBytes]);
      if (!rowBuf) {
        LOG_ERR("SDCF", "Failed to allocate row buffer (%u bytes)", rowBytes);
        freeStyleMiniKern(s);
        return false;
      }
      for (uint8_t k = 0; k < numLeft; k++) {
        const uint8_t oldL = sortedOldL[k];
        const uint8_t newL = sortedNewL[k];
        const uint32_t rowFileOff = s.kernMatrixFileOffset + (oldL - 1u) * rowBytes;
        if (!file.seekSet(rowFileOff)) {
          LOG_ERR("SDCF", "Failed to seek to kern row %u", oldL);
          freeStyleMiniKern(s);
          return false;
        }
        if (file.read(reinterpret_cast<uint8_t*>(rowBuf.get()), rowBytes) != static_cast<int>(rowBytes)) {
          LOG_ERR("SDCF", "Failed to read kern row %u", oldL);
          freeStyleMiniKern(s);
          return false;
        }
        int8_t* miniRow = s.miniKernMatrix + (newL - 1u) * miniRowBytes;
        for (uint16_t newR = 1; newR <= numRight; newR++) {
          memcpy(miniRow + (newR - 1u) * entryBytes, rowBuf.get() + (newToOldRight[newR] - 1u) * entryBytes,
                 entryBytes);
        }
      }
    }
  }

  s.miniKernLeftEntryCount = lIdx;
  s.miniKernRightEntryCount = rIdx;
  s.miniKernLeftClassCount = numLeft;
  s.miniKernRightClassCount = numRight;
  return true;
}

// Build kerning for the complete cumulative metadata cache, not merely the
// latest paragraph's codepoints. The existing MAX_PAGE_GLYPHS cap bounds this
// exactly as it bounds a full page prewarm; the full source matrix is never
// allocated. Repeated queries on an unchanged cache reuse the mini matrix.
bool SdCardFont::ensureCachedKernMatrix(PerStyle& s, HalFile& file) {
  if (!loadStyleKernLigatureData(s)) return false;
  if (s.miniKernReady) return true;
  if (s.header.kernLeftEntryCount == 0 || s.header.kernRightEntryCount == 0) {
    s.miniKernReady = true;
    return true;
  }
  std::unique_ptr<uint32_t[]> cps(new (std::nothrow) uint32_t[s.miniGlyphCount]);
  if (!cps) return false;
  uint32_t count = 0;
  for (uint32_t iv = 0; iv < s.miniIntervalCount; ++iv) {
    const auto& interval = s.miniIntervals[iv];
    for (uint32_t cp = interval.first; cp <= interval.last; ++cp) {
      if (count >= s.miniGlyphCount) return false;
      cps[count++] = cp;
    }
  }
  if (count != s.miniGlyphCount) return false;
  if (!file && !mmapDataBase_ && !Storage.openFileForRead("SDCF", filePath_, file)) return false;
  s.miniKernReady = buildMiniKernMatrix(s, cps.get(), count, file);
  return s.miniKernReady;
}

// --- Glyph miss callback ---

void SdCardFont::applyGlyphMissCallback(uint8_t styleIdx) {
  overflowCtx_[styleIdx].self = this;
  overflowCtx_[styleIdx].styleIdx = styleIdx;

  auto& s = styles_[styleIdx];
  s.stubData.glyphMissHandler = &SdCardFont::onGlyphMiss;
  s.stubData.glyphMissCtx = &overflowCtx_[styleIdx];
  s.stubData.capsGlyph = s.caps.glyphCount ? &SdCardFont::onCapsGlyph : nullptr;
  s.stubData.alternateKerning = s.caps.glyphCount ? &SdCardFont::onAlternateKerning : nullptr;
  s.miniData.capsGlyph = s.stubData.capsGlyph;
  s.miniData.alternateKerning = s.stubData.alternateKerning;
}

// --- V6 authored small capitals ---

bool SdCardFont::readAt(HalFile& file, uint32_t offset, void* out, size_t count) const {
  if (offset > fileSize_ || count > fileSize_ - offset) return false;
  if (mmapDataBase_) {
    memcpy(out, mmapDataBase_ + offset, count);
    return true;
  }
  if (!file && !Storage.openFileForRead("SDCF", filePath_, file)) return false;
  return (file.position() == offset || file.seekSet(offset)) &&
         file.read(static_cast<uint8_t*>(out), count) == static_cast<int>(count);
}

// All supported versions use sorted codepoint maps with 1-based matrix IDs.
// Validate before exposing either mmap pointers or lazy SD tables to kerning
// lookup. Class 0 remains valid: it explicitly means no kerning in legacy files.
// A fixed stack buffer bounds validation memory independently of map size.
bool SdCardFont::validateKernClassMaps(const PerStyle& s, HalFile& file) const {
  constexpr uint32_t CHUNK_ENTRIES = 64;
  uint8_t chunk[CHUNK_ENTRIES * sizeof(EpdKernClassEntry)];
  for (unsigned side = 0; side < 2; ++side) {
    const uint32_t offset = side ? s.kernRightFileOffset : s.kernLeftFileOffset;
    const uint32_t count = side ? s.header.kernRightEntryCount : s.header.kernLeftEntryCount;
    const uint8_t classes = side ? s.header.kernRightClassCount : s.header.kernLeftClassCount;
    uint16_t previous = 0;
    for (uint32_t first = 0; first < count; first += CHUNK_ENTRIES) {
      const uint32_t entries = std::min(CHUNK_ENTRIES, count - first);
      if (!readAt(file, offset + first * sizeof(EpdKernClassEntry), chunk, entries * sizeof(EpdKernClassEntry)))
        return false;
      for (uint32_t i = 0; i < entries; ++i) {
        const auto* entry = chunk + i * sizeof(EpdKernClassEntry);
        const uint16_t cp = readU16(entry);
        if (((first || i) && cp <= previous) || entry[2] > classes) return false;
        previous = cp;
      }
    }
  }
  return true;
}

bool SdCardFont::validateV6Style(PerStyle& s, HalFile& file) {
  if (!s.header.intervalCount || !s.header.glyphCount || !s.fullIntervals) return false;
  uint32_t expectedIndex = 0, previousEnd = 0;
  for (uint32_t i = 0; i < s.header.intervalCount; ++i) {
    const auto& interval = s.fullIntervals[i];
    if (interval.first > interval.last || interval.last > 0x10FFFF || (i && interval.first <= previousEnd) ||
        interval.offset != expectedIndex)
      return false;
    expectedIndex += interval.last - interval.first + 1;
    previousEnd = interval.last;
  }
  if (expectedIndex != s.header.glyphCount) return false;
  uint32_t styleEnd = static_cast<uint32_t>(fileSize_);
  for (const auto& other : styles_) {
    if (!other.present || &other == &s) continue;
    if (other.intervalsFileOffset == s.intervalsFileOffset) return false;
    if (other.intervalsFileOffset > s.intervalsFileOffset) styleEnd = std::min(styleEnd, other.intervalsFileOffset);
  }
  auto& caps = s.caps;
  const uint32_t normalEnd = caps.offset ? caps.offset : styleEnd;
  if (normalEnd < s.bitmapFileOffset || normalEnd > styleEnd) return false;
  // Require tight sequential bitmap bounds, so a malformed feature cannot
  // overlap ordinary glyphs, another style, or an unrelated part of the file.
  const auto validateGlyphs = [&](uint32_t start, uint32_t count, uint32_t bitmapBytes) {
    uint32_t expectedOffset = 0;
    uint8_t record[16];
    for (uint32_t i = 0; i < count; ++i) {
      if (!readAt(file, start + i * 16, record, sizeof(record))) return false;
      const uint32_t width = record[0] + 256u * record[10];
      const uint32_t height = record[1] + 256u * record[11];
      const uint64_t bytes = (uint64_t(width) * height + 3) / 4;
      const uint32_t length = readU16(record + 8), offset = readU32(record + 12);
      if (bytes != length || offset != expectedOffset || offset > bitmapBytes || length > bitmapBytes - offset)
        return false;
      expectedOffset += length;
    }
    return expectedOffset == bitmapBytes;
  };
  if (!validateGlyphs(s.glyphsFileOffset, s.header.glyphCount, normalEnd - s.bitmapFileOffset)) return false;
  if (!caps.offset) return true;
  uint8_t header[24];
  if (!readAt(file, caps.offset, header, sizeof(header)) || memcmp(header, "SCAP", 4) || header[4] != 1 || header[5] ||
      readU16(header + 12) || readU16(header + 14))
    return false;
  caps.smcpCount = readU16(header + 6);
  caps.c2scCount = readU16(header + 8);
  caps.glyphCount = readU16(header + 10);
  caps.kernCount = readU32(header + 16);
  caps.bitmapBytes = readU32(header + 20);
  if (!caps.glyphCount || !(caps.smcpCount || caps.c2scCount)) return false;
  uint64_t cursor = uint64_t(caps.offset) + sizeof(header);
  const auto advance = [&](uint64_t bytes, uint32_t& offset) {
    if (cursor > styleEnd || bytes > styleEnd - cursor) return false;
    offset = static_cast<uint32_t>(cursor);
    cursor += bytes;
    return true;
  };
  if (!advance(uint64_t(caps.smcpCount) * 6, caps.smcpOffset) ||
      !advance(uint64_t(caps.c2scCount) * 6, caps.c2scOffset) ||
      !advance(uint64_t(caps.glyphCount) * 16, caps.glyphOffset) ||
      !advance(uint64_t(caps.kernCount) * 10, caps.kernOffset) || !advance(caps.bitmapBytes, caps.bitmapOffset) ||
      cursor != styleEnd)
    return false;
  std::unique_ptr<uint8_t[]> referenced(new (std::nothrow) uint8_t[(caps.glyphCount + 7u) / 8u]());
  if (!referenced) return false;
  for (unsigned mode = 1; mode <= 2; ++mode) {
    const uint32_t count = mode == 1 ? caps.smcpCount : caps.c2scCount;
    const uint32_t offset = mode == 1 ? caps.smcpOffset : caps.c2scOffset;
    uint32_t previous = 0;
    uint8_t entry[6];
    for (uint32_t i = 0; i < count; ++i) {
      if (!readAt(file, offset + i * 6, entry, sizeof(entry))) return false;
      const uint32_t cp = readU32(entry);
      const uint16_t id = readU16(entry + 4);
      if ((i && cp <= previous) || cp > 0x10FFFF || findGlobalGlyphIndex(s, cp) < 0 || id >= caps.glyphCount)
        return false;
      referenced[id / 8] |= uint8_t(1u << (id % 8));
      previous = cp;
    }
  }
  for (uint32_t i = 0; i < caps.glyphCount; ++i)
    if (!(referenced[i / 8] & (1u << (i % 8)))) return false;
  if (!validateGlyphs(caps.glyphOffset, caps.glyphCount, caps.bitmapBytes)) return false;
  const auto validKey = [&](uint32_t key) {
    return key >= EPD_ALTERNATE_GLYPH_BASE ? key - EPD_ALTERNATE_GLYPH_BASE < caps.glyphCount
                                           : findGlobalGlyphIndex(s, key) >= 0;
  };
  uint32_t previousLeft = 0, previousRight = 0;
  uint8_t pair[10];
  for (uint32_t i = 0; i < caps.kernCount; ++i) {
    if (!readAt(file, caps.kernOffset + i * 10, pair, sizeof(pair))) return false;
    const uint32_t left = readU32(pair), right = readU32(pair + 4);
    if (!validKey(left) || !validKey(right) || (left < EPD_ALTERNATE_GLYPH_BASE && right < EPD_ALTERNATE_GLYPH_BASE) ||
        !readI16(pair + 8) || (i && (left < previousLeft || (left == previousLeft && right <= previousRight))))
      return false;
    previousLeft = left;
    previousRight = right;
  }
  return true;
}

uint32_t SdCardFont::onCapsGlyph(void* ctx, uint32_t cp, uint8_t mode) {
  if (!ctx || (mode != 1 && mode != 2)) return 0;
  auto& oc = *static_cast<OverflowContext*>(ctx);
  auto& self = *oc.self;
  if (!self.loaded_ || oc.styleIdx >= MAX_STYLES) return 0;
  auto& caps = self.styles_[oc.styleIdx].caps;
  if (caps.lastMode == mode && caps.lastCp == cp) return caps.lastKey;
  FsFile file;
  uint32_t low = 0, high = mode == 1 ? caps.smcpCount : caps.c2scCount;
  const uint32_t offset = mode == 1 ? caps.smcpOffset : caps.c2scOffset;
  uint32_t result = 0;
  while (low < high) {
    const uint32_t mid = low + (high - low) / 2;
    uint8_t entry[6];
    if (!self.readAt(file, offset + mid * 6, entry, sizeof(entry))) return 0;  // Retry transient SD failures.
    const uint32_t source = readU32(entry);
    if (source < cp)
      low = mid + 1;
    else if (source > cp)
      high = mid;
    else {
      result = EPD_ALTERNATE_GLYPH_BASE + readU16(entry + 4);
      break;
    }
  }
  caps.lastCp = cp;
  caps.lastMode = mode;
  caps.lastKey = result;
  return result;
}

int16_t SdCardFont::onAlternateKerning(void* ctx, uint32_t left, uint32_t right) {
  if (!ctx) return 0;
  auto& oc = *static_cast<OverflowContext*>(ctx);
  auto& self = *oc.self;
  if (!self.loaded_ || oc.styleIdx >= MAX_STYLES) return 0;
  auto& caps = self.styles_[oc.styleIdx].caps;
  if (caps.kernCached && caps.lastLeft == left && caps.lastRight == right) return caps.lastKern;
  FsFile file;
  uint32_t low = 0, high = caps.kernCount;
  int16_t result = 0;
  while (low < high) {
    const uint32_t mid = low + (high - low) / 2;
    uint8_t pair[10];
    if (!self.readAt(file, caps.kernOffset + mid * 10, pair, sizeof(pair))) return 0;
    const uint32_t l = readU32(pair), r = readU32(pair + 4);
    if (l < left || (l == left && r < right))
      low = mid + 1;
    else if (l > left || r > right)
      high = mid;
    else {
      result = readI16(pair + 8);
      break;
    }
  }
  caps.lastLeft = left;
  caps.lastRight = right;
  caps.lastKern = result;
  caps.kernCached = true;
  return result;
}

// --- Compute per-style file offsets from a base data offset ---

bool SdCardFont::computeStyleFileOffsets(PerStyle& s, uint32_t baseOffset, size_t fileSize) const {
  // Calculate in 64 bits before narrowing, rejecting wrapped offsets and truncated
  // metadata before either SD seeks or direct mmap access can use them.
  uint64_t offset = baseOffset;
  s.intervalsFileOffset = baseOffset;
  offset += uint64_t(s.header.intervalCount) * sizeof(EpdUnicodeInterval);
  if (offset > fileSize || offset > UINT32_MAX) return false;
  s.glyphsFileOffset = static_cast<uint32_t>(offset);
  offset += uint64_t(s.header.glyphCount) * sizeof(EpdGlyph);
  if (offset > fileSize || offset > UINT32_MAX) return false;
  s.kernLeftFileOffset = static_cast<uint32_t>(offset);
  offset += uint64_t(s.header.kernLeftEntryCount) * sizeof(EpdKernClassEntry);
  if (offset > fileSize || offset > UINT32_MAX) return false;
  s.kernRightFileOffset = static_cast<uint32_t>(offset);
  offset += uint64_t(s.header.kernRightEntryCount) * sizeof(EpdKernClassEntry);
  if (offset > fileSize || offset > UINT32_MAX) return false;
  s.kernMatrixFileOffset = static_cast<uint32_t>(offset);
  offset += uint64_t(s.header.kernLeftClassCount) * s.header.kernRightClassCount * (rasterDensity_ == 2 ? 2u : 1u);
  if (offset > fileSize || offset > UINT32_MAX) return false;
  s.ligatureFileOffset = static_cast<uint32_t>(offset);
  offset += uint64_t(s.header.ligaturePairCount) * sizeof(EpdLigaturePair);
  if (offset > fileSize || offset > UINT32_MAX) return false;
  s.bitmapFileOffset = static_cast<uint32_t>(offset);
  return true;
}

// --- Interval table sharing ---

// Returns the lowest earlier style that owns a table byte-identical to style `styleIdx`'s
// on-file intervals, or -1. Only owners are candidates, so a borrower never chains.
// Reads the file in stack-sized chunks, so a mismatch costs a re-read, never an allocation.
int8_t SdCardFont::findIdenticalIntervals(const uint8_t styleIdx, HalFile& file) {
  const auto& s = styles_[styleIdx];
  static constexpr uint32_t CHUNK = 16;  // 192 bytes of stack
  EpdUnicodeInterval chunk[CHUNK];
  for (uint8_t k = 0; k < styleIdx; k++) {
    const auto& owner = styles_[k];
    if (!owner.present || owner.intervalsOwner >= 0 || !owner.fullIntervals) continue;
    if (owner.header.intervalCount != s.header.intervalCount) continue;
    if (!file.seekSet(s.intervalsFileOffset)) return -1;
    bool same = true;
    for (uint32_t done = 0; same && done < s.header.intervalCount; done += CHUNK) {
      const uint32_t n = std::min(CHUNK, s.header.intervalCount - done);
      const int bytes = static_cast<int>(n * sizeof(EpdUnicodeInterval));
      same = file.read(reinterpret_cast<uint8_t*>(chunk), bytes) == bytes &&
             memcmp(chunk, owner.fullIntervals + done, bytes) == 0;
    }
    if (same) {
      LOG_DBG("SDCF", "Style %u shares style %u's %u-interval table (%u B not allocated)", styleIdx, k,
              s.header.intervalCount, static_cast<unsigned>(s.header.intervalCount * sizeof(EpdUnicodeInterval)));
      return static_cast<int8_t>(k);
    }
  }
  return -1;
}

int8_t SdCardFont::findIdenticalIntervals(const uint8_t styleIdx, const uint8_t* records) {
  const auto& s = styles_[styleIdx];
  const size_t bytes = s.header.intervalCount * sizeof(EpdUnicodeInterval);
  for (uint8_t k = 0; k < styleIdx; k++) {
    const auto& owner = styles_[k];
    if (!owner.present || owner.intervalsOwner >= 0 || !owner.fullIntervals) continue;
    if (owner.header.intervalCount != s.header.intervalCount) continue;
    if (memcmp(records, owner.fullIntervals, bytes) == 0) {
      LOG_DBG("SDCF", "Style %u shares style %u's %u-interval table (%u B not allocated)", styleIdx, k,
              s.header.intervalCount, static_cast<unsigned>(bytes));
      return static_cast<int8_t>(k);
    }
  }
  return -1;
}

// --- Load ---

bool SdCardFont::load(const char* path) {
  freeAll();
  if (strlen(path) >= sizeof(filePath_)) {
    LOG_ERR("SDCF", "Path too long (%zu bytes, max %zu)", strlen(path), sizeof(filePath_) - 1);
    return false;
  }
  strncpy(filePath_, path, sizeof(filePath_) - 1);
  filePath_[sizeof(filePath_) - 1] = '\0';

  FsFile file;
  if (!Storage.openFileForRead("SDCF", path, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont: %s", path);
    return false;
  }

  // Read and validate global header
  uint8_t headerBuf[HEADER_SIZE];
  if (file.read(headerBuf, HEADER_SIZE) != HEADER_SIZE) {
    LOG_ERR("SDCF", "Failed to read header");
    file.close();
    return false;
  }

  if (memcmp(headerBuf, CPFONT_MAGIC, 8) != 0) {
    LOG_ERR("SDCF", "Invalid magic bytes");
    file.close();
    return false;
  }

  const uint16_t fileVersion = readU16(headerBuf + 8);
  const uint8_t density = cpfont::rasterDensity(headerBuf);
  if (!density) {
    LOG_ERR("SDCF", "Unsupported font version/density: %u/%u", fileVersion, headerBuf[13]);
    file.close();
    return false;
  }
  if (fileVersion >= 5) {
    uint32_t crc = 0xFFFFFFFFu;
    uint8_t chunk[512];
    size_t remaining = file.fileSize() - HEADER_SIZE;
    while (remaining) {
      const size_t count = std::min(remaining, sizeof(chunk));
      if (file.read(chunk, count) != static_cast<int>(count)) {
        file.close();
        return false;
      }
      crc = cpfont::crc32Update(crc, chunk, count);
      remaining -= count;
    }
    if ((crc ^ 0xFFFFFFFFu) != readU32(headerBuf + 14) || !file.seekSet(HEADER_SIZE)) {
      LOG_ERR("SDCF", "Invalid v5 font checksum");
      file.close();
      return false;
    }
  }
  rasterDensity_ = density;
  fileSize_ = file.fileSize();
  if (fileSize_ > UINT32_MAX) {
    file.close();
    return false;
  }
  // V5 header includes a checksum of the TOC and bitmap payload, making the
  // section-cache font ID sensitive to regenerated outlines and raster policy.
  uint32_t hash = fnv1a(headerBuf, HEADER_SIZE);

  bool is2Bit = (readU16(headerBuf + 10) & 1) != 0;

  // Local name `numStyles` instead of `styleCount` to avoid shadowing the
  // member function styleCount() (cppcheck shadowFunction warning).
  uint8_t numStyles = headerBuf[12];
  if (numStyles == 0 || numStyles > MAX_STYLES) {
    LOG_ERR("SDCF", "Invalid style count: %u", numStyles);
    file.close();
    return false;
  }

  // Read style TOC
  for (uint8_t i = 0; i < numStyles; i++) {
    uint8_t tocBuf[STYLE_TOC_ENTRY_SIZE];
    if (file.read(tocBuf, STYLE_TOC_ENTRY_SIZE) != STYLE_TOC_ENTRY_SIZE) {
      LOG_ERR("SDCF", "Failed to read style TOC entry %u", i);
      file.close();
      freeAll();
      return false;
    }

    // Accumulate TOC entry into content hash
    hash = fnv1a(tocBuf, STYLE_TOC_ENTRY_SIZE, hash);

    uint8_t styleId = tocBuf[0];
    if (styleId >= MAX_STYLES) {
      if (fileVersion == 6) {
        freeAll();
        return false;
      }
      LOG_ERR("SDCF", "Invalid styleId %u in TOC", styleId);
      continue;
    }

    auto& s = styles_[styleId];
    if (fileVersion == 6 && s.present) {
      freeAll();
      return false;
    }
    s.present = true;
    s.header.intervalCount = readU32(tocBuf + 4);
    s.header.glyphCount = readU32(tocBuf + 8);
    s.header.advanceY = tocBuf[12];
    s.header.ascender = readI16(tocBuf + 13);
    s.header.descender = readI16(tocBuf + 15);
    s.header.kernLeftEntryCount = readU16(tocBuf + 17);
    s.header.kernRightEntryCount = readU16(tocBuf + 19);
    s.header.kernLeftClassCount = tocBuf[21];
    s.header.kernRightClassCount = tocBuf[22];
    s.header.ligaturePairCount = tocBuf[23];
    s.header.is2Bit = is2Bit;

    // Sanity-check counts to reject malformed files before allocating
    static constexpr uint32_t MAX_INTERVALS = 4096;
    static constexpr uint32_t MAX_GLYPHS = 65536;
    if (s.header.intervalCount > MAX_INTERVALS || s.header.glyphCount > MAX_GLYPHS) {
      if (fileVersion == 6) {
        freeAll();
        return false;
      }
      LOG_ERR("SDCF", "Style %u: unreasonable counts (intervals=%u, glyphs=%u)", styleId, s.header.intervalCount,
              s.header.glyphCount);
      s.present = false;
      continue;
    }

    s.caps.offset = fileVersion == 6 ? readU32(tocBuf + 28) : 0;
    uint32_t dataOffset = readU32(tocBuf + 24);
    if (fileVersion == 6 &&
        (tocBuf[1] || tocBuf[2] || tocBuf[3] || dataOffset < HEADER_SIZE + numStyles * STYLE_TOC_ENTRY_SIZE)) {
      freeAll();
      return false;
    }
    if (!computeStyleFileOffsets(s, dataOffset, file.fileSize())) {
      LOG_ERR("SDCF", "Style %u: metadata out of bounds", styleId);
      file.close();
      freeAll();
      return false;
    }
  }

  styleCount_ = numStyles;
  contentHash_ = hash;

  // Load full intervals into RAM for each present style
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    auto& s = styles_[i];
    if (!s.present) continue;

    // Compared against the file BEFORE allocating: de-duplicating after the read would still
    // need both copies resident at once, and that peak is what fails on a tight heap.
    s.intervalsOwner = findIdenticalIntervals(i, file);
    if (s.intervalsOwner >= 0) {
      s.fullIntervals = styles_[s.intervalsOwner].fullIntervals;
    } else {
      s.fullIntervals = new (std::nothrow) EpdUnicodeInterval[s.header.intervalCount];
      if (!s.fullIntervals) {
        LOG_ERR("SDCF", "Failed to allocate %u intervals for style %u", s.header.intervalCount, i);
        file.close();
        freeAll();
        return false;
      }

      if (!file.seekSet(s.intervalsFileOffset)) {
        LOG_ERR("SDCF", "Failed to seek to intervals for style %u", i);
        file.close();
        freeAll();
        return false;
      }
      size_t intervalsBytes = s.header.intervalCount * sizeof(EpdUnicodeInterval);
      if (file.read(reinterpret_cast<uint8_t*>(s.fullIntervals), intervalsBytes) != static_cast<int>(intervalsBytes)) {
        LOG_ERR("SDCF", "Failed to read intervals for style %u", i);
        file.close();
        freeAll();
        return false;
      }
    }

    // Validate ordinary maps in every version before kerning can index the
    // matrix; V6 also validates its glyph and authored-capital feature tables.
    if (!validateKernClassMaps(s, file) || (fileVersion == 6 && !validateV6Style(s, file))) {
      freeAll();
      return false;
    }

    // Initialize stub data
    memset(&s.stubData, 0, sizeof(s.stubData));
    s.stubData.advanceY = s.header.advanceY;
    s.stubData.ascender = s.header.ascender;
    s.stubData.descender = s.header.descender;
    s.stubData.is2Bit = s.header.is2Bit;
    s.stubData.wideGlyphs = rasterDensity_ == 2;

    s.epdFont.data = &s.stubData;
    applyGlyphMissCallback(i);
  }

  file.close();
  loaded_ = true;

  LOG_DBG("SDCF", "Loaded: %s (v%u, %u styles)", path, fileVersion, styleCount_);
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    const auto& h = styles_[i].header;
    LOG_DBG("SDCF", "  style[%u]: %u intervals, %u glyphs, advY=%u, asc=%d, desc=%d, kernL=%u, kernR=%u, ligs=%u", i,
            h.intervalCount, h.glyphCount, h.advanceY, h.ascender, h.descender, h.kernLeftEntryCount,
            h.kernRightEntryCount, h.ligaturePairCount);
  }
  return true;
}

// --- Load from mmap ---

// Helper: inline read of a little-endian uint16_t / int16_t / uint32_t from a byte pointer
// (same helpers as the file-based loader above)
static inline uint16_t mmapU16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static inline int16_t mmapI16(const uint8_t* p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); }
static inline uint32_t mmapU32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

bool SdCardFont::loadFromMmap(const uint8_t* base, size_t size, const char* sdPath) {
  freeAll();

  if (!base || size < HEADER_SIZE) {
    LOG_ERR("SDCF", "loadFromMmap: invalid pointer or size %u", static_cast<unsigned>(size));
    return false;
  }

  // Validate magic and version
  if (memcmp(base, CPFONT_MAGIC, 8) != 0) {
    LOG_ERR("SDCF", "loadFromMmap: invalid magic bytes");
    return false;
  }
  const uint16_t fileVersion = mmapU16(base + 8);
  const uint8_t density = cpfont::rasterDensity(base);
  if (!density) {
    LOG_ERR("SDCF", "loadFromMmap: unsupported version/density %u/%u", fileVersion, base[13]);
    return false;
  }
  if (fileVersion >= 5 &&
      (cpfont::crc32Update(0xFFFFFFFFu, base + HEADER_SIZE, size - HEADER_SIZE) ^ 0xFFFFFFFFu) != readU32(base + 14)) {
    LOG_ERR("SDCF", "loadFromMmap: invalid v5 font checksum");
    return false;
  }
  rasterDensity_ = density;
  if (size > UINT32_MAX) return false;
  fileSize_ = size;
  mmapDataBase_ = base;  // Also makes cleanup safe if a later mapped table fails validation.

  // Content hash: accumulate global header
  uint32_t hash = fnv1a(base, HEADER_SIZE);

  const bool is2Bit = (mmapU16(base + 10) & 1) != 0;
  const uint8_t numStyles = base[12];
  if (numStyles == 0 || numStyles > MAX_STYLES) {
    LOG_ERR("SDCF", "loadFromMmap: invalid style count %u", numStyles);
    return false;
  }

  // Parse style TOC
  for (uint8_t i = 0; i < numStyles; i++) {
    const uint8_t* tocEntry = base + HEADER_SIZE + i * STYLE_TOC_ENTRY_SIZE;
    if (tocEntry + STYLE_TOC_ENTRY_SIZE > base + size) {
      LOG_ERR("SDCF", "loadFromMmap: TOC entry %u out of bounds", i);
      freeAll();
      return false;
    }
    hash = fnv1a(tocEntry, STYLE_TOC_ENTRY_SIZE, hash);

    const uint8_t styleId = tocEntry[0];
    if (styleId >= MAX_STYLES) {
      if (fileVersion == 6) {
        freeAll();
        return false;
      }
      LOG_ERR("SDCF", "loadFromMmap: invalid styleId %u in TOC", styleId);
      continue;
    }

    auto& s = styles_[styleId];
    if (fileVersion == 6 && s.present) {
      freeAll();
      return false;
    }
    s.present = true;
    s.header.intervalCount = mmapU32(tocEntry + 4);
    s.header.glyphCount = mmapU32(tocEntry + 8);
    s.header.advanceY = tocEntry[12];
    s.header.ascender = mmapI16(tocEntry + 13);
    s.header.descender = mmapI16(tocEntry + 15);
    s.header.kernLeftEntryCount = mmapU16(tocEntry + 17);
    s.header.kernRightEntryCount = mmapU16(tocEntry + 19);
    s.header.kernLeftClassCount = tocEntry[21];
    s.header.kernRightClassCount = tocEntry[22];
    s.header.ligaturePairCount = tocEntry[23];
    s.header.is2Bit = is2Bit;

    static constexpr uint32_t MAX_INTERVALS = 4096;
    static constexpr uint32_t MAX_GLYPHS = 65536;
    if (s.header.intervalCount > MAX_INTERVALS || s.header.glyphCount > MAX_GLYPHS) {
      if (fileVersion == 6) {
        freeAll();
        return false;
      }
      LOG_ERR("SDCF", "loadFromMmap: style %u unreasonable counts", styleId);
      s.present = false;
      continue;
    }

    s.caps.offset = fileVersion == 6 ? readU32(tocEntry + 28) : 0;
    const uint32_t dataOffset = mmapU32(tocEntry + 24);
    if (fileVersion == 6 &&
        (tocEntry[1] || tocEntry[2] || tocEntry[3] || dataOffset < HEADER_SIZE + numStyles * STYLE_TOC_ENTRY_SIZE)) {
      freeAll();
      return false;
    }
    if (!computeStyleFileOffsets(s, dataOffset, size)) {
      LOG_ERR("SDCF", "loadFromMmap: style %u metadata out of bounds", styleId);
      freeAll();
      return false;
    }
  }

  styleCount_ = numStyles;
  contentHash_ = hash;

  // Copy metadata from the mmap region into heap-allocated, naturally-aligned
  // buffers. A direct reinterpret_cast into flash would be faster but is unsafe
  // on ESP32-C3 (RISC-V): section offsets are not guaranteed 4-byte aligned for
  // styles ≥1 (kern-class entries are 3 bytes each, kern matrix is byte-granularity),
  // so casting to EpdUnicodeInterval* / EpdLigaturePair* (both contain uint32_t
  // fields) would cause LoadAccessFault. We copy once at load time; subsequent
  // accesses are safe via naturally-aligned heap pointers.
  // metadataOwned_ = true so freeStyleAll() will delete[] these arrays.
  // mmapDataBase_ is kept for kerning matrices (read as raw bytes, including
  // LE int16 v5 entries) and packed EpdGlyph records. Later styles can start at
  // any byte offset after variable-length matrices and bitmaps. EpdGlyph has
  // explicit alignment 1, so mapped metric access does not assume alignment.
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    auto& s = styles_[i];
    if (!s.present) continue;

    // Copy fullIntervals into a heap buffer (alignment-safe on RISC-V).
    const size_t intervalsSz = s.header.intervalCount * sizeof(EpdUnicodeInterval);
    if (s.intervalsFileOffset + intervalsSz > size) {
      LOG_ERR("SDCF", "loadFromMmap: intervals for style %u out of bounds", i);
      freeAll();
      return false;
    }
    s.intervalsOwner = findIdenticalIntervals(i, base + s.intervalsFileOffset);
    if (s.intervalsOwner >= 0) {
      s.fullIntervals = styles_[s.intervalsOwner].fullIntervals;
    } else {
      s.fullIntervals = new (std::nothrow) EpdUnicodeInterval[s.header.intervalCount];
      if (!s.fullIntervals) {
        LOG_ERR("SDCF", "loadFromMmap: OOM for intervals style %u", i);
        freeAll();
        return false;
      }
      memcpy(s.fullIntervals, base + s.intervalsFileOffset, intervalsSz);
    }

    // Kern class tables: EpdKernClassEntry is __attribute__((packed)) with only
    // uint16_t + uint8_t members. GCC emits byte-load sequences for all member
    // accesses on packed structs, so aliasing flash directly is safe on RISC-V.
    // No heap copy needed — saves ~6 KB for a typical 4-style Latin font.
    if (s.header.kernLeftEntryCount > 0) {
      const size_t leftSz = s.header.kernLeftEntryCount * sizeof(EpdKernClassEntry);
      const size_t rightSz = s.header.kernRightEntryCount * sizeof(EpdKernClassEntry);
      if (s.kernLeftFileOffset + leftSz + rightSz > size) {
        LOG_ERR("SDCF", "loadFromMmap: kern tables for style %u out of bounds", i);
        freeAll();
        return false;
      }
      s.kernLeftClasses = reinterpret_cast<EpdKernClassEntry*>(const_cast<uint8_t*>(base + s.kernLeftFileOffset));
      s.kernRightClasses = reinterpret_cast<EpdKernClassEntry*>(const_cast<uint8_t*>(base + s.kernRightFileOffset));
      s.kernClassesLoaded = true;
    }

    // Copy ligature pairs (EpdLigaturePair contains uint32_t — needs heap alignment).
    if (s.header.ligaturePairCount > 0) {
      const size_t ligSz = s.header.ligaturePairCount * sizeof(EpdLigaturePair);
      if (s.ligatureFileOffset + ligSz > size) {
        LOG_ERR("SDCF", "loadFromMmap: ligature table for style %u out of bounds", i);
        freeAll();
        return false;
      }
      s.ligaturePairs = new (std::nothrow) EpdLigaturePair[s.header.ligaturePairCount];
      if (!s.ligaturePairs) {
        LOG_ERR("SDCF", "loadFromMmap: OOM for ligature pairs style %u", i);
        freeAll();
        return false;
      }
      memcpy(s.ligaturePairs, base + s.ligatureFileOffset, ligSz);
      s.ligLoaded = true;
    }

    FsFile featureFile;  // readAt uses the mmap bytes and never opens this handle.
    if (!validateKernClassMaps(s, featureFile) || (fileVersion == 6 && !validateV6Style(s, featureFile))) {
      freeAll();
      return false;
    }

    // Initialize stub data
    memset(&s.stubData, 0, sizeof(s.stubData));
    s.stubData.advanceY = s.header.advanceY;
    s.stubData.ascender = s.header.ascender;
    s.stubData.descender = s.header.descender;
    s.stubData.is2Bit = s.header.is2Bit;
    s.stubData.wideGlyphs = rasterDensity_ == 2;
    s.stubData.ligaturePairs = s.ligaturePairs;
    s.stubData.ligaturePairCount = s.header.ligaturePairCount;

    s.epdFont.data = &s.stubData;
    applyGlyphMissCallback(i);
  }

  // Retain optional source provenance. readAt() serves overflow glyph metrics
  // and bitmaps directly from this mapping, without opening the SD source.
  if (sdPath && strlen(sdPath) < sizeof(filePath_)) {
    strncpy(filePath_, sdPath, sizeof(filePath_) - 1);
    filePath_[sizeof(filePath_) - 1] = '\0';
  }
  // Metadata was copied into heap arrays above — owned, must be delete[]'d on free.
  // mmapDataBase_ is kept for the kern MATRIX and glyph array (byte-safe mmap reads).
  metadataOwned_ = true;
  mmapDataBase_ = base;
  loaded_ = true;

  LOG_DBG("SDCF", "Loaded from mmap: %u styles, size=%u", styleCount_, static_cast<unsigned>(size));
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    const auto& h = styles_[i].header;
    LOG_DBG("SDCF", "  style[%u]: %u intervals, %u glyphs, advY=%u, asc=%d, desc=%d", i, h.intervalCount, h.glyphCount,
            h.advanceY, h.ascender, h.descender);
  }
  return true;
}

// --- Codepoint lookup ---

int32_t SdCardFont::findGlobalGlyphIndex(const PerStyle& s, uint32_t codepoint) const {
  int left = 0;
  int right = static_cast<int>(s.header.intervalCount) - 1;
  while (left <= right) {
    int mid = left + (right - left) / 2;
    const auto& interval = s.fullIntervals[mid];
    if (codepoint < interval.first) {
      right = mid - 1;
    } else if (codepoint > interval.last) {
      left = mid + 1;
    } else {
      return static_cast<int32_t>(interval.offset + (codepoint - interval.first));
    }
  }
  return -1;
}

// --- Prewarm ---

int SdCardFont::prewarm(const char* utf8Text, uint8_t styleMask, bool metadataOnly, bool loadKernLigatureData) {
  if (!loaded_) return -1;

  unsigned long startMs = millis();

  // Step 1: Extract unique codepoints from UTF-8 text (shared across all styles).
  // Dedup uses O(n^2) linear scan — worst case is MAX_PAGE_GLYPHS (512) unique codepoints
  // = ~131K comparisons, but in practice pages contain far fewer unique codepoints so the
  // actual cost is much lower. This is dwarfed by SD I/O that follows. Alternatives (hash
  // set, bitmap) exceed the 256-byte stack limit or add template bloat.
  // Heap-allocated: MAX_PAGE_GLYPHS * 4 = 2048 bytes, too large for stack (limit < 256 bytes)
  std::unique_ptr<uint32_t[]> codepoints(new (std::nothrow) uint32_t[MAX_PAGE_GLYPHS]);
  if (!codepoints) {
    LOG_ERR("SDCF", "Failed to allocate codepoint buffer (%u bytes)", MAX_PAGE_GLYPHS * 4);
    return -1;
  }
  uint32_t cpCount = 0;

  const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8Text);
  while (*p && cpCount < MAX_PAGE_GLYPHS) {
    uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;

    bool found = false;
    for (uint32_t i = 0; i < cpCount; i++) {
      if (codepoints[i] == cp) {
        found = true;
        break;
      }
    }
    if (!found) {
      codepoints[cpCount++] = cp;
    }
  }

  // Always include the replacement character
  {
    bool hasReplacement = false;
    for (uint32_t i = 0; i < cpCount; i++) {
      if (codepoints[i] == REPLACEMENT_GLYPH) {
        hasReplacement = true;
        break;
      }
    }
    if (!hasReplacement && cpCount < MAX_PAGE_GLYPHS) {
      codepoints[cpCount++] = REPLACEMENT_GLYPH;
    }
  }

  // Add ligature output codepoints from all styles being prewarmed.
  // Load ligature metadata when either doing a full prewarm or when
  // metadata-only layout measurement needs applyLigatures()/getKerning().
  if (!metadataOnly || loadKernLigatureData) {
    for (uint8_t si = 0; si < MAX_STYLES; si++) {
      if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
      auto& s = styles_[si];

      loadStyleKernLigatureData(s, /*ligatureOnly=*/true);
      if (s.ligaturePairs && s.header.ligaturePairCount > 0) {
        for (uint8_t li = 0; li < s.header.ligaturePairCount && cpCount < MAX_PAGE_GLYPHS; li++) {
          uint32_t leftCp = s.ligaturePairs[li].pair >> 16;
          uint32_t rightCp = s.ligaturePairs[li].pair & 0xFFFF;
          uint32_t outCp = s.ligaturePairs[li].ligatureCp;

          bool hasLeft = false, hasRight = false;
          for (uint32_t i = 0; i < cpCount; i++) {
            if (codepoints[i] == leftCp) hasLeft = true;
            if (codepoints[i] == rightCp) hasRight = true;
            if (hasLeft && hasRight) break;
          }
          if (!hasLeft || !hasRight) continue;

          bool hasOut = false;
          for (uint32_t i = 0; i < cpCount; i++) {
            if (codepoints[i] == outCp) {
              hasOut = true;
              break;
            }
          }
          if (!hasOut) {
            codepoints[cpCount++] = outCp;
          }
        }
      }
    }
  }

  // Sort codepoints for ordered interval building
  uint32_t* const cpBase = codepoints.get();
  std::sort(cpBase, cpBase + cpCount);

  // Prewarm each requested style
  int totalMissed = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
    int missedForStyle = prewarmStyle(si, cpBase, cpCount, metadataOnly, loadKernLigatureData);

    // The bitmap arena is a single contiguous block, so on a fragmented heap it can fail with
    // plenty of free bytes -- and the whole style was being dropped for it, which costs every
    // glyph on the page rather than the few that would not fit. Retry with the largest prefix
    // the biggest free block can hold, halving if variable-width glyphs made that estimate
    // optimistic. Ported from crosspoint-reader PR #3126 (Sung-jin Brian Hong
    // <serialx@serialx.net>).
    if (missedForStyle == PREWARM_ARENA_TOO_LARGE) {
      const uint32_t perGlyph = styles_[si].measuredBytesPerGlyph > 0 ? styles_[si].measuredBytesPerGlyph : 1;
      const uint32_t maxAlloc = ESP.getMaxAllocHeap();
      // Leave working headroom outside the arena: the read order and mappings are still
      // allocated after it inside prewarmStyle.
      const uint32_t arenaBytes = maxAlloc > PREWARM_MAX_ALLOC_RESERVE ? maxAlloc - PREWARM_MAX_ALLOC_RESERVE : 0;
      uint32_t fit = arenaBytes / perGlyph;
      if (fit > cpCount) fit = cpCount;
      while (fit > 0) {
        LOG_DBG("SDCF", "Arena retry: %u -> %u glyphs (%uB/glyph, maxAlloc=%u)", cpCount, fit, perGlyph, maxAlloc);
        missedForStyle = prewarmStyle(si, cpBase, fit, metadataOnly, loadKernLigatureData);
        if (missedForStyle != PREWARM_ARENA_TOO_LARGE) break;
        fit /= 2;
      }
      if (missedForStyle == PREWARM_ARENA_TOO_LARGE) {
        missedForStyle = static_cast<int>(cpCount);  // nothing resident, same as before
      } else {
        missedForStyle += static_cast<int>(cpCount - fit);  // the dropped suffix is missing too
      }
    }
    totalMissed += missedForStyle;
  }

  stats_.prewarmTotalMs = millis() - startMs;
  return totalMissed;
}

// Returns true iff every codepoint in `codepoints[0..cpCount)` is already covered
// by the style's current miniIntervals. O(cpCount * log intervalCount).
bool SdCardFont::allCpsCovered(const PerStyle& s, const uint32_t* codepoints, uint32_t cpCount) {
  if (s.miniIntervalCount == 0) return false;
  for (uint32_t i = 0; i < cpCount; i++) {
    const uint32_t cp = codepoints[i];
    // Binary search miniIntervals for the range containing cp.
    int lo = 0, hi = static_cast<int>(s.miniIntervalCount) - 1;
    bool found = false;
    while (lo <= hi) {
      int mid = (lo + hi) / 2;
      const auto& iv = s.miniIntervals[mid];
      if (cp < iv.first) {
        hi = mid - 1;
      } else if (cp > iv.last) {
        lo = mid + 1;
      } else {
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  return true;
}

int SdCardFont::prewarmStyle(uint8_t styleIdx, const uint32_t* codepoints, uint32_t cpCount, bool metadataOnly,
                             bool loadKernLigatureData) {
  auto& s = styles_[styleIdx];

  // Mapped metadata can cover the entire font without allocating a glyph or
  // kerning table. Switching from a FULL mini cache must restore those full
  // tables too: a newly measured word may use classes absent from that page.
  if (mmapDataBase_ && metadataOnly) {
    const auto* mappedGlyphs = reinterpret_cast<const EpdGlyph*>(mmapDataBase_ + s.glyphsFileOffset);
    if (s.miniMode != PerStyle::MiniMode::METADATA || s.miniData.glyph != mappedGlyphs) {
      freeStyleMiniData(s);
      s.miniData.intervals = s.fullIntervals;
      s.miniData.intervalCount = s.header.intervalCount;
      s.miniData.glyph = mappedGlyphs;
      s.miniData.advanceY = s.header.advanceY;
      s.miniData.ascender = s.header.ascender;
      s.miniData.descender = s.header.descender;
      s.miniData.is2Bit = s.header.is2Bit;
      s.miniData.wideGlyphs = rasterDensity_ == 2;
      s.miniData.glyphMissHandler = &SdCardFont::onGlyphMiss;
      s.miniData.glyphMissCtx = &overflowCtx_[styleIdx];
      s.miniData.capsGlyph = s.stubData.capsGlyph;
      s.miniData.alternateKerning = s.stubData.alternateKerning;
      s.epdFont.data = &s.miniData;
      s.miniMode = PerStyle::MiniMode::METADATA;
    }
    // Classes and ligatures are already resident or mapped. Wiring them here
    // costs no SD reads, even when the caller did not request them explicitly.
    applyKernLigaturePointers(s, s.miniData, /*mapped=*/true);
    return 0;
  }

  // ---- Fast-path coverage check (SD fonts) ----
  // If the existing cache already covers all requested cps in a compatible mode,
  // there's nothing to do. This is the dominant case during pagination after the
  // first paragraph has populated the metadata cache.
  if (s.miniMode != PerStyle::MiniMode::NONE && allCpsCovered(s, codepoints, cpCount)) {
    // For metadata-only calls, METADATA or FULL cache both satisfy layout queries.
    // For full (bitmap) calls, only FULL satisfies — METADATA lacks bitmap data.
    if (metadataOnly || s.miniMode == PerStyle::MiniMode::FULL) {
      if (!metadataOnly || loadKernLigatureData) {
        FsFile file;
        const bool ready = ensureCachedKernMatrix(s, file);
        if (file) file.close();
        // Rewire even after a failed build: do not leave pointers into freed
        // mini tables. A later call can retry because miniKernReady stays false.
        applyKernLigaturePointers(s, s.miniData);
        if (!ready) return static_cast<int>(cpCount);
      }
      return 0;
    }
  }

  // ---- Decide merge vs rebuild ----
  // Merge mode: extend the existing METADATA cache with the new cps without
  // re-reading already-loaded glyphs. Only safe when:
  //   - request is metadata-only (no bitmap data needed)
  //   - existing mode is METADATA
  //   - merged total stays within MAX_PAGE_GLYPHS
  // In all other cases we fall back to today's full-rebuild behavior (which is
  // also what happens for full bitmap prewarms — those are page-scoped).
  const bool tryMerge =
      metadataOnly && s.miniMode == PerStyle::MiniMode::METADATA && s.miniGlyphs != nullptr && s.miniGlyphCount > 0;

  // Map codepoints to global glyph indices for this style
  struct CpGlyphMapping {
    uint32_t codepoint;
    int32_t globalIndex;
  };

  // Worst-case allocation: existing + new (for the merge path) or just new.
  const uint32_t mappingCapacity = tryMerge ? (s.miniGlyphCount + cpCount) : cpCount;
  CpGlyphMapping* mappings = new (std::nothrow) CpGlyphMapping[mappingCapacity];
  if (!mappings) {
    LOG_ERR("SDCF", "Failed to allocate mapping array for style %u", styleIdx);
    return static_cast<int>(cpCount);
  }

  uint32_t validCount = 0;

  // For merge: seed mappings with already-loaded cps + their global indices,
  // recovered by walking miniIntervals and re-resolving via fullIntervals.
  // (We don't store globalIndex per mini-glyph; recomputing it is a binary
  // search per loaded cp, cheap relative to SD I/O.)
  if (tryMerge) {
    for (uint32_t iv = 0; iv < s.miniIntervalCount; iv++) {
      const auto& interval = s.miniIntervals[iv];
      for (uint32_t cp = interval.first; cp <= interval.last; cp++) {
        int32_t gIdx = findGlobalGlyphIndex(s, cp);
        if (gIdx < 0) continue;  // shouldn't happen; defensive
        mappings[validCount].codepoint = cp;
        mappings[validCount].globalIndex = gIdx;
        validCount++;
      }
    }
  }

  // Mark where the existing-glyph block ends; new cps follow.
  const uint32_t existingCount = validCount;

  // Add new requested cps that aren't already in the existing block.
  for (uint32_t i = 0; i < cpCount; i++) {
    const uint32_t cp = codepoints[i];

    // Skip cps already in the existing block (only relevant in merge mode).
    if (existingCount > 0) {
      bool dup = false;
      // Existing cps are sorted; binary search.
      int lo = 0, hi = static_cast<int>(existingCount) - 1;
      while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (mappings[mid].codepoint < cp) {
          lo = mid + 1;
        } else if (mappings[mid].codepoint > cp) {
          hi = mid - 1;
        } else {
          dup = true;
          break;
        }
      }
      if (dup) continue;
    }

    int32_t idx = findGlobalGlyphIndex(s, cp);
    if (idx >= 0) {
      if (validCount >= MAX_PAGE_GLYPHS) {
        LOG_DBG("SDCF", "Cumulative cap (%u glyphs) reached for style %u; dropping merge cache", MAX_PAGE_GLYPHS,
                styleIdx);
        // Soft cap: discard the accumulated cache and fall back to rebuilding
        // with just the new request set. Caller's text will be covered;
        // already-loaded but no-longer-requested cps are lost.
        delete[] mappings;
        freeStyleMiniData(s);
        // Recurse with rebuild semantics by clearing tryMerge state and trying
        // again — implemented as inline reset below.
        return prewarmStyle(styleIdx, codepoints, cpCount, metadataOnly, loadKernLigatureData);
      }
      mappings[validCount].codepoint = cp;
      mappings[validCount].globalIndex = idx;
      validCount++;
    }
  }
  // Sort the full mappings array by codepoint (existing block is already sorted,
  // so std::sort on a partially sorted array is fast in practice; insertion-sort
  // would be optimal but std::sort is fine for our sizes).
  if (validCount > 0) {
    std::sort(mappings, mappings + validCount,
              [](const CpGlyphMapping& a, const CpGlyphMapping& b) { return a.codepoint < b.codepoint; });
  }

  // Count cps that are *newly* missing this accumulation cycle (i.e. not present
  // in mappings[] AND not previously reported). Already-reported misses are
  // suppressed so the same 4 special chars don't spam the log every paragraph.
  // `mappings[]` is sorted by codepoint after the std::sort above, enabling
  // O(log validCount) lookup per requested cp.
  int missed = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    const uint32_t cp = codepoints[i];
    int lo = 0, hi = static_cast<int>(validCount) - 1;
    bool found = false;
    while (lo <= hi) {
      int mid = (lo + hi) / 2;
      if (mappings[mid].codepoint < cp) {
        lo = mid + 1;
      } else if (mappings[mid].codepoint > cp) {
        hi = mid - 1;
      } else {
        found = true;
        break;
      }
    }
    if (found) continue;

    // cp wasn't resolved. Check if we've already reported it this cycle.
    bool alreadyReported = false;
    for (uint8_t r = 0; r < s.reportedMissCount; r++) {
      if (s.reportedMisses[r] == cp) {
        alreadyReported = true;
        break;
      }
    }
    if (alreadyReported) continue;

    if (s.reportedMissCount < PerStyle::MAX_REPORTED_MISSES) {
      s.reportedMisses[s.reportedMissCount++] = cp;
    }
    missed++;
  }

  if (validCount == 0) {
    freeStyleMiniData(s);
    delete[] mappings;
    s.epdFont.data = &s.stubData;
    return missed;
  }

  // Stash the old miniGlyphs so we can copy already-loaded entries by codepoint
  // before freeing them. (For merge path; nullptr when not merging.)
  EpdGlyph* oldGlyphs = tryMerge ? s.miniGlyphs : nullptr;
  EpdUnicodeInterval* oldIntervals = tryMerge ? s.miniIntervals : nullptr;
  uint32_t oldIntervalCount = tryMerge ? s.miniIntervalCount : 0;
  // Detach so freeStyleMiniData doesn't delete them yet.
  if (tryMerge) {
    s.miniIntervals = nullptr;
    s.miniGlyphs = nullptr;
    s.miniIntervalCount = 0;
    s.miniGlyphCount = 0;
  }

  // freeStyleMiniData wipes everything, including miniBitmap (which is nullptr
  // in metadata mode anyway). Safe in either path.
  freeStyleMiniData(s);

  uint32_t intervalCapacity = validCount;
  s.miniIntervals = new (std::nothrow) EpdUnicodeInterval[intervalCapacity];
  if (!s.miniIntervals) {
    LOG_ERR("SDCF", "Failed to allocate mini intervals for style %u", styleIdx);
    delete[] oldGlyphs;
    delete[] oldIntervals;
    delete[] mappings;
    return static_cast<int>(cpCount);
  }

  s.miniIntervalCount = 0;
  uint32_t rangeStart = 0;
  for (uint32_t i = 1; i <= validCount; i++) {
    if (i == validCount || mappings[i].codepoint != mappings[i - 1].codepoint + 1) {
      s.miniIntervals[s.miniIntervalCount].first = mappings[rangeStart].codepoint;
      s.miniIntervals[s.miniIntervalCount].last = mappings[i - 1].codepoint;
      s.miniIntervals[s.miniIntervalCount].offset = rangeStart;
      s.miniIntervalCount++;
      rangeStart = i;
    }
  }

  // Allocate mini glyph array
  s.miniGlyphCount = validCount;
  s.miniGlyphs = new (std::nothrow) EpdGlyph[s.miniGlyphCount];
  if (!s.miniGlyphs) {
    LOG_ERR("SDCF", "Failed to allocate mini glyphs for style %u", styleIdx);
    delete[] oldGlyphs;
    delete[] oldIntervals;
    delete[] mappings;
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }

  // Build a tracking array of which mappings still need SD I/O. For the merge
  // path, copy already-loaded glyph metadata from oldGlyphs first; remaining
  // entries get read from SD below.
  bool* needsRead = new (std::nothrow) bool[validCount];
  if (!needsRead) {
    LOG_ERR("SDCF", "Failed to allocate needsRead array for style %u", styleIdx);
    delete[] oldGlyphs;
    delete[] oldIntervals;
    delete[] mappings;
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }
  for (uint32_t i = 0; i < validCount; i++) needsRead[i] = true;

  if (tryMerge && oldGlyphs && oldIntervals) {
    // For each cp in the new merged set, look it up in the old miniIntervals to
    // see if we already have its EpdGlyph. If yes, copy it over and skip the read.
    for (uint32_t i = 0; i < validCount; i++) {
      const uint32_t cp = mappings[i].codepoint;
      // Binary search oldIntervals for cp.
      int lo = 0, hi = static_cast<int>(oldIntervalCount) - 1;
      while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const auto& iv = oldIntervals[mid];
        if (cp < iv.first) {
          hi = mid - 1;
        } else if (cp > iv.last) {
          lo = mid + 1;
        } else {
          s.miniGlyphs[i] = oldGlyphs[iv.offset + (cp - iv.first)];
          needsRead[i] = false;
          break;
        }
      }
    }
  }

  delete[] oldGlyphs;
  delete[] oldIntervals;

  // Count how many SD reads are still needed, and build a sorted read order.
  uint32_t toReadCount = 0;
  for (uint32_t i = 0; i < validCount; i++) {
    if (needsRead[i]) toReadCount++;
  }

  uint32_t* readOrder = nullptr;
  if (toReadCount > 0) {
    readOrder = new (std::nothrow) uint32_t[toReadCount];
    if (!readOrder) {
      LOG_ERR("SDCF", "Failed to allocate read order for style %u", styleIdx);
      delete[] needsRead;
      delete[] mappings;
      freeStyleMiniData(s);
      return static_cast<int>(cpCount);
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < validCount; i++) {
      if (needsRead[i]) readOrder[k++] = i;
    }
    std::sort(readOrder, readOrder + toReadCount,
              [&](uint32_t a, uint32_t b) { return mappings[a].globalIndex < mappings[b].globalIndex; });
  }

  unsigned long sdStart = millis();
  uint32_t seekCount = 0;
  FsFile file;

  if (toReadCount > 0) {
    if (!mmapDataBase_ && !Storage.openFileForRead("SDCF", filePath_, file)) {
      LOG_ERR("SDCF", "Failed to reopen .cpfont for prewarm (style %u)", styleIdx);
      delete[] readOrder;
      delete[] needsRead;
      delete[] mappings;
      freeStyleMiniData(s);
      return static_cast<int>(cpCount);
    }

    // readAt copies mapped metrics directly, so a cached flash font needs no
    // SD source for full prewarm either. For SD it seeks only when necessary;
    // lastReadIndex tracks those nonsequential reads for diagnostics.
    int32_t lastReadIndex = INT32_MIN;
    for (uint32_t i = 0; i < toReadCount; i++) {
      uint32_t mapIdx = readOrder[i];
      int32_t gIdx = mappings[mapIdx].globalIndex;

      uint32_t fileOff = s.glyphsFileOffset + static_cast<uint32_t>(gIdx) * sizeof(EpdGlyph);
      if (!mmapDataBase_ && gIdx != lastReadIndex + 1) seekCount++;
      if (!readAt(file, fileOff, &s.miniGlyphs[mapIdx], sizeof(EpdGlyph))) {
        LOG_ERR("SDCF", "Prewarm: short glyph read (style %u, glyph %d)", styleIdx, gIdx);
        if (file) file.close();
        delete[] readOrder;
        delete[] needsRead;
        delete[] mappings;
        freeStyleMiniData(s);
        return static_cast<int>(cpCount);
      }
      lastReadIndex = gIdx;
    }
  }
  delete[] needsRead;
  delete[] readOrder;
  readOrder = nullptr;

  uint32_t totalBitmapSize = 0;
  bool bitmapsFromMmap = false;

  if (!metadataOnly) {
    // Compute total bitmap size
    for (uint32_t i = 0; i < validCount; i++) {
      totalBitmapSize += s.miniGlyphs[i].dataLength;
    }

    // Flash-mmap fast path: the whole .cpfont is already mapped, and glyph
    // bitmaps are raw bytes with no alignment requirement — the same property
    // that licenses the kern-matrix fast path in buildMiniKernMatrix(). Keep
    // each glyph's dataOffset exactly as read from the file and point miniData
    // at the mapped bitmap section, so the per-page arena is never allocated
    // and no bitmap byte is read from SD. Skipped for pure-SD fonts (mmap
    // unavailable), which keep the copy-into-arena path below.
    if (mmapDataBase_) {
      bitmapsFromMmap = true;
    } else {
      // The metadata pass above only opened the file for cps that needed a
      // metadata read — open it now if it isn't already.
      if (!file) {
        if (!Storage.openFileForRead("SDCF", filePath_, file)) {
          LOG_ERR("SDCF", "Failed to reopen .cpfont for bitmap prewarm (style %u)", styleIdx);
          delete[] mappings;
          freeStyleMiniData(s);
          return static_cast<int>(cpCount);
        }
      }

      // Recorded before the allocation, because the retry in prewarm() needs it precisely when
      // that allocation fails. Rounded up — see the field comment.
      if (validCount > 0) s.measuredBytesPerGlyph = (totalBitmapSize + validCount - 1) / validCount;

      s.miniBitmap = new (std::nothrow) uint8_t[totalBitmapSize > 0 ? totalBitmapSize : 1];
      if (!s.miniBitmap) {
        LOG_ERR("SDCF", "Failed to allocate mini bitmap (%u bytes) for style %u", totalBitmapSize, styleIdx);
        file.close();
        delete[] mappings;
        freeStyleMiniData(s);
        // Not a glyph count: this is one contiguous block, so it can fail with plenty of free
        // heap. Let the caller retry with fewer glyphs instead of losing the whole style.
        return PREWARM_ARENA_TOO_LARGE;
      }

      // Allocate a fresh readOrder covering all validCount glyphs, sorted by
      // bitmap file offset for sequential I/O.
      uint32_t* bitmapOrder = new (std::nothrow) uint32_t[validCount];
      if (!bitmapOrder) {
        LOG_ERR("SDCF", "Failed to allocate bitmap read order for style %u", styleIdx);
        file.close();
        delete[] mappings;
        freeStyleMiniData(s);
        return static_cast<int>(cpCount);
      }
      for (uint32_t i = 0; i < validCount; i++) bitmapOrder[i] = i;
      std::sort(bitmapOrder, bitmapOrder + validCount,
                [&](uint32_t a, uint32_t b) { return s.miniGlyphs[a].dataOffset < s.miniGlyphs[b].dataOffset; });

      uint32_t miniBitmapOffset = 0;
      uint32_t lastBitmapEnd = UINT32_MAX;
      for (uint32_t i = 0; i < validCount; i++) {
        uint32_t mapIdx = bitmapOrder[i];
        EpdGlyph& glyph = s.miniGlyphs[mapIdx];

        if (glyph.dataLength == 0) {
          glyph.dataOffset = miniBitmapOffset;
          continue;
        }

        uint32_t fileOff = s.bitmapFileOffset + glyph.dataOffset;
        if (fileOff != lastBitmapEnd) {
          file.seekSet(fileOff);
          seekCount++;
        }
        if (file.read(s.miniBitmap + miniBitmapOffset, glyph.dataLength) != static_cast<int>(glyph.dataLength)) {
          LOG_ERR("SDCF", "Prewarm: short bitmap read (style %u)", styleIdx);
          file.close();
          delete[] bitmapOrder;
          delete[] mappings;
          freeStyleMiniData(s);
          return static_cast<int>(cpCount);
        }
        lastBitmapEnd = fileOff + glyph.dataLength;

        glyph.dataOffset = miniBitmapOffset;
        miniBitmapOffset += glyph.dataLength;
      }
      delete[] bitmapOrder;
    }
  }

  uint32_t sdTime = millis() - sdStart;
  delete[] mappings;

  // Render and explicitly requested layout kerning use the same adjustments.
  // Metadata caches can accumulate several requests, so build from the entire
  // resident glyph set and reuse it until that set changes. This avoids both
  // zero-kern layout drift and repeated SD reads for already covered words.
  bool kernLigOk = false;
  if (!metadataOnly || loadKernLigatureData) {
    kernLigOk = ensureCachedKernMatrix(s, file);
  }

  if (file) file.close();

  // Populate miniData and swap
  memset(&s.miniData, 0, sizeof(s.miniData));
  // On the mmap fast path miniBitmap stays null and each glyph's dataOffset is
  // still relative to the font's bitmap section, so the base is that section in
  // the flash mapping. EpdFontData::bitmap is a const pointer and built-in fonts
  // already point it at flash rodata, so the draw path is unchanged.
  s.miniData.bitmap = bitmapsFromMmap ? (mmapDataBase_ + s.bitmapFileOffset) : s.miniBitmap;
  s.miniData.glyph = s.miniGlyphs;
  s.miniData.intervals = s.miniIntervals;
  s.miniData.intervalCount = s.miniIntervalCount;
  s.miniData.advanceY = s.header.advanceY;
  s.miniData.ascender = s.header.ascender;
  s.miniData.descender = s.header.descender;
  s.miniData.is2Bit = s.header.is2Bit;
  s.miniData.wideGlyphs = rasterDensity_ == 2;
  if (kernLigOk) {
    applyKernLigaturePointers(s, s.miniData);
  } else if (loadKernLigatureData && s.ligLoaded) {
    s.miniData.ligaturePairs = s.ligaturePairs;
    s.miniData.ligaturePairCount = s.header.ligaturePairCount;
  }
  s.miniData.glyphMissHandler = &SdCardFont::onGlyphMiss;
  s.miniData.glyphMissCtx = &overflowCtx_[styleIdx];
  s.miniData.capsGlyph = s.stubData.capsGlyph;
  s.miniData.alternateKerning = s.stubData.alternateKerning;

  s.epdFont.data = &s.miniData;
  s.miniMode = metadataOnly ? PerStyle::MiniMode::METADATA : PerStyle::MiniMode::FULL;

  // Accumulate stats
  stats_.sdReadTimeMs += sdTime;
  stats_.seekCount += seekCount;
  stats_.uniqueGlyphs += validCount;
  stats_.bitmapBytes += totalBitmapSize;

  if ((!metadataOnly || loadKernLigatureData) && !kernLigOk) {
    // Glyphs remain usable, but signal that the requested layout/render data
    // was not completely prepared rather than silently claiming success.
    return missed + static_cast<int>(cpCount);
  }
  return missed;
}

// --- Cache management ---

void SdCardFont::clearCache() {
  clearOverflow();
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    freeStyleMiniData(styles_[i]);
    styles_[i].reportedMissCount = 0;
    applyGlyphMissCallback(i);
  }
}

void SdCardFont::clearAccumulation() {
  // Same as clearCache() but skips the overflow ring buffer (per-glyph on-demand
  // loads are independent of the cumulative metadata cache). Also resets the
  // miss-report tracker so each section can re-report its missing cps once.
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    freeStyleMiniData(styles_[i]);
    styles_[i].reportedMissCount = 0;
    applyGlyphMissCallback(i);
  }
}

// --- Stats ---

void SdCardFont::logStats(const char* label) {
  // Suppress when this font wasn't touched this phase — FontCacheManager iterates every
  // registered SD font, but only the active one for the page will have non-zero stats.
  if (stats_.prewarmTotalMs == 0 && stats_.sdReadTimeMs == 0 && stats_.seekCount == 0 && stats_.uniqueGlyphs == 0 &&
      stats_.bitmapBytes == 0) {
    return;
  }
  LOG_DBG("SDCF", "[%s] total=%ums sd_read=%ums seeks=%u glyphs=%u bitmap=%u bytes", label, stats_.prewarmTotalMs,
          stats_.sdReadTimeMs, stats_.seekCount, stats_.uniqueGlyphs, stats_.bitmapBytes);
}

void SdCardFont::resetStats() { stats_ = Stats{}; }

// --- Public accessors ---

EpdFont* SdCardFont::getEpdFont(uint8_t style) {
  if (style >= MAX_STYLES || !styles_[style].present) return nullptr;
  return &styles_[style].epdFont;
}

bool SdCardFont::hasStyle(uint8_t style) const { return style < MAX_STYLES && styles_[style].present; }

// --- On-demand glyph loading (overflow buffer) ---

const EpdGlyph* SdCardFont::onGlyphMiss(void* ctx, uint32_t codepoint) {
  auto* oc = static_cast<OverflowContext*>(ctx);
  auto* self = oc->self;
  uint8_t styleIdx = oc->styleIdx;

  if (!self->loaded_ || styleIdx >= MAX_STYLES || !self->styles_[styleIdx].present) return nullptr;
  auto& s = self->styles_[styleIdx];
  const bool alternate = codepoint >= EPD_ALTERNATE_GLYPH_BASE;
  if ((!alternate && !s.fullIntervals) || (alternate && codepoint - EPD_ALTERNATE_GLYPH_BASE >= s.caps.glyphCount))
    return nullptr;

  // Diagnostic (throttled): first few misses are logged verbosely, then every 64th miss.
  s.onDemandMissCount++;
  const bool logDetailedMiss = s.onDemandMissLogged < 12 || (s.onDemandMissCount % 64u) == 0u;
  if (logDetailedMiss) {
    LOG_DBG("SDCF", "onGlyphMiss: U+%04X style %u miniMode=%u miniIntervals=%u bitmap=%p miss#=%lu", codepoint,
            styleIdx, static_cast<uint8_t>(s.miniMode), s.miniIntervalCount, s.miniBitmap, s.onDemandMissCount);
    if (s.onDemandMissLogged == 12) {
      LOG_DBG("SDCF", "onGlyphMiss: suppressing verbose logs for style %u; logging every 64th miss", styleIdx);
    }
    s.onDemandMissLogged++;
  }

  // Check overflow cache first (matching both codepoint and style).
  // Probe all slots: the ring may be sparse after failed insertions.
  for (uint32_t i = 0; i < OVERFLOW_CAPACITY; i++) {
    if (!self->overflow_[i].occupied) {
      continue;
    }
    if (self->overflow_[i].codepoint == codepoint && self->overflow_[i].styleIdx == styleIdx) {
      return &self->overflow_[i].glyph;
    }
  }

  // Look up global glyph index via full intervals
  int32_t globalIdx =
      alternate ? static_cast<int32_t>(codepoint - EPD_ALTERNATE_GLYPH_BASE) : self->findGlobalGlyphIndex(s, codepoint);
  if (globalIdx < 0) return nullptr;

  // Pick overflow slot (ring buffer). Read into temporaries first so the
  // existing slot stays valid if SD I/O fails.
  uint32_t slot = self->overflowNext_;
  const bool overwriteOccupied = self->overflow_[slot].occupied;
  self->overflowNext_ = (slot + 1) % OVERFLOW_CAPACITY;

  // Read into the same bounded overflow ring for SD and mmap, preserving the
  // bitmap identity contract used by GfxRenderer. No alternate bitmap table copy.
  FsFile file;
  EpdGlyph tempGlyph;
  const uint32_t glyphBase = alternate ? s.caps.glyphOffset : s.glyphsFileOffset;
  const uint32_t bitmapBase = alternate ? s.caps.bitmapOffset : s.bitmapFileOffset;
  if (!self->readAt(file, glyphBase + static_cast<uint32_t>(globalIdx) * sizeof(EpdGlyph), &tempGlyph,
                    sizeof(tempGlyph)))
    return nullptr;
  if (alternate &&
      (tempGlyph.dataOffset > s.caps.bitmapBytes || tempGlyph.dataLength > s.caps.bitmapBytes - tempGlyph.dataOffset))
    return nullptr;
  uint8_t* tempBitmap = nullptr;
  if (tempGlyph.dataLength) {
    tempBitmap = new (std::nothrow) uint8_t[tempGlyph.dataLength];
    if (!tempBitmap) return nullptr;
    if (uint64_t(bitmapBase) + tempGlyph.dataOffset > UINT32_MAX ||
        !self->readAt(file, bitmapBase + tempGlyph.dataOffset, tempBitmap, tempGlyph.dataLength)) {
      delete[] tempBitmap;
      return nullptr;
    }
  }

  // All reads succeeded — commit to slot (evict old entry when overwriting an occupied slot).
  if (overwriteOccupied) {
    LOG_DBG("SDCF", "Overflow: evicting U+%04X style %u from slot %u", self->overflow_[slot].codepoint,
            self->overflow_[slot].styleIdx, slot);
    delete[] self->overflow_[slot].bitmap;
  } else if (self->overflowCount_ < OVERFLOW_CAPACITY) {
    self->overflowCount_++;
  }
  self->overflow_[slot].glyph = tempGlyph;
  self->overflow_[slot].bitmap = tempBitmap;
  self->overflow_[slot].codepoint = codepoint;
  self->overflow_[slot].styleIdx = styleIdx;
  self->overflow_[slot].occupied = true;

  LOG_DBG("SDCF", "Overflow: loaded U+%04X style %u on demand (slot %u/%u)", codepoint, styleIdx, slot,
          OVERFLOW_CAPACITY);

  return &self->overflow_[slot].glyph;
}

bool SdCardFont::isOverflowGlyph(const EpdGlyph* glyph) const {
  for (uint32_t i = 0; i < OVERFLOW_CAPACITY; i++) {
    if (!overflow_[i].occupied) {
      continue;
    }
    if (&overflow_[i].glyph == glyph) return true;
  }
  return false;
}

const uint8_t* SdCardFont::getOverflowBitmap(const EpdGlyph* glyph) const {
  for (uint32_t i = 0; i < OVERFLOW_CAPACITY; i++) {
    if (!overflow_[i].occupied) {
      continue;
    }
    if (&overflow_[i].glyph == glyph) {
      return overflow_[i].bitmap;
    }
  }
  return nullptr;
}

SdCardFont* SdCardFont::fromMissCtx(void* ctx) { return static_cast<OverflowContext*>(ctx)->self; }
