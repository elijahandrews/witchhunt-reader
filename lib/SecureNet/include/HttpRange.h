#pragma once

// WitchReader SecureNet — HTTP Range helpers for bounded (chunked) downloads.
//
// Why: wolfSSL reads each TLS record into one heap buffer of the record's length, and a server may
// send records of up to 16 KB. On the C3 that is a 17,408 B block (TLSF rounds a 16,401 B request up
// to its size class), which a network session on the X3 does not have once the handshake and the
// Wi-Fi receive buffers have taken their share. A Range request for C bytes bounds the response, and
// with it every record, to C plus the response headers: the server cannot send more than that.
// GitHub's raw CDN, for example, sends 16 KB records once a connection has carried ~56 KB, but an
// 8 KB range arrives as one 8,209 B record (measured with openssl -trace, 2026-10-05).
//
// Pure functions, no Arduino: host-tested in test/http_range.

#include <cstddef>
#include <string>

namespace crosspoint {
namespace http_range {

// --- request / response -------------------------------------------------------------------------

// The value of a Range request header for bytes first..last inclusive: "bytes=first-last".
std::string rangeHeaderValue(size_t first, size_t last);

// A parsed Content-Range response header (RFC 9110 14.4).
struct ContentRange {
  bool satisfied = false;   // "bytes first-last/..."; false for the 416 form "bytes */total"
  size_t first = 0;         // valid when satisfied
  size_t last = 0;          // valid when satisfied, inclusive
  bool totalKnown = false;  // false for ".../*"
  size_t total = 0;         // whole-resource size, valid when totalKnown
};

// Parses "bytes 0-8191/269830", "bytes 0-8191/*" and "bytes */269830" (unit case-insensitive,
// surrounding spaces allowed). False for anything else, including last < first, last >= total and
// numbers that do not fit; out is then left as default.
bool parseContentRange(const char* value, ContentRange& out);

// What a response to a Range request for bytes from requestedFirst means for the download.
enum class RangeReply {
  Partial,        // 206 for exactly the requested start: append the body
  WholeBody,      // 200: the server ignored Range and sends the whole resource from byte 0
  Unsatisfiable,  // 416: nothing at or after requestedFirst
  Mismatch,       // 206 without a usable Content-Range, or for a different start
  Error,          // any other status
};
RangeReply classifyReply(int status, bool haveContentRange, const ContentRange& range, size_t requestedFirst);

// --- chunk planning -----------------------------------------------------------------------------

struct Chunk {
  size_t first = 0;
  size_t last = 0;  // inclusive
  size_t length() const { return last - first + 1; }
};

// The next range to request from offset. False when the total is known and reached. When the
// total is unknown the range is a full chunk; the transfer ends at a short chunk or a 416.
bool nextChunk(size_t offset, size_t chunkSize, bool totalKnown, size_t total, Chunk& out);

// Whether the transfer is complete after a 206 that delivered `received` of `requested` bytes and
// left the byte count at `offset`.
bool transferComplete(size_t offset, bool totalKnown, size_t total, size_t requested, size_t received);

// --- sizing from the heap -----------------------------------------------------------------------

// The C3 heap block a malloc(request) occupies: ESP-IDF's TLSF rounds every request above 64 B up
// to its second-level class (heap/tlsf mapping_search; 16 classes per power of two on pools up to
// 256 KB), after light heap poisoning has added 12 B. On the S3's PSRAM pool (32 classes) the real
// block is smaller, so this overestimates there, which is the safe side.
size_t heapBlockFor(size_t request);

// The record buffer a response of chunkSize body bytes can make wolfSSL allocate, in the worst
// case of headers and body sharing one record: body + header allowance + AEAD expansion, as a
// heap block.
size_t recordBlockFor(size_t chunkSize);

// What the Wi-Fi/lwIP receive path can hold at once while that record arrives: the TCP window
// (CONFIG_LWIP_TCP_WND_DEFAULT 5760 = 4 segments), or the whole response if it is smaller, as Wi-Fi
// receive buffers.
size_t receiveBuffersFor(size_t chunkSize);

// The largest chunk from {8, 6, 4, 2, 1} KB whose record block and receive buffers both fit in the
// largest free block, so they fit even if every receive buffer is carved out of that block. 1 KB
// when none fits: the smallest records are the best remaining chance.
size_t chunkSizeForLargestBlock(size_t largestFreeBlock);

// Chunk only when memory is tight. Streaming means 16 KB records: a 17,408 B record block plus the
// receive buffers is ~24 KB, needed whole again for every record. 40 KB leaves room for one
// unrelated allocation to split the block between records. The S3 boards (PSRAM in the default
// heap) are always above it; the C3 boards with Wi-Fi up are below it.
bool shouldChunk(size_t largestFreeBlock);

constexpr size_t DEFAULT_CHUNK_BYTES = 8192;
constexpr size_t MIN_CHUNK_BYTES = 1024;
constexpr size_t STREAM_MIN_LARGEST_BLOCK = 40 * 1024;
// GitHub's 206 headers measured 968 B; most servers send less.
constexpr size_t RESPONSE_HEADER_ALLOWANCE = 1536;
// The largest per-record expansion of the suites we negotiate: TLS 1.2 AES-GCM (8-byte explicit
// nonce + 16-byte tag). TLS 1.3 adds 17 (content type + tag).
constexpr size_t RECORD_EXPANSION = 24;

}  // namespace http_range
}  // namespace crosspoint
