#pragma once

#include <WordTypography.h>

#include <algorithm>
#include <cstdint>

// Matches order of PARAGRAPH_ALIGNMENT in CrossPointSettings
enum class CssTextAlign : uint8_t { Justify = 0, Left = 1, Center = 2, Right = 3, None = 4 };
// Auto is not a length: it is the marker for an explicit `width: auto` / `height: auto`, which
// must beat a length set earlier in the cascade rather than be ignored as unparseable.
// imageWidth/imageHeight report it as unset; letterSpacing uses it as the explicit
// inherit/unset marker. The element resolver handles it before toPixels().
enum class CssUnit : uint8_t { Pixels = 0, Em = 1, Rem = 2, Points = 3, Percent = 4, Auto = 5, Inherit = 6 };

// Represents a CSS length value with its unit, allowing deferred resolution to pixels
struct CssLength {
  float value = 0.0f;
  CssUnit unit = CssUnit::Pixels;

  CssLength() = default;
  CssLength(const float v, const CssUnit u) : value(v), unit(u) {}

  // Convenience constructor for pixel values (most common case)
  explicit CssLength(const float pixels) : value(pixels) {}

  // Returns true if this length can be resolved to pixels with the given context.
  // Percentage units require a non-zero containerWidth to resolve.
  [[nodiscard]] bool isResolvable(const float containerWidth = 0) const {
    return unit != CssUnit::Percent || containerWidth > 0;
  }

  // Resolve to pixels given the current em size (font line height)
  // containerWidth is needed for percentage units (e.g. viewport width)
  [[nodiscard]] float toPixels(const float emSize, const float containerWidth = 0) const {
    switch (unit) {
      case CssUnit::Em:
      case CssUnit::Rem:
        return value * emSize;
      case CssUnit::Points:
        return value * 1.33f;  // Approximate pt to px conversion
      case CssUnit::Percent:
        return value * containerWidth / 100.0f;
      default:
        return value;
    }
  }

  // Resolve to int16_t pixels (for BlockStyle fields)
  [[nodiscard]] int16_t toPixelsInt16(const float emSize, const float containerWidth = 0) const {
    const float pixels = toPixels(emSize, containerWidth);
    if (pixels != pixels) return 0;  // Invalid authored values must not reach a narrowing conversion.
    return static_cast<int16_t>(std::max(-32768.0f, std::min(32767.0f, pixels)));
  }
};

// Font style options matching CSS font-style property
enum class CssFontStyle : uint8_t { Normal = 0, Italic = 1 };

// Font weight options - CSS supports 100-900, we simplify to normal/bold
enum class CssFontWeight : uint8_t { Normal = 0, Bold = 1 };

// Text decoration options
enum class CssTextDecoration : uint8_t { None = 0, Underline = 1, LineThrough = 2, UnderlineLineThrough = 3 };

// Display options. Block and Inline only matter where the tag does not already decide it: a
// <span> styled display:block is laid out as a block, one styled display:inline is not.
enum class CssDisplay : uint8_t { Block = 0, None = 1, Inline = 2, InlineBlock = 3 };

// Vertical align options relevant for inline text positioning
enum class CssVerticalAlign : uint8_t { Baseline = 0, Super = 1, Sub = 2 };

// Float options — used to detect inline images beside paragraph text
enum class CssFloat : uint8_t { None = 0, Left = 1, Right = 2 };

enum class CssClear : uint8_t { None = 0, Left = 1, Right = 2, Both = 3, Inherit = 4 };
enum class CssHyphens : uint8_t { Inherit = 0, None = 1, Manual = 2, Auto = 3 };
enum class CssLigatures : uint8_t { Inherit = 0, Normal = 1, None = 2 };
enum class CssBorderStyle : uint8_t { None = 0, Solid = 1, Inherit = 2 };

// Inherit is kept explicitly so it can cancel an earlier declaration in the cascade.
enum class CssTextTransform : uint8_t { None = 0, Uppercase = 1, Lowercase = 2, Inherit = 3 };
enum class CssWhiteSpace : uint8_t { Normal = 0, Pre = 1, PreWrap = 2, PreLine = 3, NoWrap = 4, Inherit = 5 };

