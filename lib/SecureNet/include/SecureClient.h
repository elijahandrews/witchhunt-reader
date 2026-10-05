#pragma once

// WitchReader SecureNet — TLS 1.3 secure client.
//
// Vendored/adapted from Free-Ink/freeink-sdk (MIT), libs/network/SecureNet,
// and crosspoint-reader PR #2475. wolfSSL is GPLv2/commercial — used here under
// GPL compatibility with this firmware.
//
// WHY: the precompiled mbedTLS shipped in the pioarduino ESP-IDF package has
// TLS 1.3 compiled out as empty stubs, so esp_http_client / WiFiClientSecure
// cannot reach TLS-1.3-only servers (e.g. KOSync at sync.koreader.rocks).
// SecureClient is an Arduino Client wrapping a wolfSSL session over a plain
// WiFiClient transport, independent of the system mbedTLS.
//
// Cert policy (WitchReader): verified-first. connect() verifies the peer against
// the curated root set (see CrossPointRoots.h). On a *verification-class*
// failure it optionally logs a warning and retries once with verification
// disabled (setAllowInsecureFallback(true), the default). Non-verification
// failures (DNS/TCP/handshake) never trigger the insecure retry. OTA disables
// the fallback so firmware download always fails closed.
//
// OPT-IN: enable with -DFREEINK_NET_WOLFSSL=1 and add wolfSSL to lib_deps. With
// the flag off, this compiles to an inert stub (connect() returns 0) so the
// firmware builds without the wolfSSL dependency present.

#include <Arduino.h>
#include <Client.h>
#include <WiFiClient.h>

#include <cstddef>
#include <cstdint>

namespace crosspoint {

// What a kept TLS session is filed under: the host and port it was made to, and the verification
// it passed (trust store, and whether certificate dates were waived for want of a clock). Only
// SecureClient uses it; it sits at namespace scope so the session cache in SecureClient.cpp can.
struct TlsSessionKey {
  static constexpr size_t HOST_MAX = 64;  // a longer host name is simply not cached
  char host[HOST_MAX];
  const char* roots;
  uint16_t port;
  bool datesWaived;
};

class SecureClient : public Client {
 public:
  SecureClient() = default;
  ~SecureClient() override;

  // Certificate / verification configuration (applied before connect()).
  // rootCA is a (concatenated) PEM string; not copied — must outlive the client.
  void setCACert(const char* rootCA) { _rootCA = rootCA; }
  // Skip peer verification entirely: no trust store, no hostname check, and
  // lastConnectWasInsecure() reports it. Takes a bool because the client is reused
  // across requests, so the caller must be able to turn it back off.
  void setInsecure(bool insecure = true) { _insecure = insecure; }
  // When true (default) and a CA is set, a verification-class handshake failure
  // is retried once with verification disabled (logged). OTA sets this false.
  void setAllowInsecureFallback(bool allow) { _allowInsecureFallback = allow; }
  // Keep CA-chain, signature, and hostname verification enabled, but tolerate certificate
  // notBefore/notAfter errors because the device has no trustworthy wall clock. Unlike the
  // insecure fallback this waives exactly one property: every non-date verification failure
  // stays fatal. Off by default — callers turn it on only after HalClock::ensureUsableForTls()
  // has failed, i.e. when full date validation is genuinely unobtainable.
  //
  // This has to act in TWO places, which is easy to get wrong:
  //   1. loading the trust store. wolfSSL_CTX_load_verify_buffer() date-checks the ROOTS as it
  //      parses them, so on a 1970 clock every curated root's notBefore is "in the future" and
  //      the store fails to load outright — before any handshake, so no verify callback can
  //      rescue it and no verification error is even produced to classify.
  //   2. the handshake itself, for the peer chain, via a verify callback.
  // Approach adapted from the Kapfilm/witchhunt-reader fork, which had (2); (1) is what makes
  // it actually work on a cold-booted RTC-less board.
  void setAllowCertificateDateErrors(bool allow) { _allowCertificateDateErrors = allow; }
  // True if the last successful connect() fell back to an unverified handshake.
  bool lastConnectWasInsecure() const { return _lastWasInsecure; }
  // True if the last read() that failed hard on this connection could not allocate the buffer for
  // a TLS record (wolfSSL MEMORY_E). Cleared by the next connect().
  bool lastReadWasOutOfMemory() const;

  // Heap low-water sampled ACROSS the last handshake (free bytes / largest
  // contiguous block). Distinct from ESP.getMinFreeHeap() (all-time since boot):
  // this isolates what the TLS handshake itself cost. SIZE_MAX until a connect runs.
  size_t handshakeMinFree() const { return _handshakeMinFree; }
  size_t handshakeMinLargest() const { return _handshakeMinLargest; }

  // Connect + TLS handshake to host:port (SNI = host). Returns 1 on success.
  int connect(IPAddress ip, uint16_t port) override;
  int connect(const char* host, uint16_t port) override;

  size_t write(uint8_t b) override;
  size_t write(const uint8_t* buf, size_t size) override;
  int available() override;
  int read() override;
  int read(uint8_t* buf, size_t size) override;
  int peek() override;
  void flush() override;
  void stop() override;
  uint8_t connected() override;
  operator bool() override { return connected(); }

  // True if this build has wolfSSL TLS 1.3 support compiled in.
  static bool tls13Available();

  // TLS 1.3 session resumption. A connection that verified its peer keeps the session ticket the
  // server sends after the handshake. The next connect() to the same host and port, verifying
  // against the same trust store under the same date policy, offers it; a server that accepts it
  // sends no certificate chain, so no chain verification runs. Unverified connections neither
  // keep nor use a session. At most two are kept. This drops them all: call it when the network
  // session ends, so a ticket never outlives the network session it came from.
  static void clearSessionCache();

 private:
  // verifyPeer: true = load the curated CA + WOLFSSL_VERIFY_PEER; false = VERIFY_NONE.
  // mayResume: the method can negotiate TLS 1.3, so a cached session may be offered (the kept
  // sessions are TLS 1.3 ones, and a TLS-1.2-only method must not be handed one).
  int connectWithMethod(const char* host, uint16_t port, void* method, const char* label, bool verifyPeer,
                        bool mayResume);
  // One connect attempt at the given verification level, incl. the TLS1.2 retry.
  int connectAtVerify(const char* host, uint16_t port, bool verifyPeer);

  WiFiClient _transport;
  const char* _rootCA = nullptr;
  bool _insecure = false;
  bool _allowInsecureFallback = true;
  bool _allowCertificateDateErrors = false;
  bool _lastWasInsecure = false;
  int _lastConnectErr = 0;                 // wolfSSL_get_error() from the last failed handshake
  int _lastReadErr = 0;                    // wolfSSL_get_error() from the last hard read failure
  size_t _handshakeMinFree = SIZE_MAX;     // heap trough during the last handshake
  size_t _handshakeMinLargest = SIZE_MAX;  // largest-block trough during the last handshake
  void* _ssl = nullptr;                    // WOLFSSL*      (opaque; keeps wolfSSL headers out of here)
  void* _ctx = nullptr;                    // WOLFSSL_CTX*
  bool _connected = false;
  // Session resumption (see clearSessionCache()).
  TlsSessionKey _sessionKey = {};  // this connection's key; valid while _keepSession is set
  int _ticketLen = 0;              // length of the last session ticket on this connection, 0 = none
  bool _keepSession = false;       // a verified handshake completed: keep its ticket on stop()
  bool _sessionOffered = false;    // this connect offered a cached session
};

}  // namespace crosspoint
