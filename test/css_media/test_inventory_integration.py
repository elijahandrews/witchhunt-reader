"""Exercise the production Epub inventory with load()'s released-metadata state."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'lib/Epub/Epub.cpp').read_text()
method = source[source.index('bool Epub::collectCssMediaUsage('):source.index('Epub::CssParseStatus Epub::parseCssFiles(')]
prelude = r'''
#include <Epub/css/HeadMediaInventory.h>
#include <Epub/css/CssMedia.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
struct Print { virtual ~Print()=default; virtual size_t write(uint8_t)=0; virtual size_t write(const uint8_t*,size_t)=0; };
std::vector<std::string> spine;
std::map<std::string,std::string> entries;
bool metadataOkay=true;
unsigned opens=0, closes=0;
struct BookMetadataCache {
 bool loaded=false;
 struct Entry {std::string href;};
 ~BookMetadataCache() {++closes;}
 bool isLoaded() const {return loaded;}
 bool load() {++opens; return loaded=metadataOkay;}
 int getSpineCount() const {return loaded?spine.size():0;}
 Entry getSpineEntry(int i) {return {spine.at(i)};}
};
auto makeBookMetadataCacheNoThrow(const std::string&, bool cacheSizes) {assert(!cacheSizes); return std::make_unique<BookMetadataCache>();}
struct Epub {
 std::vector<std::string> cssFiles={"OPS/print.css", "OPS/mixed.css", "OPS/unlinked.css"};
 std::unique_ptr<BookMetadataCache> bookMetadataCache;
 std::string filepath="synthetic.epub", cachePath="native-cache";
 int getSpineItemsCount() const {return bookMetadataCache ? bookMetadataCache->getSpineCount() : 0;}
 BookMetadataCache::Entry getSpineItem(int i) const {return bookMetadataCache ? bookMetadataCache->getSpineEntry(i) : BookMetadataCache::Entry{};}
 bool collectCssMediaUsage(std::vector<uint8_t>&) const;
};
struct {unsigned getFreeHeap()const{return 1024*1024;}} ESP;
void delay(unsigned) {}
namespace FsHelpers {std::string normalisePath(std::string s){return s;} std::string decodeUriEscapes(std::string s){return s;}}
struct ZipFile {
 explicit ZipFile(const std::string&) {}
 bool readFileToStream(const char* path, Print& sink, size_t chunk, bool allowEarlyStop) {
  auto it=entries.find(path); if(it==entries.end())return false;
  const auto& text=it->second;
  for(size_t pos=0;pos<text.size();pos+=chunk) {
   const size_t n=std::min(chunk,text.size()-pos);
   if(sink.write(reinterpret_cast<const uint8_t*>(text.data()+pos),n)!=n)return allowEarlyStop;
  }
  return true;
 }
};
'''
main = r'''
int main() {
 Epub book; std::vector<uint8_t> usage;
 spine={"OPS/one.xhtml","OPS/two.xhtml"};
 entries[spine[0]]="<html><head><link rel='stylesheet' href='print.css' media='print'/><link rel='stylesheet' href='mixed.css' media='print'/></head><body/></html>";
 entries[spine[1]]="<html><head><link rel='stylesheet' href='mixed.css' media='screen'/></head><body/></html>";
 assert(!book.bookMetadataCache);
 assert(book.collectCssMediaUsage(usage));
 assert((usage==std::vector<uint8_t>{1,3,0})); assert(opens==1 && closes==1);
 for (const char* href : {"./print.css", "/OPS/print.css", "sub/../print.css", "%70rint.css", "./%70rint.css"}) {
  entries[spine[1]]=std::string("<html><head><link rel='stylesheet' href='")+href+"' media='screen'/></head><body/></html>";
  assert(book.collectCssMediaUsage(usage)); assert(usage[0]==3);
 }
 entries[spine[1]]="<html xml:base='other/'><head/></html>"; assert(!book.collectCssMediaUsage(usage));
 entries[spine[1]]="<html><head><base href='other/'/></head></html>"; assert(!book.collectCssMediaUsage(usage));
 entries[spine[1]]="<html><head/></html>";
 book.bookMetadataCache=std::make_unique<BookMetadataCache>(); book.bookMetadataCache->load();
 unsigned before=opens; assert(book.collectCssMediaUsage(usage)); assert(opens==before);
 book.bookMetadataCache.reset(); metadataOkay=false; assert(!book.collectCssMediaUsage(usage));
 metadataOkay=true; spine.push_back("missing.xhtml"); assert(!book.collectCssMediaUsage(usage));
 spine.pop_back(); entries[spine[1]]="<html><head><link"; assert(!book.collectCssMediaUsage(usage));
 spine.clear(); assert(!book.collectCssMediaUsage(usage));
}
'''
with tempfile.TemporaryDirectory(prefix='crossink-media-') as temp:
    cpp = Path(temp) / 'test.cpp'
    exe = Path(temp) / 'test'
    yxml = Path(temp) / 'yxml.o'
    subprocess.run([os.environ.get('CC', 'cc'), '-fsanitize=address,undefined', '-I'+str(root/'lib/Yxml'), '-c', str(root/'lib/Yxml/yxml.c'), '-o', str(yxml)], check=True)
    cpp.write_text(prelude + method + main)
    subprocess.run([os.environ.get('CXX','c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I'+str(root/'lib/Epub'), '-I'+str(root/'lib/SaxParser'), '-I'+str(root/'lib/Yxml'),
                    str(cpp), str(root/'lib/Epub/Epub/css/HeadMediaInventory.cpp'), str(root/'lib/SaxParser/SaxParserYxml.cpp'), str(yxml), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
