#include "HttpDownloader.h"

#include <Arduino.h>
#include <CrossPointRoots.h>
#include <HalClock.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <base64.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "CrossPointSettings.h"

// All HTTPS runs over the wolfSSL-backed SecureNet stack (verified against the
// curated CrossPointRoots); plain http uses SecureNet's WiFiClient passthrough.
// The former mbedtls/esp_http_client path (with its per-host cert pins and
// crt_bundle workarounds) has been removed — TLS 1.3 support and lower heap use
// were the reasons for the switch. See lib/SecureNet.

namespace {

std::string extractHostFromUrl(const std::string& url) {
  size_t schemeEnd = url.find("://");
  size_t hostStart = schemeEnd == std::string::npos ? 0 : schemeEnd + 3;
  size_t hostEnd = url.find('/', hostStart);
  if (hostEnd == std::string::npos) hostEnd = url.size();
  return url.substr(hostStart, hostEnd - hostStart);
}

// Clock guard for TLS. The plausibility window and the SNTP retry policy now live in
// HalClock (isPlausibleForTls / ensureUsableForTls) so every TLS entry point shares one rule —
// this used to be a private copy here, which is why the KOReader paths never got it. Returns
// whether full certificate date validation is possible; false means the caller should tolerate
// date errors (and only date errors) for this request.
bool ensureClockForTls() { return HalClock::ensureUsableForTls(SETTINGS.ntpServer); }

// Per-request timeout handed to SecureHttpClient::setTimeout(). 60s gives slow
// servers room to send their first headers; SecureHttpClient reuses it as the
// idle deadline for each body read. The response body streams in
// SecureHttpClient's own READ_CHUNK-sized pieces.
constexpr int HTTP_TIMEOUT_MS = 60000;

struct Sink {
  // Returns false to abort the transfer (e.g. SD write failure or user cancel).
  std::function<bool(const uint8_t*, size_t)> write;
  // Drops everything written so far (the file is truncated). Set only for file downloads, which are
  // the only transfers fetched in Range chunks: a chunk lands where the previous one ended, and a
  // server that ignores Range restarts the body from byte 0.
  std::function<bool()> rewind;
  HttpDownloader::ProgressCallback progress;
  size_t total = 0;
  size_t downloaded = 0;
};

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// Runs once per http call (or once per session for reused sessions): logs
// heap stats and ensures the wall clock is set so TLS cert-date validation
// can succeed. Shared by both TLS backends. Returns false when https was requested and the
// clock could not be established — the caller then permits date errors alone.
bool logPreCallContext(const std::string& url) {
  LOG_DBG("HTTP", "Heap free: %u, largest block: %u", esp_get_free_heap_size(),
          heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
  if (url.compare(0, 8, "https://") == 0) {
    return ensureClockForTls();
  }
  return true;
}

// Does this transfer verify the peer? Resolves the caller's policy against the
// user's "skip HTTPS validation" setting — the escape hatch for self-hosted
// servers with a private CA or a self-signed certificate, which the device
// otherwise cannot reach at all. Strict ignores it by design.
bool verificationRequested(HttpDownloader::TlsPolicy tls) {
  switch (tls) {
    case HttpDownloader::TlsPolicy::Strict:
      return true;
    case HttpDownloader::TlsPolicy::Verified:
      return !SETTINGS.skipHttpsValidation;
    case HttpDownloader::TlsPolicy::Unverified:
      return false;
  }
  return true;
}

// Modem sleep parks the radio between DTIM beacons, which costs packets on a transfer long
// enough to span them -- so a small OPDS feed usually gets away with it and a large category
// or a multi-MB book consistently does not, surfacing as a stall or a short read rather than
// a clean error. OtaUpdater has disabled power-save around firmware downloads for exactly
// this reason since it was written; OPDS feeds and book fetches, which run just as long,
// never did.
// Ported from crosspoint-reader PR #3252 (Foulad / @sfoulad).
//
// Scoped to the request, not the session: a Session's keep-alive can sit idle between files
// while the user picks the next one, and holding the radio awake through that would spend
// battery for nothing.
struct WifiPowerSaveGuard {
  WifiPowerSaveGuard() {
    const esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) LOG_ERR("HTTP", "Failed to disable WiFi power-save: %d", err);
  }
  ~WifiPowerSaveGuard() {
    const esp_err_t err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (err != ESP_OK) LOG_ERR("HTTP", "Failed to restore WiFi power-save: %d", err);
  }
};

// The response as one streamed body: every byte the server sends goes to the sink.
int streamGet(crosspoint::SecureHttpClient& http, const std::string& url, Sink& sink) {
  auto body = [&](const uint8_t* data, size_t len) -> bool {
    if (!sink.write(data, len)) return false;  // abort
    sink.downloaded += len;
    if (sink.progress && sink.total > 0) {
      if (!sink.progress(sink.downloaded, sink.total)) return false;
    }
    return true;
  };
  auto progress = [&](size_t /*downloaded*/, size_t total) -> bool {
    sink.total = total;
    return true;
  };
  return http.get(url, body, progress);
}

bool isTransportFailure(int rc) {
  using C = crosspoint::SecureHttpClient;
  return rc == C::ERR_CONNECT || rc == C::ERR_SEND || rc == C::ERR_TIMEOUT || rc == C::ERR_TRUNCATED;
}

size_t largestFreeBlock() { return heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT); }

