#include "Page.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <PrintSerialization.h>
#include <Serialization.h>
#include <Utf8.h>

#include "PublisherBoxPaint.h"
#include "tables/TableColumnLayout.h"
#include "tables/TableTextLineOrder.h"

namespace {

constexpr uint16_t MAX_PAGE_ELEMENTS = Page::MAX_ELEMENTS;
constexpr uint8_t MAX_TABLE_ROWS_PER_FRAGMENT = 64;
constexpr uint8_t MAX_TABLE_CELLS_PER_ROW = 8;
constexpr uint8_t MAX_TABLE_LINES_PER_CELL = 64;
static_assert(TableFragmentCell::MAX_SERIALIZED_LINES == MAX_TABLE_LINES_PER_CELL);
static_assert(TableFragmentRow::MAX_SERIALIZED_CELLS == MAX_TABLE_CELLS_PER_ROW);
static_assert(PageTableFragment::MAX_SERIALIZED_ROWS == MAX_TABLE_ROWS_PER_FRAGMENT);

uint16_t tableSelectionForLine(const size_t elementIndex, const uint8_t logicalColumn) {
  if (elementIndex >= MAX_PAGE_ELEMENTS || logicalColumn >= MAX_TABLE_CELLS_PER_ROW) {
    return UINT16_MAX;
  }
  return static_cast<uint16_t>(elementIndex * MAX_TABLE_CELLS_PER_ROW + logicalColumn);
}

template <typename Predicate>
void renderFilteredPageElements(const std::vector<std::unique_ptr<PageElement>>& elements, GfxRenderer& renderer,
                                const int fontId, const int xOffset, const int yOffset, const bool foregroundBlack,
                                Predicate&& predicate) {
  for (const auto& element : elements) {
    if (predicate(*element)) {
      element->render(renderer, fontId, xOffset, yOffset, foregroundBlack);
    }
  }
}

}  // namespace

void PageLine::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                      const bool foregroundBlack) {
  block->render(renderer, fontId, xPos + xOffset, yPos + yOffset, foregroundBlack);
}

bool PageLine::serialize(Print& file) {
  if (!serialization::tryWritePod(file, xPos) || !serialization::tryWritePod(file, yPos)) {
    LOG_ERR("PGE", "Serialization failed: could not write PageLine coordinates");
    return false;
  }

  // serialize TextBlock pointed to by PageLine
  return block->serialize(file);
}

std::unique_ptr<PageLine> PageLine::deserialize(FsFile& file) {
  int16_t xPos;
  int16_t yPos;
  if (!serialization::tryReadPod(file, xPos) || !serialization::tryReadPod(file, yPos)) {
    LOG_ERR("PGE", "Deserialization failed: truncated PageLine coordinates");
    return nullptr;
  }

  auto tb = TextBlock::deserialize(file);
  if (!tb) {
    LOG_ERR("PGE", "Deserialization failed: PageLine text block was invalid");
    return nullptr;
  }

  auto* pageLine = new (std::nothrow) PageLine(std::move(tb), xPos, yPos);
  if (!pageLine) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageLine");
    return nullptr;
  }
  return std::unique_ptr<PageLine>(pageLine);
}

void PageImage::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                       const bool foregroundBlack) {
  render(renderer, fontId, xOffset, yOffset, foregroundBlack, true);
}

void PageImage::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                       const bool foregroundBlack, const bool imageGrayscale) {
  (void)fontId;
  // Images don't use fontId for text rendering
  imageBlock->render(renderer, xPos + xOffset, yPos + yOffset, foregroundBlack, imageGrayscale);
}

void PageImage::renderPlaceholder(GfxRenderer& renderer, const int xOffset, const int yOffset,
                                  const bool foregroundBlack) const {
  imageBlock->renderPlaceholder(renderer, xPos + xOffset, yPos + yOffset, foregroundBlack);
}

bool PageImage::serialize(Print& file) {
  if (!serialization::tryWritePod(file, xPos) || !serialization::tryWritePod(file, yPos) ||
      !serialization::tryWritePod(file, static_cast<uint8_t>(inlineImage))) {
    LOG_ERR("PGE", "Serialization failed: could not write PageImage coordinates");
    return false;
  }

  // serialize ImageBlock
  return imageBlock->serialize(file);
}

std::unique_ptr<PageImage> PageImage::deserialize(FsFile& file) {
  int16_t xPos;
  int16_t yPos;
  uint8_t inlineFlag;
  if (!serialization::tryReadPod(file, xPos) || !serialization::tryReadPod(file, yPos) ||
      !serialization::tryReadPod(file, inlineFlag) || inlineFlag > 1) {
    LOG_ERR("PGE", "Deserialization failed: truncated PageImage coordinates");
    return nullptr;
  }

  auto ib = ImageBlock::deserialize(file);
  if (!ib) {
    LOG_ERR("PGE", "Deserialization failed: PageImage block was invalid");
    return nullptr;
  }

  auto* pageImage = new (std::nothrow) PageImage(std::move(ib), xPos, yPos, inlineFlag != 0);
  if (!pageImage) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageImage");
    return nullptr;
  }
  return std::unique_ptr<PageImage>(pageImage);
}

void PageHorizontalRule::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                                const bool foregroundBlack) {
  (void)fontId;
  if (width == 0 || thickness == 0) {
    return;
  }

  renderer.drawLine(xPos + xOffset, yPos + yOffset, xPos + xOffset + width - 1, yPos + yOffset, thickness,
                    foregroundBlack);
}

bool PageHorizontalRule::serialize(Print& file) {
  return serialization::tryWritePod(file, xPos) && serialization::tryWritePod(file, yPos) &&
         serialization::tryWritePod(file, width) && serialization::tryWritePod(file, thickness);
}

std::unique_ptr<PageHorizontalRule> PageHorizontalRule::deserialize(FsFile& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  uint16_t width = 0;
  uint8_t thickness = 0;
  if (!serialization::tryReadPod(file, xPos) || !serialization::tryReadPod(file, yPos) ||
      !serialization::tryReadPod(file, width) || !serialization::tryReadPod(file, thickness)) {
    LOG_ERR("PGE", "Deserialization failed: truncated PageHorizontalRule metadata");
    return nullptr;
  }

  if (width == 0 || thickness == 0) {
    LOG_ERR("PGE", "Deserialization failed: invalid horizontal rule metadata (width=%u thickness=%u)", width,
            thickness);
    return nullptr;
  }

  auto* rule = new (std::nothrow) PageHorizontalRule(width, thickness, xPos, yPos);
  if (!rule) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageHorizontalRule");
    return nullptr;
  }
  return std::unique_ptr<PageHorizontalRule>(rule);
}

