#include "ImageFormatDetector.h"

#include <algorithm>
#include <cstring>

namespace {
using Format = ImageFormatDetector::Format;
Format raster(const uint8_t* bytes, size_t length) {
  static constexpr uint8_t png[] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
  if (length >= 8 && std::memcmp(bytes, png, 8) == 0) return Format::Png;
  if (length >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff) return Format::Jpeg;
  if (length >= 6 && (std::memcmp(bytes, "GIF87a", 6) == 0 || std::memcmp(bytes, "GIF89a", 6) == 0)) return Format::Gif;
  return Format::Unknown;
}
bool whitespace(uint8_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Recognize only a root SVG tag, skipping a BOM, XML processing instructions
// and comments. No substrings inside comments, DTDs or arbitrary text qualify.
// Full XML validity remains the bounded SVG rasterizer's responsibility.
struct SvgPrefix {
  enum State : uint8_t { Start, Bom2, Bom3, Space, Less, Bang, Dash, Comment, Instruction, Name, Done, Failed };
  State state = Start;
  uint8_t previous = 0;
  uint8_t dashes = 0;
  char name[64]{};
  size_t nameSize = 0;
  bool byte(uint8_t c) {
    switch (state) {
      case Start:
        state = Space;
        if (c == 0xef) {
          state = Bom2;
          return true;
        }
        [[fallthrough]];
      case Space:
        if (whitespace(c)) return true;
        state = c == '<' ? Less : Failed;
        break;
      case Bom2:
        state = c == 0xbb ? Bom3 : Failed;
        break;
      case Bom3:
        state = c == 0xbf ? Space : Failed;
        break;
      case Less:
        if (c == '?') {
          state = Instruction;
          previous = 0;
          break;
        }
        if (c == '!') {
          state = Bang;
          break;
        }
        state = Name;
        [[fallthrough]];
      case Name:
        if (whitespace(c) || c == '/' || c == '>') {
          const char* local = std::strrchr(name, ':');
          state = std::strcmp(local ? local + 1 : name, "svg") == 0 ? Done : Failed;
        } else if (nameSize + 1 < sizeof(name) &&
                   ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                    c == ':' || c == '-' || c == '.')) {
          name[nameSize++] = static_cast<char>(c);
        } else
          state = Failed;
        break;
      case Bang:
        state = c == '-' ? Dash : Failed;
        break;
      case Dash:
        state = c == '-' ? Comment : Failed;
        dashes = 0;
        break;
      case Comment:
        if (c == '>' && dashes >= 2) {
          state = Space;
          dashes = 0;
        } else
          dashes = c == '-' ? std::min<uint8_t>(2, dashes + 1) : 0;
        break;
      case Instruction:
        if (c == '>' && previous == '?') state = Space;
        previous = c;
        break;
      case Done:
      case Failed:
        break;
    }
    return state != Failed;
  }
};
}  // namespace

ImageFormatDetector::Format ImageFormatDetector::detect(const uint8_t* data, size_t size) {
  if (!data || !size) return Format::Unknown;
  const auto format = raster(data, size);
  if (format != Format::Unknown) return format;
  SvgPrefix prefix;
  for (size_t i = 0; i < std::min(size, kMaxSvgPrefixBytes); ++i) {
    if (!prefix.byte(data[i])) return Format::Unknown;
    if (prefix.state == SvgPrefix::Done) return Format::Svg;
  }
  return Format::Unknown;
}

ImageFormatDetector::Format ImageFormatDetector::detect(HalFile& file, uint32_t base) {
  if (!file) return Format::Unknown;
  const size_t original = file.position();
  if (!file.seek(base)) return Format::Unknown;
  uint8_t buffer[128];
  const int first = file.read(buffer, 8);
  Format format = first > 0 ? raster(buffer, static_cast<size_t>(first)) : Format::Unknown;
  if (format == Format::Unknown && first > 0) {
    SvgPrefix prefix;
    size_t consumed = 0;
    int count = first;
    bool keep = true;
    while (keep && count > 0) {
      for (int i = 0; i < count; ++i) {
        ++consumed;
        if (!prefix.byte(buffer[i])) {
          keep = false;
          break;
        }
        if (prefix.state == SvgPrefix::Done) {
          format = Format::Svg;
          keep = false;
          break;
        }
      }
      if (!keep || consumed >= kMaxSvgPrefixBytes) break;
      count = file.read(buffer, std::min(sizeof(buffer), kMaxSvgPrefixBytes - consumed));
    }
  }
  // A failed rewind must never be hidden from a caller that will reuse its handle.
  if (!file.seek(original)) return Format::Unknown;
  return format;
}

ImageFormatDetector::Format ImageFormatDetector::detectPath(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("IMG", path, file)) return Format::Unknown;
  const Format format = detect(file);
  return file.close() ? format : Format::Unknown;
}

bool ImageFormatDetector::isBaselineJpeg(const uint8_t* data, size_t size) {
  if (!data || size < 4 || data[0] != 0xff || data[1] != 0xd8) return false;
  size_t at = 2;
  for (unsigned markerCount = 0; markerCount < 1024 && at < size; ++markerCount) {
    if (data[at++] != 0xff) return false;
    unsigned padding = 0;
    while (at < size && data[at] == 0xff && ++padding <= 1024) ++at;
    if (at >= size || padding > 1024) return false;
    const auto marker = data[at++];
    if (marker == 0xc0) return true;
    if (marker == 0xda || marker == 0xd9 || (marker >= 0xc1 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc)) return false;
    if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd8)) continue;
    if (size - at < 2) return false;
    const size_t length = (size_t(data[at]) << 8) | data[at + 1];
    if (length < 2 || length > size - at) return false;
    at += length;
  }
  return false;
}
