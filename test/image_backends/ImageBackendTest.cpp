#include <GfxRenderer.h>
#include <ImageFormatDetector.h>
#include <SvgRasterizer.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <climits>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <string>
#include <vector>

#include "ImageDecoderFactory.h"
#include "OriginalRasterFixtures.h"
#include "SvgToFramebufferConverter.h"

// Production makeUniqueNoThrow calls retain their real allocation path; only
// explicitly selected nothrow allocations fail, without changing std containers.
static int failAllocation = -1;
void* operator new(size_t n, const std::nothrow_t&) noexcept {
  if (failAllocation >= 0 && failAllocation-- == 0) return nullptr;
  try {
    return ::operator new(n);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](size_t n, const std::nothrow_t&) noexcept {
  if (failAllocation >= 0 && failAllocation-- == 0) return nullptr;
  try {
    return ::operator new[](n);
  } catch (...) {
    return nullptr;
  }
}

namespace {
using Format = ImageFormatDetector::Format;
const std::string svg = "<svg width='32' height='32'><path fill='#606060' d='M4 6H20V24H4Z'/></svg>";
std::vector<uint8_t> fromHex(const std::string& hex) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i < hex.size(); i += 2) out.push_back(std::stoul(hex.substr(i, 2), nullptr, 16));
  return out;
}
std::vector<uint8_t> bytes(const std::string& text) { return {text.begin(), text.end()}; }
std::vector<uint8_t> readFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {(std::istreambuf_iterator<char>(file)), {}};
}
class ImageBackend : public testing::Test {
 protected:
  std::filesystem::path directory;
  void SetUp() override {
    static size_t next = 0;
    directory = std::filesystem::temp_directory_path() /
                ("crossink-images-" + std::to_string(getpid()) + "-" + std::to_string(++next));
    std::filesystem::create_directories(directory);
    ImageStorageTest::reset();
    failAllocation = -1;
  }
  void TearDown() override {
    failAllocation = -1;
    EXPECT_TRUE(ImageStorageTest::readers.empty());
    EXPECT_EQ(ImageStorageTest::duplicateReads, 0u);
    std::filesystem::remove_all(directory);
  }
  std::string write(const std::string& name, const std::vector<uint8_t>& data) {
    const auto path = (directory / name).string();
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), data.size());
    return path;
  }
};

TEST_F(ImageBackend, StrictMagicAndBoundedSvgRootDetection) {
  for (const auto& [hex, format] : std::vector<std::pair<const char*, Format>>{
           {PNG_HEX, Format::Png}, {JPEG_HEX, Format::Jpeg}, {GIF_HEX, Format::Gif}}) {
    auto data = fromHex(hex);
    EXPECT_EQ(ImageFormatDetector::detect(data.data(), data.size()), format);
  }
  for (const auto& prefix : {std::string{}, std::string("\xef\xbb\xbf<?xml version='1.0'?>\n"),
                             std::string("<!--") + std::string(4000, 'x') + "-->"}) {
    auto data = bytes(prefix + "<s:svg xmlns:s='http://www.w3.org/2000/svg' width='1' height='1'/> ");
    EXPECT_EQ(ImageFormatDetector::detect(data.data(), data.size()), Format::Svg);
    EXPECT_EQ(ImageFormatDetector::detectPath(write("no-extension", data)), Format::Svg);
  }
  for (const auto& text :
       {"GIF00x", "not <svg/>", "<!-- <svg/> -->", "<svgish/>", "<!DOCTYPE svg><svg/>", "<html><svg/></html>"}) {
    const auto data = bytes(text);
    EXPECT_EQ(ImageFormatDetector::detect(data.data(), data.size()), Format::Unknown);
  }
  auto longPrefix = bytes("<!--" + std::string(ImageFormatDetector::kMaxSvgPrefixBytes, 'x') + "--><svg/>");
  EXPECT_EQ(ImageFormatDetector::detect(longPrefix.data(), longPrefix.size()), Format::Unknown);
  auto png = fromHex(PNG_HEX);
  png[7] = 0;
  EXPECT_EQ(ImageFormatDetector::detect(png.data(), png.size()), Format::Unknown);
  EXPECT_EQ(ImageFormatDetector::detect(nullptr, 100), Format::Unknown);
}

