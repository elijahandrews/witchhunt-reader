// SdCardFont shares a style's interval table with an earlier style when the two are
// byte-identical, instead of allocating a copy per style. These tests pin the ownership rules
// that make that safe: only the owner frees, a borrower survives unloadMetadata() and is
// re-pointed by reloadMetadata(), and a same-sized but different table is never shared.
// Run under the ASan build to catch a double free or a stale borrowed pointer.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <new>
#include <string>
#include <tuple>
#include <vector>

#include "EpdFontData.h"
#include "HalStorage.h"
#include "SdCardFont.h"

namespace {

// Counts array allocations of exactly one interval table's size. The count is chosen so no
// other allocation in SdCardFont is plausibly the same size.
constexpr uint32_t INTERVALS = 37;
constexpr size_t TABLE_BYTES = INTERVALS * sizeof(EpdUnicodeInterval);
bool countingTables = false;
int tableAllocs = 0;
bool failKernChunk = false;

}  // namespace

void* operator new[](const size_t size) {
  if (countingTables && size == TABLE_BYTES) ++tableAllocs;
  if (void* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](const size_t size, const std::nothrow_t&) noexcept {
  if (failKernChunk && size == 4096) return nullptr;
  if (countingTables && size == TABLE_BYTES) ++tableAllocs;
  return std::malloc(size ? size : 1);
}
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }

