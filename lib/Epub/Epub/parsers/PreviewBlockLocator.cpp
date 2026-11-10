#include "PreviewBlockLocator.h"

#include <cstring>

PreviewBlockLocator::PreviewBlockLocator(const char* anchorId, const IsBlockTagFn isBlockTag)
    : anchorId_(anchorId), isBlockTag_(isBlockTag) {
  if (!anchorId_ || anchorId_[0] == '\0' || !isBlockTag_) {
    return;
  }

  parser_.init(this, handleStartElement, handleEndElement, nullptr, nullptr, true);
}

bool PreviewBlockLocator::feed(const char* data, const int len, const bool isFinal) {
  if (!ok()) return false;
  if (done()) return true;
  if (len < 0 || !parser_.feed(reinterpret_cast<const uint8_t*>(data), static_cast<size_t>(len))) return false;
  return !isFinal || parser_.finalize();
}

void PreviewBlockLocator::handleStartElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<PreviewBlockLocator*>(userData);
  self->ordinal_ += 1;

  const char* id = nullptr;
  for (int i = 0; atts && atts[i]; i += 2) {
    if (strcmp(atts[i], "id") == 0) {
      id = atts[i + 1];
      break;
    }
  }

  if (id && strcmp(id, self->anchorId_) == 0) {
    // A block that carries the anchor itself is already the right starting point. An inline anchor
    // resolves outwards to its innermost enclosing block so that block's leading text is kept.
    self->startOrdinal_ = (self->isBlockTag_(name) || self->openBlockCount_ == 0)
                              ? self->ordinal_
                              : self->openBlockOrdinals_[self->openBlockCount_ - 1];
    self->parser_.stop();
    return;
  }

  if (self->isBlockTag_(name) && self->openBlockCount_ < MAX_BLOCK_NESTING) {
    self->openBlockOrdinals_[self->openBlockCount_] = self->ordinal_;
    self->openBlockDepths_[self->openBlockCount_] = self->depth_;
    self->openBlockCount_ += 1;
  }
  self->depth_ += 1;
}

void PreviewBlockLocator::handleEndElement(void* userData, const XML_Char* name) {
  (void)name;
  auto* self = static_cast<PreviewBlockLocator*>(userData);
  if (self->depth_ > 0) {
    self->depth_ -= 1;
  }
  // The innermost open block closes when depth returns to the depth it was opened at.
  if (self->openBlockCount_ > 0 && self->openBlockDepths_[self->openBlockCount_ - 1] == self->depth_) {
    self->openBlockCount_ -= 1;
  }
}
