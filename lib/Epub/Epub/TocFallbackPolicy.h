#pragma once

// Crosspoint PR #2603 (Ryan Jarvis), carried through the user's Witch branch.
// Compare mapped links so a long obsolete NCX cannot displace a working nav.
namespace toc_fallback {
constexpr bool navIsSparse(bool parsed, int mapped, int spineCount) { return parsed && mapped < (spineCount + 1) / 2; }
constexpr bool shouldAdoptNcx(bool navParsed, int navMapped, bool ncxParsed, int ncxMapped) {
  return ncxParsed && (!navParsed || ncxMapped > navMapped);
}
}  // namespace toc_fallback
