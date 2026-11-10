#!/usr/bin/env python3
"""Exercise actual Page box paint/cache methods and AA compositing with synthetic pixels."""
from pathlib import Path
import subprocess
import tempfile
from test_reader_source_reposition import method
ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include <Epub/tables/TableColumnLayout.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include "Epub/PublisherBoxPaint.h"
#include <Print.h>
#define LOG_ERR(...) ((void)0)
struct FsFile:Print {
  std::vector<uint8_t> data; size_t pos=0;
  size_t write(uint8_t b)override{data.push_back(b);return 1;}
  int read(void* out,size_t n){if(n>data.size()-pos)return 0;std::memcpy(out,data.data()+pos,n);pos+=n;return n;}
};
namespace serialization {
template<class T> bool tryWritePod(Print& out,const T& value){return out.write(reinterpret_cast<const uint8_t*>(&value),sizeof(value))==sizeof(value);}
template<class T> bool tryReadPod(FsFile& in,T& value){return in.read(&value,sizeof(value))==sizeof(value);}
}
template<class T,class... Args>auto makeUniqueNoThrow(Args&&...args){return std::make_unique<T>(std::forward<Args>(args)...);}
struct FontCacheManager {bool scan=false;bool isScanning()const{return scan;}};
struct GfxRenderer {
  enum RenderMode {BW,GRAYSCALE_LSB,GRAYSCALE_MSB};
  RenderMode mode=BW; FontCacheManager fonts;
  enum Orientation {Portrait,PortraitInverted,LandscapeClockwise,LandscapeCounterClockwise};
  Orientation orientation=Portrait;bool strip=false;int stripStart=3,stripRows=4;
  bool isStripTargetActive()const{return strip;}
  int getWriteOriginY()const{return stripStart;}int getWriteRows()const{return stripRows;}
  int getDisplayHeight()const{return height;}Orientation getOrientation()const{return orientation;}
  int width=20,height=20;std::vector<uint8_t> pixels=std::vector<uint8_t>(400,1);
  FontCacheManager* getFontCacheManager(){return &fonts;}
  RenderMode getRenderMode()const{return mode;}
  int getScreenWidth()const{return width;}int getScreenHeight()const{return height;}
  void drawPixel(int x,int y,bool black){assert(x>=0&&y>=0&&x<width&&y<height);pixels[y*width+x]=black?0:1;}
  void fillRect(int x,int y,int w,int h,bool black){for(int row=std::max(0,y);row<std::min(height,y+h);++row)for(int col=std::max(0,x);col<std::min(width,x+w);++col)drawPixel(col,row,black);}
};
enum PageElementTag{TAG_PageLine,TAG_PageImage,TAG_PageBorderBox,TAG_PageTableFragment};
struct PageElement {
  int16_t xPos,yPos;PageElement(int16_t x,int16_t y):xPos(x),yPos(y){}virtual ~PageElement()=default;
  virtual void render(GfxRenderer&,int,int,int,bool)=0;
  virtual bool serialize(Print&)=0;
  virtual PageElementTag getTag()const=0;
};
'''
SUFFIX = r'''
struct ImageBlock {int getWidth()const{return 2;}int getHeight()const{return 2;}};
struct PageImage:PageElement {
  ImageBlock image;PageImage(int x,int y):PageElement(x,y){}
  const ImageBlock& getImageBlock()const{return image;}
  ImageBlock& getImageBlock(){return image;}bool isInlineImage()const{return false;}
  void render(GfxRenderer&,int,int,int,bool)override{}bool serialize(Print&)override{return true;}
  PageElementTag getTag()const override{return TAG_PageImage;}
};
struct Text:PageElement {
  Text():PageElement(1,5){}
  void render(GfxRenderer& r,int,int dx,int dy,bool)override{if(!r.fonts.scan)r.fillRect(xPos+dx,yPos+dy,12,2,r.mode==GfxRenderer::BW);}
  bool serialize(Print&)override{return true;}PageElementTag getTag()const override{return TAG_PageLine;}
};
struct PageImagePlacement {ImageBlock* block;int x,y;bool inlineImage=false;bool inTable=false;};
using PageImageVisitor=bool(*)(const PageImagePlacement&,void*);
struct TableCell{uint8_t colSpan=1;std::vector<int> lines{0};std::shared_ptr<ImageBlock> image=std::make_shared<ImageBlock>();};
struct TableRow{int height=8;std::vector<TableCell> cells{TableCell{}};};
struct PageTableFragment:PageElement {
  uint16_t width=12;uint8_t columnCount=2,cellPadding=1;uint16_t lineHeight=2;std::vector<TableRow> rows{TableRow{}};
  PageTableFragment(int x,int y):PageElement(x,y){}
  void render(GfxRenderer&,int,int,int,bool)override{}bool serialize(Print&)override{return true;}
  PageElementTag getTag()const override{return TAG_PageTableFragment;}
};
struct Page {
  std::vector<std::unique_ptr<PageElement>> elements;
  void renderText(GfxRenderer&,int,int,int,bool)const;
  bool forEachImage(PageImageVisitor,void*,bool* = nullptr)const;
};
'''
CASES = r'''
int main(){
  CssBorderSide sides[4]{};
  auto outer=std::make_unique<PageBorderBox>(16,16,sides,false,0,0);outer->configureBackground(0,0);
  auto inner=std::make_unique<PageBorderBox>(8,8,sides,false,4,4);inner->configureBackground(255,1);
  FsFile cache;assert(inner->serialize(cache));auto restored=PageBorderBox::deserialize(cache);assert(restored);
  assert(restored->paintLayer()==1);bool dark=true;assert(restored->paintsPixel(2,2,6,6,dark)&&!dark);
  for(size_t n=0;n<cache.data.size();++n){FsFile cut;cut.data.assign(cache.data.begin(),cache.data.begin()+n);assert(!PageBorderBox::deserialize(cut));}
  FsFile bad;bad.data=cache.data;bad.data.back()=8;assert(!PageBorderBox::deserialize(bad));
  Page page;
  // Parser closes the inner scope first. Depth determines paint order, not close order.
  page.elements.push_back(std::move(inner));page.elements.push_back(std::move(outer));
  page.elements.push_back(std::make_unique<Text>());page.elements.push_back(std::make_unique<PageImage>(6,5));
  page.elements.push_back(std::make_unique<PageTableFragment>(8,2));
  GfxRenderer r;page.renderText(r,0,0,0,true);
  assert(r.pixels[2*20+2]==0);assert(r.pixels[4*20+4]==1);assert(r.pixels[5*20+5]==0);
  for(auto mode:{GfxRenderer::GRAYSCALE_LSB,GfxRenderer::GRAYSCALE_MSB}){
    r.mode=mode;r.pixels.assign(400,0);page.renderText(r,0,0,0,true);
    assert(r.pixels[5*20+2]==0); // black parent stays solid under the AA glyph
    assert(r.pixels[5*20+5]==1); // white child leaves its glyph antialiasing intact
    assert(r.pixels[5*20+6]==0); // opaque image masks underlying text selectors
    assert(r.pixels[5*20+10]==0); // native in-cell image uses the same opaque AA mask
  }
  // A strip only touches its rotated logical region; no page-wide repaint.
  Page stripPage;auto fill=std::make_unique<PageBorderBox>(20,20,sides,false,0,0);fill->configureBackground(0);
  stripPage.elements.push_back(std::move(fill));r.strip=true;
  for(auto orientation:{GfxRenderer::Portrait,GfxRenderer::PortraitInverted,GfxRenderer::LandscapeClockwise,GfxRenderer::LandscapeCounterClockwise}){
    r.orientation=orientation;r.pixels.assign(400,1);stripPage.renderText(r,0,0,0,true);
    for(int y=0;y<20;++y)for(int x=0;x<20;++x){
      const int pos=orientation==GfxRenderer::Portrait?19-x:orientation==GfxRenderer::PortraitInverted?x:orientation==GfxRenderer::LandscapeClockwise?19-y:y;
      assert(r.pixels[y*20+x]==(pos>=3&&pos<7?0:1));
    }
  }
  r.strip=false;
  r.fonts.scan=true;r.pixels.assign(400,1);page.renderText(r,0,0,0,true);
  assert(std::all_of(r.pixels.begin(),r.pixels.end(),[](auto p){return p==1;}));
  r.fonts.scan=false;r.mode=GfxRenderer::BW;
  PageBorderBox clipped(8,8,sides,false,-4,-4);clipped.configureBackground(128);clipped.render(r,0,0,0,true);
  unsigned black=0;for(int y=0;y<4;++y)for(int x=0;x<4;++x)black+=r.pixels[y*20+x]==0;assert(black==8);
  sides[0]={3,CssBorderStyle::Double};PageBorderBox border(8,8,sides,false,0,0);
  assert(border.paintsPixel(3,0,3,0,dark)&&dark);assert(!border.paintsPixel(3,1,3,1,dark));
  assert(border.paintsPixel(3,2,3,2,dark)&&dark);
}
'''
def main():
    header=(ROOT/'lib/Epub/Epub/Page.h').read_text()
    source=(ROOT/'lib/Epub/Epub/Page.cpp').read_text()
    box=method(header,'class PageBorderBox final')+';\n'
    functions=''.join(method(source,sig) for sig in (
      'bool PageBorderBox::paintsPixel(', 'void PageBorderBox::render(',
      'bool PageBorderBox::serialize(', 'std::unique_ptr<PageBorderBox> PageBorderBox::deserialize(',
      'template <typename Predicate>','bool Page::forEachImage(', 'void finalizePublisherBackgrounds(', 'void Page::renderText('))
    with tempfile.TemporaryDirectory(prefix='crossink-background-') as directory:
        cpp,exe=Path(directory)/'test.cpp',Path(directory)/'test'
        cpp.write_text(PREFIX+box+SUFFIX+functions+CASES)
        subprocess.run(['clang++','-std=c++17','-fsanitize=address,undefined','-fno-sanitize-recover=all',
          '-I',str(ROOT/'lib/Epub'),'-I',str(ROOT/'test/section_persistence/stubs'),str(cpp),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
    print('Publisher backgrounds: nested white masks, gray glyphs, image masks, cache truncation, clipping and scan safety passed')
if __name__=='__main__':main()
