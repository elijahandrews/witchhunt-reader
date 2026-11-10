#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Epub/FootnoteEntry.h"
#include "Epub/blocks/TextBlock.h"

class ImageBlock {
  std::string path_;
  int width_, height_;

 public:
  ImageBlock(std::string path, std::string, int width, int height)
      : path_(std::move(path)), width_(width), height_(height) {}
  int getWidth() const { return width_; }
  int getHeight() const { return height_; }
  const std::string& getImagePath() const { return path_; }
  std::unique_ptr<ImageBlock> fittedTo(int width, int height) const {
    const float scale = std::min({1.0f, float(width) / width_, float(height) / height_});
    return std::make_unique<ImageBlock>(path_, "", std::max(1, int(width_ * scale)), std::max(1, int(height_ * scale)));
  }
};

struct TableFragmentCell {
  static constexpr uint8_t MAX_SERIALIZED_LINES = 64;
  static constexpr uint16_t MAX_IMAGE_HEIGHT = 240;
  bool isHeader = false;
  uint8_t colSpan = 1;
  std::vector<std::shared_ptr<TextBlock>> lines;
  std::shared_ptr<ImageBlock> image;
  uint32_t imageSource = UINT32_MAX;
};

struct TableFragmentRow {
  static constexpr uint8_t MAX_SERIALIZED_CELLS = 8;
  uint16_t height = 0;
  bool headerSeparator = false;
  std::vector<TableFragmentCell> cells;
};

class Page {
 public:
  static constexpr uint16_t MAX_FOOTNOTES_PER_PAGE = 16;
};
