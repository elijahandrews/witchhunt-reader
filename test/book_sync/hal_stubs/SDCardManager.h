#pragma once
#include <Print.h>
#include <common/FsApiConstants.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <vector>
#define FS_DATE(y, m, d) ((y) + (m) + (d))
#define FS_TIME(h, m, s) ((h) + (m) + (s))
struct FsDateTime {
  template <typename F>
  static void setCallback(F) {}
};
struct Node {
  std::string path, data;
  bool directory = false;
};
inline std::map<std::string, std::shared_ptr<Node>> card;
inline bool failClose = false;
class FsFile {
 public:
  std::shared_ptr<Node> n;
  size_t pos = 0, next = 0;
  oflag_t accessMode = O_RDONLY;
  operator bool() const { return bool(n); }
  bool isOpen() const { return bool(n); }
  bool close() {
    n.reset();
    return !failClose;
  }
  bool isDirectory() const { return n && n->directory; }
  void flush() {}
  bool sync() { return true; }
  size_t size() const { return n ? n->data.size() : 0; }
  size_t fileSize() const { return size(); }
  size_t getName(char* p, size_t cap) {
    auto s = n->path.substr(n->path.find_last_of('/') + 1);
    size_t len = std::min(s.size(), cap - 1);
    memcpy(p, s.data(), len);
    p[len] = 0;
    return len;
  }
  bool getCreateDateTime(uint16_t*, uint16_t*) { return false; }
  bool getModifyDateTime(uint16_t*, uint16_t*) { return false; }
  bool seekSet(uint64_t p) {
    pos = p;
    return true;
  }
  bool seekCur(int64_t p) {
    pos += p;
    return true;
  }
  size_t position() const { return pos; }
  int available() const { return n && pos < size() ? int(size() - pos) : 0; }
  int read(void* b, size_t count) {
    if (!n || accessMode == O_WRONLY) return -1;
    size_t got = std::min(count, pos < size() ? size() - pos : size_t{0});
    if (got) memcpy(b, n->data.data() + pos, got);
    pos += got;
    return got;
  }
  int read() {
    uint8_t b;
    return read(&b, 1) == 1 ? b : -1;
  }
  size_t write(const void* b, size_t count) {
    if (!n || accessMode == O_RDONLY) return 0;
    n->data.resize(std::max(size(), pos + count));
    memcpy(n->data.data() + pos, b, count);
    pos += count;
    return count;
  }
  size_t write(uint8_t b) { return write(&b, 1); }
  bool rename(const char* path) {
    if (card.count(path)) return false;
    card.erase(n->path);
    n->path = path;
    card[path] = n;
    return true;
  }
  void rewindDirectory() { next = 0; }
  unsigned getError() { return 0; }
  FsFile openNextFile() {
    std::vector<std::shared_ptr<Node>> children;
    std::string prefix = n->path + (n->path == "/" ? "" : "/");
    for (auto& [p, c] : card)
      if (p.starts_with(prefix) && p != n->path && p.find('/', prefix.size()) == std::string::npos)
        children.push_back(c);
    FsFile f;
    if (next < children.size()) f.n = children[next++];
    return f;
  }
};
class SDCardManager {
 public:
  static SDCardManager& getInstance() {
    static SDCardManager s;
    return s;
  }
  bool begin() { return true; }
  bool ready() const { return true; }
  void shutdown() {}
  void* detachFilesystemForRawAccess() { return this; }
  uint64_t sdTotalBytes() const { return 1000000; }
  uint64_t sdUsedBytes() { return 0; }
  uint64_t sdFreeBytes() { return 1000000; }
  bool exists(const char* p) { return card.count(p); }
  FsFile open(const char* path, oflag_t flags = O_RDONLY) {
    FsFile f;
    if (!card.count(path)) {
      if (!(flags & O_CREAT)) return f;
      card[path] = std::make_shared<Node>(Node{path, "", false});
    }
    f.n = card[path];
    f.accessMode = flags & (O_WRONLY | O_RDWR);
    if (flags & O_TRUNC) f.n->data.clear();
    return f;
  }
  bool mkdir(const char* p, bool = true) {
    if (!exists(p)) card[p] = std::make_shared<Node>(Node{p, "", true});
    return true;
  }
  bool ensureDirectoryExists(const char* p) { return mkdir(p); }
  bool remove(const char* p) { return card.erase(p) > 0; }
  bool rmdir(const char* p) { return remove(p); }
  bool rename(const char* a, const char* b) {
    auto f = open(a);
    return f && f.rename(b);
  }
  bool openFileForRead(const char*, const char* p, FsFile& f) {
    f = open(p);
    return bool(f);
  }
  bool openFileForWrite(const char*, const char* p, FsFile& f) {
    f = open(p, O_CREAT | O_TRUNC | O_RDWR);
    return bool(f);
  }
  bool writeFile(const char* p, const String& s) {
    auto f = open(p, O_CREAT | O_TRUNC | O_RDWR);
    return f.write(s.data(), s.size()) == s.size();
  }
  std::vector<String> listFiles(const char*, int) { return {}; }
  String readFile(const char*) { return {}; }
  bool readFileToStream(const char*, Print&, size_t) { return true; }
  size_t readFileToBuffer(const char*, char*, size_t, size_t) { return 0; }
};
