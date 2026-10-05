// Host tests for the pure Range helpers behind chunked downloads (lib/SecureNet HttpRange).

#include <HttpRange.h>
#include <gtest/gtest.h>

#include <vector>

namespace hr = crosspoint::http_range;

TEST(HttpRangeHeader, FormatsAnInclusiveByteRange) {
  EXPECT_EQ(hr::rangeHeaderValue(0, 8191), "bytes=0-8191");
  EXPECT_EQ(hr::rangeHeaderValue(262144, 269829), "bytes=262144-269829");
  EXPECT_EQ(hr::rangeHeaderValue(7, 7), "bytes=7-7");
}

TEST(HttpContentRange, ParsesARangeWithItsTotal) {
  hr::ContentRange r;
  ASSERT_TRUE(hr::parseContentRange("bytes 0-8191/269830", r));
  EXPECT_TRUE(r.satisfied);
  EXPECT_EQ(r.first, 0u);
  EXPECT_EQ(r.last, 8191u);
  EXPECT_TRUE(r.totalKnown);
  EXPECT_EQ(r.total, 269830u);
}

TEST(HttpContentRange, ParsesAnUnknownTotal) {
  hr::ContentRange r;
  ASSERT_TRUE(hr::parseContentRange("bytes 8192-16383/*", r));
  EXPECT_TRUE(r.satisfied);
  EXPECT_EQ(r.first, 8192u);
  EXPECT_EQ(r.last, 16383u);
  EXPECT_FALSE(r.totalKnown);
}

TEST(HttpContentRange, ParsesTheUnsatisfiedForm) {
  hr::ContentRange r;
  ASSERT_TRUE(hr::parseContentRange("bytes */269830", r));
  EXPECT_FALSE(r.satisfied);
  EXPECT_TRUE(r.totalKnown);
  EXPECT_EQ(r.total, 269830u);
}

TEST(HttpContentRange, AcceptsAnyUnitCaseAndSurroundingSpaces) {
  hr::ContentRange r;
  EXPECT_TRUE(hr::parseContentRange("  Bytes 0-0/1 ", r));
  EXPECT_TRUE(hr::parseContentRange("BYTES   10-19/20", r));
  EXPECT_EQ(r.first, 10u);
}

TEST(HttpContentRange, RejectsMalformedValues) {
  hr::ContentRange r;
  const std::vector<const char*> bad = {
      "",                                   // empty
      "bytes",                              // no range
      "byte 0-1/2",                         // wrong unit
      "items 0-1/2",                        // other unit
      "bytes0-1/2",                         // no space after the unit
      "bytes 0-1",                          // no total
      "bytes 5-4/10",                       // last before first
      "bytes 0-10/10",                      // last past the end
      "bytes */*",                          // says nothing
      "bytes -1/10",                        // no first
      "bytes 0-/10",                        // no last
      "bytes 0-1/2x",                       // trailing garbage
      "bytes 0-1/-2",                       // negative total
      "bytes 1-2/99999999999999999999999",  // does not fit
  };
  for (const char* value : bad) {
    EXPECT_FALSE(hr::parseContentRange(value, r)) << value;
    EXPECT_FALSE(r.satisfied) << value;
    EXPECT_FALSE(r.totalKnown) << value;
  }
  EXPECT_FALSE(hr::parseContentRange(nullptr, r));
}

TEST(HttpRangeReply, A206ForTheRequestedStartIsPartial) {
  hr::ContentRange r;
  ASSERT_TRUE(hr::parseContentRange("bytes 8192-16383/269830", r));
  EXPECT_EQ(hr::classifyReply(206, true, r, 8192), hr::RangeReply::Partial);
}

TEST(HttpRangeReply, A206ForAnotherStartOrWithoutContentRangeIsAMismatch) {
  hr::ContentRange r;
  ASSERT_TRUE(hr::parseContentRange("bytes 0-8191/269830", r));
  EXPECT_EQ(hr::classifyReply(206, true, r, 8192), hr::RangeReply::Mismatch);
  EXPECT_EQ(hr::classifyReply(206, false, hr::ContentRange{}, 0), hr::RangeReply::Mismatch);
  ASSERT_TRUE(hr::parseContentRange("bytes */269830", r));
  EXPECT_EQ(hr::classifyReply(206, true, r, 0), hr::RangeReply::Mismatch);
}

TEST(HttpRangeReply, A200MeansTheServerIgnoredRange) {
  // The 200-instead-of-206 fallback: whatever was written must be rewound before this body.
  EXPECT_EQ(hr::classifyReply(200, false, hr::ContentRange{}, 0), hr::RangeReply::WholeBody);
  EXPECT_EQ(hr::classifyReply(200, false, hr::ContentRange{}, 8192), hr::RangeReply::WholeBody);
}

TEST(HttpRangeReply, OtherStatuses) {
  hr::ContentRange r;
  ASSERT_TRUE(hr::parseContentRange("bytes */16384", r));
  EXPECT_EQ(hr::classifyReply(416, true, r, 16384), hr::RangeReply::Unsatisfiable);
  EXPECT_EQ(hr::classifyReply(404, false, hr::ContentRange{}, 0), hr::RangeReply::Error);
  EXPECT_EQ(hr::classifyReply(500, false, hr::ContentRange{}, 0), hr::RangeReply::Error);
  EXPECT_EQ(hr::classifyReply(302, false, hr::ContentRange{}, 0), hr::RangeReply::Error);
}

