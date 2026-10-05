#include "HttpRange.h"

#include <cctype>
#include <cstdint>

namespace crosspoint {
namespace http_range {

namespace {

// Reads one decimal number. False when there is no digit or the value does not fit in size_t.
bool parseNumber(const char*& p, size_t& out) {
  if (*p < '0' || *p > '9') return false;
  size_t value = 0;
  while (*p >= '0' && *p <= '9') {
    const size_t digit = static_cast<size_t>(*p - '0');
    if (value > (SIZE_MAX - digit) / 10) return false;
    value = value * 10 + digit;
    ++p;
  }
  out = value;
  return true;
}

void skipSpaces(const char*& p) {
  while (*p == ' ' || *p == '\t') ++p;
}

// ESP-IDF TLSF on the C3: 4-byte alignment, 16 second-level classes per power of two (pools up to
// 256 KB, tlsf.c control_construct), rounding from 64 B up (small_block_size = 1 << (4 + 2)).
constexpr size_t HEAP_ALIGN = 4;
constexpr unsigned TLSF_SL_LOG2 = 4;
constexpr size_t TLSF_SMALL_BLOCK = 64;
// CONFIG_HEAP_POISONING_LIGHT: 8-byte head + 4-byte tail canary per allocation.
constexpr size_t HEAP_POISON_BYTES = 12;
// lwIP receive window (CONFIG_LWIP_TCP_WND_DEFAULT 5760) in segments of CONFIG_LWIP_TCP_MSS 1436.
constexpr size_t TCP_MSS = 1436;
constexpr size_t TCP_WINDOW_SEGMENTS = 4;
// A full-MSS frame as the Wi-Fi driver hands it to lwIP (802.11 + LLC + IP + TCP headers on top).
constexpr size_t WIFI_RX_FRAME_BYTES = 1600;

unsigned highestBit(size_t value) {
  unsigned bit = 0;
  while (value >>= 1) ++bit;
  return bit;
}

}  // namespace

std::string rangeHeaderValue(size_t first, size_t last) {
  return "bytes=" + std::to_string(first) + "-" + std::to_string(last);
}

bool parseContentRange(const char* value, ContentRange& out) {
  out = ContentRange{};
  if (value == nullptr) return false;
  const char* p = value;
  skipSpaces(p);
  // The unit, case-insensitively. The comparison stops at the first mismatch, so a short string
  // ends it at its terminator.
  static constexpr char UNIT[] = "bytes";
  for (size_t i = 0; i + 1 < sizeof(UNIT); ++i) {
    if (std::tolower(static_cast<unsigned char>(p[i])) != UNIT[i]) return false;
  }
  p += sizeof(UNIT) - 1;
  if (*p != ' ') return false;
  skipSpaces(p);

  ContentRange range;
  if (*p == '*') {
    ++p;
  } else {
    if (!parseNumber(p, range.first) || *p != '-') return false;
    ++p;
    if (!parseNumber(p, range.last) || range.last < range.first) return false;
    range.satisfied = true;
  }
  if (*p != '/') return false;
  ++p;
  if (*p == '*') {
    if (!range.satisfied) return false;  // "*/*" says nothing at all
    ++p;
  } else {
    if (!parseNumber(p, range.total)) return false;
    range.totalKnown = true;
    if (range.satisfied && range.last >= range.total) return false;
  }
  skipSpaces(p);
  if (*p != '\0') return false;
  out = range;
  return true;
}

RangeReply classifyReply(int status, bool haveContentRange, const ContentRange& range, size_t requestedFirst) {
  switch (status) {
    case 206:
      return haveContentRange && range.satisfied && range.first == requestedFirst ? RangeReply::Partial
                                                                                  : RangeReply::Mismatch;
    case 200:
      return RangeReply::WholeBody;
    case 416:
      return RangeReply::Unsatisfiable;
    default:
      return RangeReply::Error;
  }
}

bool nextChunk(size_t offset, size_t chunkSize, bool totalKnown, size_t total, Chunk& out) {
  if (chunkSize == 0) return false;
  if (totalKnown && offset >= total) return false;
  size_t last = offset > SIZE_MAX - (chunkSize - 1) ? SIZE_MAX : offset + (chunkSize - 1);
  if (totalKnown && last >= total) last = total - 1;
  out.first = offset;
  out.last = last;
  return true;
}

bool transferComplete(size_t offset, bool totalKnown, size_t total, size_t requested, size_t received) {
  if (totalKnown) return offset >= total;
  return received < requested;
}

size_t heapBlockFor(size_t request) {
  size_t size = (request + HEAP_POISON_BYTES + HEAP_ALIGN - 1) & ~(HEAP_ALIGN - 1);
  if (size >= TLSF_SMALL_BLOCK) {
    const size_t round = static_cast<size_t>(1) << (highestBit(size) - TLSF_SL_LOG2);
    size = (size + round - 1) & ~(round - 1);
  }
  return size;
}

size_t recordBlockFor(size_t chunkSize) {
  return heapBlockFor(chunkSize + RESPONSE_HEADER_ALLOWANCE + RECORD_EXPANSION);
}

size_t receiveBuffersFor(size_t chunkSize) {
  const size_t responseBytes = chunkSize + RESPONSE_HEADER_ALLOWANCE;
  size_t segments = (responseBytes + TCP_MSS - 1) / TCP_MSS;
  if (segments > TCP_WINDOW_SEGMENTS) segments = TCP_WINDOW_SEGMENTS;
  return segments * heapBlockFor(WIFI_RX_FRAME_BYTES);
}

size_t chunkSizeForLargestBlock(size_t largestFreeBlock) {
  static constexpr size_t LADDER[] = {DEFAULT_CHUNK_BYTES, 6144, 4096, 2048, MIN_CHUNK_BYTES};
  for (const size_t chunk : LADDER) {
    if (recordBlockFor(chunk) + receiveBuffersFor(chunk) <= largestFreeBlock) return chunk;
  }
  return MIN_CHUNK_BYTES;
}

bool shouldChunk(size_t largestFreeBlock) { return largestFreeBlock < STREAM_MIN_LARGEST_BLOCK; }

}  // namespace http_range
}  // namespace crosspoint
