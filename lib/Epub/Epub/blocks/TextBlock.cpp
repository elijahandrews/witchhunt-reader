#include "TextBlock.h"

#include <BuildArena.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

bool TextBlock::guideDotsEnabled = false;

namespace {
// Style and continuation share one arena byte: bits 0-6 are the EpdFontFamily::Style, bit 7
// says the word is glued to its predecessor. See TextBlock::wordContinues.
uint8_t packStyle(const EpdFontFamily::Style style, const bool continues) {
  return static_cast<uint8_t>((static_cast<uint8_t>(style) & ~TextBlock::WORD_CONTINUES_BIT) |
                              (continues ? TextBlock::WORD_CONTINUES_BIT : 0));
}
uint32_t packedTypography(uint32_t typography, EpdFontFamily::Style style) {
  return (typography & ~wordTypography::ALL_SMALL_CAPS) |
         ((style & EpdFontFamily::ALL_SMALL_CAPS) ? wordTypography::ALL_SMALL_CAPS : 0);
}
}  // namespace

TextBlock::ArenaOffsets TextBlock::arenaOffsets(const uint16_t wordCount, const bool hasSizes,
                                                const bool hasTypography) {
  // Layout documented in TextBlock.h: 16-bit arrays first (textOff, xpos), then
  // 8-bit arrays (styles, optional sizes), then the text blob. textOff sits at 0.
  const size_t wc = wordCount;
  ArenaOffsets o{};
  o.xpos = wc * sizeof(uint16_t);
  o.styles = o.xpos + wc * sizeof(int16_t);
  o.sizes = o.styles + wc * sizeof(uint8_t);  // only meaningful when hasSizes
  o.text = o.sizes + (hasSizes ? wc * sizeof(uint8_t) : 0);
  o.typography = (o.text + 3) & ~size_t(3);
  if (hasTypography) o.text = o.typography + wc * sizeof(uint32_t);
  return o;
}

size_t TextBlock::arenaSize(const uint16_t wordCount, const bool hasSizes, const uint16_t textBytes,
                            const bool hasTypography) {
  return arenaOffsets(wordCount, hasSizes, hasTypography).text + textBytes;
}

bool TextBlock::allocArena(const size_t size, BuildArena* scratch) {
  if (scratch != nullptr) {
    // The two 16-bit arrays come first, so the base needs 2-byte alignment; 4 keeps the
    // cursor tidy for whatever follows.
    arena = static_cast<uint8_t*>(scratch->alloc(size, 4));
    if (arena != nullptr) {
      arenaOwned_ = false;
      return true;
    }
    // Region full (a page beyond what it was sized for): fall through to the heap for this
    // line alone. A mixed page is fine -- ownership is per block.
  }
  arena = new (std::nothrow) uint8_t[size];
  arenaOwned_ = arena != nullptr;
  return arena != nullptr;
}

void TextBlock::bindArenaPointers() {
  const uint8_t* base = arena;
  const ArenaOffsets o = arenaOffsets(numWords, sizesPresent, typographyPresent);
  textOffArr = reinterpret_cast<const uint16_t*>(base);
  xposArr = reinterpret_cast<const int16_t*>(base + o.xpos);
  stylesArr = base + o.styles;
  sizesArr = sizesPresent ? base + o.sizes : nullptr;
  textArr = reinterpret_cast<const char*>(base + o.text);
  typographyArr = typographyPresent ? reinterpret_cast<const uint32_t*>(base + o.typography) : nullptr;
}

