#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "PipelineRunner.h"
#include "StoredZipWriter.h"

namespace {
std::string featureDump(const std::string& name, int width = 460, int images = 0, bool warm = false) {
  const auto dir = std::filesystem::temp_directory_path() / "witch-epub-feature-tests" / name;
  if (!warm) std::filesystem::remove_all(dir);
  pipeline_harness::Profile profile;
  profile.viewportWidth = width;
  profile.viewportHeight = 3000;
  profile.imageRendering = images;
  profile.fontSizeNormalization = false;
  std::ostringstream result;
  EXPECT_TRUE(pipeline_harness::runAndDump(std::string(CORPUS_DIR) + "/test_epub_features.epub", dir.string(), profile,
                                           result));
  std::ofstream("/tmp/witch-feature-" + name + ".txt") << result.str();
  return result.str();
}
std::vector<std::string> words(const std::string& dump) {
  const std::regex pattern("   W[^\\n]* t=([^\\n]*)\\n");
  std::vector<std::string> result;
  for (auto it = std::sregex_iterator(dump.begin(), dump.end(), pattern); it != std::sregex_iterator(); ++it)
    result.push_back((*it)[1]);
  return result;
}
bool hasWord(const std::vector<std::string>& all, const std::string& word) {
  return std::find(all.begin(), all.end(), word) != all.end();
}
}  // namespace

TEST(EpubFeatures, PreservesIndentationTabsSpanBoundariesAndBlankLines) {
  const auto dump = featureDump("spaces");
  const auto all = words(dump);
  EXPECT_TRUE(hasWord(all, "  "));
  EXPECT_TRUE(hasWord(all, "LEAD  "));
  EXPECT_TRUE(hasWord(all, "A       "));
  EXPECT_TRUE(hasWord(all, "AB "));
  EXPECT_TRUE(hasWord(all, " "));
  EXPECT_TRUE(hasWord(all, "CD"));
  EXPECT_TRUE(hasWord(all, "XY"));
  EXPECT_TRUE(hasWord(all, "  "));
  EXPECT_TRUE(hasWord(all, "Z"));
  EXPECT_TRUE(hasWord(all, "AFTER"));
  EXPECT_TRUE(hasWord(all, "COLLAPSE"));
  // The synthetic monospace face advances9 px. Preserved space runs and tabs must
  // occupy their exact columns, including styled fragments, without an extra word gap.
  EXPECT_NE(dump.find("W x=18 s=0 z=100 f=4 ls=0 t=LEAD  "), std::string::npos);
  EXPECT_NE(dump.find("W x=72 s=0 z=100 f=4 ls=0 t=GAP"), std::string::npos);
  EXPECT_NE(dump.find("W x=72 s=0 z=100 f=4 ls=0 t=TAB"), std::string::npos);
  EXPECT_NE(dump.find("W x=36 s=1 z=100 f=4 ls=0 t=CD"), std::string::npos);
  EXPECT_NE(dump.find("W x=36 s=0 z=100 f=4 ls=0 t=Z"), std::string::npos);
  EXPECT_NE(dump.find("LINE y=120 x=0 align=1"), std::string::npos);
  EXPECT_FALSE(hasWord(all, "AFTER   "));
  EXPECT_EQ(dump, featureDump("spaces", 460, 0, true));
}

TEST(EpubFeatures, PreDoesNotWrapAndPreWrapDoes) {
  const auto dump = featureDump("wrap", 160);
  // The unwrapped specimen remains one physical line; its offscreen text is clipped at draw.
  const std::regex noWrap("LINE[^\\n]*words=12\\n   W[^\\n]*t=NOWRAP ");
  EXPECT_TRUE(std::regex_search(dump, noWrap));
  const std::regex wrap("LINE[^\\n]*words=12\\n   W[^\\n]*t=WRAP ");
  EXPECT_FALSE(std::regex_search(dump, wrap));
  EXPECT_NE(dump.find("t=ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 "), std::string::npos);
  EXPECT_EQ(dump.find("t=ABCDEFGHIJKLM-"), std::string::npos);
  EXPECT_NE(dump.find("f=4 ls=0 t=CODE"), std::string::npos);
  EXPECT_NE(dump.find("f=4 ls=0 t=KEY"), std::string::npos);
  EXPECT_NE(dump.find("f=4 ls=0 t=OUTPUT"), std::string::npos);
  EXPECT_TRUE(std::regex_search(dump, std::regex("z=100( f=0 ls=0)? t=PROSE")));
  EXPECT_EQ(dump, featureDump("wrap", 160, 0, true));
}