TEST_F(ImageBackend, FileProbeRestoresCursorAndClosesBeforeDecoderOpens) {
  auto data = bytes("prefix");
  const auto png = fromHex(PNG_HEX);
  data.insert(data.end(), png.begin(), png.end());
  auto path = write("image.bin", data);
  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("TEST", path, file));
  ASSERT_TRUE(file.seek(3));
  EXPECT_EQ(ImageFormatDetector::detect(file, 6), Format::Png);
  EXPECT_EQ(file.position(), 3u);
  ImageStorageTest::readLimit = 1;
  EXPECT_EQ(ImageFormatDetector::detect(file, 6), Format::Unknown);
  EXPECT_EQ(file.position(), 3u);
  ImageStorageTest::failSeek = true;
  EXPECT_EQ(ImageFormatDetector::detect(file), Format::Unknown);
  ImageStorageTest::failSeek = false;
  file.close();
  ImageStorageTest::reset();
  path = write("image.jpeg", png);
  auto* decoder = ImageDecoderFactory::getDecoder(path);
  ASSERT_NE(decoder, nullptr);
  EXPECT_STREQ(decoder->getFormatName(), "PNG");
  EXPECT_TRUE(ImageStorageTest::readers.empty());
  ImageDimensions dimensions{};
  EXPECT_TRUE(decoder->getDimensions(path, dimensions));
  EXPECT_EQ(dimensions.width, 8);
  EXPECT_EQ(dimensions.height, 8);
  ImageStorageTest::failClose = true;
  EXPECT_EQ(ImageFormatDetector::detectPath(path), Format::Unknown);
}

TEST_F(ImageBackend, RealRasterPixelsAndCachesIgnoreWrongOrMissingExtensions) {
  for (const auto& [hex, name] :
       std::vector<std::pair<const char*, const char*>>{{PNG_HEX, "PNG"}, {JPEG_HEX, "JPEG"}, {GIF_HEX, "GIF"}}) {
    const auto data = fromHex(hex);
    std::array<uint8_t, 64 * 48 / 8> expected;
    expected.fill(255);
    for (int y = 3; y < 11; ++y)
      for (int x = 4; x < 8; ++x) expected[y * 8 + x / 8] &= ~(1 << (7 - x % 8));
    for (const auto* suffix : {"wrong.png", "wrong.jpg", "wrong.gif", "no-extension"}) {
      auto path = write(suffix, data);
      auto* decoder = ImageDecoderFactory::getDecoder(path);
      ASSERT_NE(decoder, nullptr);
      EXPECT_STREQ(decoder->getFormatName(), name);
      GfxRenderer renderer;
      RenderConfig config{4, 3, 8, 8};
      config.useDithering = true;
      config.useExactDimensions = true;
      config.cachePath = (directory / "raster.pxc").string();
      ASSERT_TRUE(decoder->decodeToFramebuffer(path, renderer, config));
      EXPECT_EQ(renderer.frame, expected) << name << '/' << suffix;
      const auto pxc = readFile(config.cachePath);
      ASSERT_EQ(pxc.size(), 20u);
      EXPECT_EQ((std::vector<uint8_t>{pxc[0], pxc[1], pxc[2], pxc[3]}), (std::vector<uint8_t>{8, 0, 8, 0}));
      for (int y = 0; y < 8; ++y) {
        EXPECT_EQ(pxc[4 + y * 2], 0);
        EXPECT_EQ(pxc[5 + y * 2], 255);
      }
    }
    auto* decoder = ImageDecoderFactory::getDecoder(data.data(), data.size());
    ASSERT_NE(decoder, nullptr);
    const auto format = ImageFormatDetector::detect(data.data(), data.size());
    EXPECT_EQ(ImageDecoderFactory::supportsMemorySource(format), format != Format::Gif);
    if (ImageDecoderFactory::supportsMemorySource(format)) {
      GfxRenderer renderer;
      RenderConfig config{4, 3, 8, 8};
      config.sourceData = data.data();
      config.sourceSize = data.size();
      config.useDithering = true;
      config.useExactDimensions = true;
      ASSERT_TRUE(decoder->decodeToFramebuffer("absent-source", renderer, config));
      EXPECT_EQ(renderer.frame, expected);
    }
  }
}

