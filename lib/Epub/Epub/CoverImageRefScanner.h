#pragma once

#include <Print.h>
#include <SaxParser/SaxParser.h>

#include <algorithm>
#include <cstring>
#include <string_view>

// References come only from image elements, never comments or hyperlinks.
// The bounded HTML scan also accepts ordinary SVG image wrappers.
class CoverImageRefScanner final : public Print {
 public:
  static constexpr size_t MaxScanBytes = 64 * 1024;
  static constexpr size_t MaxRefBytes = 512;
  CoverImageRefScanner() { valid_ = parser_.init(this, start, nullptr, nullptr, nullptr, true); }
  std::string_view result() const { return valid_ ? std::string_view(reference_) : std::string_view{}; }
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* bytes, size_t count) override {
    if (!valid_ || done_ || scanned_ >= MaxScanBytes) return 0;
    const size_t amount = std::min(count, MaxScanBytes - scanned_);
    scanned_ += amount;
    valid_ = parser_.feed(bytes, amount);
    return !valid_ || done_ || scanned_ == MaxScanBytes ? 0 : amount;
  }

 private:
  static void start(void* context, const char* name, const char** attributes) {
    auto& self = *static_cast<CoverImageRefScanner*>(context);
    if (self.parser_.truncationFlags() & (SaxParser::kTruncAttrValue | SaxParser::kTruncMaxAttrs)) return;
    if (const auto* colon = strrchr(name, ':')) name = colon + 1;
    if (strcmp(name, "img") && strcmp(name, "image")) return;
    unsigned priority = 0;
    for (const char* wanted : {"xlink:href", "src", "href"}) {
      for (size_t i = 0; attributes && attributes[i]; i += 2) {
        if (strcmp(attributes[i], wanted) || priority >= self.priority_) continue;
        const std::string_view value(attributes[i + 1]);
        if (value.empty() || value.size() > MaxRefBytes || value.front() == '#' || value.find(':') != value.npos)
          continue;
        memcpy(self.reference_, value.data(), value.size());
        self.reference_[value.size()] = '\0';
        self.priority_ = priority;
        if (priority == 0) {
          self.done_ = true;
          self.parser_.stop();
        }
        return;
      }
      ++priority;
    }
  }
  SaxParser parser_;
  char reference_[MaxRefBytes + 1] = {};
  size_t scanned_ = 0;
  bool valid_ = false;
  bool done_ = false;
  unsigned priority_ = 3;
};
