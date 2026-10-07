#pragma once

#include <SaxParser/SaxParser.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Streaming, layout-independent phrase matching. Source offsets refer to the ORIGINAL
// inflated XHTML, so a result can be resolved by Section under any font/viewport.
// Own one on the heap for a search session: fixed scratch avoids allocations per character
// and bounds RAM independently of chapter length and the number of matches.
class BookTextSearch {
 public:
  static constexpr size_t MAX_QUERY_BYTES = 128;
  static constexpr size_t SNIPPET_BYTES = 240;
  using MatchCallback = bool (*)(void*, uint32_t, const char*);

  BookTextSearch(std::string_view query, MatchCallback callback, void* context);
  bool validQuery() const { return queryLength_ != 0; }
  bool beginChapter();
  bool feed(const uint8_t* bytes, size_t length);
  bool finishChapter();
  bool stopped() const { return stopped_; }

 private:
  static constexpr size_t FOLDED_BYTES = 512;
  static constexpr size_t HISTORY_SIZE = 256;
  static constexpr size_t PENDING_SIZE = 64;
  static constexpr uint32_t BEFORE = 24;
  static constexpr uint32_t AFTER = 32;
  struct Token {
    uint32_t codepoint = 0;
    uint32_t source = 0;
  };
  struct Pending {
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t source = 0;
  };

  SaxParser parser_;
  MatchCallback callback_;
  void* context_;
  std::array<char, FOLDED_BYTES> query_{};
  std::array<uint16_t, FOLDED_BYTES> prefix_{};
  std::array<uint32_t, FOLDED_BYTES> origins_{};
  std::array<Token, HISTORY_SIZE> history_{};
  std::array<Pending, PENDING_SIZE> pending_{};
  std::array<char, SNIPPET_BYTES + 1> snippet_{};
  size_t queryLength_ = 0;
  size_t matched_ = 0;
  uint32_t foldedCount_ = 0;
  uint32_t tokenCount_ = 0;
  size_t pendingFirst_ = 0;
  size_t pendingCount_ = 0;
  uint32_t lastMatchSource_ = UINT32_MAX;
  int depth_ = 0;
  int bodyDepth_ = -1;
  int skipDepth_ = -1;
  bool stopped_ = false;
  bool previousSpace_ = true;
  // A UTF-8 character may cross SAX/feed chunks; its raw origin is its first byte.
  uint32_t utf8Value_ = 0;
  uint32_t utf8Source_ = 0;
  uint32_t utf8Minimum_ = 0;
  uint8_t utf8Remaining_ = 0;

  void acceptByte(uint8_t byte, uint32_t source);
  void acceptCodepoint(uint32_t cp, uint32_t source);
  void foldCodepoint(uint32_t cp, uint32_t ordinal);
  void foldByte(char byte, uint32_t ordinal);
  void emitPending();
  void boundary();
  static void startElement(void*, const char*, const char**);
  static void endElement(void*, const char*);
  static void characters(void*, const char*, int);
  static void entity(void*, const char*, int);
};
