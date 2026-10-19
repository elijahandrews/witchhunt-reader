#pragma once
#include <Utf8.h>
#include <Utf8CaseData.h>

#include <algorithm>
#include <cstdint>
#include <iterator>

namespace smallCaps {
inline constexpr float SCALE = 0.75f;
// Synthetic fallback uses only one-to-one Unicode case mappings. Real font
// features are tried first, so their sharp-s and language-specific shapes win.
inline bool fold(uint32_t& cp) {
  if (cp >= 'a' && cp <= 'z') {
    cp -= 'a' - 'A';
    return true;
  }
  const auto* end = std::end(utf8_case_detail::upperMappings);
  const auto* found = std::lower_bound(std::begin(utf8_case_detail::upperMappings), end, cp,
                                       [](const auto& entry, uint32_t value) { return entry.codepoint < value; });
  if (found == end || found->codepoint != cp) return false;
  const auto* text = reinterpret_cast<const uint8_t*>(found->text);
  const auto upper = utf8NextCodepoint(&text);
  if (!upper || *text) return false;
  cp = upper;
  return true;
}
inline bool isUppercase(uint32_t cp) {
  if (cp >= 'A' && cp <= 'Z') return true;
  const auto* end = std::end(utf8_case_detail::lowerMappings);
  const auto* found = std::lower_bound(std::begin(utf8_case_detail::lowerMappings), end, cp,
                                       [](const auto& entry, uint32_t value) { return entry.codepoint < value; });
  return found != end && found->codepoint == cp;
}
}  // namespace smallCaps
