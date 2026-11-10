#pragma once

#include <Print.h>

// Optional foreground-input cancellation for one render-owned EPUB operation.
// No heap allocation; a scope restores the previous hook even on early returns.
class StreamCancellation {
 public:
  using Check = bool (*)(void*);
  bool requested() const { return check_ && check_(context_); }
  bool enabled() const { return check_ != nullptr; }
  class Scope {
   public:
    Scope(StreamCancellation& owner, Check check, void* context)
        : owner_(owner), previous_(owner.check_), previousContext_(owner.context_) {
      owner_.check_ = check;
      owner_.context_ = context;
    }
    ~Scope() {
      owner_.check_ = previous_;
      owner_.context_ = previousContext_;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    StreamCancellation& owner_;
    Check previous_;
    void* previousContext_;
  };
  class Sink final : public Print {
   public:
    Sink(Print& output, const StreamCancellation& cancellation) : output_(output), cancellation_(cancellation) {}
    size_t write(uint8_t byte) override { return write(&byte, 1); }
    size_t write(const uint8_t* bytes, size_t length) override {
      return cancellation_.requested() ? 0 : output_.write(bytes, length);
    }

   private:
    Print& output_;
    const StreamCancellation& cancellation_;
  };

 private:
  Check check_ = nullptr;
  void* context_ = nullptr;
};
