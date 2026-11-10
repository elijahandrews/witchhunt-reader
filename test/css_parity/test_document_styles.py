"""Exercise real Epub CSS orchestration with synthetic archive/storage faults."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'lib/Epub/Epub.cpp').read_text()
methods = source[source.index('namespace {\nconstexpr uint8_t DocumentCssVersion'):source.index('bool Epub::openProtection')]
prelude = r'''#include <Arduino.h>
#include <Memory.h>
#include <HalStorage.h>
#include <Epub/css/CssParser.h>
#include <Epub/css/CssMedia.h>
#include <Epub/css/HeadMediaInventory.h>
#include <Epub/css/HeadStyleExtractor.h>
#include <algorithm>
#include <cassert>
#include <map>
#include <memory>
#include <string>
#include <vector>
std::vector<std::string> spine;
std::map<std::string,std::string> entries;
bool metadataOkay=true, failHeadWrite=false;
struct BookMetadataCache {
 bool loaded=false;
 struct Entry{std::string href;};
 bool isLoaded()const{return loaded;} bool load(){return loaded=metadataOkay;}
 int getSpineCount()const{return loaded?spine.size():0;}
 Entry getSpineEntry(int i)const{return {spine.at(i)};}
};
auto makeBookMetadataCacheNoThrow(const std::string&,bool cacheSizes){assert(!cacheSizes);return std::make_unique<BookMetadataCache>();}
struct ZipFile {
 struct EntryTarget{uint64_t hash;uint16_t len,index;const char* path;};
 struct EntryIdentity{uint32_t crc32=0,compressedSize=0,uncompressedSize=0;bool found=false;};
 explicit ZipFile(const std::string&,void* =nullptr){}
 static uint64_t fnvHash64(const char*,size_t){return 0;}
 void fillEntryIdentities(const EntryTarget* targets,size_t count,EntryIdentity* out,size_t) {
  for(size_t i=0;i<count;++i){auto it=entries.find(targets[i].path);if(it==entries.end())continue;uint32_t hash=2166136261u;for(unsigned char c:it->second)hash=(hash^c)*16777619u;out[targets[i].index]={hash,uint32_t(it->second.size()),uint32_t(it->second.size()),true};}
 }
 bool readFileToStream(const char* path,Print& sink,size_t chunk,bool allowEarlyStop=false){
  auto it=entries.find(path);if(it==entries.end())return false;
  if(failHeadWrite && Storage.exists("book/.tmp.head.css"))Storage.data("book/.tmp.head.css").failAt=0;
  const auto& text=it->second;
  for(size_t pos=0;pos<text.size();pos+=chunk){size_t n=std::min(chunk,text.size()-pos);if(sink.write(reinterpret_cast<const uint8_t*>(text.data()+pos),n)!=n)return allowEarlyStop;}
  return true;
 }
};
struct Epub {
 enum class CssParseStatus{Failed,Partial,Complete};
 std::vector<std::string> cssFiles={"OPS/print.css","OPS/global.css"};
 std::unique_ptr<BookMetadataCache> bookMetadataCache;
 std::unique_ptr<CssParser> cssParser=std::make_unique<CssParser>("book");
 std::string filepath="synthetic.epub",cachePath="book";
 mutable int zipDirectoryCache=0;
 const std::string& getCachePath()const{return cachePath;}
 int getSpineItemsCount()const{return bookMetadataCache?bookMetadataCache->getSpineCount():0;}
 bool getItemSize(const std::string& path,size_t* size)const{auto it=entries.find(path);if(it==entries.end())return false;*size=it->second.size();return true;}
 bool readItemContentsToStream(const std::string& path,Print& sink,size_t chunk)const{return ZipFile(filepath).readFileToStream(path.c_str(),sink,chunk);}
 bool collectCssMediaUsage(std::vector<uint8_t>&)const;
 bool compileDocumentStyles()const;
 bool documentStyleCachesValid()const;
 bool documentStyleCache(int,std::string&,bool&)const;
 CssParseStatus parseCssFiles(bool force=false)const;
};
'''
main = r'''int main(){
 fakeheap::reset();Storage.reset();spine={"OPS/one.xhtml","OPS/two.xhtml"};
 entries["OPS/print.css"]=".print{display:none}";entries["OPS/global.css"]="p{line-height:1.5} #strong{line-height:3}";
 entries[spine[0]]="<html><head><meta name='synthetic' content='only'><link rel='stylesheet' href='print.css' media='print'><style media='print'>p{display:none}</style><style><![CDATA[p{line-height:1.8;font-family:Body,serif}p::before{content:'one'}@font-face{font-family:Body;src:url(font-one.ttf)}]]></style></head><body/></html>";
 entries[spine[1]]="<html><head><link rel='stylesheet' href='global.css'/><style>p{line-height:2.4;font-family:Body,serif}@font-face{font-family:Body;src:url(font-two.ttf)}</style></head><body/></html>";
 Epub book;
 assert(!book.bookMetadataCache);assert(book.parseCssFiles()==Epub::CssParseStatus::Complete);assert(book.documentStyleCachesValid());
 book.bookMetadataCache=std::make_unique<BookMetadataCache>();book.bookMetadataCache->load();
 std::string path;bool truncated=false;assert(book.documentStyleCache(0,path,truncated)&&path=="book/doc_css/0"&&!truncated);
 CssParser one(path,book.cssParser.get());assert(one.loadFromCache());assert(std::abs(one.resolveStyle("p","").lineHeightMultiplier-1.2f)<0.00001f);assert(one.resolveStyle("p","").display!=CssDisplay::None);
 assert(*one.pseudoContentFor("p","","before")=="one");
 auto first=one.resolveStyle("p","").fontFamily;assert(book.cssParser->fontCatalog().stack(first)->families[0].name=="doc0:body");
 assert(book.documentStyleCache(1,path,truncated));CssParser two(path,book.cssParser.get());assert(two.loadFromCache());assert(std::abs(two.resolveStyle("p","").lineHeightMultiplier-1.6f)<0.00001f);assert(!two.pseudoContentFor("p","","before"));
 auto second=two.resolveStyle("p","").fontFamily;assert(first!=second);assert(book.cssParser->fontCatalog().faces().size()==2);
 CssFontCatalog cold;assert(CssParser::loadFontCatalog("book",cold));assert(cold.stack(first)&&cold.stack(second));assert(cold.faces().size()==2);
 assert(cold.faces()[0].sources[0].path=="OPS/font-one.ttf");assert(cold.faces()[1].sources[0].path=="OPS/font-two.ttf");
 auto saved=entries;entries.clear();assert(book.parseCssFiles()==Epub::CssParseStatus::Complete);entries=saved;
 entries[spine[1]]="<html><head><link";assert(book.parseCssFiles(true)==Epub::CssParseStatus::Partial);
 assert(Storage.exists("book/doc_css/.media_retry"));assert(!book.documentStyleCachesValid());assert(!Storage.exists("book/doc_css/index.bin"));
 assert(book.cssParser->loadFromCache());assert(book.cssParser->resolveStyle("div","print").display==CssDisplay::None);book.cssParser->clear();
 entries=saved;assert(book.parseCssFiles()==Epub::CssParseStatus::Complete);assert(!Storage.exists("book/doc_css/.media_retry"));assert(book.documentStyleCachesValid());
 assert(book.cssParser->loadFromCache());assert(!book.cssParser->resolveStyle("div","print").defined.display);book.cssParser->clear();
 failHeadWrite=true;assert(book.parseCssFiles(true)==Epub::CssParseStatus::Partial);assert(!Storage.exists("book/doc_css/index.bin"));assert(!Storage.exists("book/.tmp.head.css"));failHeadWrite=false;
 assert(book.parseCssFiles()==Epub::CssParseStatus::Complete);
 auto& index=Storage.data("book/doc_css/index.bin").bytes;index.back()^=1;assert(!book.documentStyleCachesValid());assert(book.parseCssFiles()==Epub::CssParseStatus::Complete);
 metadataOkay=false;book.bookMetadataCache.reset();assert(!book.compileDocumentStyles());metadataOkay=true;
 assert(book.parseCssFiles()==Epub::CssParseStatus::Complete);
 Epub bases;bases.cssFiles={"OPS/a/style.css","OPS/b/style.css"};
 entries["OPS/a/style.css"]=entries["OPS/b/style.css"]="@font-face{font-family:Body;src:url(font.ttf)}p{background-image:url(image.png)}";
 assert(bases.parseCssFiles(true)==Epub::CssParseStatus::Complete);
 const auto& faces=bases.cssParser->fontCatalog().faces();assert(faces.size()==4);assert(faces[0].sources[0].path=="OPS/a/font.ttf");assert(faces[1].sources[0].path=="OPS/b/font.ttf");
 entries.erase("OPS/global.css");assert(book.parseCssFiles(true)!=Epub::CssParseStatus::Complete);entries=saved;
 fakeheap::internal.free=90*1024;assert(book.parseCssFiles(true)==Epub::CssParseStatus::Partial);fakeheap::internal.free=1024*1024;
 assert(book.parseCssFiles()==Epub::CssParseStatus::Complete);
}
'''
with tempfile.TemporaryDirectory(prefix='crossink-document-css-') as temp:
    cpp = Path(temp) / 'test.cpp'
    exe = Path(temp) / 'test'
    yxml = Path(temp) / 'yxml.o'
    subprocess.run([os.environ.get('CC', 'cc'), '-fsanitize=address,undefined', '-I'+str(root/'lib/Yxml'), '-c', str(root/'lib/Yxml/yxml.c'), '-o', str(yxml)], check=True)
    cpp.write_text(prelude + methods + main)
    cmd = [os.environ.get('CXX', 'c++'), '-std=c++20', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-DARDUINO_ARCH_ESP32=1', '-DCROSSINK_MEMORY_TEST=1']
    for inc in ['test/memory_policy/stubs', 'lib/Memory', 'lib/MemoryBudget', 'lib', 'lib/Epub', 'lib/SaxParser', 'lib/Yxml']:
        cmd += ['-I' + str(root / inc)]
    cmd += [str(cpp)]
    for file in ['CssParser.cpp', 'CssFontCatalog.cpp', 'HeadMediaInventory.cpp', 'HeadStyleExtractor.cpp']:
        cmd += [str(root / 'lib/Epub/Epub/css' / file)]
    cmd += [str(root / 'lib/SaxParser/SaxParserYxml.cpp'), str(yxml), '-o', str(exe)]
    subprocess.run(cmd, check=True)
    subprocess.run([str(exe)], check=True)
