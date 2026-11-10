#include <gtest/gtest.h>

#include "EpubReaderUtils.h"
#include "ReaderWordHold.h"
#include "TwoSlotFile.h"

class ProgressSlotsTest : public testing::Test {
 protected:
  Epub epub;
  EpubReaderUtils::Progress loaded;
  const std::string path = "/epub/progress.bin";
  void SetUp() override { Storage.reset(); }
};

TEST_F(ProgressSlotsTest, ImportsWitchPrefixWithoutTreatingPercentAsTextOffset) {
  const uint8_t bytes[] = {3, 0, 8, 0, 40, 0, 75, 12, 0};
  auto legacy = Storage.open("/witch/progress.bin", O_RDWR | O_CREAT);
  legacy.write(bytes, sizeof(bytes));
  legacy.close();
  const auto original = Storage.data("/witch/progress.bin").bytes;
  ASSERT_TRUE(EpubReaderUtils::loadProgress(epub, loaded));
  EXPECT_TRUE(loaded.importedFromWitch);
  EXPECT_EQ(loaded.spineIndex, 3);
  EXPECT_EQ(loaded.pageNumber, 8);
  EXPECT_EQ(loaded.pageCount, 40);
  EXPECT_EQ(loaded.legacyParagraph, 12);
  EXPECT_FALSE(loaded.hasVisibleTextOffset);
  EXPECT_FALSE(Storage.exists(path.c_str()));
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 3, 9, 44, 1234));
  ASSERT_TRUE(EpubReaderUtils::loadProgress(epub, loaded));
  EXPECT_FALSE(loaded.importedFromWitch);
  EXPECT_EQ(loaded.pageNumber, 9);
  EXPECT_EQ(Storage.data("/witch/progress.bin").bytes, original);
}

TEST_F(ProgressSlotsTest, CorruptNewSaveNeverResurrectsAnOlderWitchPosition) {
  auto legacy = Storage.open("/witch/progress.bin", O_RDWR | O_CREAT);
  const uint8_t bytes[] = {3, 0, 8, 0, 40, 0, 75};
  legacy.write(bytes, sizeof(bytes));
  legacy.close();
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 3, 20, 40));
  Storage.data(path).bytes[16] ^= 1;
  EXPECT_FALSE(EpubReaderUtils::loadProgress(epub, loaded));
}

TEST_F(ProgressSlotsTest, WitchSourceTailSurvivesDecodeAndTruncatedTailKeepsPrefix) {
  uint8_t bytes[24] = {3, 0, 8, 0, 40, 0, 75, 12, 0, 9, 0, 40, 0, 0, 'S', 'R', 'C', '1', 0x78, 0x56, 0x34, 0x12, 7, 0};
  witch_progress::Position position;
  ASSERT_TRUE(witch_progress::decode(bytes, sizeof(bytes), position));
  EXPECT_EQ(position.sourceOffset, 0x12345678U);
  EXPECT_EQ(position.sourceCharacter, 7);
  ASSERT_TRUE(witch_progress::decode(bytes, 20, position));
  EXPECT_EQ(position.sourceOffset, UINT32_MAX);
  EXPECT_EQ(position.page, 8);
  EXPECT_FALSE(witch_progress::decode(bytes, 5, position));
  EXPECT_FALSE(witch_progress::decode(bytes, 40, position));
}

TEST(ReaderWordHold, RejectsOldContactAfterPageChangesAndMissedPressEdge) {
  ReaderWordHold hold;
  ReaderWordHold::Page first{1, 2, 1000, 0};
  hold.observe(true, true, first, 1200, 100);
  const ReaderWordHold::Page second{1, 3, 1300, 0};
  hold.observe(true, true, second, 1800, 700);
  EXPECT_FALSE(hold.consume(second));
  hold.reset();
  // A first observed tick can already contain a long contact. It must still
  // have begun after the displayed page, regardless of missed press events.
  hold.observe(true, true, second, 1800, 700);
  EXPECT_FALSE(hold.consume(second));
  hold.observe(true, true, second, 1850, 20);  // new contact, release was missed
  hold.observe(true, true, second, 2500, 670);
  EXPECT_TRUE(hold.consume(second));
  EXPECT_FALSE(hold.consume(second));
}

