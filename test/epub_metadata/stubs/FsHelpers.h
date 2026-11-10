#pragma once
#include <cctype>
#include <filesystem>
#include <string>
namespace FsHelpers {
inline std::string decodeUriEscapes(const std::string& path) {
  std::string result;
  for (size_t i = 0; i < path.size(); ++i) {
    if (path[i] == '%' && i + 2 < path.size() && std::isxdigit(path[i + 1]) && std::isxdigit(path[i + 2])) {
      result.push_back(static_cast<char>(std::stoi(path.substr(i + 1, 2), nullptr, 16)));
      i += 2;
    } else
      result += path[i];
  }
  return result;
}
inline std::string normalisePath(const std::string& path) {
  return std::filesystem::path(path).lexically_normal().string();
}
inline bool checkFileExtension(std::string_view path, std::string_view extension) {
  if (path.size() < extension.size()) return false;
  path.remove_prefix(path.size() - extension.size());
  for (size_t i = 0; i < path.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(path[i])) != extension[i]) return false;
  return true;
}
inline bool hasJpgExtension(std::string_view path) {
  return checkFileExtension(path, ".jpg") || checkFileExtension(path, ".jpeg");
}
inline bool hasPngExtension(std::string_view path) { return checkFileExtension(path, ".png"); }
inline bool hasGifExtension(std::string_view path) { return checkFileExtension(path, ".gif"); }
}  // namespace FsHelpers
