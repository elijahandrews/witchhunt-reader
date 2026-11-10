#pragma once
#include <Epub/css/CssFontCatalog.h>
#include <map>
#include <set>
struct CssParser {
  CssFontCatalog catalog;
  bool ready = true;
  bool fontCatalogReady() const { return ready; }
  const CssFontCatalog& fontCatalog() const { return catalog; }
};
class Epub {
 public:
  CssParser css;
  std::map<std::string,std::vector<uint8_t>> files;
  std::set<std::string> failing, identityFailing;
  unsigned extracts = 0;
  CssParser* getCssParser() { return &css; }
  std::string getCachePath() const { return "/book"; }
  int getFontItemSize(const std::string& path,size_t* bytes) { if(identityFailing.count(path)) return -1; return getItemSize(path,bytes)?1:0; }
  bool getItemSize(const std::string& path,size_t* bytes) { auto p=files.find(path); if(p==files.end()) return false; *bytes=p->second.size(); return true; }
  bool readFontContentsValidatedToStream(const std::string& path,Print& file,size_t) {
    ++extracts;
    if(failing.count(path)) return false;
    const auto& bytes=files.at(path); return file.write(bytes.data(),bytes.size())==bytes.size();
  }
};