PageDropCap::PageDropCap(const uint8_t fontSize, const uint16_t scale256, const EpdFontFamily::Style style,
                         const char* utf8, const int16_t xPos, const int16_t yPos, const uint32_t fontFamily)
    : PageElement(xPos, yPos), fontSize(fontSize), fontFamily(fontFamily), scale256(scale256), style(style) {
  strncpy(text, utf8, MAX_TEXT_BYTES);
}

void PageDropCap::render(GfxRenderer& renderer, const int sectionFontId, const int xOffset, const int yOffset,
                         const bool foregroundBlack) {
  // A sized variant that can no longer be loaded draws from the section font instead.
  const int drawFontId = renderer.resolveBookFont(sectionFontId, fontFamily, fontSize);
  int x = xPos + xOffset;
  const auto* cursor = reinterpret_cast<const unsigned char*>(text);
  while (const uint32_t cp = utf8NextCodepoint(&cursor)) {
    x += renderer.drawScaledCodepoint(drawFontId, cp, style, x, yPos + yOffset, scale256, foregroundBlack);
  }
}

bool PageDropCap::serialize(Print& file) {
  if (!serialization::tryWritePod(file, xPos)) {
    LOG_ERR("PGE", "Truncated decoration write");
    return false;
  }
  if (!serialization::tryWritePod(file, yPos)) {
    LOG_ERR("PGE", "Truncated decoration write");
    return false;
  }
  if (!serialization::tryWritePod(file, fontSize)) {
    LOG_ERR("PGE", "Truncated decoration write");
    return false;
  }
  if (!serialization::tryWritePod(file, scale256)) {
    LOG_ERR("PGE", "Truncated decoration write");
    return false;
  }
  if (!serialization::tryWritePod(file, static_cast<uint8_t>(style))) {
    LOG_ERR("PGE", "Truncated decoration write");
    return false;
  }
  if (!serialization::tryWritePod(file, fontFamily)) return false;
  const auto length = static_cast<uint8_t>(strnlen(text, MAX_TEXT_BYTES));
  if (!serialization::tryWritePod(file, length)) {
    LOG_ERR("PGE", "Truncated decoration write");
    return false;
  }
  return file.write(reinterpret_cast<const uint8_t*>(text), length) == length;
}

std::unique_ptr<PageDropCap> PageDropCap::deserialize(FsFile& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  uint8_t fontSize = 0;
  uint16_t scale256 = 0;
  uint8_t style = 0;
  uint8_t length = 0;
  uint32_t fontFamily = 0;
  if (!serialization::tryReadPod(file, xPos)) {
    LOG_ERR("PGE", "Truncated decoration read");
    return nullptr;
  }
  if (!serialization::tryReadPod(file, yPos)) {
    LOG_ERR("PGE", "Truncated decoration read");
    return nullptr;
  }
  if (!serialization::tryReadPod(file, fontSize)) {
    LOG_ERR("PGE", "Truncated decoration read");
    return nullptr;
  }
  if (!serialization::tryReadPod(file, scale256)) {
    LOG_ERR("PGE", "Truncated decoration read");
    return nullptr;
  }
  if (!serialization::tryReadPod(file, style)) {
    LOG_ERR("PGE", "Truncated decoration read");
    return nullptr;
  }
  if (!serialization::tryReadPod(file, fontFamily) || !serialization::tryReadPod(file, length)) {
    LOG_ERR("PGE", "Truncated decoration read");
    return nullptr;
  }
  char text[MAX_TEXT_BYTES + 1] = {};
  if (length == 0 || length > MAX_TEXT_BYTES || scale256 == 0 || scale256 > 4096 || file.read(text, length) != length) {
    LOG_ERR("PGE", "Deserialization failed: invalid drop cap");
    return nullptr;
  }
  auto dropCap = makeUniqueNoThrow<PageDropCap>(fontSize, scale256, static_cast<EpdFontFamily::Style>(style), text,
                                                xPos, yPos, fontFamily);
  if (!dropCap) LOG_ERR("PGE", "Deserialization failed: could not allocate PageDropCap");
  return dropCap;
}

bool PageBorderBox::paintsPixel(int localX, int localY, int screenX, int screenY, bool& dark) const {
  return publisherBox::pixel(localX, localY, width, height, screenX, screenY, backgroundGray, shaded, sides, dark);
}

void PageBorderBox::render(GfxRenderer& renderer, const int, const int xOffset, const int yOffset,
                           const bool foregroundBlack) {
  const auto* fonts = renderer.getFontCacheManager();
  if ((fonts && fonts->isScanning()) || renderer.getRenderMode() != GfxRenderer::BW) return;
  const int x = xPos + xOffset, y = yPos + yOffset;
  const int left = std::max(0, -x), right = std::min<int>(width, renderer.getScreenWidth() - x);
  const int top = std::max(0, -y), bottom = std::min<int>(height, renderer.getScreenHeight() - y);
  for (int row = top; row < bottom; ++row)
    for (int col = left; col < right; ++col) {
      bool dark = false;
      if (paintsPixel(col, row, x + col, y + row, dark))
        renderer.drawPixel(x + col, y + row, dark ? foregroundBlack : !foregroundBlack);
    }
}

bool PageBorderBox::serialize(Print& file) {
  if (!serialization::tryWritePod(file, xPos)) {
    LOG_ERR("PGE", "Truncated decoration write");
    return false;
  }
  if (!serialization::tryWritePod(file, yPos)) {
    LOG_ERR("PGE", "Truncated decoration write");
    return false;
  }
  if (!serialization::tryWritePod(file, width)) {
    LOG_ERR("PGE", "Truncated decoration write");
    return false;
  }
  if (!serialization::tryWritePod(file, height)) {
    LOG_ERR("PGE", "Truncated decoration write");
    return false;
  }
  for (const CssBorderSide& side : sides) {
    if (!serialization::tryWritePod(file, side.width)) {
      LOG_ERR("PGE", "Truncated decoration write");
      return false;
    }
    if (!serialization::tryWritePod(file, static_cast<uint8_t>(side.style))) {
      LOG_ERR("PGE", "Truncated decoration write");
      return false;
    }
  }
  if (!serialization::tryWritePod(file, shaded)) {
    LOG_ERR("PGE", "Truncated decoration write");
    return false;
  }
  return serialization::tryWritePod(file, backgroundGray) && serialization::tryWritePod(file, backgroundLayer);
}

