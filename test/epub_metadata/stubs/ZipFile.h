#pragma once
#include <cstdint>
#include <string>
#include <vector>
class ZipFile {
 public:
  struct SizeTarget {
    uint64_t hash;
    uint16_t len;
    uint16_t index;
  };
  inline static std::vector<std::string> paths;
  inline static bool failEnumeration = false;
  explicit ZipFile(const std::string&, void* = nullptr) {}
  template <typename F>
  bool enumerateFilePaths(F callback) {
    for (const auto& path : paths) callback(path);
    return !failEnumeration;
  }
  bool open() { return true; }
  void close() {}
  static uint64_t fnvHash64(const char*, size_t) { return 0; }
  int fillUncompressedSizes(const SizeTarget*, size_t, uint32_t*, size_t) { return 0; }
  bool getInflatedFileSize(const char*, size_t* size) {
    *size = 1;
    return true;
  }
};
