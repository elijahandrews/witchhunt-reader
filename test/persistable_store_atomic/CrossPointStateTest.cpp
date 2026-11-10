#include <CrossPointState.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

namespace {
constexpr char STATE_PATH[] = "/.crosspoint/crossink-state.json";
constexpr char LEGACY_PATH[] = "/.crosspoint/state.json";
}

class CrossPointStateTest : public testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    EXPECT_FALSE(APP_STATE.loadFromFile());  // Clear the remembered on-disk snapshot.
    JsonDocument defaults;
    APP_STATE.fromJson(defaults.as<JsonVariantConst>());
  }
};

TEST_F(CrossPointStateTest, SkipsRepeatedSuccessfulSnapshot) {
  APP_STATE.openEpubPath = "/book.epub";
  ASSERT_TRUE(APP_STATE.saveToFile());
  ASSERT_TRUE(APP_STATE.saveToFile());
  EXPECT_EQ(Storage.writeAttempts, 1U);
}

TEST_F(CrossPointStateTest, PersistsReaderExitAndWallpaperChanges) {
  APP_STATE.readerActivityLoadCount = 1;
  ASSERT_TRUE(APP_STATE.saveToFile());
  APP_STATE.readerActivityLoadCount = 0;
  ASSERT_TRUE(APP_STATE.saveToFile());
  APP_STATE.pushRecentSleep(9);
  ASSERT_TRUE(APP_STATE.saveToFile());
  EXPECT_EQ(Storage.writeAttempts, 3U);
  ASSERT_TRUE(APP_STATE.loadFromFile());
  EXPECT_EQ(APP_STATE.readerActivityLoadCount, 0);
  EXPECT_TRUE(APP_STATE.isRecentSleep(9, 1));
}

TEST_F(CrossPointStateTest, RecreatesDeletedStateFile) {
  ASSERT_TRUE(APP_STATE.saveToFile());
  ASSERT_TRUE(Storage.remove(STATE_PATH));
  ASSERT_TRUE(APP_STATE.saveToFile());
  EXPECT_TRUE(Storage.exists(STATE_PATH));
  EXPECT_EQ(Storage.writeAttempts, 2U);
}

TEST_F(CrossPointStateTest, RetriesFailedFirstSave) {
  Storage.failNextWrite();
  EXPECT_FALSE(APP_STATE.saveToFile());
  EXPECT_TRUE(APP_STATE.saveToFile());
  EXPECT_EQ(Storage.writeAttempts, 2U);
}

TEST_F(CrossPointStateTest, FailedRewriteInvalidatesPreviousSnapshot) {
  APP_STATE.openEpubPath = "/old.epub";
  ASSERT_TRUE(APP_STATE.saveToFile());
  APP_STATE.openEpubPath = "/new.epub";
  Storage.failNextWrite();
  EXPECT_FALSE(APP_STATE.saveToFile());
  APP_STATE.openEpubPath = "/old.epub";
  ASSERT_TRUE(APP_STATE.saveToFile());
  EXPECT_EQ(Storage.writeAttempts, 3U);
  ASSERT_TRUE(APP_STATE.loadFromFile());
  EXPECT_EQ(APP_STATE.openEpubPath, "/old.epub");
}

TEST_F(CrossPointStateTest, ReloadInvalidatesPreviousSnapshot) {
  APP_STATE.openEpubPath = "/old.epub";
  ASSERT_TRUE(APP_STATE.saveToFile());
  Storage.put(STATE_PATH, R"({"openEpubPath":"/new.epub"})");
  ASSERT_TRUE(APP_STATE.loadFromFile());
  APP_STATE.openEpubPath = "/old.epub";
  ASSERT_TRUE(APP_STATE.saveToFile());
  EXPECT_EQ(Storage.writeAttempts, 2U);
  ASSERT_TRUE(APP_STATE.loadFromFile());
  EXPECT_EQ(APP_STATE.openEpubPath, "/old.epub");
}

TEST_F(CrossPointStateTest, ImportsLegacyWithoutOverwritingOriginal) {
  const std::string legacy = R"({"openEpubPath":"/prior.epub","witchOnly":42})";
  Storage.put(LEGACY_PATH, legacy);
  ASSERT_TRUE(APP_STATE.loadFromFile());
  EXPECT_EQ(APP_STATE.openEpubPath, "/prior.epub");
  APP_STATE.openEpubPath = "/current.epub";
  ASSERT_TRUE(APP_STATE.saveToFile());
  EXPECT_EQ(Storage.readFile(LEGACY_PATH), legacy);
  ASSERT_TRUE(APP_STATE.loadFromFile());
  EXPECT_EQ(APP_STATE.openEpubPath, "/current.epub");
}

TEST_F(CrossPointStateTest, CorruptOwnStateNeverResurrectsLegacy) {
  Storage.put(LEGACY_PATH, R"({"openEpubPath":"/prior.epub"})");
  Storage.put(STATE_PATH, "{broken");
  EXPECT_FALSE(APP_STATE.loadFromFile());
  EXPECT_TRUE(APP_STATE.openEpubPath.empty());
}

TEST_F(CrossPointStateTest, OwnBackupTakesPriorityOverLegacy) {
  Storage.put(LEGACY_PATH, R"({"openEpubPath":"/prior.epub"})");
  Storage.put(std::string(STATE_PATH) + ".bak", R"({"openEpubPath":"/current.epub"})");
  ASSERT_TRUE(APP_STATE.loadFromFile());
  EXPECT_EQ(APP_STATE.openEpubPath, "/current.epub");
}

TEST_F(CrossPointStateTest, ImportsInterruptedLegacyBackupWithoutRenamingIt) {
  const std::string backupPath = std::string(LEGACY_PATH) + ".bak";
  const std::string legacy = R"({"openEpubPath":"/prior.epub","witchOnly":42})";
  Storage.put(backupPath, legacy);
  ASSERT_TRUE(APP_STATE.loadFromFile());
  EXPECT_EQ(APP_STATE.openEpubPath, "/prior.epub");
  ASSERT_TRUE(APP_STATE.saveToFile());
  EXPECT_EQ(Storage.readFile(backupPath.c_str()), legacy);
  EXPECT_FALSE(Storage.exists(LEGACY_PATH));
}
