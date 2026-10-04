// FontManifestReader: the glue the Font Manager and the web font API share -- the manifest file read
// in two streamed passes into each one's own family type, checked family by family.

#include <FontManifestReader.h>
#include <gtest/gtest.h>

#include <cctype>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

struct TestFile {
  std::string name;
  size_t size = 0;
  uint32_t crc32 = 0;
  bool hasCrc32 = false;
};

struct TestFamily {
  std::string name;
  std::string description;
  std::vector<TestFile> files;
  size_t totalSize = 0;
  bool installed = false;  // set by the callers afterwards; the reader leaves it alone
};

// Stand-ins for FontInstaller's checks, with the same shape: a family name is 1-31 characters of
// [A-Za-z0-9_-]; a file name is 1-60 characters with no "..".
bool familyNameOk(const char* name) {
  const std::string n(name);
  if (n.empty() || n.size() > 31) return false;
  for (const char c : n) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return false;
  }
  return true;
}

bool fileNameOk(const std::string& name) {
  return !name.empty() && name.size() <= 60 && name.find("..") == std::string::npos;
}

struct Outcome {
  FontManifestStatus status = FontManifestStatus::Invalid;
  std::vector<TestFamily> families;
  std::string baseUrl = "untouched";
  std::string failure;
  int version = -1;
};

Outcome read(const std::string& json) {
  HalFile file = HalFile::fromString(json);
  FontManifestReader<TestFamily> reader("TEST", familyNameOk, fileNameOk);
  Outcome out;
  out.families.push_back(TestFamily{"Stale", "from an earlier read", {}, 0, false});
  out.status = reader.read(file, out.families, out.baseUrl);
  out.failure = reader.failure() ? reader.failure() : "";
  out.version = reader.version();
  return out;
}

std::vector<std::string> names(const std::vector<TestFamily>& families) {
  std::vector<std::string> out;
  for (const auto& f : families) out.push_back(f.name);
  return out;
}

const std::string kManifest = R"({
  "version": 2,
  "baseUrl": "https://example.com/fonts/",
  "families": [
    {"name": "Alpha", "description": "First", "styles": ["regular"],
     "files": [{"name": "Alpha/Alpha_10.cpfont", "size": 1000, "crc32": 11},
               {"name": "Alpha/Alpha_12.cpfont", "size": 2000, "crc32": 22}]},
    {"name": "Beta", "description": "Second",
     "files": [{"name": "Beta/Beta_10.cpfont", "size": 3000}]}
  ]
})";

}  // namespace

TEST(FontManifestReaderTest, BuildsEachFamilyWithItsFilesAndTotal) {
  const Outcome out = read(kManifest);
  ASSERT_EQ(out.status, FontManifestStatus::Ok) << out.failure;
  EXPECT_EQ(out.version, 2);
  EXPECT_EQ(out.baseUrl, "https://example.com/fonts/");
  ASSERT_EQ(out.families.size(), 2u);

  const TestFamily& alpha = out.families[0];
  EXPECT_EQ(alpha.name, "Alpha");
  EXPECT_EQ(alpha.description, "First");
  ASSERT_EQ(alpha.files.size(), 2u);
  EXPECT_EQ(alpha.files[0].name, "Alpha/Alpha_10.cpfont");
  EXPECT_EQ(alpha.files[0].size, 1000u);
  EXPECT_TRUE(alpha.files[0].hasCrc32);
  EXPECT_EQ(alpha.files[0].crc32, 11u);
  EXPECT_EQ(alpha.files[1].crc32, 22u);
  EXPECT_EQ(alpha.totalSize, 3000u);

  const TestFamily& beta = out.families[1];
  EXPECT_EQ(beta.name, "Beta");
  ASSERT_EQ(beta.files.size(), 1u);
  EXPECT_FALSE(beta.files[0].hasCrc32);
  EXPECT_EQ(beta.files[0].crc32, 0u);
  EXPECT_EQ(beta.totalSize, 3000u);
}

// The first pass counts the families, so the list is reserved once at its final size -- every
// element of families[], as the whole-document parse reserved families.size().
TEST(FontManifestReaderTest, ReservesTheListForEveryFamilyEntry) {
  const Outcome out = read(R"({"version":2,"families":[{"name":"A"},{"name":"bad name"},{"name":"C"}]})");
  ASSERT_EQ(out.status, FontManifestStatus::Ok);
  EXPECT_EQ(names(out.families), (std::vector<std::string>{"A", "C"}));
  EXPECT_EQ(out.families.capacity(), 3u);
}

