#pragma once

#include <cstddef>
#include <cstdint>

#if CONFIG_IDF_TARGET_ESP32S3
// X4 Pro's SDK already links the hardware SHA implementation. Keep this local
// to book sync; changing wolfSSL's TLS crypto configuration is unnecessary.
#include <mbedtls/sha256.h>
#else
#include <wolfssl/wolfcrypt/sha256.h>
#endif

class BookSyncHash {
 public:
#if CONFIG_IDF_TARGET_ESP32S3
  mbedtls_sha256_context context{};
  bool start() {
    mbedtls_sha256_init(&context);
    return mbedtls_sha256_starts(&context, 0) == 0;
  }
  bool update(const uint8_t* data, size_t n) { return mbedtls_sha256_update(&context, data, n) == 0; }
  bool finish(uint8_t* digest) { return mbedtls_sha256_finish(&context, digest) == 0; }
  void free() { mbedtls_sha256_free(&context); }
#else
  wc_Sha256 context{};
  bool start() { return wc_InitSha256(&context) == 0; }
  bool update(const uint8_t* data, size_t n) { return wc_Sha256Update(&context, data, n) == 0; }
  bool finish(uint8_t* digest) { return wc_Sha256Final(&context, digest) == 0; }
  void free() { wc_Sha256Free(&context); }
#endif
};
