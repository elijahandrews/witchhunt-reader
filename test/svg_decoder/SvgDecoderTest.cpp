#include <CooperativeAbort.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include "DirectPixelWriter.h"
#include "PixelCache.h"
#include "SvgRasterizer.h"
#include "SvgToFramebufferConverter.h"

namespace {
class SvgDecoder : public ::testing::Test {
 protected:
  std::filesystem::path directory;
  void SetUp() override {
    static unsigned counter = 0;
    directory = std::filesystem::temp_directory_path() /
                ("svg_decoder_" + std::to_string(getpid()) + "_" + std::to_string(++counter));
    std::filesystem::create_directories(directory);
    CooperativeAbort::setLongTaskAbortPredicate(nullptr);
    CooperativeAbort::clearAborted();
  }
  void TearDown() override {
    CooperativeAbort::setLongTaskAbortPredicate(nullptr);
    CooperativeAbort::clearAborted();
    std::filesystem::remove_all(directory);
  }
  std::string write(const std::string& svg) {
    const auto path = (directory / "source.svg").string();
    std::ofstream(path, std::ios::binary) << svg;
    return path;
  }
  static std::string document(const std::string& content, const std::string& attrs = "width='32' height='32'") {
    return "<svg xmlns='http://www.w3.org/2000/svg' " + attrs + ">" + content + "</svg>";
  }
  static std::string path(const std::string& data, const std::string& attrs = "") {
    return "<path d='" + data + "' " + attrs + "/>";
  }
  bool load(SvgRasterizer& rasterizer, const std::string& svg, int width = 32, int height = 32) {
    FsFile source;
    if (!Storage.openFileForRead("test", write(svg), source)) return false;
    const bool ok = rasterizer.load(source, width, height);
    source.close();
    return ok;
  }
  std::vector<uint8_t> render(const std::string& svg, int width = 32, int height = 32) {
    SvgRasterizer rasterizer;
    if (!load(rasterizer, svg, width, height)) {
      ADD_FAILURE() << "SVG was rejected";
      return {};
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height);
    const bool ok = rasterizer.render(
        [](void* ctx, int y, const uint8_t* row, int width) {
          auto& pixels = *static_cast<std::vector<uint8_t>*>(ctx);
          std::copy_n(row, width, pixels.begin() + y * width);
          return true;
        },
        &pixels);
    EXPECT_TRUE(ok);
    return pixels;
  }
  // Independent analytic coverage reference: sixteen geometric point tests per
  // pixel, rather than the production path flattener or scanline intersections.
  static std::vector<uint8_t> reference(const std::function<bool(double, double)>& inside, int width = 32,
                                        int height = 32) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x) {
        unsigned white = 0;
        for (int sy = 0; sy < 4; ++sy)
          for (int sx = 0; sx < 4; ++sx)
            if (!inside(x + (sx + .5) / 4, y + (sy + .5) / 4)) ++white;
        pixels[y * width + x] = (white * 255 + 8) / 16;
      }
    return pixels;
  }
  static void nearPixels(const std::vector<uint8_t>& actual, const std::vector<uint8_t>& expected,
                         double maxAverageError = 1.2, int maxPixelError = 64) {
    ASSERT_EQ(actual.size(), expected.size());
    double error = 0;
    int maximum = 0;
    for (size_t i = 0; i < actual.size(); ++i) {
      const int d = std::abs(actual[i] - expected[i]);
      error += d;
      maximum = std::max(maximum, d);
    }
    EXPECT_LE(error / actual.size(), maxAverageError);
    EXPECT_LE(maximum, maxPixelError);
  }
  std::vector<uint8_t> cache(const std::string& name, int width, int height) {
    std::ifstream stream(directory / name, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)), {});
    if (bytes.size() != static_cast<size_t>(6 + ((width + 3) / 4) * height)) {
      ADD_FAILURE() << "Bad cache size";
      return {};
    }
    EXPECT_EQ(bytes[0] | bytes[1] << 8, PixelCache::PXC_MAGIC);
    EXPECT_EQ(bytes[2] | bytes[3] << 8, width);
    EXPECT_EQ(bytes[4] | bytes[5] << 8, height);
    std::vector<uint8_t> pixels(width * height);
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x)
        pixels[y * width + x] = (bytes[6 + y * ((width + 3) / 4) + x / 4] >> (6 - (x % 4) * 2)) & 3;
    return pixels;
  }
};