namespace {

constexpr uint32_t GLYPHS = INTERVALS * 2;
constexpr uint32_t HEADER = 32;
constexpr uint32_t TOC_ENTRY = 32;

void putU16(std::vector<uint8_t>& b, const size_t at, const uint16_t v) {
  b[at] = v & 0xFF;
  b[at + 1] = v >> 8;
}
void putU32(std::vector<uint8_t>& b, const size_t at, const uint32_t v) {
  for (int i = 0; i < 4; i++) b[at + i] = (v >> (8 * i)) & 0xFF;
}

// A v4 .cpfont with four styles and no kerning or ligatures. Each interval covers two
// codepoints starting at `firstCp[style]` (the last holds U+FFFD), so styles built from the same base have
// byte-identical tables and a style built from another base has the same COUNT but
// different contents.
std::vector<uint8_t> buildFont(const uint32_t (&firstCp)[SdCardFont::MAX_STYLES]) {
  const uint32_t styleBytes = INTERVALS * sizeof(EpdUnicodeInterval) + GLYPHS * sizeof(EpdGlyph);
  const uint32_t dataStart = HEADER + SdCardFont::MAX_STYLES * TOC_ENTRY;
  std::vector<uint8_t> b(dataStart + SdCardFont::MAX_STYLES * styleBytes, 0);

  const char magic[8] = {'C', 'P', 'F', 'O', 'N', 'T', '\0', '\0'};
  std::copy(magic, magic + 8, b.begin());
  putU16(b, 8, 4);   // version
  putU16(b, 10, 0);  // 1-bit
  b[12] = SdCardFont::MAX_STYLES;

  for (uint8_t s = 0; s < SdCardFont::MAX_STYLES; s++) {
    const size_t toc = HEADER + s * TOC_ENTRY;
    const uint32_t data = dataStart + s * styleBytes;
    b[toc] = s;
    putU32(b, toc + 4, INTERVALS);
    putU32(b, toc + 8, GLYPHS);
    b[toc + 12] = 20;  // advanceY
    putU32(b, toc + 24, data);
    for (uint32_t j = 0; j < INTERVALS; j++) {
      const size_t at = data + j * sizeof(EpdUnicodeInterval);
      // The last interval holds U+FFFD, which prewarm() always adds to the set it resolves.
      const uint32_t first = (j == INTERVALS - 1) ? 0xFFFD : firstCp[s] + 3 * j;
      putU32(b, at, first);
      putU32(b, at + 4, first + 1);
      putU32(b, at + 8, 2 * j);
    }
  }
  return b;
}

// Named after the running test: ctest -j runs each test in its own process, and a shared path
// lets one test's "wb" truncate the file while another is still loading it.
std::string writeTemp(const std::vector<uint8_t>& bytes) {
  const auto path = std::filesystem::temp_directory_path() /
                    (std::string("sd_font_intervals_") +
                     std::to_string(std::hash<std::string>{}(
                         std::string(testing::UnitTest::GetInstance()->current_test_info()->test_suite_name()) +
                         testing::UnitTest::GetInstance()->current_test_info()->name())) +
                     ".cpfont");
  FILE* f = std::fopen(path.string().c_str(), "wb");
  EXPECT_NE(f, nullptr);
  std::fwrite(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
  return path.string();
}

// Regular, bold and italic share one table; bold-italic has its own of the same size.
constexpr uint32_t kMixed[SdCardFont::MAX_STYLES] = {'A', 'A', 'A', 'a'};

// Glyphs of style `style` for "A" that the font could not resolve.
int missesForA(SdCardFont& font, const uint8_t style) {
  return font.prewarm("A", static_cast<uint8_t>(1u << style), /*metadataOnly=*/true);
}

class Counting {
 public:
  Counting() {
    tableAllocs = 0;
    countingTables = true;
  }
  ~Counting() { countingTables = false; }
};

TEST(SdFontIntervals, LoadSharesIdenticalTablesOnly) {
  const std::string path = writeTemp(buildFont(kMixed));
  SdCardFont font;
  {
    Counting c;
    ASSERT_TRUE(font.load(path.c_str()));
    EXPECT_EQ(tableAllocs, 2);  // one for styles 0-2, one for style 3
  }
  for (uint8_t s = 0; s < 3; s++) EXPECT_EQ(missesForA(font, s), 0) << "style " << int(s);
  // Same size as the others but a different table: 'A' is not in it. Had it been wrongly
  // aliased to style 0's table, this would resolve.
  EXPECT_EQ(missesForA(font, 3), 1);
}

TEST(SdFontIntervals, UnloadReloadRepointsBorrowers) {
  const std::string path = writeTemp(buildFont(kMixed));
  SdCardFont font;
  ASSERT_TRUE(font.load(path.c_str()));
  for (int round = 0; round < 3; round++) {
    font.clearCache();
    font.unloadMetadata();
    Counting c;
    ASSERT_TRUE(font.reloadMetadata());
    EXPECT_EQ(tableAllocs, 2) << "round " << round;
  }
  font.clearCache();
  for (uint8_t s = 0; s < 3; s++) EXPECT_EQ(missesForA(font, s), 0) << "style " << int(s);
  EXPECT_EQ(missesForA(font, 3), 1);
}

TEST(SdFontIntervals, ReloadIntoAnotherFontFreesCleanly) {
  const std::string path = writeTemp(buildFont(kMixed));
  SdCardFont font;
  ASSERT_TRUE(font.load(path.c_str()));
  font.unloadMetadata();
  // load() starts with freeAll() over a font whose borrowers hold no table: must not free twice.
  ASSERT_TRUE(font.load(path.c_str()));
}

TEST(SdFontIntervals, MmapSharesIdenticalTablesOnly) {
  const std::vector<uint8_t> bytes = buildFont(kMixed);
  const std::string path = writeTemp(bytes);
  SdCardFont font;
  {
    Counting c;
    ASSERT_TRUE(font.loadFromMmap(bytes.data(), bytes.size(), path.c_str()));
    EXPECT_EQ(tableAllocs, 2);
  }
  // An mmap font's metadata prewarm wires each style straight to its full table, so the
  // sharing is visible as pointer identity.
  ASSERT_EQ(font.prewarm("A", 0x0F, /*metadataOnly=*/true), 0);
  const EpdUnicodeInterval* regular = font.getEpdFont(0)->data->intervals;
  ASSERT_NE(regular, nullptr);
  EXPECT_EQ(font.getEpdFont(1)->data->intervals, regular);
  EXPECT_EQ(font.getEpdFont(2)->data->intervals, regular);
  EXPECT_NE(font.getEpdFont(3)->data->intervals, regular);
  EXPECT_EQ(font.getEpdFont(3)->data->intervals[0].first, static_cast<uint32_t>('a'));
}

TEST(SdFontIntervals, AllDifferentSharesNothing) {
  constexpr uint32_t kDistinct[SdCardFont::MAX_STYLES] = {'A', 'a', 0x100, 0x200};
  const std::string path = writeTemp(buildFont(kDistinct));
  SdCardFont font;
  Counting c;
  ASSERT_TRUE(font.load(path.c_str()));
  EXPECT_EQ(tableAllocs, 4);
}

// Write the format explicitly, independently of EpdGlyph's in-memory constructor.
// V4 deliberately has nonzero reserved dimension bytes: old files must ignore them.
void updateChecksum(std::vector<uint8_t>& bytes) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = HEADER; i < bytes.size(); ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0);
  }
  putU32(bytes, 14, crc ^ 0xFFFFFFFFu);
}

