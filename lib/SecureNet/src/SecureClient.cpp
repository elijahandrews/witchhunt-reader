#include "SecureClient.h"

#include <Arduino.h>  // micros(), getCpuFrequencyMhz() for the handshake cost split
#include <Logging.h>
#include <esp_heap_caps.h>

// wolfSSL is only pulled in when explicitly enabled, keeping flag-off builds
// (and host tests) free of the dependency. See SecureClient.h.
#if defined(FREEINK_NET_WOLFSSL)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>    // the session cache's mutex
#include <wolfssl/error-ssl.h>  // VERIFY_CERT_ERROR, DOMAIN_NAME_MISMATCH (SSL-layer codes)
#include <wolfssl/ssl.h>

#include <cstring>
#include <ctime>  // time() for the clock reported alongside a cert-date load failure

// Session resumption needs the ticket support scripts/patch_wolfssl.py switches on. Without it the
// ticket API below is not even declared, so say where the define should have come from.
#if !defined(HAVE_SESSION_TICKET)
#error "HAVE_SESSION_TICKET did not reach wolfSSL: scripts/patch_wolfssl.py must patch user_settings.h"
#endif

// The Arduino-wolfSSL library's logging.c references this hook, normally defined
// in the library's wolfssl.h sketch glue (not compiled in a PlatformIO lib build),
// so we provide it. Signature must match wolfcrypt/logging.h exactly (int return).
// Routes wolfSSL's internal debug logging to our logger.
extern "C" int wolfSSL_Arduino_Serial_Print(const char* const s) {
  if (s) LOG_DBG("WOLF", "%s", s);
  return 0;
}
#endif