std::unique_ptr<PageBorderBox> PageBorderBox::deserialize(FsFile& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  CssBorderSide sides[4];
  bool shaded = false;
  if (!serialization::tryReadPod(file, xPos)) {
    LOG_ERR("PGE", "Truncated decoration read");
    return nullptr;
  }
  if (!serialization::tryReadPod(file, yPos)) {
    LOG_ERR("PGE", "Truncated decoration read");
    return nullptr;
  }
  if (!serialization::tryReadPod(file, width)) {
    LOG_ERR("PGE", "Truncated decoration read");
    return nullptr;
  }
  if (!serialization::tryReadPod(file, height)) {
    LOG_ERR("PGE", "Truncated decoration read");
    return nullptr;
  }
  for (CssBorderSide& side : sides) {
    uint8_t style = 0;
    if (!serialization::tryReadPod(file, side.width)) {
      LOG_ERR("PGE", "Truncated decoration read");
      return nullptr;
    }
    if (!serialization::tryReadPod(file, style)) {
      LOG_ERR("PGE", "Truncated decoration read");
      return nullptr;
    }
    if (side.width > 8 || style > static_cast<uint8_t>(CssBorderStyle::Dashed)) {
      LOG_ERR("PGE", "Deserialization failed: invalid border style %u", style);
      return nullptr;
    }
    side.style = static_cast<CssBorderStyle>(style);
  }
  if (!serialization::tryReadPod(file, shaded)) {
    LOG_ERR("PGE", "Truncated decoration read");
    return nullptr;
  }
  uint16_t gray = 256;
  uint8_t layer = 0;
  if (!serialization::tryReadPod(file, gray) || !serialization::tryReadPod(file, layer) ||
      gray > publisherBox::Transparent || layer > publisherBox::MaxLayer || width > 4096 || height > 4096) {
    LOG_ERR("PGE", "Invalid publisher box background");
    return nullptr;
  }
  if (width == 0 || height == 0) {
    LOG_ERR("PGE", "Deserialization failed: empty border box");
    return nullptr;
  }
  auto box = makeUniqueNoThrow<PageBorderBox>(width, height, sides, shaded, xPos, yPos);
  if (!box)
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageBorderBox");
  else
    box->configureBackground(gray, layer);
  return box;
}

bool TableFragmentCell::serialize(Print& file) const {
  if (colSpan == 0 || colSpan > MAX_TABLE_CELLS_PER_ROW || lines.size() > MAX_TABLE_LINES_PER_CELL) {
    LOG_ERR("PTB", "Serialization failed: invalid cell span/line count (span=%u lines=%u)", colSpan,
            static_cast<uint32_t>(lines.size()));
    return false;
  }

  if (!serialization::tryWritePod(file, isHeader) || !serialization::tryWritePod(file, colSpan) ||
      !serialization::tryWritePod(file, static_cast<uint8_t>(lines.size()))) {
    LOG_ERR("PTB", "Serialization failed: could not write table cell header");
    return false;
  }
  for (const auto& line : lines) {
    if (!line || !line->serialize(file)) {
      LOG_ERR("PTB", "Serialization failed: invalid table cell line");
      return false;
    }
  }
  const uint8_t hasImage = image ? 1 : 0;
  return serialization::tryWritePod(file, hasImage) &&
         (!image || (serialization::tryWritePod(file, imageSource) && image->serialize(file)));
}

bool TableFragmentCell::deserialize(FsFile& file, TableFragmentCell& outCell) {
  uint8_t lineCount = 0;
  if (!serialization::tryReadPod(file, outCell.isHeader) || !serialization::tryReadPod(file, outCell.colSpan) ||
      !serialization::tryReadPod(file, lineCount)) {
    LOG_ERR("PTB", "Deserialization failed: truncated table cell metadata");
    return false;
  }
  if (outCell.colSpan == 0 || outCell.colSpan > MAX_TABLE_CELLS_PER_ROW || lineCount > MAX_TABLE_LINES_PER_CELL) {
    LOG_ERR("PTB", "Deserialization failed: invalid cell span/line count (span=%u lines=%u)", outCell.colSpan,
            lineCount);
    return false;
  }

  outCell.lines.clear();
  outCell.lines.reserve(lineCount);
  for (uint8_t i = 0; i < lineCount; i++) {
    auto line = TextBlock::deserialize(file);
    if (!line) {
      LOG_ERR("PTB", "Deserialization failed: invalid table cell line");
      return false;
    }
    outCell.lines.push_back(std::move(line));
  }
  uint8_t hasImage = 0;
  outCell.image.reset();
  outCell.imageSource = UINT32_MAX;
  if (!serialization::tryReadPod(file, hasImage) || hasImage > 1) return false;
  if (hasImage) {
    if (!serialization::tryReadPod(file, outCell.imageSource)) return false;
    outCell.image = ImageBlock::deserialize(file);
    if (!outCell.image) return false;
  }
  return true;
}

bool TableFragmentRow::serialize(Print& file) const {
  if (cells.size() > MAX_TABLE_CELLS_PER_ROW) {
    LOG_ERR("PTB", "Serialization failed: row cell count %u exceeds maximum", static_cast<uint32_t>(cells.size()));
    return false;
  }
  uint8_t logicalColumns = 0;
  for (const auto& cell : cells) {
    if (cell.colSpan == 0 || static_cast<uint8_t>(logicalColumns + cell.colSpan) > MAX_TABLE_CELLS_PER_ROW) {
      LOG_ERR("PTB", "Serialization failed: row colspan exceeds maximum");
      return false;
    }
    logicalColumns = static_cast<uint8_t>(logicalColumns + cell.colSpan);
  }

  if (!serialization::tryWritePod(file, height) || !serialization::tryWritePod(file, headerSeparator) ||
      !serialization::tryWritePod(file, static_cast<uint8_t>(cells.size()))) {
    LOG_ERR("PTB", "Serialization failed: could not write table row metadata");
    return false;
  }
  for (const auto& cell : cells) {
    if (!cell.serialize(file)) {
      return false;
    }
  }
  return true;
}

