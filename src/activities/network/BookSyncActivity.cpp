#include "BookSyncActivity.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <cstring>
#include <new>

#include "SilentRestart.h"
#include "activities/NetworkMemoryTrim.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/themes/ButtonHintStrip.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookSyncHash.h"
#include "util/BookSyncProtocol.h"

namespace {
constexpr char CONFIG[] = "/.crosspoint/book-sync.json";
constexpr char CA[] = "/.crosspoint/book-sync-ca.pem";
constexpr char TEMP_DIR[] = "/.crosspoint/book-sync";
constexpr char MANIFEST[] = "/.crosspoint/book-sync/manifest";
constexpr char INDEX[] = "/.crosspoint/book-sync/local.bin";
constexpr char CACHE[] = "/.crosspoint/book-sync/hash-index";
constexpr char REPORT[] = "/.crosspoint/book-sync/last-run.json";
constexpr char STAGE[] = "/.crosspoint/book-sync/book";
constexpr char DEST_DIR[] = "/Books";
struct PhaseTimer {
  uint32_t& elapsed;
  unsigned long started = millis();
  explicit PhaseTimer(uint32_t& elapsed) : elapsed(elapsed) {}
  ~PhaseTimer() { elapsed += millis() - started; }
};
// A truncated/oversized line, embedded NUL, and EOF without a newline all fail closed.
struct ManifestReader {
  // Independent of the 8 KiB hash buffer: its unread bytes survive book/index
  // reads between manifest lines. A fixed 512 B avoids a library-sized allocation.
  uint8_t bytes[512]{};
  size_t position = 0, length = 0;
  void reset() { position = length = 0; }
  bool available(FsFile& file) const { return position < length || file.available(); }
  bool readLine(FsFile& file, char* line) {
    for (size_t i = 0; i < BookSync::LINE_CAP - 1; ++i) {
      if (position == length) {
        const int n = file.read(bytes, sizeof(bytes));
        if (n <= 0) return false;
        position = 0;
        length = n;
      }
      const int c = bytes[position++];
      if (c == '\n') {
        line[i] = 0;
        return true;
      }
      if (c < 1 || c > 126) return false;
      line[i] = static_cast<char>(c);
    }
    return false;
  }
};
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
  // One reusable heap buffer: 8 KiB reads reduce SD call overhead without a
  // library-sized allocation or putting the transfer buffer on the loop stack.
  uint8_t bytes[8192]{};
  uint8_t expected[32]{}, actual[32]{};
  BookSyncHash sha;
  ManifestReader lines;
  uint32_t indexGeneration = 0;
  BookSync::Manifest parser;
  BookSync::Entry entry;
  BookSync::CandidateSizes sizes;
  std::string url, token, ca;
  unsigned visited = 0, indexed = 0;
  uint64_t hashedBytes = 0;
};

BookSyncActivity::BookSyncActivity(GfxRenderer& renderer, MappedInputManager& input)
    : UiListActivity("BookSync", renderer, input), status(StrId::STR_BOOK_SYNC_CONNECTING) {}
