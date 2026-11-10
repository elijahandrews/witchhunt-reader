#include "HeadStyleExtractor.h"

#include <algorithm>
#include <cstring>

#include "CssMedia.h"
namespace {
std::string_view local(const char* name) {
  if (const auto* colon = std::strrchr(name, ':')) return colon + 1;
  return name;
}
}  // namespace
HeadStyleExtractor::HeadStyleExtractor(Print& output) : output_(output) {
  parser_.init(this, start, end, text, nullptr, true);
}
HeadStyleExtractor::~HeadStyleExtractor() = default;
void HeadStyleExtractor::stop(bool failure) {
  failed_ |= failure;
  complete_ = !failure;
  parser_.stop();
}
bool HeadStyleExtractor::emit(const char* bytes, size_t count) {
  if (cssBytes_ + count > MaxBytes || output_.write(reinterpret_cast<const uint8_t*>(bytes), count) != count) {
    stop(true);
    return false;
  }
  cssBytes_ += count;
  return true;
}
bool HeadStyleExtractor::feed(const uint8_t* bytes, size_t count) {
  if (!valid() || complete_) return complete_;
  count = std::min(count, MaxBytes - bytes_);
  bytes_ += count;
  const bool result = parser_.feed(bytes, count);
  if (!complete_ && (!result || bytes_ == MaxBytes)) failed_ = true;
  if (parser_.truncationFlags() & 0x1fu) {
    failed_ = true;
    complete_ = false;
  }
  return valid();
}
void HeadStyleExtractor::start(void* raw, const char* rawName, const char** attrs) {
  auto& self = *static_cast<HeadStyleExtractor*>(raw);
  ++self.depth_;
  const auto name = local(rawName);
  for (size_t i = 0; attrs && attrs[i]; i += 2)
    if (cssMedia::equal(attrs[i], "xml:base")) {
      self.stop(true);
      return;
    }
  if (cssMedia::equal(name, "head")) {
    self.inHead_ = true;
    return;
  }
  if (cssMedia::equal(name, "body")) {
    self.stop();
    return;
  }
  if (!self.inHead_) return;
  if (cssMedia::equal(name, "base")) {
    self.stop(true);
    return;
  }
  if (!cssMedia::equal(name, "style")) return;
  const char *type = nullptr, *media = nullptr;
  for (size_t i = 0; attrs && attrs[i]; i += 2) {
    if (cssMedia::equal(local(attrs[i]), "type")) type = attrs[i + 1];
    if (cssMedia::equal(local(attrs[i]), "media")) media = attrs[i + 1];
  }
  self.styleDepth_ = self.depth_;
  self.capture_ =
      (!type || cssMedia::equal(cssMedia::trim(type), "text/css")) && (!media || cssMedia::mayMatchScreen(media));
  if (self.capture_) self.emit("\n", 1);
}
void HeadStyleExtractor::end(void* raw, const char* rawName) {
  auto& self = *static_cast<HeadStyleExtractor*>(raw);
  const auto name = local(rawName);
  if (self.styleDepth_ == self.depth_ && cssMedia::equal(name, "style")) {
    if (self.capture_ && self.emit("\n", 1)) self.hasStyles_ = true;
    self.capture_ = false;
    self.styleDepth_ = 0;
  }
  if (cssMedia::equal(name, "head")) {
    self.stop();
    return;
  }
  if (self.depth_) --self.depth_;
}
void HeadStyleExtractor::text(void* raw, const char* bytes, int count) {
  auto& self = *static_cast<HeadStyleExtractor*>(raw);
  if (self.capture_ && self.valid() && count > 0) self.emit(bytes, count);
}