bool TableFragmentRow::deserialize(FsFile& file, TableFragmentRow& outRow) {
  uint8_t cellCount = 0;
  if (!serialization::tryReadPod(file, outRow.height) || !serialization::tryReadPod(file, outRow.headerSeparator) ||
      !serialization::tryReadPod(file, cellCount)) {
    LOG_ERR("PTB", "Deserialization failed: truncated table row metadata");
    return false;
  }
  if (cellCount > MAX_TABLE_CELLS_PER_ROW) {
    LOG_ERR("PTB", "Deserialization failed: row cell count %u exceeds maximum", cellCount);
    return false;
  }

  outRow.cells.clear();
  outRow.cells.reserve(cellCount);
  uint8_t logicalColumns = 0;
  for (uint8_t i = 0; i < cellCount; i++) {
    TableFragmentCell cell;
    if (!TableFragmentCell::deserialize(file, cell)) {
      return false;
    }
    if (static_cast<uint8_t>(logicalColumns + cell.colSpan) > MAX_TABLE_CELLS_PER_ROW) {
      LOG_ERR("PTB", "Deserialization failed: row colspan exceeds maximum");
      return false;
    }
    logicalColumns = static_cast<uint8_t>(logicalColumns + cell.colSpan);
    outRow.cells.push_back(std::move(cell));
  }
  return true;
}

uint16_t PageTableFragment::getHeight() const {
  uint16_t total = 1;  // Bottom border.
  for (const auto& row : rows) {
    total = static_cast<uint16_t>(total + row.height);
  }
  return total;
}

void PageTableFragment::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                               const bool foregroundBlack) {
  if (columnCount == 0 || rows.empty() || width < 2) {
    return;
  }

  const int drawX = xPos + xOffset;
  const int drawY = yPos + yOffset;
  const uint16_t totalHeight = getHeight();

  std::vector<int16_t> columnStarts(columnCount + 1);
  for (uint8_t i = 0; i < columnCount; i++) {
    columnStarts[i] = static_cast<int16_t>(TableColumnLayout::columnStart(width, columnCount, i));
  }
  // Text clips use a half-open right edge; keep the final cell's full width
  // available while the border itself remains one pixel inside the table.
  columnStarts[columnCount] = static_cast<int16_t>(TableColumnLayout::columnStart(width, columnCount, columnCount));

  renderer.drawRect(drawX, drawY, width, totalHeight, foregroundBlack);

  int currentY = 0;
  for (size_t rowIndex = 0; rowIndex < rows.size(); rowIndex++) {
    const auto& row = rows[rowIndex];

    uint8_t logicalColumn = 0;

    for (size_t colIndex = 0; colIndex < row.cells.size() && colIndex < columnCount; colIndex++) {
      if (logicalColumn >= columnCount) break;
      const auto& cell = row.cells[colIndex];
      const uint8_t span =
          std::min<uint8_t>(cell.colSpan == 0 ? 1 : cell.colSpan, static_cast<uint8_t>(columnCount - logicalColumn));
      const int cellTextX = drawX + columnStarts[logicalColumn] + cellPadding;
      const int cellTextY = drawY + currentY + cellPadding;
      const int cellTextWidth = columnStarts[logicalColumn + span] - columnStarts[logicalColumn] - cellPadding * 2;
      const int cellTextHeight = row.height - cellPadding * 2;

      renderer.beginTextClip(cellTextX, cellTextY, cellTextWidth, cellTextHeight);
      for (size_t lineIndex = 0; lineIndex < cell.lines.size(); lineIndex++) {
        cell.lines[lineIndex]->render(renderer, fontId, cellTextX, cellTextY + static_cast<int>(lineIndex) * lineHeight,
                                      foregroundBlack);
      }
      renderer.endTextClip();
      logicalColumn = static_cast<uint8_t>(logicalColumn + span);
    }

    // Colspan cells own the interior vertical boundaries they cover. Draw
    // only boundaries that are present in this row, preserving the existing
    // one-column grid for ordinary cells.
    logicalColumn = 0;
    for (const auto& cell : row.cells) {
      if (logicalColumn >= columnCount) break;
      const uint8_t span =
          std::min<uint8_t>(cell.colSpan == 0 ? 1 : cell.colSpan, static_cast<uint8_t>(columnCount - logicalColumn));
      if (logicalColumn + span < columnCount) {
        const int x = drawX + columnStarts[logicalColumn + span];
        renderer.drawLine(x, drawY + currentY, x, drawY + currentY + row.height - 1, foregroundBlack);
      }
      logicalColumn = static_cast<uint8_t>(logicalColumn + span);
      if (logicalColumn >= columnCount) break;
    }

    currentY += row.height;
    if (rowIndex + 1 < rows.size()) {
      const int lineWidth = row.headerSeparator ? 2 : 1;
      renderer.drawLine(drawX, drawY + currentY, drawX + width - 1, drawY + currentY, lineWidth, foregroundBlack);
    }
  }
}

bool Page::forEachImage(const PageImageVisitor visitor, void* context, bool* invalidGeometry) const {
  if (invalidGeometry) *invalidGeometry = false;
  if (!visitor) return false;
  for (const auto& element : elements) {
    if (element->getTag() == TAG_PageImage) {
      auto& image = static_cast<PageImage&>(*element);
      if (!visitor({&image.getImageBlock(), image.xPos, image.yPos, image.isInlineImage()}, context)) return false;
    } else if (element->getTag() == TAG_PageTableFragment) {
      const auto& table = static_cast<const PageTableFragment&>(*element);
      int y = table.yPos;
      for (const auto& row : table.rows) {
        uint8_t column = 0;
        for (const auto& cell : row.cells) {
          if (column >= table.columnCount) break;
          const auto span = std::min<uint8_t>(std::max<uint8_t>(1, cell.colSpan), table.columnCount - column);
          if (cell.image) {
            const int width =
                TableColumnLayout::innerWidth(table.width, table.columnCount, column, span, table.cellPadding);
            const int x = table.xPos + TableColumnLayout::columnStart(table.width, table.columnCount, column) +
                          table.cellPadding + std::max(0, (width - cell.image->getWidth()) / 2);
            const int imageY = y + table.cellPadding + cell.lines.size() * table.lineHeight;
            if (cell.image->getWidth() <= 0 || cell.image->getHeight() <= 0 || cell.image->getWidth() > width ||
                imageY + cell.image->getHeight() > y + row.height - table.cellPadding) {
              LOG_ERR("PTB", "Skipping invalid table image geometry");
              if (invalidGeometry) *invalidGeometry = true;
            } else if (!visitor({cell.image.get(), x, imageY, true, true}, context))
              return false;
          }
          column += span;
        }
        y += row.height;
      }
    }
  }
  return true;
}