// Fetches url in Range requests, so that no response, and so no TLS record, is larger than a chunk
// plus its headers (lib/SecureNet/HttpRange.h has why and the sizing). Each request is sized from
// the largest free block just before it, starting from firstLargest, which the caller measured, and
// never above chunkCeiling. An out-of-memory read lowers chunkCeiling for good: the caller keeps it
// for the rest of its session. Returns 200 once the whole file has been written, else the failing
// HTTP status or SecureHttpError.
//
// The handling of a server that ignores Range (200 instead of 206: rewind the sink, take the whole
// body) and the resume from the bytes already received are adapted from Free-Ink/freeink-sdk f80a99c
// (ResumableFetch.h, Justin Mitchell). Different here: every request is a bounded range on the same
// kept-alive connection, the total comes from Content-Range, and chunks after the first go straight
// to the URL the first one was redirected to.
int chunkedGet(crosspoint::SecureHttpClient& http, const std::string& url, Sink& sink, size_t firstLargest,
               size_t& chunkCeiling) {
  namespace hr = crosspoint::http_range;
  using C = crosspoint::SecureHttpClient;
  struct RequestModeGuard {
    C& client;
    ~RequestModeGuard() {
      client.clearRange();
      client.setQuietRequests(false);
    }
  } guard{http};
  http.setQuietRequests(true);

  const unsigned long startMs = millis();
  std::string target = url;
  size_t offset = 0;
  bool totalKnown = false;
  size_t total = 0;
  unsigned requests = 0;
  bool wholeBody = false;
  hr::ChunkSizer sizer(chunkCeiling);
  hr::RetryBudget retries;
  size_t chunkSize = sizer.next(firstLargest);
  size_t smallest = chunkSize;
  size_t largestUsed = chunkSize;
  bool outOfMemory = false;

  hr::Chunk chunk;
  while (true) {
    if (requests > 0) {
      // The heap fragments as the download runs: size this request from the block there is now.
      const size_t largest = largestFreeBlock();
      const size_t previous = chunkSize;
      chunkSize = sizer.next(largest);
      if (chunkSize != previous) {
        LOG_DBG("HTTP", "chunk size %u -> %u B at %u B (largest free block %u B%s)", static_cast<unsigned>(previous),
                static_cast<unsigned>(chunkSize), static_cast<unsigned>(offset), static_cast<unsigned>(largest),
                outOfMemory ? ", after an out-of-memory read" : "");
      }
      smallest = std::min(smallest, chunkSize);
      largestUsed = std::max(largestUsed, chunkSize);
      outOfMemory = false;
    }
    if (!hr::nextChunk(offset, chunkSize, totalKnown, total, chunk)) break;
    http.setRange(chunk.first, chunk.last);
    hr::RangeReply reply = hr::RangeReply::Error;
    bool classified = false;
    size_t received = 0;
    auto body = [&](const uint8_t* data, size_t len) -> bool {
      if (!classified) {
        classified = true;
        reply = hr::classifyReply(http.lastStatus(), http.lastHasContentRange(), http.lastContentRange(), chunk.first);
        if (reply == hr::RangeReply::WholeBody) {
          // The server ignored Range: this body is the whole file from byte 0.
          if (offset > 0 && (!sink.rewind || !sink.rewind())) return false;
          offset = 0;
          totalKnown = http.lastContentLength() >= 0;
          total = totalKnown ? static_cast<size_t>(http.lastContentLength()) : 0;
        } else if (reply == hr::RangeReply::Partial && http.lastContentRange().totalKnown) {
          totalKnown = true;
          total = http.lastContentRange().total;
        }
      }
      // An error page (or a range we did not ask for) is drained, never written into the file.
      if (reply != hr::RangeReply::Partial && reply != hr::RangeReply::WholeBody) return true;
      if (!sink.write(data, len)) return false;
      offset += len;
      received += len;
      sink.downloaded = offset;
      sink.total = totalKnown ? total : 0;
      if (sink.progress && sink.total > 0 && !sink.progress(sink.downloaded, sink.total)) return false;
      return true;
    };

    const int rc = http.get(target, body, nullptr);
    ++requests;
    if (rc == C::ERR_ABORTED) return rc;
    if (rc < 0) {
      // The connection broke mid-chunk. What was written stays; reconnect (a resumed TLS session,
      // so no certificate chain) and continue from there. A record that did not fit the heap makes
      // the next requests smaller, whatever the largest block then reads.
      outOfMemory = http.lastReadOutOfMemory();
      if (outOfMemory) {
        sizer.onOutOfMemory();
        if (sizer.ceiling() != chunkCeiling) {
          LOG_DBG("HTTP", "chunk ceiling %u -> %u B for the rest of the session (out-of-memory read at %u B)",
                  static_cast<unsigned>(chunkCeiling), static_cast<unsigned>(sizer.ceiling()),
                  static_cast<unsigned>(offset));
          chunkCeiling = sizer.ceiling();
        }
      }
      if (!isTransportFailure(rc) || !retries.reconnectAfterFailure(received > 0, totalKnown ? total : offset)) {
        LOG_ERR("HTTP", "chunked download stopped at %u B after %u requests, %u reconnect(s): rc=%d%s",
                static_cast<unsigned>(offset), requests, retries.reconnects(), rc,
                outOfMemory ? " (out of memory)" : "");
        return rc;
      }
      http.close();
      http.open(target);  // measured at the top of the loop with the new connection in place
      continue;
    }
    if (!classified) {  // a response without a body: a 416, or an empty one
      reply = hr::classifyReply(rc, http.lastHasContentRange(), http.lastContentRange(), chunk.first);
    }
    if (reply == hr::RangeReply::WholeBody) {
      wholeBody = true;
      break;
    }
    if (reply == hr::RangeReply::Unsatisfiable && offset > 0 && (!totalKnown || offset == total)) {
      break;  // the previous chunk ended exactly at the end of a file of unknown size
    }
    if (reply != hr::RangeReply::Partial) {
      LOG_ERR("HTTP", "chunked download: unexpected reply %d to bytes %u-%u", rc, static_cast<unsigned>(chunk.first),
              static_cast<unsigned>(chunk.last));
      return rc;
    }
    target = http.lastUrl();  // follow a redirect once, not once per chunk
    if (received > 0) retries.onProgress();
    if (hr::transferComplete(offset, totalKnown, total, chunk.length(), received)) break;
  }

  LOG_DBG("HTTP", "chunked download done: %u B in %u requests, %lu ms, %u reconnect(s), chunks %u-%u B%s",
          static_cast<unsigned>(offset), requests, millis() - startMs, retries.reconnects(),
          static_cast<unsigned>(smallest), static_cast<unsigned>(largestUsed),
          wholeBody ? " (server ignored Range: whole file streamed)" : "");
  return 200;
}

