#include <gtest/gtest.h>

#include <string>

#include "activities/reader/EpubProgressRecord.h"

namespace {

EpubProgressRecord sample() {
  EpubProgressRecord r;
  r.spineIndex = 1733;  // above 255: both bytes of every field matter
  r.page = 300;
  r.pageCount = 412;
  r.percent = 87;
  return r;
}

TEST(EpubProgressRecord, WithoutAParagraphItIsTheSevenByteRecordEveryReaderKnows) {
  uint8_t data[EpubProgressRecord::kMaxSize];
  ASSERT_EQ(7u, sample().encode(data));
  // The prefix the sleep screen and the home progress badge read: spine, page, pageCount, percent.
  EXPECT_EQ(1733, data[0] | (data[1] << 8));
  EXPECT_EQ(300, data[2] | (data[3] << 8));
  EXPECT_EQ(412, data[4] | (data[5] << 8));
  EXPECT_EQ(87, data[6]);
}

TEST(EpubProgressRecord, AParagraphRoundTrips) {
  EpubProgressRecord r = sample();
  r.paragraph = 1025;
  uint8_t data[EpubProgressRecord::kMaxSize];
  const size_t size = r.encode(data);
  ASSERT_EQ(9u, size);

  const auto back = EpubProgressRecord::decode(data, size);
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(1733, back->spineIndex);
  EXPECT_EQ(300, back->page);
  EXPECT_EQ(412, back->pageCount);
  EXPECT_EQ(87, back->percent);
  ASSERT_TRUE(back->paragraph.has_value());
  EXPECT_EQ(1025, *back->paragraph);
}

TEST(EpubProgressRecord, ParagraphZeroIsNotAnAnchor) {
  EpubProgressRecord r = sample();
  r.paragraph = 0;  // the LUT's "no <p> opened yet"
  uint8_t data[EpubProgressRecord::kMaxSize];
  EXPECT_EQ(7u, r.encode(data));

  const uint8_t zeroTail[9] = {1, 0, 2, 0, 3, 0, 50, 0, 0};
  const auto decoded = EpubProgressRecord::decode(zeroTail, sizeof(zeroTail));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_FALSE(decoded->paragraph.has_value());
}

// Files written by earlier firmware must keep loading exactly as before.
TEST(EpubProgressRecord, ReadsEveryOlderSize) {
  const uint8_t data[9] = {5, 0, 12, 0, 40, 0, 33, 7, 0};

  const auto four = EpubProgressRecord::decode(data, 4);
  ASSERT_TRUE(four.has_value());
  EXPECT_EQ(5, four->spineIndex);
  EXPECT_EQ(12, four->page);
  EXPECT_EQ(0, four->pageCount) << "unknown, so nothing is rescaled against it";

  const auto six = EpubProgressRecord::decode(data, 6);
  ASSERT_TRUE(six.has_value());
  EXPECT_EQ(40, six->pageCount);

  const auto seven = EpubProgressRecord::decode(data, 7);
  ASSERT_TRUE(seven.has_value());
  EXPECT_EQ(33, seven->percent);
  EXPECT_FALSE(seven->paragraph.has_value());

  const auto eight = EpubProgressRecord::decode(data, 8);
  ASSERT_TRUE(eight.has_value()) << "a torn paragraph write still restores the page";
  EXPECT_FALSE(eight->paragraph.has_value());
}

EpubProgressRecord::Shown sampleShown() {
  EpubProgressRecord::Shown shown;
  shown.chapterPage = 314;  // above 255, like the fields above
  shown.chapterTotal = 1027;
  shown.printedPage = "(42)";
  return shown;
}

TEST(EpubProgressRecord, WhatTheReaderShowedRoundTrips) {
  EpubProgressRecord r = sample();
  r.paragraph = 1025;
  r.shown = sampleShown();
  uint8_t data[EpubProgressRecord::kMaxSize];
  const size_t size = r.encode(data);

  const auto back = EpubProgressRecord::decode(data, size);
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(1733, back->spineIndex);
  EXPECT_EQ(300, back->page);
  ASSERT_TRUE(back->paragraph.has_value());
  EXPECT_EQ(1025, *back->paragraph);
  ASSERT_TRUE(back->shown.has_value());
  EXPECT_EQ(314, back->shown->chapterPage);
  EXPECT_EQ(1027, back->shown->chapterTotal);
  EXPECT_EQ("(42)", back->shown->printedPage);
}

TEST(EpubProgressRecord, WhatTheReaderShowedLeavesThePrefixAlone) {
  EpubProgressRecord r = sample();
  r.shown = sampleShown();
  uint8_t data[EpubProgressRecord::kMaxSize];
  r.encode(data);
  // The home progress badge and older firmware read only this far.
  EXPECT_EQ(1733, data[0] | (data[1] << 8));
  EXPECT_EQ(300, data[2] | (data[3] << 8));
  EXPECT_EQ(412, data[4] | (data[5] << 8));
  EXPECT_EQ(87, data[6]);
}

TEST(EpubProgressRecord, WhatTheReaderShowedNeedsNoParagraph) {
  EpubProgressRecord r = sample();
  r.shown = sampleShown();
  uint8_t data[EpubProgressRecord::kMaxSize];
  const auto back = EpubProgressRecord::decode(data, r.encode(data));
  ASSERT_TRUE(back.has_value());
  EXPECT_FALSE(back->paragraph.has_value()) << "the paragraph slot is written as 0, the LUT's none";
  ASSERT_TRUE(back->shown.has_value());
  EXPECT_EQ(314, back->shown->chapterPage);
}

TEST(EpubProgressRecord, AChapterWithNoPrintedPageRoundTrips) {
  EpubProgressRecord r = sample();
  r.shown = EpubProgressRecord::Shown{};
  r.shown->chapterPage = 3;
  r.shown->chapterTotal = 5;
  uint8_t data[EpubProgressRecord::kMaxSize];
  const auto back = EpubProgressRecord::decode(data, r.encode(data));
  ASSERT_TRUE(back.has_value() && back->shown.has_value());
  EXPECT_EQ(5, back->shown->chapterTotal);
  EXPECT_TRUE(back->shown->printedPage.empty());
}

TEST(EpubProgressRecord, APrintedPageTooLongToStoreIsLeftOutNotCut) {
  EpubProgressRecord r = sample();
  r.shown = sampleShown();
  r.shown->printedPage = std::string("(") + std::string(EpubProgressRecord::kMaxPrintedPage, 'x') + ")";
  uint8_t data[EpubProgressRecord::kMaxSize];
  const auto back = EpubProgressRecord::decode(data, r.encode(data));
  ASSERT_TRUE(back.has_value() && back->shown.has_value());
  EXPECT_TRUE(back->shown->printedPage.empty());
  EXPECT_EQ(1027, back->shown->chapterTotal) << "the chapter position survives without the label";
}

TEST(EpubProgressRecord, ATornTailKeepsThePosition) {
  EpubProgressRecord r = sample();
  r.paragraph = 1025;
  r.shown = sampleShown();
  uint8_t data[EpubProgressRecord::kMaxSize];
  const size_t size = r.encode(data);

  // Cut inside the label, and inside the fixed part after the paragraph.
  for (const size_t cut : {size - 1, size_t{12}}) {
    const auto back = EpubProgressRecord::decode(data, cut);
    ASSERT_TRUE(back.has_value()) << cut;
    EXPECT_EQ(300, back->page) << cut;
    ASSERT_TRUE(back->paragraph.has_value()) << cut;
    EXPECT_EQ(1025, *back->paragraph) << cut;
    EXPECT_FALSE(back->shown.has_value()) << cut;
  }
}

TEST(EpubProgressRecord, RejectsWhatWasAlwaysRejected) {
  const uint8_t data[9] = {};
  EXPECT_FALSE(EpubProgressRecord::decode(data, 0).has_value());
  EXPECT_FALSE(EpubProgressRecord::decode(data, 3).has_value());
  EXPECT_FALSE(EpubProgressRecord::decode(data, 5).has_value()) << "a torn write of the 6-byte record";
}

}  // namespace

