#include <BidiUtils.h>
#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <Epub/converters/ImageDimsProbe.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <Epub/parsers/PreviewBlockLocator.h>
#include <Epub/tables/CompactTableLayout.h>
#include <GfxRenderer.h>

#include <algorithm>

std::vector<Hyphenator::BreakInfo> parserTestHyphenBreaks;
std::vector<Hyphenator::BreakInfo> Hyphenator::breakOffsets(const std::string&, bool) { return parserTestHyphenBreaks; }

namespace BidiUtils {
bool startsWithRtl(const char*, int) { return false; }
int detectParagraphLevel(const char*, int fallbackLevel, int) { return fallbackLevel; }
bool computeVisualWordOrder(const std::vector<std::string>& words, bool, std::vector<uint16_t>& order) {
  order.resize(words.size());
  for (size_t index = 0; index < words.size(); ++index) order[index] = static_cast<uint16_t>(index);
  return true;
}
}  // namespace BidiUtils

TextBlock::TextBlock(const std::vector<std::string>& words, const std::vector<int16_t>& wordXpos,
                     const std::vector<EpdFontFamily::Style>&, const std::vector<uint8_t>&,
                     const std::vector<uint16_t>&, const std::vector<uint16_t>&, const std::vector<uint8_t>& wordFlags,
                     const std::vector<bool>&, const BlockStyle& blockStyle, std::vector<std::string> rubyTexts,
                     const std::vector<uint8_t>& wordSizes, const char*, int8_t characterSpacing,
                     const std::vector<uint32_t>& fontFamilies, const std::vector<int16_t>& letterSpacings,
                     const std::vector<uint16_t>& points64, const std::vector<uint8_t>& typography)
    : blockStyle(blockStyle),
      characterSpacing(characterSpacing),
      numWords(static_cast<uint16_t>(words.size())),
      rubyTexts(std::move(rubyTexts)) {
  if (!points64.empty()) {
    wordPoints64 = std::make_unique<uint16_t[]>(points64.size());
    std::copy(points64.begin(), points64.end(), wordPoints64.get());
  }
  if (!typography.empty()) {
    wordTypographyFlags = std::make_unique<uint8_t[]>(typography.size());
    std::copy(typography.begin(), typography.end(), wordTypographyFlags.get());
  }
  if (!letterSpacings.empty()) {
    wordLetterSpacings = std::make_unique<int16_t[]>(letterSpacings.size());
    std::copy(letterSpacings.begin(), letterSpacings.end(), wordLetterSpacings.get());
  }
  if (!fontFamilies.empty()) {
    wordFamilies = std::make_unique<uint32_t[]>(fontFamilies.size());
    std::copy(fontFamilies.begin(), fontFamilies.end(), wordFamilies.get());
  }
  if (wordXpos.empty()) return;
  const size_t headerBytes = (wordXpos.size() * sizeof(int16_t) + wordFlags.size() + wordSizes.size() + 1) & ~size_t(1);
  size_t characters = 0;
  for (const auto& word : words) characters += word.size() + 1;
  arena = std::make_unique<uint8_t[]>(headerBytes + words.size() * sizeof(uint16_t) + characters);
  auto* offsets = reinterpret_cast<uint16_t*>(arena.get() + headerBytes);
  auto* text = reinterpret_cast<char*>(offsets + words.size());
  size_t at = 0;
  for (size_t i = 0; i < words.size(); ++i) {
    offsets[i] = at;
    std::memcpy(text + at, words[i].c_str(), words[i].size() + 1);
    at += words[i].size() + 1;
  }
  textOffArr = offsets;
  textArr = text;
  textBytes = characters;
  auto* positions = reinterpret_cast<int16_t*>(arena.get());
  std::copy(wordXpos.begin(), wordXpos.end(), positions);
  xposArr = positions;
  if (!wordSizes.empty()) {
    auto* sizes = arena.get() + wordXpos.size() * sizeof(int16_t) + wordFlags.size();
    std::copy(wordSizes.begin(), wordSizes.end(), sizes);
    wordSizesArr = sizes;
    wordSizesPresent = true;
  }
  if (!wordFlags.empty()) {
    auto* flags = arena.get() + wordXpos.size() * sizeof(int16_t);
    std::copy(wordFlags.begin(), wordFlags.end(), flags);
    wordFlagsArr = flags;
    wordFlagsPresent = true;
  }
}
bool TextBlock::hasRuby() const { return false; }

bool Page::hasImages() const {
  for (const auto& element : elements) {
    if (element->getTag() == TAG_PageImage) return true;
    if (element->getTag() == TAG_PageTableFragment && static_cast<const PageTableFragment&>(*element).hasImages())
      return true;
  }
  return false;
}

