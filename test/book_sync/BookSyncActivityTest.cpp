#include <gtest/gtest.h>
#include <network/HttpDownloader.h>
#include <openssl/sha.h>

#include "src/activities/network/BookSyncActivity.h"

namespace {
const std::string ORIGIN = "https://files.example:8444";
const std::string BODY = std::string("PK\x03\x04") + "Synthetic EPUB fixture bytes";
std::string digest(const std::string& data) {
  unsigned char bytes[32];
  SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), bytes);
  std::string result;
  for (auto b : bytes) {
    result += "0123456789abcdef"[b >> 4];
    result += "0123456789abcdef"[b & 15];
  }
  return result;
}
std::string name(char id = 'b') { return "Example~" + std::string(64, id) + ".epub"; }
std::string row(char id = 'b') { return digest(BODY) + "\t" + std::to_string(BODY.size()) + "\t" + name(id) + "\n"; }
std::string endpoint(char id = 'b') { return ORIGIN + "/v1/files/" + std::string(64, id) + "/" + digest(BODY); }
class SyncActivityTest : public testing::Test {
 protected:
  GfxRenderer renderer;
  MappedInputManager input;
  void SetUp() override {
    Storage.reset();
    injectCancel = false;
    HttpDownloader::requests.clear();
    HttpDownloader::responses.clear();
    HttpDownloader::abortBook = HttpDownloader::corruptBook = HttpDownloader::cancelBook = false;
    Storage.put("/.crosspoint/book-sync.json",
                "{\"url\":\"" + ORIGIN + "\",\"token\":\"" + std::string(64, 'a') + "\"}");
    Storage.put("/.crosspoint/book-sync-ca.pem", "fictional CA for the transport mock");
    Storage.put("/.crosspoint/settings.json", "unchanged settings");
    Storage.put("/.crosspoint/epub_123/progress", "unchanged reading position");
    HttpDownloader::responses[ORIGIN + "/v1/manifest"] = "WITCHSYNC1\n" + row() + "END\t1\n";
    HttpDownloader::responses[endpoint()] = BODY;
  }
  void run(const char* expected) {
    BookSyncActivity activity(renderer, input);
    activity.onEnter();
    ASSERT_GE(renderer.text.size(), 2U);
    EXPECT_EQ(renderer.text[0], expected);
    EXPECT_EQ(Storage.content("/.crosspoint/settings.json"), "unchanged settings");
    EXPECT_EQ(Storage.content("/.crosspoint/epub_123/progress"), "unchanged reading position");
    for (const auto& [path, entry] : Storage.entries) {
      if (path.starts_with("/.crosspoint/book-sync/")) EXPECT_TRUE(entry->directory) << path;
    }
  }
};
TEST_F(SyncActivityTest, USBBookAtDifferentPathIsSkippedAndProgressPreserved) {
  Storage.put("/USB library/Renamed book.EPUB", BODY);
  run("done");
  EXPECT_EQ(Storage.content("/USB library/Renamed book.EPUB"), BODY);
  EXPECT_FALSE(Storage.exists(("/Books/Sync/" + name()).c_str()));
  EXPECT_EQ(HttpDownloader::requests.size(), 1U);
  EXPECT_EQ(renderer.text[1], "0/1/0");
}
TEST_F(SyncActivityTest, AddsVerifiedBookAndSecondRunSkips) {
  Storage.put("/USB library/Other.epub", "existing unrelated content");
  run("done");
  EXPECT_EQ(Storage.content("/Books/Sync/" + name()), BODY);
  EXPECT_EQ(Storage.content("/USB library/Other.epub"), "existing unrelated content");
  EXPECT_EQ(renderer.text[1], "1/0/0");
  HttpDownloader::requests.clear();
  run("done");
  EXPECT_EQ(HttpDownloader::requests.size(), 1U);
  EXPECT_EQ(renderer.text[1], "0/1/0");
}
TEST_F(SyncActivityTest, ChangedExistingDestinationIsNotReplaced) {
  Storage.put("/Books/Sync/" + name(), "existing different edition");
  run("done");
  EXPECT_EQ(Storage.content("/Books/Sync/" + name()), "existing different edition");
  EXPECT_EQ(HttpDownloader::requests.size(), 1U);
  EXPECT_EQ(renderer.text[1], "0/0/1");
}
TEST_F(SyncActivityTest, DuplicateServerContentOnlyDownloadsOnce) {
  HttpDownloader::responses[ORIGIN + "/v1/manifest"] = "WITCHSYNC1\n" + row('b') + row('c') + "END\t2\n";
  HttpDownloader::responses[endpoint('c')] = BODY;
  run("done");
  EXPECT_EQ(HttpDownloader::requests.size(), 2U);
  EXPECT_EQ(renderer.text[1], "1/1/0");
}
TEST_F(SyncActivityTest, PartialDownloadNeverPublishes) {
  HttpDownloader::abortBook = true;
  run("failed");
  EXPECT_FALSE(Storage.exists(("/Books/Sync/" + name()).c_str()));
}
TEST_F(SyncActivityTest, HashMismatchNeverPublishes) {
  HttpDownloader::corruptBook = true;
  run("failed");
  EXPECT_FALSE(Storage.exists(("/Books/Sync/" + name()).c_str()));
}
TEST_F(SyncActivityTest, CancelDuringDownloadCleansStage) {
  HttpDownloader::cancelBook = true;
  run("cancelled");
  EXPECT_FALSE(Storage.exists(("/Books/Sync/" + name()).c_str()));
}
TEST_F(SyncActivityTest, IncompleteInventoryDoesNotDownloadAnyBooks) {
  HttpDownloader::responses[ORIGIN + "/v1/manifest"] = "WITCHSYNC1\n" + row();
  run("failed");
  EXPECT_EQ(HttpDownloader::requests.size(), 1U);
}
TEST_F(SyncActivityTest, InvalidInventoryDoesNotDownloadValidPrefix) {
  HttpDownloader::responses[ORIGIN + "/v1/manifest"] = "WITCHSYNC1\n" + row() + "malformed\nEND\t2\n";
  run("failed");
  EXPECT_EQ(HttpDownloader::requests.size(), 1U);
}
TEST_F(SyncActivityTest, NoSpaceAndRenameFailureKeepLibraryUntouched) {
  Storage.freeBytes = 0;
  run("failed");
  EXPECT_EQ(HttpDownloader::requests.size(), 1U);
  Storage.freeBytes = 1ULL << 30;
  Storage.failRename = true;
  run("failed");
  EXPECT_FALSE(Storage.exists(("/Books/Sync/" + name()).c_str()));
}
TEST_F(SyncActivityTest, NonHTTPSConfigFailsBeforeSendingCredentials) {
  Storage.put("/.crosspoint/book-sync.json",
              "{\"url\":\"http://example.test\",\"token\":\"" + std::string(64, 'a') + "\"}");
  run("failed");
  EXPECT_TRUE(HttpDownloader::requests.empty());
}
}  // namespace
