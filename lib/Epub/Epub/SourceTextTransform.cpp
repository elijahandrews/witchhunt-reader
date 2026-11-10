#include "SourceTextTransform.h"

#include <Utf8.h>
#include <Utf8CaseData.h>

#include <algorithm>
#include <cstring>
#include <iterator>

namespace {
uint32_t nextBounded(const unsigned char*& cursor, const unsigned char* end) {
  if (cursor == end || *cursor == 0) return 0;
  if (end - cursor >= 4) return utf8NextCodepoint(&cursor);
  // The UTF-8 decoder accepts zero-terminated strings; string_view need not
  // have a terminator. Pad its final codepoint without reading past the view.
  unsigned char tail[5]{};
  std::memcpy(tail, cursor, end - cursor);
  const auto* decoded = tail;
  const auto cp = utf8NextCodepoint(&decoded);
  cursor += decoded - tail;
  return cp;
}
std::string_view mapping(uint32_t cp, CssTextTransform transform, const char* begin, size_t length) {
  static constexpr char upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  static constexpr char lower[] = "abcdefghijklmnopqrstuvwxyz";
  if (transform == CssTextTransform::Uppercase && cp >= 'a' && cp <= 'z') return {upper + (cp - 'a'), 1};
  if (transform == CssTextTransform::Lowercase && cp >= 'A' && cp <= 'Z') return {lower + (cp - 'A'), 1};
  if (transform == CssTextTransform::Uppercase || transform == CssTextTransform::Lowercase) {
    const auto* first = transform == CssTextTransform::Uppercase ? std::begin(utf8_case_detail::upperMappings)
                                                                 : std::begin(utf8_case_detail::lowerMappings);
    const auto* last = transform == CssTextTransform::Uppercase ? std::end(utf8_case_detail::upperMappings)
                                                                : std::end(utf8_case_detail::lowerMappings);
    const auto* found =
        std::lower_bound(first, last, cp, [](const auto& item, uint32_t value) { return item.codepoint < value; });
    if (found != last && found->codepoint == cp) return found->text;
  }
  return {begin, length};
}
size_t characters(std::string_view text) {
  size_t count = 0;
  for (const auto byte : text)
    if ((static_cast<uint8_t>(byte) & 0xc0) != 0x80) ++count;
  return count;
}
}  // namespace

SourceMappedText transformSourceText(std::string_view source, CssTextTransform transform) {
  SourceMappedText result;
  if (source.empty()) return result;
  result.text.reserve(source.size());
  const auto* cursor = reinterpret_cast<const unsigned char*>(source.data());
  const auto* end = cursor + source.size();
  uint16_t ordinal = 0;
  while (cursor < end) {
    const auto* begin = cursor;
    const uint32_t cp = nextBounded(cursor, end);
    if (!cp) break;
    const auto mapped = mapping(cp, transform, reinterpret_cast<const char*>(begin), cursor - begin);
    const size_t count = characters(mapped);
    if (count != 1 && result.origins.empty()) {
      result.origins.reserve(source.size() + 1);
      for (uint16_t i = 0; i < ordinal; ++i) result.origins.push_back(i);
    }
    if (!result.origins.empty() || count != 1)
      for (size_t i = 0; i < count; ++i) result.origins.push_back(ordinal);
    result.text.append(mapped);
    ++ordinal;
  }
  if (!result.origins.empty()) result.origins.push_back(ordinal);
  const std::string composed = utf8ComposeNfc(result.text);
  if (composed == result.text) return result;

  // Composition can replace the last glyph of an earlier source prefix. Keep
  // its original ordinal instead of charging it to the combining mark.
  // This slow path is bounded by the parser's word buffer and only runs for NFD.
  result.origins.clear();
  std::string prefix;
  std::string previous;
  cursor = reinterpret_cast<const unsigned char*>(source.data());
  ordinal = 0;
  while (cursor < end) {
    const auto* begin = cursor;
    const uint32_t cp = nextBounded(cursor, end);
    if (!cp) break;
    prefix.append(mapping(cp, transform, reinterpret_cast<const char*>(begin), cursor - begin));
    const auto next = utf8ComposeNfc(prefix);
    const auto* oldPtr = reinterpret_cast<const unsigned char*>(previous.c_str());
    const auto* newPtr = reinterpret_cast<const unsigned char*>(next.c_str());
    size_t common = 0;
    while (*oldPtr && *newPtr) {
      const auto oldCp = utf8NextCodepoint(&oldPtr);
      const auto newCp = utf8NextCodepoint(&newPtr);
      if (oldCp != newCp) break;
      ++common;
    }
    const uint16_t origin = common < result.origins.size() ? result.origins[common] : ordinal;
    result.origins.resize(common);
    result.origins.resize(characters(next), origin);
    previous = next;
    ++ordinal;
  }
  result.origins.push_back(ordinal);
  result.text = composed;
  return result;
}
