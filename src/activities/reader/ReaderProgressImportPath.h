#pragma once
#include <HalStorage.h>

#include <functional>
#include <string>

// TXT byte offsets and XTC page numbers have a compatible legacy prefix.
// Resolve only a read path; every save still targets the isolated new cache.
inline std::string readerProgressImportPath(const std::string& cachePath, const std::string& bookPath,
                                            const char* type) {
  const std::string current = cachePath + "/progress.bin";
  if (Storage.exists(current.c_str())) return current;
  return cachePath.substr(0, cachePath.find_last_of('/')) + "/" + type + "_" +
         std::to_string(std::hash<std::string>{}(bookPath)) + "/progress.bin";
}