TextBlock::TextBlock(std::vector<std::string> words, std::vector<int16_t> word_xpos,
                     std::vector<EpdFontFamily::Style> word_styles, const BlockStyle& blockStyle,
                     std::vector<uint8_t> word_sizes, const std::vector<bool>& word_continues,
                     const std::vector<uint32_t>& word_typography)
    // Narrow the parser's full BlockStyle to the render-only slice: the spacing
    // fields have already been consumed into word_xpos / the line's y by layout.
    : renderStyle{blockStyle.fontSizeMultiplier, blockStyle.headingFontId, blockStyle.alignment} {
  // Normalize the all-100% case to no sizes so uniform lines (the common case)
  // keep the zero-cost fast paths in render/serialize/line-height.
  if (std::all_of(word_sizes.begin(), word_sizes.end(), [](const uint8_t s) { return s == 100; })) {
    word_sizes.clear();
  }
  const bool hasSizes = !word_sizes.empty();

  if (words.size() != word_xpos.size() || words.size() != word_styles.size() || words.size() > 10000 ||
      (hasSizes && words.size() != word_sizes.size())) {
    LOG_ERR("TXB", "Construction failed: size mismatch (words=%u, xpos=%u, styles=%u, sizes=%u)",
            static_cast<uint32_t>(words.size()), static_cast<uint32_t>(word_xpos.size()),
            static_cast<uint32_t>(word_styles.size()), static_cast<uint32_t>(word_sizes.size()));
    isValid = false;
    return;
  }

  if (!word_typography.empty()) {
    if (word_typography.size() != words.size()) {
      isValid = false;
      return;
    }
  }
  const auto typographyFor = [&](size_t i) {
    return packedTypography(word_typography.empty() ? 0 : word_typography[i], word_styles[i]);
  };
  if (!words.empty()) {
    uniformTypography = typographyFor(0);
    for (size_t i = 1; i < words.size(); ++i)
      if (typographyFor(i) != uniformTypography) typographyPresent = true;
  }
  numWords = static_cast<uint16_t>(words.size());
  sizesPresent = hasSizes;
  if (numWords == 0) {
    return;  // valid empty block, no arena
  }

  // Pass 1: total text size, one NUL per word. A line is at most a physical row
  // of the page, so uint16_t offsets are ample; reject anything larger.
  size_t totalText = 0;
  for (const auto& w : words) totalText += w.size() + 1;
  if (totalText > UINT16_MAX) {
    LOG_ERR("TXB", "Construction failed: text size %u exceeds arena limit", static_cast<uint32_t>(totalText));
    numWords = 0;
    sizesPresent = false;
    isValid = false;
    return;
  }
  textBytes = static_cast<uint16_t>(totalText);

  const size_t size = arenaSize(numWords, sizesPresent, textBytes, typographyPresent);
  if (!allocArena(size, nullptr)) {
    LOG_ERR("TXB", "OOM: arena %u bytes", static_cast<uint32_t>(size));
    numWords = 0;
    textBytes = 0;
    sizesPresent = false;
    isValid = false;
    return;
  }

  // Pass 2: fill through mutable pointers derived from the same layout offsets
  // that bindArenaPointers() uses for the const views (bound below for reads).
  uint8_t* base = arena;
  const ArenaOffsets o = arenaOffsets(numWords, sizesPresent, typographyPresent);
  auto* textOff = reinterpret_cast<uint16_t*>(base);
  auto* xpos = reinterpret_cast<int16_t*>(base + o.xpos);
  uint8_t* styles = base + o.styles;
  char* text = reinterpret_cast<char*>(base + o.text);
  uint16_t off = 0;
  for (uint16_t i = 0; i < numWords; i++) {
    textOff[i] = off;
    xpos[i] = word_xpos[i];
    styles[i] = packStyle(word_styles[i], i < word_continues.size() && word_continues[i]);
    memcpy(text + off, words[i].data(), words[i].size());
    off += static_cast<uint16_t>(words[i].size());
    text[off++] = '\0';
  }
  if (sizesPresent) {
    uint8_t* sizes = base + o.sizes;
    for (uint16_t i = 0; i < numWords; i++) sizes[i] = word_sizes[i];
  }
  if (typographyPresent) {
    auto* typography = reinterpret_cast<uint32_t*>(base + o.typography);
    for (uint16_t i = 0; i < numWords; ++i) typography[i] = typographyFor(i);
  }
  bindArenaPointers();
}

