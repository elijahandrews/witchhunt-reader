#!/usr/bin/env python3
"""Run the production warming scheduler with bounded, cancellable cache fixtures."""
from pathlib import Path
import subprocess
import tempfile
from test_reader_source_reposition import method

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>
#include <utility>
#include "Epub/StreamCancellation.h"
struct ImageBlock {enum class CacheWarmResult {Ready,Unavailable,Cancelled};};
struct GfxRenderer {};
struct Spec {};
struct Layout {int marginLeft=7,marginTop=11,viewportWidth=480,viewportHeight=800;};
Layout computeReaderViewportLayout(GfxRenderer&,bool){return {};}
int normalizeRenderMode(int m){return m;}
const char* sectionCacheSuffixForRenderMode(int){return "";}
struct {int epubRenderMode=0;int getReaderFontId(){return 3;}} SETTINGS;
namespace MemoryBudget {
inline bool sufficient=true;
int snapshot(){return 0;}
bool hasHeap(int,unsigned,unsigned){return sufficient;}
}
struct Epub {
  StreamCancellation cancel;
  struct ScopedReadCancellation {
    StreamCancellation::Scope scope;
    ScopedReadCancellation(Epub& epub,StreamCancellation::Check check,void* context):scope(epub.cancel,check,context){}
  };
  int getSpineItemsCount() const {return 3;}
};
struct EpubReaderActivity;
inline std::vector<std::pair<int,int>> warmed;
inline int cancelAt=0, failPage=-1;
inline bool recoverCurrent=false;
struct Page {
  int spine,number;
  bool hasImages() const {return true;}
  ImageBlock::CacheWarmResult warmImageCaches(GfxRenderer&,int x,int y,bool(*check)(void*),void* context,bool* changed=nullptr)const;
};
struct Section {
  int spine=0,currentPage=2,pageCount=4;
  bool building=false;
  Section()=default;
  Section(std::unique_ptr<Epub>&,int s,GfxRenderer&,const char*):spine(s),currentPage(0),pageCount(6){}
  bool isBuilding()const{return building;}
  bool loadSectionFile(Spec){return true;}
  std::unique_ptr<Page> loadPage(int p){assert(p>=0&&p<pageCount);return std::make_unique<Page>(Page{spine,p});}
};
template<class T,class... Args> auto makeUniqueNoThrow(Args&&...args){return std::make_unique<T>(std::forward<Args>(args)...);}
struct EpubReaderActivity {
  std::unique_ptr<Epub> epub=std::make_unique<Epub>();
  std::unique_ptr<Section> section=std::make_unique<Section>();
  GfxRenderer renderer;
  bool activeFootnotePreview=false,automaticPageTurnActive=false;
  struct {bool pending=false; bool hasPending()const{return pending;}} pendingManualPageTurns;
  std::atomic<bool> backgroundBuildYieldForInput{false},silentPrefetchBuildActive{false},silentPrefetchCancelRequested{false};
  struct ImageWarmMiss {int spine=-1,page=-1;uint8_t attempts=0;};
  std::array<ImageWarmMiss,8> imageWarmMisses{};
  uint8_t imageWarmMissNext=0;
  int imageWarmSpine=-1,imageWarmPage=-1,currentSpineIndex=0;
  bool backgroundBuildCanUsePageBuffer()const{return true;}
  int buildProfileForRenderMode(int m){return m;}
  Spec readerRenderSpecForProfile(int,int,int,int){return {};}
  int updates=0;
  void requestUpdate(bool immediate){assert(immediate);++updates;}
  void warmUpcomingImageCaches();
};
ImageBlock::CacheWarmResult Page::warmImageCaches(GfxRenderer&,int x,int y,bool(*check)(void*),void* context,bool* changed)const {
  auto& reader=*static_cast<EpubReaderActivity*>(context);
  if(changed)*changed=std::exchange(recoverCurrent,false);
  assert(reader.silentPrefetchBuildActive.load());
  assert(reader.epub->cancel.enabled());
  assert(x==7 && y==11);
  warmed.emplace_back(spine,number);
  if(cancelAt && warmed.size()==static_cast<size_t>(cancelAt))reader.silentPrefetchCancelRequested=true;
  if(check(context))return ImageBlock::CacheWarmResult::Cancelled;
  return number==failPage?ImageBlock::CacheWarmResult::Unavailable:ImageBlock::CacheWarmResult::Ready;
}
struct Output:Print {
  size_t bytes=0;
  size_t write(uint8_t)override{++bytes;return 1;}
  size_t write(const uint8_t*,size_t n)override{bytes+=n;return n;}
};
'''
CASES = r'''
int main(){
  EpubReaderActivity reader;
  reader.warmUpcomingImageCaches();
  const std::vector<std::pair<int,int>> expected={{0,2},{0,3},{1,0},{1,1},{1,2},{1,3}};
  assert(warmed==expected); assert(reader.section->currentPage==2);
  assert(!reader.silentPrefetchBuildActive && !reader.epub->cancel.enabled());
  reader.warmUpcomingImageCaches(); assert(warmed==expected);
  assert(reader.updates==0);
  reader.imageWarmSpine=-1; recoverCurrent=true;
  reader.warmUpcomingImageCaches(); assert(reader.updates==1);
  reader.warmUpcomingImageCaches(); assert(reader.updates==1);
  warmed.clear(); cancelAt=2;
  EpubReaderActivity cancelled;
  cancelled.warmUpcomingImageCaches();
  assert(warmed.size()==2 && cancelled.imageWarmSpine==-1);
  assert(!cancelled.silentPrefetchBuildActive && !cancelled.epub->cancel.enabled());
  cancelAt=0; warmed.clear();
  cancelled.warmUpcomingImageCaches(); assert(warmed==expected);
  warmed.clear(); failPage=2;
  EpubReaderActivity retry;
  for(int i=0;i<3;++i)retry.warmUpcomingImageCaches();
  assert(std::count(warmed.begin(),warmed.end(),std::make_pair(0,2))==2);
  assert(std::count(warmed.begin(),warmed.end(),std::make_pair(1,2))==2);
  warmed.clear(); failPage=-1;
  MemoryBudget::sufficient=false;
  EpubReaderActivity low; low.warmUpcomingImageCaches(); assert(warmed.empty());
  MemoryBudget::sufficient=true;
  low.pendingManualPageTurns.pending=true; low.warmUpcomingImageCaches(); assert(warmed.empty());
  Output out; StreamCancellation control; bool stop=false, innerStop=true;
  auto check=[](void* p){return *static_cast<bool*>(p);};
  const uint8_t bytes[]={1,2,3};
  {
    StreamCancellation::Scope scope(control,check,&stop);
    StreamCancellation::Sink sink(out,control);
    assert(sink.write(bytes,3)==3); stop=true; assert(sink.write(bytes,3)==0);
    stop=false;
    {StreamCancellation::Scope inner(control,check,&innerStop);assert(sink.write(bytes,3)==0);}
    assert(sink.write(bytes,3)==3);
  }
  assert(!control.enabled() && out.bytes==6);
}
'''

def main():
    source = (ROOT / 'src/activities/reader/EpubReaderActivity.cpp').read_text()
    production = method(source, 'void EpubReaderActivity::warmUpcomingImageCaches(')
    with tempfile.TemporaryDirectory(prefix='crossink-warm-scheduler-') as directory:
        cpp, exe = Path(directory) / 'test.cpp', Path(directory) / 'test'
        cpp.write_text(FIXTURE + production + CASES)
        subprocess.run(['clang++', '-std=c++17', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-I', str(ROOT / 'lib/Epub'), '-I', str(ROOT / 'test/section_persistence/stubs'),
                        str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print('Image warming: window, chapter spill, cancellation, retry bounds, heap/input gates and transfer scopes passed')

if __name__ == '__main__':
    main()
