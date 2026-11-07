#pragma once

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string_view>

// Bounded, line-oriented v1 inventory. Filenames are ASCII and carry a SHA-256 of the
// source-relative path, keeping case-sensitive server paths distinct on FAT. No remote URLs
// or directory components are accepted. Views live only until the next line is read.
namespace BookSync {
constexpr uint32_t MAX_BOOK_BYTES = 512U * 1024 * 1024;
constexpr uint32_t MAX_MANIFEST_BYTES = 3U * 1024 * 1024;
constexpr uint32_t MAX_BOOKS = 10000;
constexpr size_t LINE_CAP = 256;
constexpr size_t NAME_CAP = 131;

// A fixed 512-byte size filter avoids reading EPUBs which cannot possibly match
// this inventory. Collisions only cause extra SHA-256 work, never a false match.
// Saturation on large libraries degrades to the full scan without growing RAM.
struct CandidateSizes {
  uint32_t bits[128]{};
  static uint32_t key(uint32_t size) {
    size ^= size >> 16;
    size *= 0x7feb352dU;
    return size ^ (size >> 15);
  }
  void add(uint32_t size) {
    const uint32_t value = key(size);
    for (const uint32_t bit : {value & 4095U, (value >> 16) & 4095U}) bits[bit / 32] |= 1U << (bit % 32);
  }
  bool mayContain(uint32_t size) const {
    const uint32_t value = key(size);
    for (const uint32_t bit : {value & 4095U, (value >> 16) & 4095U})
      if (!(bits[bit / 32] & (1U << (bit % 32)))) return false;
    return true;
  }
};

inline bool hex(std::string_view value) {
  for (char c : value)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return !value.empty();
}
inline bool number(std::string_view value, uint32_t max, uint32_t& out) {
  out = 0;
  if (value.empty() || value.size() > 10) return false;
  for (char c : value) {
    if (c < '0' || c > '9' || out > max / 10 || (out == max / 10 && uint32_t(c - '0') > max % 10)) return false;
    out = out * 10 + c - '0';
  }
  return true;
}
inline bool origin(std::string_view url) {
  if (url.size() > 255 || url.substr(0, 8) != "https://") return false;
  auto host = url.substr(8);
  const auto colon = host.find(':');
  if (colon != host.npos) {
    uint32_t port;
    if (!number(host.substr(colon + 1), 65535, port) || port == 0) return false;
    host = host.substr(0, colon);
  }
  if (host.empty() || host.front() == '.' || host.back() == '.' || host.find("..") != host.npos) return false;
  for (char c : host)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-'))
      return false;
  return true;
}
struct Entry {
  std::string_view hash, name, id;
  uint32_t size = 0;
};
struct Manifest {
  enum class Line { Invalid, Header, Book, End };
  uint32_t count = 0;
  bool started = false, ended = false;
  char previous[NAME_CAP] = {};
  Line accept(std::string_view text, Entry& out) {
    if (ended) return Line::Invalid;
    if (!started) {
      started = true;
      return text == "WITCHSYNC1" ? Line::Header : Line::Invalid;
    }
    if (text.substr(0, 4) == "END\t") {
      uint32_t total;
      ended = number(text.substr(4), MAX_BOOKS, total) && total == count;
      return ended ? Line::End : Line::Invalid;
    }
    const auto first = text.find('\t'), second = text.find('\t', first == text.npos ? 0 : first + 1);
    if (first != 64 || second == text.npos || !hex(text.substr(0, first)) ||
        !number(text.substr(first + 1, second - first - 1), MAX_BOOK_BYTES, out.size) || out.size < 4)
      return Line::Invalid;
    out.name = text.substr(second + 1);
    if (out.name.size() < 71 || out.name.size() >= NAME_CAP || out.name.substr(out.name.size() - 5) != ".epub")
      return Line::Invalid;
    const size_t separator = out.name.size() - 70;
    if (!separator || separator > 60 || out.name[separator] != '~') return Line::Invalid;
    for (char c : out.name.substr(0, separator))
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' ||
            c == '-'))
        return Line::Invalid;
    out.id = out.name.substr(separator + 1, 64);
    if (!hex(out.id) || out.name <= std::string_view(previous) || count >= MAX_BOOKS) return Line::Invalid;
    memcpy(previous, out.name.data(), out.name.size());
    previous[out.name.size()] = 0;
    out.hash = text.substr(0, 64);
    ++count;
    return Line::Book;
  }
};
}  // namespace BookSync