// A family whose name or any file name fails its check is left out; the rest still load.
TEST(FontManifestReaderTest, LeavesOutFamiliesThatFailTheirChecks) {
  const std::string longName(200, 'x');
  const Outcome out = read(R"({"version":2,"families":[
    {"name":"Good","files":[{"name":"Good/Good_10.cpfont","size":1}]},
    {"name":"../evil","files":[{"name":"x.cpfont","size":1}]},
    {"name":"BadFile","files":[{"name":"ok.cpfont","size":1},{"name":"../../etc","size":1}]},
    {"name":"TooLong","files":[{"name":")" +
                           longName + R"(","size":1}]},
    {"name":"NotAnObject","files":[{"name":"ok.cpfont","size":1}, 42]},
    7,
    {"files":[{"name":"NoName/x.cpfont","size":1}]},
    {"name":"AlsoGood"}
  ]})");
  ASSERT_EQ(out.status, FontManifestStatus::Ok) << out.failure;
  EXPECT_EQ(names(out.families), (std::vector<std::string>{"Good", "AlsoGood"}));
  EXPECT_TRUE(out.families[1].files.empty());
}

// A file name over the parser's buffer is rejected even when the check alone would pass it.
TEST(FontManifestReaderTest, AnOverflowedFileNameRejectsItsFamily) {
  const std::string longName(FontManifestParser::FILE_NAME_BUF_SIZE + 10, 'y');
  HalFile file = HalFile::fromString(R"({"version":2,"families":[{"name":"F","files":[{"name":")" + longName +
                                     R"(","size":1}]},{"name":"G"}]})");
  FontManifestReader<TestFamily> reader("TEST", familyNameOk, [](const std::string&) { return true; });
  std::vector<TestFamily> families;
  std::string baseUrl;
  ASSERT_EQ(reader.read(file, families, baseUrl), FontManifestStatus::Ok);
  EXPECT_EQ(names(families), (std::vector<std::string>{"G"}));
}

// Whatever goes wrong, nothing half-built is left behind -- not even what the lists held before.
TEST(FontManifestReaderTest, AnInvalidDocumentLeavesNothingBehind) {
  for (const std::string& json : {
           kManifest.substr(0, kManifest.size() / 2),                        // cut short
           std::string(R"({"version":2,"families":[{"name":nope}]})"),       // not JSON
           std::string(""),                                                  // empty
           R"({"version":2,"baseUrl":")" + std::string(300, 'u') + R"("})",  // base URL too long
       }) {
    SCOPED_TRACE(json.substr(0, 60));
    const Outcome out = read(json);
    EXPECT_EQ(out.status, FontManifestStatus::Invalid);
    EXPECT_FALSE(out.failure.empty());
    EXPECT_TRUE(out.families.empty());
    EXPECT_EQ(out.baseUrl, "");
  }
}

TEST(FontManifestReaderTest, AnUnreadableFileIsInvalid) {
  HalFile file;  // no backing data: seek and read fail
  FontManifestReader<TestFamily> reader("TEST", familyNameOk, fileNameOk);
  std::vector<TestFamily> families;
  std::string baseUrl;
  EXPECT_EQ(reader.read(file, families, baseUrl), FontManifestStatus::Invalid);
  EXPECT_NE(reader.failure(), nullptr);
}

// Only versions 1 and 2 are read; any other leaves nothing behind and reports the version found.
TEST(FontManifestReaderTest, OnlyVersions1And2AreAccepted) {
  Outcome out = read(R"({"version":3,"families":[{"name":"A"}]})");
  EXPECT_EQ(out.status, FontManifestStatus::UnsupportedVersion);
  EXPECT_EQ(out.version, 3);
  EXPECT_TRUE(out.families.empty());
  EXPECT_EQ(out.baseUrl, "");

  out = read(R"({"families":[{"name":"A"}]})");
  EXPECT_EQ(out.status, FontManifestStatus::UnsupportedVersion);
  EXPECT_EQ(out.version, 0);

  out = read(R"({"version":1,"families":[{"name":"A"}]})");
  EXPECT_EQ(out.status, FontManifestStatus::Ok);
  EXPECT_EQ(names(out.families), (std::vector<std::string>{"A"}));
}

// The shipped manifest, end to end: every family loads, each with the sum of its file sizes.
TEST(FontManifestReaderTest, ReadsTheShippedManifest) {
  std::ifstream in(FONT_MANIFEST_PATH, std::ios::binary);
  ASSERT_TRUE(in.good()) << FONT_MANIFEST_PATH;
  const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  const Outcome out = read(json);
  ASSERT_EQ(out.status, FontManifestStatus::Ok) << out.failure;
  ASSERT_EQ(out.families.size(), 28u);
  EXPECT_EQ(out.families.capacity(), 28u);
  size_t files = 0;
  for (const auto& family : out.families) {
    size_t sum = 0;
    for (const auto& file : family.files) {
      sum += file.size;
      EXPECT_TRUE(file.hasCrc32) << file.name;
    }
    EXPECT_EQ(family.totalSize, sum) << family.name;
    files += family.files.size();
  }
  EXPECT_EQ(files, 133u);
  EXPECT_EQ(out.families[0].name, "Alegreya");
  EXPECT_EQ(out.families[0].files[0].name, "Alegreya/Alegreya_10.cpfont");
}