TEST_F(SvgDecoder, IntrinsicDimensionsViewBoxUnitsAndNamespaces) {
  ImageDimensions out{};
  for (const auto& attrs : {"width='24' height='12'", "viewBox='-4 -2 24 12'", "width='18pt' height='9pt'",
                            "width='100%' height='100%' viewBox='0 0 24 12'"}) {
    const auto svg = document("", attrs);
    ASSERT_TRUE(SvgToFramebufferConverter::getDimensionsFromBuffer(reinterpret_cast<const uint8_t*>(svg.data()),
                                                                   svg.size(), out));
    EXPECT_EQ(out.width, 24);
    EXPECT_EQ(out.height, 12);
    SvgToFramebufferConverter decoder;
    ASSERT_TRUE(decoder.getDimensions(write(svg), out));
    EXPECT_EQ(out.width, 24);
    EXPECT_EQ(out.height, 12);
  }
  const std::string prefixed =
      "<?xml version='1.0'?><!-- synthetic --><s:svg xmlns:s='http://www.w3.org/2000/svg' width='32' "
      "height='32'><s:path d='M4 4h24v24H4z'/></s:svg>";
  EXPECT_EQ(render(prefixed), render(document(path("M4 4h24v24H4z"))));
}

TEST_F(SvgDecoder, AbsoluteRelativeRepeatedCommandsAndImplicitClose) {
  const auto expected = reference([](double x, double y) { return x >= 4 && x < 28 && y >= 4 && y < 28; });
  for (const auto& data : {"M4 4L28 4L28 28L4 28Z", "m4 4h24v24h-24z", "M4,4 28,4 28,28 4,28", "M4e0 4e0H2.8e1V28H4"})
    EXPECT_EQ(render(document(path(data))), expected) << data;
}

TEST_F(SvgDecoder, ViewBoxOriginAndAspectRatio) {
  EXPECT_EQ(render(document(path("M-4 -2h24v12h-24z"), "viewBox='-4 -2 24 12'")),
            reference([](double, double y) { return y >= 8 && y < 24; }));
  EXPECT_EQ(render(document(path("M-4 -2h24v12h-24z"), "viewBox='-4 -2 24 12' preserveAspectRatio='none'")),
            std::vector<uint8_t>(1024, 0));
}

TEST_F(SvgDecoder, InheritedEvenOddNonzeroAndReverseWinding) {
  const std::string same = "M2 2H30V30H2Z M10 10H22V22H10Z";
  const std::string reversed = "M2 2H30V30H2Z M10 10V22H22V10Z";
  const auto hole = reference([](double x, double y) {
    return x >= 2 && x < 30 && y >= 2 && y < 30 && !(x >= 10 && x < 22 && y >= 10 && y < 22);
  });
  EXPECT_EQ(render(document("<g fill-rule='evenodd'><g>" + path(same) + "</g></g>")), hole);
  EXPECT_EQ(render(document(path(reversed))), hole);
  EXPECT_EQ(render(document(path(same))),
            reference([](double x, double y) { return x >= 2 && x < 30 && y >= 2 && y < 30; }));
}

TEST_F(SvgDecoder, InheritedFillStylePrecedenceAndWhitePaint) {
  for (const auto& attrs : {"style='fill:#606060' fill='black'", "fill='black' style='fill:#606060'"}) {
    auto pixels = render(document("<g fill='#606060'><g fill='none'>" + path("M0 0h32v32H0z", attrs) + "</g>" +
                                  path("M8 8h16v16H8z", "fill='white'") + "</g>"));
    ASSERT_EQ(pixels.size(), 1024u);
    EXPECT_EQ(pixels[0], 96);
    EXPECT_EQ(pixels[16 * 32 + 16], 255);
  }
  auto inherited =
      render(document("<g fill='#606060'>" + path("M0 0h32v32H0z", "fill='white' style='fill:inherit'") + "</g>"));
  EXPECT_EQ(inherited, std::vector<uint8_t>(1024, 96));
  auto alpha = render(document(path("M0 0h32v32H0z", "fill-opacity='.5'")));
  EXPECT_EQ(alpha, std::vector<uint8_t>(1024, 127));
}

