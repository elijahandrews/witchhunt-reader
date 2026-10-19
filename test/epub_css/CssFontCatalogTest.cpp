#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../lib/Epub/Epub/css/CssParser.h"
#include "BuildArena.h"

namespace {
class CssEmbeddedFonts : public ::testing::Test {
 protected:
  std::string directory;
  void SetUp() override {
    static unsigned sequence = 0;
    directory = (std::filesystem::temp_directory_path() /
                 ("witch-css-fonts-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                  "-" + std::to_string(sequence++)))
                    .string();
    std::filesystem::create_directories(directory);
  }
  void TearDown() override { std::filesystem::remove_all(directory); }
  bool append(CssParser& parser, const std::string& css, const std::string& path = "OPS/Styles/main.css") {
    const std::string temp = directory + "/source.css";
    std::ofstream(temp) << css;
    FsFile source;
    if (!Storage.openFileForRead("TEST", temp, source)) return false;
    parser.setStylesheetPath(path);
    return parser.loadFromStream(source);
  }
  bool compile(CssParser& parser, const std::string& css) {
    return parser.beginCacheCompile() && append(parser, css) && parser.endCacheCompile();
  }
};

TEST_F(CssEmbeddedFonts, OrderedNamedFallbacksAndQuotedGenericsRemainDistinct) {
  CssFontCatalog catalog;
  bool added = false;
  const uint8_t id = catalog.intern("'Missing',  Embedded Face , monospace", &added);
  ASSERT_TRUE(added);
  ASSERT_GE(id, CssFontCatalog::FIRST_NAMED_ID);
  const auto* stack = catalog.stack(id);
  ASSERT_NE(stack, nullptr);
  ASSERT_EQ(stack->families.size(), 3u);
  EXPECT_EQ(stack->families[0].name, "missing");
  EXPECT_EQ(stack->families[1].name, "embedded face");
  EXPECT_EQ(stack->families[2].generic, wordTypography::Monospace);
  EXPECT_EQ(catalog.fallbackFamily(id), wordTypography::Monospace);
  EXPECT_EQ(catalog.intern("missing, 'EMBEDDED FACE', MONOSPACE", &added), id);
  EXPECT_FALSE(added);
  EXPECT_NE(catalog.intern("Embedded Face, Missing, monospace"), id);
  EXPECT_EQ(catalog.intern("serif, 'Embedded Face'"), wordTypography::Serif);
  const auto literal = catalog.intern("'serif', sans-serif");
  ASSERT_NE(catalog.stack(literal), nullptr);
  EXPECT_EQ(catalog.stack(literal)->families.front().generic, wordTypography::Reader);
  EXPECT_EQ(catalog.fallbackFamily(literal), wordTypography::SansSerif);
  EXPECT_EQ(catalog.intern("\\45 mbedded Face, serif"), catalog.intern("'Embedded Face', serif"));
  EXPECT_EQ(catalog.intern("initial"), wordTypography::Reader);
  EXPECT_EQ(catalog.intern("unset"), wordTypography::Inherit);
  EXPECT_EQ(catalog.intern("inherit, serif"), CssFontCatalog::INVALID_ID);
}

TEST_F(CssEmbeddedFonts, StreamingFontFacesRetainCaseWeightsSourcesAndOrder) {
  CssParser parser(directory);
  ASSERT_TRUE(compile(parser, R"CSS(
    .title {font-family: "Missing", "Embedded Face", serif; font-variant-caps: all-small-caps}
    @font-face { font-family: 'Embedded Face'; font-weight: 600; font-style: italic;
      src: local("OS font"), url("https://example.invalid/remote.ttf"),
           url('../Fonts/Real%20Face.OTF?revision=1#face') format('opentype'),
           url(../Fonts/Fallback.TTF) format(truetype); }
    @font-face {font-family: 'Embedded Face'; font-weight: bold; src: url('../Fonts/Bold.ttf');}
    @media print { .ignored { font-family: 'Print'; } @font-face {font-family:'Print';src:url(print.ttf)} }
    @import 'unavailable.css';
    .mono {font-family: monospace}
  )CSS"));
  const auto style = parser.resolveStyle("p", "title");
  EXPECT_EQ(style.smallCaps, 2);
  ASSERT_NE(parser.fontCatalog().stack(style.fontFamily), nullptr);
  const auto& faces = parser.fontCatalog().faces();
  ASSERT_EQ(faces.size(), 2u);
  EXPECT_EQ(faces[0].family, "embedded face");
  EXPECT_EQ(faces[0].weight, 600);
  EXPECT_EQ(faces[0].style, CssFontCatalog::Style::Italic);
  ASSERT_EQ(faces[0].sources.size(), 2u);
  EXPECT_EQ(faces[0].sources[0].path, "OPS/Fonts/Real Face.OTF");
  EXPECT_EQ(faces[0].sources[0].format, "opentype");
  EXPECT_EQ(faces[0].sources[1].path, "OPS/Fonts/Fallback.TTF");
  EXPECT_EQ(faces[1].weight, 700);
  EXPECT_EQ(parser.resolveStyle("code", "mono").fontFamily, wordTypography::Monospace);
  EXPECT_FALSE(parser.resolveStyle("p", "ignored").hasFontFamily());
}

TEST_F(CssEmbeddedFonts, QuotedPunctuationAndReadChunkBoundariesAreData) {
  CssParser parser(directory);
  const std::string css = std::string(501, ' ') + R"CSS(
    @FONT-FACE {font-family:'A;B,/*C*/}';src:url('../Fonts/A;B,}/*C*/.ttf')}
    .odd {font-family:'A;B,/*C*/}', serif; font-variant: small-caps}
    .normal {font-family:monospace}
  )CSS";
  ASSERT_TRUE(compile(parser, css));
  ASSERT_EQ(parser.fontCatalog().faces().size(), 1u);
  EXPECT_EQ(parser.fontCatalog().faces()[0].family, "a;b,/*c*/}");
  EXPECT_EQ(parser.fontCatalog().faces()[0].sources[0].path, "OPS/Fonts/A;B,}/*C*/.ttf");
  const auto rule = parser.resolveStyle("p", "odd");
  const auto inl =
      CssParser::parseInlineStyle("font-family:'A;B,/*C*/}', serif; font-variant: all-small-caps", &parser);
  EXPECT_EQ(inl.fontFamily, rule.fontFamily);
  EXPECT_EQ(inl.smallCaps, 2);
  EXPECT_EQ(rule.smallCaps, 1);
  EXPECT_EQ(parser.resolveStyle("p", "normal").fontFamily, wordTypography::Monospace);
}

TEST_F(CssEmbeddedFonts, StacksAndCapsSurviveDiskArenaAndClearAboveTwoBitIds) {
  CssParser parser(directory);
  std::string css;
  for (unsigned i = 0; i < 80; ++i)
    css += ".f" + std::to_string(i) + "{font-family:'Family " + std::to_string(i) + "', serif; font-variant-caps:" +
           std::vector<std::string>{"normal", "small-caps", "all-small-caps", "inherit"}[i % 4] + "}\n";
  ASSERT_TRUE(compile(parser, css));
  for (int mode = 0; mode < 3; ++mode) {
    parser.clear();
    BuildArena arena(16384);
    if (mode == 2) {
      parser.setIndexArena(&arena);
      parser.setLeanResolve(true);
    }
    ASSERT_TRUE(parser.loadFromCache());
    if (mode == 2) EXPECT_TRUE(parser.isArenaResident());
    for (unsigned i = 0; i < 80; ++i) {
      const auto s = parser.resolveStyle("p", "f" + std::to_string(i));
      EXPECT_EQ(s.smallCaps, i % 4);
      EXPECT_TRUE(s.hasSmallCaps());
      EXPECT_EQ(s.fontFamily, i + CssFontCatalog::FIRST_NAMED_ID);
      const auto* stack = parser.fontCatalog().stack(s.fontFamily);
      ASSERT_NE(stack, nullptr);
      EXPECT_EQ(stack->families[0].name, "family " + std::to_string(i));
      const auto packed = wordTypography::pack(s.fontFamily, -123);
      EXPECT_EQ(wordTypography::family(packed), s.fontFamily);
      EXPECT_EQ(wordTypography::tracking(packed), -123);
    }
    parser.clear();
  }
}

TEST_F(CssEmbeddedFonts, InlineStackIsDurableBeforeItsIdEscapesAndReusesStylesheetStack) {
  CssParser parser(directory);
  ASSERT_TRUE(compile(parser, ".one{font-family:'First',serif}"));
  const uint8_t first = parser.resolveStyle("p", "one").fontFamily;
  EXPECT_EQ(CssParser::parseInlineStyle("font-family:First,serif", &parser).fontFamily, first);
  const uint8_t second = CssParser::parseInlineStyle("font-family:'Inline',monospace", &parser).fontFamily;
  EXPECT_EQ(second, first + 1);
  CssParser reopened(directory);
  ASSERT_TRUE(reopened.loadFromCache());
  EXPECT_EQ(reopened.resolveStyle("p", "one").fontFamily, first);
  ASSERT_NE(reopened.fontCatalog().stack(second), nullptr);
  EXPECT_EQ(reopened.fontCatalog().stack(second)->families[0].name, "inline");
  EXPECT_EQ(CssParser::parseInlineStyle("font-family:'Inline',monospace", &reopened).fontFamily, second);
  parser.clear();
  EXPECT_EQ(CssParser::parseInlineStyle("font-family:'Inline',monospace", &parser).fontFamily, second);
}

TEST_F(CssEmbeddedFonts, FailedInlinePersistenceNeverReturnsOrRetainsUnstableId) {
  CssParser parser("");
  for (int pass = 0; pass < 2; ++pass) {
    const auto s = CssParser::parseInlineStyle("font-family:'Unavailable',monospace", &parser);
    EXPECT_TRUE(s.hasFontFamily());
    EXPECT_EQ(s.fontFamily, wordTypography::Monospace);
    EXPECT_EQ(parser.fontCatalog().stackCount(), 0u);
  }
}

TEST_F(CssEmbeddedFonts, MissingCatalogRejectsCacheAndAbortedRebuildInvalidatesIds) {
  CssParser parser(directory);
  ASSERT_TRUE(compile(parser, ".one{font-family:First,serif}"));
  CssFontCatalog::removeFiles(directory);
  CssParser reopened(directory);
  EXPECT_FALSE(reopened.loadFromCache());
  ASSERT_TRUE(compile(parser, ".two{font-family:Second,serif}"));
  ASSERT_TRUE(parser.beginCacheCompile());
  ASSERT_TRUE(append(parser, ".three{font-family:Third,serif}"));
  parser.abortCacheCompile();
  EXPECT_FALSE(parser.hasCache());
  CssFontCatalog empty;
  EXPECT_FALSE(empty.load(directory));
}

TEST_F(CssEmbeddedFonts, AlternatingSnapshotsRecoverAfterTruncatedNewGeneration) {
  CssFontCatalog catalog;
  const uint8_t first = catalog.intern("First,serif");
  ASSERT_TRUE(catalog.save(directory));
  const uint8_t second = catalog.intern("Second,serif");
  ASSERT_TRUE(catalog.save(directory));
  std::ofstream(directory + "/css_fonts.1.bin", std::ios::binary | std::ios::trunc) << "CFNT";
  CssFontCatalog recovered;
  ASSERT_TRUE(recovered.load(directory));
  EXPECT_NE(recovered.stack(first), nullptr);
  EXPECT_EQ(recovered.stack(second), nullptr);
  EXPECT_EQ(recovered.intern("Second,serif"), second);
  ASSERT_TRUE(recovered.save(directory));
  CssFontCatalog fresh;
  ASSERT_TRUE(fresh.load(directory));
  EXPECT_NE(fresh.stack(second), nullptr);
}

TEST_F(CssEmbeddedFonts, BoundedStacksUseGenericFallbackAndPersistTruncation) {
  CssFontCatalog catalog;
  for (unsigned i = 0; i < CssFontCatalog::MAX_STACKS; ++i)
    EXPECT_EQ(catalog.intern("f" + std::to_string(i) + ",monospace"), CssFontCatalog::FIRST_NAMED_ID + i);
  EXPECT_EQ(catalog.intern("overflow,monospace"), wordTypography::Monospace);
  EXPECT_TRUE(catalog.truncated());
  ASSERT_TRUE(catalog.save(directory));
  CssFontCatalog recovered;
  ASSERT_TRUE(recovered.load(directory));
  EXPECT_TRUE(recovered.truncated());
  EXPECT_EQ(recovered.stackCount(), CssFontCatalog::MAX_STACKS);
  EXPECT_EQ(wordTypography::family(wordTypography::pack(254, 0)), 254);
}

TEST_F(CssEmbeddedFonts, RejectsRemoteTraversalAndRetainsSeparateStylesheetBases) {
  CssParser parser(directory);
  ASSERT_TRUE(parser.beginCacheCompile());
  ASSERT_TRUE(append(
      parser,
      R"CSS(@font-face{font-family:One;src:url('../../../escape.ttf'),url('//example.invalid/x.ttf'),url(data:ignored),url('../Fonts/One.otf')})CSS"));
  ASSERT_TRUE(append(parser, R"CSS(@font-face{font-family:Two;src:url('./Two.ttf')})CSS", "Other/CSS/fonts.css"));
  ASSERT_TRUE(parser.endCacheCompile());
  const auto& faces = parser.fontCatalog().faces();
  ASSERT_EQ(faces.size(), 2u);
  ASSERT_EQ(faces[0].sources.size(), 1u);
  EXPECT_EQ(faces[0].sources[0].path, "OPS/Fonts/One.otf");
  EXPECT_EQ(faces[1].sources[0].path, "Other/CSS/Two.ttf");
}
TEST_F(CssEmbeddedFonts, WhiteSpaceModesSurviveBothCacheCodecs) {
  CssParser parser(directory);
  const std::vector<std::string> values = {"normal", "pre", "pre-wrap", "pre-line", "nowrap", "inherit"};
  std::string css;
  for (size_t i = 0; i < values.size(); ++i) css += ".w" + std::to_string(i) + "{white-space:" + values[i] + "}";
  ASSERT_TRUE(compile(parser, css));
  for (int mode = 0; mode < 2; ++mode) {
    parser.clear();
    BuildArena arena(8192);
    if (mode) {
      parser.setIndexArena(&arena);
      parser.setLeanResolve(true);
    }
    ASSERT_TRUE(parser.loadFromCache());
    for (size_t i = 0; i < values.size(); ++i) {
      const auto style = parser.resolveStyle("span", "w" + std::to_string(i));
      EXPECT_TRUE(style.hasWhiteSpace());
      EXPECT_EQ(static_cast<unsigned>(style.whiteSpace), i);
    }
    parser.clear();
  }
  EXPECT_EQ(CssParser::parseInlineStyle("white-space:pre;white-space:unset").whiteSpace, CssWhiteSpace::Inherit);
  EXPECT_EQ(CssParser::parseInlineStyle("white-space:pre;white-space:initial").whiteSpace, CssWhiteSpace::Normal);
  EXPECT_FALSE(CssParser::parseInlineStyle("white-space:invalid").hasWhiteSpace());
}
}  // namespace

TEST_F(CssEmbeddedFonts, DocumentOverlayPreservesSpecificityAndScopesNamedFaces) {
  CssParser book(directory);
  ASSERT_TRUE(book.beginCacheCompile());
  ASSERT_TRUE(append(book, ".shared{font-family:Alias,serif;font-style:italic} #priority{font-weight:bold}"));
  std::filesystem::create_directories(directory + "/a");
  std::filesystem::create_directories(directory + "/b");
  CssParser first(directory + "/a", &book), second(directory + "/b", &book);
  const auto compileLocal = [&](CssParser& parser, const std::string& source) {
    const auto path = directory + "/head.css";
    std::ofstream(path) << ".shared{font-family:Alias,serif;font-style:normal} p{font-weight:normal}"
                           "@font-face{font-family:Alias;src:url(" +
                               source + ")}";
    FsFile file;
    if (!Storage.openFileForRead("T", path, file) || !parser.beginCacheCompile()) return false;
    parser.setStylesheetPath("OPS/Heads/part.xhtml");
    return parser.appendCompiledFromStream(file, true) && file.seek(0) && parser.appendCompiledFromStream(file) &&
           parser.endCacheCompile();
  };
  ASSERT_TRUE(compileLocal(first, "../Fonts/one.ttf"));
  ASSERT_TRUE(compileLocal(second, "../Fonts/two.ttf"));
  ASSERT_TRUE(book.endCacheCompile());
  const auto global = book.resolveStyle("p", "shared", "priority");
  const auto one = book.resolveStyle("p", "shared", "priority", &first);
  const auto two = book.resolveStyle("p", "shared", "priority", &second);
  EXPECT_EQ(one.fontWeight, CssFontWeight::Bold);
  EXPECT_EQ(one.fontStyle, CssFontStyle::Normal);
  EXPECT_EQ(global.fontStyle, CssFontStyle::Italic);
  EXPECT_NE(one.fontFamily, two.fontFamily);
  EXPECT_NE(one.fontFamily, global.fontFamily);
  const auto* firstStack = book.fontCatalog().stack(one.fontFamily);
  const auto* secondStack = book.fontCatalog().stack(two.fontFamily);
  ASSERT_NE(firstStack, nullptr);
  ASSERT_NE(secondStack, nullptr);
  EXPECT_NE(firstStack->families[0].name, secondStack->families[0].name);
  book.clear();
  first.clear();
  second.clear();
  ASSERT_TRUE(book.loadFromCache());
  ASSERT_TRUE(first.loadFromCache());
  ASSERT_TRUE(second.loadFromCache());
  EXPECT_EQ(book.resolveStyle("p", "shared", "priority", &first).fontFamily, one.fontFamily);
  EXPECT_EQ(book.resolveStyle("p", "shared", "priority", &second).fontFamily, two.fontFamily);
  EXPECT_EQ(book.resolveStyle("p", "shared", "priority").fontFamily, global.fontFamily);
}

TEST_F(CssEmbeddedFonts, CatalogOnlyModeNeverLeaksRulesOrBackgrounds) {
  CssParser book(directory);
  ASSERT_TRUE(book.beginCacheCompile());
  const auto path = directory + "/source.css";
  std::ofstream(path) << ".secret {font-family:Leak,serif;background:url(secret.png) no-repeat;font-weight:bold}"
                         "@font-face{font-family:Face;src:url(face.ttf)}";
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("T", path, file));
  ASSERT_TRUE(book.appendCompiledFromStream(file, true));
  ASSERT_TRUE(book.endCacheCompile());
  EXPECT_FALSE(book.resolveStyle("p", "secret").defined.anySet());
  EXPECT_EQ(book.backgroundImageFor("p", "secret"), nullptr);
  EXPECT_EQ(book.fontCatalog().stackCount(), 0u);
  EXPECT_EQ(book.fontCatalog().faces().size(), 1u);
}

TEST_F(CssEmbeddedFonts, LocalBackgroundNoneAndImagesRespectSpecificityAfterReload) {
  CssParser book(directory);
  ASSERT_TRUE(
      compile(book, ".picture{background:url(base.png) no-repeat} #fixed{background:url(fixed.png) no-repeat}"));
  std::filesystem::create_directories(directory + "/local");
  CssParser local(directory + "/local", &book);
  ASSERT_TRUE(compile(local, ".picture{background:none} p{background:url(local.png) no-repeat}"));
  for (int reload = 0; reload < 2; ++reload) {
    EXPECT_EQ(book.backgroundImageFor("p", "picture", "", &local), nullptr);
    const auto* fixed = book.backgroundImageFor("p", "picture", "fixed", &local);
    ASSERT_NE(fixed, nullptr);
    EXPECT_EQ(*fixed, "OPS/Styles/fixed.png");
    const auto* image = book.backgroundImageFor("p", "", "", &local);
    ASSERT_NE(image, nullptr);
    EXPECT_EQ(*image, "OPS/Styles/local.png");
    local.clear();
    book.clear();
    ASSERT_TRUE(book.loadFromCache());
    ASSERT_TRUE(local.loadFromCache());
  }
}

TEST_F(CssEmbeddedFonts, RepeatedHeadFacesAcrossOneHundredChaptersReuseCatalogBindings) {
  CssParser book(directory);
  ASSERT_TRUE(book.beginCacheCompile());
  ASSERT_TRUE(append(book, ".global{font-family:Repeat,serif}"));
  uint8_t shared = 0;
  for (int chapter = 0; chapter < 100; ++chapter) {
    const auto dir = directory + "/" + std::to_string(chapter);
    std::filesystem::create_directories(dir);
    CssParser local(dir, &book);
    const auto path = directory + "/head.css";
    std::ofstream(path) << ".local{font-weight:" << (chapter % 2 ? "bold" : "normal")
                        << ";font-family:Repeat,serif}"
                           "@font-face{font-family:Repeat;src:url(../Fonts/Repeat.ttf)}";
    FsFile file;
    ASSERT_TRUE(Storage.openFileForRead("T", path, file));
    ASSERT_TRUE(local.beginCacheCompile());
    local.setStylesheetPath("OPS/Heads/ch.xhtml");
    ASSERT_TRUE(local.appendCompiledFromStream(file, true));
    ASSERT_TRUE(file.seek(0));
    ASSERT_TRUE(local.appendCompiledFromStream(file));
    ASSERT_TRUE(local.endCacheCompile());
    const auto id = book.resolveStyle("p", "global", "", &local).fontFamily;
    if (chapter)
      EXPECT_EQ(id, shared);
    else
      shared = id;
    EXPECT_FALSE(local.rulesTruncated());
    EXPECT_EQ(book.fontCatalog().faces().size(), 1u);
    EXPECT_EQ(book.fontCatalog().stackCount(), 2u);
  }
  EXPECT_FALSE(book.fontCatalog().truncated());
  ASSERT_TRUE(book.endCacheCompile());
}

TEST_F(CssEmbeddedFonts, ScopedCatalogSaveFailureLatchesDegradationAndRetryUsesTheFont) {
  CssParser book(directory);
  ASSERT_TRUE(compile(book, ".global{font-family:OnlyInHead,serif}"));
  std::filesystem::create_directories(directory + "/local");
  CssParser local(directory + "/local", &book);
  ASSERT_TRUE(local.beginCacheCompile());
  const auto path = directory + "/head.css";
  std::ofstream(path) << "@font-face{font-family:OnlyInHead;src:url(font.ttf)}";
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("T", path, file));
  ASSERT_TRUE(local.appendCompiledFromStream(file, true));
  ASSERT_TRUE(local.endCacheCompile());
  file.close();
  // Keep the already-resolved global selector available while only catalog persistence fails.
  ASSERT_GE(book.resolveStyle("p", "global").fontFamily, CssFontCatalog::FIRST_NAMED_ID);
  const auto before = book.fontCatalogFailureEpoch();
  std::filesystem::rename(directory, directory + "-held");
  std::ofstream(directory) << "block directory recreation";
  EXPECT_EQ(book.resolveStyle("p", "global", "", &local).fontFamily, wordTypography::Serif);
  EXPECT_NE(book.fontCatalogFailureEpoch(), before);
  const auto failed = book.fontCatalogFailureEpoch();
  std::filesystem::remove(directory);
  std::filesystem::rename(directory + "-held", directory);
  EXPECT_GE(book.resolveStyle("p", "global", "", &local).fontFamily, CssFontCatalog::FIRST_NAMED_ID);
  EXPECT_EQ(book.fontCatalogFailureEpoch(), failed);
}

TEST_F(CssEmbeddedFonts, HeadCatalogLimitIsPersistedAsSimplifiedWithoutRetry) {
  CssParser book(directory);
  ASSERT_TRUE(book.beginCacheCompile());
  std::filesystem::create_directories(directory + "/local");
  CssParser local(directory + "/local", &book);
  ASSERT_TRUE(local.beginCacheCompile());
  const auto path = directory + "/head.css";
  {
    std::ofstream source(path);
    for (unsigned i = 0; i < CssFontCatalog::MAX_FACES + 1; ++i)
      source << "@font-face{font-family:F" << i << ";src:url(f" << i << ".ttf)}\n";
  }
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("T", path, file));
  ASSERT_TRUE(local.appendCompiledFromStream(file, true));
  ASSERT_TRUE(local.endCacheCompile());
  ASSERT_TRUE(book.endCacheCompile());
  EXPECT_TRUE(local.rulesTruncated());
  EXPECT_TRUE(book.fontCatalog().truncated());
  local.clear();
  book.clear();
  ASSERT_TRUE(book.loadFromCache());
  ASSERT_TRUE(local.loadFromCache());
  EXPECT_TRUE(local.rulesTruncated());
}
