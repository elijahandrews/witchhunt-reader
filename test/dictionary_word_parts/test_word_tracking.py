"""Compile production dictionary measurement/highlight paths with cached tracking."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
activity = (root / "src/activities/reader/DictionaryWordSelectActivity.cpp").read_text()
navigator = (root / "src/util/WordSelectNavigator.cpp").read_text()
def function(source, start, stop):
    return source[source.index(start):source.index(stop, source.index(start))]
measure = function(activity, "const char* withoutSoftHyphens(", "bool isRtlWord(")
draw = function(navigator, "void WordSelectNavigator::drawSingleHighlight(", "void WordSelectNavigator::drawTouchDragCursor(")
get_word = function(navigator, "const WordSelectNavigator::WordInfo* WordSelectNavigator::getWordAt(", "std::string WordSelectNavigator::buildPhrase(")
prelude = r'''
#include <algorithm>
#include <cassert>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>
#include <EpdFontFamily.h>
#define private public
#include "util/WordSelectNavigator.h"
#undef private
namespace BidiUtils { enum class BidiBaseDir { LTR, RTL }; }
struct Call { std::string text; int x, spacing, tracking, typography; EpdFontFamily::Style style; };
class GfxRenderer {
 public:
 mutable std::vector<Call> calls;
 mutable int resolves=0;
 int resolveBookFont64(int id, uint32_t family, uint16_t points) const {
   ++resolves; assert(id==1 && family==99 && points==717); return 2;
 }
 void fillRect(int,int,int,int,bool) const {}
 void drawText(int, int x, int, const char* text, bool, EpdFontFamily::Style style,
               BidiUtils::BidiBaseDir, int8_t spacing=0, int16_t tracking=0, uint8_t typography=0) const {
   calls.push_back({text,x,spacing,tracking,typography,style});
 }
 int getTextAdvanceX(int, const char* text, EpdFontFamily::Style, uint32_t followingCp=0,
                     int8_t spacing=0, int16_t tracking=0, uint8_t typography=0) const {
   calls.push_back({text,0,spacing,tracking,typography,EpdFontFamily::REGULAR});
   const int length=std::strlen(text);
   return length*12 + ((std::max(0,length-1)+(followingCp?1:0))*(spacing*8+tracking)+8)/16;
 }
};
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;
'''
main = r'''
int main() {
 static_assert(sizeof(WordSelectNavigator::WordInfo)==52);
 GfxRenderer renderer; char scratch[64];
 for (int tracking : {-8,0,19,64}) for (int spacing : {-1,0,2}) {
   assert(measureWordAdvanceX(renderer,1,"A-",2,EpdFontFamily::REGULAR,spacing,tracking,3,scratch,sizeof scratch,'B')==24+(2*(spacing*8+tracking)+8)/16);
   const int expected=48+(3*(spacing*8+tracking)+8)/16;
   assert(measureWordAdvanceX(renderer,1,"ABCD",4,EpdFontFamily::REGULAR,spacing,tracking,3,scratch,sizeof scratch)==expected);
   assert(measureWordAdvanceX(renderer,1,"AB\xc2\xad" "CD",6,EpdFontFamily::REGULAR,spacing,tracking,3,scratch,sizeof scratch)==expected);
   for (bool rtl : {false,true}) {
     renderer.calls.clear();
     const int result=measureWordAdvanceX(renderer,1,"ABCD",4,EpdFontFamily::REGULAR,2,30,rtl,spacing,tracking,3,scratch,sizeof scratch);
     assert(result==54+(spacing*8+tracking+8)/16);
     assert(renderer.calls.size()==1 && renderer.calls[0].text==(rtl?"AB":"CD"));
     WordSelectNavigator::WordInfo word;
     word.characterSpacing=spacing; word.letterSpacing=tracking;
     word.fontId=1; word.fontFamily=99; word.points64=717; word.typography=7; word.screenX=17; word.focusBoundary=2; word.focusSuffixX=30; word.isRtl=rtl;
     WordSelectNavigator selection; selection.words.reset(&word,1); selection.textPool="ABCD";
     renderer.calls.clear(); selection.drawSingleHighlight(renderer,20,0,true);
     assert(renderer.calls.size()==2);
     assert(renderer.calls[0].x==17 && renderer.calls[1].x==47);
     for (const auto& call:renderer.calls) assert(call.spacing==spacing && call.tracking==tracking && call.typography==7);
     word.focusBoundary=0; renderer.calls.clear(); selection.drawSingleHighlight(renderer,20,0,true);
     assert(renderer.calls.size()==1 && renderer.calls[0].spacing==spacing && renderer.calls[0].tracking==tracking && renderer.calls[0].typography==7);
   }
 }
}
'''
with tempfile.TemporaryDirectory(prefix="crossink-dictionary-tracking-") as temp:
    source=Path(temp)/"test.cpp"; binary=Path(temp)/"test"
    source.write_text(prelude+measure+get_word+draw+main)
    subprocess.run([os.environ.get("CXX","c++"),"-std=c++20","-DCROSSINK_SCALABLE_FONTS","-Wall","-Wextra","-Werror",
                    "-fsanitize=address,undefined", "-I"+str(root/"src"),"-I"+str(root/"lib/EpdFont"),
                    str(source),"-o",str(binary)], check=True)
    subprocess.run([str(binary)],check=True)
