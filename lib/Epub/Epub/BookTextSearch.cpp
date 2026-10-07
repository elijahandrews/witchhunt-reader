#include "BookTextSearch.h"

#include <Utf8.h>
#include <Utf8CaseData.h>

#include <algorithm>
#include <cstring>
#include <iterator>

#include "htmlEntities.h"

namespace {
bool isSpace(uint32_t cp) {
  return (cp >= 9 && cp <= 13) || cp == 32 || cp == 0x85 || cp == 0xa0 || cp == 0x1680 ||
         (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 || cp == 0x202f || cp == 0x205f || cp == 0x3000;
}
size_t encode(uint32_t cp, char* out) {
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xc0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 63));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = static_cast<char>(0xe0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 63));
    out[2] = static_cast<char>(0x80 | (cp & 63));
    return 3;
  }
  out[0] = static_cast<char>(0xf0 | (cp >> 18));
  out[1] = static_cast<char>(0x80 | ((cp >> 12) & 63));
  out[2] = static_cast<char>(0x80 | ((cp >> 6) & 63));
  out[3] = static_cast<char>(0x80 | (cp & 63));
  return 4;
}
// Reuse the renderer's full Unicode case table, without a temporary string per character.
const char* upper(uint32_t cp, char (&scratch)[5]) {
  if (cp >= 'a' && cp <= 'z') cp -= 32;
  if (cp >= 0x80) {
    const auto* first = std::begin(utf8_case_detail::upperMappings);
    const auto* last = std::end(utf8_case_detail::upperMappings);
    const auto* found =
        std::lower_bound(first, last, cp, [](const auto& item, uint32_t value) { return item.codepoint < value; });
    if (found != last && found->codepoint == cp) return found->text;
  }
  scratch[encode(cp, scratch)] = '\0';
  return scratch;
}
std::string_view localName(const char* name) {
  const char* colon = std::strchr(name, ':');
  return colon ? colon + 1 : name;
}
bool block(std::string_view name) {
  static constexpr const char* names[] = {"p",  "div",     "section", "article",    "aside",  "blockquote", "pre", "br",
                                          "hr", "h1",      "h2",      "h3",         "h4",     "h5",         "h6",  "li",
                                          "ul", "ol",      "dl",      "dt",         "dd",     "table",      "tr",  "td",
                                          "th", "caption", "figure",  "figcaption", "header", "footer"};
  for (const char* candidate : names)
    if (name == candidate) return true;
  return false;
}
bool hidden(const char** atts) {
  for (size_t i = 0; atts && atts[i]; i += 2) {
    if (std::strcmp(atts[i], "hidden") == 0) return true;
    // Honor the unambiguous inline hide directive. The search is independent of reader CSS
    // settings; external stylesheet visibility is deliberately not a second style resolver.
    if (std::strcmp(atts[i], "style") == 0) {
      std::string_view style(atts[i + 1]);
      size_t pos = 0;
      while (pos < style.size()) {
        const size_t end = style.find(';', pos);
        auto declaration = style.substr(pos, end == std::string_view::npos ? end : end - pos);
        char compact[32]{};
        size_t length = 0;
        for (char c : declaration) {
          if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
          if (length + 1 >= sizeof(compact)) break;
          compact[length++] = c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
        }
        if (std::strcmp(compact, "display:none") == 0 || std::strcmp(compact, "display:none!important") == 0)
          return true;
        if (end == std::string_view::npos) break;
        pos = end + 1;
      }
    }
  }
  return false;
}
}  // namespace

BookTextSearch::BookTextSearch(std::string_view query, MatchCallback callback, void* context)
    : callback_(callback), context_(context) {
  if (query.empty() || query.size() > MAX_QUERY_BYTES) return;
  // Query normalization uses the same decoder/space/ignore/case path as source text. A null
  // callback marks this brief construction phase; no parser or match records are involved.
  callback_ = nullptr;
  for (const auto byte : query) acceptByte(static_cast<uint8_t>(byte), 0);
  if (utf8Remaining_) acceptCodepoint(0xfffd, 0);
  while (queryLength_ && query_[queryLength_ - 1] == ' ') --queryLength_;
  callback_ = callback;
  for (size_t i = 1, prefix = 0; i < queryLength_; ++i) {
    while (prefix && query_[i] != query_[prefix]) prefix = prefix_[prefix - 1];
    if (query_[i] == query_[prefix]) ++prefix;
    prefix_[i] = static_cast<uint16_t>(prefix);
  }
}