// File downloads over https, on a heap too tight for 16 KB TLS records, are fetched in Range
// chunks; everything else streams as one response. Decided on the largest free block once the
// connection is up, just before the first request. chunkCeiling is the session's (see chunkedGet).
// Returns what SecureHttpClient::get() would: the HTTP status (200 for a complete chunked download)
// or a negative SecureHttpError.
int transfer(crosspoint::SecureHttpClient& http, const std::string& url, Sink& sink, size_t& chunkCeiling) {
  namespace hr = crosspoint::http_range;
  if (!sink.rewind) return streamGet(http, url, sink);
  if (url.compare(0, 8, "https://") != 0) {
    LOG_DBG("HTTP", "download mode: streamed (plain http)");
    return streamGet(http, url, sink);
  }
  if (!http.open(url)) return crosspoint::SecureHttpClient::ERR_CONNECT;
  const size_t largest = largestFreeBlock();
  if (!hr::shouldChunk(largest)) {
    LOG_DBG("HTTP", "download mode: streamed (largest free block %u B)", static_cast<unsigned>(largest));
    return streamGet(http, url, sink);
  }
  LOG_DBG("HTTP", "download mode: chunked %u B (largest free block %u B, ceiling %u B)",
          static_cast<unsigned>(hr::ChunkSizer(chunkCeiling).next(largest)), static_cast<unsigned>(largest),
          static_cast<unsigned>(chunkCeiling));
  return chunkedGet(http, url, sink, largest, chunkCeiling);
}

