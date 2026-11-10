#include "HeadMediaInventory.h"

#include <algorithm>
#include <cstring>

#include "CssMedia.h"

namespace {
std::string_view localName(const char* name) {
  if (const char* colon = std::strrchr(name, ':')) return colon + 1;
  return name;
}
}  // namespace
HeadMediaInventory::HeadMediaInventory(Link link, void* context) : link_(link), context_(context) {
  parser_.init(this, start, end, nullptr, nullptr, true);
}
HeadMediaInventory::~HeadMediaInventory() = default;

void HeadMediaInventory::stop() {
  complete_ = true;
  parser_.stop();
}
bool HeadMediaInventory::feed(const uint8_t* bytes, const size_t length) {
  if (!valid() || complete_) return complete_;
  const size_t count = std::min(length, MaxBytes - bytes_);
  bytes_ += count;
  const bool result = parser_.feed(bytes, count);
  if (!complete_ && (!result || bytes_ == MaxBytes)) failed_ = true;
  if (parser_.truncationFlags() & 0x1fu) {
    failed_ = true;
    complete_ = false;
  }
  return valid();
}
void HeadMediaInventory::start(void* context, const char* rawName, const char** attrs) {
  auto& self = *static_cast<HeadMediaInventory*>(context);
  const auto name = localName(rawName);
  // XML/HTML base overrides change relative href resolution. Retain every
  // stylesheet when one is present instead of making an unsafe exclusion.
  for (size_t i = 0; attrs && attrs[i]; i += 2) {
    if (cssMedia::equal(attrs[i], "xml:base")) {
      self.failed_ = true;
      self.parser_.stop();
      return;
    }
  }
  if (self.inHead_ && cssMedia::equal(name, "base")) {
    self.failed_ = true;
    self.parser_.stop();
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
  if (!self.inHead_ || !cssMedia::equal(name, "link")) return;
  const char *href = nullptr, *media = nullptr, *rel = nullptr;
  for (size_t i = 0; attrs && attrs[i]; i += 2) {
    const auto key = localName(attrs[i]);
    if (cssMedia::equal(key, "href")) href = attrs[i + 1];
    if (cssMedia::equal(key, "media")) media = attrs[i + 1];
    if (cssMedia::equal(key, "rel")) rel = attrs[i + 1];
  }
  if (!href || !rel) return;
  std::string_view relations(rel);
  bool stylesheet = false;
  while (!(relations = cssMedia::trim(relations)).empty()) {
    size_t end = 0;
    while (end < relations.size() && !cssMedia::space(relations[end])) ++end;
    stylesheet |= cssMedia::equal(relations.substr(0, end), "stylesheet");
    relations.remove_prefix(end);
  }
  if (stylesheet && self.link_) self.link_(self.context_, href, !media || cssMedia::mayMatchScreen(media));
}
void HeadMediaInventory::end(void* context, const char* name) {
  auto& self = *static_cast<HeadMediaInventory*>(context);
  if (cssMedia::equal(localName(name), "head")) self.stop();
}