bool BookTextSearch::beginChapter() {
  if (!validQuery() || !callback_) return false;
  matched_ = foldedCount_ = tokenCount_ = pendingFirst_ = pendingCount_ = 0;
  lastMatchSource_ = UINT32_MAX;
  depth_ = 0;
  bodyDepth_ = skipDepth_ = -1;
  utf8Remaining_ = 0;
  previousSpace_ = true;
  stopped_ = false;
  return parser_.init(this, startElement, endElement, characters, entity, true, true);
}
bool BookTextSearch::feed(const uint8_t* bytes, size_t length) { return parser_.feed(bytes, length); }
bool BookTextSearch::finishChapter() {
  const bool ok = parser_.finalize();
  if (utf8Remaining_) {
    utf8Remaining_ = 0;
    acceptCodepoint(0xfffd, utf8Source_);
  }
  while (pendingCount_ && !stopped_) emitPending();
  return ok;
}

void BookTextSearch::acceptByte(uint8_t byte, uint32_t source) {
  if (utf8Remaining_) {
    if ((byte & 0xc0) == 0x80) {
      utf8Value_ = (utf8Value_ << 6) | (byte & 63);
      if (--utf8Remaining_ == 0) {
        const bool valid =
            utf8Value_ >= utf8Minimum_ && utf8Value_ <= 0x10ffff && !(utf8Value_ >= 0xd800 && utf8Value_ <= 0xdfff);
        acceptCodepoint(valid ? utf8Value_ : 0xfffd, utf8Source_);
      }
      return;
    }
    utf8Remaining_ = 0;
    acceptCodepoint(0xfffd, utf8Source_);
  }
  if (byte < 0x80) {
    acceptCodepoint(byte, source);
  } else if (byte >= 0xc2 && byte <= 0xf4) {
    utf8Source_ = source;
    utf8Remaining_ = byte < 0xe0 ? 1 : byte < 0xf0 ? 2 : 3;
    utf8Minimum_ = byte < 0xe0 ? 0x80 : byte < 0xf0 ? 0x800 : 0x10000;
    utf8Value_ = byte & (byte < 0xe0 ? 31 : byte < 0xf0 ? 15 : 7);
  } else {
    acceptCodepoint(0xfffd, source);
  }
}

void BookTextSearch::acceptCodepoint(uint32_t cp, uint32_t source) {
  if (stopped_ || cp == 0 || cp == 0xad || utf8IsDefaultIgnorable(cp)) return;
  const bool space = isSpace(cp);
  if (space && previousSpace_) return;
  if (space) cp = ' ';
  previousSpace_ = space;
  const uint32_t ordinal = tokenCount_;
  if (callback_) {
    ++tokenCount_;
    history_[ordinal % HISTORY_SIZE] = {cp, source};
  }
  // Lower before expanding uppercase so capital sharp S and compatibility capitals
  // such as the Kelvin sign compare like their ordinary lowercase forms too.
  const auto* first = std::begin(utf8_case_detail::lowerMappings);
  const auto* last = std::end(utf8_case_detail::lowerMappings);
  const auto* lower =
      cp >= 0x80
          ? std::lower_bound(first, last, cp, [](const auto& item, uint32_t value) { return item.codepoint < value; })
          : last;
  if (lower != last && lower->codepoint == cp) {
    auto* text = reinterpret_cast<const unsigned char*>(lower->text);
    while (*text && !stopped_) foldCodepoint(utf8NextCodepoint(&text), ordinal);
  } else {
    foldCodepoint(cp, ordinal);
  }
  while (pendingCount_ && !stopped_ && tokenCount_ - pending_[pendingFirst_].end > AFTER) emitPending();
}

void BookTextSearch::foldCodepoint(uint32_t cp, uint32_t ordinal) {
  char scratch[5];
  const char* folded = upper(cp, scratch);
  if (!callback_) {
    for (; *folded && queryLength_ < query_.size(); ++folded) query_[queryLength_++] = *folded;
  } else {
    for (; *folded && !stopped_; ++folded) foldByte(*folded, ordinal);
  }
}

