#!/usr/bin/env python3
"""Compile real status-bar drawing/layout, toggle and source-anchor methods against host IO."""
import argparse
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[2])
a=p.parse_args(); r=a.repo.resolve(); out=a.out.resolve(); out.mkdir(parents=True,exist_ok=True)
(out/'HalStorage.h').write_text('#pragma once\nclass FsFile;\n')
def between(s,start,end):
 assert s.count(start)==1 and s.count(end)==1
 return start+s.split(start,1)[1].split(end,1)[0]
ui=(r/'src/components/UITheme.cpp').read_text()
base=(r/'src/components/themes/BaseTheme.cpp').read_text()
line=(r/'src/activities/reader/LineReaderActivity.cpp').read_text()
epub=(r/'src/activities/reader/EpubReaderActivity.cpp').read_text()
txt=(r/'src/activities/reader/TxtReaderActivity.cpp').read_text()
md=(r/'src/activities/reader/MdReaderActivity.cpp').read_text()
def function(text,start):
 i=text.index(start); opening=text.index('{',i); depth=1; j=opening+1
 while depth:
  depth += (text[j]=='{')-(text[j]=='}'); j+=1
 return text[i:j]+'\n'
header=r'''#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>
#include "CrossPointSettings.h"
#include "components/themes/TapTargets.h"
#include "TouchTransform.h"
static const char* goBackLabel="Go back";
#define tr(id) goBackLabel
CrossPointSettings CrossPointSettings::instance;
int saves=0;
bool CrossPointSettings::saveToFile() const { ++saves; return true; }
struct ThemeMetrics { int progressBarMarginTop=4, statusBarVerticalMargin=14, batteryHeight=10,
  statusBarHorizontalMargin=8, batteryWidth=20; };
struct Rect { int x,y,width,height; };
enum class SyncIndicator { None, Active, Success, Error };
constexpr int SMALL_FONT_ID=0;
struct GfxRenderer {
 mutable int draws=0;
 int width=480,height=800,orientation=0,top=9,right=3,bottom=3,left=3;
 struct Text { int x,y,width; std::string text; };
 mutable std::vector<Text> texts;
 mutable std::vector<Rect> batteries;
 void getOrientedViewableTRBL(int* t,int* r,int* b,int* l) const { *t=top; *r=right; *b=bottom; *l=left; }
 int getScreenHeight() const { return height; } int getScreenWidth() const { return width; }
 int getHeldOrientation() const { return orientation; }
 int getTextWidth(int,const char* s) const { return std::strlen(s)*6; }
 void fillRect(int,int,int,int,bool=true) const { ++draws; }
 void drawText(int,int x,int y,const char* text) const { ++draws; texts.push_back({x,y,getTextWidth(0,text),text}); }
 void drawRect(int,int,int,int,bool=true) const { ++draws; }
 std::string truncatedText(int,const char* s,int width) const { return std::string(s).substr(0,std::max(0,width/6)); }
};
struct HalClock { static bool isSynced() { return true; } static time_t now() { return 120; } static void formatTime(char* s,size_t n,bool) { std::snprintf(s,n,"12:00"); } };
struct PowerProbe { int getBatteryPercentage() const { return 51; } } powerManager;
struct Tracker { void onPageTurn() {} };
Tracker& globalReadingSessionTracker() { static Tracker t; return t; }
struct UITheme {
 static UITheme& getInstance() { static UITheme t; return t; }
 ThemeMetrics getMetrics() const { return {}; }
 static int getProgressBarHeight(uint8_t,uint8_t);
 static int getStatusBarItemsHeight(); static int getStatusBarTopHeight(bool=false);
 static int getStatusBarBottomHeight(bool=false); static int getStatusBarHeight(bool=false);
};
struct BaseTheme {
 void drawStatusBar(GfxRenderer&,float,int,int,std::string,int,bool,const std::string&,bool,bool,SyncIndicator,Rect* = nullptr) const;
 void drawPopup(GfxRenderer&,const char*) const {}
 void drawBatteryLeft(const GfxRenderer& g,Rect r,bool) const { ++g.draws; r.width=40; g.batteries.push_back(r); }
 static int statusBarBatteryWidth(const GfxRenderer&,ThemeMetrics,bool) { return 40; }
} GUI;
constexpr int SYNC_INDICATOR_SIZE=12;
void drawSyncIndicator(const GfxRenderer& g,int,int,SyncIndicator s) { if(s!=SyncIndicator::None) ++g.draws; }
struct LineReaderActivity {
 int currentPage=0, statusBarSourcePage=-1;
 size_t statusBarSourceOffset=0;
 std::vector<size_t> pageOffsets;
 int changes=0, totalPages=4;
 int lastStatusBarPage=-1,lastStatusBarBattery=50,lastStatusBarClockMinute=1;
 bool shouldSkipPeriodicUpdate() const;
 void goToNextPage(); void goToPreviousPage();
 void requestUpdate() {} void launchFinishedBookFlow() {}
 void beforeConfiguredAction(CrossPointSettings::BUTTON_ACTION action);
 void beforeMdConfiguredAction(CrossPointSettings::BUTTON_ACTION action);
 void onPageChanged() { ++changes; }
 size_t sourceBeforeStatusBarReflow() const;
 void restoreStatusBarSource(size_t);
};
bool locked=false;
bool tryLockAllowed=true;
struct RenderLock {
 struct ExclusiveActivityAccess {};
 enum class Mode { Try };
 bool owned=true;
 explicit RenderLock(ExclusiveActivityAccess) { assert(!locked); locked=true; }
 explicit RenderLock(Mode) : owned(tryLockAllowed && !locked) { if(owned) locked=true; }
 bool ownsLock() const { return owned; }
 ~RenderLock() { if(owned) locked=false; }
};
struct ReaderUtils { static void enforceExitFullRefresh(GfxRenderer&) { assert(locked); } };
struct InputProbe {
 bool touch=true,tapped=false,suppressed=false; int x=0,y=0; mutable int observedOrientation=-1;
 bool hasTouch() const { return touch; }
 bool wasScreenTappedIn(touchtransform::Orientation o,int& px,int& py) const {
  observedOrientation=static_cast<int>(o); px=x; py=y; return tapped && !suppressed;
 }
 void suppressTouchContact() { suppressed=true; }
};
struct EpubProbe {
 using BA=CrossPointSettings::BUTTON_ACTION;
 bool epub=true;
 GfxRenderer renderer;
 InputProbe mappedInput;
 int returns=0;
 bool handleJumpBackTouch();
 void returnFromJump() { ++returns; }
 bool buildingPopupShown_=false;
 void showIndexingPopup() { // INDEXING_POPUP
 }
 bool pendingPreRender=true, usePreRenderedBuffer=true, preRenderedPlanesStaged_=true;
 struct { bool ready=true; } preRenderedPage;
 struct Pending { bool active=false; } pendingGrayscale_{true};
 struct SectionProbe { int currentPage=0; };
 std::unique_ptr<SectionProbe> section=std::make_unique<SectionProbe>();
 bool backgroundBorrowActive_=false;
 mutable bool statusRefreshDeferred_=false;
 int lastStatusBarPage=-1,lastStatusBarBattery=50,lastStatusBarClockMinute=1;
 bool shouldSkipPeriodicUpdate() const;
 int captures=0; bool update=false;
 uint8_t expectedBefore=1;
 void captureSourceBeforeReflow() { assert(locked && SETTINGS.statusBarVisible==expectedBefore); ++captures; }
 void requestUpdate() { update=true; }
 void toggle() { switch(BA::BTN_TOGGLE_STATUS_BAR) {
'''
toggle=between(epub,'    case BA::BTN_TOGGLE_STATUS_BAR:','    case BA::BTN_TOGGLE_BIONIC_READING:')
body=header+toggle+'default: break; } } };\n'
body+=between(ui,'namespace {','}  // namespace')+'}\n'
body+=ui[ui.index('int UITheme::getProgressBarHeight'):]
body+=between(base,'int progressBarPixelHeight','constexpr int SYNC_INDICATOR_SIZE')
body+=between(base,'void BaseTheme::drawStatusBar','void BaseTheme::drawHelpText')
body+=function(epub,'bool EpubReaderActivity::handleJumpBackTouch()').replace('EpubReaderActivity::','EpubProbe::')
# Use the actual configured-tap branch; only zone classification/action IO is stubbed.
gestures=(r/'src/GestureEventManager.cpp').read_text()
branch=function(gestures,'  if (inReader && input.wasScreenTappedIn(touchOrientation, x, y))')
body+='''
namespace TapZones { int zoneFor(int,int,int,int) { return 0; } }
namespace TouchGestures { int tapGestureFor(int) { return 0; } }
using Gesture=int;
struct GestureProbe {
 InputProbe& input; int actions=0;
 bool boundAction(Gesture,int&,bool) { ++actions; return true; }
 bool consume(bool inReader, touchtransform::Orientation touchOrientation) {
  int x=0,y=0,width=480,height=800,action=0;
'''+branch+'return false; } };\n'