namespace crosspoint {

bool SecureClient::tls13Available() {
#if defined(FREEINK_NET_WOLFSSL)
  return true;
#else
  return false;
#endif
}

SecureClient::~SecureClient() { stop(); }

#if defined(FREEINK_NET_WOLFSSL)

namespace {
// Handshake I/O accounting. wcRecv/wcSend are the ONLY path between wolfSSL and the socket, so
// timing them splits a handshake into "in the transport" and "computing" -- and those have
// completely different fixes. A single elapsed figure cannot tell them apart, and the poll count
// alone is misleading: Arduino's NetworkClient::write() drives select() with a
// WIFI_CLIENT_SELECT_TIMEOUT_US budget per retry, so a stalled send blocks INSIDE one
// wolfSSL_connect() call and looks exactly like slow crypto from the outside.
// The connect path is single-threaded, so plain statics are enough; connectWithMethod() resets
// them per attempt.
struct HandshakeIoStats {
  uint32_t ioUs;
  uint32_t recvCalls;
  uint32_t sendCalls;
  uint32_t recvBytes;
  uint32_t sendBytes;
  uint32_t slowestIoUs;
};
HandshakeIoStats g_handshakeIo = {};

// Bridge wolfSSL's I/O to the underlying WiFiClient transport. Non-blocking:
// return WANT_READ/WANT_WRITE when the socket has nothing yet so wolfSSL retries.
int wcSend(WOLFSSL* /*ssl*/, char* buf, int sz, void* ctx) {
  auto* t = static_cast<WiFiClient*>(ctx);
  const uint32_t ioStartUs = micros();
  const int n = t->write(reinterpret_cast<const uint8_t*>(buf), sz);
  const uint32_t elapsedUs = micros() - ioStartUs;
  g_handshakeIo.ioUs += elapsedUs;
  g_handshakeIo.sendCalls++;
  if (n > 0) g_handshakeIo.sendBytes += static_cast<uint32_t>(n);
  if (elapsedUs > g_handshakeIo.slowestIoUs) g_handshakeIo.slowestIoUs = elapsedUs;
  if (n <= 0) {
    // A dead transport must surface as CONN_CLOSE: mapping it to WANT_WRITE
    // makes the handshake spin until the deadline instead of failing fast.
    // (Ported from Free-Ink/freeink-sdk 43132fc.)
    if (!t->connected()) return WOLFSSL_CBIO_ERR_CONN_CLOSE;
    return WOLFSSL_CBIO_ERR_WANT_WRITE;
  }
  return n;
}
int wcRecv(WOLFSSL* /*ssl*/, char* buf, int sz, void* ctx) {
  auto* t = static_cast<WiFiClient*>(ctx);
  const uint32_t ioStartUs = micros();
  int result;
  if (!t->connected() && t->available() == 0) {
    result = WOLFSSL_CBIO_ERR_CONN_CLOSE;
  } else if (t->available() == 0) {
    result = WOLFSSL_CBIO_ERR_WANT_READ;
  } else {
    const int n = t->read(reinterpret_cast<uint8_t*>(buf), sz);
    result = (n <= 0) ? WOLFSSL_CBIO_ERR_WANT_READ : n;
  }
  const uint32_t elapsedUs = micros() - ioStartUs;
  g_handshakeIo.ioUs += elapsedUs;
  g_handshakeIo.recvCalls++;
  if (result > 0) g_handshakeIo.recvBytes += static_cast<uint32_t>(result);
  if (elapsedUs > g_handshakeIo.slowestIoUs) g_handshakeIo.slowestIoUs = elapsedUs;
  return result;
}

// True if the wolfSSL error code is a peer-certificate-verification failure
// (as opposed to a transport/protocol failure). Only these justify the
// verified->insecure fallback: retrying insecurely on a DNS/TCP/version error
// gains nothing and would hide the real problem.
bool isVerificationError(int err) {
  switch (err) {
    case ASN_NO_SIGNER_E:    // no trusted root for the chain
    case ASN_SIG_CONFIRM_E:  // signature check failed
    case ASN_BEFORE_DATE_E:  // notBefore in the future (clock)
    case ASN_AFTER_DATE_E:   // expired
    case ASN_SELF_SIGNED_E:
    case CRL_CERT_DATE_ERR:
    case VERIFY_CERT_ERROR:  // generic cert verification failure
    case DOMAIN_NAME_MISMATCH:
      return true;
    default:
      return false;
  }
}

// Per-certificate verification timing. wolfSSL issues the verify callback once per chain
// certificate -- intermediates first, leaf last -- so the gap between consecutive invocations is
// that certificate's verification cost. This is what turns a single "the server flight took
// 3061 ms" into a per-hop number, which is what decides whether shortening the chain is worth a
// trust-store change. Only populated when CROSSPOINT_TLS_VERIFY_TIMING is built in; without it
// wolfSSL calls the callback on errors only and this stays empty, which is harmless.
struct ChainVerifyStats {
  uint32_t callStartMs;  // start of the wolfSSL_connect() call the chain is being processed in
  uint32_t lastCbMs;
  uint32_t count;
  uint32_t gapMs[8];
  int depth[8];
};
ChainVerifyStats g_chainVerify = {};

// Mirrors SecureClient::_allowCertificateDateErrors for the callback, which wolfSSL gives no
// user-data pointer. The connect path is single-threaded (same reasoning as g_handshakeIo), and
// connectWithMethod() sets this immediately before installing the callback.
bool g_allowCertificateDateErrors = false;

// wolfSSL's verify callback. Two jobs, and the timing one must never change the verdict.
//
// Verdict: when the caller could not obtain a trustworthy clock, accept precisely the two
// validity-window errors and pass everything else (chain, signature, trust anchor, hostname)
// straight through as the failure it is. Otherwise return `preverify` untouched, which is
// exactly what wolfSSL does when no callback is installed -- so installing this unconditionally
// (which the timing needs) leaves behaviour identical.
int verifyCallback(int preverify, WOLFSSL_X509_STORE_CTX* store) {
  const uint32_t nowMs = millis();
  if (g_chainVerify.count < sizeof(g_chainVerify.gapMs) / sizeof(g_chainVerify.gapMs[0])) {
    // The first gap is measured from the start of the enclosing wolfSSL_connect() call, so it
    // carries the ServerHello work (key exchange, transcript) ahead of the first certificate;
    // every later gap is one certificate's verification on its own.
    const uint32_t since = (g_chainVerify.count == 0) ? g_chainVerify.callStartMs : g_chainVerify.lastCbMs;
    g_chainVerify.gapMs[g_chainVerify.count] = nowMs - since;
    g_chainVerify.depth[g_chainVerify.count] = store != nullptr ? store->error_depth : -1;
  }
  g_chainVerify.count++;
  g_chainVerify.lastCbMs = nowMs;

  if (g_allowCertificateDateErrors && preverify == 0 && store != nullptr &&
      (store->error == ASN_BEFORE_DATE_E || store->error == ASN_AFTER_DATE_E)) {
    LOG_INF("TLS", "Ignoring certificate date error %d (no trusted clock); chain and hostname still verified",
            store->error);
    return 1;
  }
  // Name the link that failed: depth 0 is the leaf, higher depths walk up towards the root, so a
  // chain failure no longer surfaces as one connect-level error with no certificate attached.
  if (preverify == 0 && store != nullptr) {
    // Heap alongside: wolfSSL reports an allocation failure inside the signature math as a signature
    // failure (ASN_SIG_CONFIRM_E), so a -155 with a small largest block is memory, not the chain.
    LOG_ERR("TLS", "verify failed at depth %d: err=%d (callback #%lu) heap=%u largest=%u", store->error_depth,
            store->error, static_cast<unsigned long>(g_chainVerify.count), esp_get_free_heap_size(),
            heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
  }
  return preverify;
}

// ---- TLS session resumption ----------------------------------------------------------------
//
// Every connection used to repeat the full certificate check. GitHub's chain costs two RSA-4096
// signature checks, and on the X3 the connection made seconds after the first (the font list,
// then the first font file) failed that check for lack of heap (err=-155 at depth 1). A TLS 1.3
// resumption skips it: the server sends no certificate chain at all.
//
// The ticket arrives after the handshake, as a NewSessionTicket that wolfSSL processes inside
// wolfSSL_read() (tls13.c DoTls13NewSessionTicket), so the session is taken when the connection
// is stopped, not when it connects.
//
// Rules:
//  - Only a connection that VERIFIED its peer keeps a session, filed under host, port, trust store
//    and date policy, and only a connection that would verify the same way is offered it. A
//    resumed handshake carries no certificate: wolfSSL takes the peer as authenticated by the
//    ticket (tls13.c SetupPskKey, peerAuthGood), so a ticket from an unverified connection must
//    never stand in for a verification.
//  - Two slots; the least recently used session goes first.
//  - The cache owns its sessions outright and hands out copies. wolfSSL is built SINGLE_THREADED,
//    so its reference counts are plain integers, and the S3 KOSync worker connects from its own
//    task: a session object shared between the cache and a live connection could race on its
//    count. The copy costs nothing extra, because wolfSSL copies a shared session before resuming
//    from it anyway (internal.c HaveUniqueSessionObj).
//  - A mutex guards the slots, for the same worker.
constexpr size_t SESSION_CACHE_SLOTS = 2;

// wolfSSL refuses to build a ClientHello around a ticket longer than MAX_PSK_ID_LEN (1536 with
// TLS 1.3, wolfssl/internal.h; tls.c TLSX_PopulateExtensions returns BUFFER_ERROR), so such a
// ticket would fail every connect it was offered on. It is not kept.
constexpr int MAX_RESUMPTION_TICKET_LEN = 1536;

struct CachedSession {
  WOLFSSL_SESSION* session;  // owned; nullptr marks a free slot
  uint32_t lastUsed;         // least-recently-used stamp
  TlsSessionKey key;
};
CachedSession g_sessionCache[SESSION_CACHE_SLOTS] = {};
uint32_t g_sessionCacheTick = 0;

// Statically allocated, so the cache takes no heap of its own; a function-local static is
// initialised exactly once even when two tasks arrive together.
SemaphoreHandle_t sessionCacheMutex() {
  static StaticSemaphore_t storage;
  static const SemaphoreHandle_t mutex = xSemaphoreCreateMutexStatic(&storage);
  return mutex;
}

class SessionCacheLock {
 public:
  SessionCacheLock() { xSemaphoreTake(sessionCacheMutex(), portMAX_DELAY); }
  ~SessionCacheLock() { xSemaphoreGive(sessionCacheMutex()); }
  SessionCacheLock(const SessionCacheLock&) = delete;
  SessionCacheLock& operator=(const SessionCacheLock&) = delete;
};

// False when the host does not fit the key: such a host neither keeps nor is offered a session.
bool makeSessionKey(TlsSessionKey& key, const char* host, uint16_t port, const char* roots, bool datesWaived) {
  const size_t len = strlen(host);
  if (len >= sizeof(key.host)) return false;
  memcpy(key.host, host, len + 1);
  key.roots = roots;
  key.port = port;
  key.datesWaived = datesWaived;
  return true;
}

bool sameSessionKey(const TlsSessionKey& a, const TlsSessionKey& b) {
  return a.port == b.port && a.roots == b.roots && a.datesWaived == b.datesWaived && strcmp(a.host, b.host) == 0;
}

// Caller holds the lock.
CachedSession* findCachedSession(const TlsSessionKey& key) {
  for (CachedSession& slot : g_sessionCache) {
    if (slot.session != nullptr && sameSessionKey(slot.key, key)) return &slot;
  }
  return nullptr;
}

// A private copy of the session kept for `key`, or nullptr. The caller owns the copy.
WOLFSSL_SESSION* copyCachedSession(const TlsSessionKey& key) {
  const SessionCacheLock lock;
  CachedSession* slot = findCachedSession(key);
  if (slot == nullptr) return nullptr;
  slot->lastUsed = ++g_sessionCacheTick;
  return wolfSSL_SESSION_dup(slot->session);
}

// Takes ownership of `session` and files it under `key`: in place of that key's previous session,
// else in a free slot, else in place of the least recently used one. What it displaces is freed.
void storeCachedSession(const TlsSessionKey& key, WOLFSSL_SESSION* session) {
  const SessionCacheLock lock;
  CachedSession* slot = findCachedSession(key);
  if (slot == nullptr) {
    slot = &g_sessionCache[0];
    for (CachedSession& candidate : g_sessionCache) {
      if (candidate.session == nullptr) {
        slot = &candidate;
        break;
      }
      if (candidate.lastUsed < slot->lastUsed) slot = &candidate;
    }
  }
  if (slot->session != nullptr) wolfSSL_SESSION_free(slot->session);
  slot->session = session;
  slot->key = key;
  slot->lastUsed = ++g_sessionCacheTick;
}

void dropCachedSession(const TlsSessionKey& key) {
  const SessionCacheLock lock;
  CachedSession* slot = findCachedSession(key);
  if (slot == nullptr) return;
  wolfSSL_SESSION_free(slot->session);
  slot->session = nullptr;
}

// wolfSSL calls this from inside wolfSSL_read() when a NewSessionTicket arrives (internal.c
// SetTicket). It only notes the ticket's length; stop() takes the session itself.
int onSessionTicket(WOLFSSL* /*ssl*/, const unsigned char* /*ticket*/, int len, void* ctx) {
  *static_cast<int*>(ctx) = len;
  return 0;
}

// The session to keep from a connection that is closing, as a reference the caller owns, or
// nullptr. wolfSSL_get1_session() hands back the connection's own session object with one more
// reference, so the caller must not publish it until wolfSSL_free() has dropped the other one.
WOLFSSL_SESSION* sessionToKeep(WOLFSSL* ssl, const TlsSessionKey& key, int ticketLen) {
  if (ticketLen <= 0) {
    LOG_DBG("TLS", "no session ticket from %s on this connection; nothing new kept", key.host);
    return nullptr;
  }
  if (ticketLen > MAX_RESUMPTION_TICKET_LEN) {
    LOG_DBG("TLS", "session ticket from %s is %dB, too long to offer back; not kept", key.host, ticketLen);
    return nullptr;
  }
  WOLFSSL_SESSION* session = wolfSSL_get1_session(ssl);
  if (session != nullptr) LOG_DBG("TLS", "kept session for %s (ticket %dB)", key.host, ticketLen);
  return session;
}
}  // namespace

void SecureClient::clearSessionCache() {
  unsigned dropped = 0;
  {
    const SessionCacheLock lock;
    for (CachedSession& slot : g_sessionCache) {
      if (slot.session == nullptr) continue;
      wolfSSL_SESSION_free(slot.session);
      slot.session = nullptr;
      ++dropped;
    }
  }
  if (dropped > 0) LOG_DBG("TLS", "dropped %u cached TLS session(s)", dropped);
}

// One handshake attempt at a fixed verification level and TLS method.
int SecureClient::connectWithMethod(const char* host, uint16_t port, void* method, const char* label, bool verifyPeer,
                                    bool mayResume) {
  stop();
#ifdef CROSSPOINT_TLS_VERIFY_TIMING
  // Proves the instrumentation define actually reached the wolfSSL headers. It travels via
  // build_flags -> user_settings.h, and PlatformIO's build cache can hand back a library object
  // compiled without it, in which case the verify callback silently never fires and the chain
  // timing comes back empty with nothing to say why.
#if defined(WOLFSSL_ALWAYS_VERIFY_CB) && defined(WOLFSSL_VERIFY_CB_ALL_CERTS)
  static bool loggedVerifyCbBuild = false;
  if (!loggedVerifyCbBuild) {
    loggedVerifyCbBuild = true;
    LOG_DBG("TLS", "per-cert verify timing built in (ALWAYS_VERIFY_CB + VERIFY_CB_ALL_CERTS)");
  }
#else
#warning "CROSSPOINT_TLS_VERIFY_TIMING set but wolfSSL verify-callback defines did not reach the headers"
  static bool loggedVerifyCbMissing = false;
  if (!loggedVerifyCbMissing) {
    loggedVerifyCbMissing = true;
    LOG_ERR("TLS", "per-cert verify timing requested but wolfSSL was built without the callback defines");
  }
#endif
#endif
  // Split the connect into its three costs -- name resolution + TCP, then the handshake, and how
  // many non-blocking poll iterations the handshake took. A single "connect took N ms" cannot
  // tell a slow resolver from a slow server from our own 5 ms poll granularity, and on this
  // device the handshake is now the largest item in a sync.
  const uint32_t transportStartMs = millis();
  if (!_transport.connect(host, port)) return 0;
  const uint32_t transportMs = millis() - transportStartMs;

  auto* ctx = wolfSSL_CTX_new(static_cast<WOLFSSL_METHOD*>(method));
  if (!ctx) {
    _transport.stop();
    return 0;
  }
  _ctx = ctx;

  // Sessions are kept by the session cache above, not by wolfSSL's own global cache. Left on,
  // that cache takes a copy of every session, and with tickets enabled it mallocs a copy of any
  // ticket too long for its static buffer and holds it until the slot is reused (ssl_sess.c
  // AddSessionToCache): heap that nothing here would ever read.
  wolfSSL_CTX_set_session_cache_mode(ctx, WOLFSSL_SESS_CACHE_OFF);

  // Verifying the peer: decides the trust store below and whether a TLS session may be kept.
  const bool verifying = verifyPeer && !_insecure && _rootCA;
  if (verifying) {
    // The roots are date-checked AS THEY LOAD, not only during the handshake: plain
    // load_verify_buffer() passes WOLFSSL_LOAD_VERIFY_DEFAULT_FLAGS (== WOLFSSL_LOAD_FLAG_NONE),
    // so a cold-booted RTC-less board sitting at 1970 sees every curated root's notBefore as
    // "in the future" and loads none of them. WOLFSSL_LOAD_FLAG_DATE_ERR_OKAY switches the parse
    // to VERIFY_SKIP_DATE, which suppresses the notBefore/notAfter test and nothing else — the
    // signature and structural checks are unchanged (wolfcrypt/src/asn.c, `cert->badDate`).
    const word32 loadFlags =
        _allowCertificateDateErrors ? WOLFSSL_LOAD_FLAG_DATE_ERR_OKAY : WOLFSSL_LOAD_VERIFY_DEFAULT_FLAGS;
    const int loadRet = wolfSSL_CTX_load_verify_buffer_ex(ctx, reinterpret_cast<const unsigned char*>(_rootCA),
                                                          strlen(_rootCA), WOLFSSL_FILETYPE_PEM, 0, loadFlags);
    if (loadRet != WOLFSSL_SUCCESS) {
      // Report the wolfSSL code and the epoch together. This failure happens before the
      // handshake, so it leaves no verification error for connect() to classify and used to
      // surface as a bare "connect failed" — with -150/-151 and a 1970 timestamp side by side,
      // "the clock is wrong" is readable straight off the log.
      LOG_ERR("TLS", "load_verify_buffer failed for curated roots: err=%d epoch=%lld (dateErrorsAllowed=%d)", loadRet,
              static_cast<long long>(time(nullptr)), _allowCertificateDateErrors ? 1 : 0);
      if (loadRet == ASN_BEFORE_DATE_E || loadRet == ASN_AFTER_DATE_E) {
        LOG_ERR("TLS", "  -> the device clock is outside the roots' validity window; sync NTP before connecting");
      }
      stop();
      return 0;
    }
    g_allowCertificateDateErrors = _allowCertificateDateErrors;
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER, verifyCallback);
  } else {
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_NONE, nullptr);
  }
  wolfSSL_SetIORecv(ctx, wcRecv);
  wolfSSL_SetIOSend(ctx, wcSend);