TEST(EpubProgressRecord, TaggedSourceHasLiteralLittleEndianBytesAndPreservesShownPrefix) {
  auto record = sample();
  record.paragraph = 91;
  record.shown = sampleShown();
  uint8_t original[EpubProgressRecord::kMaxSize] = {};
  const size_t prefix = record.encode(original);
  record.source = EpubProgressRecord::Source{0x78563412, 0x9abc};
  uint8_t bytes[EpubProgressRecord::kMaxSize] = {};
  const size_t size = record.encode(bytes);
  ASSERT_EQ(size, prefix + 10);
  EXPECT_EQ(memcmp(original, bytes, prefix), 0);
  const uint8_t literal[] = {'S', 'R', 'C', '1', 0x12, 0x34, 0x56, 0x78, 0xbc, 0x9a};
  EXPECT_EQ(memcmp(bytes + prefix, literal, sizeof(literal)), 0);
  const auto decoded = EpubProgressRecord::decode(bytes, size);
  ASSERT_TRUE(decoded && decoded->source);
  EXPECT_EQ(decoded->source->offset, 0x78563412u);
  EXPECT_EQ(decoded->source->characterOffset, 0x9abc);
  EXPECT_EQ(decoded->shown->printedPage, record.shown->printedPage);
}

TEST(EpubProgressRecord, TornOrUnrecognizedSourceKeepsTheCompleteOlderRecord) {
  for (const std::string& label : {std::string{}, std::string(EpubProgressRecord::kMaxPrintedPage, 'x')}) {
    auto record = sample();
    record.paragraph = 91;
    record.shown = sampleShown();
    record.shown->printedPage = label;
    record.source = EpubProgressRecord::Source{12345, 128};
    uint8_t bytes[EpubProgressRecord::kMaxSize] = {};
    const size_t size = record.encode(bytes);
    const size_t tail = size - EpubProgressRecord::kSourceTailSize;
    for (size_t truncated = tail; truncated < size; ++truncated) {
      const auto decoded = EpubProgressRecord::decode(bytes, truncated);
      ASSERT_TRUE(decoded);
      EXPECT_FALSE(decoded->source);
      EXPECT_EQ(decoded->paragraph, record.paragraph);
      ASSERT_TRUE(decoded->shown);
      EXPECT_EQ(decoded->shown->printedPage, label);
    }
    bytes[tail] = 'X';
    EXPECT_FALSE(EpubProgressRecord::decode(bytes, size)->source);
    bytes[tail] = 'S';
    memset(bytes + tail + 4, 0xff, 4);
    EXPECT_FALSE(EpubProgressRecord::decode(bytes, size)->source);
  }
}

TEST(EpubProgressRecord, PendingSourceNeedsNoParagraphOrShownAndOldRecordsRemainUnchanged) {
  auto record = sample();
  record.source = EpubProgressRecord::Source{0, 17};
  uint8_t bytes[EpubProgressRecord::kMaxSize] = {};
  const size_t size = record.encode(bytes);
  ASSERT_EQ(size, 24u);
  const auto decoded = EpubProgressRecord::decode(bytes, size);
  ASSERT_TRUE(decoded && decoded->source);
  EXPECT_EQ(decoded->source->offset, 0u);
  EXPECT_EQ(decoded->source->characterOffset, 17);
  EXPECT_FALSE(decoded->paragraph);
  record.source.reset();
  EXPECT_EQ(record.encode(bytes), 7u);
  EXPECT_FALSE(EpubProgressRecord::decode(bytes, 7)->source);
  record.source = EpubProgressRecord::Source{UINT32_MAX, 0};
  EXPECT_EQ(record.encode(bytes), 7u);
}
