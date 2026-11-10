#pragma once
#include <string>
namespace FsHelpers {
inline bool hasEpubExtension(const std::string& path) { return path.ends_with(".epub"); }
inline bool hasXtcExtension(const std::string& path) { return path.ends_with(".xtc"); }
inline bool hasTxtExtension(const std::string& path) { return path.ends_with(".txt"); }
inline bool hasMarkdownExtension(const std::string& path) { return path.ends_with(".md"); }
}  // namespace FsHelpers
