#include <Arduino.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/BookSearchSession.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "Utf8Case.h"

namespace {
std::shared_ptr<Epub> navigationBook(const std::string& tag) {
  const auto cache = std::filesystem::temp_directory_path() / "witch-search-navigation" / tag;
  std::filesystem::remove_all(cache);
  auto epub = std::make_shared<Epub>(std::string(CORPUS_DIR) + "/test_epub_features.epub", cache.string());
  EXPECT_TRUE(epub->load(true));
  epub->setupCacheDir();
  return epub;
}
std::vector<BookSearchSession::Hit> bookMatches(const std::shared_ptr<Epub>& epub, const char* query) {
  BookSearchSession session(epub, query);
  EXPECT_TRUE(session.begin());
  for (int i = 0; !session.done() && i < 10000; ++i) session.step(512);
  EXPECT_TRUE(session.done());
  EXPECT_FALSE(session.failed());
  EXPECT_EQ(session.failedSections(), 0);
  std::vector<BookSearchSession::Hit> hits(session.count());
  for (uint32_t i = 0; i < session.count(); ++i) EXPECT_TRUE(session.readHit(i, hits[i]));
  return hits;
}
Section::BuildParams navigationParams(int width, int height) {
  Section::BuildParams params;
  params.fontId = 1;
  params.viewportWidth = width;
  params.viewportHeight = height;
  params.embeddedStyle = true;
  params.fontSizeNormalization = false;
  params.inlineFootnotePreviews = false;
  return params;
}
std::string pageWords(Section& section, uint16_t pageNumber) {
  section.currentPage = pageNumber;
  const auto page =
      section.hasActiveBuild() ? section.loadPageFromActiveBuild(pageNumber) : section.loadPageFromSectionFile();
  EXPECT_NE(page, nullptr);
  std::string text;
  if (!page) return text;
  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* block = static_cast<const PageLine*>(element.get())->getBlock();
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      if (!text.empty() && !block->wordContinues(i)) text += ' ';
      text += block->wordText(i);
    }
  }
  return utf8CaseMap(text, true);
}
}  // namespace

TEST(BookSearchNavigation, OriginalFeatureFixtureQueriesLandOnMatchingRenderedText) {
  struct Query {
    const char* phrase;
    const char* firstWord;
    size_t matches;
  };
  static constexpr Query queries[] = {{"silver lantern", "SILVER", 3}, {"copper & stone", "COPPER", 1},
                                      {"CAFÉ STRASSE", "CAFÉ", 1},     {"interleaved", "INTERLEAVED", 1},
                                      {"softhyphen", "SOFTHYPHEN", 1}, {"rain returns", "RAIN", 1}};
  const auto epub = navigationBook("fixture-phrases");
  GfxRenderer renderer;
  for (const auto& query : queries) {
    const auto hits = bookMatches(epub, query.phrase);
    ASSERT_EQ(hits.size(), query.matches) << query.phrase;
    for (const auto& hit : hits) {
      EXPECT_EQ(hit.spine, 6) << query.phrase;
      for (const auto width : {460, 230}) {
        const auto params = navigationParams(width, 240);
        Section section(epub, hit.spine, renderer);
        section.setSourceLookupTarget({hit.sourceOffset, 0});
        ASSERT_TRUE(section.createSectionFile(params, {}, true));
        ASSERT_TRUE(section.loadSectionFile(params));
        const auto target = section.sourceLookupPage();
        ASSERT_TRUE(target) << query.phrase << " source=" << hit.sourceOffset << " width=" << width;
        ASSERT_LT(*target, section.pageCount);
        EXPECT_NE(pageWords(section, *target).find(query.firstWord), std::string::npos)
            << query.phrase << " source=" << hit.sourceOffset << " width=" << width;
        // The warmed-cache path may decline an ambiguous range, but must never
        // confidently select a different page from the exact streaming probe.
        Section cached(epub, hit.spine, renderer);
        ASSERT_TRUE(cached.loadSectionFile(params));
        if (const auto cachedPage = cached.getPageForSourceOffset({hit.sourceOffset, 0}))
          EXPECT_EQ(*cachedPage, *target);
      }
    }
  }
}

