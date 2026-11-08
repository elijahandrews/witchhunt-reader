#pragma once
// Host adapter tests stream orchestration, not the ESP32 hardware peripheral.
#include <openssl/sha.h>
using mbedtls_sha256_context = SHA256_CTX;
inline bool failHashStart = false, failHashUpdate = false, failHashFinish = false;
inline void mbedtls_sha256_init(mbedtls_sha256_context*) {}
inline int mbedtls_sha256_starts(mbedtls_sha256_context* s, int is224) {
  return !failHashStart && !is224 && SHA256_Init(s) == 1 ? 0 : -1;
}
inline int mbedtls_sha256_update(mbedtls_sha256_context* s, const unsigned char* p, size_t n) {
  return !failHashUpdate && SHA256_Update(s, p, n) == 1 ? 0 : -1;
}
inline int mbedtls_sha256_finish(mbedtls_sha256_context* s, unsigned char* p) {
  return !failHashFinish && SHA256_Final(p, s) == 1 ? 0 : -1;
}
inline void mbedtls_sha256_free(mbedtls_sha256_context*) {}
