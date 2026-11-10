#include "ImageDecoderFactory.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <Memory.h>

#include "GifToFramebufferConverter.h"
#include "JpegToFramebufferConverter.h"
#include "PngToFramebufferConverter.h"
#include "SvgToFramebufferConverter.h"

namespace {
using Format = ImageFormatDetector::Format;
Format nameHint(const std::string& path) {
  if (FsHelpers::hasJpgExtension(path)) return Format::Jpeg;
  if (FsHelpers::hasPngExtension(path)) return Format::Png;
  if (FsHelpers::hasGifExtension(path)) return Format::Gif;
  const auto end = path.size();
  if (end >= 4 && path[end - 4] == '.' && (path[end - 3] == 's' || path[end - 3] == 'S') &&
      (path[end - 2] == 'v' || path[end - 2] == 'V') && (path[end - 1] == 'g' || path[end - 1] == 'G'))
    return Format::Svg;
  return Format::Unknown;
}
}  // namespace

std::unique_ptr<JpegToFramebufferConverter> ImageDecoderFactory::jpegDecoder;
std::unique_ptr<PngToFramebufferConverter> ImageDecoderFactory::pngDecoder;
std::unique_ptr<GifToFramebufferConverter> ImageDecoderFactory::gifDecoder;
std::unique_ptr<SvgToFramebufferConverter> ImageDecoderFactory::svgDecoder;

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string& imagePath) {
  // Existing memory-source callers can have no extracted file yet; preserve
  // their name hint until they pass their borrowed bytes to the overload below.
  // An existing file is always dispatched by contents, never its extension.
  if (!Storage.exists(imagePath.c_str())) return getDecoder(nameHint(imagePath));
  return getDecoder(ImageFormatDetector::detectPath(imagePath));
}

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const uint8_t* data, size_t size) {
  return getDecoder(ImageFormatDetector::detect(data, size));
}

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const Format format) {
  // Factory objects are tiny, lazy and reused; expensive decoder workspaces
  // stay scoped to each actual decode in the native implementations.
  ImageToFramebufferDecoder* decoder = nullptr;
  switch (format) {
    case Format::Jpeg:
      if (!jpegDecoder) jpegDecoder = makeUniqueNoThrow<JpegToFramebufferConverter>();
      decoder = jpegDecoder.get();
      break;
    case Format::Png:
      if (!pngDecoder) pngDecoder = makeUniqueNoThrow<PngToFramebufferConverter>();
      decoder = pngDecoder.get();
      break;
    case Format::Gif:
      if (!gifDecoder) gifDecoder = makeUniqueNoThrow<GifToFramebufferConverter>();
      decoder = gifDecoder.get();
      break;
    case Format::Svg:
      if (!svgDecoder) svgDecoder = makeUniqueNoThrow<SvgToFramebufferConverter>();
      decoder = svgDecoder.get();
      break;
    case Format::Unknown:
      return nullptr;
  }
  if (!decoder) LOG_ERR("DEC", "OOM allocating image decoder (%u)", unsigned(format));
  return decoder;
}

bool ImageDecoderFactory::isFormatSupported(const std::string& path) { return nameHint(path) != Format::Unknown; }
bool ImageDecoderFactory::isFormatSupported(const uint8_t* data, size_t size) {
  return ImageFormatDetector::detect(data, size) != Format::Unknown;
}
bool ImageDecoderFactory::supportsMemorySource(const std::string& path) { return supportsMemorySource(nameHint(path)); }
bool ImageDecoderFactory::supportsMemorySource(const Format format) {
  return format == Format::Jpeg || format == Format::Png;
}