TEST(EpubFeatures, SmallCapsInheritThroughBlocksLinksAndExplicitResets) {
  const auto dump = featureDump("caps");
  for (const auto* label : {"CapsBlock", "CapsInherited", "CapsNested", "CapsAgain", "CapsRestored"})
    EXPECT_TRUE(std::regex_search(dump, std::regex("s=64 z=100( f=0 ls=0)? t=" + std::string(label)))) << label;
  for (const auto* label : {"AllInline", "AllBlock", "AllInherited", "AllRestored"})
    EXPECT_TRUE(std::regex_search(dump, std::regex("s=192 z=100( f=0 ls=0)? t=" + std::string(label)))) << label;
  EXPECT_NE(dump.find("s=196 z=100 t=AllLink"), std::string::npos);
  for (const auto* label : {"NormalNested", "NormalOverride", "OutsideCaps"})
    EXPECT_NE(dump.find("s=0 z=100 t=" + std::string(label)), std::string::npos) << label;
  EXPECT_EQ(dump, featureDump("caps", 460, 0, true));
}

TEST(EpubFeatures, MathImageReplacesDescendantsOnlyWhenAccepted) {
  const auto dump = featureDump("math");
  EXPECT_EQ(dump.find("t=HiddenMathFallback"), std::string::npos);
  EXPECT_EQ(dump.find("t=HiddenCellMath"), std::string::npos);
  for (const auto* label : {"BeforeMath", "AfterMath", "MissingMathVisible", "UnsupportedMathVisible",
                            "NoImageMathVisible", "MissingCellMathVisible"})
    EXPECT_NE(dump.find("t=" + std::string(label)), std::string::npos) << label;
  EXPECT_NE(dump.find("IMG "), std::string::npos);
  EXPECT_EQ(dump, featureDump("math", 460, 0, true));
  const auto placeholders = featureDump("math-placeholders", 460, 1);
  EXPECT_NE(placeholders.find("t=HiddenMathFallback"), std::string::npos);
  EXPECT_NE(placeholders.find("t=HiddenCellMath"), std::string::npos);
}

TEST(EpubFeatures, HeadStylesAreLocalAndTheirFontsResolveFromTheDocumentDirectory) {
  const auto dump = featureDump("heads");
  EXPECT_NE(dump.find("t=SHAREDONE"), std::string::npos);
  EXPECT_NE(dump.find("t=sharedtwo"), std::string::npos);
  EXPECT_NE(dump.find("s=1 z=100 t=PriorityOne"), std::string::npos);
  EXPECT_NE(dump.find("s=1 z=100 t=PriorityTwo"), std::string::npos);
  EXPECT_EQ(dump.find("t=SharedOne"), std::string::npos);
  EXPECT_EQ(dump, featureDump("heads", 460, 0, true));
}

TEST(EpubFeatures, HeadFontAliasesAndGlobalReferencesStayDistinctAfterCachedReopen) {
  const auto dir = std::filesystem::temp_directory_path() / "witch-head-font-alias-tests";
  std::filesystem::remove_all(dir);
  const auto path = std::string(CORPUS_DIR) + "/test_epub_features.epub";
  uint8_t firstId = 0, secondId = 0;
  for (int reopen = 0; reopen < 2; ++reopen) {
    Epub book(path, dir.string());
    ASSERT_TRUE(book.load(true));
    auto* global = book.getCssParser();
    ASSERT_NE(global, nullptr);
    ASSERT_TRUE(global->loadFromCache());
    for (int chapter = 4; chapter <= 5; ++chapter) {
      std::string cache;
      bool truncated = false;
      ASSERT_TRUE(book.documentStyleCache(chapter, cache, truncated));
      ASSERT_FALSE(cache.empty());
      EXPECT_FALSE(truncated);
      CssParser local(cache, global);
      ASSERT_TRUE(local.loadFromCache());
      const auto fromHead = global->resolveStyle("p", "shared", "", &local);
      const auto fromGlobal = global->resolveStyle("p", "fromglobal", "", &local);
      const auto fromInline = CssParser::parseInlineStyle("font-family:Chapter Face,serif", &local);
      EXPECT_EQ(fromGlobal.fontFamily, fromHead.fontFamily);
      EXPECT_EQ(fromInline.fontFamily, fromHead.fontFamily);
      const auto* stack = global->fontCatalog().stack(fromHead.fontFamily);
      ASSERT_NE(stack, nullptr);
      const auto& faces = global->fontCatalog().faces();
      const auto match = std::find_if(faces.begin(), faces.end(),
                                      [&](const auto& f) { return f.family == stack->families.front().name; });
      ASSERT_NE(match, faces.end());
      ASSERT_FALSE(match->sources.empty());
      EXPECT_EQ(match->sources.front().path, chapter == 4 ? "OPS/Fonts/One.ttf" : "OPS/Fonts/Two.ttf");
      auto& id = chapter == 4 ? firstId : secondId;
      if (reopen)
        EXPECT_EQ(fromHead.fontFamily, id);
      else
        id = fromHead.fontFamily;
    }
    EXPECT_NE(firstId, secondId);
  }
}