TEST_F(ImageBackend, SvgOrientationPlanesAndClippingPreserveOtherPixels) {
  SvgToFramebufferConverter decoder;
  const auto path = write("shape.svg", bytes(svg));
  for (int orientation = 0; orientation < 4; ++orientation)
    for (int mode = 0; mode < 3; ++mode)
      for (const auto& xy : {std::pair{3, 2}, std::pair{-8, -3}, std::pair{100, 100}}) {
        GfxRenderer renderer;
        renderer.orientation = static_cast<GfxRenderer::Orientation>(orientation);
        renderer.mode = static_cast<GfxRenderer::RenderMode>(mode);
        renderer.frame.fill(mode == 0 ? 255 : 0);
        RenderConfig config{xy.first, xy.second, 32, 32};
        config.useDithering = false;
        ASSERT_TRUE(decoder.decodeToFramebuffer(path, renderer, config));
        for (int y = 0; y < renderer.getScreenHeight(); ++y)
          for (int x = 0; x < renderer.getScreenWidth(); ++x) {
            const bool inside = x >= xy.first + 4 && x < xy.first + 20 && y >= xy.second + 6 && y < xy.second + 24;
            int px = x, py = y;
            if (orientation == 0) {
              px = y;
              py = 47 - x;
            }
            if (orientation == 1) {
              px = 63 - x;
              py = 47 - y;
            }
            if (orientation == 2) {
              px = 63 - y;
              py = x;
            }
            const bool bit = (renderer.frame[py * 8 + px / 8] >> (7 - px % 8)) & 1;
            EXPECT_EQ(bit, mode == 0 ? !inside : inside);
          }
      }
}

TEST_F(ImageBackend, SvgCompleteNativeCacheSurvivesScreenClippingAndFailedCacheIsRemoved) {
  SvgToFramebufferConverter decoder;
  const auto path =
      write("shape.svg", bytes("<svg width='32' height='32'><path fill='#606060' d='M0 0H32V32H0Z'/></svg>"));
  GfxRenderer renderer;
  RenderConfig config{-8, -3, 32, 32};
  config.useDithering = false;
  config.cachePath = (directory / "svg.pxc").string();
  ASSERT_TRUE(decoder.decodeToFramebuffer(path, renderer, config));
  const auto pxc = readFile(config.cachePath);
  ASSERT_EQ(pxc.size(), 260u);
  EXPECT_TRUE(std::all_of(pxc.begin() + 4, pxc.end(), [](uint8_t b) { return b == 0x55; }));
  ImageStorageTest::writeLimit = 30;
  ASSERT_TRUE(decoder.decodeToFramebuffer(path, renderer, config));
  EXPECT_FALSE(std::filesystem::exists(config.cachePath));
  ImageStorageTest::reset();
  EXPECT_TRUE(decoder.decodeToFramebuffer(path, renderer, config));
  EXPECT_EQ(readFile(config.cachePath), pxc);
  config.cachePath = path;
  EXPECT_FALSE(decoder.decodeToFramebuffer(path, renderer, config));
  EXPECT_EQ(ImageFormatDetector::detectPath(path), Format::Svg);
}

TEST_F(ImageBackend, SvgPhysicalStripMatchesFullFrameAndNightModeRetainsMidtones) {
  SvgToFramebufferConverter decoder;
  const auto path =
      write("midtones.svg", bytes("<svg width='32' height='32'><path fill='#606060' d='M0 0H32V32H0Z'/></svg>"));
  for (int orientation = 0; orientation < 4; ++orientation) {
    for (bool inverted : {false, true}) {
      GfxRenderer full;
      full.orientation = static_cast<GfxRenderer::Orientation>(orientation);
      full.inverted = inverted;
      RenderConfig config{3, 4, 32, 32};
      config.useDithering = false;
      ASSERT_TRUE(decoder.decodeToFramebuffer(path, full, config));
      for (int origin : {0, 7, 30, 43}) {
        GfxRenderer strip;
        strip.orientation = full.orientation;
        strip.inverted = inverted;
        strip.origin = origin;
        strip.rows = 5;
        ASSERT_TRUE(decoder.decodeToFramebuffer(path, strip, config));
        for (int row = 0; row < 5; ++row)
          for (int byte = 0; byte < 8; ++byte)
            EXPECT_EQ(strip.frame[row * 8 + byte], full.frame[(origin + row) * 8 + byte]);
        EXPECT_TRUE(
            std::all_of(strip.frame.begin() + 40, strip.frame.end(), [](uint8_t value) { return value == 255; }));
      }
      if (inverted) {
        EXPECT_TRUE(std::any_of(full.frame.begin(), full.frame.end(),
                                [](uint8_t value) { return value != 0 && value != 255; }));
      }
    }
  }
}

