#pragma once
#include <cassert>
#include <string>
class Epub {
 public:
  enum class XLocationLoadMode { Skip };
  Epub(const std::string&, const char*) {}
  bool load(bool, bool, XLocationLoadMode) {
    assert(false && "JSON migration must not parse books");
    return false;
  }
  std::string getTitle() const { return {}; }
  std::string getAuthor() const { return {}; }
  std::string getThumbBmpPath() const { return {}; }
};