  auto* ssl = wolfSSL_new(ctx);
  if (!ssl) {
    stop();
    return 0;
  }
  _ssl = ssl;
  wolfSSL_SetIOReadCtx(ssl, &_transport);
  wolfSSL_SetIOWriteCtx(ssl, &_transport);
  wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, host, strlen(host));
  if (verifyPeer && !_insecure) {
    // Also check the hostname against the cert SAN/CN, not just the chain.
    wolfSSL_check_domain_name(ssl, host);
  }

  // Resumption: only a connection that verifies (the roots were loaded above) keeps a session or
  // is offered one, and only under the key it would be verified with. If the server does not take
  // the offered session it runs a full handshake on this same connection, with the Certificate it
  // must then send verified as usual (tls13.c: the PSK is cleared when ServerHello picks none).
  _ticketLen = 0;
  _keepSession = false;
  _sessionOffered = false;
  const char* fullHandshakeReason = "unverified, sessions not kept";
  const bool resumable =
      verifying && mayResume && makeSessionKey(_sessionKey, host, port, _rootCA, _allowCertificateDateErrors);
  if (resumable) {
    fullHandshakeReason = "no cached session";
    wolfSSL_set_SessionTicket_cb(ssl, onSessionTicket, &_ticketLen);
    if (WOLFSSL_SESSION* cached = copyCachedSession(_sessionKey)) {
      _sessionOffered = wolfSSL_set_session(ssl, cached) == WOLFSSL_SUCCESS;
      wolfSSL_SESSION_free(cached);  // the connection holds its own reference once set
      if (!_sessionOffered) {
        // wolfSSL refuses a session past its lifetime (the ticket's hint, by the wall clock).
        dropCachedSession(_sessionKey);
        fullHandshakeReason = "cached session expired";
      }
    }
  } else if (verifying) {
    fullHandshakeReason = mayResume ? "host name too long to cache" : "TLS 1.2 retry, sessions not kept";
  }

  // Non-blocking recv => retry wolfSSL_connect across handshake round-trips.
  // Sample the heap low-water across the handshake (this is where ECC/RSA bignum
  // allocations peak) so callers can report the handshake's real heap trough,
  // distinct from the all-time ESP.getMinFreeHeap() figure.
  auto sampleHeapTrough = [this]() {
    const size_t freeNow = esp_get_free_heap_size();
    const size_t largestNow = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    if (freeNow < _handshakeMinFree) _handshakeMinFree = freeNow;
    if (largestNow < _handshakeMinLargest) _handshakeMinLargest = largestNow;
  };
  sampleHeapTrough();
  g_handshakeIo = {};
  g_chainVerify = {};
  const uint32_t handshakeStartMs = millis();
  uint32_t pollCount = 0;
  // Per-call durations. A wolfSSL_connect() call returns as soon as it needs more data, so each
  // entry is one uninterrupted stretch of work: the first covers building the ClientHello (the
  // ECDHE key generation), and whichever one consumes the server's flight covers the shared
  // secret plus the certificate chain verification. With compute dominating the handshake, the
  // shape of this list says which of those to attack -- a single fat entry at the end is chain
  // work, a fat first entry is key generation, and evenly spread is the math backend itself.
  constexpr size_t MAX_TIMED_CALLS = 12;
  uint32_t callMs[MAX_TIMED_CALLS] = {};
  size_t callCount = 0;
  auto timedConnect = [&]() {
    const uint32_t callStartMs = millis();
    g_chainVerify.callStartMs = callStartMs;
    const int rc = wolfSSL_connect(ssl);
    if (callCount < MAX_TIMED_CALLS) callMs[callCount++] = millis() - callStartMs;
    return rc;
  };
  const uint32_t deadline = millis() + 15000;
  int ret;
  while ((ret = timedConnect()) != WOLFSSL_SUCCESS) {
    sampleHeapTrough();
    pollCount++;
    const int err = wolfSSL_get_error(ssl, ret);
    if (err != WOLFSSL_ERROR_WANT_READ && err != WOLFSSL_ERROR_WANT_WRITE) {
      // Record whether this was a cert-verify failure so the caller can decide
      // on the insecure fallback; keep the log at DBG (connect() logs the WARN).
      _lastConnectErr = err;
      LOG_DBG("TLS", "wolfSSL_connect(%s) failed: err=%d", label, err);
      // A connection that failed with a session offered must not be offered it again.
      if (_sessionOffered) dropCachedSession(_sessionKey);
      stop();
      return 0;
    }
    if (static_cast<int32_t>(millis() - deadline) >= 0) {
      _lastConnectErr = WOLFSSL_ERROR_WANT_READ;  // treat timeout as transport, not verify
      LOG_INF("TLS", "handshake timeout to %s", host);
      if (_sessionOffered) dropCachedSession(_sessionKey);
      stop();
      return 0;
    }
    delay(5);
  }
  sampleHeapTrough();
  // What was actually negotiated. With most of the handshake measured as pure compute, the group
  // is the question that decides the fix: the SP fast path (sp_c32.c) covers P-256/P-384 only, so
  // an X25519 key share runs on the portable fe_operations.c code instead and none of the SP
  // tuning applies to it. Its own line -- the logger truncates at 256 chars including the prefix,
  // and appending this to the cost line below cut the group name off exactly when it mattered.
  const char* negotiatedCurve = wolfSSL_get_curve_name(ssl);
  const char* negotiatedCipher = wolfSSL_get_cipher_name(ssl);
  const char* negotiatedVersion = wolfSSL_get_version(ssl);
  LOG_DBG("TLS", "negotiated %s: %s / %s / group=%s", host, negotiatedVersion ? negotiatedVersion : "?",
          negotiatedCipher ? negotiatedCipher : "?", negotiatedCurve ? negotiatedCurve : "?");

  // Read this as three buckets that must add up: transport I/O, our own poll sleeps
  // (pollCount x 5 ms), and whatever is left, which is wolfSSL computing. Each has its own fix.
  const uint32_t handshakeMs = millis() - handshakeStartMs;
  const uint32_t ioMs = g_handshakeIo.ioUs / 1000;
  const uint32_t sleepMs = pollCount * 5;
  const long computeMs = static_cast<long>(handshakeMs) - static_cast<long>(ioMs) - static_cast<long>(sleepMs);
  LOG_DBG("TLS", "connect %s: tcp+dns=%lu hs=%lu = io %lu + sleep %lu + compute %ld | rx %lu/%luB tx %lu/%luB cpu=%lu",
          host, static_cast<unsigned long>(transportMs), static_cast<unsigned long>(handshakeMs),
          static_cast<unsigned long>(ioMs), static_cast<unsigned long>(sleepMs), computeMs,
          static_cast<unsigned long>(g_handshakeIo.recvCalls), static_cast<unsigned long>(g_handshakeIo.recvBytes),
          static_cast<unsigned long>(g_handshakeIo.sendCalls), static_cast<unsigned long>(g_handshakeIo.sendBytes),
          static_cast<unsigned long>(getCpuFrequencyMhz()));

  char callList[128];
  size_t callListLen = 0;
  for (size_t i = 0; i < callCount && callListLen < sizeof(callList) - 8; ++i) {
    callListLen += snprintf(callList + callListLen, sizeof(callList) - callListLen, i == 0 ? "%lu" : ",%lu",
                            static_cast<unsigned long>(callMs[i]));
  }
  LOG_DBG("TLS", "handshake call ms: [%s]%s", callList, callCount >= MAX_TIMED_CALLS ? " (truncated)" : "");

  if (g_chainVerify.count > 0) {
    char chainList[128];
    size_t chainLen = 0;
    const size_t shown = g_chainVerify.count < 8 ? g_chainVerify.count : 8;
    for (size_t i = 0; i < shown && chainLen < sizeof(chainList) - 16; ++i) {
      chainLen += snprintf(chainList + chainLen, sizeof(chainList) - chainLen, i == 0 ? "d%d:%lu" : " d%d:%lu",
                           g_chainVerify.depth[i], static_cast<unsigned long>(g_chainVerify.gapMs[i]));
    }
    // First entry includes the pre-certificate ServerHello work; the rest are per-certificate.
    LOG_DBG("TLS", "chain verify: %lu certs [%s] (first entry includes key exchange)",
            static_cast<unsigned long>(g_chainVerify.count), chainList);
  }

  // One line that settles the device test: a resumed connection received no certificate chain and
  // verified nothing; a full one says why it was not resumed.
  if (_sessionOffered && wolfSSL_session_reused(ssl)) {
    LOG_DBG("TLS", "resumed session for %s: no certificate chain, nothing verified", host);
  } else {
    if (_sessionOffered) {
      dropCachedSession(_sessionKey);  // the server would not take it; do not offer it again
      fullHandshakeReason = "server declined the cached session";
    }
    LOG_DBG("TLS", "full handshake for %s (%s)", host, fullHandshakeReason);
  }
  _keepSession = resumable;
  _connected = true;
  return 1;
}