void BookTextSearch::foldByte(char byte, uint32_t ordinal) {
  origins_[foldedCount_++ % FOLDED_BYTES] = ordinal;
  while (matched_ && byte != query_[matched_]) matched_ = prefix_[matched_ - 1];
  if (byte == query_[matched_]) ++matched_;
  if (matched_ != queryLength_) return;
  const uint32_t start = origins_[(foldedCount_ - queryLength_) % FOLDED_BYTES];
  const uint32_t source = history_[start % HISTORY_SIZE].source;
  // A Unicode expansion (e.g. ß -> SS) can match one-letter queries twice at the same
  // source character; one selectable result per source character is sufficient.
  if (source != UINT32_MAX && source != lastMatchSource_) {
    if (pendingCount_ == PENDING_SIZE) emitPending();
    if (!stopped_) {
      pending_[(pendingFirst_ + pendingCount_) % PENDING_SIZE] = {start, ordinal, source};
      ++pendingCount_;
      lastMatchSource_ = source;
    }
  }
  matched_ = prefix_[matched_ - 1];
}

void BookTextSearch::emitPending() {
  const auto pending = pending_[pendingFirst_];
  pendingFirst_ = (pendingFirst_ + 1) % PENDING_SIZE;
  --pendingCount_;
  const uint32_t first = pending.start > BEFORE ? pending.start - BEFORE : 0;
  const uint32_t last = std::min(tokenCount_, pending.end + AFTER + 1);
  size_t used = 0;
  if (first) {
    std::memcpy(snippet_.data(), "...", 3);
    used = 3;
  }
  bool truncated = false;
  for (uint32_t ordinal = first; ordinal < last; ++ordinal) {
    char encoded[4];
    const size_t bytes = encode(history_[ordinal % HISTORY_SIZE].codepoint, encoded);
    if (used + bytes > SNIPPET_BYTES - 3) {
      truncated = true;
      break;
    }
    std::memcpy(snippet_.data() + used, encoded, bytes);
    used += bytes;
  }
  while (used && snippet_[used - 1] == ' ') --used;
  if (truncated || last < tokenCount_) {
    std::memcpy(snippet_.data() + used, "...", 3);
    used += 3;
  }
  snippet_[used] = '\0';
  if (!callback_(context_, pending.source, snippet_.data())) {
    stopped_ = true;
    parser_.stop();
  }
}

void BookTextSearch::boundary() {
  if (bodyDepth_ >= 0 && skipDepth_ < 0) acceptCodepoint(' ', parser_.sourceByteOffset());
}
void BookTextSearch::startElement(void* context, const char* name, const char** atts) {
  auto& self = *static_cast<BookTextSearch*>(context);
  const auto tag = localName(name);
  if (tag == "body" && self.bodyDepth_ < 0) self.bodyDepth_ = self.depth_;
  if (self.skipDepth_ < 0 && (tag == "head" || tag == "script" || tag == "style" || tag == "svg" || hidden(atts)))
    self.skipDepth_ = self.depth_;
  if (block(tag)) self.boundary();
  ++self.depth_;
}
void BookTextSearch::endElement(void* context, const char* name) {
  auto& self = *static_cast<BookTextSearch*>(context);
  --self.depth_;
  if (block(localName(name))) self.boundary();
  if (self.skipDepth_ == self.depth_) self.skipDepth_ = -1;
  if (self.bodyDepth_ == self.depth_) self.bodyDepth_ = -1;
}
void BookTextSearch::characters(void* context, const char* text, int length) {
  auto& self = *static_cast<BookTextSearch*>(context);
  if (self.bodyDepth_ < 0 || self.skipDepth_ >= 0) return;
  for (int i = 0; i < length && !self.stopped_; ++i)
    self.acceptByte(static_cast<uint8_t>(text[i]), self.parser_.characterSourceOffset(i));
}
void BookTextSearch::entity(void* context, const char* text, int length) {
  auto& self = *static_cast<BookTextSearch*>(context);
  if (self.bodyDepth_ < 0 || self.skipDepth_ >= 0) return;
  const char* decoded = lookupHtmlEntity(text, static_cast<size_t>(length));
  const uint32_t source = self.parser_.characterSourceOffset(0);
  if (decoded) {
    for (; *decoded && !self.stopped_; ++decoded) self.acceptByte(static_cast<uint8_t>(*decoded), source);
  } else {
    for (int i = 0; i < length && !self.stopped_; ++i)
      self.acceptByte(static_cast<uint8_t>(text[i]), source == UINT32_MAX ? source : source + i);
  }
}