// Bitmask for tracking which properties have been explicitly set
struct CssPropertyFlags {
  uint32_t textAlign : 1;
  uint32_t fontStyle : 1;
  uint32_t fontWeight : 1;
  uint32_t textDecoration : 1;
  uint32_t textIndent : 1;
  uint32_t marginTop : 1;
  uint32_t marginBottom : 1;
  uint32_t marginLeft : 1;
  uint32_t marginRight : 1;
  uint32_t paddingTop : 1;
  uint32_t paddingBottom : 1;
  uint32_t paddingLeft : 1;
  uint32_t paddingRight : 1;
  uint32_t imageHeight : 1;
  uint32_t imageWidth : 1;
  uint32_t display : 1;
  uint32_t verticalAlign : 1;
  uint32_t listStyleNone : 1;
  uint32_t pageBreakBefore : 1;
  uint32_t pageBreakAfter : 1;
  uint32_t lineHeight : 1;
  uint32_t fontSizeMultiplier : 1;
  uint32_t cssFloat : 1;
  uint32_t smallCaps : 1;
  uint32_t textTransform : 1;
  uint32_t fontFamily : 1;
  uint32_t letterSpacing : 1;
  uint32_t whiteSpace : 1;
  // Invisibility. Three bits, not one: they come from three properties that cascade
  // independently, and a shared bit would let a later `visibility: visible` cancel an
  // `opacity: 0` (or vice versa) purely by declaration order.
  uint32_t colorTransparent : 1;
  uint32_t opacityZero : 1;
  uint32_t visibilityHidden : 1;

  uint32_t minHeight : 1;
  uint32_t maxHeight : 1;
  uint32_t maxWidth : 1;
  uint32_t borderTopWidth : 1;
  uint32_t borderBottomWidth : 1;
  uint32_t borderTopStyle : 1;
  uint32_t borderBottomStyle : 1;
  uint32_t backgroundGray : 1;
  uint32_t breakInsideAvoid : 1;
  uint32_t cssClear : 1;
  uint32_t hyphens : 1;
  uint32_t ligatures : 1;
  uint32_t hyphenateLimitBefore : 1;
  uint32_t hyphenateLimitAfter : 1;
  uint32_t hyphenateLimitLines : 1;

  CssPropertyFlags()
      : textAlign(0),
        fontStyle(0),
        fontWeight(0),
        textDecoration(0),
        textIndent(0),
        marginTop(0),
        marginBottom(0),
        marginLeft(0),
        marginRight(0),
        paddingTop(0),
        paddingBottom(0),
        paddingLeft(0),
        paddingRight(0),
        imageHeight(0),
        imageWidth(0),
        display(0),
        verticalAlign(0),
        listStyleNone(0),
        pageBreakBefore(0),
        pageBreakAfter(0),
        lineHeight(0),
        fontSizeMultiplier(0),
        cssFloat(0),
        smallCaps(0),
        textTransform(0),
        fontFamily(0),
        letterSpacing(0),
        whiteSpace(0),
        colorTransparent(0),
        opacityZero(0),
        visibilityHidden(0),
        minHeight(0),
        maxHeight(0),
        maxWidth(0),
        borderTopWidth(0),
        borderBottomWidth(0),
        borderTopStyle(0),
        borderBottomStyle(0),
        backgroundGray(0),
        breakInsideAvoid(0),
        cssClear(0),
        hyphens(0),
        ligatures(0),
        hyphenateLimitBefore(0),
        hyphenateLimitAfter(0),
        hyphenateLimitLines(0) {}