TEST_F(ImageBackend, SvgAllocationAndReadFailuresLeaveFrameUntouched) {
  SvgToFramebufferConverter decoder;
  const auto path = write("shape.svg", bytes(svg));
  RenderConfig config{0, 0, 32, 32};
  config.cachePath = (directory / "svg.pxc").string();
  unsigned rejected = 0;
  for (int allocation = 0; allocation < 20; ++allocation) {
    std::filesystem::remove(config.cachePath);
    GfxRenderer renderer;
    const auto before = renderer.frame;
    failAllocation = allocation;
    const bool ok = decoder.decodeToFramebuffer(path, renderer, config);
    failAllocation = -1;
    if (!ok) {
      ++rejected;
      EXPECT_EQ(renderer.frame, before);
      EXPECT_FALSE(std::filesystem::exists(config.cachePath));
    }
  }
  EXPECT_GE(rejected, 10u);
  for (const auto limit : {size_t{0}, size_t{15}, size_t{svg.size() + 15}}) {
    GfxRenderer renderer;
    const auto before = renderer.frame;
    ImageStorageTest::readLimit = limit;
    EXPECT_FALSE(decoder.decodeToFramebuffer(path, renderer, config));
    EXPECT_EQ(renderer.frame, before);
    ImageStorageTest::reset();
  }
  GfxRenderer renderer;
  config.x = INT_MAX;
  EXPECT_FALSE(decoder.decodeToFramebuffer(path, renderer, config));
}
TEST_F(ImageBackend, CacheOnlyDecodesAllFormatsWithoutTouchingAnyFramebufferPlane) {
  const std::vector<std::pair<std::string, std::vector<uint8_t>>> fixtures = {
      {"png", fromHex(PNG_HEX)}, {"jpeg", fromHex(JPEG_HEX)}, {"gif", fromHex(GIF_HEX)}, {"svg", bytes(svg)}};
  for (const auto& [name, data] : fixtures) {
    SCOPED_TRACE(name);
    const auto path = write(name, data);
    auto* decoder = ImageDecoderFactory::getDecoder(path);
    ASSERT_NE(decoder, nullptr);
    RenderConfig config{5, 7, 32, 32};
    config.useExactDimensions = true;
    config.cachePath = (directory / "reference.pxc").string();
    GfxRenderer reference;
    ASSERT_TRUE(decoder->decodeToFramebuffer(path, reference, config));
    const auto expected = readFile(config.cachePath);
    ASSERT_EQ(expected.size(), 4u + 32u * 8u);
    config.cacheOnly = true;
    // Match dither phase while making the entire target offscreen.
    config.x += 128;
    config.y -= 128;
    config.cachePath = (directory / "warm.pxc").string();
    for (auto orientation : {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise, GfxRenderer::PortraitInverted,
                             GfxRenderer::LandscapeCounterClockwise}) {
      for (auto mode : {GfxRenderer::BW, GfxRenderer::GRAYSCALE_MSB, GfxRenderer::GRAYSCALE_LSB}) {
        GfxRenderer renderer;
        renderer.orientation = orientation;
        renderer.mode = mode;
        renderer.origin = 9;
        renderer.rows = 7;
        renderer.frame.fill(0xa5);
        const auto before = renderer.frame;
        ASSERT_TRUE(decoder->decodeToFramebuffer(path, renderer, config));
        EXPECT_EQ(renderer.frame, before);
        EXPECT_EQ(readFile(config.cachePath), expected);
      }
    }
  }
}

