#pragma once
#include <HalStorage.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "FootnoteEntry.h"
#include "PageCountEstimator.h"
#include "blocks/ImageBlock.h"
#include "blocks/TextBlock.h"

enum PageElementTag : uint8_t {
  TAG_PageLine = 1,
  TAG_PageImage = 2,  // New tag
  TAG_PageTableFragment = 3,
  TAG_PageHorizontalRule = 4,
  TAG_PageDropCap = 5,
  TAG_PageBorderBox = 6,
};

// represents something that has been added to a page
class PageElement {
 public:
  int16_t xPos;
  int16_t yPos;
  explicit PageElement(const int16_t xPos, const int16_t yPos) : xPos(xPos), yPos(yPos) {}
  virtual ~PageElement() = default;
  virtual void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool foregroundBlack = true) = 0;
  virtual bool serialize(Print& file) = 0;
  virtual PageElementTag getTag() const = 0;  // Add type identification
};

// a line from a block element
class PageLine final : public PageElement {
  std::shared_ptr<TextBlock> block;

 public:
  PageLine(std::shared_ptr<TextBlock> block, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), block(std::move(block)) {}
  const std::shared_ptr<TextBlock>& getBlock() const { return block; }
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool foregroundBlack = true) override;
  bool serialize(Print& file) override;
  PageElementTag getTag() const override { return TAG_PageLine; }
  static std::unique_ptr<PageLine> deserialize(FsFile& file);
};

// New PageImage class
class PageImage final : public PageElement {
  std::unique_ptr<ImageBlock> imageBlock;
  bool inlineImage = false;

 public:
  PageImage(std::unique_ptr<ImageBlock> block, const int16_t xPos, const int16_t yPos, const bool inlineImage = false)
      : PageElement(xPos, yPos), imageBlock(std::move(block)), inlineImage(inlineImage) {}
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool foregroundBlack = true) override;
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool foregroundBlack, bool imageGrayscale);
  void renderPlaceholder(GfxRenderer& renderer, int xOffset, int yOffset, bool foregroundBlack) const;
  bool serialize(Print& file) override;
  PageElementTag getTag() const override { return TAG_PageImage; }
  static std::unique_ptr<PageImage> deserialize(FsFile& file);
  const ImageBlock& getImageBlock() const { return *imageBlock; }
  ImageBlock& getImageBlock() { return *imageBlock; }
  bool isInlineImage() const { return inlineImage; }
};

class PageHorizontalRule final : public PageElement {
  uint16_t width;
  uint8_t thickness;

 public:
  PageHorizontalRule(uint16_t width, uint8_t thickness, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), width(width), thickness(thickness) {}

  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool foregroundBlack = true) override;
  bool serialize(Print& file) override;
  PageElementTag getTag() const override { return TAG_PageHorizontalRule; }
  static std::unique_ptr<PageHorizontalRule> deserialize(FsFile& file);
};

// An initial letter spanning several lines, drawn scaled up from a font's glyph.
// yPos is the baseline of the last spanned line.
class PageDropCap final : public PageElement {
 public:
  static constexpr size_t MAX_TEXT_BYTES = 12;  // leading punctuation plus the letter

 private:
  uint8_t fontSize;
  uint32_t fontFamily = 0;
  uint16_t scale256;
  EpdFontFamily::Style style;
  char text[MAX_TEXT_BYTES + 1] = {};

 public:
  PageDropCap(uint8_t fontSize, uint16_t scale256, EpdFontFamily::Style style, const char* utf8, int16_t xPos,
              int16_t yPos, uint32_t fontFamily = 0);
  const char* getText() const { return text; }
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool foregroundBlack = true) override;
  bool serialize(Print& file) override;
  PageElementTag getTag() const override { return TAG_PageDropCap; }
  static std::unique_ptr<PageDropCap> deserialize(FsFile& file);
};

// A CSS border and/or background shade around a block element's slice on this page.
// Sides split off by a page break have zero width.
class PageBorderBox final : public PageElement {
  uint16_t width;
  uint16_t height;
  CssBorderSide sides[4];  // top, right, bottom, left
  bool shaded;
  uint16_t backgroundGray = 256;
  uint8_t backgroundLayer = 0;

 public:
  PageBorderBox(uint16_t width, uint16_t height, const CssBorderSide (&sides)[4], bool shaded, int16_t xPos,
                int16_t yPos)
      : PageElement(xPos, yPos),
        width(width),
        height(height),
        sides{sides[0], sides[1], sides[2], sides[3]},
        shaded(shaded) {}
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool foregroundBlack = true) override;
  bool serialize(Print& file) override;
  PageElementTag getTag() const override { return TAG_PageBorderBox; }
  void configureBackground(uint16_t gray, uint8_t layer = 0) {
    backgroundGray = std::min<uint16_t>(gray, 256);
    backgroundLayer = std::min<uint8_t>(layer, 7);
  }
  uint8_t paintLayer() const { return backgroundLayer; }
  uint16_t getWidth() const { return width; }
  uint16_t getHeight() const { return height; }
  bool paintsPixel(int localX, int localY, int screenX, int screenY, bool& dark) const;
  static std::unique_ptr<PageBorderBox> deserialize(FsFile& file);
};

