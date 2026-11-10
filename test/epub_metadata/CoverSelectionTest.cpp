#include <Epub/CoverImageRefScanner.h>
#include <Epub/SpineHrefResolver.h>
#include <Epub/converters/ImageDimsProbe.h>
#include <FsHelpers.h>
#include <Memory.h>
#include <ZipFile.h>
#include <gtest/gtest.h>

#include <map>

namespace {
// The selected production blocks run unchanged; only EPUB archive access is a
// synthetic source map. No copied selection algorithm or private book content.
struct CoverHarness {
  struct Metadata {
    std::string coverItemHref;
  } bookMetadata;
  struct Opf {
    std::string guideCoverPageHref;
  } opfParser;
  std::string filepath;
  int zipDirectoryCache = 0;
  std::map<std::string, std::string> items;
  bool failRead = false;
  CoverHarness() {
    ZipFile::paths.clear();
    ZipFile::failEnumeration = false;
  }
  bool getItemSize(const std::string& path, size_t* bytes) {
    const auto item = items.find(path);
    if (item == items.end()) return false;
    *bytes = item->second.size();
    return true;
  }
  bool readItemContentsToStream(const std::string& path, Print& output, size_t chunk, bool = false) {
    const auto item = items.find(path);
    if (item == items.end() || failRead) return false;
    for (size_t i = 0; i < item->second.size(); i += chunk) {
      const size_t n = std::min(chunk, item->second.size() - i);
      if (output.write(reinterpret_cast<const uint8_t*>(item->second.data()) + i, n) != n) break;
    }
    return true;
  }
  std::string findCoverImageInDocument(const std::string& document) {
#include "CoverDocument.inc"
  }
  void select() {
#include "CoverSelection.inc"
  }
};
}  // namespace

TEST(CoverSelection, NamedSvgWrapperResolvesRelativeRasterCover) {
  CoverHarness source;
  source.bookMetadata.coverItemHref = "OPS/pages/cover.svg";
  source.items["OPS/pages/cover.svg"] = "<svg><image href='../pictures/cover%20one.png#view'/></svg>";
  source.items["OPS/pictures/cover one.png"] = "image";
  source.select();
  EXPECT_EQ(source.bookMetadata.coverItemHref, "OPS/pictures/cover one.png");
}

TEST(CoverSelection, MissingNamedCoverUsesGuideThenBoundedCommonNameScan) {
  CoverHarness source;
  source.bookMetadata.coverItemHref = "missing.png";
  source.opfParser.guideCoverPageHref = "OPS/guide.xhtml";
  source.items["OPS/guide.xhtml"] = "<html><body><img src='actual.jpg'></body></html>";
  source.items["OPS/actual.jpg"] = "image";
  ZipFile::paths = {"pictures/cover.gif"};
  source.select();
  EXPECT_EQ(source.bookMetadata.coverItemHref, "OPS/actual.jpg");
  source.items.erase("OPS/actual.jpg");
  source.select();
  EXPECT_EQ(source.bookMetadata.coverItemHref, "pictures/cover.gif");
  source.bookMetadata.coverItemHref = "missing.png";
  ZipFile::failEnumeration = true;
  source.select();
  EXPECT_TRUE(source.bookMetadata.coverItemHref.empty());
}

TEST(CoverSelection, MisnamedRasterSvgCoverIsKeptAndValidNamedCoverHasPrecedence) {
  CoverHarness source;
  source.bookMetadata.coverItemHref = "misnamed.svg";
  const uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xc0, 0, 7, 8, 0, 16, 0, 16};
  source.items["misnamed.svg"].assign(reinterpret_cast<const char*>(jpeg), sizeof(jpeg));
  ZipFile::paths = {"cover.jpg"};
  source.select();
  EXPECT_EQ(source.bookMetadata.coverItemHref, "misnamed.svg");
  source.bookMetadata.coverItemHref = "valid.png";
  source.items["valid.png"] = "image";
  source.select();
  EXPECT_EQ(source.bookMetadata.coverItemHref, "valid.png");
}

TEST(CoverSelection, BrokenWrapperAndReadFailureFallBackWithoutFabricatedReference) {
  CoverHarness source;
  source.bookMetadata.coverItemHref = "cover.svg";
  source.items["cover.svg"] = "<svg><text>no image</text></svg>";
  source.select();
  EXPECT_TRUE(source.bookMetadata.coverItemHref.empty());
  source.items["cover.svg"] = "<svg><image href='missing.jpg'/></svg>";
  source.bookMetadata.coverItemHref = "cover.svg";
  source.failRead = true;
  source.select();
  EXPECT_TRUE(source.bookMetadata.coverItemHref.empty());
}
