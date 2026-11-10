#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "css/CssStyle.h"

struct SourceMappedText {
  std::string text;
  // Empty for a one-to-one mapping. Otherwise each rendered codepoint maps to
  // its original source ordinal; the final entry is the original character count.
  std::vector<uint16_t> origins;
};

SourceMappedText transformSourceText(std::string_view source, CssTextTransform transform);
