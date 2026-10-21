#include <OutlineFontFace.h>

#include <iostream>

#include "OutlineRenderProbe.h"

namespace {
struct Face {
  OutlineFontFace outline;
  EpdFontData data{};
  EpdFont font{&data};
  EpdFontFamily family{&font};
  Face(const std::vector<uint8_t>& bytes, const OutlineFontFace::Options& options) {
    assert(outline.openMemory(bytes.data(), bytes.size(), options));
    const auto m = outline.lineMetrics();
    data.advanceY = m.advanceY;
    data.ascender = m.ascender;
    data.descender = m.descender;
    data.is2Bit = true;
    data.outline = &OutlineFontFace::callbacks();
    data.outlineCtx = &outline;
  }
};
const auto allCaps = static_cast<EpdFontFamily::Style>(EpdFontFamily::SMALL_CAPS | EpdFontFamily::ALL_SMALL_CAPS);
}  // namespace

int runOutlineQualityChecks(GfxRenderer& g, HalDisplay& d, const char* path) {
  using namespace outlineProbe;
  const auto bytes = readFont(path);
  int checks = 0, differences = 0, scenarios = 0;
  const char* samples[] = {"still ill minimum HNT", "jfj AVATAR To 012345", "a\xcc\x81 i\xcc\x88 j\xcc\xa7"};
  for (auto policy : {OutlineFontFace::RasterPolicy::Oversample2x, OutlineFontFace::RasterPolicy::FinalSize})
    for (int phases : {4, 8, 16}) {
      OutlineFontFace::Options options;
      options.logicalPx = 10 * 150.0f / 72.0f;
      options.opticalPoints = 10;
      options.rasterPolicy = policy;
      options.phaseSteps = phases;
      Face face(bytes, options);
      for (float scale : {.4f, .5f, .675f}) {
        g.insertScaledFont(81, face.family, scale);
        for (auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted, GfxRenderer::LandscapeClockwise,
                                 GfxRenderer::LandscapeCounterClockwise}) {
          g.setOrientation(orientation);
          for (auto style : {EpdFontFamily::REGULAR, EpdFontFamily::SMALL_CAPS, allCaps}) {
            const int scenario = scenarios++;
            const int tracking = scenario % 2 ? -3 : 3;
            const unsigned darkness = scenario % 5;
            const char* sample = samples[(scenario + scenario / 3) % 3];
            g.setTextDarkness(darkness);
            for (int edge = 0; edge < 3; ++edge) {
              const int x = edge == 0 ? 24 : edge == 1 ? -7 : g.getScreenWidth() - 20;
              const int y = edge == 0 ? 40 : edge == 1 ? -5 : g.getScreenHeight() - 20;
              const auto paint = [&] { g.drawTextSpaced(81, x, y, sample, true, style, 1, tracking, false); };
              const auto cold = capture(g, d, paint);
              const auto expected = reference(face.family, sample, style, scale, tracking, x, y, darkness, orientation);
              if (!(cold == expected.frame)) {
                std::cerr << "outline pixel oracle failed policy=" << int(policy) << " phase=" << phases
                          << " scale=" << scale << " orientation=" << int(orientation) << " style=" << int(style)
                          << " edge=" << edge << " text=" << sample << '\n';
                assert(false);
              }
              assert(cold == capture(g, d, paint));
              assert(cold == capture(g, d, paint, true));
              const auto actualAdvance = g.getTextAdvanceXSpaced(81, sample, style, 1, tracking, false);
              if (actualAdvance != expected.advance) {
                std::cerr << "advance oracle failed policy=" << int(policy) << " phases=" << phases
                          << " scale=" << scale << " style=" << int(style) << " tracking=" << tracking
                          << " text=" << sample << " actual=" << actualAdvance << " expected=" << expected.advance
                          << " allowed=" << expected.minimumAdvance << ".." << expected.maximumAdvance << '\n';
              }
              assert(actualAdvance == expected.advance);
              assert(g.getTextWidthSpaced(81, sample, style, 1, tracking, false) >= expected.right - expected.left);
              ++checks;
            }
            // Ink metrics have no tracking argument, so check their own untracked run.
            int above, below;
            const auto bounds = reference(face.family, sample, style, scale, 0, 0, 0, 0, orientation);
            assert(g.getTextInkMetrics(81, sample, style, &above, &below));
            assert(above >= -bounds.top && below >= bounds.bottom);
          }
        }
        g.removeFont(81);
      }
      g.setOrientation(GfxRenderer::Portrait);
      g.setTextDarkness(0);
      g.insertScaledFont(81, face.family, .5f);
      auto paint = [&] {
        g.drawTextSpaced(81, 20, 20, "jfj HNT still AVATAR", true, EpdFontFamily::REGULAR, 1, 3, false);
      };
      const auto before = capture(g, d, paint);
      for (int n = 0; n < 900; ++n) {
        const auto gid = face.outline.glyphId(33 + n % 90);
        EpdRasterGlyph raster;
        if (gid) assert(face.outline.raster(gid, .35f + .05f * (n / 90), int8_t((n % 16) - 8), &raster));
      }
      assert(before == capture(g, d, paint));  // eviction/cold bitmap parity
      g.removeFont(81);
      Face reopened(bytes, options);
      g.insertScaledFont(81, reopened.family, .5f);
      assert(before == capture(g, d, paint));
      g.removeFont(81);
      checks += 2;
    }

  // Comparison pages: same source face, optical setting, pen advances, kerning,
  // strings, and baseline in all modes. Only raster policy/phase count changes.
  std::ofstream summary("outline-quality-metrics.csv");
  summary << "points,phases,policy,sample,advance\n";
  for (int points : {8, 10, 12})
    for (int phases : {4, 8}) {
      std::vector<int> advances;
      Frame legacy;
      for (auto policy : {OutlineFontFace::RasterPolicy::LegacyBitmap, OutlineFontFace::RasterPolicy::Oversample2x,
                          OutlineFontFace::RasterPolicy::FinalSize}) {
        OutlineFontFace::Options options;
        options.logicalPx = points * 150.0f / 72.0f;
        options.opticalPoints = points;
        options.rasterPolicy = policy;
        options.phaseSteps = phases;
        Face face(bytes, options);
        g.insertScaledFont(81, face.family, .5f);
        const std::string mode = policy == OutlineFontFace::RasterPolicy::LegacyBitmap   ? "legacy"
                                 : policy == OutlineFontFace::RasterPolicy::Oversample2x ? "oversample"
                                                                                         : "final";
        const auto draw = [&] {
          const std::string title =
              mode + " / " + std::to_string(points) + "pt / " + std::to_string(phases) + " phases";
          g.drawTextScaled(2, 12, 8, title.c_str(), true, EpdFontFamily::REGULAR, .5f);
          int row = 42, index = 0;
          for (auto style : {EpdFontFamily::REGULAR, EpdFontFamily::SMALL_CAPS, allCaps}) {
            for (const char* line :
                 {"still ill minimum HNT", "AVATAR To 0123456789", "jfj \xc3\xa1 \xc3\xa9 \xc3\xaf a\xcc\x81"}) {
              g.drawTextSpaced(81, 16, row, line, true, style, 1, 0, false);
              row += g.getLineHeight(81) + 8;
              ++index;
            }
          }
        };
        const auto image = capture(g, d, draw);
        int index = 0;
        for (auto style : {EpdFontFamily::REGULAR, EpdFontFamily::SMALL_CAPS, allCaps})
          for (const char* line :
               {"still ill minimum HNT", "AVATAR To 0123456789", "jfj \xc3\xa1 \xc3\xa9 \xc3\xaf a\xcc\x81"}) {
            const int advance = g.getTextAdvanceXSpaced(81, line, style, 1, 0, false);
            if (policy == OutlineFontFace::RasterPolicy::LegacyBitmap)
              advances.push_back(advance);
            else
              assert(advance == advances[index]);
            summary << points << ',' << phases << ',' << mode << ',' << index++ << ',' << advance << '\n';
          }
        save("outline-quality-" + mode + "-p" + std::to_string(points) + "-q" + std::to_string(phases) + ".pgm", image);
        if (policy == OutlineFontFace::RasterPolicy::LegacyBitmap)
          legacy = image;
        else {
          bool changed = false;
          for (int y = 40; y < 700; ++y)
            for (int x = 0; x < 480; ++x) {
              int at;
              uint8_t mask;
              assert(physical(x, y, GfxRenderer::Portrait, at, mask));
              changed |= ink(image, at, mask) != ink(legacy, at, mask);
            }
          if (changed) ++differences;
        }
        g.removeFont(81);
      }
    }
  assert(differences == 12);
  g.setOrientation(GfxRenderer::Portrait);
  g.setRenderMode(GfxRenderer::BW);
  g.setTextDarkness(0);
  std::cout << "outline final-pixel/phase/capture/staged/orientation/clip/metrics/cache cases=" << checks
            << " passed\n";
  return 0;
}
