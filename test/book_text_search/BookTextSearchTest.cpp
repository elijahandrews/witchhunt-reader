#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Epub/BookTextSearch.h"

namespace {
struct Match {
  uint32_t offset;
  std::string snippet;
};
bool collect(void* context, uint32_t offset, const char* snippet) {
  static_cast<std::vector<Match>*>(context)->push_back({offset, snippet});
  return true;
}
std::vector<Match> findMatches(std::string_view query, const std::string& html, size_t chunk = 1024) {
  std::vector<Match> matches;
  auto matcher = std::make_unique<BookTextSearch>(query, collect, &matches);
  EXPECT_TRUE(matcher->beginChapter());
  for (size_t pos = 0; pos < html.size(); pos += chunk)
    EXPECT_TRUE(matcher->feed(reinterpret_cast<const uint8_t*>(html.data() + pos), std::min(chunk, html.size() - pos)));
  EXPECT_TRUE(matcher->finishChapter());
  return matches;
}
std::string document(std::string body) {
  return "<!DOCTYPE html SYSTEM 'about:legacy-compat'><html><head><title>Hidden title</title></head><body>" + body +
         "</body></html>";
}

}  // namespace
TEST(BookTextSearch, FindsPhraseAcrossInlineTagsAndCase) {
  const auto html = document("<p>The Sil<span>ver</span> <em>LANTERN</em> glows beside a door.</p>");
  const auto matches = findMatches("silver lantern", html);
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(matches[0].offset, html.find("Sil<span>"));
  EXPECT_NE(matches[0].snippet.find("The Silver LANTERN glows beside a door."), std::string::npos);
}
TEST(BookTextSearch, EveryFeedBoundaryHasIdenticalResults) {
  const auto html = document("<p>Before &eacute;lan <b>Straße</b> &#x1f319; &amp; SILVER\n\tLANTERN after.</p>");
  for (const size_t chunk : {1, 2, 3, 7, 255, 256, 257, 1024}) {
    const auto matches = findMatches("ÉLAN STRASSE 🌙 & silver lantern", html, chunk);
    ASSERT_EQ(matches.size(), 1u) << chunk;
    EXPECT_EQ(matches[0].offset, html.find("&eacute;")) << chunk;
    EXPECT_NE(matches[0].snippet.find("élan Straße 🌙 & SILVER LANTERN after."), std::string::npos) << chunk;
  }
}
TEST(BookTextSearch, UnicodeCaseExpansionAndFinalSigma) {
  const auto html = document("<p>Straße STRASSE ΟΣ ος οσ</p>");
  EXPECT_EQ(findMatches("strasse", html, 1).size(), 2u);
  EXPECT_EQ(findMatches("οσ", html, 2).size(), 3u);
}
TEST(BookTextSearch, UnicodeWhitespaceAndIgnorablesAreNormalized) {
  const auto html = document("<p>Silver&nbsp;\n\tlan&#x2009;tern</p><p>co&shy;op&#x200b;erate</p>");
  EXPECT_EQ(findMatches("  SILVER\tlan tern  ", html).size(), 1u);
  EXPECT_EQ(findMatches("cooperate", html, 1).size(), 1u);
}
TEST(BookTextSearch, BlockBoundariesSeparateAndInlineBoundariesJoin) {
  const auto html = document("<p>one</p><div>two<br/>three</div><p>four<b>teen</b></p>");
  EXPECT_EQ(findMatches("one two three fourteen", html).size(), 1u);
  EXPECT_EQ(findMatches("onetwo", html).size(), 0u);
  EXPECT_EQ(findMatches("four teen", html).size(), 0u);
}
TEST(BookTextSearch, HeadScriptStyleSvgAndHiddenContentAreSkipped) {
  const auto html = document(
      "<p>visible</p><script>secret</script><style>secret</style><svg><text>secret</text></svg>"
      "<div hidden=\"hidden\">secret</div><span style=\"DISPLAY: none !important\">secret</span>");
  EXPECT_TRUE(findMatches("hidden title", html).empty());
  EXPECT_TRUE(findMatches("secret", html).empty());
  EXPECT_EQ(findMatches("visible", html).size(), 1u);
}
TEST(BookTextSearch, NamespacedXhtmlWorks) {
  EXPECT_EQ(findMatches("silver lantern",
                        "<x:html xmlns:x=\"urn:test\"><x:body><x:p>silver</x:p><x:p>lantern</x:p></x:body></x:html>")
                .size(),
            1u);
}
TEST(BookTextSearch, OverlappingMatchesRetainDifferentSourceOrigins) {
  const auto html = document("<p>banana</p>");
  const auto matches = findMatches("ana", html);
  ASSERT_EQ(matches.size(), 2u);
  EXPECT_EQ(matches[0].offset, html.find("banana") + 1);
  EXPECT_EQ(matches[1].offset, html.find("banana") + 3);
}
TEST(BookTextSearch, ExpansionDoesNotDuplicateSameSourceCharacter) {
  const auto matches = findMatches("s", document("<p>ß</p>"));
  ASSERT_EQ(matches.size(), 1u);
}
TEST(BookTextSearch, NumericAndNamedEntitiesKeepRawOrigins) {
  const auto html = document("<p>&amp; &eacute; &#233; &#x1f319; lantern</p>");
  for (const auto& item :
       std::vector<std::pair<std::string, std::string>>{{"&", "&amp;"}, {"🌙", "&#x1f319;"}, {"lantern", "lantern"}}) {
    const auto matches = findMatches(item.first, html, 1);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].offset, html.find(item.second));
  }
  const auto accented = findMatches("é", html, 1);
  ASSERT_EQ(accented.size(), 2u);
  EXPECT_EQ(accented[0].offset, html.find("&eacute;"));
  EXPECT_EQ(accented[1].offset, html.find("&#233;"));
}
TEST(BookTextSearch, UnsupportedMultiCharacterEntityRemainsLiteralWithDistinctOrigins) {
  // fjlig is outside the renderer's supported named-entity table. Both parser
  // and search preserve its spelling; they must not collapse its letters to '&'.
  const auto html = document("<p>&fjlig;</p>");
  for (const size_t chunk : {1, 2, 256}) {
    const auto first = findMatches("f", html, chunk);
    const auto second = findMatches("j", html, chunk);
    ASSERT_EQ(first.size(), 1u);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(first[0].offset, html.find("&fjlig;") + 1);
    EXPECT_EQ(second[0].offset, html.find("&fjlig;") + 2);
    EXPECT_NE(first[0].offset, second[0].offset);
    EXPECT_NE(second[0].snippet.find("&fjlig;"), std::string::npos);
  }
}
TEST(BookTextSearch, BufferFlushBoundaryInsideMultibyteTextIsSafe) {
  const auto html = document("<p>" + std::string(255, 'x') + "é silver lantern</p>");
  const auto matches = findMatches("é silver lantern", html, 257);
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(matches[0].offset, html.find("é"));
}
TEST(BookTextSearch, EmptyWhitespaceInvisibleAndOversizeQueriesAreRejected) {
  for (const auto& query : std::vector<std::string>{"", " \t\r\n", "\xc2\xad\xe2\x80\x8b", std::string(129, 'a')}) {
    BookTextSearch matcher(query, collect, nullptr);
    EXPECT_FALSE(matcher.validQuery());
    EXPECT_FALSE(matcher.beginChapter());
  }
}
TEST(BookTextSearch, MaximumQueryAndLongContextStayBounded) {
  const std::string query(128, 'a');
  const auto matches =
      findMatches(query, document("<p>" + std::string(400, 'z') + query + std::string(400, 'z') + "</p>"), 1);
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_LE(matches[0].snippet.size(), BookTextSearch::SNIPPET_BYTES);
  EXPECT_NE(matches[0].snippet.find(query), std::string::npos);
}
TEST(BookTextSearch, ThousandsOfOverlapsDoNotLoseResults) {
  const auto matches = findMatches("a", document("<p>" + std::string(12000, 'a') + "</p>"), 511);
  ASSERT_EQ(matches.size(), 12000u);
  for (size_t i = 1; i < matches.size(); ++i) EXPECT_EQ(matches[i].offset, matches[i - 1].offset + 1);
}
TEST(BookTextSearch, MatchCallbackCanStopScanning) {
  unsigned count = 0;
  BookTextSearch matcher(
      "a", [](void* ctx, uint32_t, const char*) { return ++*static_cast<unsigned*>(ctx) < 3; }, &count);
  ASSERT_TRUE(matcher.beginChapter());
  const auto html = document("<p>" + std::string(2000, 'a') + "</p>");
  EXPECT_TRUE(matcher.feed(reinterpret_cast<const uint8_t*>(html.data()), html.size()));
  EXPECT_TRUE(matcher.stopped());
  EXPECT_EQ(count, 3u);
}
TEST(BookTextSearch, ChapterBoundaryFlushesContextWithoutJoiningPhrases) {
  std::vector<Match> matches;
  BookTextSearch matcher("silver lantern", collect, &matches);
  for (const auto& html : {document("<p>silver</p>"), document("<p>lantern</p>")}) {
    ASSERT_TRUE(matcher.beginChapter());
    EXPECT_TRUE(matcher.feed(reinterpret_cast<const uint8_t*>(html.data()), html.size()));
    EXPECT_TRUE(matcher.finishChapter());
  }
  EXPECT_TRUE(matches.empty());
}
TEST(BookTextSearch, MissingPhraseHasNoMatches) {
  EXPECT_TRUE(findMatches("missing phrase", document("<p>some prose</p>")).empty());
}