TEST(EpubFeatures, MissingHeadCacheAndValidBitIndexCorruptionRebuildOnOpen) {
  const auto dir = std::filesystem::temp_directory_path() / "witch-head-cache-integrity";
  std::filesystem::remove_all(dir);
  const auto path = std::string(CORPUS_DIR) + "/test_epub_features.epub";
  std::string cacheRoot, localPath;
  {
    Epub book(path, dir.string());
    ASSERT_TRUE(book.load(true));
    cacheRoot = book.getCachePath();
    bool truncated = false;
    ASSERT_TRUE(book.documentStyleCache(4, localPath, truncated));
    ASSERT_FALSE(localPath.empty());
    std::fstream index(cacheRoot + "/doc_css/index.bin", std::ios::in | std::ios::out | std::ios::binary);
    index.seekp(3 + 4);
    index.put(0);
    index.close();  // valid flag, invalid content checksum
    EXPECT_FALSE(book.documentStyleCache(4, localPath, truncated));
  }
  {
    Epub reopened(path, dir.string());
    ASSERT_TRUE(reopened.load(true));
    bool truncated = false;
    ASSERT_TRUE(reopened.documentStyleCache(4, localPath, truncated));
    ASSERT_FALSE(localPath.empty());
    ASSERT_TRUE(std::filesystem::remove(localPath + "/css_rules.cache"));
  }
  {
    Epub reopened(path, dir.string());
    ASSERT_TRUE(reopened.load(true));
    bool truncated = false;
    ASSERT_TRUE(reopened.documentStyleCache(4, localPath, truncated));
    EXPECT_TRUE(std::filesystem::exists(localPath + "/css_rules.cache"));
  }
}

TEST(EpubFeatures, AbortedSectionDoesNotLeakItsHeadRulesIntoAnotherChapter) {
  const auto dir = std::filesystem::temp_directory_path() / "witch-epub-feature-tests" / "head-abort";
  std::filesystem::remove_all(dir);
  auto book = std::make_shared<Epub>(std::string(CORPUS_DIR) + "/test_epub_features.epub", dir.string());
  ASSERT_TRUE(book->load(true));
  GfxRenderer renderer;
  Section::BuildParams params;
  params.fontId = 1;
  params.lineCompression = 1;
  params.extraParagraphSpacing = false;
  params.paragraphAlignment = 0;
  params.viewportWidth = 460;
  params.viewportHeight = 3000;
  params.hyphenationEnabled = false;
  params.fontSizeNormalization = false;
  params.embeddedStyle = true;
  params.bionicReadingEnabled = false;
  params.imageRendering = 0;
  Section first(book, 4, renderer);
  {
    const host_clock::Ticking tick(1);
    EXPECT_NE(first.stepSectionBuild(params, 1), Section::BuildStep::Failed);
    ASSERT_TRUE(first.hasActiveBuild());
    first.abortSectionBuild();
  }
  Section second(book, 5, renderer);
  ASSERT_TRUE(second.createSectionFile(params, nullptr, false));
  ASSERT_TRUE(first.createSectionFile(params, nullptr, false));
  const auto after = featureDump("head-abort", 460, 0, true);
  EXPECT_NE(after.find("t=SHAREDONE"), std::string::npos);
  EXPECT_NE(after.find("t=sharedtwo"), std::string::npos);
  EXPECT_EQ(after, featureDump("head-clean"));
}