TEST_F(SvgDecoder, QuadraticCurveMatchesAnalyticParabola) {
  // x=4+24t, y=4+48t-48t^2; fill between y=4 and the parabola.
  const auto expected = reference([](double x, double y) {
    const double t = (x - 4) / 24;
    return x >= 4 && x < 28 && y >= 4 && y < 4 + 48 * t * (1 - t);
  });
  nearPixels(render(document(path("M4 4 Q16 28 28 4Z"))), expected);
  EXPECT_EQ(render(document(path("M4 4 Q16 28 28 4Z"))), render(document(path("M4 4 q12 24 24 0z"))));
}

TEST_F(SvgDecoder, CubicCurveMatchesAnalyticReference) {
  // Cubic controls make x linear in t; y=4+72t(1-t).
  nearPixels(render(document(path("M4 4 C12 28 20 28 28 4Z"))), reference([](double x, double y) {
               const double t = (x - 4) / 24;
               return x >= 4 && x < 28 && y >= 4 && y < 4 + 72 * t * (1 - t);
             }));
}

TEST_F(SvgDecoder, SmoothCommandsReflectCorrectControlAndResetAfterLine) {
  EXPECT_EQ(render(document(path("M2 16C6 2 10 2 16 16S26 30 30 16L30 30H2Z"))),
            render(document(path("M2 16C6 2 10 2 16 16C22 30 26 30 30 16L30 30H2Z"))));
  EXPECT_EQ(render(document(path("M2 16Q9 2 16 16T30 16L30 30H2Z"))),
            render(document(path("M2 16Q9 2 16 16Q23 30 30 16L30 30H2Z"))));
  EXPECT_EQ(render(document(path("M2 2L16 16S26 30 30 16Z"))), render(document(path("M2 2L16 16C16 16 26 30 30 16Z"))));
}

TEST_F(SvgDecoder, EllipticalArcsMatchAnalyticCircleAndRotatedEllipse) {
  nearPixels(render(document(path("M28 16A12 12 0 0 1 4 16A12 12 0 0 1 28 16Z"))),
             reference([](double x, double y) { return (x - 16) * (x - 16) + (y - 16) * (y - 16) < 144; }));
  nearPixels(render(document(path("M16 28A12 6 90 0 1 16 4A12 6 90 0 1 16 28Z"))),
             reference([](double x, double y) { return (x - 16) * (x - 16) / 36 + (y - 16) * (y - 16) / 144 < 1; }));
  EXPECT_EQ(render(document(path("M28 16A12 12 0 01 4 16a12 12 0 01 24 0Z"))),
            render(document(path("M28 16A12 12 0 0 1 4 16A12 12 0 0 1 28 16Z"))));
}

TEST_F(SvgDecoder, ArcSweepLargeArcAndRadiiCorrection) {
  nearPixels(render(document(path("M28 16A2 2 0 0 1 4 16Z"))),
             reference([](double x, double y) { return y >= 16 && (x - 16) * (x - 16) + (y - 16) * (y - 16) < 144; }));
  nearPixels(render(document(path("M28 16A12 12 0 0 0 4 16Z"))),
             reference([](double x, double y) { return y < 16 && (x - 16) * (x - 16) + (y - 16) * (y - 16) < 144; }));
  const auto small = render(document(path("M28 16A12 12 0 0 1 16 28L16 16Z")));
  const auto large = render(document(path("M28 16A12 12 0 1 0 16 28L16 16Z")));
  ASSERT_EQ(small.size(), 1024u);
  ASSERT_EQ(large.size(), 1024u);
  EXPECT_EQ(small[20 * 32 + 20], 0);
  EXPECT_EQ(small[10 * 32 + 10], 255);
  EXPECT_EQ(large[10 * 32 + 10], 0);
  EXPECT_EQ(large[20 * 32 + 20], 255);
  EXPECT_EQ(render(document(path("M4 4A0 4 0 0 1 28 4L28 28H4Z"))), render(document(path("M4 4H28V28H4Z"))));
}

TEST_F(SvgDecoder, LongPathsExceedOldSaxAttributeLimit) {
  std::string data = "M2 2";
  for (int i = 0; i < 100; ++i) data += "L30 2L30 30L2 30L2 2";
  ASSERT_GT(data.size(), 384u);
  EXPECT_EQ(render(document(path(data))),
            reference([](double x, double y) { return x >= 2 && x < 30 && y >= 2 && y < 30; }));
}