TEST(BookTextSearch, CapitalSharpSAndCompatibilityCapitalsCompareEqually) {
  const auto html = document("<p>STRAẞE straße STRASSE Kelvin KELVIN kelvin</p>");
  EXPECT_EQ(findMatches("strasse", html).size(), 3u);
  EXPECT_EQ(findMatches("kelvin", html).size(), 3u);
}
TEST(BookTextSearch, InlineDisplayUsesLastValidDeclarationAndImportantPrecedence) {
  for (const auto& style : {"display:none;display:block", "display:none!important;display:block!important",
                            "display:block!important;display:none", "display:none;display:inline flow-root",
                            "display:block;display:bogus", "display:block;display:n one"}) {
    EXPECT_EQ(findMatches("visible", document(std::string("<p style='") + style + "'>visible</p>")).size(), 1u)
        << style;
  }
  for (const auto& style : {"display:block;display:none", "display:none!important;display:block",
                            "display:block!important;display:none!important", "display:none;display:bogus",
                            "display:none;display:block!bogus"}) {
    EXPECT_TRUE(findMatches("hidden", document(std::string("<p style='") + style + "'>hidden</p>")).empty()) << style;
  }
}

TEST(BookTextSearch, RepairsBareHtmlVoidTagsWithoutShiftingRawMatches) {
  const std::string html = "<html><head><meta name='x'><link href='s.css'></head><body><p>first<br>silver <img src='x'>lantern</p></body></html>";
  for (size_t chunk : {1u, 7u, 4096u}) {
    const auto hits = findMatches("silver", html, chunk);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].offset, html.find("silver"));
  }
}