struct TableFragmentCell {
  static constexpr uint8_t MAX_SERIALIZED_LINES = 64;
  static constexpr uint16_t MAX_IMAGE_HEIGHT = 240;
  bool isHeader = false;
  uint8_t colSpan = 1;
  std::vector<std::shared_ptr<TextBlock>> lines;
  std::shared_ptr<ImageBlock> image;
  uint32_t imageSource = UINT32_MAX;

  bool serialize(Print& file) const;
  static bool deserialize(FsFile& file, TableFragmentCell& outCell);
};

struct TableFragmentRow {
  static constexpr uint8_t MAX_SERIALIZED_CELLS = 8;
  uint16_t height = 0;
  bool headerSeparator = false;
  std::vector<TableFragmentCell> cells;

  bool serialize(Print& file) const;
  static bool deserialize(FsFile& file, TableFragmentRow& outRow);
};

struct PageTextLine {
  const TextBlock* block = nullptr;
  int xPos = 0;
  int yPos = 0;
  int clipX = 0;
  int clipY = 0;
  int clipWidth = 0;
  int clipHeight = 0;
  int lineHeight = 0;
  bool isTableText = false;
  // Identifies a logical table column within this page layout. Normal text and
  // unsupported table shapes use the sentinel, so their selection behavior is
  // unchanged.
  uint16_t tableSelection = UINT16_MAX;
};

using PageTextLineVisitor = bool (*)(const PageTextLine& line, void* context);
struct PageImagePlacement {
  ImageBlock* block = nullptr;
  int x = 0, y = 0;
  bool inlineImage = false;
  bool inTable = false;
};
using PageImageVisitor = bool (*)(const PageImagePlacement& image, void* context);

class PageTableFragment final : public PageElement {
  friend class Page;

 public:
  static constexpr uint8_t MAX_SERIALIZED_ROWS = 64;

 private:
  uint16_t width;
  uint8_t columnCount;
  uint8_t cellPadding;
  uint16_t lineHeight;
  std::vector<TableFragmentRow> rows;

 public:
  PageTableFragment(uint16_t width, uint8_t columnCount, uint8_t cellPadding, uint16_t lineHeight,
                    std::vector<TableFragmentRow> rows, int16_t xPos, int16_t yPos)
      : PageElement(xPos, yPos),
        width(width),
        columnCount(columnCount),
        cellPadding(cellPadding),
        lineHeight(lineHeight),
        rows(std::move(rows)) {}

  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool foregroundBlack = true) override;
  bool serialize(Print& file) override;
  PageElementTag getTag() const override { return TAG_PageTableFragment; }
  static std::unique_ptr<PageTableFragment> deserialize(FsFile& file);
  uint16_t getHeight() const;
  bool hasImages() const {
    for (const auto& row : rows)
      for (const auto& cell : row.cells)
        if (cell.image) return true;
    return false;
  }
};

class Page {
  SourceAnchor sourceAnchor_;
  SourceRange sourceRange_;
  bool sourceHasText_ = false;
  bool sourceProbeMatched_ = false;

 public:
  void noteSourceSpan(SourceWordSpan source) {
    if (!source.valid()) return;
    if (!sourceHasText_ || source.start < sourceAnchor_.sourceOffset)
      sourceAnchor_ = {source.start, source.characterOffset};
    sourceHasText_ = true;
    sourceRange_.first = std::min(sourceRange_.first, source.start);
    sourceRange_.end = std::max(sourceRange_.end, source.end);
    sourceProbeMatched_ |= source.probeByte != UINT16_MAX;
  }
  void noteSourceLine(const TextBlock& line) {
    if (!line.sourceAnchor().valid()) return;
    noteSourceSpan({line.sourceAnchor().sourceOffset, line.sourceEnd(), line.sourceAnchor().characterOffset,
                    static_cast<uint16_t>(line.sourceProbeMatched() ? 0 : UINT16_MAX)});
    sourceRange_.first = std::min(sourceRange_.first, line.sourceMin());
  }
  void noteSourcePoint(uint32_t offset) {
    if (offset == UINT32_MAX) return;
    if (!sourceAnchor_.valid()) sourceAnchor_ = {offset, 0};
    sourceRange_.first = std::min(sourceRange_.first, offset);
    sourceRange_.end = std::max(sourceRange_.end, offset + 1);
  }
  void mergeSource(const Page& other) {
    if (other.sourceHasText_ && (!sourceHasText_ || other.sourceAnchor_.sourceOffset < sourceAnchor_.sourceOffset))
      sourceAnchor_ = other.sourceAnchor_;
    else if (!sourceAnchor_.valid())
      sourceAnchor_ = other.sourceAnchor_;
    sourceHasText_ |= other.sourceHasText_;
    sourceRange_.first = std::min(sourceRange_.first, other.sourceRange_.first);
    sourceRange_.end = std::max(sourceRange_.end, other.sourceRange_.end);
    sourceProbeMatched_ |= other.sourceProbeMatched_;
  }
  SourceAnchor sourceAnchor() const { return sourceAnchor_; }
  SourceRange sourceRange() const { return sourceRange_; }
  bool sourceProbeMatched() const { return sourceProbeMatched_; }
  struct PublisherPageMarker {
    int16_t yPos = 0;
    char label[16] = {};
  };