TEST(BookSearchNavigation, MidParagraphMatchesSurviveRepeatedLayoutChangesInDirectAndNestedText) {
  const auto epub = navigationBook("long-paragraph");
  const auto hits = bookMatches(epub, "Marker 0048");
  ASSERT_EQ(hits.size(), 2u);
  GfxRenderer renderer;
  static constexpr int dimensions[][2] = {{460, 760}, {230, 360}, {340, 480}, {460, 760}};
  for (const auto& hit : hits) {
    ASSERT_TRUE(hit.spine == 7 || hit.spine == 8);
    std::optional<uint16_t> initialPage;
    for (const auto& dim : dimensions) {
      const auto params = navigationParams(dim[0], dim[1]);
      Section section(epub, hit.spine, renderer);
      // Keep the original selected passage, as the reader does across consecutive
      // resizes. Recapturing each page's earlier first word would drift backwards.
      section.setSourceLookupTarget({hit.sourceOffset, 0});
      ASSERT_TRUE(section.createSectionFile(params, {}, true));
      ASSERT_TRUE(section.loadSectionFile(params));
      const auto target = section.sourceLookupPage();
      ASSERT_TRUE(target);
      EXPECT_GT(*target, 0u);
      EXPECT_LT(*target, section.pageCount - 1);
      EXPECT_NE(pageWords(section, *target).find("MARKER 0048"), std::string::npos)
          << "spine=" << hit.spine << " width=" << dim[0] << " height=" << dim[1];
      // Independently find the displayed marker: a valid source lookup must name
      // that exact rendered page, not merely a similar fraction of the chapter.
      int markerPage = -1;
      for (uint16_t page = 0; page < section.pageCount; ++page) {
        if (pageWords(section, page).find("MARKER 0048") == std::string::npos) continue;
        ASSERT_EQ(markerPage, -1);
        markerPage = page;
      }
      EXPECT_EQ(*target, markerPage);
      if (dim[0] == 460) {
        if (initialPage) EXPECT_EQ(*target, *initialPage);
        initialPage = *target;
      }
    }
  }
}

TEST(BookSearchNavigation, LiveSearchLandingAndAbortedReflowRetainTheSelectedPassage) {
  const auto epub = navigationBook("incremental-passage");
  const auto hits = bookMatches(epub, "Marker 0048");
  ASSERT_EQ(hits.size(), 2u);
  const auto& hit = hits.front();
  GfxRenderer renderer;
  Section section(epub, hit.spine, renderer);
  const SourceAnchor selected{hit.sourceOffset, 0};
  section.setSourceLookupTarget(selected);
  const host_clock::Ticking tick(1);
  bool landedDuringBuild = false;
  for (int i = 0; i < 20000; ++i) {
    const auto step = section.stepSectionBuild(navigationParams(230, 360), 1);
    ASSERT_NE(step, Section::BuildStep::Failed);
    if (section.hasActiveBuild() && section.sourceLookupPage()) {
      landedDuringBuild = true;
      break;
    }
    if (step == Section::BuildStep::Done) break;
  }
  ASSERT_TRUE(landedDuringBuild);
  const auto livePage = section.sourceLookupPage();
  ASSERT_TRUE(livePage);
  EXPECT_NE(pageWords(section, *livePage).find("MARKER 0048"), std::string::npos);
  EXPECT_FALSE(section.getPageForSourceOffset(selected));  // A partial range cannot prove uniqueness.
  section.abortSectionBuild();

  // The same match remains the target when another settings change replaces an
  // unfinished reflow. Exercise the real abort/restart path rather than a page ratio.
  Section resized(epub, hit.spine, renderer);
  resized.setSourceLookupTarget(selected);
  bool finished = false;
  const auto params = navigationParams(340, 480);
  for (int i = 0; i < 20000; ++i) {
    const auto step = resized.stepSectionBuild(params, 1);
    ASSERT_NE(step, Section::BuildStep::Failed);
    if (step == Section::BuildStep::Done) {
      finished = true;
      break;
    }
  }
  ASSERT_TRUE(finished);
  ASSERT_TRUE(resized.loadSectionFile(params));
  ASSERT_TRUE(resized.sourceLookupPage());
  EXPECT_NE(pageWords(resized, *resized.sourceLookupPage()).find("MARKER 0048"), std::string::npos);
}
