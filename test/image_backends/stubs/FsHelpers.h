#pragma once
#include <string>
namespace FsHelpers {
inline bool extension(const std::string& path, const char* suffix) {
  const auto n = std::char_traits<char>::length(suffix);
  if (path.size() < n) return false;
  for (size_t i = 0; i < n; ++i) {
    char c = path[path.size() - n + i];
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (c != suffix[i]) return false;
  }
  return true;
}
inline bool hasJpgExtension(const std::string& p) { return extension(p, ".jpg") || extension(p, ".jpeg"); }
inline bool hasPngExtension(const std::string& p) { return extension(p, ".png"); }
inline bool hasGifExtension(const std::string& p) { return extension(p, ".gif"); }
}  // namespace FsHelpers
