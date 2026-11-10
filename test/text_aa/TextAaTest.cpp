#include <cassert>

#include "activities/reader/DeferredGrayscale.h"
int main() {
  GfxRenderer r;
  r.bw.fill(0xa5);
  const auto bw = r.bw;
  ReaderUtils::DeferredGrayscalePlanes planes;
  int cadence = 5;
  const auto draw = [&] { return r.drawPlane(); };
  assert(ReaderUtils::displayDeferredGrayscale(r, cadence, planes, draw, true));
  assert(cadence == 4 && r.textSubmissions == 1 && r.baseSubmissions == 0 && r.graySubmissions == 0);
  assert(r.bw == bw && r.mode == GfxRenderer::BW && !r.target && allocations == 2);
  assert(ReaderUtils::displayDeferredGrayscale(r, cadence, planes, draw, true));
  assert(allocations == 2 && cadence == 3);
  cadence = 1;
  assert(!ReaderUtils::displayDeferredGrayscale(r, cadence, planes, draw, true));
  assert(cadence == 1 && r.textSubmissions == 2);
  cadence = -1;
  assert(!ReaderUtils::displayDeferredGrayscale(r, cadence, planes, draw, true));
  cadence = 5;
  assert(!ReaderUtils::displayDeferredGrayscale(r, cadence, planes, draw));  // Images never use text waveforms.
  r.accepts = false;
  assert(!ReaderUtils::displayDeferredGrayscale(r, cadence, planes, draw, true));
  assert(cadence == 5 && r.bw == bw);
  const auto rejected = r.textSubmissions;
  assert(!ReaderUtils::displayDeferredGrayscale(r, cadence, planes, [] { return false; }, true));
  assert(r.textSubmissions == rejected && !r.target && r.mode == GfxRenderer::BW && r.bw == bw);
  planes = {};
  allocationFails = true;
  assert(!ReaderUtils::displayDeferredGrayscale(r, cadence, planes, draw, true));
  assert(!planes.lsb && !planes.msb && cadence == 5);
  allocationFails = false;
  failAllocation = allocations + 2;
  assert(!ReaderUtils::displayDeferredGrayscale(r, cadence, planes, draw, true));
  assert(!planes.lsb && !planes.msb && cadence == 5);
  failAllocation = 0;
  psramAvailable = false;
  assert(!ReaderUtils::displayDeferredGrayscale(r, cadence, planes, draw, true));
  psramAvailable = true;
  r.textCapable = false;
  assert(!ReaderUtils::displayDeferredGrayscale(r, cadence, planes, draw, true));
  r.deferredCapable = true;
  r.textCapable = true;
  assert(ReaderUtils::displayDeferredGrayscale(r, cadence, planes, draw, true));
  assert(cadence == 4 && r.baseSubmissions == 1 && r.graySubmissions == 1 && r.cleanup == 1 && r.bw == bw);
}
