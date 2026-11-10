#pragma once
#include <Print.h>
#include <SaxParser/SaxParser.h>

#include <cstddef>
#include <cstdint>

// Streams only complete screen-applicable <style> bodies from a bounded XHTML
// head. The caller publishes the resulting CSS only after complete()+valid().
class HeadStyleExtractor {
 public:
  static constexpr size_t MaxBytes = 64 * 1024;
  explicit HeadStyleExtractor(Print& output);
  ~HeadStyleExtractor();
  HeadStyleExtractor(const HeadStyleExtractor&) = delete;
  HeadStyleExtractor& operator=(const HeadStyleExtractor&) = delete;
  bool feed(const uint8_t* bytes, size_t count);
  bool complete() const { return complete_; }
  bool valid() const { return parser_.isActive() && !failed_; }
  bool hasStyles() const { return hasStyles_; }

 private:
  static void start(void*, const char*, const char**);
  static void end(void*, const char*);
  static void text(void*, const char*, int);
  void stop(bool failure = false);
  bool emit(const char*, size_t);
  Print& output_;
  SaxParser parser_;
  size_t bytes_ = 0, cssBytes_ = 0;
  unsigned depth_ = 0, styleDepth_ = 0;
  bool inHead_ = false, capture_ = false, complete_ = false, failed_ = false, hasStyles_ = false;
};
