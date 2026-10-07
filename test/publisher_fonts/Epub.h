#pragma once
#include <Epub/css/CssParser.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
// Archive-boundary double with real extracted files; the manager's source-size,
// cache naming, checksum, FreeType streaming, and lifetime behavior run unchanged.
class Epub {
 public:
  CssParser css;
  std::string cache;
  std::map<std::string, std::filesystem::path> items;
  std::map<std::string, size_t> reportedSizes;
  std::set<std::string> failingExtractions;
  unsigned extracts = 0;
  const CssParser* getCssParser() const { return &css; }
  const std::string& getCachePath() const { return cache; }
  bool getItemSize(const std::string& path, size_t* bytes) const {
    auto explicitSize = reportedSizes.find(path);
    if (explicitSize != reportedSizes.end()) {
      *bytes = explicitSize->second;
      return true;
    }
    auto item = items.find(path);
    if (item == items.end() || !std::filesystem::exists(item->second)) return false;
    *bytes = std::filesystem::file_size(item->second);
    return true;
  }
  bool getItemCrc32(const std::string& path, uint32_t* crc) const {
    auto item = items.find(path);
    if (!crc || item == items.end()) return false;
    std::ifstream source(item->second, std::ios::binary);
    if (!source) return false;
    uint32_t result = 0xffffffffu;
    char byte;
    while (source.get(byte)) {
      result ^= static_cast<unsigned char>(byte);
      for (unsigned bit = 0; bit < 8; ++bit) result = (result >> 1) ^ ((result & 1u) ? 0xedb88320u : 0u);
    }
    *crc = result ^ 0xffffffffu;
    return source.eof();
  }
  bool extractItemToFile(const std::string& path, const std::string& output) {
    ++extracts;
    if (failingExtractions.count(path)) return false;
    auto item = items.find(path);
    if (item == items.end()) return false;
    std::filesystem::create_directories(std::filesystem::path(output).parent_path());
    std::error_code error;
    std::filesystem::copy_file(item->second, output, std::filesystem::copy_options::overwrite_existing, error);
    return !error;
  }
};