TEST(HttpRangeChunks, PlansOffsetsUpToAShortFinalChunk) {
  // 20,000 bytes in 8 KB chunks: two full chunks, then the 3,616-byte remainder.
  std::vector<hr::Chunk> plan;
  size_t offset = 0;
  hr::Chunk c;
  while (hr::nextChunk(offset, 8192, true, 20000, c)) {
    plan.push_back(c);
    offset += c.length();
  }
  ASSERT_EQ(plan.size(), 3u);
  EXPECT_EQ(plan[0].first, 0u);
  EXPECT_EQ(plan[0].last, 8191u);
  EXPECT_EQ(plan[1].first, 8192u);
  EXPECT_EQ(plan[1].last, 16383u);
  EXPECT_EQ(plan[2].first, 16384u);
  EXPECT_EQ(plan[2].last, 19999u);
  EXPECT_EQ(plan[2].length(), 3616u);
  EXPECT_EQ(offset, 20000u);
}

TEST(HttpRangeChunks, AnExactMultipleEndsOnAFullChunk) {
  hr::Chunk c;
  ASSERT_TRUE(hr::nextChunk(8192, 8192, true, 16384, c));
  EXPECT_EQ(c.last, 16383u);
  EXPECT_FALSE(hr::nextChunk(16384, 8192, true, 16384, c));
}

TEST(HttpRangeChunks, AnUnknownTotalAsksForFullChunksUntilOneIsShort) {
  hr::Chunk c;
  ASSERT_TRUE(hr::nextChunk(0, 4096, false, 0, c));
  EXPECT_EQ(c.first, 0u);
  EXPECT_EQ(c.last, 4095u);
  ASSERT_TRUE(hr::nextChunk(4096, 4096, false, 0, c));
  EXPECT_EQ(c.last, 8191u);
  // A full chunk is not the end; a short one is.
  EXPECT_FALSE(hr::transferComplete(8192, false, 0, 4096, 4096));
  EXPECT_TRUE(hr::transferComplete(9000, false, 0, 4096, 808));
}

TEST(HttpRangeChunks, AKnownTotalCompletesAtTheTotal) {
  EXPECT_FALSE(hr::transferComplete(8192, true, 20000, 8192, 8192));
  EXPECT_TRUE(hr::transferComplete(20000, true, 20000, 3616, 3616));
}

TEST(HttpRangeChunks, AZeroChunkPlansNothing) {
  hr::Chunk c;
  EXPECT_FALSE(hr::nextChunk(0, 0, false, 0, c));
}

TEST(HttpRangeHeap, RoundsLikeTheC3Heap) {
  // A full TLS 1.3 record (2^14 + 17) occupies the 17,408 B block seen on the device.
  EXPECT_EQ(hr::heapBlockFor(16401), 17408u);
  // An 8 KB range sent as one record (8,209 B, measured from GitHub) needs 8,704 B.
  EXPECT_EQ(hr::heapBlockFor(8209), 8704u);
  EXPECT_EQ(hr::heapBlockFor(1), 16u);
}

TEST(HttpRangeHeap, ChunkSizeFollowsTheLargestFreeBlock) {
  // X3, after a resumed handshake: the largest block was 16,372 B, too small for an 8 KB chunk's
  // record block and receive buffers (16,896 B), enough for 6 KB (14,592 B).
  EXPECT_EQ(hr::chunkSizeForLargestBlock(16372), 6144u);
  EXPECT_EQ(hr::chunkSizeForLargestBlock(16896), 8192u);
  EXPECT_EQ(hr::chunkSizeForLargestBlock(30000), 8192u);
  EXPECT_EQ(hr::chunkSizeForLargestBlock(12544), 4096u);
  EXPECT_EQ(hr::chunkSizeForLargestBlock(9000), 2048u);
  EXPECT_EQ(hr::chunkSizeForLargestBlock(6016), 1024u);
  // Nothing fits: the smallest chunk is still the best chance.
  EXPECT_EQ(hr::chunkSizeForLargestBlock(2000), hr::MIN_CHUNK_BYTES);
}

TEST(HttpRangeHeap, EveryChunkSizeIsBoundedByItsOwnBudget) {
  for (size_t chunk : {8192u, 6144u, 4096u, 2048u, 1024u}) {
    const size_t need = hr::recordBlockFor(chunk) + hr::receiveBuffersFor(chunk);
    EXPECT_EQ(hr::chunkSizeForLargestBlock(need), chunk) << chunk;
  }
}

TEST(HttpRangeHeap, ChunksOnlyWhenTheLargestBlockIsSmall) {
  EXPECT_TRUE(hr::shouldChunk(23540));  // X3 Font Manager before the connect
  EXPECT_TRUE(hr::shouldChunk(40 * 1024 - 1));
  EXPECT_FALSE(hr::shouldChunk(40 * 1024));
  EXPECT_FALSE(hr::shouldChunk(4 * 1024 * 1024));  // S3 with PSRAM in the default heap
}