TEST_F(SvgDecoder, MalformedAndUnsupportedInputsRejectBeforeDrawing) {
  const std::vector<std::string> invalid = {"<svg width='32' height='32'><path d='M0 0L32 0L32 32Z'></svg>",
                                            document(path("M0 0L")),
                                            document(path("L0 0")),
                                            document(path("M0 0Z1 2")),
                                            document(path("Mnan 0Z")),
                                            document(path("M1e 0Z")),
                                            document(path("M0 0A2 2 0 2 0 4 4Z")),
                                            document(path("M0 0R1 2Z")),
                                            document(path("M0 0H32V32H0Z", "transform='translate(1)'")),
                                            document(path("M0 0H32V32H0Z", "fill='url(#gradient)'")),
                                            document("<text>unsupported</text>"),
                                            document(path("M0 0H32V32H0Z", "stroke='black'")),
                                            document("<use href='#shape'/>"),
                                            document("", "width='0' height='32'"),
                                            document("", "viewBox='0 0 -32 32'"),
                                            document("", "width='9999999' height='1'")};
  SvgToFramebufferConverter decoder;
  GfxRenderer renderer;
  RenderConfig config{0, 0, 32, 32};
  config.cachePath = (directory / "invalid.pxc").string();
  for (const auto& svg : invalid) {
    EXPECT_FALSE(decoder.decodeToFramebuffer(write(svg), renderer, config)) << svg;
    EXPECT_TRUE(std::all_of(renderer.frame.begin(), renderer.frame.end(), [](uint8_t b) { return b == 255; }));
    EXPECT_FALSE(std::filesystem::exists(config.cachePath));
    EXPECT_FALSE(std::filesystem::exists(config.cachePath + ".part"));
  }
}

TEST_F(SvgDecoder, ResourceBoundsAndAbortLatch) {
  SvgRasterizer rasterizer;
  EXPECT_FALSE(load(rasterizer, document(path("M0 0" + std::string(SvgRasterizer::MAX_PATH_BYTES, ' ')))));
  EXPECT_FALSE(load(rasterizer, document(std::string(SvgRasterizer::MAX_SOURCE_BYTES, ' '))));
  std::string deep;
  for (int i = 0; i < 20; ++i) deep += "<g>";
  for (int i = 0; i < 20; ++i) deep += "</g>";
  EXPECT_FALSE(load(rasterizer, document(deep)));
  std::string many;
  for (int i = 0; i < 65; ++i) many += path("M0 0H2V2H0Z");
  EXPECT_FALSE(load(rasterizer, document(many)));
  EXPECT_FALSE(load(rasterizer, document(""), 2049, 1));
  EXPECT_FALSE(load(rasterizer, document(""), 2048, 2048));
  CooperativeAbort::setLongTaskAbortPredicate([]() { return true; });
  EXPECT_FALSE(load(rasterizer, document(path("M0 0H32V32H0Z"))));
  EXPECT_TRUE(CooperativeAbort::consumeAborted());
  CooperativeAbort::setLongTaskAbortPredicate(nullptr);
  ASSERT_TRUE(load(rasterizer, document(path("M0 0H32V32H0Z"))));
  unsigned rows = 0;
  EXPECT_FALSE(
      rasterizer.render([](void* p, int, const uint8_t*, int) { return ++*static_cast<unsigned*>(p) < 3; }, &rows));
  EXPECT_EQ(rows, 3u);
  EXPECT_FALSE(CooperativeAbort::wasAborted());
}

