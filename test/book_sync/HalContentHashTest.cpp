#define HAL_STORAGE_IMPL
#include <SDCardManager.h>
#include <gtest/gtest.h>

#include <functional>

#include "hal/HalSpiBus.h"
#include "hal/HalStorage.h"
HalSpiBus::Lock::Lock() = default;
HalSpiBus::Lock::~Lock() = default;
class HalContentHashTest : public testing::Test {
 protected:
  uint8_t hash[32]{};
  uint32_t bytes = 0;
  void SetUp() override {
    card.clear();
    failClose = false;
    Storage.begin();
    SDCardManager::getInstance().mkdir("/");
    SDCardManager::getInstance().mkdir("/Books");
    SDCardManager::getInstance().mkdir("/.crosspoint");
  }
  void seal() {
    uint32_t generation;
    ASSERT_TRUE(Storage.beginContentHashScan(generation));
    Storage.saveContentHashSeal(generation, hash, 512);
    ASSERT_TRUE(Storage.loadContentHashSeal(hash, bytes));
  }
  void invalidated() { EXPECT_FALSE(Storage.loadContentHashSeal(hash, bytes)); }
  void put(const char* p) {
    HalFile f;
    ASSERT_TRUE(Storage.openFileForWrite("test", p, f));
    ASSERT_EQ(f.write(uint8_t{42}), 1u);
    ASSERT_TRUE(f.close());
  }
};
TEST_F(HalContentHashTest, EveryPublicMutationInvalidatesRealHalSeal) {
  const std::function<void()> changes[] = {[] { Storage.writeFile("/Books/new", String("x")); },
                                           [] { Storage.ensureDirectoryExists("/Books/newdir"); },
                                           [] { Storage.mkdir("/Books/mkdir"); },
                                           [] { Storage.remove("/Books/missing"); },
                                           [] { Storage.rmdir("/Books/missing"); },
                                           [] { Storage.rename("/Books/missing", "/Books/new"); },
                                           [] { auto f = Storage.open("/Books/open", O_WRONLY | O_CREAT); },
                                           [] {
                                             HalFile f;
                                             Storage.openFileForWrite("test", "/Books/write", f);
                                           }};
  for (const auto& change : changes) {
    seal();
    auto before = Storage.contentGeneration();
    change();
    EXPECT_GT(Storage.contentGeneration(), before);
    invalidated();
  }
}
TEST_F(HalContentHashTest, WriterMutationsAndMovesCannotBeBlessed) {
  put("/.crosspoint/stage");
  seal();
  HalFile writer;
  ASSERT_TRUE(Storage.openFileForUpdate("test", "/.crosspoint/stage", writer));
  uint32_t generation;
  EXPECT_FALSE(Storage.beginContentHashScan(generation));
  ASSERT_TRUE(Storage.rename("/.crosspoint/stage", "/Books/moved"));
  invalidated();
  writer.write(uint8_t{4});
  Storage.saveContentHashSeal(Storage.contentGeneration(), hash, 512);
  invalidated();
  writer.close();
  seal();
  ASSERT_TRUE(Storage.openFileForUpdate("test", "/Books/moved", writer));
  invalidated();
  auto before = Storage.contentGeneration();
  writer.write(uint8_t{5});
  EXPECT_GT(Storage.contentGeneration(), before);
  writer.close();
  seal();
}
TEST_F(HalContentHashTest, HalFileRenameCountsReadOnlyAndWritableHandles) {
  put("/.crosspoint/private");
  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("test", "/.crosspoint/private", file));
  seal();
  ASSERT_TRUE(file.rename("/Books/renamed"));
  invalidated();
  uint32_t generation;
  EXPECT_FALSE(Storage.beginContentHashScan(generation));
  file.close();
  seal();
}
TEST_F(HalContentHashTest, PrivateClosedWritesDoNotInvalidateButTraversalDoes) {
  seal();
  put("/.crosspoint/progress");
  EXPECT_TRUE(Storage.loadContentHashSeal(hash, bytes));
  Storage.ensureDirectoryExists("/Books");
  EXPECT_TRUE(Storage.loadContentHashSeal(hash, bytes));
  put("/.crosspoint/../Books/traversal");
  invalidated();
}
TEST_F(HalContentHashTest, USBRemountShutdownAndStaleGenerationFailClosed) {
  for (const auto& change :
       std::vector<std::function<void()>>{[] { Storage.beginUsbDrive(); }, [] { Storage.endUsbDrive(); },
                                          [] { Storage.begin(); }, [] { Storage.shutdown(); }}) {
    seal();
    change();
    invalidated();
  }
  uint32_t generation;
  ASSERT_TRUE(Storage.beginContentHashScan(generation));
  Storage.remove("/Books/changed");
  Storage.saveContentHashSeal(generation, hash, 512);
  invalidated();
}
TEST_F(HalContentHashTest, WriterMoveAssignmentAndDestructionReleaseAdmission) {
  {
    auto first = Storage.open("/.crosspoint/a", O_CREAT | O_WRONLY);
    auto second = Storage.open("/.crosspoint/b", O_CREAT | O_WRONLY);
    first = std::move(second);
    uint32_t generation;
    EXPECT_FALSE(Storage.beginContentHashScan(generation));
  }
  seal();
}
TEST_F(HalContentHashTest, FailedCloseInvalidatesTrust) {
  put("/.crosspoint/private");
  seal();
  HalFile file;
  ASSERT_TRUE(Storage.openFileForUpdate("test", "/.crosspoint/private", file));
  failClose = true;
  EXPECT_FALSE(file.close());
  failClose = false;
  invalidated();
}

