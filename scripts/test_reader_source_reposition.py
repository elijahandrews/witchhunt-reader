#!/usr/bin/env python3
"""Execute production reader reposition methods against interrupted-navigation fixtures."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def method(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'


FIXTURE = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <optional>
#include "Epub/SourceAnchor.h"
#define LOG_DBG(...) ((void)0)
struct Section {
  int currentPage = 0, pageCount = 12;
  bool building = false;
  std::optional<uint16_t> sourceProbe, sourceLookup;
  bool isBuilding() const {return building;}
  std::optional<uint16_t> getSourceTargetPage() const {return sourceProbe;}
  std::optional<uint16_t> getPageForSourceAnchor(SourceAnchor) const {return sourceLookup;}
  std::optional<uint32_t> getSourceTargetVisibleOffset() const {return {};}
  std::optional<uint32_t> getVisibleTextOffsetForPage(uint16_t p) const {return p * 10;}
  std::optional<uint16_t> getPageForVisibleTextOffset(uint32_t v, bool) const {return v / 10;}
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t) const {return {};}
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t) const {return {};}
};
struct EpubReaderActivity {
  Section owned; Section* section = &owned;
  struct { void remember(int, bool) {} } jumpBack;
  uint32_t pendingSourceTarget = UINT32_MAX;
  uint16_t pendingSourceTargetCharacter = 0;
  std::optional<uint16_t> pendingResolvedSourcePage;
  std::optional<uint32_t> cachedVisibleTextOffset, statusBarRelayoutOffset;
  int cachedChapterPageNumber = 0, cachedChapterTotalPageCount = 0;
  int currentSpineIndex = 0, cachedSpineIndex = 0, statusBarRelayoutSpine = 0;
  int statusBarRelayoutPage = 0, cachedTocIndex = 0;
  uint16_t cachedPageParagraphIndex = UINT16_MAX, cachedPageParagraphOffset = 0, cachedPageParagraphSpan = 0;
  bool pendingRelayoutReposition = false, progressSaveRequiredAfterRelayout = false;
  int captureJumpOrigin() {return 0;}
  bool isRelayoutCatchUpComplete() {return true;}
  void rememberJumpOrigin(bool followedLink = false);
  bool applyDeferredReposition();
};
'''
CASES = r'''
int main() {
  // An allocation failure after cache resolution must not leak that page to a new jump.
  EpubReaderActivity interrupted;
  interrupted.pendingSourceTarget = 500;
  interrupted.pendingSourceTargetCharacter = 9;
  interrupted.pendingResolvedSourcePage = 8;
  interrupted.rememberJumpOrigin();
  assert(interrupted.pendingSourceTarget == UINT32_MAX);
  assert(interrupted.pendingSourceTargetCharacter == 0);
  assert(!interrupted.pendingResolvedSourcePage);
  interrupted.cachedVisibleTextOffset = 20;
  assert(interrupted.applyDeferredReposition());
  assert(interrupted.section->currentPage == 2);
  // Defense against a stale optional from a cancellation path: no active source target.
  EpubReaderActivity cancelled;
  cancelled.pendingResolvedSourcePage = 8;
  cancelled.cachedVisibleTextOffset = 30;
  assert(cancelled.applyDeferredReposition());
  assert(cancelled.section->currentPage == 3);
  // Actual source navigation takes precedence and clears pending state after use.
  EpubReaderActivity source;
  source.pendingSourceTarget = 500;
  source.pendingResolvedSourcePage = 8;
  source.cachedVisibleTextOffset = 30;
  source.pendingRelayoutReposition = true;
  assert(source.applyDeferredReposition());
  assert(source.section->currentPage == 8);
  assert(source.progressSaveRequiredAfterRelayout);
  assert(!source.pendingResolvedSourcePage && source.pendingSourceTarget == UINT32_MAX);
  // Do not fall back to a ratio before an incremental probe reaches the target.
  EpubReaderActivity building;
  building.pendingSourceTarget = 500;
  building.section->building = true;
  building.cachedChapterTotalPageCount = 24;
  building.cachedChapterPageNumber = 12;
  assert(!building.applyDeferredReposition());
  assert(building.pendingSourceTarget == 500 && building.section->currentPage == 0);
  building.section->sourceProbe = 6;
  assert(building.applyDeferredReposition());
  assert(building.section->currentPage == 6);
}
'''


def main():
    source = (ROOT / 'src/activities/reader/EpubReaderActivity.cpp').read_text()
    methods = ''.join(method(source, signature) for signature in (
        'void EpubReaderActivity::rememberJumpOrigin(',
        'bool EpubReaderActivity::applyDeferredReposition('))
    with tempfile.TemporaryDirectory(prefix='crossink-source-position-') as directory:
        cpp = Path(directory) / 'test.cpp'
        exe = Path(directory) / 'test'
        cpp.write_text(FIXTURE + methods + CASES)
        subprocess.run(['clang++', '-std=c++17', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', '-I', str(ROOT / 'lib/Epub'),
                        str(cpp), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print('Reader source navigation: interruption, cancellation, exact target and incremental probe passed')


if __name__ == '__main__':
    main()
