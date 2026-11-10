#pragma once

#include <Print.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

class HalFile : public Print {
  std::string contents_;
  size_t offset_ = 0;
  std::string* sink_ = nullptr;

 public:
  void setWriteSink(std::string* sink) { sink_ = sink; contents_.clear(); offset_ = 0; }
  void setContents(std::string contents) {
    sink_ = nullptr;
    contents_ = std::move(contents);
    offset_ = 0;
  }
  int available() const { return contents_.size() - offset_; }
  size_t position() const { return offset_; }
  size_t size() const { return contents_.size(); }
  int read(void* out, size_t count) {
    count = std::min(count, contents_.size() - offset_);
    std::memcpy(out, contents_.data() + offset_, count);
    offset_ += count;
    return count;
  }
  bool seek(size_t offset) {
    if (offset > contents_.size()) return false;
    offset_ = offset;
    return true;
  }
  bool seekSet(size_t offset) { return seek(offset); }
  bool close() { sync(); sink_ = nullptr; return true; }
  bool sync() { if (sink_) *sink_ = contents_; return true; }
  bool isOpen() const { return false; }
  operator bool() const { return sink_ || !contents_.empty(); }
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* bytes, size_t count) {
    if (offset_ + count > contents_.size()) contents_.resize(offset_ + count);
    std::memcpy(contents_.data() + offset_, bytes, count);
    offset_ += count;
    return count;
  }
};

using FsFile = HalFile;

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }
  std::unordered_map<std::string, std::string> files;
  bool openFileForRead(const char*, const std::string& path, HalFile& file) {
    auto it = files.find(path);
    if (it == files.end()) return false;
    file.setContents(it->second);
    return true;
  }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    file.setWriteSink(&files[path]); return true;
  }
  bool remove(const char* path) { return files.erase(path) != 0; }
  bool exists(const char* path) const { return files.count(path); }
  bool rename(const char* from, const char* to) {
    auto it = files.find(from); if (it == files.end()) return false;
    files[to] = std::move(it->second); files.erase(it); return true;
  }
};

#define Storage HalStorage::getInstance()