BookSyncActivity::~BookSyncActivity() = default;
const char* BookSyncActivity::headerTitle() const { return tr(STR_BOOK_SYNC); }
const char* BookSyncActivity::footerBackLabel() const {
  return busy ? tr(STR_CANCEL) : UiListActivity::footerBackLabel();
}
void BookSyncActivity::buildScreen(UiScreen& screen) { layoutListArea(screen); }
void BookSyncActivity::drawChrome() {
  UiListActivity::drawChrome();
  const int y = renderer.getScreenHeight() / 2;
  renderer.drawCenteredText(UI_10_FONT_ID, y - 35, I18N.get(status));
  char summary[128];
  if (status == StrId::STR_BOOK_SYNC_CHECKING) {
    snprintf(summary, sizeof(summary), tr(STR_BOOK_SYNC_SCAN_PROGRESS), checkedShown, hashedMiBShown);
  } else {
    snprintf(summary, sizeof(summary), tr(STR_BOOK_SYNC_COUNTS), added, skipped, conflicts);
  }
  renderer.drawCenteredText(UI_10_FONT_ID, y + 5, summary);
}
void BookSyncActivity::show(StrId message) {
  {
    RenderLock lock(*this);
    status = message;
  }
  const auto before = millis();
  requestUpdateAndWait();
  metrics.paintWaitMs += millis() - before;
  lastPaint = millis();
}
void BookSyncActivity::onEnter() {
  startedAt = millis();
  UiListActivity::onEnter();
  releaseMemoryForDownload(renderer, "SYNC");
  work.reset(new (std::nothrow) Work());
  if (!work) {
    {
      RenderLock lock(*this);
      busy = false;
    }
    show(StrId::STR_BOOK_SYNC_FAILED);
    return;
  }
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
  Storage.armContentHashRestart();
  silentRestart();
}
void BookSyncActivity::connected(bool success) {
  metrics.wifiMs = millis() - startedAt;
  if (!success) {
    saveMetrics(false);
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
  metrics.totalMs = millis() - startedAt;
  saveMetrics(ok);
}
bool BookSyncActivity::pump() {
  mappedInput.update();
  // sync() runs inside onEnter/the Wi-Fi result callback. ActivityManager's
  // normal hint-strip dispatcher cannot run until it returns, so route Cancel
  // here in the same portrait coordinates and raw-button mapping as that path.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) cancelled = true;
  int x = 0, y = 0;
  if (ButtonHintStrip::hasStrip() && mappedInput.wasScreenTappedIn(touchtransform::Portrait, x, y) &&
      ButtonHintStrip::hitTest(x, y) == mappedInput.rawIndex(MappedInputManager::Button::Back))
    cancelled = true;
  buttonEvents.update();
  ButtonEventManager::ButtonEvent event;
  while (buttonEvents.consumeEvent(event)) {
    if (event.button == MappedInputManager::Button::Back) cancelled = true;
  }
  if (cancelled) return false;
  const auto now = millis();
  if (now - lastPaint > 2000) {
    {
      RenderLock lock(*this);
      checkedShown = work->indexed;
      hashedMiBShown = static_cast<unsigned>(work->hashedBytes / (1024 * 1024));
    }
    requestUpdate(true);
    lastPaint = now;
  }
  // Poll input after every bounded read, but yield by elapsed time rather than
  // sleeping once per KiB. Wi-Fi and the input sampler still get regular time.
  if (now - lastYield >= 20) {
    delay(1);
    lastYield = millis();
  }
  return true;
}
bool BookSyncActivity::hashFile(FsFile& file, uint8_t* digest, HashKind kind) {
  if (!work->sha.start()) {
    work->sha.free();
    return false;
  }
  bool ok = true;
  uint64_t remaining = file.size64();
  while (remaining) {
    if (!pump()) {
      ok = false;
      break;
    }
    const auto beforeRead = micros();
    const int n = file.read(work->bytes, std::min<uint64_t>(remaining, sizeof(work->bytes)));
    if (kind != HashKind::Index) {
      metrics.readUs += uint32_t(micros() - beforeRead);
      ++metrics.readCalls;
    }
    if (n <= 0 || static_cast<uint64_t>(n) > remaining) {
      ok = false;
      break;
    }
    const auto beforeHash = micros();
    ok = work->sha.update(work->bytes, n);
    if (kind != HashKind::Index) metrics.hashUs += uint32_t(micros() - beforeHash);
    if (!ok) break;
    remaining -= n;
    if (kind == HashKind::Local) {
      work->hashedBytes += n;
      metrics.localBytes += n;
    } else if (kind == HashKind::Verify) {
      metrics.verifyBytes += n;
    }
  }
  if (ok) ok = pump() && work->sha.finish(digest);
  work->sha.free();
  return ok;
}

bool BookSyncActivity::loadIndex(FsFile& index) {
  uint32_t bytes = 0;
  if (!Storage.loadContentHashSeal(work->expected, bytes)) return false;
  FsFile cache;
  if (!Storage.openFileForRead("SYNC", CACHE, cache) || cache.fileSize() != bytes) return false;
  static_assert(sizeof(work->sizes) == 512);
  if (cache.read(work->bytes, sizeof(work->sizes)) != sizeof(work->sizes) ||
      memcmp(work->bytes, &work->sizes, sizeof(work->sizes)) || !Storage.openFileForWrite("SYNC", INDEX, index))
    return false;
  // Hash the exact bytes copied, in one pass; never validate and then reread a
  // mutable cache. The retained digest protects against interrupted SD writes.
  bool ok = work->sha.start();
  if (ok) ok = work->sha.update(work->bytes, sizeof(work->sizes));
  uint32_t remaining = bytes - sizeof(work->sizes);
  while (ok && remaining) {
    if (!pump()) {
      ok = false;
      break;
    }
    const int n = cache.read(work->bytes, std::min<size_t>(remaining, sizeof(work->bytes)));
    ok = n > 0 && n % 32 == 0 && uint32_t(n) <= remaining && work->sha.update(work->bytes, n) &&
         index.write(work->bytes, n) == size_t(n);
    if (ok) remaining -= n;
  }
  if (ok) ok = work->sha.finish(work->actual) && memcmp(work->expected, work->actual, 32) == 0;
  work->sha.free();
  if (!index.close()) ok = false;
  // No writable handles may remain when trusting a seal. This includes private
  // handles that could have been renamed into the public library while open.
  uint32_t currentBytes = 0;
  return ok && Storage.contentGeneration() == work->indexGeneration &&
         Storage.loadContentHashSeal(work->expected, currentBytes) && bytes == currentBytes &&
         memcmp(work->expected, work->actual, 32) == 0;
}

void BookSyncActivity::saveIndex(FsFile& index) {
  // Adding a book invalidated trust. Rebuild once on the next zero-addition run;
  // never bless an index across concurrent content writes or a failed read.
  if (added || cancelled || metrics.cacheHit || Storage.contentGeneration() != work->indexGeneration) return;
  FsFile cache;
  if (!index.seek(0) || !Storage.openFileForWrite("SYNC", CACHE, cache)) return;
  bool ok = work->sha.start();
  if (ok)
    ok = work->sha.update(reinterpret_cast<const uint8_t*>(&work->sizes), sizeof(work->sizes)) &&
         cache.write(&work->sizes, sizeof(work->sizes)) == sizeof(work->sizes);
  while (ok && index.available()) {
    if (!pump()) {
      ok = false;
      break;
    }
    const int n = index.read(work->bytes, sizeof(work->bytes));
    ok = n > 0 && n % 32 == 0 && work->sha.update(work->bytes, n) && cache.write(work->bytes, n) == size_t(n);
  }
  const uint32_t bytes = sizeof(work->sizes) + index.fileSize();
  if (ok) ok = work->sha.finish(work->actual);
  work->sha.free();
  if (!cache.close() || !index.close()) ok = false;
  if (ok) Storage.saveContentHashSeal(work->indexGeneration, work->actual, bytes);
}

void BookSyncActivity::appendDebugState(std::string& out) const {
  char text[240];
  snprintf(text, sizeof(text),
           " sync_busy=%u sync_cancelled=%u added=%u existing=%u conflicts=%u total_ms=%u wifi_ms=%u cache_hit=%u",
           busy, cancelled, added, skipped, conflicts, metrics.totalMs, metrics.wifiMs, metrics.cacheHit);
  out += text;
  snprintf(text, sizeof(text), " manifest_ms=%u scan_ms=%u download_ms=%u verify_ms=%u", metrics.manifestMs,
           metrics.scanMs, metrics.downloadMs, metrics.verifyMs);
  out += text;
  snprintf(text, sizeof(text),
           " read_us=%llu hash_us=%llu local_bytes=%llu verify_bytes=%llu reads=%u cache_ms=%u space_ms=%u paint_ms=%u",
           static_cast<unsigned long long>(metrics.readUs), static_cast<unsigned long long>(metrics.hashUs),
           static_cast<unsigned long long>(metrics.localBytes), static_cast<unsigned long long>(metrics.verifyBytes),
           metrics.readCalls, metrics.cacheMs, metrics.freeSpaceMs, metrics.paintWaitMs);
  out += text;
}

void BookSyncActivity::saveMetrics(bool success) {
  // Aggregate numbers only, once per attempt. No names, URLs, paths or credentials.
  // Available even when informational serial logs are compiled out on X4 Pro.
  metrics.totalMs = millis() - startedAt;
  JsonDocument doc;
  doc["schema"] = 1;
  doc["success"] = success;
  doc["cancelled"] = cancelled;
  doc["total_ms"] = metrics.totalMs;
  doc["wifi_ms"] = metrics.wifiMs;
  doc["manifest_ms"] = metrics.manifestMs;
  doc["scan_ms"] = metrics.scanMs;
  doc["download_ms"] = metrics.downloadMs;
  doc["verify_ms"] = metrics.verifyMs;
  doc["cache_ms"] = metrics.cacheMs;
  doc["space_ms"] = metrics.freeSpaceMs;
  doc["paint_wait_ms"] = metrics.paintWaitMs;
  doc["sd_read_us"] = metrics.readUs;
  doc["sha256_us"] = metrics.hashUs;
  doc["local_bytes"] = metrics.localBytes;
  doc["verified_download_bytes"] = metrics.verifyBytes;
  doc["read_calls"] = metrics.readCalls;
  doc["cache_hit"] = metrics.cacheHit;
  doc["added"] = added;
  doc["existing"] = skipped;
  doc["conflicts"] = conflicts;
  FsFile report;
  if (Storage.ensureDirectoryExists(TEMP_DIR) && Storage.openFileForWrite("SYNC", REPORT, report)) {
    serializeJson(doc, report);
    report.close();
  }
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
      if (++work->indexed > BookSync::MAX_BOOKS) return false;
      if (work->sizes.mayContain(static_cast<uint32_t>(file.size64())) &&
          (!hashFile(file, work->actual) || index.write(work->actual, 32) != 32))
        return false;
    }
  }
  // SdFat also ends iteration on an I/O error. Never trust or retain a partial
  // index, which could miss a USB copy and download the same content again.
  return directory.getError() == 0;
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
  FsFile manifest;
  {
    PhaseTimer phase(metrics.manifestMs);
    if (HttpDownloader::downloadToFile(
            session, work->url + "/v1/manifest", MANIFEST,
            [this](unsigned n, unsigned total) {
              return n <= BookSync::MAX_MANIFEST_BYTES && total <= BookSync::MAX_MANIFEST_BYTES && pump();
            },
            "reader", work->token, HttpDownloader::TlsPolicy::Strict) != HttpDownloader::OK)
      return false;
    if (!Storage.openFileForRead("SYNC", MANIFEST, manifest) || manifest.fileSize() > BookSync::MAX_MANIFEST_BYTES)
      return false;
    // Validate the COMPLETE manifest before downloading anything. No partial inventory can pass.
    while (work->lines.available(manifest)) {
      if (!work->lines.readLine(manifest, work->line) || !pump()) return false;
      const auto kind = work->parser.accept(work->line, work->entry);
      if (kind == BookSync::Manifest::Line::Invalid) return false;
      if (kind == BookSync::Manifest::Line::Book) work->sizes.add(work->entry.size);
    }
  }
  if (!work->parser.ended || !manifest.seek(0)) return false;
  work->lines.reset();
  if (work->parser.count == 0) return true;
  show(StrId::STR_BOOK_SYNC_CHECKING);
  FsFile index;
  if (!Storage.beginContentHashScan(work->indexGeneration)) return false;
  const auto cacheStarted = millis();
  metrics.cacheHit = loadIndex(index);
  metrics.cacheMs += millis() - cacheStarted;
  if (!metrics.cacheHit) {
    if (cancelled || (index && !index.close()) || !Storage.openFileForWrite("SYNC", INDEX, index)) return false;
    const auto scanStarted = millis();
    const bool scanned = indexLocal("/", index);
    metrics.scanMs = millis() - scanStarted;
    if (!scanned) return false;
  }
  if (Storage.contentGeneration() != work->indexGeneration || (index && !index.close()) ||
      !Storage.openFileForUpdate("SYNC", INDEX, index))
    return false;
  show(StrId::STR_BOOK_SYNC_DOWNLOADING);
  work->parser = {};
  while (work->lines.available(manifest)) {
    if (!work->lines.readLine(manifest, work->line) || !pump()) return false;
    const auto kind = work->parser.accept(work->line, work->entry);
    if (kind == BookSync::Manifest::Line::Invalid) return false;
    if (kind != BookSync::Manifest::Line::Book) continue;
    decodeHash(work->entry.hash, work->expected);
    bool found;
    if (!containsHash(index, work->expected, found) || Storage.contentGeneration() != work->indexGeneration)
      return false;
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
    const auto spaceStarted = millis();
    const uint64_t freeBytes = Storage.sdFreeBytes();
    metrics.freeSpaceMs += millis() - spaceStarted;
    if (freeBytes < uint64_t(work->entry.size) + 1024 * 1024) return false;
    const std::string url =
        work->url + "/v1/files/" + std::string(work->entry.id) + "/" + std::string(work->entry.hash);
    const uint32_t size = work->entry.size;
    const auto downloadStarted = millis();
    const auto downloaded = HttpDownloader::downloadToFile(
        session, url, STAGE, [this, size](unsigned n, unsigned total) { return n <= size && total <= size && pump(); },
        "reader", work->token, HttpDownloader::TlsPolicy::Strict);
    metrics.downloadMs += millis() - downloadStarted;
    if (downloaded != HttpDownloader::OK) return false;
    {
      PhaseTimer phase(metrics.verifyMs);
      FsFile book;
      if (!Storage.openFileForRead("SYNC", STAGE, book) || book.fileSize() != size || book.read(work->bytes, 4) != 4 ||
          memcmp(work->bytes, "PK\x03\x04", 4) || !book.seek(0) || !hashFile(book, work->actual, HashKind::Verify) ||
          memcmp(work->actual, work->expected, 32) || !book.close())
        return false;
    }
    // Recheck just before publication. Never use the downloader's replacing rename on a book.
    if (Storage.contentGeneration() != work->indexGeneration || Storage.exists(destination.c_str()) ||
        !Storage.rename(STAGE, destination.c_str()))
      return false;
    // Exactly one public destination mutation is ours. Any concurrent change
    // remains visible to the generation check on the next inventory row.
    ++work->indexGeneration;
    {
      RenderLock lock(*this);
      ++added;
    }
    // Include new books in deduplication for subsequent same-content entries in this inventory.
    if (!index.seek(index.fileSize()) || index.write(work->expected, 32) != 32) return false;
    index.flush();
  }
  if (!work->parser.ended || Storage.contentGeneration() != work->indexGeneration) return false;
  const auto cacheStartedAtEnd = millis();
  saveIndex(index);
  metrics.cacheMs += millis() - cacheStartedAtEnd;
  return !cancelled;
}
