#include <Arduino.h>
#include <CssParser.h>
#include <gtest/gtest.h>
TEST(CssParity, CatalogOrderedNativeTokens) {
  fakeheap::reset();
  Storage.reset();
  CssFontCatalog catalog;
  const auto id = catalog.intern("Missing, 'Named, face', serif");
  ASSERT_NE(id, 0u);
  ASSERT_NE(id, CssFontCatalog::INVALID_ID);
  const auto* stack = catalog.stack(id);
  ASSERT_NE(stack, nullptr);
  ASSERT_EQ(stack->families.size(), 3u);
  EXPECT_EQ(stack->families[0].name, "missing");
  EXPECT_EQ(stack->families[1].name, "named, face");
  EXPECT_EQ(stack->families[2].generic, genericFont::Serif);
  EXPECT_EQ(catalog.intern("BODY"), CssParser::hashFontFamily("body"));
  EXPECT_NE(catalog.intern("'serif'"), genericFont::Serif);
}

struct CssParityTest : testing::Test {
  void SetUp() override {
    fakeheap::reset();
    Storage.reset();
  }
  bool parse(CssParser& css, const std::string& text, const std::string& directory = "EPUB/css/",
             bool fontsOnly = false) {
    Storage.put("source.css", {text.begin(), text.end()});
    HalFile input;
    if (!Storage.openFileForRead("test", "source.css", input)) return false;
    const bool ok = css.loadFromStream(input, directory, fontsOnly);
    input.close();
    return ok;
  }
};
TEST_F(CssParityTest, SourceCandidatesDescriptorsAndOrderSurviveBothCodecs) {
  CssParser css("book");
  ASSERT_TRUE(parse(css,
                    "@font-face{font-family:'Body';font-weight:450;font-style:oblique;src:local('No'),url('../fonts/"
                    "missing.ttf') format('truetype'), url('../fonts/face.otf') format('opentype');} "
                    "@font-face{font-family:Body;font-weight:450;font-style:oblique;src:url(../fonts/later.ttf)} "
                    "p{font-family:Missing,Body,serif}"));
  ASSERT_TRUE(css.saveToCache());
  CssFontCatalog catalog;
  ASSERT_TRUE(CssParser::loadFontCatalog("book", catalog));
  ASSERT_EQ(catalog.faces().size(), 2u);
  const auto& face = catalog.faces()[0];
  EXPECT_EQ(face.family, CssParser::hashFontFamily("Body"));
  EXPECT_EQ(face.weight, 450);
  EXPECT_EQ(face.style, CssFontCatalog::Style::Oblique);
  ASSERT_EQ(face.sources.size(), 2u);
  EXPECT_EQ(face.sources[0].path, "EPUB/fonts/missing.ttf");
  EXPECT_EQ(face.sources[0].format, "truetype");
  EXPECT_EQ(face.sources[1].path, "EPUB/fonts/face.otf");
  EXPECT_EQ(catalog.faces()[1].declarationOrder, 2u);
  const auto token = css.resolveStyle("p", "").fontFamily;
  ASSERT_NE(catalog.stack(token), nullptr);
  const auto signature = catalog.signature();
  ASSERT_TRUE(catalog.save("side"));
  CssFontCatalog restored;
  ASSERT_TRUE(restored.load("side"));
  EXPECT_EQ(restored.signature(), signature);
}
TEST_F(CssParityTest, ScopedHeadsDoNotRenameUnrelatedFallbackMembers) {
  CssParser book("book"), local("local", &book);
  local.setFontScope("doc7:");
  const std::string source = "p{font-family:Body,Other,serif}@font-face{font-family:Body;src:url(font.ttf)}";
  ASSERT_TRUE(parse(local, source, "EPUB/chapters/", true));
  ASSERT_TRUE(parse(local, source, "EPUB/chapters/"));
  const auto token = local.resolveStyle("p", "").fontFamily;
  const auto* stack = book.fontCatalog().stack(token);
  ASSERT_NE(stack, nullptr);
  ASSERT_EQ(stack->families.size(), 3u);
  EXPECT_EQ(stack->families[0].name, "doc7:body");
  EXPECT_EQ(stack->families[1].name, "other");
  EXPECT_EQ(stack->families[2].generic, genericFont::Serif);
  ASSERT_TRUE(local.saveToCache());
  local.clear();
  ASSERT_TRUE(local.loadFromCache());
  EXPECT_EQ(local.resolveStyle("p", "").fontFamily, token);
}
TEST_F(CssParityTest, ExtendedFieldsSurviveResidentAndDiskCaches) {
  const std::string source =
      "p "
      "{white-space:pre-line;line-height:180%;color:rgba(0,0,0,0);opacity:0;visibility:hidden;min-height:2em;max-"
      "height:70%;max-width:none;background-color:#888;float:right;clear:both;break-inside:avoid-page;hyphens:auto;"
      "hyphenate-limit-before:3;hyphenate-limit-after:4;hyphenate-limit-lines:2;font-variant-caps:all-small-caps;font-"
      "variant-ligatures:none;}";
  for (int mode = 0; mode < 3; ++mode) {
    fakeheap::reset(mode != 0);
    CssParser css("book");
    ASSERT_TRUE(parse(css, source));
    ASSERT_TRUE(css.saveToCache());
    css.clear();
    if (mode == 2) fakeheap::external.fail = 1;
    ASSERT_TRUE(css.loadFromCache());
    const auto s = css.resolveStyle("p", "");
    EXPECT_EQ(s.whiteSpace, CssWhiteSpace::PreLine);
    EXPECT_FLOAT_EQ(s.lineHeightMultiplier, 1.2f);
    EXPECT_TRUE(s.isTextTransparent());
    EXPECT_TRUE(s.isElementHidden());
    EXPECT_EQ(s.minHeight.value, 2);
    EXPECT_EQ(s.minHeight.unit, CssUnit::Em);
    EXPECT_EQ(s.maxHeight.value, 70);
    EXPECT_EQ(s.maxWidth.unit, CssUnit::Auto);
    EXPECT_EQ(s.backgroundGray, 136);
    EXPECT_EQ(s.cssFloat, CssFloat::Right);
    EXPECT_EQ(s.cssClear, CssClear::Both);
    EXPECT_TRUE(s.breakInsideAvoid);
    EXPECT_EQ(s.hyphens, CssHyphens::Auto);
    EXPECT_EQ(s.hyphenateLimitBefore, 3);
    EXPECT_EQ(s.hyphenateLimitAfter, 4);
    EXPECT_EQ(s.hyphenateLimitLines, 2);
    EXPECT_EQ(s.fontVariantCaps, CssFontVariantCaps::AllSmallCaps);
    EXPECT_EQ(s.ligatures, CssLigatures::None);
  }
}
TEST_F(CssParityTest, WhiteSpaceResetAndInheritanceRemainDistinct) {
  const char* modes[] = {"normal", "pre", "pre-wrap", "pre-line", "nowrap", "inherit"};
  for (uint8_t i = 0; i < 6; ++i) {
    auto s = CssParser::parseInlineStyle(std::string("white-space:") + modes[i]);
    EXPECT_EQ(s.whiteSpace, static_cast<CssWhiteSpace>(i));
    EXPECT_TRUE(s.defined.whiteSpace);
  }
  auto s = CssParser::parseInlineStyle(
      "line-height:inherit;white-space:unset;hyphens:initial;hyphenate-limit-lines:inherit;clear:inherit;break-inside:"
      "inherit;font-variant:none");
  EXPECT_EQ(s.lineHeightMultiplier, 0);
  EXPECT_EQ(s.whiteSpace, CssWhiteSpace::Inherit);
  EXPECT_EQ(s.hyphens, CssHyphens::Manual);
  EXPECT_EQ(s.hyphenateLimitLines, 255);
  EXPECT_EQ(s.cssClear, CssClear::Inherit);
  EXPECT_TRUE(s.breakInsideInherit);
  EXPECT_EQ(s.ligatures, CssLigatures::None);
  s = CssParser::parseInlineStyle(
      "line-height:normal;font-variant-caps:inherit;max-width:20oops;hyphenate-limit-before:0");
  EXPECT_EQ(s.lineHeightMultiplier, 1);
  EXPECT_EQ(s.fontVariantCaps, CssFontVariantCaps::Inherit);
  EXPECT_FALSE(s.defined.maxWidth);
  EXPECT_FALSE(s.defined.hyphenateLimitBefore);
}
TEST_F(CssParityTest, TinyInvisibleTextPreservesNormalSmallText) {
  EXPECT_TRUE(CssParser::parseInlineStyle("font-size:1px").isFontSizeInvisible());
  EXPECT_TRUE(CssParser::parseInlineStyle("font-size:1pt").isFontSizeInvisible());
  EXPECT_FALSE(CssParser::parseInlineStyle("font-size:.1em").isFontSizeInvisible());
  EXPECT_FALSE(CssParser::parseInlineStyle("font-size:xx-small").isFontSizeInvisible());
  auto s = CssParser::parseInlineStyle("color:#1230");
  EXPECT_TRUE(s.isTextTransparent());
  EXPECT_FALSE(s.isElementHidden());
}
TEST_F(CssParityTest, LiteralPseudoAndBackgroundPathsSurviveQuotesAndCache) {
  CssParser css("book");
  ASSERT_TRUE(
      parse(css,
            "p::before{content:'A;{}/*not comment*/' '\\201c';font-variant-caps:all-small-caps} p::after{content:'B'} "
            "p::first-line{letter-spacing:.2em} p{background:url('../images/back.png') no-repeat;}"));
  ASSERT_TRUE(css.saveToCache());
  css.clear();
  ASSERT_TRUE(css.loadFromCache());
  ASSERT_TRUE(css.pseudoContentFor("p", "", "before"));
  EXPECT_EQ(*css.pseudoContentFor("p", "", "before"), "A;{}/*not comment*/\xe2\x80\x9c");
  EXPECT_EQ(*css.pseudoContentFor("p", "", "after"), "B");
  EXPECT_FALSE(css.resolveStyle("p", "").defined.pseudoContent);
  EXPECT_EQ(css.resolvePseudoStyle("p", "", "before").fontVariantCaps, CssFontVariantCaps::AllSmallCaps);
  EXPECT_EQ(css.resolvePseudoStyle("p", "", "first-line").letterSpacing.value, .2f);
  EXPECT_EQ(*css.backgroundImageFor("p", ""), "EPUB/images/back.png");
  EXPECT_EQ(css.resolveStyle("p", "").backgroundRepeat, CssBackgroundRepeat::NoRepeat);
}
TEST_F(CssParityTest, LocalOverlayWinsOnlyEqualOrLowerSpecificity) {
  CssParser book("book"), local("local", &book);
  ASSERT_TRUE(parse(
      book,
      "p.note{line-height:2.4} #strong{line-height:3} p{background-image:url(global.png)} p::before{content:'book'}"));
  ASSERT_TRUE(
      parse(local, "p{line-height:1.5;background-image:none} p.note{line-height:1.8} p::before{content:'local'}"));
  ASSERT_TRUE(book.saveToCache());
  ASSERT_TRUE(local.saveToCache());
  book.clear();
  local.clear();
  ASSERT_TRUE(book.loadFromCache());
  ASSERT_TRUE(local.loadFromCache());
  EXPECT_FLOAT_EQ(book.resolveStyle("p", "note", {}, "", false, &local).lineHeightMultiplier, 1.2f);
  EXPECT_FLOAT_EQ(book.resolveStyle("p", "note", {}, "strong", false, &local).lineHeightMultiplier, 2.0f);
  EXPECT_EQ(*book.backgroundImageFor("p", "", {}, "", &local), "");
  EXPECT_EQ(*book.pseudoContentFor("p", "", "before", {}, "", &local), "local");
}
TEST_F(CssParityTest, MalformedAndOversizedMetadataCannotPublishPartialTokens) {
  CssFontCatalog catalog;
  EXPECT_EQ(catalog.intern("'unterminated"), CssFontCatalog::INVALID_ID);
  EXPECT_EQ(catalog.intern("url(nope)"), CssFontCatalog::INVALID_ID);
  EXPECT_EQ(catalog.stackCount(), 0u);
  CssParser css("book");
  ASSERT_TRUE(
      parse(css, "@font-face{font-family:Body;src:url(https://invalid.invalid/font.ttf),url(../../../../escape.ttf)}"));
  EXPECT_TRUE(css.fontCatalog().faces().empty());
  EXPECT_FALSE(parse(css, "p{content:'unterminated"));
  EXPECT_FALSE(parse(css, "p::before{content:'" + std::string(5000, 'x') + "'}"));
}
TEST_F(CssParityTest, CacheCorruptionAndOldVersionAreRejected) {
  CssParser css("book");
  ASSERT_TRUE(parse(css, "p{line-height:1.8}p::before{content:'literal'}@font-face{font-family:Face;src:url(a.ttf)}"));
  ASSERT_TRUE(css.saveToCache());
  const auto original = Storage.bytes("book/css_rules.cache");
  for (size_t n : {size_t(0), size_t(7), original.size() - 1}) {
    Storage.put("book/css_rules.cache", {original.begin(), original.begin() + n});
    EXPECT_EQ(css.inspectCache(), CssParser::CacheStatus::Invalid);
  }
  auto corrupt = original;
  corrupt[4] = CssParser::CSS_CACHE_VERSION - 1;
  Storage.put("book/css_rules.cache", corrupt);
  EXPECT_EQ(css.inspectCache(), CssParser::CacheStatus::Invalid);
  corrupt = original;
  corrupt[corrupt.size() - 6] ^= 0x40;
  Storage.put("book/css_rules.cache", corrupt);
  EXPECT_EQ(css.inspectCache(), CssParser::CacheStatus::Invalid);
  CssFontCatalog output;
  EXPECT_FALSE(CssParser::loadFontCatalog("book", output));
}
TEST_F(CssParityTest, InlineAppendSurvivesRuleReleaseAndColdReload) {
  CssParser css("book");
  ASSERT_TRUE(parse(css, "p{font-family:Body,serif}"));
  ASSERT_TRUE(css.saveToCache());
  const auto s = css.parseInlineStyleWithFonts("font-family:'Inline Font',sans-serif");
  ASSERT_NE(css.fontCatalog().stack(s.fontFamily), nullptr);
  css.clear();
  ASSERT_NE(css.fontCatalog().stack(s.fontFamily), nullptr);
  CssParser cold("book");
  ASSERT_NE(cold.fontCatalog().stack(s.fontFamily), nullptr);
}
TEST_F(CssParityTest, LowMemoryRefusesNewCatalogButInspectionRemainsStreaming) {
  CssParser css("book");
  ASSERT_TRUE(parse(css, "@font-face{font-family:Face;src:url(font.ttf)}p{font-family:Face,serif}"));
  ASSERT_TRUE(css.saveToCache());
  fakeheap::internal.free = 48 * 1024;
  EXPECT_EQ(css.inspectCache(), CssParser::CacheStatus::Complete);
  CssFontCatalog catalog;
  EXPECT_EQ(catalog.intern("Refused,serif"), 0u);
  EXPECT_TRUE(catalog.truncated());
  EXPECT_EQ(catalog.stackCount(), 0u);
}