TextBlock::TextBlock(const WordRange& range, const std::vector<int16_t>& word_xpos, const BlockStyle& blockStyle,
                     BuildArena* scratch)
    : renderStyle{blockStyle.fontSizeMultiplier, blockStyle.headingFontId, blockStyle.alignment} {
  if (range.words == nullptr || range.styles == nullptr) {
    LOG_ERR("TXB", "Construction failed: null word range");
    isValid = false;
    return;
  }
  const std::vector<std::string>& words = *range.words;
  const std::vector<EpdFontFamily::Style>& styleSrc = *range.styles;
  const size_t first = range.first;
  const size_t count = range.count;
  const bool hasTypographySrc = range.typography && !range.typography->empty();
  if (hasTypographySrc) {
    if (first > range.typography->size() || count > range.typography->size() - first) {
      isValid = false;
      return;
    }
  }

  // Every source array must actually span [first, first + count); xpos is per line so it is
  // indexed from 0 and only needs `count` entries.
  const bool hasSizeSrc = range.sizes != nullptr && !range.sizes->empty();
  // Optional, and only used when it actually spans the range -- a caller that does not track
  // continuation (or hands over a short vector) just produces space-separated words.
  const std::vector<bool>* continuesSrc =
      (range.continues != nullptr && first + count <= range.continues->size()) ? range.continues : nullptr;
  if (count > 10000 || first + count > words.size() || first + count > styleSrc.size() || word_xpos.size() != count ||
      (hasSizeSrc && first + count > range.sizes->size())) {
    LOG_ERR("TXB", "Construction failed: range out of bounds (first=%u, count=%u, words=%u, xpos=%u)",
            static_cast<uint32_t>(first), static_cast<uint32_t>(count), static_cast<uint32_t>(words.size()),
            static_cast<uint32_t>(word_xpos.size()));
    isValid = false;
    return;
  }

  const auto typographyFor = [&](size_t i) {
    return packedTypography(hasTypographySrc ? (*range.typography)[first + i] : 0, styleSrc[first + i]);
  };
  if (count) {
    uniformTypography = typographyFor(0);
    for (size_t i = 1; i < count; ++i)
      if (typographyFor(i) != uniformTypography) typographyPresent = true;
  }

  // Normalize the all-100% case to no sizes, matching the vector constructor so both paths
  // produce byte-identical arenas (and therefore identical section-cache bytes).
  bool anyNon100 = false;
  if (hasSizeSrc) {
    const std::vector<uint8_t>& sizeSrc = *range.sizes;
    for (size_t i = 0; i < count && !anyNon100; i++) anyNon100 = sizeSrc[first + i] != 100;
  }
  sizesPresent = anyNon100;

  numWords = static_cast<uint16_t>(count);
  if (numWords == 0) {
    return;  // valid empty block, no arena
  }

  // Pass 1: total text size, one NUL per word, soft hyphens excluded — they are stripped
  // during the copy below, so reserving their bytes would leave a gap between words.
  size_t totalText = 0;
  for (size_t i = 0; i < count; i++) {
    const std::string& w = words[first + i];
    size_t stripped = 0;
    for (size_t pos = w.find(SOFT_HYPHEN_UTF8); pos != std::string::npos;
         pos = w.find(SOFT_HYPHEN_UTF8, pos + SOFT_HYPHEN_BYTES)) {
      stripped += SOFT_HYPHEN_BYTES;
    }
    totalText += w.size() - stripped + 1;
  }
  if (totalText > UINT16_MAX) {
    LOG_ERR("TXB", "Construction failed: text size %u exceeds arena limit", static_cast<uint32_t>(totalText));
    numWords = 0;
    sizesPresent = false;
    isValid = false;
    return;
  }
  textBytes = static_cast<uint16_t>(totalText);

  const size_t size = arenaSize(numWords, sizesPresent, textBytes, typographyPresent);
  if (!allocArena(size, scratch)) {
    LOG_ERR("TXB", "OOM: arena %u bytes", static_cast<uint32_t>(size));
    numWords = 0;
    textBytes = 0;
    sizesPresent = false;
    isValid = false;
    return;
  }

  // Pass 2: fill straight from the caller's arrays — no intermediate per-line vectors.
  uint8_t* base = arena;
  const ArenaOffsets o = arenaOffsets(numWords, sizesPresent, typographyPresent);
  auto* textOff = reinterpret_cast<uint16_t*>(base);
  auto* xpos = reinterpret_cast<int16_t*>(base + o.xpos);
  uint8_t* styles = base + o.styles;
  char* text = reinterpret_cast<char*>(base + o.text);
  uint16_t off = 0;
  for (uint16_t i = 0; i < numWords; i++) {
    textOff[i] = off;
    xpos[i] = word_xpos[i];
    styles[i] = packStyle(styleSrc[first + i], continuesSrc != nullptr && (*continuesSrc)[first + i]);
    // Copy the word one soft-hyphen-free run at a time.
    const std::string& w = words[first + i];
    size_t start = 0;
    for (size_t pos = w.find(SOFT_HYPHEN_UTF8); pos != std::string::npos; pos = w.find(SOFT_HYPHEN_UTF8, start)) {
      const size_t run = pos - start;
      memcpy(text + off, w.data() + start, run);
      off += static_cast<uint16_t>(run);
      start = pos + SOFT_HYPHEN_BYTES;
    }
    const size_t tail = w.size() - start;
    memcpy(text + off, w.data() + start, tail);
    off += static_cast<uint16_t>(tail);
    text[off++] = '\0';
  }
  if (sizesPresent) {
    uint8_t* sizes = base + o.sizes;
    const std::vector<uint8_t>& sizeSrc = *range.sizes;
    for (uint16_t i = 0; i < numWords; i++) sizes[i] = sizeSrc[first + i];
  }
  if (typographyPresent) {
    auto* typography = reinterpret_cast<uint32_t*>(base + o.typography);
    for (uint16_t i = 0; i < numWords; ++i) typography[i] = typographyFor(i);
  }
  bindArenaPointers();
}