TEST_F(SvgDecoder, GrayAndMonochromeCacheBandsIncludeClippedPixels) {
  SvgToFramebufferConverter decoder;
  GfxRenderer renderer;
  const auto source = write(document(path("M0 0H32V32H0Z", "fill='#606060'")));
  RenderConfig config{-8, -3, 32, 32};
  config.useDithering = false;
  config.cachePath = (directory / "gray.pxc").string();
  config.companionCachePath = (directory / "bw.pxc").string();
  ASSERT_TRUE(decoder.decodeToFramebuffer(source, renderer, config));
  EXPECT_EQ(cache("gray.pxc", 32, 32), std::vector<uint8_t>(1024, 1));
  EXPECT_EQ(cache("bw.pxc", 32, 32), std::vector<uint8_t>(1024, 0));
  EXPECT_FALSE(std::filesystem::exists(config.cachePath + ".part"));
  config.monochromeOutput = true;
  config.useDithering = true;
  config.cachePath = (directory / "dither-bw.pxc").string();
  config.companionCachePath = (directory / "dither-gray.pxc").string();
  ASSERT_TRUE(decoder.decodeToFramebuffer(source, renderer, config));
  const auto bw = cache("dither-bw.pxc", 32, 32);
  EXPECT_GT(std::count(bw.begin(), bw.end(), 0), 0);
  EXPECT_GT(std::count(bw.begin(), bw.end(), 3), 0);
  EXPECT_TRUE(std::all_of(bw.begin(), bw.end(), [](uint8_t v) { return v == 0 || v == 3; }));
  config.monochromeOutput = false;
  config.cachePath = (directory / "other-gray.pxc").string();
  config.companionCachePath = (directory / "other-bw.pxc").string();
  ASSERT_TRUE(decoder.decodeToFramebuffer(source, renderer, config));
  EXPECT_EQ(cache("other-bw.pxc", 32, 32), bw);
  EXPECT_EQ(cache("other-gray.pxc", 32, 32), cache("dither-gray.pxc", 32, 32));
}

TEST_F(SvgDecoder, OrientationPlanesAndNativeGray8MatchGeometry) {
  SvgToFramebufferConverter decoder;
  const auto source = write(document(path("M4 6H20V24H4Z", "fill='#606060'")));
  for (int o = 0; o < 4; ++o)
    for (int m = 0; m < 3; ++m) {
      GfxRenderer renderer;
      renderer.orientation = static_cast<GfxRenderer::Orientation>(o);
      renderer.mode = static_cast<GfxRenderer::RenderMode>(m);
      renderer.frame.fill(m == 0 ? 255 : 0);
      RenderConfig config{3, 2, 32, 32};
      config.useDithering = false;
      ASSERT_TRUE(decoder.decodeToFramebuffer(source, renderer, config));
      for (int y = 0; y < renderer.getScreenHeight(); ++y)
        for (int x = 0; x < renderer.getScreenWidth(); ++x) {
          const bool inside = x >= 7 && x < 23 && y >= 8 && y < 26;
          int px = x, py = y;
          if (o == 0) {
            px = y;
            py = 47 - x;
          } else if (o == 1) {
            px = 63 - x;
            py = 47 - y;
          } else if (o == 2) {
            px = 63 - y;
            py = x;
          }
          const bool bit = (renderer.frame[py * 8 + px / 8] >> (7 - px % 8)) & 1;
          EXPECT_EQ(bit, m == 0 ? !inside : inside) << o << "," << m << "," << x << "," << y;
        }
      std::vector<uint8_t> gray8(64 * 48, 255);
      DirectGray8Writer writer;
      writer.init(renderer, gray8.data(), 64);
      const auto before = renderer.frame;
      config.gray8 = &writer;
      config.cachePath = (directory / "forbidden.pxc").string();
      ASSERT_TRUE(decoder.decodeToFramebuffer(source, renderer, config));
      EXPECT_EQ(renderer.frame, before);
      EXPECT_FALSE(std::filesystem::exists(config.cachePath));
      EXPECT_EQ(std::count(gray8.begin(), gray8.end(), 96), 16 * 18);
    }
}

TEST_F(SvgDecoder, InterruptedDecodeDoesNotPublishCache) {
  SvgToFramebufferConverter decoder;
  GfxRenderer renderer;
  const auto source = write(document(path("M0 0H32V32H0Z")));
  RenderConfig config{0, 0, 32, 32};
  config.cachePath = (directory / "aborted.pxc").string();
  static unsigned polls;
  polls = 0;
  CooperativeAbort::setLongTaskAbortPredicate([]() { return ++polls > 12; });
  EXPECT_FALSE(decoder.decodeToFramebuffer(source, renderer, config));
  EXPECT_TRUE(CooperativeAbort::consumeAborted());
  EXPECT_FALSE(std::filesystem::exists(config.cachePath));
  EXPECT_FALSE(std::filesystem::exists(config.cachePath + ".part"));
  CooperativeAbort::setLongTaskAbortPredicate(nullptr);
  EXPECT_TRUE(decoder.decodeToFramebuffer(source, renderer, config));
  EXPECT_TRUE(std::filesystem::exists(config.cachePath));
}
}  // namespace