std::vector<uint8_t> buildMetricFont(bool v5, bool largeClasses = false) {
  constexpr uint32_t data = HEADER + TOC_ENTRY;
  constexpr uint32_t glyphs = data + 3 * 12;
  constexpr uint32_t left = glyphs + 4 * 16;
  constexpr uint32_t right = left + 6;
  constexpr uint32_t matrix = right + 6;
  const uint16_t classes = largeClasses ? 255 : 2;
  const uint32_t bitmap = matrix + classes * classes * (v5 ? 2 : 1);
  const uint16_t wideBytes = v5 ? (267u * 257u * 2u + 7u) / 8u : 3u;
  std::vector<uint8_t> b(bitmap + wideBytes + 3, 0);
  const char magic[8] = {'C', 'P', 'F', 'O', 'N', 'T', '\0', '\0'};
  std::copy(magic, magic + 8, b.begin());
  putU16(b, 8, v5 ? 5 : 4);
  putU16(b, 10, 1);
  b[12] = 1;
  b[13] = v5 ? 2 : 0;
  putU32(b, HEADER + 4, 3);
  putU32(b, HEADER + 8, 4);
  b[HEADER + 12] = 40;
  putU16(b, HEADER + 17, 2);
  putU16(b, HEADER + 19, 2);
  b[HEADER + 21] = classes;
  b[HEADER + 22] = classes;
  putU32(b, HEADER + 24, data);
  const uint32_t first[] = {'A', 'V', 0xFFFD};
  const uint32_t last[] = {'B', 'V', 0xFFFD};
  const uint32_t index[] = {0, 2, 3};
  for (size_t i = 0; i < 3; ++i) {
    putU32(b, data + 12 * i, first[i]);
    putU32(b, data + 12 * i + 4, last[i]);
    putU32(b, data + 12 * i + 8, index[i]);
  }
  for (size_t i = 0; i < 4; ++i) {
    const size_t at = glyphs + i * 16;
    b[at] = i == 0 ? 11 : 2;
    b[at + 1] = 1;
    putU16(b, at + 2, i == 0 ? 4500 : 48);
    putU16(b, at + 4, static_cast<uint16_t>(-9));
    putU16(b, at + 6, 260);
    putU16(b, at + 8, i == 0 ? wideBytes : 1);
    b[at + 10] = i == 0 ? 1 : 0;
    b[at + 11] = i == 0 ? 1 : 0;
    putU32(b, at + 12, i == 0 ? 0 : wideBytes + i - 1);
  }
  putU16(b, left, 'A');
  b[left + 2] = largeClasses ? 255 : 2;
  putU16(b, left + 3, 'V');
  b[left + 5] = largeClasses ? 9 : 1;
  putU16(b, right, 'A');
  b[right + 2] = largeClasses ? 254 : 1;
  putU16(b, right + 3, 'V');
  b[right + 5] = largeClasses ? 255 : 2;
  const int16_t kern[] = {11, -168, -403, 44};
  const int8_t kernV4[] = {11, -68, -103, 44};
  for (size_t i = 0; i < 4; ++i) {
    if (v5) {
      const size_t entry = largeClasses ? ((i < 2 ? 8u : 254u) * 255u + (i % 2 ? 254u : 253u)) : i;
      putU16(b, matrix + 2 * entry, static_cast<uint16_t>(kern[i]));
    } else
      b[matrix + i] = static_cast<uint8_t>(kernV4[i]);
  }
  std::fill(b.begin() + bitmap, b.end(), 0xE4);
  b[bitmap] = 0x39;
  b[bitmap + wideBytes - 1] = 0xA7;
  b[bitmap + wideBytes] = 0xB1;
  b[bitmap + wideBytes + 1] = 0xC2;
  b[bitmap + wideBytes + 2] = 0xD3;
  if (v5) updateChecksum(b);
  return b;
}