uint8_t TextBlock::maxSizePct() const {
  if (!sizesPresent) {
    return 100;
  }
  uint8_t m = 0;
  for (uint16_t i = 0; i < numWords; i++) m = std::max(m, sizesArr[i]);
  return m;
}

// Mirrors the per-word geometry in render() below. Kept adjacent to it on
// purpose: the two must move together.

float TextBlock::lineHeight(const GfxRenderer& renderer, const int fontId) const {
  const int baseFont = renderStyle.headingFontId ? renderStyle.headingFontId : fontId;
  float height = numWords ? 0.0f : renderer.getLineHeight(baseFont) * renderStyle.fontSizeMultiplier;
  for (uint16_t i = 0; i < numWords; ++i) {
    const auto f = renderer.resolveTextFont(baseFont, wordTypography::family(wordTypography(i)), wordScale(i));
    height = std::max(height, renderer.getLineHeight(f.fontId) * wordScale(i) * f.scale);
  }
  return height;
}

TextBlock::WordBox TextBlock::wordBox(const GfxRenderer& renderer, const uint16_t i, const int fontId, const int x,
                                      const int y) const {
  WordBox box;
  if (!isValid || i >= numWords) return box;
  const int baseFont = renderStyle.headingFontId ? renderStyle.headingFontId : fontId;
  const int blockAscender = renderer.getFontAscenderSizeScaled(baseFont, renderStyle.fontSizeMultiplier);
  int lineAscender = numWords ? 0 : blockAscender;
  for (uint16_t j = 0; j < numWords; ++j) {
    const auto f = renderer.resolveTextFont(baseFont, wordTypography::family(wordTypography(j)), wordScale(j));
    lineAscender = std::max(lineAscender, renderer.getFontAscenderSizeScaled(f.fontId, wordScale(j) * f.scale));
  }
  const auto f = renderer.resolveTextFont(baseFont, wordTypography::family(wordTypography(i)), wordScale(i));
  const float scale = wordScale(i) * f.scale;
  const auto style = wordStyle(i);
  const int ascender = renderer.getFontAscenderSizeScaled(f.fontId, scale);
  box.fontId = f.fontId;
  box.style = style;
  box.scale = scale;
  box.x = xposArr[i] + x;
  box.y = y + lineAscender - ascender;
  if (style & EpdFontFamily::SUP)
    box.y -= blockAscender * 2 / 5;
  else if (style & EpdFontFamily::SUB)
    box.y += blockAscender / 4;
  box.width =
      renderer.getTextWidthSpaced(f.fontId, wordText(i), style, scale, wordTypography::tracking(wordTypography(i)));
  box.height = renderer.getLineHeightScaled(f.fontId, scale);
  return box;
}

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y) const {
  if (!isValid) return;
  const int baseFont = renderStyle.headingFontId ? renderStyle.headingFontId : fontId;
  if (renderer.isFontCacheScanning()) {
    // Record text/family/style only. Measuring glyphs here would load SD metadata
    // before the prewarm pass and inflate its low-memory working set.
    for (uint16_t i = 0; i < numWords; ++i) {
      const auto f = renderer.resolveTextFont(baseFont, wordTypography::family(wordTypography(i)), wordScale(i));
      renderer.drawTextSpaced(f.fontId, x + xposArr[i], y, wordText(i), true, wordStyle(i), wordScale(i) * f.scale,
                              wordTypography::tracking(wordTypography(i)));
    }
    return;
  }
  const int blockAscender = renderer.getFontAscenderSizeScaled(baseFont, renderStyle.fontSizeMultiplier);
  int lineAscender = numWords ? 0 : blockAscender;
  for (uint16_t i = 0; i < numWords; ++i) {
    const auto f = renderer.resolveTextFont(baseFont, wordTypography::family(wordTypography(i)), wordScale(i));
    lineAscender = std::max(lineAscender, renderer.getFontAscenderSizeScaled(f.fontId, wordScale(i) * f.scale));
  }
  const int dotSize = std::max(2, blockAscender / 8);
  int prevWordEndX = 0;
  for (uint16_t i = 0; i < numWords; ++i) {
    const auto f = renderer.resolveTextFont(baseFont, wordTypography::family(wordTypography(i)), wordScale(i));
    const float scale = wordScale(i) * f.scale;
    const auto style = wordStyle(i);
    const int ascender = renderer.getFontAscenderSizeScaled(f.fontId, scale);
    const int wordX = x + xposArr[i];
    int wordY = y + lineAscender - ascender;
    if (style & EpdFontFamily::SUP)
      wordY -= blockAscender * 2 / 5;
    else if (style & EpdFontFamily::SUB)
      wordY += blockAscender / 4;
    renderer.drawTextSpaced(f.fontId, wordX, wordY, wordText(i), true, style, scale,
                            wordTypography::tracking(wordTypography(i)));
    const bool decorated = style & (EpdFontFamily::UNDERLINE | EpdFontFamily::STRIKETHROUGH);
    if (!guideDotsEnabled && !decorated) continue;
    const int width =
        renderer.getTextWidthSpaced(f.fontId, wordText(i), style, scale, wordTypography::tracking(wordTypography(i)));
    const int baseline = wordY + ascender;
    if (guideDotsEnabled && i > 0) {
      const int gap = wordX - prevWordEndX;
      if (gap >= dotSize + 2)
        renderer.fillRect(prevWordEndX + (gap - dotSize) / 2, baseline - blockAscender / 3 - dotSize / 2, dotSize,
                          dotSize, true);
    }
    prevWordEndX = wordX + width;
    if (style & EpdFontFamily::UNDERLINE) renderer.drawLine(wordX, baseline + 3, wordX + width, baseline + 3, 2, true);
    if (style & EpdFontFamily::STRIKETHROUGH)
      renderer.drawLine(wordX, wordY + ascender / 2 + 4, wordX + width, wordY + ascender / 2 + 4, 2, true);
  }
}

