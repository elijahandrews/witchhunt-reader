#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/BookSearchSession.h"
#include "Epub/BookTextSearch.h"
#include "StoredZipWriter.h"

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
  return "<html><head><title>Hidden title</title></head><body>" + body + "</body></html>";
}

std::shared_ptr<Epub> makeBook(const std::string& tag, const std::vector<std::string>& bodies,
                               bool missingLast = false) {
  const auto dir = std::filesystem::temp_directory_path() / "witch-book-search-tests" / tag;
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  test_zip::StoredZipWriter zip;
  zip.add("mimetype", "application/epub+zip");
  zip.add("META-INF/container.xml",
          "<container><rootfiles><rootfile full-path=\"content.opf\" "
          "media-type=\"application/oebps-package+xml\"/></rootfiles></container>");
  std::string opf = "<package><metadata><title>Synthetic Search Specimens</title></metadata><manifest>";
  for (size_t i = 0; i < bodies.size(); ++i) {
    const auto id = std::to_string(i);
    opf += "<item id=\"s" + id + "\" href=\"chapter" + id + ".xhtml\" media-type=\"application/xhtml+xml\"/>";
    if (!missingLast || i + 1 < bodies.size()) zip.add("chapter" + id + ".xhtml", document(bodies[i]));
  }
  opf += "</manifest><spine>";
  for (size_t i = 0; i < bodies.size(); ++i) opf += "<itemref idref=\"s" + std::to_string(i) + "\"/>";
  zip.add("content.opf", opf + "</spine></package>");
  zip.write((dir / "synthetic.epub").string());
  auto epub = std::make_shared<Epub>((dir / "synthetic.epub").string(), (dir / "cache").string());
  EXPECT_TRUE(epub->load(true));
  epub->setupCacheDir();
  return epub;
}
void finish(BookSearchSession& session, size_t budget = 4096) {
  for (int i = 0; i < 100000 && !session.done(); ++i) session.step(budget);
  ASSERT_TRUE(session.done());
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
TEST(BookTextSearch, VoidTagRepairDoesNotShiftLaterOrigins) {
  const auto html = document("<p>before<br>after &nbsp; <img src=\"x\">target</p>");
  const auto matches = findMatches("target", html, 1);
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(matches[0].offset, html.find("target"));
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

TEST(BookSearchSession, ScansCurrentBookAndReadsArbitraryResultPages) {
  auto epub =
      makeBook("pages", {std::string(5000, 'x') + "<p>Silver <em>lantern</em></p>", "<p>SILVER LANTERN second</p>"});
  BookSearchSession session(epub, "silver lantern");
  ASSERT_TRUE(session.begin());
  session.step(1);
  EXPECT_FALSE(session.done());
  finish(session, 512);
  ASSERT_EQ(session.count(), 2u);
  EXPECT_FALSE(session.failed());
  EXPECT_EQ(session.failedSections(), 0);
  BookSearchSession::Hit hit;
  ASSERT_TRUE(session.readHit(1, hit));
  EXPECT_EQ(hit.spine, 1);
  EXPECT_NE(std::string(hit.snippet).find("SILVER LANTERN second"), std::string::npos);
  ASSERT_TRUE(session.readHit(0, hit));
  EXPECT_EQ(hit.spine, 0);
  EXPECT_FALSE(session.readHit(2, hit));
}
TEST(BookSearchSession, ManyResultsStayOnDiskAndRandomAccessIsStable) {
  auto epub = makeBook("many", {"<p>" + std::string(3000, 'a') + "</p>"});
  BookSearchSession session(epub, "a");
  ASSERT_TRUE(session.begin());
  finish(session, 100);
  ASSERT_EQ(session.count(), 3000u);
  BookSearchSession::Hit first, last;
  ASSERT_TRUE(session.readHit(0, first));
  ASSERT_TRUE(session.readHit(2999, last));
  EXPECT_EQ(last.sourceOffset, first.sourceOffset + 2999);
}
TEST(BookSearchSession, ExplicitLimitStopsPathologicalMatchCounts) {
  auto epub = makeBook("limit", {"<p>" + std::string(15000, 'a') + "</p>"});
  BookSearchSession session(epub, "a");
  ASSERT_TRUE(session.begin());
  finish(session);
  EXPECT_TRUE(session.limited());
  EXPECT_FALSE(session.failed());
  EXPECT_EQ(session.count(), BookSearchSession::MAX_MATCHES);
}
TEST(BookSearchSession, CancelDeletesTemporaryResultsAndStopsWork) {
  auto epub = makeBook("cancel", {"<p>" + std::string(8000, 'a') + "</p>"});
  BookSearchSession session(epub, "a");
  ASSERT_TRUE(session.begin());
  session.step(1);
  const auto path = epub->getCachePath() + "/search-results.tmp";
  EXPECT_TRUE(std::filesystem::exists(path));
  session.cancel();
  EXPECT_TRUE(session.done());
  EXPECT_FALSE(std::filesystem::exists(path));
  const auto count = session.count();
  session.step();
  EXPECT_EQ(session.count(), count);
}
TEST(BookSearchSession, CompletionDestructorDeletesTemporaryResults) {
  auto epub = makeBook("destructor", {"<p>silver lantern</p>"});
  const auto path = epub->getCachePath() + "/search-results.tmp";
  {
    BookSearchSession session(epub, "lantern");
    ASSERT_TRUE(session.begin());
    finish(session);
    EXPECT_TRUE(std::filesystem::exists(path));
  }
  EXPECT_FALSE(std::filesystem::exists(path));
}
TEST(BookSearchSession, MissingSpineIsReportedWithoutDroppingGoodResults) {
  auto epub = makeBook("missing", {"<p>silver lantern</p>", "<p>missing</p>"}, true);
  BookSearchSession session(epub, "lantern");
  ASSERT_TRUE(session.begin());
  finish(session);
  EXPECT_EQ(session.count(), 1u);
  EXPECT_EQ(session.failedSections(), 1);
  EXPECT_FALSE(session.failed());
}
TEST(BookSearchSession, DoesNotMatchAcrossChapters) {
  auto epub = makeBook("boundary", {"<p>silver</p>", "<p>lantern</p>"});
  BookSearchSession session(epub, "silver lantern");
  ASSERT_TRUE(session.begin());
  finish(session);
  EXPECT_EQ(session.count(), 0u);
}
TEST(BookSearchSession, NoMatchesAndInvalidQueryAreDistinct) {
  auto epub = makeBook("none", {"<p>silver lantern</p>"});
  BookSearchSession missing(epub, "different words");
  ASSERT_TRUE(missing.begin());
  finish(missing);
  EXPECT_EQ(missing.count(), 0u);
  EXPECT_FALSE(missing.failed());
  BookSearchSession blank(epub, "   ");
  EXPECT_FALSE(blank.validQuery());
  EXPECT_FALSE(blank.begin());
}

TEST(BookTextSearch, CapitalSharpSAndCompatibilityCapitalsCompareEqually) {
  const auto html = document("<p>STRAẞE straße STRASSE Kelvin KELVIN kelvin</p>");
  EXPECT_EQ(findMatches("strasse", html).size(), 3u);
  EXPECT_EQ(findMatches("kelvin", html).size(), 3u);
}
TEST(BookSearchSession, DenseMatchesYieldBeforeScanningAnEntireChapter) {
  auto epub = makeBook("dense-yield", {"<p>" + std::string(8000, 'a') + "</p>"});
  BookSearchSession session(epub, "a");
  ASSERT_TRUE(session.begin());
  session.step(4096);
  EXPECT_FALSE(session.done());
  EXPECT_LT(session.count(), 512u);
}
TEST(BookSearchSession, MalformedSpineIsReportedAndLaterSpinesAreStillSearched) {
  auto epub = makeBook("malformed", {"<p>lantern</wrong>", "<p>lantern</p>"});
  BookSearchSession session(epub, "lantern");
  ASSERT_TRUE(session.begin());
  finish(session);
  EXPECT_EQ(session.failedSections(), 1);
  EXPECT_GE(session.count(), 1u);
  BookSearchSession::Hit hit;
  ASSERT_TRUE(session.readHit(session.count() - 1, hit));
  EXPECT_EQ(hit.spine, 1);
}
TEST(BookSearchSession, CannotCreateResultsIsReportedAndDestructionIsSafe) {
  auto epub = makeBook("output-fail", {"<p>lantern</p>"});
  std::filesystem::create_directory(epub->getCachePath() + "/search-results.tmp");
  BookSearchSession session(epub, "lantern");
  EXPECT_FALSE(session.begin());
  EXPECT_TRUE(session.failed());
  EXPECT_TRUE(session.done());
}

TEST(BookSearchSession, ResultReadFailureIsReported) {
  auto epub = makeBook("read-fail", {"<p>lantern</p>"});
  BookSearchSession session(epub, "lantern");
  ASSERT_TRUE(session.begin());
  finish(session);
  ASSERT_EQ(session.count(), 1u);
  BookSearchSession::Hit hit;
  HalFile::failReadsFrom = 0;
  const bool read = session.readHit(0, hit);
  HalFile::failReadsFrom = -1;
  EXPECT_FALSE(read);
  EXPECT_TRUE(session.failed());
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
TEST(BookSearchSession, UnstartedOrInvalidSessionDoesNotRemoveAnotherSessionsResults) {
  auto epub = makeBook("ownership", {"<p>lantern</p>"});
  BookSearchSession owner(epub, "lantern");
  ASSERT_TRUE(owner.begin());
  finish(owner);
  {
    BookSearchSession neverStarted(epub, "lantern");
    BookSearchSession invalid(epub, "  ");
    EXPECT_FALSE(invalid.begin());
  }
  EXPECT_TRUE(std::filesystem::exists(epub->getCachePath() + "/search-results.tmp"));
  BookSearchSession::Hit hit;
  EXPECT_TRUE(owner.readHit(0, hit));
}