TEST_F(ImageBackend, CacheOnlyCancellationDropsPartialFilesAndAllowsImmediateRetry) {
  const std::vector<std::pair<std::string, std::vector<uint8_t>>> fixtures = {
      {"png", fromHex(PNG_HEX)}, {"jpeg", fromHex(JPEG_HEX)}, {"gif", fromHex(GIF_HEX)}, {"svg", bytes(svg)}};
  for (const auto& [name, data] : fixtures) {
    SCOPED_TRACE(name);
    const auto path = write(name, data);
    auto* decoder = ImageDecoderFactory::getDecoder(path);
    ASSERT_NE(decoder, nullptr);
    RenderConfig config{5, 7, 32, 32};
    config.cacheOnly = config.useExactDimensions = true;
    config.cachePath = (directory / "cancel.pxc").string();
    struct Cancel {
      std::string path;
      unsigned calls = 0;
      bool fired = false;
    } cancel{config.cachePath};
    config.cancelContext = &cancel;
    config.shouldCancel = [](void* context) {
      auto& state = *static_cast<Cancel*>(context);
      if (!state.fired && std::filesystem::exists(state.path) && ++state.calls == 4) {
        state.fired = true;
        return true;  // A one-shot input must still abort the decoder.
      }
      return false;
    };
    bool observed = false;
    config.cancellationObserved = &observed;
    GfxRenderer renderer;
    const auto before = renderer.frame;
    EXPECT_FALSE(decoder->decodeToFramebuffer(path, renderer, config));
    EXPECT_TRUE(cancel.fired);
    EXPECT_TRUE(observed);
    EXPECT_FALSE(std::filesystem::exists(config.cachePath));
    EXPECT_EQ(renderer.frame, before);
    config.shouldCancel = nullptr;
    observed = false;
    EXPECT_TRUE(decoder->decodeToFramebuffer(path, renderer, config));
    EXPECT_EQ(readFile(config.cachePath).size(), 4u + 32u * 8u);
    EXPECT_EQ(renderer.frame, before);
  }
}

TEST_F(ImageBackend, CacheOnlyWriteFailureIsNotSuccessAndDoesNotChangeTheFrame) {
  for (const auto& [name, data] : std::vector<std::pair<std::string, std::vector<uint8_t>>>{
           {"png", fromHex(PNG_HEX)}, {"jpeg", fromHex(JPEG_HEX)}, {"gif", fromHex(GIF_HEX)}, {"svg", bytes(svg)}}) {
    SCOPED_TRACE(name);
    const auto path = write(name, data);
    auto* decoder = ImageDecoderFactory::getDecoder(path);
    ASSERT_NE(decoder, nullptr);
    RenderConfig config{0, 0, 32, 32};
    config.cacheOnly = config.useExactDimensions = true;
    config.cachePath = (directory / "fault.pxc").string();
    for (size_t limit : {0u, 3u, 29u, 250u}) {
      ImageStorageTest::writeLimit = limit;
      GfxRenderer renderer;
      const auto before = renderer.frame;
      EXPECT_FALSE(decoder->decodeToFramebuffer(path, renderer, config));
      EXPECT_FALSE(std::filesystem::exists(config.cachePath));
      EXPECT_EQ(renderer.frame, before);
    }
    ImageStorageTest::writeLimit = SIZE_MAX;
  }
}

TEST_F(ImageBackend, PngCancellationAfterLastRowCannotPublishFinishedCache) {
  const auto path = write("tail.png", fromHex(PNG_HEX));
  auto* decoder = ImageDecoderFactory::getDecoder(path);
  ASSERT_NE(decoder, nullptr);
  RenderConfig config{0, 0, 32, 32};
  config.cacheOnly = config.useExactDimensions = true;
  config.cachePath = (directory / "tail.pxc").string();
  bool observed = false;
  config.cancellationObserved = &observed;
  config.shouldCancel = [](void*) {
    // advanceTo(31) has flushed every preceding row. The last callback has
    // finished; only the final row and cache close remain unpublished.
    return SIZE_MAX - ImageStorageTest::writeLimit >= 4 + 31 * 8;
  };
  GfxRenderer renderer;
  const auto before = renderer.frame;
  EXPECT_FALSE(decoder->decodeToFramebuffer(path, renderer, config));
  EXPECT_TRUE(observed);
  EXPECT_FALSE(std::filesystem::exists(config.cachePath));
  EXPECT_EQ(renderer.frame, before);
}