bool TextBlock::serialize(FsFile& file) const {
  if (!isValid) {
    LOG_ERR("TXB", "Serialization failed: invalid block");
    return false;
  }

  // Word data: scalars, then the arena verbatim -- its in-memory layout is
  // exactly the on-disk layout (see TextBlock.h), so one write covers all
  // per-word arrays and the text blob.
  serialization::writePod(file, numWords);
  serialization::writePod(file, static_cast<uint8_t>((sizesPresent ? 1 : 0) | (typographyPresent ? 2 : 0)));
  serialization::writePod(file, textBytes);
  if (numWords > 0) {
    const size_t size = arenaSize(numWords, sizesPresent, textBytes, typographyPresent);
    if (file.write(arena, size) != size) {
      LOG_ERR("TXB", "Serialization failed: arena write (%u bytes)", static_cast<uint32_t>(size));
      return false;
    }
  }

  serialization::writePod(file, uniformTypography);

  // Style: only the render slice. The spacing fields (margins, padding, indent
  // and their "defined" flags) used to be written here, but layout consumes them
  // into the word xpos / line y before a TextBlock exists, and nothing ever read
  // them back -- so they were 19 dead bytes per line on disk as well as in RAM.
  // Dropping them is a format change; SECTION_FILE_VERSION is bumped so stale
  // caches are rejected and rebuilt.
  serialization::writePod(file, renderStyle.alignment);
  serialization::writePod(file, renderStyle.fontSizeMultiplier);
  serialization::writePod(file, renderStyle.headingFontId);

  return true;
}

