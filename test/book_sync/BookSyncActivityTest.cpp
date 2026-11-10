#include <ArduinoJson.h>
#include <LibraryBuilder.h>
#include <Memory.h>
#include <WiFi.h>
#include <gtest/gtest.h>
#include <mbedtls/sha256.h>
#include <network/HttpDownloader.h>
#include <openssl/sha.h>

#include "src/activities/network/BookSyncActivity.h"
#include "util/BookSyncHash.h"

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
    library::invalidations = 0;
    allocationCalls = failAllocationAt = 0;
    wifiConnected = true;
    failHashStart = failHashUpdate = failHashFinish = false;
    injectCancel = false;
    injectTouch = injectBackPress = false;
    fakeMillis = delayCalls = 0;
    tapX = 50;
    tapY = 740;
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
      if (path.starts_with("/.crosspoint/book-sync/") && !path.ends_with("/hash-index") &&
          !path.ends_with("/last-run.json"))
        EXPECT_TRUE(entry->directory) << path;
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
  EXPECT_EQ(library::invalidations, 1U);
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
  // Hit the native touch Cancel button.
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

JsonDocument report() {
  JsonDocument doc;
  EXPECT_FALSE(deserializeJson(doc, Storage.content("/.crosspoint/book-sync/last-run.json")));
  return doc;
}
TEST_F(SyncActivityTest, DirectoryReadErrorDoesNotDownloadDuplicateOrSealPartialIndex) {
  Storage.put("/Books/first.epub", BODY);
  Storage.put("/Books/second.epub", BODY + "other");
  serve(BODY + "other");
  failDirectoryRead = "/Books";
  failDirectoryAfterEntries = 1;
  run("failed");
  EXPECT_EQ(HttpDownloader::requests.size(), 1U);
  EXPECT_FALSE(Storage.exists(("/Books/" + name()).c_str()));
  EXPECT_FALSE(Storage.seal.valid());
  failDirectoryRead.clear();
  run("done");
  EXPECT_EQ(renderer.text[1], "0/1/0");
  EXPECT_TRUE(Storage.seal.valid());
}
TEST_F(SyncActivityTest, DirectoryReadErrorCannotSealAnEmptyConflictIndex) {
  Storage.put("/Books/" + name(), "different edition at reserved destination");
  failDirectoryRead = "/Books";
  run("failed");
  EXPECT_EQ(HttpDownloader::requests.size(), 1U);
  EXPECT_FALSE(Storage.seal.valid());
  EXPECT_EQ(Storage.content("/Books/" + name()), "different edition at reserved destination");
}
TEST_F(SyncActivityTest, ImmediateRepeatReadsNoBookBytes) {
  Storage.put("/Books/match.epub", BODY);
  run("done");
  ASSERT_TRUE(Storage.seal.valid());
  readBytes.clear();
  run("done");
  EXPECT_EQ(readBytes["/Books/match.epub"], 0U);
  EXPECT_TRUE(report()["cache_hit"].as<bool>());
  EXPECT_EQ(report()["local_bytes"].as<uint64_t>(), 0U);
  EXPECT_EQ(renderer.text[1], "0/1/0");
}
TEST_F(SyncActivityTest, OnlyExplicitlyArmedSoftwareRestartPreservesCache) {
  Storage.put("/Books/match.epub", BODY);
  run("done");
  Storage.armContentHashRestart();
  Storage.boot(true);
  readBytes.clear();
  run("done");
  EXPECT_EQ(readBytes["/Books/match.epub"], 0U);
  Storage.boot(true);  // The previous permission was consumed.
  readBytes.clear();
  run("done");
  EXPECT_EQ(readBytes["/Books/match.epub"], BODY.size());
  Storage.armContentHashRestart();
  Storage.boot(false);  // Cold boot, deep sleep and watchdog resets all take this path.
  readBytes.clear();
  run("done");
  EXPECT_EQ(readBytes["/Books/match.epub"], BODY.size());
}
TEST_F(SyncActivityTest, USBSessionInvalidatesEvenForSameSizeExternalEdit) {
  Storage.put("/Books/match.epub", BODY);
  run("done");
  Storage.usbSession();
  auto& bytes = Storage.entries.at(Storage.key("/Books/match.epub"))->data;
  bytes.back() ^= 1;  // External edits do not call HAL file mutators.
  run("done");
  EXPECT_FALSE(report()["cache_hit"].as<bool>());
  EXPECT_EQ(Storage.content("/Books/" + name()), BODY);
  EXPECT_EQ(renderer.text[1], "1/0/0");
}
TEST_F(SyncActivityTest, DeletingLocalMatchCannotLeaveTrustedDigest) {
  Storage.put("/Books/match.epub", BODY);
  run("done");
  Storage.remove("/Books/match.epub");
  run("done");
  EXPECT_EQ(Storage.content("/Books/" + name()), BODY);
}
TEST_F(SyncActivityTest, PrivateWriterRenamedIntoLibraryPreventsSealingUntilClosed) {
  Storage.put("/.crosspoint/staged", BODY);
  FsFile writer;
  ASSERT_TRUE(Storage.openFileForUpdate("TEST", "/.crosspoint/staged", writer));
  ASSERT_TRUE(Storage.rename("/.crosspoint/staged", "/Books/match.epub"));
  run("failed");
  EXPECT_FALSE(Storage.seal.valid());
  ASSERT_TRUE(writer.seek(BODY.size() - 1));
  const char changed = BODY.back() ^ 1;
  ASSERT_EQ(writer.write(&changed, 1), 1U);
  writer.close();
  run("done");
  EXPECT_EQ(Storage.content("/Books/" + name()), BODY);
}
TEST_F(SyncActivityTest, PrivateStateWritesDoNotInvalidateClosedIndex) {
  Storage.put("/Books/match.epub", BODY);
  run("done");
  Storage.put("/.crosspoint/some-other-progress", "new position");
  readBytes.clear();
  run("done");
  EXPECT_TRUE(report()["cache_hit"].as<bool>());
  EXPECT_EQ(readBytes["/Books/match.epub"], 0U);
}
TEST_F(SyncActivityTest, TruncatedOrCorruptCacheFallsBackToFullHash) {
  Storage.put("/Books/match.epub", BODY);
  run("done");
  Storage.entries.at("/.crosspoint/book-sync/hash-index")->data.back() ^= 1;
  readBytes.clear();
  run("done");
  EXPECT_FALSE(report()["cache_hit"].as<bool>());
  EXPECT_EQ(readBytes["/Books/match.epub"], BODY.size());
  Storage.entries.at("/.crosspoint/book-sync/hash-index")->data.resize(512);
  readBytes.clear();
  run("done");
  EXPECT_EQ(readBytes["/Books/match.epub"], BODY.size());
}
TEST_F(SyncActivityTest, NewCandidateSizesRequireNewLocalIndex) {
  Storage.put("/Books/match.epub", BODY);
  run("done");
  serve(BODY + "more");
  run("done");
  EXPECT_FALSE(report()["cache_hit"].as<bool>());
  EXPECT_EQ(Storage.content("/Books/" + name()), BODY + "more");
}
TEST_F(SyncActivityTest, ChangedContentWithSameCandidateSizeIsStillDownloaded) {
  Storage.put("/Books/match.epub", BODY);
  run("done");
  std::string changed = BODY;
  changed.back() ^= 1;
  serve(changed);
  run("done");
  EXPECT_TRUE(report()["cache_hit"].as<bool>());
  EXPECT_EQ(Storage.content("/Books/" + name()), changed);
  EXPECT_EQ(Storage.content("/Books/match.epub"), BODY);
}
TEST_F(SyncActivityTest, MutationDuringCachedIndexCopyForcesRescan) {
  Storage.put("/Books/match.epub", BODY);
  run("done");
  bool changed = false;
  afterRead = [&changed](const std::string& path, size_t) {
    if (path == "/.crosspoint/book-sync/hash-index" && !changed) {
      changed = true;
      Storage.remove("/Books/match.epub");
    }
  };
  run("failed");  // A concurrent library change aborts this attempt safely.
  afterRead = {};
  run("done");
  EXPECT_EQ(Storage.content("/Books/" + name()), BODY);
}
TEST_F(SyncActivityTest, CancellationDuringCachedIndexCopyKeepsBooksUntouched) {
  Storage.put("/Books/match.epub", BODY);
  run("done");
  afterRead = [](const std::string& path, size_t) {
    if (path == "/.crosspoint/book-sync/hash-index") injectBackPress = true;
  };
  run("cancelled");
  EXPECT_EQ(Storage.content("/Books/match.epub"), BODY);
  EXPECT_TRUE(report()["cancelled"].as<bool>());
}
TEST_F(SyncActivityTest, BufferedManifestRetainsLinesAcrossIndexReads) {
  std::string manifest = "WITCHSYNC1\n";
  for (char c : std::string("0123456789abcdef")) manifest += row(c);
  HttpDownloader::responses[ORIGIN + "/v1/manifest"] = manifest + "END\t16\n";
  Storage.put("/Books/match.epub", BODY);
  run("done");
  EXPECT_EQ(renderer.text[1], "0/16/0");
  EXPECT_LT(readCalls["/.crosspoint/book-sync/manifest"], 20U);
}
TEST_F(SyncActivityTest, CryptoFailureFailsClosedAndReportsElapsedFailedPhase) {
  failHashFinish = true;
  run("failed");
  EXPECT_FALSE(Storage.exists(("/Books/" + name()).c_str()));
  EXPECT_FALSE(report()["success"].as<bool>());
  EXPECT_GT(report()["verify_ms"].as<unsigned>(), 0U);
  failHashFinish = false;
  HttpDownloader::responses[ORIGIN + "/v1/manifest"] = "WITCHSYNC1\ninvalid\n";
  run("failed");
  EXPECT_GT(report()["manifest_ms"].as<unsigned>(), 0U);
}
TEST(BookSyncCacheSeal, RejectsMalformedLengthsAndConsumesRestartPermission) {
  ContentHashCacheSeal seal{};
  uint8_t digest[32]{};
  for (uint32_t bytes : {0U, 511U, 513U, ContentHashCacheSeal::MAX_BYTES + 32}) {
    seal.set(digest, bytes);
    EXPECT_FALSE(seal.valid());
  }
  seal.set(digest, 544);
  seal.armRestart();
  seal.boot(true);
  EXPECT_TRUE(seal.valid());
  seal.boot(true);
  EXPECT_FALSE(seal.valid());
}
TEST(BookSyncHash, KnownAnswersAndFragmentedMultiBlockInput) {
  for (const std::string& input : {std::string(), std::string("abc"), std::string(100000, 'a')}) {
    BookSyncHash sha;
    ASSERT_TRUE(sha.start());
    for (size_t i = 0; i < input.size(); i += 17)
      ASSERT_TRUE(
          sha.update(reinterpret_cast<const uint8_t*>(input.data() + i), std::min<size_t>(17, input.size() - i)));
    uint8_t actual[32]{}, expected[32]{};
    ASSERT_TRUE(sha.finish(actual));
    sha.free();
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), expected);
    EXPECT_EQ(memcmp(actual, expected, 32), 0);
  }
}
}  // namespace

TEST_F(SyncActivityTest, WifiScreenAllocationFailureShowsFailureWithoutStartingSync) {
  wifiConnected = false;
  failAllocationAt = 2;  // Work succeeds; native Wi-Fi activity cannot be allocated.
  BookSyncActivity activity(renderer, input);
  activity.onEnter();
  EXPECT_FALSE(activity.preventAutoSleep());
  ASSERT_GE(renderer.text.size(), 2U);
  EXPECT_EQ(renderer.text[0], "failed");
  EXPECT_TRUE(HttpDownloader::requests.empty());
}