// Model SdFat access permissions so read-after-write contracts cannot pass
// merely because the fake file treats every open handle as readable.
TEST_F(HalContentHashTest, ExplicitAccessModesEnforceReadAndWritePermissions) {
  auto writer = Storage.open("/.crosspoint/modes", O_WRONLY | O_CREAT | O_TRUNC);
  ASSERT_TRUE(writer);
  ASSERT_EQ(writer.write(uint8_t{42}), 1u);
  ASSERT_TRUE(writer.seek(0));
  uint8_t value = 99;
  EXPECT_EQ(writer.read(&value, 1), -1);
  EXPECT_EQ(value, 99);
  EXPECT_EQ(writer.read(), -1);
  ASSERT_TRUE(writer.close());

  HalFile reader;
  ASSERT_TRUE(Storage.openFileForRead("test", "/.crosspoint/modes", reader));
  EXPECT_EQ(reader.read(), 42);
  EXPECT_EQ(reader.write(uint8_t{7}), 0u);
  ASSERT_TRUE(reader.seek(1024));
  EXPECT_EQ(reader.read(&value, 1), 0);
  EXPECT_EQ(reader.read(), -1);
  EXPECT_EQ(reader.available(), 0);
  uint32_t generation;
  EXPECT_TRUE(Storage.beginContentHashScan(generation));
  ASSERT_TRUE(reader.close());
  EXPECT_EQ(card.at("/.crosspoint/modes")->data, std::string(1, char(42)));
}

TEST_F(HalContentHashTest, SectionWriteHandleSupportsReadbackAndContinuedWriting) {
  // Section::loadPageDuringBuild saves the write cursor, seeks to a serialized
  // page's uint16 element count, deserializes through the SAME handle, then
  // restores the cursor and continues appending the unfinished section.
  seal();
  const auto before = Storage.contentGeneration();
  HalFile section;
  ASSERT_TRUE(Storage.openFileForWrite("SCT", "/.crosspoint/section.tmp", section));
  uint32_t generation;
  EXPECT_FALSE(Storage.beginContentHashScan(generation));
  EXPECT_FALSE(Storage.loadContentHashSeal(hash, bytes));
  const uint8_t page[] = {2, 0, 0x31, 0x72, 0x19, 0x25};
  ASSERT_EQ(section.write(page, sizeof(page)), sizeof(page));
  const auto appendPosition = section.position();
  ASSERT_TRUE(section.seek(0));
  uint16_t elementCount = 0;
  ASSERT_EQ(section.read(&elementCount, sizeof(elementCount)), int(sizeof(elementCount)));
  EXPECT_EQ(elementCount, 2);
  uint8_t body[4]{};
  ASSERT_EQ(section.read(body, sizeof(body)), int(sizeof(body)));
  EXPECT_EQ(std::memcmp(body, page + sizeof(elementCount), sizeof(body)), 0);
  ASSERT_TRUE(section.seek(appendPosition));
  ASSERT_EQ(section.write(uint8_t{0x5a}), 1u);
  EXPECT_FALSE(Storage.beginContentHashScan(generation));
  ASSERT_TRUE(section.close());
  EXPECT_TRUE(Storage.beginContentHashScan(generation));
  EXPECT_EQ(Storage.contentGeneration(), before);  // Private section writes stay private.
  EXPECT_TRUE(Storage.loadContentHashSeal(hash, bytes));
  EXPECT_EQ(card.at("/.crosspoint/section.tmp")->data, std::string("\x02\0\x31\x72\x19\x25\x5a", 7));

  ASSERT_TRUE(Storage.openFileForUpdate("SCT", "/.crosspoint/section.tmp", section));
  EXPECT_FALSE(Storage.beginContentHashScan(generation));
  ASSERT_TRUE(section.seek(0));
  EXPECT_EQ(section.read(), 2);
  ASSERT_TRUE(section.seek(section.size()));
  ASSERT_EQ(section.write(uint8_t{0x6b}), 1u);
  ASSERT_TRUE(section.seek(7));
  EXPECT_EQ(section.read(), 0x6b);
  ASSERT_TRUE(section.close());
  EXPECT_TRUE(Storage.beginContentHashScan(generation));
}
