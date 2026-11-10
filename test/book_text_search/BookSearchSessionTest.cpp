#include <Epub.h>
#include <Epub/BookSearchSession.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "StoredZipWriter.h"
namespace {
std::shared_ptr<Epub> makeBook(const std::vector<std::string>& bodies, bool missingLast = false,
                               bool deflated = false) {
  auto book = std::make_shared<Epub>();
  test_zip::StoredZipWriter writer;
  for (size_t i = 0; i < bodies.size(); ++i) {
    const auto name = "section" + std::to_string(i) + ".xhtml";
    book->items.push_back({name});
    if (missingLast && i + 1 == bodies.size()) continue;
    const auto html = "<html><body>" + bodies[i] + "</body></html>";
    if (deflated)
      writer.addDeflated(name, html);
    else
      writer.add(name, html);
  }
  const auto bytes = writer.bytes();
  Storage.put(book->path, {bytes.begin(), bytes.end()});
  return book;
}
void finish(BookSearchSession& session, size_t budget = 4096) {
  for (int i = 0; i < 100000 && !session.done(); ++i) session.step(budget);
  ASSERT_TRUE(session.done());
}
std::vector<std::string> resultPaths() {
  auto paths = Storage.paths();
  std::erase_if(paths, [](const auto& path) { return path.find("/search-") == std::string::npos; });
  return paths;
}
class BookSearchSessionTest : public ::testing::Test {
  void SetUp() override { Storage.reset(); }
};
}  // namespace
TEST_F(BookSearchSessionTest, StoredAndDeflatedSpinesYieldAndResolveRawResults) {
  for (const bool compressed : {false, true}) {
    Storage.reset();
    auto book =
        makeBook({"<p>" + std::string(5000, 'x') + " silver lantern</p>", "<p>Silver Lantern</p>"}, false, compressed);
    BookSearchSession session(book, "silver lantern");
    ASSERT_TRUE(session.begin());
    session.step(32);
    EXPECT_FALSE(session.done());
    EXPECT_EQ(session.sectionsScanned(), 0);
    finish(session, 256);
    EXPECT_FALSE(session.failed());
    EXPECT_EQ(session.failedSections(), 0);
    ASSERT_EQ(session.count(), 2);
    BookSearchSession::Hit hit;
    ASSERT_TRUE(session.readHit(1, hit));
    EXPECT_EQ(hit.spine, 1);
    EXPECT_EQ(hit.sourceOffset, std::string("<html><body><p>").size());
    EXPECT_EQ(Storage.reopenWhileOpen, 0U);
  }
}
TEST_F(BookSearchSessionTest, TwoLiveSessionsAndStaleFilesHaveIndependentOwnership) {
  auto book = makeBook({"<p>silver silver</p>"});
  Storage.put(book->cache + "/search-0.tmp", {'o', 'l', 'd'});
  BookSearchSession first(book, "silver"), second(book, "silver");
  ASSERT_TRUE(first.begin());
  ASSERT_TRUE(second.begin());
  const auto paths = resultPaths();
  ASSERT_EQ(paths.size(), 3U);
  finish(first);
  finish(second);
  EXPECT_EQ(first.count(), 2U);
  EXPECT_EQ(second.count(), 2U);
  first.cancel();
  EXPECT_EQ(resultPaths().size(), 2U);
  BookSearchSession::Hit hit;
  EXPECT_TRUE(second.readHit(1, hit));
  second.cancel();
  EXPECT_EQ(resultPaths().size(), 1U);
  EXPECT_EQ(Storage.bytes(book->cache + "/search-0.tmp"), (std::vector<uint8_t>{'o', 'l', 'd'}));
}
TEST_F(BookSearchSessionTest, FailedAndUnstartedSessionsCannotDeleteAnotherFile) {
  auto book = makeBook({"<p>silver</p>"});
  BookSearchSession owner(book, "silver");
  ASSERT_TRUE(owner.begin());
  auto paths = resultPaths();
  {
    BookSearchSession invalid(book, "   ");
    EXPECT_FALSE(invalid.begin());
  }
  {
    BookSearchSession unused(book, "silver");
  }
  Storage.failCreate = true;
  {
    BookSearchSession failed(book, "silver");
    EXPECT_FALSE(failed.begin());
    EXPECT_TRUE(failed.failed());
  }
  EXPECT_EQ(resultPaths(), paths);
  finish(owner);
  EXPECT_EQ(owner.count(), 1U);
}
TEST_F(BookSearchSessionTest, LimitAndCancellationRemainBounded) {
  auto book = makeBook({"<p>" + std::string(15000, 'a') + "</p>"});
  BookSearchSession session(book, "a");
  ASSERT_TRUE(session.begin());
  session.step(128);
  EXPECT_LT(session.count(), 128U);
  finish(session);
  EXPECT_EQ(session.count(), BookSearchSession::MAX_MATCHES);
  EXPECT_TRUE(session.limited());
  EXPECT_FALSE(session.failed());
  session.cancel();
  EXPECT_TRUE(resultPaths().empty());
}
TEST_F(BookSearchSessionTest, DenseMatchesYieldAndResumeWithoutLosingResults) {
  for (const bool compressed : {false, true}) {
    Storage.reset();
    auto book = makeBook({"<p>" + std::string(8192, 'a') + "</p>"}, false, compressed);
    BookSearchSession session(book, "a");
    ASSERT_TRUE(session.begin());
    session.step(8192);
    EXPECT_FALSE(session.done());
    EXPECT_GE(session.count(), 64U);
    EXPECT_LT(session.count(), 512U);
    EXPECT_EQ(session.sectionsScanned(), 0);
    finish(session, 8192);
    EXPECT_FALSE(session.failed());
    EXPECT_EQ(session.failedSections(), 0);
    ASSERT_EQ(session.count(), 8192U);
    BookSearchSession::Hit hit;
    const size_t start = std::string("<html><body><p>").size();
    ASSERT_TRUE(session.readHit(0, hit));
    EXPECT_EQ(hit.sourceOffset, start);
    ASSERT_TRUE(session.readHit(8191, hit));
    EXPECT_EQ(hit.sourceOffset, start + 8191);
    session.cancel();
    EXPECT_TRUE(resultPaths().empty());
    EXPECT_EQ(Storage.reopenWhileOpen, 0U);
  }
}
TEST_F(BookSearchSessionTest, DenseCompressedSearchCanCancelAfterOneTurn) {
  auto book = makeBook({"<p>" + std::string(8192, 'a') + "</p>"}, false, true);
  BookSearchSession session(book, "a");
  ASSERT_TRUE(session.begin());
  session.step(8192);
  ASSERT_FALSE(session.done());
  ASSERT_LT(session.count(), 512U);
  const uint32_t count = session.count();
  session.cancel();
  session.step(8192);
  EXPECT_EQ(session.count(), count);
  EXPECT_TRUE(session.done());
  EXPECT_TRUE(resultPaths().empty());
}
TEST_F(BookSearchSessionTest, MissingAndMalformedSectionsRetainGoodResults) {
  auto book = makeBook({"<p>silver</p>", "<p>silver</p><broken>", "<p>absent</p>"}, true);
  BookSearchSession session(book, "silver");
  ASSERT_TRUE(session.begin());
  finish(session);
  EXPECT_EQ(session.failedSections(), 2);
  EXPECT_EQ(session.count(), 2U);
  EXPECT_FALSE(session.failed());
}
TEST_F(BookSearchSessionTest, WriteReadAndCloseFailuresAreReported) {
  auto book = makeBook({"<p>silver</p>"});
  Storage.defaultWriteLimit = 10;
  {
    BookSearchSession failed(book, "silver");
    ASSERT_TRUE(failed.begin());
    finish(failed);
    EXPECT_TRUE(failed.failed());
  }
  EXPECT_TRUE(resultPaths().empty());
  Storage.defaultWriteLimit = SIZE_MAX;
  {
    BookSearchSession session(book, "silver");
    ASSERT_TRUE(session.begin());
    finish(session);
    Storage.failReadsAt(resultPaths().at(0), 0);
    BookSearchSession::Hit hit;
    EXPECT_FALSE(session.readHit(0, hit));
    EXPECT_TRUE(session.failed());
  }
  Storage.defaultCloseFailure = true;
  {
    BookSearchSession failed(book, "silver");
    ASSERT_TRUE(failed.begin());
    finish(failed);
    EXPECT_TRUE(failed.failed());
  }
  EXPECT_TRUE(resultPaths().empty());
}
TEST_F(BookSearchSessionTest, CancelStopsWorkAndReleasesItsFile) {
  auto book = makeBook({"<p>" + std::string(8000, 'x') + "</p>"});
  BookSearchSession session(book, "silver");
  ASSERT_TRUE(session.begin());
  session.step(128);
  session.cancel();
  EXPECT_TRUE(session.done());
  EXPECT_TRUE(resultPaths().empty());
  session.step();
  EXPECT_EQ(session.count(), 0U);
}