  [[nodiscard]] bool anySet() const {
    return textAlign || fontStyle || fontWeight || textDecoration || textIndent || marginTop || marginBottom ||
           marginLeft || marginRight || paddingTop || paddingBottom || paddingLeft || paddingRight || imageHeight ||
           imageWidth || display || verticalAlign || listStyleNone || pageBreakBefore || pageBreakAfter || lineHeight ||
           fontSizeMultiplier || cssFloat || smallCaps || textTransform || fontFamily || letterSpacing || whiteSpace ||
           colorTransparent || opacityZero || visibilityHidden || minHeight || maxHeight || maxWidth ||
           borderTopWidth || borderBottomWidth || borderTopStyle || borderBottomStyle || backgroundGray ||
           breakInsideAvoid || cssClear || hyphens || ligatures || hyphenateLimitBefore || hyphenateLimitAfter ||
           hyphenateLimitLines;
  }

  void clearAll() {
    minHeight = maxHeight = maxWidth = borderTopWidth = borderBottomWidth = borderTopStyle = borderBottomStyle =
        backgroundGray = breakInsideAvoid = cssClear = hyphens = ligatures = hyphenateLimitBefore =
            hyphenateLimitAfter = hyphenateLimitLines = 0;
    textAlign = fontStyle = fontWeight = textDecoration = textIndent = 0;
    marginTop = marginBottom = marginLeft = marginRight = 0;
    paddingTop = paddingBottom = paddingLeft = paddingRight = 0;
    imageHeight = imageWidth = display = verticalAlign = 0;
    listStyleNone = pageBreakBefore = pageBreakAfter = lineHeight = fontSizeMultiplier = cssFloat = 0;
    whiteSpace = fontFamily = letterSpacing = smallCaps = textTransform = colorTransparent = opacityZero =
        visibilityHidden = 0;
  }
};

// Represents a collection of CSS style properties
// Only stores properties relevant to e-ink text rendering
// Length values are stored as CssLength (value + unit) for deferred resolution
struct CssStyle {
  CssTextAlign textAlign = CssTextAlign::Left;
  CssFontStyle fontStyle = CssFontStyle::Normal;
  CssFontWeight fontWeight = CssFontWeight::Normal;
  CssTextDecoration textDecoration = CssTextDecoration::None;

  CssLength textIndent;     // First-line indent (deferred resolution)
  CssLength marginTop;      // Vertical spacing before block
  CssLength marginBottom;   // Vertical spacing after block
  CssLength marginLeft;     // Horizontal spacing left of block
  CssLength marginRight;    // Horizontal spacing right of block
  CssLength paddingTop;     // Padding before
  CssLength paddingBottom;  // Padding after
  CssLength paddingLeft;    // Padding left
  CssLength paddingRight;   // Padding right
  CssLength imageHeight;    // Height for img (e.g. 2em) – width derived from aspect ratio when only height set
  CssLength imageWidth;     // Width for img when both or only width set
  CssDisplay display = CssDisplay::Block;                       // display property (Block, Inline or None)
  CssVerticalAlign verticalAlign = CssVerticalAlign::Baseline;  // vertical-align for inline elements

  bool listStyleNone = false;          // true when list-style-type: none / list-style: none
  bool pageBreakBefore = false;        // true when page-break-before: always (or break-before: page)
  bool pageBreakAfter = false;         // true when page-break-after: always (or break-after: page)
  float lineHeightMultiplier = 1.0f;   // normalised line-height multiplier (relative to default y_advance)
  float fontSizeMultiplier = 1.0f;     // font-size multiplier relative to body em size
  CssFloat cssFloat = CssFloat::None;  // float: left/right — signals inline image context
  uint8_t smallCaps = 0;               // 0 normal, 1 small-caps, 2 all-small-caps, 3 inherit/unset
  CssTextTransform textTransform = CssTextTransform::None;
  CssWhiteSpace whiteSpace = CssWhiteSpace::Normal;
  uint8_t fontFamily = wordTypography::Reader;
  CssLength letterSpacing;  // Auto is the explicit inherit/unset marker. Percent is invalid for this property.
  // Text made invisible while still in the flow. A PDF-to-EPUB conversion emits every OCR word
  // as its own absolutely positioned div with `color: transparent` under a full-page scan; laid
  // out as ordinary text that became eleven pages of single words per scan (Deckhand).
  // colorTransparent hides TEXT only (images are not coloured); opacityZero and visibilityHidden
  // hide the whole element, images included. `-webkit-text-fill-color` and alpha-zero colours
  // (rgba/hsla/#rrggbb00) set colorTransparent too.
  // Invariant: a DEFINED invisibility property is always true. The visible value (color: red,
  // opacity: 0.5, visibility: visible) undefines it rather than defining it false, so a rule
  // that sets only visible values still has no supported declaration and stays out of the rule
  // cache. The value bools exist for uniformity with every other property; see CssParser.
  bool colorTransparent = false;
  bool opacityZero = false;
  bool visibilityHidden = false;  // visibility: hidden | collapse

