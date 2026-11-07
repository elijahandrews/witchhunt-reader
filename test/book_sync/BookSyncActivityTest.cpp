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
    injectTouch = injectBackPress = false;
    fakeMillis = delayCalls = 0;
    tapX = 50;
    tapY = 780;
    ButtonHintStrip::invalidate();
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
  void serve(const std::string& body) {
    HttpDownloader::responses.clear();
    HttpDownloader::responses[ORIGIN + "/v1/manifest"] =
        "WITCHSYNC1\n" + digest(body) + "\t" + std::to_string(body.size()) + "\t" + name() + "\nEND\t1\n";
    HttpDownloader::responses[ORIGIN + "/v1/files/" + std::string(64, 'b') + "/" + digest(body)] = body;
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
  EXPECT_FALSE(Storage.exists(("/Books/" + name()).c_str()));
  EXPECT_EQ(HttpDownloader::requests.size(), 1U);
  EXPECT_EQ(renderer.text[1], "0/1/0");
}
TEST_F(SyncActivityTest, AddsVerifiedBookAndSecondRunSkips) {
  Storage.put("/USB library/Other.epub", "existing unrelated content");
  run("done");
  EXPECT_EQ(Storage.content("/Books/" + name()), BODY);
  EXPECT_EQ(Storage.content("/USB library/Other.epub"), "existing unrelated content");
  EXPECT_EQ(renderer.text[1], "1/0/0");
  HttpDownloader::requests.clear();
  run("done");
  EXPECT_EQ(HttpDownloader::requests.size(), 1U);
  EXPECT_EQ(renderer.text[1], "0/1/0");
}
TEST_F(SyncActivityTest, ChangedExistingDestinationIsNotReplaced) {
  Storage.put("/Books/" + name(), "existing different edition");
  run("done");
  EXPECT_EQ(Storage.content("/Books/" + name()), "existing different edition");
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
  EXPECT_FALSE(Storage.exists(("/Books/" + name()).c_str()));
}
TEST_F(SyncActivityTest, HashMismatchNeverPublishes) {
  HttpDownloader::corruptBook = true;
  run("failed");
  EXPECT_FALSE(Storage.exists(("/Books/" + name()).c_str()));
}
TEST_F(SyncActivityTest, CancelDuringDownloadCleansStage) {
  HttpDownloader::cancelBook = true;
  run("cancelled");
  EXPECT_FALSE(Storage.exists(("/Books/" + name()).c_str()));
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
  EXPECT_FALSE(Storage.exists(("/Books/" + name()).c_str()));
}
TEST_F(SyncActivityTest, NonHTTPSConfigFailsBeforeSendingCredentials) {
  Storage.put("/.crosspoint/book-sync.json",
              "{\"url\":\"http://example.test\",\"token\":\"" + std::string(64, 'a') + "\"}");
  run("failed");
  EXPECT_TRUE(HttpDownloader::requests.empty());
}
TEST_F(SyncActivityTest, UnrelatedSizesDoNotReadBookContents) {
  Storage.put("/Books/unrelated.epub", std::string(2 * 1024 * 1024, 'x'));
  Storage.put("/Books/match.epub", BODY);
  run("done");
  EXPECT_EQ(readBytes["/Books/unrelated.epub"], 0U);
  EXPECT_EQ(readBytes["/Books/match.epub"], BODY.size());
  EXPECT_EQ(HttpDownloader::requests.size(), 1U);
}
TEST_F(SyncActivityTest, EmptyManifestDoesNotReadLocalBooks) {
  HttpDownloader::responses[ORIGIN + "/v1/manifest"] = "WITCHSYNC1\nEND\t0\n";
  Storage.put("/Books/local.epub", BODY);
  run("done");
  EXPECT_EQ(readBytes["/Books/local.epub"], 0U);
}
TEST_F(SyncActivityTest, LargeCandidateUsesBulkReadsWithoutSleepPerChunk) {
  const auto body = BODY + std::string(1024 * 1024, 'x');
  serve(body);
  Storage.put("/Books/large.epub", body);
  run("done");
  EXPECT_EQ(readBytes["/Books/large.epub"], body.size());
  EXPECT_LE(readCalls["/Books/large.epub"], 130U);
  EXPECT_LT(delayCalls, 100U);
  EXPECT_EQ(HttpDownloader::requests.size(), 1U);
  RecordProperty("epub_read_calls", static_cast<int>(readCalls["/Books/large.epub"]));
  RecordProperty("cooperative_delay_calls", static_cast<int>(delayCalls));
}
TEST_F(SyncActivityTest, SameSizeDifferentContentIsStillFullyChecked) {
  std::string other = BODY;
  other.back() ^= 1;
  Storage.put("/Books/different.epub", other);
  run("done");
  EXPECT_EQ(readBytes["/Books/different.epub"], BODY.size());
  EXPECT_EQ(Storage.content("/Books/" + name()), BODY);
  EXPECT_EQ(Storage.content("/Books/different.epub"), other);
}
TEST_F(SyncActivityTest, TapCancelInterruptsHashWithinOneReadAndRetryWorks) {
  const auto body = BODY + std::string(1024 * 1024, 'x');
  serve(body);
  Storage.put("/Books/large.epub", body);
  input.backRawIndex = 2;  // Honor the drawn/remapped Back slot.
  tapX = 250;
  afterRead = [](const std::string& path, size_t position) {
    if (path == "/Books/large.epub" && position >= 32768) injectTouch = true;
  };
  run("cancelled");
  EXPECT_LE(readBytes["/Books/large.epub"], 32768U);
  EXPECT_EQ(HttpDownloader::requests.size(), 1U);
  EXPECT_EQ(Storage.content("/Books/large.epub"), body);
  afterRead = {};
  injectTouch = false;
  run("done");
  EXPECT_FALSE(Storage.exists(("/Books/" + name()).c_str()));
}
TEST_F(SyncActivityTest, BackPressInterruptsHashWithoutWaitingForRelease) {
  const auto body = BODY + std::string(128 * 1024, 'x');
  serve(body);
  Storage.put("/Books/large.epub", body);
  afterRead = [](const std::string& path, size_t) {
    if (path == "/Books/large.epub") injectBackPress = true;
  };
  run("cancelled");
  EXPECT_LE(readBytes["/Books/large.epub"], 8192U);
  EXPECT_EQ(HttpDownloader::requests.size(), 1U);
}
TEST_F(SyncActivityTest, TapsOutsideCancelDoNotAbort) {
  Storage.put("/Books/match.epub", BODY);
  tapY = 200;
  afterRead = [](const std::string& path, size_t) {
    if (path == "/Books/match.epub") injectTouch = true;
  };
  run("done");
  EXPECT_EQ(renderer.text[1], "0/1/0");
}
TEST_F(SyncActivityTest, CancelDuringDirectoryScanBeforeReadingAnyBook) {
  Storage.put("/Books/unrelated.epub", std::string(100000, 'x'));
  afterDirectoryEntry = [](const std::string& path) {
    if (path == "/Books/unrelated.epub") injectTouch = true;
  };
  run("cancelled");
  EXPECT_EQ(readBytes["/Books/unrelated.epub"], 0U);
  EXPECT_EQ(HttpDownloader::requests.size(), 1U);
}
TEST_F(SyncActivityTest, SlowScanShowsAdvancingProgressAndCancelLabel) {
  const auto body = BODY + std::string(2 * 1024 * 1024, 'x');
  serve(body);
  Storage.put("/Books/large.epub", body);
  afterRead = [this](const std::string& path, size_t) {
    if (path == "/Books/large.epub") {
      EXPECT_EQ(renderer.footer, "cancel");
      fakeMillis += 50;
    }
  };
  run("done");
  EXPECT_NE(std::find(renderer.history.begin(), renderer.history.end(), "Checked: 1  Hashed: 1 MiB"),
            renderer.history.end());
  EXPECT_EQ(renderer.footer, "back");
}
}  // namespace