std::unique_ptr<TextBlock> TextBlock::deserialize(FsFile& file, BuildArena* scratch) {
  uint16_t wc = 0;
  uint8_t hasSizes = 0;
  uint16_t textBytes = 0;
  if (file.read(reinterpret_cast<uint8_t*>(&wc), sizeof(wc)) != static_cast<int>(sizeof(wc)) ||
      file.read(&hasSizes, sizeof(hasSizes)) != static_cast<int>(sizeof(hasSizes)) ||
      file.read(reinterpret_cast<uint8_t*>(&textBytes), sizeof(textBytes)) != static_cast<int>(sizeof(textBytes))) {
    LOG_ERR("TXB", "Deserialization failed: incomplete arena header");
    return nullptr;
  }

  // Sanity checks: cap the arena allocation and reject impossible geometry
  // (every word carries at least its NUL terminator).
  if (wc > 10000) {
    LOG_ERR("TXB", "Deserialization failed: word count %u exceeds maximum", wc);
    return nullptr;
  }
  if ((wc == 0 && textBytes != 0) || (wc > 0 && textBytes < wc)) {
    LOG_ERR("TXB", "Deserialization failed: bad text size %u for %u words", textBytes, wc);
    return nullptr;
  }

  std::unique_ptr<TextBlock> block(new (std::nothrow) TextBlock());
  if (!block) {
    LOG_ERR("TXB", "OOM: TextBlock");
    return nullptr;
  }
  block->numWords = wc;
  block->textBytes = textBytes;
  if (hasSizes & ~3u) return nullptr;
  block->sizesPresent = (hasSizes & 1) != 0;
  block->typographyPresent = (hasSizes & 2) != 0;

  if (wc > 0) {
    const size_t size = arenaSize(wc, block->sizesPresent, textBytes, block->typographyPresent);
    if (!block->allocArena(size, scratch)) {
      LOG_ERR("TXB", "OOM: arena %u bytes", static_cast<uint32_t>(size));
      return nullptr;
    }
    if (file.read(block->arena, size) != static_cast<int>(size)) {
      LOG_ERR("TXB", "Deserialization failed: arena read (%u bytes)", static_cast<uint32_t>(size));
      return nullptr;
    }
    block->bindArenaPointers();

    // Validate offsets before anything dereferences wordText(): offset 0 first,
    // strictly increasing, in bounds, and every word NUL-terminated (word i ends
    // at the byte before offset i+1; the last word at the last text byte).
    const uint16_t* textOff = block->textOffArr;
    const char* text = block->textArr;
    if (textOff[0] != 0 || text[textBytes - 1] != '\0') {
      LOG_ERR("TXB", "Deserialization failed: corrupt text layout");
      return nullptr;
    }
    for (uint16_t i = 1; i < wc; i++) {
      if (textOff[i] <= textOff[i - 1] || textOff[i] >= textBytes || text[textOff[i] - 1] != '\0') {
        LOG_ERR("TXB", "Deserialization failed: corrupt word offset %u", i);
        return nullptr;
      }
    }
  }

  serialization::readPod(file, block->uniformTypography);

  // Style: the render slice only (see serialize).
  RenderStyle& renderStyle = block->renderStyle;
  serialization::readPod(file, renderStyle.alignment);
  serialization::readPod(file, renderStyle.fontSizeMultiplier);
  serialization::readPod(file, renderStyle.headingFontId);

  return block;
}
