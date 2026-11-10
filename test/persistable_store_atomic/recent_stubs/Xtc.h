#pragma once
#include <cassert>
#include <string>
class Xtc {
 public:
  Xtc(const std::string&, const char*) {}
  bool load() {
    assert(false && "JSON migration must not parse books");
    return false;
  }
  std::string getTitle() const { return {}; }
  std::string getAuthor() const { return {}; }
  std::string getThumbBmpPath() const { return {}; }
};