bool Page::hasImages() const {
  return !forEachImage([](const PageImagePlacement&, void*) { return false; }, nullptr);
}

bool Page::hasImagesNeedingDecode() const {
  return !forEachImage([](const PageImagePlacement& image, void*) { return !image.block->needsDecode(); }, nullptr);
}

bool Page::getImageBoundingBox(int16_t& outX, int16_t& outY, int16_t& outW, int16_t& outH) const {
  struct Bounds {
    int x = INT16_MAX, y = INT16_MAX, right = INT16_MIN, bottom = INT16_MIN;
    bool found = false;
  } bounds;
  forEachImage(
      [](const PageImagePlacement& image, void* context) {
        auto& b = *static_cast<Bounds*>(context);
        b.found = true;
        b.x = std::min(b.x, image.x);
        b.y = std::min(b.y, image.y);
        b.right = std::max(b.right, image.x + image.block->getWidth());
        b.bottom = std::max(b.bottom, image.y + image.block->getHeight());
        return true;
      },
      &bounds);
  if (bounds.found) {
    outX = std::clamp(bounds.x, int(INT16_MIN), int(INT16_MAX));
    outY = std::clamp(bounds.y, int(INT16_MIN), int(INT16_MAX));
    outW = std::clamp(bounds.right - bounds.x, 0, int(INT16_MAX));
    outH = std::clamp(bounds.bottom - bounds.y, 0, int(INT16_MAX));
  }
  return bounds.found;
}

bool Page::forEachTextLine(const PageTextLineVisitor visitor, void* context) const {
  if (!visitor) return false;

  for (size_t elementIndex = 0; elementIndex < elements.size(); ++elementIndex) {
    const auto& element = elements[elementIndex];
    if (!element) continue;

    if (element->getTag() == TAG_PageLine) {
      const auto& line = static_cast<const PageLine&>(*element);
      if (line.getBlock() && !visitor({line.getBlock().get(), line.xPos, line.yPos, 0, 0, 0, 0,
                                       line.getBlock()->getBlockStyle().lineHeight},
                                      context)) {
        return false;
      }
      continue;
    }

    if (element->getTag() != TAG_PageTableFragment) continue;
    const auto& fragment = static_cast<const PageTableFragment&>(*element);
    if (fragment.columnCount == 0 || fragment.rows.empty() || fragment.width < 2) continue;

    int currentY = 0;
    for (const auto& row : fragment.rows) {
      const bool visited =
          TableTextLineOrder::forEachCellLineInVisualOrder(row, [&](const size_t cellIndex, const size_t lineIndex) {
            uint8_t logicalColumn = 0;
            for (size_t precedingCell = 0; precedingCell < cellIndex; ++precedingCell) {
              if (logicalColumn >= fragment.columnCount) return true;
              const auto& cell = row.cells[precedingCell];
              logicalColumn = static_cast<uint8_t>(
                  logicalColumn + std::min<uint8_t>(cell.colSpan == 0 ? 1 : cell.colSpan,
                                                    static_cast<uint8_t>(fragment.columnCount - logicalColumn)));
            }
            if (logicalColumn >= fragment.columnCount) return true;

            const auto& cell = row.cells[cellIndex];
            if (!cell.lines[lineIndex]) return true;
            const uint8_t span = std::min<uint8_t>(cell.colSpan == 0 ? 1 : cell.colSpan,
                                                   static_cast<uint8_t>(fragment.columnCount - logicalColumn));
            const int cellX = fragment.xPos +
                              TableColumnLayout::columnStart(fragment.width, fragment.columnCount, logicalColumn) +
                              fragment.cellPadding;
            const int cellY = fragment.yPos + currentY + fragment.cellPadding;
            const int cellWidth = TableColumnLayout::innerWidth(fragment.width, fragment.columnCount, logicalColumn,
                                                                span, fragment.cellPadding);
            const int cellHeight = std::max(0, static_cast<int>(row.height) - fragment.cellPadding * 2);
            const int lineY = cellY + static_cast<int>(lineIndex) * fragment.lineHeight;
            const PageTextLine line{cell.lines[lineIndex].get(),
                                    cellX,
                                    lineY,
                                    cellX,
                                    cellY,
                                    cellWidth,
                                    cellHeight,
                                    fragment.lineHeight,
                                    true,
                                    tableSelectionForLine(elementIndex, logicalColumn)};
            return visitor(line, context);
          });
      if (!visited) return false;
      currentY += row.height;
    }
  }
  return true;
}

bool PageTableFragment::serialize(Print& file) {
  if (rows.size() > MAX_TABLE_ROWS_PER_FRAGMENT) {
    LOG_ERR("PTB", "Serialization failed: fragment row count %u exceeds maximum", static_cast<uint32_t>(rows.size()));
    return false;
  }

  if (!serialization::tryWritePod(file, xPos) || !serialization::tryWritePod(file, yPos) ||
      !serialization::tryWritePod(file, width) || !serialization::tryWritePod(file, columnCount) ||
      !serialization::tryWritePod(file, cellPadding) || !serialization::tryWritePod(file, lineHeight) ||
      !serialization::tryWritePod(file, static_cast<uint8_t>(rows.size()))) {
    LOG_ERR("PTB", "Serialization failed: could not write fragment metadata");
    return false;
  }
  for (const auto& row : rows) {
    if (!row.serialize(file)) {
      return false;
    }
  }
  return true;
}