TEST(ReaderWordHold, RejectsStaleBuffersAndOrientationChanges) {
  ReaderWordHold hold;
  ReaderWordHold::Page page{1, 2, 1000, 0};
  hold.observe(true, true, page, 1200, 100);
  hold.observe(true, false, page, 1800, 700);
  EXPECT_FALSE(hold.consume(page));
  hold.observe(false, true, page, 1900, 0);
  hold.observe(true, true, page, 2000, 20);
  page.orientation = 1;
  hold.observe(true, true, page, 2700, 720);
  EXPECT_FALSE(hold.consume(page));
}
TEST_F(ProgressSlotsTest, AlternatesAndRestoresOffsetsFromNewestSlot) {
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 2, 10, 100));
  const auto first = Storage.data(path).bytes;
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 3, 4, 20, 200));
  EXPECT_EQ(Storage.data(path).bytes, first);
  ASSERT_TRUE(EpubReaderUtils::loadProgress(epub, loaded));
  EXPECT_EQ(loaded.spineIndex, 3);
  EXPECT_EQ(loaded.pageNumber, 4);
  EXPECT_EQ(loaded.visibleTextOffset, 200u);
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 3, 5, 20));
  ASSERT_TRUE(EpubReaderUtils::loadProgress(epub, loaded));
  EXPECT_EQ(loaded.pageNumber, 5);
  EXPECT_FALSE(loaded.hasVisibleTextOffset);
}
TEST_F(ProgressSlotsTest, CorruptionAndInterruptedWritesRetainPreviousPosition) {
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 2, 10));
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 3, 10));
  Storage.data(path).writeLimit = 5;
  EXPECT_FALSE(EpubReaderUtils::saveProgress(epub, 1, 4, 10));
  ASSERT_TRUE(EpubReaderUtils::loadProgress(epub, loaded));
  EXPECT_EQ(loaded.pageNumber, 3);
  Storage.data(path + ".bak").bytes[3] ^= 1;
  EXPECT_FALSE(EpubReaderUtils::loadProgress(epub, loaded));
}
TEST_F(ProgressSlotsTest, SoleLegacySnapshotSurvivesFirstUpgradeFailure) {
  auto legacy = Storage.open(path.c_str(), O_RDWR | O_CREAT);
  const uint8_t bytes[] = {1, 0, 2, 0, 10, 0};
  legacy.write(bytes, sizeof(bytes));
  legacy.close();
  auto backup = Storage.open((path + ".bak").c_str(), O_RDWR | O_CREAT);
  backup.close();
  Storage.data(path + ".bak").writeLimit = 5;
  EXPECT_FALSE(EpubReaderUtils::saveProgress(epub, 1, 3, 10));
  ASSERT_TRUE(EpubReaderUtils::loadProgress(epub, loaded));
  EXPECT_EQ(loaded.pageNumber, 2);
  Storage.data(path + ".bak").writeLimit = SIZE_MAX;
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 3, 10));
  ASSERT_TRUE(EpubReaderUtils::loadProgress(epub, loaded));
  EXPECT_EQ(loaded.pageNumber, 3);
}
TEST_F(ProgressSlotsTest, SequenceWrapAndUnknownFlags) {
  progress_record::Fields fields{1, 2, 10, true, 0, false};
  uint8_t bytes[progress_record::RECORD_BYTES];
  progress_record::encode(fields, UINT32_MAX, bytes);
  auto primary = progress_record::decode(bytes, sizeof(bytes));
  progress_record::encode(fields, 0, bytes);
  auto backup = progress_record::decode(bytes, sizeof(bytes));
  EXPECT_EQ(progress_record::newest(primary, backup), progress_record::BACKUP);
  EXPECT_EQ(progress_record::target(primary, backup), progress_record::PRIMARY);
  bytes[11] = 2;
  progress_record::writeU32(bytes + 16, progress_record::crc32(bytes, 16));
  EXPECT_EQ(progress_record::decode(bytes, sizeof(bytes)).kind, progress_record::SlotKind::Invalid);
}
TEST_F(ProgressSlotsTest, GlobalSizedPayloadRoundTripsWithoutChangingPayloadBytes) {
  uint8_t bytes[159] = {3};
  bytes[158] = 79;
  ASSERT_TRUE(two_slot::write("TEST", path.c_str(), (path + ".bak").c_str(), bytes, sizeof(bytes)));
  bytes[100] = 17;
  ASSERT_TRUE(two_slot::write("TEST", path.c_str(), (path + ".bak").c_str(), bytes, sizeof(bytes)));
  uint8_t result[two_slot::MAX_PAYLOAD_BYTES] = {};
  ASSERT_EQ(two_slot::read(
                "TEST", path.c_str(), (path + ".bak").c_str(), sizeof(bytes),
                [](const uint8_t* data, size_t size) { return size == 159 && data[0] == 3; }, result),
            sizeof(bytes));
  EXPECT_EQ(memcmp(bytes, result, sizeof(bytes)), 0);
}

TEST_F(ProgressSlotsTest, SourceAnchorsRoundTripWithNativeAndExtendedSlots) {
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 2, 10, 123));
  EXPECT_EQ(Storage.data(path).bytes.size(), progress_record::RECORD_BYTES);
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 3, 12, 145, {9876, 7}));
  ASSERT_TRUE(EpubReaderUtils::loadProgress(epub, loaded));
  EXPECT_EQ(loaded.sourceAnchor.sourceOffset, 9876u);
  EXPECT_EQ(loaded.sourceAnchor.characterOffset, 7u);
  EXPECT_EQ(loaded.visibleTextOffset, 145u);
  // A damaged new anchor cannot attach itself to the older native position.
  Storage.data(path + ".bak").bytes[20] ^= 1;
  ASSERT_TRUE(EpubReaderUtils::loadProgress(epub, loaded));
  EXPECT_EQ(loaded.pageNumber, 2);
  EXPECT_FALSE(loaded.sourceAnchor.valid());
}
TEST_F(ProgressSlotsTest, InterruptedSourceUpgradeAndDowngradePreserveTheOtherSlot) {
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 2, 10, 123, {200, 1}));
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 3, 10, 145, {300, 2}));
  Storage.data(path).writeLimit = 23;
  EXPECT_FALSE(EpubReaderUtils::saveProgress(epub, 1, 4, 10, 180, {400, 3}));
  ASSERT_TRUE(EpubReaderUtils::loadProgress(epub, loaded));
  EXPECT_EQ(loaded.sourceAnchor.sourceOffset, 300u);
  Storage.data(path).writeLimit = SIZE_MAX;
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 5, 10, 200));
  ASSERT_TRUE(EpubReaderUtils::loadProgress(epub, loaded));
  EXPECT_EQ(loaded.pageNumber, 5);
  EXPECT_FALSE(loaded.sourceAnchor.valid());
}
