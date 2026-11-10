#pragma once
#include <cstdint>
namespace genericFont {
constexpr uint32_t Serif = 0xfffffff1u;
constexpr uint32_t SansSerif = 0xfffffff2u;
constexpr uint32_t Monospace = 0xfffffff3u;
constexpr bool isGeneric(uint32_t family) { return family == Serif || family == SansSerif || family == Monospace; }
// Book resolvers return positive registered font IDs, zero for reader fallback,
// or these exact signed sentinels. Never classify arbitrary negative font IDs.
constexpr int resolverResult(uint32_t family) {
  return family == Serif || family == SansSerif ? static_cast<int>(family) : 0;
}
constexpr uint32_t fromResolverResult(int result) {
  return result == static_cast<int>(Serif) ? Serif : result == static_cast<int>(SansSerif) ? SansSerif : 0;
}
}  // namespace genericFont