TEST_F(ImageBackend, PngCancellationAfterOpenClosesItsOwnedSource) {
  const auto path = write("open.png", fromHex(PNG_HEX));
  auto* decoder = ImageDecoderFactory::getDecoder(path);
  ASSERT_NE(decoder, nullptr);
  RenderConfig config{0, 0, 32, 32};
  config.cacheOnly = true;
  config.cachePath = (directory / "open.pxc").string();
  unsigned checks = 0;
  config.cancelContext = &checks;
  config.shouldCancel = [](void* context) { return ++*static_cast<unsigned*>(context) == 2; };
  GfxRenderer renderer;
  EXPECT_FALSE(decoder->decodeToFramebuffer(path, renderer, config));
  EXPECT_TRUE(ImageStorageTest::readers.empty());
  EXPECT_FALSE(std::filesystem::exists(config.cachePath));
}

#ifdef PROGRESSIVE_FIXTURE_DIR
uint8_t cachedLevel(const std::vector<uint8_t>& data, int width, int x, int y) {
  return (data[4 + y * ((width + 3) / 4) + x / 4] >> (6 - (x % 4) * 2)) & 3;
}

TEST_F(ImageBackend, BaselineMemoryAdmissionRejectsProgressiveAndTruncatedMarkers) {
  const auto baseline = readFile(std::filesystem::path(PROGRESSIVE_FIXTURE_DIR) / "contrast_420.jpg");
  const auto progressive = readFile(std::filesystem::path(PROGRESSIVE_FIXTURE_DIR) / "lines.jpg");
  ASSERT_TRUE(ImageFormatDetector::isBaselineJpeg(baseline.data(), baseline.size()));
  ASSERT_FALSE(ImageFormatDetector::isBaselineJpeg(progressive.data(), progressive.size()));
  EXPECT_FALSE(ImageFormatDetector::isBaselineJpeg(nullptr, 100));
  for (size_t size = 0; size < 20; ++size) {
    EXPECT_FALSE(ImageFormatDetector::isBaselineJpeg(baseline.data(), size));
  }
  for (const auto& malformed : std::vector<std::vector<uint8_t>>{
           {0xff, 0xd8, 0xff, 0xe0, 0xff, 0xff}, {0xff, 0xd8, 0xff, 0xe0, 0, 1},
           {0xff, 0xd8, 0xff, 0xda, 0, 2}, {0xff, 0xd8, 0, 0xc0}, {0xff, 0xd8, 0xff, 0xc1}}) {
    EXPECT_FALSE(ImageFormatDetector::isBaselineJpeg(malformed.data(), malformed.size()));
  }
}

TEST_F(ImageBackend, ProgressiveNativeCachePreservesFineAcDetailAndFramebuffer) {
  const auto path = (std::filesystem::path(PROGRESSIVE_FIXTURE_DIR) / "lines.jpg").string();
  auto* decoder = ImageDecoderFactory::getDecoder(path);
  ASSERT_NE(decoder, nullptr);
  RenderConfig config{300, 400, 96, 64};  // Complete cache even entirely off screen.
  config.cacheOnly = true;
  config.useDithering = false;
  config.cachePath = (directory / "full.pxc").string();
  GfxRenderer renderer;
  const auto before = renderer.frame;
  ASSERT_TRUE(decoder->decodeToFramebuffer(path, renderer, config));
  const auto full = readFile(config.cachePath);
  ASSERT_EQ(full.size(), 4 + 24 * 64);
  for (int y = 0; y < 64; ++y) {
    for (int x = 3; x < 96; x += 7) {
      EXPECT_EQ(cachedLevel(full, 96, x, y), 0);
      EXPECT_GE(cachedLevel(full, 96, x - 1, y), 2);
      EXPECT_GE(cachedLevel(full, 96, x + 1, y), 2);
    }
  }
  EXPECT_EQ(renderer.frame, before);
  // The native DC-only decoder is a safe, lower-detail memory-pressure fallback.
  // Deliberately fail just the full decoder's first context allocation.
  failAllocation = 0;
  config.cachePath = (directory / "fallback.pxc").string();
  ASSERT_TRUE(decoder->decodeToFramebuffer(path, renderer, config));
  const auto fallback = readFile(config.cachePath);
  ASSERT_EQ(fallback.size(), full.size());
  EXPECT_NE(full, fallback);
  EXPECT_GE(cachedLevel(fallback, 96, 3, 24), 1);
  EXPECT_EQ(renderer.frame, before);
}

