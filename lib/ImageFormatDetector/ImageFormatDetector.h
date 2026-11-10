#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>

// Content admission only; each decoder validates its complete format. No file
// is left open by detectPath(), and borrowed handles retain their exact cursor.
class ImageFormatDetector {
 public:
  enum class Format : uint8_t { Unknown, Jpeg, Png, Gif, Svg };
  static constexpr size_t kMaxSvgPrefixBytes = 32768;
  static Format detect(const uint8_t* data, size_t size);
  static Format detect(HalFile& file, uint32_t base = 0);
  static Format detectPath(const std::string& path);
  // A bounded marker walk proves SOF0 before allowing the single-pass memory
  // route. Progressive/unknown JPEG headers need the seekable full decoder.
  static bool isBaselineJpeg(const uint8_t* data, size_t size);
};
