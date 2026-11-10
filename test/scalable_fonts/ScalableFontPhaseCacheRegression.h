#pragma once

#include <HalScalableFont.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

// Original ASCII prose, with the same word-local fractional pen and +5
// character spacing as the reader. Host-only regression: no private book/font
// and no inspection of the adapter's cache implementation.
static void verifyWarmPhaseCache(HalScalableFont& owner, const std::vector<uint8_t>& fontBytes) {
  static constexpr char prose[] =
      "The afternoon light fell across the little office and found a fine layer of dust on the shelf. "
      "I had finished the first page, but the final sentence still felt difficult. Outside, five people "
      "were waiting for the ferry. A quiet conversation drifted through the open window. The tide was "
      "falling, and the boats settled lower beside the wall. Someone laughed. A gull turned above the "
      "water, then disappeared behind the roof. I put down the pencil and read the paragraph again. "
      "There was nothing unusual about the words, yet their meaning had changed. The old map showed "
      "a footpath that no longer reached the shore. Perhaps the bridge had fallen in a storm. Perhaps "
      "we had simply forgotten where to look. I folded the letter and slipped it into my pocket. "
      "Before leaving, I checked the small clock above the door. We still had enough time to find the "
      "beginning of the path. A friend would meet us at the station with coffee and a different map.";
  struct Request {
    uint32_t codepoint;
    int x;
    int y;
    int8_t phase;
  };
  const auto* face = owner.atSize(11);
  assert(face);
  const auto* data = face->data;
  assert(data->fractionalAdvances && data->dynamicGlyphHandler && data->kerning32Handler && data->rasterHandler);
  std::vector<Request> requests;
  requests.reserve(sizeof(prose));
  int wordX = 20, wordY = 30;
  const char* cursor = prose;
  while (*cursor) {
    while (*cursor == ' ') ++cursor;
    if (!*cursor) break;
    const size_t begin = requests.size();
    int x = 0, remainder = 0, advance = 0, gaps = 0;
    uint32_t previous = 0;
    while (*cursor && *cursor != ' ') {
      const uint32_t cp = static_cast<unsigned char>(*cursor++);
      const auto* glyph = data->dynamicGlyphHandler(data->glyphMissCtx, cp);
      assert(glyph);
      const int currentAdvance = fp5::advance(*glyph);
      if (previous) {
        const int total = advance + data->kerning32Handler(data->glyphMissCtx, previous, cp) + remainder;
        const int step = fp5::toPixel(total);
        remainder = total - step * 32;
        x += step + fp4::toPixel((gaps + 1) * 40) - fp4::toPixel(gaps * 40);
        ++gaps;
      }
      requests.push_back({cp, x, 0, int8_t(remainder >> 1)});
      advance = currentAdvance;
      previous = cp;
    }
    const int width = x + fp5::toPixel(advance + remainder);
    if (wordX + width > 460) {
      wordX = 20;
      wordY += 29;
    }
    if (wordY > 785) {
      requests.resize(begin);
      break;
    }
    for (size_t i = begin; i < requests.size(); ++i) {
      requests[i].x += wordX;
      requests[i].y = wordY;
    }
    wordX += width + 8;
  }
  assert(requests.size() > 500);
  constexpr int width = 480, height = 800;
  std::vector<uint8_t> referencePixels(width * height, 0), actualPixels(width * height, 0);
  const auto putPixel = [&](std::vector<uint8_t>& pixels, int x, int y, uint8_t value) {
    if (x >= 0 && x < width && y >= 0 && y < height)
      pixels[size_t(y) * width + x] = std::max(pixels[size_t(y) * width + x], value);
  };
  freeink::font::FtFont reference;
  assert(reference.init(fontBytes.data(), fontBytes.size(), 1));
  freeink::font::FtFont::RenderOptions light;
  light.hinting = freeink::font::FtFont::HintingMode::Light;
  assert(reference.setRenderOptions(light));
  for (const auto& request : requests) {
    const auto* glyph = reference.rasterize26_6(request.codepoint, scalableFontPixelSize26_6(11), request.phase * 4);
    assert(glyph);
    for (unsigned y = 0; y < glyph->height; ++y)
      for (unsigned x = 0; x < glyph->width; ++x)
        putPixel(referencePixels, request.x + glyph->xoff + int(x), request.y + glyph->yoff + int(y),
                 (unsigned(glyph->pixels[size_t(y) * glyph->width + x]) * 3 + 127) / 255);
  }
  std::vector<const uint8_t*> warmPointers(requests.size());
  uint64_t pixelHash = 0;
  for (unsigned pass = 0; pass < 3; ++pass) {
    std::fill(actualPixels.begin(), actualPixels.end(), 0);
    size_t changedPointers = 0;
    for (size_t i = 0; i < requests.size(); ++i) {
      const auto& request = requests[i];
      // Ordinary metrics and the final phase share cache capacity. Match the
      // reader's repeated coverage/metrics lookups before the raster request.
      for (unsigned lookup = 0; lookup < 4; ++lookup)
        assert(data->dynamicGlyphHandler(data->glyphMissCtx, request.codepoint));
      EpdGlyph glyph{};
      const uint8_t* packed = nullptr;
      assert(data->rasterHandler(data->glyphMissCtx, request.codepoint, request.phase, glyph, packed));
      assert(packed && glyph.width && glyph.height);
      if (pass == 1) warmPointers[i] = packed;
      if (pass == 2 && warmPointers[i] != packed) ++changedPointers;
      for (unsigned y = 0; y < glyph.height; ++y)
        for (unsigned x = 0; x < glyph.width; ++x) {
          const size_t pixel = size_t(y) * glyph.width + x;
          putPixel(actualPixels, request.x + glyph.left + int(x), request.y - glyph.top + int(y),
                   (packed[pixel / 4] >> (6 - 2 * (pixel % 4))) & 3);
        }
    }
    assert(actualPixels == referencePixels);
    pixelHash = 1469598103934665603ull;
    for (const auto value : actualPixels) pixelHash = (pixelHash ^ value) * 1099511628211ull;
    std::printf("PHASE CACHE pass=%u glyphs=%zu changedWarmPointers=%zu pixelHash=%llu\n", pass, requests.size(),
                changedPointers, static_cast<unsigned long long>(pixelHash));
    assert(changedPointers == 0);
  }
}