std::unique_ptr<PageTableFragment> PageTableFragment::deserialize(FsFile& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  uint16_t width = 0;
  uint8_t columnCount = 0;
  uint8_t cellPadding = 0;
  uint16_t lineHeight = 0;
  uint8_t rowCount = 0;
  if (!serialization::tryReadPod(file, xPos) || !serialization::tryReadPod(file, yPos) ||
      !serialization::tryReadPod(file, width) || !serialization::tryReadPod(file, columnCount) ||
      !serialization::tryReadPod(file, cellPadding) || !serialization::tryReadPod(file, lineHeight) ||
      !serialization::tryReadPod(file, rowCount)) {
    LOG_ERR("PTB", "Deserialization failed: truncated fragment metadata");
    return nullptr;
  }

  if (rowCount == 0 || rowCount > MAX_TABLE_ROWS_PER_FRAGMENT || columnCount == 0 ||
      columnCount > MAX_TABLE_CELLS_PER_ROW || width < 2 || lineHeight == 0) {
    LOG_ERR("PTB", "Deserialization failed: invalid fragment metadata (rows=%u cols=%u width=%u lineHeight=%u)",
            rowCount, columnCount, width, lineHeight);
    return nullptr;
  }

  std::vector<TableFragmentRow> rows;
  rows.reserve(rowCount);
  for (uint8_t i = 0; i < rowCount; i++) {
    TableFragmentRow row;
    if (!TableFragmentRow::deserialize(file, row)) {
      return nullptr;
    }
    uint8_t column = 0;
    for (const auto& cell : row.cells) {
      const auto span = std::min<uint8_t>(cell.colSpan, columnCount > column ? columnCount - column : 0);
      if (cell.image) {
        const int innerWidth = TableColumnLayout::innerWidth(width, columnCount, column, span, cellPadding);
        const uint32_t contentHeight =
            cell.lines.size() * uint32_t(lineHeight) + cell.image->getHeight() + cellPadding * 2;
        if (!span || cell.image->getWidth() <= 0 || cell.image->getHeight() <= 0 ||
            cell.image->getWidth() > innerWidth || cell.image->getHeight() > TableFragmentCell::MAX_IMAGE_HEIGHT ||
            contentHeight > row.height)
          return nullptr;
      }
      column += span;
    }
    rows.push_back(std::move(row));
  }

  auto* fragment =
      new (std::nothrow) PageTableFragment(width, columnCount, cellPadding, lineHeight, std::move(rows), xPos, yPos);
  if (!fragment) {
    LOG_ERR("PTB", "Deserialization failed: could not allocate PageTableFragment");
    return nullptr;
  }
  return std::unique_ptr<PageTableFragment>(fragment);
}

void Page::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                  const bool foregroundBlack, const bool imageGrayscale) const {
  renderText(renderer, fontId, xOffset, yOffset, foregroundBlack);
  renderImages(renderer, fontId, xOffset, yOffset, foregroundBlack, imageGrayscale);
}

namespace {
// Recompose background ink into a 128-byte row tile. Inner white boxes mask
// parent shading; clearing gray selectors under solid ink keeps text AA from
// lightening it. No second framebuffer or page-sized mask is retained.
void finalizePublisherBackgrounds(const Page& page, GfxRenderer& renderer, int xOffset, int yOffset) {
  const auto* fonts = renderer.getFontCacheManager();
  if (renderer.getRenderMode() == GfxRenderer::BW || (fonts && fonts->isScanning())) return;
  int firstY = renderer.getScreenHeight(), lastY = 0;
  int firstX = renderer.getScreenWidth(), lastX = 0;
  uint8_t lastLayer = 0;
  for (const auto& element : page.elements)
    if (element->getTag() == TAG_PageBorderBox) {
      const auto& box = static_cast<const PageBorderBox&>(*element);
      firstY = std::min(firstY, std::max(0, box.yPos + yOffset));
      lastY = std::max(lastY, std::min(renderer.getScreenHeight(), box.yPos + yOffset + box.getHeight()));
      firstX = std::min(firstX, std::max(0, box.xPos + xOffset));
      lastX = std::max(lastX, std::min(renderer.getScreenWidth(), box.xPos + xOffset + box.getWidth()));
      lastLayer = std::max(lastLayer, box.paintLayer());
    }
  // Gray passes often target a small physical strip. Limit compositing to its
  // logical bounds instead of redoing every box pixel for every strip.
  if (renderer.isStripTargetActive()) {
    const int start = renderer.getWriteOriginY(), end = start + renderer.getWriteRows();
    const int height = renderer.getDisplayHeight();
    switch (renderer.getOrientation()) {
      case GfxRenderer::LandscapeCounterClockwise:
        firstY = std::max(firstY, start);
        lastY = std::min(lastY, end);
        break;
      case GfxRenderer::LandscapeClockwise:
        firstY = std::max(firstY, height - end);
        lastY = std::min(lastY, height - start);
        break;
      case GfxRenderer::Portrait:
        firstX = std::max(firstX, height - end);
        lastX = std::min(lastX, height - start);
        break;
      case GfxRenderer::PortraitInverted:
        firstX = std::max(firstX, start);
        lastX = std::min(lastX, end);
        break;
    }
  }
  uint8_t row[128];
  for (int y = firstY; y < lastY; ++y) {
    for (int start = firstX; start < lastX; start += 1024) {
      const int end = std::min(lastX, start + 1024);
      std::memset(row, 0, sizeof(row));
      for (uint8_t layer = 0; layer <= lastLayer; ++layer) {
        for (const auto& element : page.elements) {
          if (element->getTag() != TAG_PageBorderBox) continue;
          const auto& box = static_cast<const PageBorderBox&>(*element);
          const int x = box.xPos + xOffset, boxY = box.yPos + yOffset;
          if (box.paintLayer() != layer || y < boxY || y >= boxY + box.getHeight()) continue;
          for (int pixel = std::max(start, x); pixel < std::min<int>(end, x + box.getWidth()); ++pixel) {
            bool dark = false;
            if (!box.paintsPixel(pixel - x, y - boxY, pixel, y, dark)) continue;
            const int bit = pixel - start;
            const uint8_t mask = 1u << (bit & 7);
            if (dark)
              row[bit / 8] |= mask;
            else
              row[bit / 8] &= static_cast<uint8_t>(~mask);
          }
        }
      }
      for (int pixel = start; pixel < end; ++pixel)
        if (row[(pixel - start) / 8] & (1u << ((pixel - start) & 7))) renderer.drawPixel(pixel, y, true);
    }
  }
  // Images are opaque, even when their gray overlay is disabled by the reader.
  struct Clear {
    GfxRenderer& renderer;
    int x, y;
  } clear{renderer, xOffset, yOffset};
  page.forEachImage(
      [](const PageImagePlacement& image, void* context) {
        auto& c = *static_cast<Clear*>(context);
        c.renderer.fillRect(image.x + c.x, image.y + c.y, image.block->getWidth(), image.block->getHeight(), true);
        return true;
      },
      &clear);
}
}  // namespace

void Page::renderText(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                      const bool foregroundBlack) const {
  for (uint8_t layer = 0; layer <= publisherBox::MaxLayer; ++layer)
    renderFilteredPageElements(elements, renderer, fontId, xOffset, yOffset, foregroundBlack,
                               [layer](const PageElement& element) {
                                 return element.getTag() == TAG_PageBorderBox &&
                                        static_cast<const PageBorderBox&>(element).paintLayer() == layer;
                               });
  renderFilteredPageElements(elements, renderer, fontId, xOffset, yOffset, foregroundBlack,
                             [](const PageElement& element) {
                               return element.getTag() != TAG_PageImage && element.getTag() != TAG_PageBorderBox;
                             });
  finalizePublisherBackgrounds(*this, renderer, xOffset, yOffset);
}

