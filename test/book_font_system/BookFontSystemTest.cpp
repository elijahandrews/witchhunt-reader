#include <BookFontSystem.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <cassert>
#include <iostream>
void face(Epub& epub,const std::string& name,std::vector<CssFontCatalog::Source> sources,
          uint16_t weight=400,CssFontCatalog::Style style=CssFontCatalog::Style::Normal) {
  CssFontCatalog::Face f; f.familyName=name; f.family=CssFontCatalog::namedToken(name);
  f.sources=std::move(sources); f.weight=weight; f.style=style; epub.css.catalog.addFace(std::move(f));
}
struct Fixture { Epub epub; GfxRenderer gfx; BookFontSystem fonts;
  Fixture() { Storage.reset(); HalScalableFont::maximumLive=100; HalScalableFont::failedStyle=-1; }
};
int main() {
  {
    Fixture f;
    f.epub.files["good.ttf"]={41,1}; f.epub.files["corrupt.ttf"]={0};
    face(f.epub,"forms",{{"missing.ttf","truetype"},{"corrupt.ttf","truetype"},{"good.ttf","truetype"}});
    const auto token=f.epub.css.catalog.intern("missing, forms, sans-serif");
    const auto fallback=f.epub.css.catalog.intern("absent, sans-serif");
    f.fonts.open(f.epub,f.gfx);
    const int id=f.fonts.resolve64(token,717); assert(id>0);
    const auto& family=f.gfx.getFontMap().at(id);
    for(unsigned i=0;i<4;++i) { assert(family.styles[i]->data->source==41); assert(family.styles[i]->data->style==i); assert(family.styles[i]->data->points64==717); }
    assert(f.fonts.hasLoadedFamily(token)); char familyName[32]; assert(f.fonts.copyFamilyName(token,familyName,sizeof(familyName)));
    assert(std::string(familyName)=="fixture");
    const auto genericFirst=f.epub.css.catalog.intern("sans-serif, forms");
    assert(!f.fonts.hasLoadedFamily(genericFirst)); assert(!f.fonts.copyFamilyName(genericFirst,familyName,sizeof(familyName)));
    assert(f.epub.extracts==2); assert(f.fonts.resolve64(token,717)==id); assert(f.epub.extracts==2);
    assert(genericFont::fromResolverResult(f.fonts.resolve(fallback,12))==genericFont::SansSerif);
    assert(f.fonts.resolve(CssFontCatalog::namedToken("absent"),12)==0);
    assert(f.fonts.resolve(genericFont::Monospace,12)>0);
  }
  assert(HalScalableFont::live==0);
  {
    Fixture f; f.epub.files["one.ttf"]={11}; f.epub.files["two.ttf"]={22};
    face(f.epub,"forms",{{"one.ttf","truetype"}}); face(f.epub,"forms",{{"two.ttf","truetype"}});
    f.fonts.open(f.epub,f.gfx); auto id=f.fonts.resolve(CssFontCatalog::namedToken("forms"),12);
    assert(id>0); assert(f.gfx.getFontMap().at(id).styles[0]->data->source==22);
  }
  {
    Fixture f; f.epub.files["good.ttf"]={41,1}; face(f.epub,"forms",{{"good.ttf","truetype"}});
    f.epub.failing.insert("good.ttf"); f.fonts.open(f.epub,f.gfx);
    const auto token=CssFontCatalog::namedToken("forms");
    assert(f.fonts.resolve(token,12)==0); const auto failed=f.epub.extracts;
    f.epub.failing.clear(); assert(f.fonts.resolve(token,12)==0); assert(f.epub.extracts==failed);
    f.fonts.retryUnavailableFonts(); assert(f.fonts.resolve(token,12)>0); assert(f.epub.extracts==failed+1);
    assert(f.fonts.failureEpoch()>0);
  }
  {
    Fixture f; f.epub.files["good.ttf"]={41,1}; face(f.epub,"forms",{{"good.ttf","truetype"}});
    f.fonts.open(f.epub,f.gfx); HalScalableFont::failedStyle=1;
    assert(f.fonts.resolve(CssFontCatalog::namedToken("forms"),12)==0);
    assert(f.gfx.getFontMap().empty()); assert(f.fonts.loadedFaces()==0);
    HalScalableFont::failedStyle=-1; f.fonts.retryUnavailableFonts();
    assert(f.fonts.resolve(CssFontCatalog::namedToken("forms"),12)>0);
  }
  assert(HalScalableFont::live==0);
  {
    Fixture f; f.epub.files["good.ttf"]={41,1};
    face(f.epub,"forms",{{"good.ttf","truetype"}});
    face(f.epub,"alias",{{"good.ttf","truetype"}});
    f.fonts.open(f.epub,f.gfx);
    const auto family=CssFontCatalog::namedToken("forms");
    const auto id=f.fonts.resolve64(family,768); assert(id>0);
    const auto* borrowed=f.gfx.getFontMap().at(id).styles[0];
    const size_t baseline=HalScalableFont::descriptors; assert(baseline==4);
    HalScalableFont::failedStyle=1;
    for(unsigned n=0;n<200;++n) {
      f.fonts.retryUnavailableFonts();
      assert(f.fonts.resolve64(family,uint16_t(800+n))==0);
      assert(HalScalableFont::descriptors==baseline);
      assert(borrowed->data->points64==768);
    }
    f.fonts.retryUnavailableFonts();
    assert(f.fonts.resolve64(CssFontCatalog::namedToken("alias"),768)==0);
    assert(HalScalableFont::descriptors==baseline);
    assert(borrowed->data->points64==768);
    HalScalableFont::failedStyle=-1; f.fonts.retryUnavailableFonts();
    assert(f.fonts.resolve64(family,1001)>0);
  }
  assert(HalScalableFont::live==0); assert(HalScalableFont::descriptors==0);
  {
    Fixture f;
    for(unsigned i=0;i<40;++i) { const auto name="face"+std::to_string(i),path=name+".ttf";
      f.epub.files[path]={uint8_t(i+1)}; face(f.epub,name,{{path,"truetype"}}); }
    f.fonts.open(f.epub,f.gfx); HalScalableFont::maximumLive=5;
    int previous=0;
    for(unsigned i=0;i<40;++i) { const int id=f.fonts.resolve(CssFontCatalog::namedToken("face"+std::to_string(i)),12);
      assert(id>0); if(previous) assert(f.gfx.getFontMap().count(previous)); previous=id; assert(HalScalableFont::live<=5); }
    assert(f.fonts.requestedFaces()>>32); assert(f.fonts.loadedFaces()>>32);
    assert(f.fonts.resolve(CssFontCatalog::namedToken("face0"),12)>0);
  }
  assert(HalScalableFont::live==0);
  {
    Fixture f; f.epub.files["ready.ttf"]={43}; face(f.epub,"ready",{{"absent.ttf","truetype"},{"ready.ttf","truetype"}});
    const auto family=CssFontCatalog::namedToken("ready");
    f.epub.css.ready=false; f.fonts.open(f.epub,f.gfx);
    const auto failedSignature=f.fonts.signature();
    assert(f.fonts.hasTemporaryFailures()); assert(!f.fonts.resolve64(family,717));
    f.epub.css.ready=true; f.epub.identityFailing.insert("ready.ttf"); f.fonts.retryUnavailableFonts();
    assert(f.fonts.signature()!=failedSignature); assert(!f.fonts.resolve64(family,717));
    assert(f.fonts.hasTemporaryFailures());
    f.epub.identityFailing.clear(); assert(!f.fonts.resolve64(family,717));
    f.fonts.retryUnavailableFonts(); assert(f.fonts.resolve64(family,717)>0);
    assert(!f.fonts.hasTemporaryFailures());
  }
  assert(HalScalableFont::live==0);
  std::cout << "publisher fallback/style/retry/LRU checks passed\n";
}