TEST_F(ImageBackend, ProgressiveCacheUsesFullLumaAtAllReducedIdctScales) {
  for (const auto& name : {"prog_full_420", "prog_full_gray", "prog_full_444_rst"}) {
    const auto path = std::filesystem::path(PROGRESSIVE_FIXTURE_DIR) / (std::string(name) + ".jpg");
    auto* decoder = ImageDecoderFactory::getDecoder(path.string());
    ASSERT_NE(decoder, nullptr);
    ImageDimensions dimensions{};
    ASSERT_TRUE(decoder->getDimensions(path.string(), dimensions));
    for (int scale = 0; scale < 4; ++scale) {
      SCOPED_TRACE(std::string(name) + " scale " + std::to_string(scale));
      const int width = dimensions.width >> scale, height = dimensions.height >> scale;
      RenderConfig config{2, 3, width, height};
      config.useExactDimensions = config.cacheOnly = true;
      config.cachePath = (directory / "scaled.pxc").string();
      GfxRenderer renderer;
      const auto before = renderer.frame;
      ASSERT_TRUE(decoder->decodeToFramebuffer(path.string(), renderer, config));
      EXPECT_EQ(readFile(config.cachePath).size(), 4u + ((width + 3) / 4) * height);
      EXPECT_EQ(renderer.frame, before);
    }
  }
}

TEST_F(ImageBackend, ProgressiveCancellationAndCacheFaultsRemainRetryable) {
  const auto path = (std::filesystem::path(PROGRESSIVE_FIXTURE_DIR) / "prog_full_420.jpg").string();
  auto* decoder = ImageDecoderFactory::getDecoder(path);
  ASSERT_NE(decoder, nullptr);
  RenderConfig config{0, 0, 203, 141};
  config.cacheOnly = true;
  config.cachePath = (directory / "progressive.pxc").string();
  GfxRenderer renderer;
  const auto before = renderer.frame;
  for (size_t limit : {0u, 3u, 100u, 2000u}) {
    ImageStorageTest::writeLimit = limit;
    EXPECT_FALSE(decoder->decodeToFramebuffer(path, renderer, config));
    EXPECT_FALSE(std::filesystem::exists(config.cachePath));
    EXPECT_EQ(renderer.frame, before);
  }
  ImageStorageTest::writeLimit = SIZE_MAX;
  bool observed = false;
  config.cancellationObserved = &observed;
  config.shouldCancel = [](void*) { return SIZE_MAX - ImageStorageTest::writeLimit >= 4 + 16 * 51; };
  EXPECT_FALSE(decoder->decodeToFramebuffer(path, renderer, config));
  EXPECT_TRUE(observed);
  EXPECT_FALSE(std::filesystem::exists(config.cachePath));
  EXPECT_EQ(renderer.frame, before);
  observed = false;
  config.shouldCancel = nullptr;
  EXPECT_TRUE(decoder->decodeToFramebuffer(path, renderer, config));
  EXPECT_EQ(readFile(config.cachePath).size(), 4 + 51 * 141);
}