TEST(EpubFeatures, HeadOnlyStylesStopAtBodyAndExposeTheBoundedScanLimit) {
  const auto dir = std::filesystem::temp_directory_path() / "witch-head-only-bounds";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto path = (dir / "head-only.epub").string();
  test_zip::StoredZipWriter zip;
  zip.add("mimetype", "application/epub+zip");
  zip.add("META-INF/container.xml",
          "<container><rootfiles><rootfile full-path='content.opf' "
          "media-type='application/oebps-package+xml'/></rootfiles></container>");
  zip.add("content.opf",
          "<package><metadata xmlns:dc='http://purl.org/dc/elements/1.1/'>"
          "<dc:title>Head Styles</dc:title><dc:language>en</dc:language></metadata><manifest>"
          "<item id='a' href='Parts/a.xhtml' media-type='application/xhtml+xml'/>"
          "<item id='b' href='Parts/b.xhtml' media-type='application/xhtml+xml'/>"
          "</manifest><spine><itemref idref='a'/><itemref idref='b'/></spine></package>");
  zip.add("Parts/a.xhtml",
          "<html><head><style><![CDATA[.head{font-family:OnlyHead,serif}"
          "@font-face{font-family:OnlyHead;src:url('../Fonts/Head.ttf')}]]></style></head><body>" +
              std::string(80 * 1024, 'x') +
              "<style>@font-face{font-family:Body;src:url(body.ttf)}</style></body></html>");
  zip.add("Parts/b.xhtml",
          "<html><head><style>p{font-weight:bold}</style><!--" + std::string(70 * 1024, 'x') +
              "--><style>@font-face{font-family:BeyondLimit;src:url(late.ttf)}</style></head><body>Text</body></html>");
  zip.write(path);
  for (int reopen = 0; reopen < 2; ++reopen) {
    Epub book(path, (dir / "cache").string());
    ASSERT_TRUE(book.load(true));
    auto* global = book.getCssParser();
    ASSERT_NE(global, nullptr);
    ASSERT_TRUE(global->loadFromCache());
    EXPECT_TRUE(global->empty());
    ASSERT_EQ(global->fontCatalog().faces().size(), 1u);
    EXPECT_EQ(global->fontCatalog().faces().front().sources.front().path, "Fonts/Head.ttf");
    std::string cache;
    bool truncated = false;
    ASSERT_TRUE(book.documentStyleCache(0, cache, truncated));
    EXPECT_FALSE(truncated);
    ASSERT_FALSE(cache.empty());
    CssParser local(cache, global);
    ASSERT_TRUE(local.loadFromCache());
    EXPECT_TRUE(global->resolveStyle("p", "head", "", &local).hasFontFamily());
    EXPECT_FALSE(global->resolveStyle("p", "head").hasFontFamily());
    ASSERT_TRUE(book.documentStyleCache(1, cache, truncated));
    EXPECT_TRUE(truncated);
    ASSERT_FALSE(cache.empty());
  }
}

TEST(EpubFeatures, ItemCrcComesFromAuthoredZipPayload) {
  const auto dir = std::filesystem::temp_directory_path() / "witch-item-crc-tests";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto path = (dir / "crc-source.epub").string();
  test_zip::StoredZipWriter zip;
  zip.add("Fonts/Checksum.ttf", "123456789");
  zip.write(path);
  Epub book(path, (dir / "cache").string());
  uint32_t crc = 0;
  ASSERT_TRUE(book.getItemCrc32("Fonts/Checksum.ttf", &crc));
  EXPECT_EQ(crc, 0xcbf43926u);  // Standard CRC32 independent known-answer vector.
  ASSERT_TRUE(book.getItemCrc32("fonts/checksum.ttf", &crc));
  EXPECT_EQ(crc, 0xcbf43926u);
  EXPECT_FALSE(book.getItemCrc32("Fonts/Missing.ttf", &crc));
  EXPECT_FALSE(book.getItemCrc32("Fonts/Checksum.ttf", nullptr));
  std::filesystem::remove_all(dir);
}
