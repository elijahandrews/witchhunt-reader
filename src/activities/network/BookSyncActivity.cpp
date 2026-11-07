#include "BookSyncActivity.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <I18n.h>
#include <WiFi.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include <algorithm>
#include <cstring>

#include "SilentRestart.h"
#include "activities/NetworkMemoryTrim.h"
#include "activities/network/WifiSelectionActivity.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookSyncProtocol.h"

namespace {
constexpr char CONFIG[] = "/.crosspoint/book-sync.json";
constexpr char CA[] = "/.crosspoint/book-sync-ca.pem";
constexpr char TEMP_DIR[] = "/.crosspoint/book-sync";
constexpr char MANIFEST[] = "/.crosspoint/book-sync/manifest";
constexpr char INDEX[] = "/.crosspoint/book-sync/local.bin";
constexpr char STAGE[] = "/.crosspoint/book-sync/book";
constexpr char DEST_DIR[] = "/Books";
// A truncated/oversized line, embedded NUL, and EOF without a newline all fail closed.
bool readLine(FsFile& file, char* line) {
  for (size_t i = 0; i < BookSync::LINE_CAP - 1; ++i) {
    const int c = file.read();
    if (c == '\n') {
      line[i] = 0;
      return true;
    }
    if (c < 1 || c > 126) return false;
    line[i] = static_cast<char>(c);
  }
  return false;
}
void decodeHash(std::string_view hash, uint8_t* out) {
  const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (size_t i = 0; i < 32; ++i) out[i] = (nibble(hash[i * 2]) << 4) | nibble(hash[i * 2 + 1]);
}
void cleanTemps() {
  Storage.remove(MANIFEST);
  Storage.remove("/.crosspoint/book-sync/manifest.part");
  Storage.remove(INDEX);
  Storage.remove(STAGE);
  Storage.remove("/.crosspoint/book-sync/book.part");
}
}  // namespace

struct BookSyncActivity::Work {
  char line[BookSync::LINE_CAP]{};
  char name[256]{};
  uint8_t bytes[1024]{};
  uint8_t expected[32]{}, actual[32]{};
  wc_Sha256 sha{};
  BookSync::Manifest parser;
  BookSync::Entry entry;
  std::string url, token, ca;
  unsigned visited = 0, indexed = 0;
};

BookSyncActivity::BookSyncActivity(GfxRenderer& renderer, MappedInputManager& input)
    : UiListActivity("BookSync", renderer, input), status(StrId::STR_BOOK_SYNC_CONNECTING) {}