TEST_F(ImageBackend, ProgressiveTruncationAndInputFailuresDoNotPublishOrLeak) {
  const auto original = readFile(std::filesystem::path(PROGRESSIVE_FIXTURE_DIR) / "prog_full_420.jpg");
  const auto path = write("input.jpg", original);
  auto* decoder = ImageDecoderFactory::getDecoder(path);
  ASSERT_NE(decoder, nullptr);
  RenderConfig config{0, 0, 203, 141};
  config.cacheOnly = true;
  config.cachePath = (directory / "incomplete.pxc").string();
  GfxRenderer renderer;
  const auto before = renderer.frame;
  for (size_t limit : {0u, 32u, 512u, 2000u, 5000u}) {
    ImageStorageTest::readLimit = limit;
    EXPECT_FALSE(decoder->decodeToFramebuffer(path, renderer, config));
    EXPECT_FALSE(std::filesystem::exists(config.cachePath));
    EXPECT_TRUE(ImageStorageTest::readers.empty());
    EXPECT_EQ(renderer.frame, before);
  }
  ImageStorageTest::readLimit = SIZE_MAX;
  for (size_t end : {original.size() / 4, original.size() / 2, original.size() * 3 / 4}) {
    write("input.jpg", {original.begin(), original.begin() + end});
    EXPECT_FALSE(decoder->decodeToFramebuffer(path, renderer, config));
    EXPECT_FALSE(std::filesystem::exists(config.cachePath));
    EXPECT_TRUE(ImageStorageTest::readers.empty());
    EXPECT_EQ(renderer.frame, before);
  }
  write("input.jpg", original);
  ASSERT_TRUE(decoder->decodeToFramebuffer(path, renderer, config));
  EXPECT_EQ(readFile(config.cachePath).size(), 4 + 51 * 141);
}

TEST_F(ImageBackend, ProgressiveSeekFailureAfterIndexedBandsCannotBecomePaddedSuccess) {
  const auto path = (std::filesystem::path(PROGRESSIVE_FIXTURE_DIR) / "prog_full_420.jpg").string();
  auto* decoder = ImageDecoderFactory::getDecoder(path);
  ASSERT_NE(decoder, nullptr);
  RenderConfig config{0, 0, 203, 141};
  config.cacheOnly = true;
  config.cachePath = (directory / "seek-failure.pxc").string();
  config.shouldCancel = [](void*) {
    // Indexing finished before the first output band. Fail only a later
    // random-access entropy read, not the header or whole-file index pass.
    if (SIZE_MAX - ImageStorageTest::writeLimit >= 4 + 16 * 51) ImageStorageTest::failSeek = true;
    return false;
  };
  GfxRenderer renderer;
  const auto before = renderer.frame;
  EXPECT_FALSE(decoder->decodeToFramebuffer(path, renderer, config));
  EXPECT_TRUE(ImageStorageTest::failSeek);
  EXPECT_FALSE(std::filesystem::exists(config.cachePath));
  EXPECT_EQ(renderer.frame, before);
  config.shouldCancel = nullptr;
  ImageStorageTest::failSeek = false;
  EXPECT_TRUE(decoder->decodeToFramebuffer(path, renderer, config));
}

TEST_F(ImageBackend, ProgressiveOneShotSeekFailureCannotPublishPartialEntropy) {
  for (const auto* name : {"lines.jpg", "prog_full_gray.jpg", "prog_full_420.jpg"}) {
    const auto path = (std::filesystem::path(PROGRESSIVE_FIXTURE_DIR) / name).string();
    auto* decoder = ImageDecoderFactory::getDecoder(path);
    ASSERT_NE(decoder, nullptr);
    for (size_t written : {5u, 300u, 600u}) {
      SCOPED_TRACE(std::string(name) + ":" + std::to_string(written));
      ImageStorageTest::writeLimit = SIZE_MAX;
      RenderConfig config{0, 0, 203, 141};
      config.cacheOnly = true;
      config.cachePath = (directory / "seek-once.pxc").string();
      struct Fault { size_t written; bool fired = false; } fault{written};
      config.cancelContext = &fault;
      config.shouldCancel = [](void* context) {
        auto& fault = *static_cast<Fault*>(context);
        if (!fault.fired && SIZE_MAX - ImageStorageTest::writeLimit >= fault.written) {
          fault.fired = true;
          ImageStorageTest::failSeekOnce = true;
        }
        return false;
      };
      GfxRenderer renderer;
      EXPECT_FALSE(decoder->decodeToFramebuffer(path, renderer, config));
      EXPECT_TRUE(fault.fired);
      EXPECT_FALSE(ImageStorageTest::failSeekOnce);
      EXPECT_FALSE(std::filesystem::exists(config.cachePath));
    }
  }
}
#endif
}  // namespace
