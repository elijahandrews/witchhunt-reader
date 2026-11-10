#pragma once
#include <Arduino.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>

namespace ImageStorageTest {
inline std::set<std::string> readers;
inline size_t duplicateReads = 0;
inline size_t readLimit = SIZE_MAX, writeLimit = SIZE_MAX;
inline bool failSeek = false, failClose = false;
inline bool failSeekOnce = false;
inline void reset() {
  duplicateReads = 0;
  readLimit = writeLimit = SIZE_MAX;
  failSeek = failClose = false;
  failSeekOnce = false;
}
}  // namespace ImageStorageTest

class HalFile {
 public:
  HalFile() = default;
  ~HalFile() { close(); }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  bool open(const char* path, bool write) {
    close();
    if (!write && ImageStorageTest::readers.contains(path)) {
      ++ImageStorageTest::duplicateReads;
      return false;
    }
    file_ = std::fopen(path, write ? "w+b" : "rb");
    if (!file_) return false;
    path_ = path;
    writable_ = write;
    if (!write) ImageStorageTest::readers.insert(path_);
    return true;
  }
  bool openForRead(const std::string& path) { return open(path.c_str(), false); }
  bool openForWrite(const std::string& path) { return open(path.c_str(), true); }
  bool close() {
    if (!file_) return true;
    const bool ok = std::fclose(file_) == 0;
    file_ = nullptr;
    if (!writable_) ImageStorageTest::readers.erase(path_);
    const bool fail = ImageStorageTest::failClose;
    ImageStorageTest::failClose = false;
    return ok && !fail;
  }
  int read(void* bytes, size_t count) {
    if (!file_ || ImageStorageTest::readLimit == 0) return -1;
    const auto got = std::fread(bytes, 1, std::min(count, ImageStorageTest::readLimit), file_);
    ImageStorageTest::readLimit -= got;
    return static_cast<int>(got);
  }
  size_t write(const void* bytes, size_t count) {
    if (!file_ || !writable_) return 0;
    const size_t written = std::fwrite(bytes, 1, std::min(count, ImageStorageTest::writeLimit), file_);
    ImageStorageTest::writeLimit -= written;
    return written;
  }
  size_t write(uint8_t byte) { return write(&byte, 1); }
  bool seek(size_t offset) {
    if (ImageStorageTest::failSeekOnce) { ImageStorageTest::failSeekOnce = false; return false; }
    return file_ && !ImageStorageTest::failSeek && std::fseek(file_, offset, SEEK_SET) == 0;
  }
  bool seekCur(int64_t offset) {
    return file_ && !ImageStorageTest::failSeek && std::fseek(file_, offset, SEEK_CUR) == 0;
  }
  size_t position() const { return file_ ? static_cast<size_t>(std::ftell(file_)) : 0; }
  size_t size() const {
    if (!file_) return 0;
    const long old = std::ftell(file_);
    std::fseek(file_, 0, SEEK_END);
    const long end = std::ftell(file_);
    std::fseek(file_, old, SEEK_SET);
    return end > 0 ? static_cast<size_t>(end) : 0;
  }
  int available() const { return position() < size() ? static_cast<int>(size() - position()) : 0; }
  bool isOpen() const { return file_ != nullptr; }
  explicit operator bool() const { return isOpen(); }

 private:
  std::FILE* file_ = nullptr;
  std::string path_;
  bool writable_ = false;
};
using FsFile = HalFile;
class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }
  bool openFileForRead(const char*, const std::string& path, HalFile& file) { return file.open(path.c_str(), false); }
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) { return file.open(path.c_str(), true); }
  bool exists(const char* path) const {
    auto* file = std::fopen(path, "rb");
    if (!file) return false;
    std::fclose(file);
    return true;
  }
  bool remove(const char* path) { return std::remove(path) == 0; }
};
#define Storage HalStorage::getInstance()