// A connect attempt at one verification level, with the v23->TLS1.2 method retry.
int SecureClient::connectAtVerify(const char* host, uint16_t port, bool verifyPeer) {
  // v23 negotiates the highest mutually supported version (TLS 1.3 when offered,
  // 1.2 otherwise). WOLFSSL_TLS13 is enabled in user_settings.h.
  if (connectWithMethod(host, port, wolfSSLv23_client_method(), "auto", verifyPeer, /*mayResume=*/true)) return 1;
  // Some TLS-1.2-only servers choke on a 1.3-capable ClientHello; retry 1.2-only,
  // but only if the failure wasn't a cert-verify problem (that won't change).
  if (isVerificationError(_lastConnectErr)) return 0;
  return connectWithMethod(host, port, wolfTLSv1_2_client_method(), "tls1.2", verifyPeer, /*mayResume=*/false);
}

int SecureClient::connect(const char* host, uint16_t port) {
  _lastWasInsecure = false;
  // Must not leak across connects: a TCP/DNS failure records no handshake
  // error, and a stale verification code from an earlier attempt would
  // misclassify it and trigger a pointless insecure-fallback retry.
  _lastConnectErr = 0;
  _handshakeMinFree = SIZE_MAX;
  _handshakeMinLargest = SIZE_MAX;

  // If explicitly insecure (debug), skip verification outright.
  if (_insecure) {
    const int ok = connectAtVerify(host, port, /*verifyPeer=*/false);
    _lastWasInsecure = ok == 1;
    return ok;
  }

  // Verified-first.
  if (connectAtVerify(host, port, /*verifyPeer=*/true)) return 1;

  // Only fall back to insecure on a verification-class failure, when allowed.
  if (_allowInsecureFallback && isVerificationError(_lastConnectErr)) {
    LOG_INF("TLS", "WARNING: cert verify failed for %s (err=%d); retrying WITHOUT verification", host, _lastConnectErr);
    const int ok = connectAtVerify(host, port, /*verifyPeer=*/false);
    _lastWasInsecure = ok == 1;
    return ok;
  }

  if (!_allowInsecureFallback && isVerificationError(_lastConnectErr)) {
    LOG_ERR("TLS", "cert verify failed for %s (err=%d); insecure fallback disabled -> aborting", host, _lastConnectErr);
  }
  return 0;
}

