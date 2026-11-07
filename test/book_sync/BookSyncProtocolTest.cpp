#include <gtest/gtest.h>

#include <string>

#include "src/util/BookSyncProtocol.h"

using namespace BookSync;
namespace {
std::string record(std::string title = "Fictional Book", std::string size = "123") {
  return std::string(64, 'a') + "\t" + size + "\t" + title + "~" + std::string(64, 'b') + ".epub";
}
Manifest::Line acceptRecord(const std::string& line) {
  Manifest state;
  Entry entry;
  EXPECT_EQ(state.accept("WITCHSYNC1", entry), Manifest::Line::Header);
  return state.accept(line, entry);
}
}  // namespace
TEST(BookSyncProtocol, AcceptsCompleteInventoryOnly) {
  Manifest state;
  Entry entry;
  EXPECT_EQ(state.accept("WITCHSYNC1", entry), Manifest::Line::Header);
  const auto line = record();
  EXPECT_EQ(state.accept(line, entry), Manifest::Line::Book);
  EXPECT_EQ(entry.size, 123U);
  EXPECT_EQ(entry.id, std::string(64, 'b'));
  EXPECT_FALSE(state.ended);
  EXPECT_EQ(state.accept("END\t1", entry), Manifest::Line::End);
  EXPECT_TRUE(state.ended);
  EXPECT_EQ(state.accept(line, entry), Manifest::Line::Invalid);
}
TEST(BookSyncProtocol, EmptyInventoryAndWrongCount) {
  Manifest state;
  Entry entry;
  state.accept("WITCHSYNC1", entry);
  EXPECT_EQ(state.accept("END\t1", entry), Manifest::Line::Invalid);
  Manifest empty;
  empty.accept("WITCHSYNC1", entry);
  EXPECT_EQ(empty.accept("END\t0", entry), Manifest::Line::End);
}
TEST(BookSyncProtocol, RejectsTraversalAndUnsafeFATNames) {
  for (const auto* name : {"../secret", "a/b", "a\\b", "CON.", "a:b", "a?b", "a\tb", "a~b", ""})
    EXPECT_EQ(acceptRecord(record(name)), Manifest::Line::Invalid) << name;
}
TEST(BookSyncProtocol, RejectsOverflowAndExcessiveSize) {
  for (const auto* size : {"", "0", "3", "536870913", "4294967296", "99999999999999999", "-1", "5x"})
    EXPECT_EQ(acceptRecord(record("Book", size)), Manifest::Line::Invalid) << size;
  EXPECT_EQ(acceptRecord(record("Book", "536870912")), Manifest::Line::Book);
}
TEST(BookSyncProtocol, RejectsMalformedDigestsAndDuplicates) {
  auto bad = record();
  bad[0] = 'G';
  EXPECT_EQ(acceptRecord(bad), Manifest::Line::Invalid);
  bad = record();
  bad[bad.size() - 6] = '/';
  EXPECT_EQ(acceptRecord(bad), Manifest::Line::Invalid);
  Manifest state;
  Entry entry;
  state.accept("WITCHSYNC1", entry);
  EXPECT_EQ(state.accept(record(), entry), Manifest::Line::Book);
  EXPECT_EQ(state.accept(record(), entry), Manifest::Line::Invalid);
}
TEST(BookSyncProtocol, BoundsNamesAndCounts) {
  EXPECT_EQ(acceptRecord(record(std::string(60, 'x'))), Manifest::Line::Book);
  EXPECT_EQ(acceptRecord(record(std::string(61, 'x'))), Manifest::Line::Invalid);
  Manifest state;
  Entry entry;
  state.accept("WITCHSYNC1", entry);
  state.count = MAX_BOOKS;
  EXPECT_EQ(state.accept(record(), entry), Manifest::Line::Invalid);
}
TEST(BookSyncProtocol, RequiresHTTPSOriginWithoutCredentialsOrPaths) {
  for (const auto* url : {"https://192.0.2.10:8444", "https://reader-sync.local", "https://books.example:443"})
    EXPECT_TRUE(origin(url)) << url;
  for (const auto* url :
       {"http://192.0.2.10", "https://u:p@host", "https://host/path", "https://host/", "https://host?x",
        "https://host#x", "https://host:0", "https://host:65536", "https://host\n", "https://", "https://a..b"})
    EXPECT_FALSE(origin(url)) << url;
}
