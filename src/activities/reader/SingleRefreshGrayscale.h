#pragma once

#include <cstddef>
#include <cstdint>

// The experimental reader menu selects the strategy; unsupported pages retain overlay AA.
constexpr bool singleRefreshTextAaEligible(bool enabled, bool compatible, bool aaEnabled, bool hasImages,
                                           bool masksReady, bool preempted) {
  return enabled && compatible && aaEnabled && !hasImages && masksReady && !preempted;
}

// Page raster preparation is independent of the grayscale presentation strategy.
// Background section indexing and foreground capture are not controlled here.
constexpr bool readerCanPrepareNextPage(bool enabled, bool buildThrough, int currentPage, int pageCount) {
  return enabled && !buildThrough && currentPage >= 0 && pageCount > 0 && currentPage < pageCount - 1;
}
constexpr bool readerCanConsumeNextPage(bool enabled, bool forward, bool complete, bool sameSpine, int currentPage,
                                        int pageCount, int cachedPage) {
  return readerCanPrepareNextPage(enabled, false, currentPage, pageCount) && forward && complete && sameSpine &&
         cachedPage == currentPage + 1;
}

// Converts the existing overlay capture to complete, absolute selector planes.
// The logical BW framebuffer remains untouched. The caller must invalidate the
// overlay cache after conversion: these bytes are no longer overlay masks.
inline bool convertPreparedOverlayToAbsolute(const uint8_t* bw, uint8_t* lsb, uint8_t* msb, size_t bytes) {
  if (!bw || !lsb || !msb || !bytes || bw == lsb || bw == msb || lsb == msb) return false;
  for (size_t i = 0; i < bytes; ++i) {
    const uint8_t plane0 = bw[i] | lsb[i];
    const uint8_t plane1 = plane0 ^ msb[i];
    lsb[i] = plane0;
    msb[i] = plane1;
  }
  return true;
}
