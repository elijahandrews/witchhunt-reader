#include <GfxRenderer.h>

#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

// Gfx/HAL handoff and cancellation; direct_hal_audit.py separately compiles the
// production HAL, full SDK facade and real UC8279 driver against a recording bus.
int runDirectGrayscaleChecks() {
  HalDisplay d;
  GfxRenderer g(d);
  g.begin();
  d.recordGray = true;
  std::array<uint8_t, 48000> lsb{}, msb{}, bw{};
  for (size_t i = 0; i < bw.size(); ++i) {
    lsb[i] = static_cast<uint8_t>(i * 31);
    msb[i] = static_cast<uint8_t>(i * 57);
    bw[i] = static_cast<uint8_t>(i * 17);
  }
  std::memcpy(g.getFrameBuffer(), bw.data(), bw.size());
  size_t checks = 0;
  assert(g.ghostClearingHalfWorthwhile());
  // Unsupported, preflight abort and missing planes never touch the panel or
  // promote a page that was not displayed.
  for (int failure = 0; failure < 4; ++failure) {
    d.directSupported = failure != 0;
    d.grayEvents.clear();
    const auto* before = g.getFrameBuffer();
    const auto t = g.displayPreparedDirectGrayscale(
        failure == 2 ? nullptr : lsb.data(), failure == 3 ? nullptr : msb.data(), [failure] { return failure == 1; });
    assert(t.aborted && d.grayEvents.empty() && g.getFrameBuffer() == before);
    assert(g.ghostClearingHalfWorthwhile());
    assert(std::memcmp(before, bw.data(), bw.size()) == 0);
    ++checks;
  }
  // Two consecutive pages exercise both directions of the framebuffer swap.
  for (int page = 0; page < 2; ++page) {
    d.directSupported = true;
    d.grayEvents.clear();
    const auto* before = g.getFrameBuffer();
    int abortCalls = 0;
    const auto t = g.displayPreparedDirectGrayscale(lsb.data(), msb.data(), [&] { return ++abortCalls > 1; });
    assert(!t.aborted && abortCalls == 1);
    assert(!g.ghostClearingHalfWorthwhile());
    assert(t.planesMs == 1 && t.displayMs == 2 && t.restoreMs == 3);
    assert(d.grayEvents == std::vector<char>({'D', 'L', 'M', 'G', 'S'}));
    assert(d.uploadedLsb == lsb && d.uploadedMsb == msb);
    assert(g.getFrameBuffer() == d.getFrameBuffer() && g.getFrameBuffer() != before);
    assert(std::memcmp(g.getFrameBuffer(), bw.data(), bw.size()) == 0);
    assert(g.getRenderMode() == GfxRenderer::BW);
    // A subsequent partial draw must change the write page, not the active one.
    g.getFrameBuffer()[0] ^= 0x55;
    assert(before[0] == bw[0]);
    bw[0] ^= 0x55;
    ++checks;
  }
  std::cout << "Direct renderer preflight/atomic upload/baseline handoff checks=" << checks << " passed\n";
  return 0;
}