int SecureClient::connect(IPAddress ip, uint16_t port) {
  char host[16];
  snprintf(host, sizeof(host), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  return connect(host, port);
}

size_t SecureClient::write(const uint8_t* buf, size_t size) {
  if (!_connected) return 0;
  const int n = wolfSSL_write(static_cast<WOLFSSL*>(_ssl), buf, size);
  return n > 0 ? static_cast<size_t>(n) : 0;
}

int SecureClient::read(uint8_t* buf, size_t size) {
  if (!_connected) return -1;
  auto* ssl = static_cast<WOLFSSL*>(_ssl);
  const int n = wolfSSL_read(ssl, buf, size);
  if (n > 0) return n;

  const int err = wolfSSL_get_error(ssl, n);
  if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) return 0;  // no data yet
  if (err == WOLFSSL_ERROR_ZERO_RETURN) {                                           // peer closed cleanly
    _connected = false;
    return 0;
  }
  // A hard read error ends the body early (the HTTP layer reports it as truncated), so say why:
  // MEMORY_E (-125) here means no contiguous block for the next record buffer.
  LOG_ERR("TLS", "read failed: err=%d ret=%d heap=%u largest=%u", err, n, esp_get_free_heap_size(),
          heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
  _connected = false;
  _keepSession = false;  // a broken connection's session is not kept, ticket or not
  return -1;
}

int SecureClient::available() {
  if (!_connected) return 0;
  return wolfSSL_pending(static_cast<WOLFSSL*>(_ssl)) + _transport.available();
}

void SecureClient::stop() {
  if (_ssl) {
    auto* ssl = static_cast<WOLFSSL*>(_ssl);
    // Closing is when the session is complete: a TLS 1.3 ticket only arrives after the handshake.
    WOLFSSL_SESSION* keep = _keepSession ? sessionToKeep(ssl, _sessionKey, _ticketLen) : nullptr;
    wolfSSL_free(ssl);
    _ssl = nullptr;
    // Published only now that the connection has let go of it, so the cache is its sole owner.
    if (keep) storeCachedSession(_sessionKey, keep);
  }
  _keepSession = false;
  _ticketLen = 0;
  if (_ctx) {
    wolfSSL_CTX_free(static_cast<WOLFSSL_CTX*>(_ctx));
    _ctx = nullptr;
  }
  _transport.stop();
  _connected = false;
}

uint8_t SecureClient::connected() { return _connected && _transport.connected(); }

#else  // !FREEINK_NET_WOLFSSL — inert stub so the firmware builds without wolfSSL.

void SecureClient::clearSessionCache() {}
int SecureClient::connectWithMethod(const char*, uint16_t, void*, const char*, bool, bool) { return 0; }
int SecureClient::connectAtVerify(const char*, uint16_t, bool) { return 0; }
int SecureClient::connect(const char* host, uint16_t port) {
  (void)host;
  (void)port;
  LOG_ERR("TLS", "TLS unavailable: build with -DFREEINK_NET_WOLFSSL=1");
  return 0;
}
int SecureClient::connect(IPAddress ip, uint16_t port) {
  (void)ip;
  (void)port;
  return 0;
}
size_t SecureClient::write(const uint8_t* buf, size_t size) {
  (void)buf;
  (void)size;
  return 0;
}
int SecureClient::read(uint8_t* buf, size_t size) {
  (void)buf;
  (void)size;
  return -1;
}
int SecureClient::available() { return 0; }
void SecureClient::stop() {
  _transport.stop();
  _connected = false;
}
uint8_t SecureClient::connected() { return 0; }

#endif

// --- transport-agnostic single-byte helpers (shared) ---
size_t SecureClient::write(uint8_t b) { return write(&b, 1); }
int SecureClient::read() {
  uint8_t b;
  return read(&b, 1) == 1 ? b : -1;
}
int SecureClient::peek() { return -1; }
void SecureClient::flush() {}

}  // namespace crosspoint