body+=between(line,'size_t LineReaderActivity::sourceBeforeStatusBarReflow','void LineReaderActivity::onButtonAction')
body+=function(line,'void LineReaderActivity::goToNextPage()')
body+=function(line,'void LineReaderActivity::goToPreviousPage()')
body+=function(line,'bool LineReaderActivity::shouldSkipPeriodicUpdate() const')
body+=function(epub,'bool EpubReaderActivity::shouldSkipPeriodicUpdate() const').replace('EpubReaderActivity::','EpubProbe::')
for name,reader in [('Txt',txt),('Md',md)]:
 start='void '+name+'ReaderActivity::onButtonAction(const CrossPointSettings::BUTTON_ACTION action) {'
 prefix=reader.split(start,1)[1].split('  using BA',1)[0]
 method='beforeConfiguredAction' if name=='Txt' else 'beforeMdConfiguredAction'
 body+='void LineReaderActivity::'+method+'(CrossPointSettings::BUTTON_ACTION action) {'+prefix+'}\n'
body+=r'''
int main() {
 auto& s=SETTINGS;
 s.statusBarUpperProgressBar=CrossPointSettings::BOOK_PROGRESS;
 s.statusBarLowerProgressBar=CrossPointSettings::CHAPTER_PROGRESS;
 s.statusBarClock=1; s.useClock=1;
 GfxRenderer g;
 for(int side=0;side<2;++side) {
  s.statusBarItemsPosition=side; s.statusBarVisible=1;
  int top=UITheme::getStatusBarTopHeight(),bottom=UITheme::getStatusBarBottomHeight();
  assert(top>0 && bottom>0);
  g.draws=0; GUI.drawStatusBar(g,50,4,8,"Synthetic",0,true,"(iv)",true,false,SyncIndicator::Active);
  assert(g.draws>4);
  s.statusBarVisible=0;
  for(bool forced:{false,true}) assert(UITheme::getStatusBarTopHeight(forced)==0 && UITheme::getStatusBarBottomHeight(forced)==0);
  g.draws=0; GUI.drawStatusBar(g,50,4,8,"Synthetic",0,true,"(iv)",true,false,SyncIndicator::Active);
  assert(g.draws==0);
  s.statusBarVisible=1;
  assert(UITheme::getStatusBarTopHeight()==top && UITheme::getStatusBarBottomHeight()==bottom);
 }
 // The action stays inside the existing footer in every orientation and leaves
 // room for battery, either clock placement, progress, printed pages and title.
 s.statusBarVisible=1; s.statusBarItemsPosition=CrossPointSettings::STATUS_BAR_ITEMS_BOTTOM;
 s.statusBarBattery=1; s.statusBarTitle=CrossPointSettings::BOOK_TITLE;
 s.statusBarChapterPageCount=1; s.statusBarBookProgressPercentage=1;
 for(int orientation=0;orientation<4;++orientation) for(int clockSide=0;clockSide<2;++clockSide) {
  g.orientation=orientation; g.width=orientation%2?800:480; g.height=orientation%2?480:800;
  g.top=orientation==0?9:3; g.right=orientation==1?9:3;
  g.bottom=orientation==2?9:3; g.left=orientation==3?9:3;
  s.statusBarClockPosition=clockSide; g.texts.clear(); g.batteries.clear();
  const int topBefore=UITheme::getStatusBarTopHeight(),bottomBefore=UITheme::getStatusBarBottomHeight();
  Rect button{};
  GUI.drawStatusBar(g,62,123,456,"Synthetic chapter title",0,true,"(iv)",true,false,SyncIndicator::Active,&button);
  assert(button.width>=30 && button.height>0 && button.x>=g.left);
  assert(button.y>=g.height-g.bottom-bottomBefore && button.y+button.height<=g.height-g.bottom);
  assert(UITheme::getStatusBarTopHeight()==topBefore && UITheme::getStatusBarBottomHeight()==bottomBefore);
  bool sawLabel=false;
  for(const auto& text:g.texts) {
   if(text.text=="<" || text.text=="Go back") {
    assert(text.x>=button.x && text.x+text.width<=button.x+button.width);
    sawLabel |= text.text=="Go back";
   } else if(!text.text.empty()) assert(text.x>=button.x+button.width);
  }
  assert(sawLabel && g.batteries.size()==1 && g.batteries[0].x>=button.x+button.width);
  TapTargets::Recorder::Builder target; target.add(button.x,button.y,button.width,button.height,0);
  TapTargets::screenActions().record(target);
  EpubProbe touch; touch.renderer.orientation=orientation;
  touch.mappedInput.tapped=true; touch.mappedInput.x=button.x+button.width/2; touch.mappedInput.y=button.y+button.height/2;
  s.touchReaderControls=1; GestureProbe gestures{touch.mappedInput};
  assert(!gestures.consume(true,static_cast<touchtransform::Orientation>(orientation)) && gestures.actions==0);
  touch.showIndexingPopup();
  assert(touch.buildingPopupShown_ && TapTargets::screenActions().hitTest(touch.mappedInput.x,touch.mappedInput.y)==0);
  assert(!touch.mappedInput.suppressed && touch.handleJumpBackTouch());
  assert(touch.mappedInput.suppressed && touch.returns==1 && touch.mappedInput.observedOrientation==orientation);
  assert(!touch.handleJumpBackTouch());
  touch.mappedInput.suppressed=false; touch.mappedInput.x=button.x+button.width;
  assert(!touch.handleJumpBackTouch() && !touch.mappedInput.suppressed);
  assert(gestures.consume(true,static_cast<touchtransform::Orientation>(orientation)) && gestures.actions==1);
  touch.mappedInput.suppressed=false; touch.mappedInput.x=button.x+1; s.touchReaderControls=0;
  assert(!touch.handleJumpBackTouch() && !touch.mappedInput.suppressed);
  s.touchReaderControls=1; TapTargets::screenActions().invalidate();
  assert(!touch.handleJumpBackTouch());
 }
 g=GfxRenderer{}; Rect button{};
 goBackLabel="An intentionally very long translated navigation action";
 GUI.drawStatusBar(g,62,123,456,"Synthetic",0,true,"(iv)",true,false,SyncIndicator::Active,&button);
 assert(button.width==30); // compact arrow fallback never collides with other items
 goBackLabel="Go back";
 s.statusBarVisible=0;
 GUI.drawStatusBar(g,62,123,456,"Synthetic",0,true,"(iv)",true,false,SyncIndicator::Active,&button);
 assert(button.width==0 && button.height==0);
 s.statusBarVisible=1; s.statusBarItemsPosition=CrossPointSettings::STATUS_BAR_ITEMS_TOP;
 GUI.drawStatusBar(g,62,123,456,"Synthetic",0,true,"(iv)",true,false,SyncIndicator::Active,&button);
 assert(button.width==0 && button.height==0);
 EpubProbe p;
 p.toggle(); assert(s.statusBarVisible==0 && p.captures==1 && p.update && !p.section);
 assert(!p.pendingPreRender && !p.usePreRenderedBuffer && !p.preRenderedPage.ready && !p.preRenderedPlanesStaged_ && !p.pendingGrayscale_.active);
 p.expectedBefore=0; p.toggle(); assert(s.statusBarVisible==1 && p.captures==2 && saves==2);
 p.epub=false; p.toggle(); assert(s.statusBarVisible==1 && saves==2);
 assert(s.statusBarClock==1 && s.statusBarUpperProgressBar==CrossPointSettings::BOOK_PROGRESS);
 LineReaderActivity l; l.pageOffsets={0,100,200,300}; l.currentPage=2;
 auto anchor=l.sourceBeforeStatusBarReflow(); assert(anchor==200);
 for(int i=0;i<4;++i) {
  l.pageOffsets={0,150,300}; l.restoreStatusBarSource(anchor);
  assert(l.currentPage==1 && l.sourceBeforeStatusBarReflow()==200);
  l.pageOffsets={0,100,200,300}; l.restoreStatusBarSource(l.sourceBeforeStatusBarReflow());
  assert(l.currentPage==2 && l.sourceBeforeStatusBarReflow()==200);
 }
 // Navigate away and back to the same numeric page: use the new page's start.
 l.pageOffsets={0,150,300}; l.restoreStatusBarSource(200);
 l.goToNextPage(); l.goToPreviousPage();
 assert(l.currentPage==1 && l.sourceBeforeStatusBarReflow()==150);
 l.restoreStatusBarSource(200);
 l.beforeConfiguredAction(CrossPointSettings::BTN_CYCLE_ORIENTATION);
 assert(l.sourceBeforeStatusBarReflow()==150);
 l.restoreStatusBarSource(200);
 l.beforeMdConfiguredAction(CrossPointSettings::BTN_CYCLE_ORIENTATION);
 assert(l.sourceBeforeStatusBarReflow()==150);
 l.restoreStatusBarSource(200);
 l.beforeConfiguredAction(CrossPointSettings::BTN_TOGGLE_STATUS_BAR);
 assert(l.sourceBeforeStatusBarReflow()==200);
 tryLockAllowed=false; assert(!p.shouldSkipPeriodicUpdate()); tryLockAllowed=true;
 s.statusBarVisible=0; p.backgroundBorrowActive_=true;
 assert(p.shouldSkipPeriodicUpdate() && !p.statusRefreshDeferred_ && l.shouldSkipPeriodicUpdate());
 s.statusBarVisible=1;
 assert(p.shouldSkipPeriodicUpdate() && p.statusRefreshDeferred_ && !l.shouldSkipPeriodicUpdate());
 l.pageOffsets={0,100,200,300}; l.currentPage=3; assert(l.sourceBeforeStatusBarReflow()==300);
 l.pageOffsets.clear(); l.currentPage=0; assert(l.sourceBeforeStatusBarReflow()==0);
 l.restoreStatusBarSource(0); assert(l.currentPage==0);
 std::cout<<"PASS: real footer layout/touch/gesture precedence in four orientations, visibility, source anchors and toggle\n";
}
'''
popup=function(epub,"  if (!buildingPopupShown_) {")
body=body.replace("// INDEXING_POPUP",popup)
src=out/'status.cpp'; src.write_text(body); exe=out/'status-test'
subprocess.run(['c++','-std=c++20','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
 '-I'+str(out),'-I'+str(r/'src'),'-I'+str(r/'lib/GfxRenderer'),str(src),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