  // Luminance 0..255; 256 transparent; 257 explicit inherit. Limits use 255 for inherit.
  CssLength minHeight = CssLength{};
  CssLength maxHeight = CssLength{0, CssUnit::Auto};
  CssLength maxWidth = CssLength{0, CssUnit::Auto};
  CssLength borderTopWidth = CssLength{3};
  CssLength borderBottomWidth = CssLength{3};
  CssBorderStyle borderTopStyle = CssBorderStyle::None;
  CssBorderStyle borderBottomStyle = CssBorderStyle::None;
  uint16_t backgroundGray = 256;
  bool breakInsideAvoid = false;
  CssClear cssClear = CssClear::None;
  CssHyphens hyphens = CssHyphens::Manual;
  CssLigatures ligatures = CssLigatures::Normal;
  uint8_t hyphenateLimitBefore = 2;
  uint8_t hyphenateLimitAfter = 2;
  uint8_t hyphenateLimitLines = 0;
  bool breakInsideInherit = false;

  CssPropertyFlags defined;  // Tracks which properties were explicitly set

  // Apply properties from another style, only overwriting if the other style
  // has that property explicitly defined
  void applyOver(const CssStyle& base) {
    if (base.defined.minHeight) {
      minHeight = base.minHeight;
      defined.minHeight = 1;
    }
    if (base.defined.maxHeight) {
      maxHeight = base.maxHeight;
      defined.maxHeight = 1;
    }
    if (base.defined.maxWidth) {
      maxWidth = base.maxWidth;
      defined.maxWidth = 1;
    }
    if (base.defined.borderTopWidth) {
      borderTopWidth = base.borderTopWidth;
      defined.borderTopWidth = 1;
    }
    if (base.defined.borderBottomWidth) {
      borderBottomWidth = base.borderBottomWidth;
      defined.borderBottomWidth = 1;
    }
    if (base.defined.borderTopStyle) {
      borderTopStyle = base.borderTopStyle;
      defined.borderTopStyle = 1;
    }
    if (base.defined.borderBottomStyle) {
      borderBottomStyle = base.borderBottomStyle;
      defined.borderBottomStyle = 1;
    }
    if (base.defined.backgroundGray) {
      backgroundGray = base.backgroundGray;
      defined.backgroundGray = 1;
    }
    if (base.defined.breakInsideAvoid) {
      breakInsideAvoid = base.breakInsideAvoid;
      defined.breakInsideAvoid = 1;
    }
    if (base.defined.cssClear) {
      cssClear = base.cssClear;
      defined.cssClear = 1;
    }
    if (base.defined.hyphens) {
      hyphens = base.hyphens;
      defined.hyphens = 1;
    }
    if (base.defined.ligatures) {
      ligatures = base.ligatures;
      defined.ligatures = 1;
    }
    if (base.defined.hyphenateLimitBefore) {
      hyphenateLimitBefore = base.hyphenateLimitBefore;
      defined.hyphenateLimitBefore = 1;
    }
    if (base.defined.hyphenateLimitAfter) {
      hyphenateLimitAfter = base.hyphenateLimitAfter;
      defined.hyphenateLimitAfter = 1;
    }
    if (base.defined.hyphenateLimitLines) {
      hyphenateLimitLines = base.hyphenateLimitLines;
      defined.hyphenateLimitLines = 1;
    }
    if (base.defined.breakInsideAvoid) breakInsideInherit = base.breakInsideInherit;

    if (base.hasTextAlign()) {
      textAlign = base.textAlign;
      defined.textAlign = 1;
    }
    if (base.hasFontStyle()) {
      fontStyle = base.fontStyle;
      defined.fontStyle = 1;
    }
    if (base.hasFontWeight()) {
      fontWeight = base.fontWeight;
      defined.fontWeight = 1;
    }
    if (base.hasTextDecoration()) {
      textDecoration = base.textDecoration;
      defined.textDecoration = 1;
    }
    if (base.hasTextIndent()) {
      textIndent = base.textIndent;
      defined.textIndent = 1;
    }
    if (base.hasMarginTop()) {
      marginTop = base.marginTop;
      defined.marginTop = 1;
    }
    if (base.hasMarginBottom()) {
      marginBottom = base.marginBottom;
      defined.marginBottom = 1;
    }
    if (base.hasMarginLeft()) {
      marginLeft = base.marginLeft;
      defined.marginLeft = 1;
    }
    if (base.hasMarginRight()) {
      marginRight = base.marginRight;
      defined.marginRight = 1;
    }
    if (base.hasPaddingTop()) {
      paddingTop = base.paddingTop;
      defined.paddingTop = 1;
    }
    if (base.hasPaddingBottom()) {
      paddingBottom = base.paddingBottom;
      defined.paddingBottom = 1;
    }
    if (base.hasPaddingLeft()) {
      paddingLeft = base.paddingLeft;
      defined.paddingLeft = 1;
    }
    if (base.hasPaddingRight()) {
      paddingRight = base.paddingRight;
      defined.paddingRight = 1;
    }
    // Keyed on the defined bit, not has*(): an explicit `auto` has to travel up the cascade so it
    // can clear a length a lower-priority rule set. has*() reports it as unset, which is what the
    // layout wants but the wrong test here.
    if (base.defined.imageHeight) {
      imageHeight = base.imageHeight;
      defined.imageHeight = 1;
    }
    if (base.defined.imageWidth) {
      imageWidth = base.imageWidth;
      defined.imageWidth = 1;
    }
    if (base.hasDisplay()) {
      display = base.display;
      defined.display = 1;
    }
    if (base.hasVerticalAlign()) {
      verticalAlign = base.verticalAlign;
      defined.verticalAlign = 1;
    }
    if (base.hasListStyleNone()) {
      listStyleNone = base.listStyleNone;
      defined.listStyleNone = 1;
    }
    if (base.hasPageBreakBefore()) {
      pageBreakBefore = base.pageBreakBefore;
      defined.pageBreakBefore = 1;
    }
    if (base.hasPageBreakAfter()) {
      pageBreakAfter = base.pageBreakAfter;
      defined.pageBreakAfter = 1;
    }
    if (base.hasLineHeight()) {
      lineHeightMultiplier = base.lineHeightMultiplier;
      defined.lineHeight = 1;
    }
    if (base.hasFontSizeMultiplier()) {
      fontSizeMultiplier = base.fontSizeMultiplier;
      defined.fontSizeMultiplier = 1;
    }
    if (base.hasCssFloat()) {
      cssFloat = base.cssFloat;
      defined.cssFloat = 1;
    }
    if (base.hasFontFamily()) {
      fontFamily = base.fontFamily;
      defined.fontFamily = 1;
    }
    if (base.hasLetterSpacing()) {
      letterSpacing = base.letterSpacing;
      defined.letterSpacing = 1;
    }
    if (base.hasTextTransform()) {
      textTransform = base.textTransform;
      defined.textTransform = 1;
    }
    if (base.hasWhiteSpace()) {
      whiteSpace = base.whiteSpace;
      defined.whiteSpace = 1;
    }
    if (base.hasSmallCaps()) {
      smallCaps = base.smallCaps;
      defined.smallCaps = 1;
    }
    if (base.hasColorTransparent()) {
      colorTransparent = base.colorTransparent;
      defined.colorTransparent = 1;
    }
    if (base.hasOpacityZero()) {
      opacityZero = base.opacityZero;
      defined.opacityZero = 1;
    }
    if (base.hasVisibilityHidden()) {
      visibilityHidden = base.visibilityHidden;
      defined.visibilityHidden = 1;
    }
  }

