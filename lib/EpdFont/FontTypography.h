#pragma once
#include <cstdint>

// Sparse per-word additions to the native, fully occupied uint8 Style field.
namespace fontTypography {
inline constexpr uint8_t AllSmallCaps = 1;
inline constexpr uint8_t NoLigatures = 2;
inline constexpr uint8_t ScriptSizeResolved = 4;
constexpr uint8_t Mask = AllSmallCaps | NoLigatures | ScriptSizeResolved;
}  // namespace fontTypography