BookSyncActivity::~BookSyncActivity() = default;
const char* BookSyncActivity::headerTitle() const { return tr(STR_BOOK_SYNC); }
void BookSyncActivity::buildScreen(UiScreen& screen) { layoutListArea(screen); }
void BookSyncActivity::drawChrome() {
  UiListActivity::drawChrome();
  const int y = renderer.getScreenHeight() / 2;
  renderer.drawCenteredText(UI_10_FONT_ID, y - 35, I18N.get(status));
  char summary[128];
  snprintf(summary, sizeof(summary), tr(STR_BOOK_SYNC_COUNTS), added, skipped, conflicts);
  renderer.drawCenteredText(UI_10_FONT_ID, y + 5, summary);
}
void BookSyncActivity::show(StrId message) {
  {
    RenderLock lock(*this);
    status = message;
  }
  requestUpdateAndWait();
  lastPaint = millis();
}
void BookSyncActivity::onEnter() {
  UiListActivity::onEnter();
  releaseMemoryForDownload(renderer, "SYNC");
  work = std::make_unique<Work>();
  if (WiFi.status() == WL_CONNECTED) {
    connected(true);
    return;
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { connected(!result.isCancelled); });
}
void BookSyncActivity::onExit() {
  UiListActivity::onExit();
  cleanTemps();
  work.reset();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  // Like the font/OPDS activities: the network trim released buried reader state.
  silentRestart();
}
void BookSyncActivity::connected(bool success) {
  if (!success) {
    finish();
    return;
  }
  releaseMemoryForDownload(renderer, "SYNC");
  buttonEvents.drain();
  const bool ok = sync();
  cleanTemps();
  buttonEvents.drain();
  {
    RenderLock lock(*this);
    busy = false;
  }
  show(cancelled ? StrId::STR_BOOK_SYNC_CANCELLED : ok ? StrId::STR_BOOK_SYNC_DONE : StrId::STR_BOOK_SYNC_FAILED);
}
bool BookSyncActivity::pump() {
  mappedInput.update();
  buttonEvents.update();
  ButtonEventManager::ButtonEvent event;
  while (buttonEvents.consumeEvent(event)) {
    if (event.button == MappedInputManager::Button::Back) cancelled = true;
  }
  if (millis() - lastPaint > 2000) {
    requestUpdate(true);
    lastPaint = millis();
  }
  delay(1);
  return !cancelled;
}
bool BookSyncActivity::hashFile(FsFile& file, uint8_t* digest) {
  if (wc_InitSha256(&work->sha) != 0) return false;
  bool ok = true;
  while (file.available()) {
    const int n = file.read(work->bytes, sizeof(work->bytes));
    if (n <= 0 || wc_Sha256Update(&work->sha, work->bytes, n) != 0 || !pump()) {
      ok = false;
      break;
    }
  }
  if (ok) ok = wc_Sha256Final(&work->sha, digest) == 0;
  wc_Sha256Free(&work->sha);
  return ok;
}
bool BookSyncActivity::indexLocal(const std::string& path, FsFile& index, unsigned depth) {
  if (depth > 32) return false;
  FsFile directory;
  if (!Storage.openFileForRead("SYNC", path, directory) || !directory.isDirectory()) return false;
  while (auto file = directory.openNextFile()) {
    if (++work->visited > 100000 || !pump()) return false;
    const auto length = file.getName(work->name, sizeof(work->name));
    if (!length || length >= sizeof(work->name) - 1) return false;
    if (work->name[0] == '.') continue;
    if (file.isDirectory()) {
      const std::string child = path + (path == "/" ? "" : "/") + work->name;
      file.close();
      if (!indexLocal(child, index, depth + 1)) return false;
    } else if (length >= 5 && strcasecmp(work->name + length - 5, ".epub") == 0 &&
               file.size64() <= BookSync::MAX_BOOK_BYTES) {
      if (++work->indexed > BookSync::MAX_BOOKS || !hashFile(file, work->actual) || index.write(work->actual, 32) != 32)
        return false;
    }
  }
  return true;
}
bool BookSyncActivity::containsHash(FsFile& index, const uint8_t* digest, bool& found) {
  found = false;
  if (!index.seek(0)) return false;
  while (index.available()) {
    const int n = index.read(work->bytes, sizeof(work->bytes));
    if (n <= 0 || n % 32 || !pump()) return false;
    for (int i = 0; i < n; i += 32) {
      if (memcmp(work->bytes + i, digest, 32) == 0) {
        found = true;
        return true;
      }
    }
  }
  return true;
}
bool BookSyncActivity::sync() {
  std::string config;
  if (!Storage.readFileToString("SYNC", CONFIG, 1024, config)) return false;
  {
    JsonDocument doc;
    if (deserializeJson(doc, config) || !doc["url"].is<const char*>() || !doc["token"].is<const char*>()) return false;
    work->url = doc["url"].as<std::string>();
    work->token = doc["token"].as<std::string>();
  }
  config.clear();
  if (!BookSync::origin(work->url) || work->token.size() != 64 || !BookSync::hex(work->token) ||
      !Storage.readFileToString("SYNC", CA, 4096, work->ca) || !Storage.ensureDirectoryExists(TEMP_DIR) ||
      !Storage.ensureDirectoryExists(DEST_DIR))
    return false;
  cleanTemps();
  show(StrId::STR_BOOK_SYNC_LIST);
  HttpDownloader::Session session(work->ca.c_str());
  if (HttpDownloader::downloadToFile(
          session, work->url + "/v1/manifest", MANIFEST,
          [this](unsigned n, unsigned total) {
            return n <= BookSync::MAX_MANIFEST_BYTES && total <= BookSync::MAX_MANIFEST_BYTES && pump();
          },
          "reader", work->token, HttpDownloader::TlsPolicy::Strict) != HttpDownloader::OK)
    return false;
  FsFile manifest;
  if (!Storage.openFileForRead("SYNC", MANIFEST, manifest) || manifest.fileSize() > BookSync::MAX_MANIFEST_BYTES)
    return false;
  // Validate the COMPLETE manifest before downloading anything. No partial inventory can pass.
  while (manifest.available()) {
    if (!readLine(manifest, work->line) ||
        work->parser.accept(work->line, work->entry) == BookSync::Manifest::Line::Invalid || !pump())
      return false;
  }
  if (!work->parser.ended || !manifest.seek(0)) return false;
  show(StrId::STR_BOOK_SYNC_CHECKING);
  FsFile index;
  if (!Storage.openFileForWrite("SYNC", INDEX, index) || !indexLocal("/", index) || !index.close() ||
      !Storage.openFileForUpdate("SYNC", INDEX, index))
    return false;
  show(StrId::STR_BOOK_SYNC_DOWNLOADING);
  work->parser = {};
  while (manifest.available()) {
    if (!readLine(manifest, work->line) || !pump()) return false;
    const auto kind = work->parser.accept(work->line, work->entry);
    if (kind == BookSync::Manifest::Line::Invalid) return false;
    if (kind != BookSync::Manifest::Line::Book) continue;
    decodeHash(work->entry.hash, work->expected);
    bool found;
    if (!containsHash(index, work->expected, found)) return false;
    if (found) {
      RenderLock lock(*this);
      ++skipped;
      continue;
    }
    const std::string destination = std::string(DEST_DIR) + "/" + std::string(work->entry.name);
    if (Storage.exists(destination.c_str())) {
      RenderLock lock(*this);
      ++conflicts;
      continue;
    }
    if (Storage.sdFreeBytes() < uint64_t(work->entry.size) + 1024 * 1024) return false;
    const std::string url =
        work->url + "/v1/files/" + std::string(work->entry.id) + "/" + std::string(work->entry.hash);
    const uint32_t size = work->entry.size;
    if (HttpDownloader::downloadToFile(
            session, url, STAGE,
            [this, size](unsigned n, unsigned total) { return n <= size && total <= size && pump(); }, "reader",
            work->token, HttpDownloader::TlsPolicy::Strict) != HttpDownloader::OK)
      return false;
    FsFile book;
    if (!Storage.openFileForRead("SYNC", STAGE, book) || book.fileSize() != size || book.read(work->bytes, 4) != 4 ||
        memcmp(work->bytes, "PK\x03\x04", 4) || !book.seek(0) || !hashFile(book, work->actual) ||
        memcmp(work->actual, work->expected, 32) || !book.close())
      return false;
    // Recheck just before publication. Never use the downloader's replacing rename on a book.
    if (Storage.exists(destination.c_str()) || !Storage.rename(STAGE, destination.c_str())) return false;
    {
      RenderLock lock(*this);
      ++added;
    }
    // Include new books in deduplication for subsequent same-content entries in this inventory.
    if (!index.seek(index.fileSize()) || index.write(work->expected, 32) != 32) return false;
    index.flush();
  }
  return work->parser.ended;
}
