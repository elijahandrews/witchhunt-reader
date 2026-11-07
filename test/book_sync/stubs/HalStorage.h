#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct CardEntry {
  std::string name, data;
  bool directory = false;
};
class HalFile {
 public:
  std::shared_ptr<CardEntry> entry;
  size_t position = 0, next = 0;
  bool writable = false;
  std::vector<std::shared_ptr<CardEntry>> children;
  operator bool() const { return bool(entry); }
  bool isDirectory() const { return entry && entry->directory; }
  size_t getName(char* out, size_t cap) {
    auto name = entry->name.substr(entry->name.find_last_of('/') + 1);
    const auto n = std::min(name.size(), cap - 1);
    memcpy(out, name.data(), n);
    out[n] = 0;
    return n;
  }
  size_t fileSize() const { return entry->data.size(); }
  uint64_t size64() const { return fileSize(); }
  bool seek(size_t n) {
    if (!entry || n > fileSize()) return false;
    position = n;
    return true;
  }
  int available() const { return entry ? int(fileSize() - position) : 0; }
  int read(void* out, size_t cap) {
    const auto n = std::min(size_t(available()), cap);
    memcpy(out, entry->data.data() + position, n);
    position += n;
    return int(n);
  }
  int read() {
    unsigned char c;
    return read(&c, 1) == 1 ? c : -1;
  }
  size_t write(const void* data, size_t n) {
    if (!entry || !writable) return 0;
    entry->data.resize(std::max(fileSize(), position + n));
    memcpy(entry->data.data() + position, data, n);
    position += n;
    return n;
  }
  bool close() {
    entry.reset();
    return true;
  }
  void flush() {}
  HalFile openNextFile() {
    HalFile result;
    if (next < children.size()) result.entry = children[next++];
    return result;
  }
};
using FsFile = HalFile;
class HalStorage {
 public:
  std::map<std::string, std::shared_ptr<CardEntry>> entries;
  bool failRename = false;
  uint64_t freeBytes = uint64_t(4) * 1024 * 1024 * 1024;
  static HalStorage& getInstance() {
    static HalStorage value;
    return value;
  }
  static std::string key(std::string value) {
    for (auto& c : value)
      if (c >= 'A' && c <= 'Z') c += 32;
    return value;
  }
  void reset() {
    entries.clear();
    failRename = false;
    freeBytes = uint64_t(4) << 30;
    ensureDirectoryExists("/");
  }
  bool ensureDirectoryExists(const char* path) {
    std::string name(path);
    auto k = key(name);
    if (entries.count(k)) return entries[k]->directory;
    if (name != "/") {
      auto parent = name.substr(0, name.find_last_of('/'));
      if (parent.empty()) parent = "/";
      ensureDirectoryExists(parent.c_str());
    }
    entries[k] = std::make_shared<CardEntry>(CardEntry{name, "", true});
    return true;
  }
  void put(std::string path, std::string data) {
    auto parent = path.substr(0, path.find_last_of('/'));
    if (parent.empty()) parent = "/";
    ensureDirectoryExists(parent.c_str());
    entries[key(path)] = std::make_shared<CardEntry>(CardEntry{path, data, false});
  }
  std::string content(const std::string& path) const {
    auto it = entries.find(key(path));
    return it == entries.end() ? "<missing>" : it->second->data;
  }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    file = {};
    const auto it = entries.find(key(path));
    if (it == entries.end()) return false;
    file.entry = it->second;
    if (file.entry->directory) {
      const auto prefix = key(path == "/" ? path : path + "/");
      for (const auto& [name, entry] : entries)
        if (name.compare(0, prefix.size(), prefix) == 0 && name.size() > prefix.size() &&
            name.find('/', prefix.size()) == name.npos)
          file.children.push_back(entry);
    }
    return true;
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    put(path, "");
    file = {};
    file.entry = entries[key(path)];
    file.writable = true;
    return true;
  }
  bool openFileForUpdate(const char* module, const std::string& path, HalFile& file) {
    if (!openFileForRead(module, path, file)) return false;
    file.writable = true;
    return true;
  }
  bool readFileToString(const char*, const std::string& path, size_t cap, std::string& out) {
    auto it = entries.find(key(path));
    if (it == entries.end() || it->second->data.empty() || it->second->data.size() > cap) return false;
    out = it->second->data;
    return true;
  }
  bool exists(const char* path) const { return entries.count(key(path)); }
  bool remove(const char* path) { return entries.erase(key(path)); }
  bool rename(const char* from, const char* to) {
    if (failRename || !exists(from) || exists(to)) return false;
    auto value = entries.at(key(from));
    entries.erase(key(from));
    value->name = to;
    entries[key(to)] = value;
    return true;
  }
  uint64_t sdFreeBytes() const { return freeBytes; }
};
#define Storage HalStorage::getInstance()