void Page::renderImages(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                        const bool foregroundBlack, const bool imageGrayscale) const {
  (void)fontId;
  struct Draw {
    GfxRenderer& renderer;
    int x, y;
    bool foreground, gray;
  } draw{renderer, xOffset, yOffset, foregroundBlack, imageGrayscale};
  forEachImage(
      [](const PageImagePlacement& image, void* context) {
        auto& d = *static_cast<Draw*>(context);
        image.block->render(d.renderer, image.x + d.x, image.y + d.y, d.foreground, d.gray);
        return true;
      },
      &draw);
}

void Page::renderWithImagePlaceholders(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                                       const bool foregroundBlack, const bool renderCachedImages,
                                       const bool imageGrayscale) const {
  renderText(renderer, fontId, xOffset, yOffset, foregroundBlack);
  struct Draw {
    GfxRenderer& renderer;
    int x, y;
    bool foreground, cached, gray;
  } draw{renderer, xOffset, yOffset, foregroundBlack, renderCachedImages, imageGrayscale};
  forEachImage(
      [](const PageImagePlacement& image, void* context) {
        auto& d = *static_cast<Draw*>(context);
        if (!d.cached || image.block->needsDecode())
          image.block->renderPlaceholder(d.renderer, image.x + d.x, image.y + d.y, d.foreground);
        else
          image.block->render(d.renderer, image.x + d.x, image.y + d.y, d.foreground, d.gray);
        return true;
      },
      &draw);
}

uint16_t Page::imageEstimateUnits(const uint16_t viewportWidth, const uint16_t viewportHeight) const {
  bool hasImage = false;
  bool hasBlockImage = false;
  bool hasReadableContent = false;
  uint64_t imageArea = 0;
  for (const auto& element : elements) {
    switch (element->getTag()) {
      case TAG_PageImage: {
        hasImage = true;
        const auto& pageImage = static_cast<const PageImage&>(*element);
        const auto& image = pageImage.getImageBlock();
        hasBlockImage = hasBlockImage || !pageImage.isInlineImage();
        if (viewportWidth > 0 && viewportHeight > 0) {
          const int imageTop = std::max(0, static_cast<int>(element->yPos));
          const int imageBottom =
              std::min(static_cast<int>(viewportHeight),
                       static_cast<int>(element->yPos) + std::max(0, static_cast<int>(image.getHeight())));
          if (imageBottom > imageTop) {
            const uint32_t visibleHeight = static_cast<uint32_t>(imageBottom - imageTop);
            // Inline icons occupy only part of a text line. Protect their
            // approximate area instead of counting every icon as a full-width
            // strip, which skews estimates for image-rich paragraphs.
            const uint32_t countedWidth =
                pageImage.isInlineImage() ? static_cast<uint32_t>(std::clamp<int>(image.getWidth(), 0, viewportWidth))
                                          : viewportWidth;
            imageArea += static_cast<uint64_t>(visibleHeight) * countedWidth;
          }
        }
        break;
      }
      case TAG_PageDropCap:
      case TAG_PageLine:
      case TAG_PageTableFragment:
        hasReadableContent = true;
        break;
      case TAG_PageHorizontalRule:
      case TAG_PageBorderBox:
        break;
    }
  }

  struct TableArea {
    bool& hasImage;
    uint64_t& area;
    uint16_t width, height;
  } tableArea{hasImage, imageArea, viewportWidth, viewportHeight};
  forEachImage(
      [](const PageImagePlacement& image, void* context) {
        if (!image.inTable) return true;
        auto& area = *static_cast<TableArea*>(context);
        area.hasImage = true;
        const int visibleHeight =
            std::max(0, std::min<int>(area.height, image.y + image.block->getHeight()) - std::max(0, image.y));
        area.area += uint64_t(visibleHeight) * std::clamp<int>(image.block->getWidth(), 0, area.width);
        return true;
      },
      &tableArea);
  if (!hasImage) return 0;
  if ((!hasReadableContent && hasBlockImage) || viewportWidth == 0 || viewportHeight == 0)
    return PageCountEstimator::kUnitsPerPage;

  const uint64_t units =
      (imageArea * PageCountEstimator::kUnitsPerPage) / (static_cast<uint64_t>(viewportWidth) * viewportHeight);
  return static_cast<uint16_t>(std::min<uint64_t>(PageCountEstimator::kUnitsPerPage, units));
}

bool Page::serialize(Print& file) const {
  const uint16_t count = elements.size();
  if (elements.size() > MAX_PAGE_ELEMENTS) {
    LOG_ERR("PGE", "Serialization failed: element count %u exceeds maximum", static_cast<uint32_t>(elements.size()));
    return false;
  }
  if (!serialization::tryWritePod(file, count)) {
    LOG_ERR("PGE", "Serialization failed: could not write element count");
    return false;
  }

  for (const auto& el : elements) {
    // Use getTag() method to determine type
    if (!serialization::tryWritePod(file, static_cast<uint8_t>(el->getTag()))) {
      LOG_ERR("PGE", "Serialization failed: could not write element tag");
      return false;
    }

    if (!el->serialize(file)) {
      return false;
    }
  }

  // Serialize footnotes (clamp to MAX_FOOTNOTES_PER_PAGE to match addFootnote/deserialize limits)
  const uint16_t fnCount = std::min<uint16_t>(footnotes.size(), MAX_FOOTNOTES_PER_PAGE);
  if (!serialization::tryWritePod(file, fnCount)) {
    LOG_ERR("PGE", "Failed to write footnote count");
    return false;
  }
  for (uint16_t i = 0; i < fnCount; i++) {
    const auto& fn = footnotes[i];
    if (file.write(reinterpret_cast<const uint8_t*>(fn.number), sizeof(fn.number)) != sizeof(fn.number) ||
        file.write(reinterpret_cast<const uint8_t*>(fn.href), sizeof(fn.href)) != sizeof(fn.href) ||
        !serialization::tryWritePod(file, fn.linkId)) {
      LOG_ERR("PGE", "Failed to write footnote");
      return false;
    }
  }

  const uint8_t markerCount = std::min<uint8_t>(publisherPageMarkers.size(), MAX_PUBLISHER_PAGE_MARKERS_PER_PAGE);
  if (!serialization::tryWritePod(file, markerCount)) {
    LOG_ERR("PGE", "Failed to write publisher page marker count");
    return false;
  }
  for (uint8_t i = 0; i < markerCount; i++) {
    const auto& marker = publisherPageMarkers[i];
    if (!serialization::tryWritePod(file, marker.yPos) ||
        file.write(reinterpret_cast<const uint8_t*>(marker.label), sizeof(marker.label)) != sizeof(marker.label)) {
      LOG_ERR("PGE", "Failed to write publisher page marker");
      return false;
    }
  }

  return true;
}

