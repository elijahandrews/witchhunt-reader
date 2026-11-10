#pragma once
#include <string_view>

namespace spine_href {
inline std::string_view basename(std::string_view path) {
  const auto slash = path.rfind('/');
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// Exact paths always win. Filename-only fallback is useful for converted books,
// but must never choose arbitrarily between same-named chapters in two folders.
template <typename HrefAt>
int resolve(std::string_view target, int count, HrefAt hrefAt) {
  if (target.empty()) return -1;
  int candidate = -1;
  bool ambiguous = false;
  const auto filename = basename(target);
  for (int i = 0; i < count; ++i) {
    const auto href = hrefAt(i);
    if (href == target) return i;
    if (basename(href) == filename) {
      if (candidate >= 0) ambiguous = true;
      candidate = i;
    }
  }
  return ambiguous ? -1 : candidate;
}
}  // namespace spine_href
