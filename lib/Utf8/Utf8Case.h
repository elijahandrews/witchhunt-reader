#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

#include "Utf8CaseData.h"

// Default full Unicode mappings: expansions such as sharp-s -> SS are retained.
// Work on complete buffered words, after entity decoding, before measuring text.
// No platform locale or byte-wise toupper/tolower (both corrupt non-ASCII UTF-8).
inline std::string utf8CaseMap(const std::string_view text, const bool uppercase) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    const uint8_t lead = static_cast<uint8_t>(text[i]);
    if (lead < 0x80) {
      char c = text[i++];
      if (uppercase && c >= 'a' && c <= 'z') c -= 'a' - 'A';
      if (!uppercase && c >= 'A' && c <= 'Z') c += 'a' - 'A';
      out.push_back(c);
      continue;
    }
    const size_t bytes = lead >= 0xc2 && lead <= 0xdf   ? 2
                         : lead >= 0xe0 && lead <= 0xef ? 3
                         : lead >= 0xf0 && lead <= 0xf4 ? 4
                                                        : 0;
    uint32_t cp = bytes == 2 ? lead & 0x1f : bytes == 3 ? lead & 0xf : lead & 7;
    bool valid = bytes != 0 && i + bytes <= text.size();
    for (size_t j = 1; valid && j < bytes; ++j) {
      const uint8_t c = static_cast<uint8_t>(text[i + j]);
      valid = (c & 0xc0) == 0x80;
      cp = (cp << 6) | (c & 0x3f);
    }
    valid = valid && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff) && (bytes != 2 || cp >= 0x80) &&
            (bytes != 3 || cp >= 0x800) && (bytes != 4 || cp >= 0x10000);
    if (!valid) {
      out.push_back(text[i++]);  // preserve invalid input, never read past the supplied slice
      continue;
    }
    const auto* begin =
        uppercase ? std::begin(utf8_case_detail::upperMappings) : std::begin(utf8_case_detail::lowerMappings);
    const auto* end = uppercase ? std::end(utf8_case_detail::upperMappings) : std::end(utf8_case_detail::lowerMappings);
    const auto* found =
        std::lower_bound(begin, end, cp, [](const auto& entry, uint32_t value) { return entry.codepoint < value; });
    if (found != end && found->codepoint == cp)
      out.append(found->text);
    else
      out.append(text.substr(i, bytes));
    i += bytes;
  }
  return out;
}