class SdFontVersion : public testing::TestWithParam<std::tuple<bool, bool>> {};

TEST_P(SdFontVersion, MetricsKernBitmapAndOverflowSurviveAllCacheModes) {
  const auto [v5, mmap] = GetParam();
  const auto bytes = buildMetricFont(v5);
  const auto path = writeTemp(bytes);
  SdCardFont font;
  ASSERT_TRUE(mmap ? font.loadFromMmap(bytes.data(), bytes.size(), path.c_str()) : font.load(path.c_str()));
  EXPECT_EQ(font.rasterDensity(), v5 ? 2 : 1);
  EXPECT_FLOAT_EQ(font.rasterScale(), v5 ? 0.5f : 1.0f);
  auto* face = font.getEpdFont();
  ASSERT_NE(face, nullptr);
  auto verifyA = [&] {
    auto glyph = face->getGlyph('A');
    ASSERT_TRUE(glyph);
    EXPECT_EQ(glyph.width, v5 ? 267 : 11);
    EXPECT_EQ(glyph.height, v5 ? 257 : 1);
    EXPECT_EQ(glyph.advanceX, 4500);
    EXPECT_EQ(glyph.left, -9);
    EXPECT_EQ(glyph.top, 260);
  };
  verifyA();  // Stub lookup must preserve wide overflow metrics too.
  auto glyph = face->getGlyph('A');
  ASSERT_TRUE(font.isOverflowGlyph(glyph.sdRecord));
  ASSERT_NE(font.getOverflowBitmap(glyph.sdRecord), nullptr);
  EXPECT_EQ(font.getOverflowBitmap(glyph.sdRecord)[0], 0x39);
  EXPECT_EQ(font.getOverflowBitmap(glyph.sdRecord)[glyph.sdRecord->dataLength - 1], 0xA7);
  ASSERT_EQ(font.prewarm("AV", 1, true, true), 0);
  verifyA();  // Both SD copied metadata and mmap metadata fast path.
  ASSERT_EQ(font.prewarm("AV", 1, false, true), 0);
  verifyA();
  EXPECT_EQ(face->getKerning('A', 'A'), v5 ? -403 : -103);
  EXPECT_EQ(face->getKerning('V', 'V'), v5 ? -168 : -68);
  EXPECT_EQ(face->getKerning('V', 'A'), 11);
  EXPECT_EQ(face->getKerning('A', 'V'), 44);
  glyph = face->getGlyph('A');
  ASSERT_NE(face->data->bitmap, nullptr);
  EXPECT_EQ(face->data->bitmap[glyph.sdRecord->dataOffset], 0x39);
  EXPECT_EQ(face->data->bitmap[glyph.sdRecord->dataOffset + glyph.sdRecord->dataLength - 1], 0xA7);
  font.clearCache();
  ASSERT_EQ(font.prewarm("A", 1, false, true), 0);
  EXPECT_EQ(face->getKerning('A', 'A'), v5 ? -403 : -103);  // Select old row 2, column 1 into a 1x1 mini matrix.
  EXPECT_EQ(face->getKerning('V', 'V'), 0);
  auto b = face->getGlyph('B');
  ASSERT_TRUE(b);
  ASSERT_TRUE(font.isOverflowGlyph(b.sdRecord));
  EXPECT_EQ(font.getOverflowBitmap(b.sdRecord)[0], 0xB1);
  font.clearCache();
  font.unloadMetadata();
  ASSERT_TRUE(font.reloadMetadata());
  ASSERT_EQ(font.prewarm("AV", 1, false, true), 0);
  verifyA();
  EXPECT_EQ(face->getKerning('A', 'A'), v5 ? -403 : -103);
  std::filesystem::remove(path);
}