  [[nodiscard]] bool hasMinHeight() const { return defined.minHeight; }
  [[nodiscard]] bool hasMaxHeight() const { return defined.maxHeight; }
  [[nodiscard]] bool hasMaxWidth() const { return defined.maxWidth; }
  [[nodiscard]] bool hasBorderTopWidth() const { return defined.borderTopWidth; }
  [[nodiscard]] bool hasBorderBottomWidth() const { return defined.borderBottomWidth; }
  [[nodiscard]] bool hasBorderTopStyle() const { return defined.borderTopStyle; }
  [[nodiscard]] bool hasBorderBottomStyle() const { return defined.borderBottomStyle; }
  [[nodiscard]] bool hasBackgroundGray() const { return defined.backgroundGray; }
  [[nodiscard]] bool hasBreakInsideAvoid() const { return defined.breakInsideAvoid; }
  [[nodiscard]] bool hasCssClear() const { return defined.cssClear; }
  [[nodiscard]] bool hasHyphens() const { return defined.hyphens; }
  [[nodiscard]] bool hasLigatures() const { return defined.ligatures; }
  [[nodiscard]] bool hasHyphenateLimitBefore() const { return defined.hyphenateLimitBefore; }
  [[nodiscard]] bool hasHyphenateLimitAfter() const { return defined.hyphenateLimitAfter; }
  [[nodiscard]] bool hasHyphenateLimitLines() const { return defined.hyphenateLimitLines; }

