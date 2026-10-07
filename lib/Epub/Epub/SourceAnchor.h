#pragma once

#include <cstdint>

// Stable within the unmodified chapter XHTML. The optional character offset selects a
// fragment of a layout-split word, in the normalized/transformed text, independently of font
// metrics. A changed text-transform may clamp that offset within the same source word.
struct SourceAnchor {
  uint32_t sourceOffset = UINT32_MAX;
  uint16_t characterOffset = 0;
  bool valid() const { return sourceOffset != UINT32_MAX; }
};

// Transient word provenance, bounded by the parser's existing word buffers. Layout splits
// retain the source interval and adjust characterOffset/probeByte to preserve exact targets.
// Only page summaries are persisted; this never adds a per-word field to the page cache.
struct SourceWordSpan {
  uint32_t start = UINT32_MAX;
  uint32_t end = UINT32_MAX;  // exclusive source-byte boundary
  uint16_t characterOffset = 0;
  uint16_t probeByte = UINT16_MAX;  // target byte within the current cooked UTF-8 fragment
  bool valid() const { return start != UINT32_MAX && end != UINT32_MAX && start < end; }
};
