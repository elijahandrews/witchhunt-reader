#pragma once
// Host crypto adapter: the activity's real hash/stream logic uses OpenSSL in this harness.
#include <openssl/sha.h>
using wc_Sha256 = SHA256_CTX;
inline int wc_InitSha256(wc_Sha256* s) { return SHA256_Init(s) == 1 ? 0 : -1; }
inline int wc_Sha256Update(wc_Sha256* s, const void* b, unsigned n) { return SHA256_Update(s, b, n) == 1 ? 0 : -1; }
inline int wc_Sha256Final(wc_Sha256* s, unsigned char* out) { return SHA256_Final(out, s) == 1 ? 0 : -1; }
inline void wc_Sha256Free(wc_Sha256*) {}
