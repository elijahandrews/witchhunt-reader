#pragma once
#include <HalStorage.h>

#include "CssStyle.h"
namespace cssExtrasCodec {
constexpr size_t Bytes = 47;
inline bool write(HalFile& file, const CssStyle& s) {
  bool ok = true;
  const auto put = [&](const void* v, size_t n) { ok = ok && file.write(static_cast<const uint8_t*>(v), n) == n; };
  const auto byte = [&](uint8_t v) { put(&v, 1); };
  const auto length = [&](const CssLength& v) {
    put(&v.value, 4);
    byte(static_cast<uint8_t>(v.unit));
  };
  uint32_t flags = 0;
  if (s.defined.lineHeight) flags |= 1u << 0;
  if (s.defined.colorTransparent) flags |= 1u << 1;
  if (s.defined.opacityZero) flags |= 1u << 2;
  if (s.defined.visibilityHidden) flags |= 1u << 3;
  if (s.defined.minHeight) flags |= 1u << 4;
  if (s.defined.maxHeight) flags |= 1u << 5;
  if (s.defined.maxWidth) flags |= 1u << 6;
  if (s.defined.backgroundGray) flags |= 1u << 7;
  if (s.defined.breakInsideAvoid) flags |= 1u << 8;
  if (s.defined.cssClear) flags |= 1u << 9;
  if (s.defined.hyphens) flags |= 1u << 10;
  if (s.defined.ligatures) flags |= 1u << 11;
  if (s.defined.hyphenateLimitBefore) flags |= 1u << 12;
  if (s.defined.hyphenateLimitAfter) flags |= 1u << 13;
  if (s.defined.hyphenateLimitLines) flags |= 1u << 14;
  if (s.defined.cssFloat) flags |= 1u << 15;
  if (s.defined.backgroundImage) flags |= 1u << 16;
  if (s.defined.pseudoContent) flags |= 1u << 17;
  if (s.defined.backgroundRepeat) flags |= 1u << 18;
  put(&flags, 4);
  put(&s.lineHeightMultiplier, 4);
  byte(static_cast<uint8_t>(s.whiteSpace));
  byte(static_cast<uint8_t>(s.cssFloat));
  byte(s.colorTransparent);
  byte(s.opacityZero);
  byte(s.visibilityHidden);
  length(s.minHeight);
  length(s.maxHeight);
  length(s.maxWidth);
  put(&s.backgroundGray, 2);
  byte(s.breakInsideAvoid);
  byte(s.breakInsideInherit);
  byte(static_cast<uint8_t>(s.cssClear));
  byte(static_cast<uint8_t>(s.hyphens));
  byte(static_cast<uint8_t>(s.ligatures));
  byte(s.hyphenateLimitBefore);
  byte(s.hyphenateLimitAfter);
  byte(s.hyphenateLimitLines);
  put(&s.backgroundImage, 4);
  put(&s.pseudoContent, 4);
  byte(static_cast<uint8_t>(s.backgroundRepeat));
  return ok;
}
inline bool read(HalFile& file, CssStyle& s) {
  bool ok = true;
  const auto get = [&](void* v, size_t n) {
    if (!ok || file.read(v, n) != static_cast<int>(n)) ok = false;
  };
  const auto byte = [&](uint8_t max) {
    uint8_t v = 0;
    get(&v, 1);
    if (v > max) ok = false;
    return v;
  };
  const auto length = [&](CssLength& v) {
    get(&v.value, 4);
    v.unit = static_cast<CssUnit>(byte(static_cast<uint8_t>(CssUnit::Auto)));
    if (!std::isfinite(v.value) || v.value < 0) ok = false;
  };
  uint32_t flags = 0;
  get(&flags, 4);
  if (flags >> 19) return false;
  s.defined.lineHeight = (flags >> 0) & 1;
  s.defined.colorTransparent = (flags >> 1) & 1;
  s.defined.opacityZero = (flags >> 2) & 1;
  s.defined.visibilityHidden = (flags >> 3) & 1;
  s.defined.minHeight = (flags >> 4) & 1;
  s.defined.maxHeight = (flags >> 5) & 1;
  s.defined.maxWidth = (flags >> 6) & 1;
  s.defined.backgroundGray = (flags >> 7) & 1;
  s.defined.breakInsideAvoid = (flags >> 8) & 1;
  s.defined.cssClear = (flags >> 9) & 1;
  s.defined.hyphens = (flags >> 10) & 1;
  s.defined.ligatures = (flags >> 11) & 1;
  s.defined.hyphenateLimitBefore = (flags >> 12) & 1;
  s.defined.hyphenateLimitAfter = (flags >> 13) & 1;
  s.defined.hyphenateLimitLines = (flags >> 14) & 1;
  s.defined.cssFloat = (flags >> 15) & 1;
  s.defined.backgroundImage = (flags >> 16) & 1;
  s.defined.pseudoContent = (flags >> 17) & 1;
  s.defined.backgroundRepeat = (flags >> 18) & 1;
  get(&s.lineHeightMultiplier, 4);
  if (!std::isfinite(s.lineHeightMultiplier) ||
      (s.lineHeightMultiplier != 0 && (s.lineHeightMultiplier < 0.7f || s.lineHeightMultiplier > 2.0f)))
    return false;
  s.whiteSpace = static_cast<CssWhiteSpace>(byte(5));
  s.cssFloat = static_cast<CssFloat>(byte(2));
  s.colorTransparent = byte(1);
  s.opacityZero = byte(1);
  s.visibilityHidden = byte(1);
  length(s.minHeight);
  length(s.maxHeight);
  length(s.maxWidth);
  get(&s.backgroundGray, 2);
  if (s.backgroundGray > 257) return false;
  s.breakInsideAvoid = byte(1);
  s.breakInsideInherit = byte(1);
  s.cssClear = static_cast<CssClear>(byte(4));
  s.hyphens = static_cast<CssHyphens>(byte(3));
  s.ligatures = static_cast<CssLigatures>(byte(2));
  s.hyphenateLimitBefore = byte(255);
  s.hyphenateLimitAfter = byte(255);
  s.hyphenateLimitLines = byte(255);
  get(&s.backgroundImage, 4);
  get(&s.pseudoContent, 4);
  s.backgroundRepeat = static_cast<CssBackgroundRepeat>(byte(4));
  return ok && s.hyphenateLimitBefore && s.hyphenateLimitAfter;
}
}  // namespace cssExtrasCodec