TEST_F(CssParityTest, InitialCatalogMemoryFailureCanRecoverWithoutReopeningBook) {
  CssParser writer("book");
  ASSERT_TRUE(parse(writer, "@font-face{font-family:Face;src:url(face.ttf)}p{font-family:Face,serif}"));
  ASSERT_TRUE(writer.saveToCache());
  CssParser reader("book");
  fakeheap::internal.free = 70 * 1024;
  EXPECT_TRUE(reader.fontCatalog().faces().empty());
  EXPECT_FALSE(reader.fontCatalogReady());
  fakeheap::internal.free = 1024 * 1024;
  EXPECT_EQ(reader.fontCatalog().faces().size(), 1u);
  EXPECT_TRUE(reader.fontCatalogReady());
}

TEST_F(CssParityTest, ColdCatalogBudgetsNestedShortNameObjectsBeforeHydration) {
  CssFontCatalog dense;
  for (unsigned i = 0; i < CssFontCatalog::MAX_STACKS; ++i) {
    std::string value = "x" + std::to_string(i);
    for (unsigned j = 1; j < 16; ++j) value += ",a";
    ASSERT_NE(dense.intern(value), CssFontCatalog::INVALID_ID);
  }
  ASSERT_EQ(dense.stackCount(), CssFontCatalog::MAX_STACKS);
  ASSERT_TRUE(dense.save("dense"));
  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("test", "dense/css_fonts.0.bin", file));
  size_t hydration = 0;
  ASSERT_TRUE(CssFontCatalog::inspect(file, &hydration));
  EXPECT_GT(hydration, 128u * 1024u);
  ASSERT_TRUE(file.seek(0));
  CssFontCatalog reader;
  const auto stable = reader.intern("Stable");
  fakeheap::internal.free = 180 * 1024;
  EXPECT_FALSE(reader.read(file));
  EXPECT_NE(reader.stack(stable), nullptr);
  file.close();
  CssFontCatalog cold;
  EXPECT_FALSE(cold.load("dense"));
  EXPECT_EQ(cold.stackCount(), 0u);
  fakeheap::internal.free = 1024 * 1024;
  ASSERT_TRUE(cold.load("dense"));
  EXPECT_EQ(cold.stackCount(), CssFontCatalog::MAX_STACKS);
}
