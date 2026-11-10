#pragma once
#include <string>
#include <vector>
class Epub {
 public:
  struct Entry {
    std::string href;
  };
  std::string path = "/synthetic.epub";
  std::string cache = "/crossink_cache";
  std::vector<Entry> items;
  const std::string& getPath() const { return path; }
  const std::string& getCachePath() const { return cache; }
  int getSpineItemsCount() const { return items.size(); }
  Entry getSpineItem(int index) const { return items.at(index); }
};