// One-shot streaming GET over SecureNet (wolfSSL). Fills the Sink and emits
// "Phase start"/"Phase open_ok"/"Phase done" heap telemetry. The TlsPolicy
// decides whether the curated roots are loaded at all and what happens when the
// peer fails to verify.
HttpDownloader::DownloadError runGetSecure(const std::string& url, const std::string& username,
                                           const std::string& password, Sink& sink, HttpDownloader::TlsPolicy tls) {
  const WifiPowerSaveGuard psGuard;
  const bool clockReady = logPreCallContext(url);
  const unsigned long startMs = millis();
  LOG_DBG("HTTP", "Phase start @%lums heap=%u largest=%u", millis() - startMs, esp_get_free_heap_size(),
          heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

  crosspoint::SecureHttpClient http;
  // No roots => SecureClient sets WOLFSSL_VERIFY_NONE and skips the hostname
  // check: one handshake, no chain work.
  http.setCACert(verificationRequested(tls) ? CROSSPOINT_ROOTS_PEM : nullptr);
  // Never retry unverified behind the caller's back; see TlsPolicy.
  http.setAllowInsecureFallback(false);
  http.setAllowCertificateDateErrors(!clockReady);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setUserAgent("WitchReader-ESP32-" CROSSPOINT_VERSION);
  if (!username.empty() && !password.empty()) {
    http.setBasicAuth(username, password);
  }

  bool openLogged = false;
  const auto write = sink.write;
  sink.write = [&openLogged, startMs, write](const uint8_t* data, size_t len) -> bool {
    if (!openLogged) {
      openLogged = true;
      LOG_DBG("HTTP", "Phase open_ok @%lums heap=%u largest=%u", millis() - startMs, esp_get_free_heap_size(),
              heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    }
    return write(data, len);
  };

  // A one-shot download is its own session: whatever ceiling it learns ends with it.
  size_t chunkCeiling = crosspoint::http_range::DEFAULT_CHUNK_BYTES;
  const int rc = transfer(http, url, sink, chunkCeiling);
  // Log the handshake heap trough on EVERY path (incl. early abort via
  // treatAbortAsSuccess) so the TLS-specific low-water is always captured,
  // distinct from the all-time ESP.getMinFreeHeap() figure.
  LOG_DBG("HTTP", "Phase done @%lums rc=%d downloaded=%zu (insecure=%d) handshakeMinFree=%u handshakeMinLargest=%u",
          millis() - startMs, rc, sink.downloaded, static_cast<int>(http.lastConnectionWasInsecure()),
          static_cast<unsigned>(http.lastHandshakeMinFree()), static_cast<unsigned>(http.lastHandshakeMinLargest()));
  if (rc == crosspoint::SecureHttpClient::ERR_ABORTED) {
    return HttpDownloader::ABORTED;
  }
  if (rc < 0) {
    LOG_ERR("HTTP", "SecureNet GET failed: rc=%d url=%s", rc, url.c_str());
    return HttpDownloader::HTTP_ERROR;
  }
  if (rc != 200) {
    LOG_ERR("HTTP", "SecureNet unexpected status: %d", rc);
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}

// Single funnel for every fetchUrl/downloadToFile overload. The policy defaults
// to the historic browsing posture; callers that carry credentials, install
// files, or fetch public data pass their own.
HttpDownloader::DownloadError runGetDispatch(const std::string& url, const std::string& username,
                                             const std::string& password, Sink& sink, HttpDownloader::TlsPolicy tls) {
  return runGetSecure(url, username, password, sink, tls);
}
}  // namespace

// ---- Session implementation ----

struct HttpDownloader::Session::Impl {
  // SecureNet keeps its own keep-alive connection alive internally (reuses the
  // open SecureClient when host/port match), so the Session just owns one
  // persistent SecureHttpClient across downloadToFile(session, ...) calls.
  std::unique_ptr<crosspoint::SecureHttpClient> http;
  bool preCallLogged = false;
  // The largest Range chunk this session's files may use. A size that ran out of memory reading a
  // record stays out of reach for the rest of the session, so later files start at a size that
  // worked (see chunkedGet).
  size_t chunkCeiling = crosspoint::http_range::DEFAULT_CHUNK_BYTES;
};

HttpDownloader::Session::Session() : impl_(std::make_unique<Impl>()) {}
HttpDownloader::Session::~Session() = default;

namespace {

// SecureNet session GET: reuse one persistent SecureHttpClient. Its internal
// keep-alive reuses the open TLS connection when the host/port match, so
// back-to-back files on the same host share a single handshake (the Session
// heap win). Cross-host requests transparently reopen inside SecureHttpClient.
HttpDownloader::DownloadError runGetSecureOnSession(HttpDownloader::Session& session, const std::string& url,
                                                    const std::string& username, const std::string& password,
                                                    Sink& sink, HttpDownloader::TlsPolicy tls) {
  const WifiPowerSaveGuard psGuard;
  auto* impl = session.impl();
  // Evaluated on every call, not just when the session is created: a session opened before the
  // clock was set must not keep that verdict for the rest of its life (nor the reverse).
  const bool clockReady = logPreCallContext(url);
  if (!impl->http) {
    impl->http = std::make_unique<crosspoint::SecureHttpClient>();
    impl->http->setTimeout(HTTP_TIMEOUT_MS);
    impl->http->setUserAgent("WitchReader-ESP32-" CROSSPOINT_VERSION);
  }
  // Re-applied per call, like the clock verdict below: a session outlives a
  // single request and must not carry a previous caller's policy.
  impl->http->setCACert(verificationRequested(tls) ? CROSSPOINT_ROOTS_PEM : nullptr);
  impl->http->setAllowInsecureFallback(false);
  impl->http->setAllowCertificateDateErrors(!clockReady);
  impl->http->clearHeaders();
  if (!username.empty() && !password.empty()) {
    impl->http->setBasicAuth(username, password);
  }

  const int rc = transfer(*impl->http, url, sink, impl->chunkCeiling);
  if (rc == crosspoint::SecureHttpClient::ERR_ABORTED) return HttpDownloader::ABORTED;
  if (rc != 200) {
    LOG_ERR("HTTP", "SecureNet session GET failed: rc=%d url=%s", rc, url.c_str());
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}

HttpDownloader::DownloadError runGetOnSession(HttpDownloader::Session& session, const std::string& url,
                                              const std::string& username, const std::string& password, Sink& sink,
                                              HttpDownloader::TlsPolicy tls) {
  return runGetSecureOnSession(session, url, username, password, sink, tls);
}
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password, TlsPolicy tls) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; };
  return runGetDispatch(url, username, password, sink, tls) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password) {
  // Preserve historic semantics: callback abort => failure.
  return fetchUrl(url, onData, false, username, password);
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, bool treatAbortAsSuccess,
                              const std::string& username, const std::string& password) {
  LOG_DBG("HTTP", "Fetching (stream): %s", url.c_str());
  if (!onData) {
    return false;
  }
  Sink sink;
  sink.write = [&onData](const uint8_t* data, size_t len) { return onData(data, len); };
  const DownloadError result = runGetDispatch(url, username, password, sink, TlsPolicy::Verified);
  if (result == OK) {
    return true;
  }
  // Optional mode for parsers that intentionally stop early once they have
  // extracted all required fields from the stream.
  return treatAbortAsSuccess && result == ABORTED;
}

bool HttpDownloader::fetchUrlVerified(const std::string& url, const DataCallback& onData, bool treatAbortAsSuccess,
                                      const std::string& username, const std::string& password) {
  LOG_DBG("HTTP", "Fetching (stream, verify-only): %s", url.c_str());
  if (!onData) {
    return false;
  }
  Sink sink;
  sink.write = [&onData](const uint8_t* data, size_t len) { return onData(data, len); };
  // Strict: OTA never honours the skip-validation setting.
  const DownloadError result = runGetDispatch(url, username, password, sink, TlsPolicy::Strict);
  if (result == OK) {
    return true;
  }
  return treatAbortAsSuccess && result == ABORTED;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password, TlsPolicy tls) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  outContent.clear();  // start clean; the sink appends, so don't carry prior content
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) {
    outContent.append(reinterpret_cast<const char*>(data), len);
    return true;
  };
  return runGetDispatch(url, username, password, sink, tls) == OK;
}

