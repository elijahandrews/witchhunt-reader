#pragma once
#include <cstdint>

// Embedded publisher faces occupy a stable positive namespace. Dynamic SD
// face IDs and their 256-ID native-size windows must never reserve it.
namespace fontIdNamespace {
inline constexpr uint32_t PUBLISHER_BASE = 0x60000000u;
inline constexpr uint32_t PUBLISHER_MASK = 0xf0000000u;
inline constexpr bool isPublisher(uint32_t id) { return (id & PUBLISHER_MASK) == PUBLISHER_BASE; }
inline constexpr uint32_t forSdFont(uint32_t id) { return isPublisher(id) ? id ^ 0x10000000u : id; }
}  // namespace fontIdNamespace