bool ImageDecoderFactory::isFormatSupported(const std::string& path) { return path.ends_with(".jpg"); }
namespace {
class FixtureImageDecoder final : public ImageToFramebufferDecoder {
 public:
  bool decodeToFramebuffer(const std::string&, GfxRenderer&, const RenderConfig&) override { return false; }
  bool getDimensions(const std::string&, ImageDimensions& dims) const override {
    GfxRenderer::fileProbeHadLoan = GfxRenderer::loanActive;
    dims = {800, 200};
    return true;
  }
  const char* getFormatName() const override { return "JPEG"; }
};
}  // namespace
bool ImageDecoderFactory::supportsMemorySource(const std::string&) { return false; }
ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string&) {
  static FixtureImageDecoder decoder;
  return &decoder;
}

ImageBlock::ImageBlock(std::string imagePath, std::string sourcePath, const int16_t width, const int16_t height)
    : imagePath(std::move(imagePath)), sourcePath(std::move(sourcePath)), width(width), height(height) {}

void PageImage::render(GfxRenderer&, int, int, int, bool) {}
void PageImage::renderPlaceholder(GfxRenderer&, int, int, bool) const {}
bool PageImage::serialize(Print&) { return false; }

PreviewBlockLocator::PreviewBlockLocator(const char*, IsBlockTagFn) {}
bool PreviewBlockLocator::feed(const char*, int, bool) { return false; }

CompactTableLayout::CompactTableLayout(GfxRenderer& renderer, int, uint16_t, uint16_t, uint16_t, uint8_t,
                                       BlockStyle tableStyle, int8_t characterSpacing)
    : renderer_(renderer), tableStyle_(tableStyle), characterSpacing_(characterSpacing) {}
bool CompactTableLayout::beginRow() { return true; }
bool CompactTableLayout::beginCell(bool, uint8_t, uint32_t, const BlockStyle&) { return true; }
bool CompactTableLayout::appendWord(std::string_view, EpdFontFamily::Style, bool, bool, uint8_t, const SourceWordSpan&,
                                    const uint16_t*, size_t) {
  return true;
}
bool CompactTableLayout::endCell(const std::vector<std::pair<int, FootnoteEntry>>&) { return true; }
bool CompactTableLayout::setCellImage(std::shared_ptr<ImageBlock>, uint32_t) { return false; }
CompactTableLayout::RowResult CompactTableLayout::finishRow(TableFragmentRow&, std::vector<std::shared_ptr<TextBlock>>&,
                                                            std::vector<FootnoteEntry>&, uint32_t&) {
  return RowResult::Ok;
}

void PageLine::render(GfxRenderer&, int, int, int, bool) {}
bool PageLine::serialize(Print&) { return false; }
void PageHorizontalRule::render(GfxRenderer&, int, int, int, bool) {}
bool PageHorizontalRule::serialize(Print&) { return false; }
void PageTableFragment::render(GfxRenderer&, int, int, int, bool) {}
bool PageTableFragment::serialize(Print&) { return false; }

PageDropCap::PageDropCap(uint8_t size, uint16_t scale, EpdFontFamily::Style style, const char* source, int16_t x,
                         int16_t y, uint32_t family)
    : PageElement(x, y), fontSize(size), fontFamily(family), scale256(scale), style(style) {
  std::strncpy(text, source, MAX_TEXT_BYTES);
}
void PageDropCap::render(GfxRenderer&, int, int, int, bool) {}
bool PageDropCap::serialize(Print&) { return false; }
void PageBorderBox::render(GfxRenderer&, int, int, int, bool) {}
bool PageBorderBox::serialize(Print&) { return false; }

int TextBlock::resolvedFontId(const GfxRenderer& r, int font) const {
  return r.getFontIdForSize64(font, blockStyle.fontSize64 ? blockStyle.fontSize64 : uint16_t(blockStyle.fontSize) * 64);
}
int TextBlock::wordFontId(const GfxRenderer& r, int font, uint16_t i) const {
  return r.resolveBookFont64(resolvedFontId(r, font), wordFamilies ? wordFamilies[i] : 0, wordFontSize64(i));
}
int TextBlock::maxAscender(const GfxRenderer& r, int font) const {
  int result = r.getFontAscenderSize(resolvedFontId(r, font));
  for (uint16_t i = 0; i < numWords; ++i) result = std::max(result, r.getFontAscenderSize(wordFontId(r, font, i)));
  return result;
}
int TextBlock::maxLineHeight(const GfxRenderer& r, int font) const {
  int result = r.getLineHeight(resolvedFontId(r, font));
  for (uint16_t i = 0; i < numWords; ++i) result = std::max(result, r.getLineHeight(wordFontId(r, font, i)));
  return result;
}