namespace {
// Common file-sink plumbing used by both downloadToFile overloads.
HttpDownloader::DownloadError finishFileDownload(HttpDownloader::DownloadError result, const std::string& destPath,
                                                 FsFile& file, size_t downloaded) {
  // Flush before any remove() on the same path; DESTRUCTOR_CLOSES_FILE would
  // otherwise close only after the remove.
  file.flush();
  file.close();
  if (result != HttpDownloader::OK) {
    Storage.remove(destPath.c_str());
    return result;
  }
  if (downloaded == 0) {
    LOG_ERR("HTTP", "no data received");
    Storage.remove(destPath.c_str());
    return HttpDownloader::HTTP_ERROR;
  }
  LOG_DBG("HTTP", "Downloaded %zu bytes", downloaded);
  return HttpDownloader::OK;
}
}  // namespace

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, const std::string& username,
                                                             const std::string& password, TlsPolicy tls) {
  LOG_DBG("HTTP", "Downloading: %s", url.c_str());
  LOG_DBG("HTTP", "Destination: %s", destPath.c_str());

  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }
  FsFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing: %s", destPath.c_str());
    return FILE_ERROR;
  }

  Sink sink;
  sink.progress = std::move(progress);
  sink.write = [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; };
  // Reopening for write truncates: the file starts again from byte 0.
  sink.rewind = [&file, &destPath]() {
    file.close();
    return Storage.openFileForWrite("HTTP", destPath.c_str(), file);
  };

  const DownloadError result = runGetDispatch(url, username, password, sink, tls);
  return finishFileDownload(result, destPath, file, sink.downloaded);
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(Session& session, const std::string& url,
                                                             const std::string& destPath, ProgressCallback progress,
                                                             const std::string& username, const std::string& password,
                                                             TlsPolicy tls) {
  LOG_DBG("HTTP", "Downloading (session): %s", url.c_str());
  LOG_DBG("HTTP", "Destination: %s", destPath.c_str());

  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }
  FsFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing: %s", destPath.c_str());
    return FILE_ERROR;
  }

  Sink sink;
  sink.progress = std::move(progress);
  sink.write = [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; };
  // Reopening for write truncates: the file starts again from byte 0.
  sink.rewind = [&file, &destPath]() {
    file.close();
    return Storage.openFileForWrite("HTTP", destPath.c_str(), file);
  };

  const DownloadError result = runGetOnSession(session, url, username, password, sink, tls);
  return finishFileDownload(result, destPath, file, sink.downloaded);
}