  [[nodiscard]] bool hasTextAlign() const { return defined.textAlign; }
  [[nodiscard]] bool hasFontStyle() const { return defined.fontStyle; }
  [[nodiscard]] bool hasFontWeight() const { return defined.fontWeight; }
  [[nodiscard]] bool hasTextDecoration() const { return defined.textDecoration; }
  [[nodiscard]] bool hasTextIndent() const { return defined.textIndent; }
  [[nodiscard]] bool hasMarginTop() const { return defined.marginTop; }
  [[nodiscard]] bool hasMarginBottom() const { return defined.marginBottom; }
  [[nodiscard]] bool hasMarginLeft() const { return defined.marginLeft; }
  [[nodiscard]] bool hasMarginRight() const { return defined.marginRight; }
  [[nodiscard]] bool hasPaddingTop() const { return defined.paddingTop; }
  [[nodiscard]] bool hasPaddingBottom() const { return defined.paddingBottom; }
  [[nodiscard]] bool hasPaddingLeft() const { return defined.paddingLeft; }
  [[nodiscard]] bool hasPaddingRight() const { return defined.paddingRight; }
  // `auto` is recorded as a defined property (so it overrides a length from a lower-priority
  // rule via applyOver) but reports as "no explicit size", which is what it means: derive the
  // axis from the image's aspect ratio.
  [[nodiscard]] bool hasImageHeight() const { return defined.imageHeight && imageHeight.unit != CssUnit::Auto; }
  [[nodiscard]] bool hasImageWidth() const { return defined.imageWidth && imageWidth.unit != CssUnit::Auto; }
  [[nodiscard]] bool hasDisplay() const { return defined.display; }
  [[nodiscard]] bool hasVerticalAlign() const { return defined.verticalAlign; }
  [[nodiscard]] bool hasListStyleNone() const { return defined.listStyleNone; }
  [[nodiscard]] bool hasCssFloat() const { return defined.cssFloat; }
  [[nodiscard]] bool hasWhiteSpace() const { return defined.whiteSpace; }
  [[nodiscard]] bool hasSmallCaps() const { return defined.smallCaps; }
  [[nodiscard]] bool hasFontFamily() const { return defined.fontFamily; }
  [[nodiscard]] bool hasLetterSpacing() const { return defined.letterSpacing; }
  [[nodiscard]] bool hasTextTransform() const { return defined.textTransform; }
  [[nodiscard]] bool hasColorTransparent() const { return defined.colorTransparent; }
  [[nodiscard]] bool hasOpacityZero() const { return defined.opacityZero; }
  [[nodiscard]] bool hasVisibilityHidden() const { return defined.visibilityHidden; }
  // The element and everything in it is invisible (images too): opacity 0, or visibility
  // hidden/collapse. Text-only invisibility (colorTransparent) is a separate question.
  [[nodiscard]] bool isElementHidden() const {
    return (defined.opacityZero && opacityZero) || (defined.visibilityHidden && visibilityHidden);
  }
  [[nodiscard]] bool isTextTransparent() const { return defined.colorTransparent && colorTransparent; }
  // Text declared too small to read: the `font-size: 1px` span a watermarking service hides in the
  // prose. Like colorTransparent a TEXT-only question (images are not sized by it), asked of the
  // element's own declared multiplier, before any baseline normalisation.
  // The line is a tenth of the body: 1.6 px of the CSS 16 px medium, 1.2 pt of 12 pt. Watermarks
  // sit at 1 px (0.0625) or 1 pt (0.083). Nothing meant to be read comes near it: the smallest
  // keyword (xx-small) is 0.6, the sup/sub default 0.5, and the renderer's own floor
  // (ParsedText::MIN_WORD_SIZE_PCT, 30 %) is three times above it. A tenth itself is left to that
  // floor, as test_font_sizes.epub pins.
  static constexpr float kInvisibleFontSizeMultiplier = 0.1f;
  [[nodiscard]] bool isFontSizeInvisible() const {
    return defined.fontSizeMultiplier && fontSizeMultiplier < kInvisibleFontSizeMultiplier;
  }
  [[nodiscard]] bool hasPageBreakBefore() const { return defined.pageBreakBefore; }
  [[nodiscard]] bool hasPageBreakAfter() const { return defined.pageBreakAfter; }
  [[nodiscard]] bool hasLineHeight() const { return defined.lineHeight; }
  [[nodiscard]] bool hasFontSizeMultiplier() const { return defined.fontSizeMultiplier; }