  static constexpr uint16_t MAX_ELEMENTS = 1024;

  // the list of block index and line numbers on this page
  // Page elements have one owner: their page. Text blocks remain shared by
  // PageLine entries when a laid-out block spans multiple lines or pages.
  std::vector<std::unique_ptr<PageElement>> elements;
  std::vector<FootnoteEntry> footnotes;
  std::vector<PublisherPageMarker> publisherPageMarkers;
  static constexpr uint16_t MAX_FOOTNOTES_PER_PAGE = EPUB_MAX_FOOTNOTES_PER_PAGE;
  static constexpr uint8_t INITIAL_FOOTNOTE_RESERVE = 2;
  static constexpr uint8_t MAX_PUBLISHER_PAGE_MARKERS_PER_PAGE = 8;
  static constexpr uint8_t INITIAL_PUBLISHER_PAGE_MARKER_RESERVE = 2;

  void addFootnote(const char* number, const char* href, const uint8_t linkId = 0) {
    if (linkId != 0 && std::any_of(footnotes.begin(), footnotes.end(),
                                   [linkId](const FootnoteEntry& entry) { return entry.linkId == linkId; })) {
      return;
    }
    if (footnotes.size() >= MAX_FOOTNOTES_PER_PAGE) return;  // Cap per-page footnotes
    if (footnotes.empty()) {
      footnotes.reserve(INITIAL_FOOTNOTE_RESERVE);
    }
    FootnoteEntry entry;
    std::strncpy(entry.number, number, sizeof(entry.number) - 1);
    entry.number[sizeof(entry.number) - 1] = '\0';
    std::strncpy(entry.href, href, sizeof(entry.href) - 1);
    entry.href[sizeof(entry.href) - 1] = '\0';
    entry.linkId = linkId;
    footnotes.push_back(entry);
  }

  void addPublisherPageMarker(const char* label, const int yPos) {
    if (!label || label[0] == '\0' || publisherPageMarkers.size() >= MAX_PUBLISHER_PAGE_MARKERS_PER_PAGE) return;
    if (publisherPageMarkers.empty()) {
      publisherPageMarkers.reserve(INITIAL_PUBLISHER_PAGE_MARKER_RESERVE);
    }
    PublisherPageMarker marker;
    marker.yPos = static_cast<int16_t>(std::clamp(yPos, static_cast<int>(INT16_MIN), static_cast<int>(INT16_MAX)));
    std::strncpy(marker.label, label, sizeof(marker.label) - 1);
    marker.label[sizeof(marker.label) - 1] = '\0';
    publisherPageMarkers.push_back(marker);
  }

  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool foregroundBlack = true,
              bool imageGrayscale = true) const;
  void renderText(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool foregroundBlack = true) const;
  void renderImages(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool foregroundBlack = true,
                    bool imageGrayscale = true) const;
  // When renderCachedImages is false, draw placeholders without checking or
  // reading image caches. This keeps transient queued pages free of image I/O.
  void renderWithImagePlaceholders(GfxRenderer& renderer, int fontId, int xOffset, int yOffset,
                                   bool foregroundBlack = true, bool renderCachedImages = true,
                                   bool imageGrayscale = true) const;
  bool forEachTextLine(PageTextLineVisitor visitor, void* context) const;
  bool forEachImage(PageImageVisitor visitor, void* context, bool* invalidGeometry = nullptr) const;
  bool serialize(Print& file) const;
  static std::unique_ptr<Page> deserialize(FsFile& file);

  // Return the fixed-point page units protected by images on this page. Text
  // pages return zero; image-only pages are one full page (256 units), while
  // mixed pages contribute their visible image-height fraction.
  uint16_t imageEstimateUnits(uint16_t viewportWidth, uint16_t viewportHeight) const;

  // Check if page contains any images (used to force full refresh)
  bool hasImages() const;

  bool hasText() const {
    return std::any_of(elements.begin(), elements.end(), [](const std::unique_ptr<PageElement>& element) {
      return element->getTag() == TAG_PageLine || element->getTag() == TAG_PageTableFragment;
    });
  }

  void prepareImageCaches() const;
  ImageBlock::CacheWarmResult warmImageCaches(GfxRenderer& renderer, int xOffset, int yOffset,
                                              bool (*shouldCancel)(void*), void* context,
                                              bool* cacheChanged = nullptr) const;

  bool hasImagesNeedingDecode() const;

  // Get bounding box of all images on the page (union of image rects)
  // Returns false if no images. Coordinates are relative to page origin.
  bool getImageBoundingBox(int16_t& outX, int16_t& outY, int16_t& outW, int16_t& outH) const;
};
