#pragma once
#include <ImageFormatDetector.h>

#include <cstdint>
#include <memory>
#include <string>

#include "ImageToFramebufferDecoder.h"

class JpegToFramebufferConverter;
class PngToFramebufferConverter;
class GifToFramebufferConverter;
class SvgToFramebufferConverter;

class ImageDecoderFactory {
 public:
  // Returns non-owning pointer - factory owns the decoder lifetime
  static bool supportsMemorySource(const std::string& imagePath);
  static ImageToFramebufferDecoder* getDecoder(const std::string& imagePath);
  // Authoritative content dispatch for extracted files or borrowed PSRAM bytes.
  // The buffer is inspected only; the returned decoder retains no source data.
  static ImageToFramebufferDecoder* getDecoder(ImageFormatDetector::Format format);
  static ImageToFramebufferDecoder* getDecoder(const uint8_t* data, size_t size);
  static bool supportsMemorySource(ImageFormatDetector::Format format);
  static bool isFormatSupported(const uint8_t* data, size_t size);
  // Name-only hint for not-yet-extracted ZIP entries. Callers must use the byte
  // overload (or ImageFormatDetector) to admit mislabeled/extensionless entries.
  static bool isFormatSupported(const std::string& imagePath);

 private:
  static std::unique_ptr<JpegToFramebufferConverter> jpegDecoder;
  static std::unique_ptr<PngToFramebufferConverter> pngDecoder;
  static std::unique_ptr<GifToFramebufferConverter> gifDecoder;
  static std::unique_ptr<SvgToFramebufferConverter> svgDecoder;
};