  void reset() {
    minHeight = CssLength{};
    maxHeight = CssLength{0, CssUnit::Auto};
    maxWidth = CssLength{0, CssUnit::Auto};
    borderTopWidth = CssLength{3};
    borderBottomWidth = CssLength{3};
    borderTopStyle = CssBorderStyle::None;
    borderBottomStyle = CssBorderStyle::None;
    backgroundGray = 256;
    breakInsideAvoid = false;
    cssClear = CssClear::None;
    hyphens = CssHyphens::Manual;
    ligatures = CssLigatures::Normal;
    hyphenateLimitBefore = 2;
    hyphenateLimitAfter = 2;
    hyphenateLimitLines = 0;
    breakInsideInherit = false;

    textAlign = CssTextAlign::Left;
    fontStyle = CssFontStyle::Normal;
    fontWeight = CssFontWeight::Normal;
    textDecoration = CssTextDecoration::None;
    textIndent = CssLength{};
    marginTop = marginBottom = marginLeft = marginRight = CssLength{};
    paddingTop = paddingBottom = paddingLeft = paddingRight = CssLength{};
    imageHeight = imageWidth = CssLength{};
    display = CssDisplay::Block;
    verticalAlign = CssVerticalAlign::Baseline;
    listStyleNone = false;
    pageBreakBefore = false;
    pageBreakAfter = false;
    lineHeightMultiplier = 1.0f;
    fontSizeMultiplier = 1.0f;
    cssFloat = CssFloat::None;
    smallCaps = 0;
    textTransform = CssTextTransform::None;
    whiteSpace = CssWhiteSpace::Normal;
    fontFamily = wordTypography::Reader;
    letterSpacing = CssLength();
    colorTransparent = false;
    opacityZero = false;
    visibilityHidden = false;
    defined.clearAll();
  }
};