std::unique_ptr<Page> Page::deserialize(FsFile& file) {
  auto* rawPage = new (std::nothrow) Page();
  if (!rawPage) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate Page");
    return nullptr;
  }
  auto page = std::unique_ptr<Page>(rawPage);

  uint16_t count;
  if (!serialization::tryReadPod(file, count)) {
    LOG_ERR("PGE", "Deserialization failed: could not read element count");
    return nullptr;
  }
  if (count > MAX_PAGE_ELEMENTS) {
    LOG_ERR("PGE", "Deserialization failed: element count %u exceeds maximum", count);
    return nullptr;
  }
  page->elements.reserve(count);

  for (uint16_t i = 0; i < count; i++) {
    uint8_t tag;
    if (!serialization::tryReadPod(file, tag)) {
      LOG_ERR("PGE", "Deserialization failed: truncated element tag");
      return nullptr;
    }

    if (tag == TAG_PageLine) {
      auto pl = PageLine::deserialize(file);
      if (!pl) {
        return nullptr;
      }
      page->elements.push_back(std::move(pl));
    } else if (tag == TAG_PageImage) {
      auto pi = PageImage::deserialize(file);
      if (!pi) {
        return nullptr;
      }
      page->elements.push_back(std::move(pi));
    } else if (tag == TAG_PageTableFragment) {
      auto fragment = PageTableFragment::deserialize(file);
      if (!fragment) {
        return nullptr;
      }
      page->elements.push_back(std::move(fragment));
    } else if (tag == TAG_PageDropCap) {
      auto cap = PageDropCap::deserialize(file);
      if (!cap) return nullptr;
      page->elements.push_back(std::move(cap));
    } else if (tag == TAG_PageBorderBox) {
      auto box = PageBorderBox::deserialize(file);
      if (!box) return nullptr;
      page->elements.push_back(std::move(box));
    } else if (tag == TAG_PageHorizontalRule) {
      auto rule = PageHorizontalRule::deserialize(file);
      if (!rule) {
        return nullptr;
      }
      page->elements.push_back(std::move(rule));
    } else {
      LOG_ERR("PGE", "Deserialization failed: Unknown tag %u", tag);
      return nullptr;
    }
  }

  // Deserialize footnotes
  uint16_t fnCount;
  if (!serialization::tryReadPod(file, fnCount)) {
    LOG_ERR("PGE", "Failed to read footnote count");
    return nullptr;
  }
  if (fnCount > MAX_FOOTNOTES_PER_PAGE) {
    LOG_ERR("PGE", "Invalid footnote count %u", fnCount);
    return nullptr;
  }
  page->footnotes.resize(fnCount);
  for (uint16_t i = 0; i < fnCount; i++) {
    auto& entry = page->footnotes[i];
    if (file.read(entry.number, sizeof(entry.number)) != sizeof(entry.number) ||
        file.read(entry.href, sizeof(entry.href)) != sizeof(entry.href) ||
        !serialization::tryReadPod(file, entry.linkId)) {
      LOG_ERR("PGE", "Failed to read footnote %u", i);
      return nullptr;
    }
    entry.number[sizeof(entry.number) - 1] = '\0';
    entry.href[sizeof(entry.href) - 1] = '\0';
  }

  uint8_t markerCount;
  if (!serialization::tryReadPod(file, markerCount)) {
    LOG_ERR("PGE", "Failed to read publisher page marker count");
    return nullptr;
  }
  if (markerCount > MAX_PUBLISHER_PAGE_MARKERS_PER_PAGE) {
    LOG_ERR("PGE", "Invalid publisher page marker count %u", markerCount);
    return nullptr;
  }
  page->publisherPageMarkers.reserve(markerCount);
  for (uint8_t i = 0; i < markerCount; i++) {
    Page::PublisherPageMarker marker;
    if (!serialization::tryReadPod(file, marker.yPos) ||
        file.read(marker.label, sizeof(marker.label)) != sizeof(marker.label)) {
      LOG_ERR("PGE", "Failed to read publisher page marker %u", i);
      return nullptr;
    }
    marker.label[sizeof(marker.label) - 1] = '\0';
    page->publisherPageMarkers.push_back(marker);
  }

  return page;
}

void Page::prepareImageCaches() const {
  forEachImage(
      [](const PageImagePlacement& image, void*) {
        image.block->prepareCache();
        return true;
      },
      nullptr);
}

ImageBlock::CacheWarmResult Page::warmImageCaches(GfxRenderer& renderer, const int xOffset, const int yOffset,
                                                  bool (*shouldCancel)(void*), void* context,
                                                  bool* cacheChanged) const {
  if (cacheChanged) *cacheChanged = false;
  struct Warm {
    GfxRenderer& renderer;
    int x, y;
    bool (*cancel)(void*);
    void* context;
    bool* changed;
    ImageBlock::CacheWarmResult result = ImageBlock::CacheWarmResult::Ready;
  } warm{renderer, xOffset, yOffset, shouldCancel, context, cacheChanged};
  bool invalidGeometry = false;
  forEachImage(
      [](const PageImagePlacement& image, void* context) {
        auto& w = *static_cast<Warm*>(context);
        if (w.cancel && w.cancel(w.context)) {
          w.result = ImageBlock::CacheWarmResult::Cancelled;
          return false;
        }
        const bool existed = !w.changed || image.block->hasValidCache();
        const auto result = image.block->warmCache(w.renderer, image.x + w.x, image.y + w.y, w.cancel, w.context);
        if (w.changed && !existed && result == ImageBlock::CacheWarmResult::Ready) *w.changed = true;
        if (result != ImageBlock::CacheWarmResult::Ready) w.result = result;
        return result != ImageBlock::CacheWarmResult::Cancelled;
      },
      &warm, &invalidGeometry);
  if (invalidGeometry && warm.result != ImageBlock::CacheWarmResult::Cancelled)
    return ImageBlock::CacheWarmResult::Unavailable;
  return warm.result;
}