TEST_P(SdFontVersion, MetadataKerningCoversCumulativeQueriesAndCacheTransitions) {
  const auto [v5, mmap] = GetParam();
  const auto bytes = buildMetricFont(v5);
  const auto path = writeTemp(bytes);
  SdCardFont font;
  ASSERT_TRUE(mmap ? font.loadFromMmap(bytes.data(), bytes.size(), path.c_str()) : font.load(path.c_str()));
  auto* face = font.getEpdFont();
  const int aa = v5 ? -403 : -103, vv = v5 ? -168 : -68;
  auto verifyPairs = [&] {
    EXPECT_EQ(face->getKerning('A', 'A'), aa);
    EXPECT_EQ(face->getKerning('V', 'V'), vv);
    EXPECT_EQ(face->getKerning('A', 'V'), 44);
    EXPECT_EQ(face->getKerning('V', 'A'), 11);
  };

  // Metrics may already be cached before a caller requests kerning. Upgrading
  // an all-covered cache must populate the matrix, not return early with zero.
  ASSERT_EQ(font.prewarm("A", 1, true, false), 0);
  ASSERT_EQ(font.prewarm("A", 1, true, true), 0);
  EXPECT_EQ(face->getKerning('A', 'A'), aa);
  const size_t readAfterFirst = HalFile::bytesRead;
  ASSERT_EQ(font.prewarm("A", 1, true, true), 0);
  EXPECT_EQ(HalFile::bytesRead, readAfterFirst);  // Stable metadata does not reread SD.

  // The second request adds V. Old A queries and cross-request pairs must stay
  // correct because both codepoints remain in the cumulative metadata cache.
  ASSERT_EQ(font.prewarm("V", 1, true, true), 0);
  verifyPairs();
  ASSERT_EQ(font.prewarm("A", 1, true, true), 0);
  verifyPairs();

  // A page-scoped bitmap cache contains just A; metadata then asks for V.
  // This also catches the mmap fast path retaining a subset's class maps.
  font.clearCache();
  ASSERT_EQ(font.prewarm("A", 1, false, true), 0);
  ASSERT_EQ(font.prewarm("AV", 1, true, true), 0);
  verifyPairs();

  // Unload/reload is allowed to preserve mini buffers, or follow clearCache.
  font.unloadMetadata();
  ASSERT_TRUE(font.reloadMetadata());
  ASSERT_EQ(font.prewarm("AV", 1, true, true), 0);
  verifyPairs();
  font.clearCache();
  font.unloadMetadata();
  ASSERT_TRUE(font.reloadMetadata());
  ASSERT_EQ(font.prewarm("AV", 1, true, true), 0);
  verifyPairs();
  std::filesystem::remove(path);
}

