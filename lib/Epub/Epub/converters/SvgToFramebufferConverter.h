#pragma once

#include "ImageToFramebufferDecoder.h"

// Bounded solid-filled SVG paths; unsupported content fails before drawing.
class SvgToFramebufferConverter final : public ImageToFramebufferDecoder {
 public:
  static bool getDimensionsFromBuffer(const uint8_t* data, size_t size, ImageDimensions& out);
  bool getDimensions(const std::string& path, ImageDimensions& out) const override;
  bool decodeToFramebuffer(const std::string& path, GfxRenderer& renderer, const RenderConfig& config) override;
  const char* getFormatName() const override { return "SVG"; }
};
