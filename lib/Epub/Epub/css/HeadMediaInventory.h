#pragma once
#include <SaxParser/SaxParser.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

// Bounded read-only head scan. A caller may exclude print-only stylesheets only
// when every spine head was inventoried successfully. No external entities.
class HeadMediaInventory {
 public:
  using Link = void (*)(void*, const char*, bool);
  static constexpr size_t MaxBytes = 64 * 1024;
  HeadMediaInventory(Link link, void* context);
  ~HeadMediaInventory();
  HeadMediaInventory(const HeadMediaInventory&) = delete;
  HeadMediaInventory& operator=(const HeadMediaInventory&) = delete;
  bool feed(const uint8_t* bytes, size_t length);
  bool complete() const { return complete_; }
  bool valid() const { return parser_.isActive() && !failed_; }

 private:
  static void start(void*, const char*, const char**);
  static void end(void*, const char*);
  void stop();
  SaxParser parser_;
  Link link_;
  void* context_;
  size_t bytes_ = 0;
  bool inHead_ = false, complete_ = false, failed_ = false;
};