TEST(SdFontMetadataKern, FailedMatrixReadIsReportedAndCoveredCacheRetries) {
  const auto bytes = buildMetricFont(true);
  const auto path = writeTemp(bytes);
  SdCardFont font;
  ASSERT_TRUE(font.load(path.c_str()));
  // Header + TOC + three intervals + four glyphs + two class maps.
  constexpr long matrixOffset = 64 + 3 * 12 + 4 * 16 + 2 * 3 + 2 * 3;
  HalFile::failReadsFrom = matrixOffset;
  const int missed = font.prewarm("AV", 1, true, true);
  HalFile::failReadsFrom = -1;  // Reset before any ASSERT can stop the test.
  EXPECT_GT(missed, 0);
  EXPECT_EQ(font.getEpdFont()->getKerning('A', 'A'), 0);
  // Metrics were successfully cached. The all-covered fast path must still
  // retry the failed kerning read and replace the empty matrix.
  ASSERT_EQ(font.prewarm("AV", 1, true, true), 0);
  EXPECT_EQ(font.getEpdFont()->getKerning('A', 'A'), -403);
  EXPECT_EQ(font.getEpdFont()->getKerning('V', 'V'), -168);
  std::filesystem::remove(path);
}

INSTANTIATE_TEST_SUITE_P(V4V5SdMmap, SdFontVersion, testing::Combine(testing::Bool(), testing::Bool()));

TEST(SdFontV5, RejectsInvalidHeaderChecksumAndTruncatedMetadata) {
  const auto original = buildMetricFont(true);
  // Header checks are outside CRC; TOC corruption is re-checksummed to reach bounds validation.
  for (const int mutation : {0, 1, 2, 3, 4, 5}) {
    auto bytes = original;
    switch (mutation) {
      case 0:
        bytes[13] = 1;
        break;
      case 1:
        bytes[18] = 1;
        break;
      case 2:
        bytes[10] = 0;
        break;
      case 3:
        bytes.back() ^= 1;
        break;
      case 4:
        putU32(bytes, HEADER + 24, 0xFFFFFFF0u);
        updateChecksum(bytes);
        break;
      case 5:
        bytes.resize(HEADER + TOC_ENTRY + 3 * 12 + 4 * 16 + 12 + 7);
        updateChecksum(bytes);
        break;
    }
    const auto path = writeTemp(bytes);
    SdCardFont sd;
    SdCardFont mapped;
    EXPECT_FALSE(sd.load(path.c_str())) << "mutation " << mutation;
    EXPECT_FALSE(mapped.loadFromMmap(bytes.data(), bytes.size(), path.c_str())) << "mutation " << mutation;
    std::filesystem::remove(path);
  }
}

TEST(SdFontV5, SparseRowsAcrossChunksAndAllocationFallback) {
  const auto bytes = buildMetricFont(true, true);
  const auto path = writeTemp(bytes);
  // Rows 9 and 255 and columns 254 and 255 catch both byte-stride errors and
  // truncating high class IDs. Force the 4KB allocation failure to run the
  // low-memory per-row fallback independently of the normal chunked path.
  for (const bool mmap : {false, true}) {
    for (const bool fallback : {false, true}) {
      SdCardFont font;
      ASSERT_TRUE(mmap ? font.loadFromMmap(bytes.data(), bytes.size(), path.c_str()) : font.load(path.c_str()));
      failKernChunk = fallback;
      const int misses = font.prewarm("AV", 1, false, true);
      failKernChunk = false;
      ASSERT_EQ(misses, 0);
      auto* face = font.getEpdFont();
      EXPECT_EQ(face->getKerning('A', 'A'), -403);
      EXPECT_EQ(face->getKerning('V', 'V'), -168);
      EXPECT_EQ(face->getKerning('V', 'A'), 11);
      EXPECT_EQ(face->getKerning('A', 'V'), 44);
    }
  }
  std::filesystem::remove(path);
}

TEST(SdFontV5, PayloadChecksumChangesCacheIdentity) {
  auto first = buildMetricFont(true);
  auto second = first;
  second.back() ^= 0xFF;
  updateChecksum(second);
  SdCardFont a, b;
  ASSERT_TRUE(a.loadFromMmap(first.data(), first.size(), nullptr));
  ASSERT_TRUE(b.loadFromMmap(second.data(), second.size(), nullptr));
  EXPECT_NE(a.contentHash(), b.contentHash());
}

}  // namespace
