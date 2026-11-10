#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Matches order of PARAGRAPH_ALIGNMENT in CrossPointSettings
enum class CssTextAlign : uint8_t { Justify = 0, Left = 1, Center = 2, Right = 3, None = 4 };
enum class CssUnit : uint8_t { Pixels = 0, Em = 1, Rem = 2, Points = 3, Percent = 4, Inherit = 5, Auto = 6 };
enum class CssTextTransform : uint8_t { None = 0, Uppercase = 1, Lowercase = 2, Inherit = 3 };
enum class CssTextDirection : uint8_t { Ltr = 0, Rtl = 1 };

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
    return unit != CssUnit::Inherit && unit != CssUnit::Auto && (unit != CssUnit::Percent || containerWidth > 0);
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
    return std::isfinite(pixels) ? static_cast<int16_t>(std::clamp(pixels, -32768.0f, 32767.0f)) : 0;
  }
};

// Font style options matching CSS font-style property
enum class CssFontStyle : uint8_t { Normal = 0, Italic = 1 };

// Font weight options - CSS supports 100-900, we simplify to normal/bold
enum class CssFontWeight : uint8_t { Normal = 0, Bold = 1 };

// Font variant caps options matching the small subset CrossInk renders.
enum class CssFontVariantCaps : uint8_t { Normal = 0, SmallCaps = 1, AllSmallCaps = 2, Inherit = 3 };

// Text decoration options. Values are bit flags so CSS can combine multiple line decorations.
enum class CssTextDecoration : uint8_t { None = 0, Underline = 1, LineThrough = 2 };

constexpr CssTextDecoration operator|(const CssTextDecoration a, const CssTextDecoration b) {
  return static_cast<CssTextDecoration>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

constexpr CssTextDecoration operator&(const CssTextDecoration a, const CssTextDecoration b) {
  return static_cast<CssTextDecoration>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

constexpr uint8_t CSS_TEXT_DECORATION_MASK =
    static_cast<uint8_t>(CssTextDecoration::Underline) | static_cast<uint8_t>(CssTextDecoration::LineThrough);

// Keep inline distinct so replaced elements can participate in text layout.
enum class CssDisplay : uint8_t { Block = 0, None = 1, Inline = 2, InlineBlock = 3 };

// Vertical alignment options for inline elements (e.g. superscript/subscript)
enum class CssVerticalAlign : uint8_t { Baseline = 0, Super = 1, Sub = 2 };

// List markers supported by the EPUB renderer.
enum class CssListStyleType : uint8_t { Disc = 0, None = 1 };

enum class CssBorderStyle : uint8_t { None, Solid, Double, Dotted, Dashed };
struct CssBorderSide {
  uint8_t width = 3;
  CssBorderStyle style = CssBorderStyle::None;
  bool visible() const { return width && style != CssBorderStyle::None; }
};

enum class CssFloat : uint8_t { None = 0, Left = 1, Right = 2 };
enum class CssClear : uint8_t { None = 0, Left = 1, Right = 2, Both = 3, Inherit = 4 };
enum class CssHyphens : uint8_t { Inherit = 0, None = 1, Manual = 2, Auto = 3 };
enum class CssLigatures : uint8_t { Inherit = 0, Normal = 1, None = 2 };
enum class CssWhiteSpace : uint8_t { Normal = 0, Pre = 1, PreWrap = 2, PreLine = 3, NoWrap = 4, Inherit = 5 };
enum class CssBackgroundRepeat : uint8_t { Repeat = 0, NoRepeat = 1, RepeatX = 2, RepeatY = 3, Inherit = 4 };

// Bitmask for tracking which properties have been explicitly set
struct CssPropertyFlags {
  uint64_t textAlign : 1;
  uint64_t fontStyle : 1;
  uint64_t fontWeight : 1;
  uint64_t textDecoration : 1;
  uint64_t textIndent : 1;
  uint64_t marginTop : 1;
  uint64_t marginBottom : 1;
  uint64_t marginLeft : 1;
  uint64_t marginRight : 1;
  uint64_t paddingTop : 1;
  uint64_t paddingBottom : 1;
  uint64_t paddingLeft : 1;
  uint64_t paddingRight : 1;
  uint64_t imageHeight : 1;
  uint64_t imageWidth : 1;
  uint64_t display : 1;
  uint64_t backgroundBlack : 1;
  uint64_t verticalAlign : 1;
  uint64_t direction : 1;
  uint64_t pageBreakBefore : 1;
  uint64_t pageBreakAfter : 1;
  uint64_t fontVariantCaps : 1;
  uint64_t listStyleType : 1;
  uint64_t fontSize : 1;
  uint64_t border : 1;
  uint64_t whiteSpace : 1;
  uint64_t shaded : 1;
  uint64_t floatLeft : 1;
  uint64_t initialLetter : 1;
  uint64_t fontFamily : 1;
  uint64_t textTransform : 1;
  uint64_t letterSpacing : 1;

  uint64_t lineHeight : 1;
  uint64_t colorTransparent : 1;
  uint64_t opacityZero : 1;
  uint64_t visibilityHidden : 1;
  uint64_t minHeight : 1;
  uint64_t maxHeight : 1;
  uint64_t maxWidth : 1;
  uint64_t backgroundGray : 1;
  uint64_t breakInsideAvoid : 1;
  uint64_t cssClear : 1;
  uint64_t hyphens : 1;
  uint64_t ligatures : 1;
  uint64_t hyphenateLimitBefore : 1;
  uint64_t hyphenateLimitAfter : 1;
  uint64_t hyphenateLimitLines : 1;
  uint64_t cssFloat : 1;
  uint64_t backgroundImage : 1;
  uint64_t pseudoContent : 1;
  uint64_t backgroundRepeat : 1;

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
        backgroundBlack(0),
        verticalAlign(0),
        direction(0),
        pageBreakBefore(0),
        pageBreakAfter(0),
        fontVariantCaps(0),
        listStyleType(0),
        fontSize(0),
        border(0),
        whiteSpace(0),
        shaded(0),
        floatLeft(0),
        initialLetter(0),
        fontFamily(0),
        textTransform(0),
        letterSpacing(0),
        lineHeight(0),
        colorTransparent(0),
        opacityZero(0),
        visibilityHidden(0),
        minHeight(0),
        maxHeight(0),
        maxWidth(0),
        backgroundGray(0),
        breakInsideAvoid(0),
        cssClear(0),
        hyphens(0),
        ligatures(0),
        hyphenateLimitBefore(0),
        hyphenateLimitAfter(0),
        hyphenateLimitLines(0),
        cssFloat(0),
        backgroundImage(0),
        pseudoContent(0),
        backgroundRepeat(0) {}

  [[nodiscard]] bool anySet() const {
    return lineHeight || colorTransparent || opacityZero || visibilityHidden || minHeight || maxHeight || maxWidth ||
           backgroundGray || breakInsideAvoid || cssClear || hyphens || ligatures || hyphenateLimitBefore ||
           hyphenateLimitAfter || hyphenateLimitLines || cssFloat || backgroundImage || pseudoContent ||
           backgroundRepeat || textTransform || letterSpacing || fontFamily || shaded || floatLeft || initialLetter ||
           whiteSpace || border || fontSize || textAlign || fontStyle || fontWeight || textDecoration || textIndent ||
           marginTop || marginBottom || marginLeft || marginRight || paddingTop || paddingBottom || paddingLeft ||
           paddingRight || imageHeight || imageWidth || display || backgroundBlack || verticalAlign || direction ||
           pageBreakBefore || pageBreakAfter || fontVariantCaps || listStyleType;
  }

  void clearAll() {
    lineHeight = colorTransparent = opacityZero = visibilityHidden = minHeight = maxHeight = maxWidth = backgroundGray =
        breakInsideAvoid = cssClear = hyphens = ligatures = hyphenateLimitBefore = hyphenateLimitAfter =
            hyphenateLimitLines = cssFloat = backgroundImage = pseudoContent = backgroundRepeat = 0;
    textAlign = fontStyle = fontWeight = textDecoration = textIndent = 0;
    marginTop = marginBottom = marginLeft = marginRight = 0;
    paddingTop = paddingBottom = paddingLeft = paddingRight = 0;
    imageHeight = imageWidth = display = backgroundBlack = verticalAlign = direction = 0;
    pageBreakBefore = pageBreakAfter = fontVariantCaps = listStyleType = fontSize = border = whiteSpace = shaded =
        floatLeft = initialLetter = fontFamily = textTransform = letterSpacing = 0;
  }
};

static_assert(sizeof(CssPropertyFlags) <= sizeof(uint64_t),
              "CssPropertyFlags exceeds 64 bits; update cache read/write in CssParser.cpp");

// Represents a collection of CSS style properties
// Only stores properties relevant to e-ink text rendering
// Length values are stored as CssLength (value + unit) for deferred resolution
struct CssStyle {
  float lineHeightMultiplier = 1.0f;  // 0 means explicit inheritance; otherwise bounded [0.7, 2].
  CssWhiteSpace whiteSpace = CssWhiteSpace::Normal;
  CssFloat cssFloat = CssFloat::None;
  bool colorTransparent = false, opacityZero = false, visibilityHidden = false;
  CssLength minHeight;
  CssLength maxHeight{0, CssUnit::Auto}, maxWidth{0, CssUnit::Auto};
  uint16_t backgroundGray = 256;  // 0..255 luminance, 256 transparent, 257 inherit.
  bool breakInsideAvoid = false, breakInsideInherit = false;
  CssClear cssClear = CssClear::None;
  CssHyphens hyphens = CssHyphens::Manual;
  CssLigatures ligatures = CssLigatures::Normal;
  uint8_t hyphenateLimitBefore = 2, hyphenateLimitAfter = 2, hyphenateLimitLines = 0;  // 255 inherit.
  uint32_t backgroundImage = 0, pseudoContent = 0;  // Bounded catalog tokens; 0 explicit none.
  CssBackgroundRepeat backgroundRepeat = CssBackgroundRepeat::Repeat;
  uint32_t fontFamily = 0;
  CssTextAlign textAlign = CssTextAlign::Left;
  CssFontStyle fontStyle = CssFontStyle::Normal;
  CssFontWeight fontWeight = CssFontWeight::Normal;
  CssTextDecoration textDecoration = CssTextDecoration::None;
  CssTextDirection direction = CssTextDirection::Ltr;
  CssFontVariantCaps fontVariantCaps = CssFontVariantCaps::Normal;
  CssTextTransform textTransform = CssTextTransform::None;
  CssLength letterSpacing;

  CssLength fontSize;       // Resolved against parent/root font, not the box width
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
  CssDisplay display = CssDisplay::Block;
  CssBorderSide borders[4];  // top, right, bottom, left; shares the existing defined edge masks
  bool shaded = false;
  bool floatLeft = false;
  uint8_t initialLetter = 0;
  bool hasVisibleBorder() const {
    for (const auto& side : borders)
      if (side.visible()) return true;
    return false;
  }
  bool preserveWhitespace = false;
  bool backgroundBlack = false;                                 // Simple black inline/block background support
  CssVerticalAlign verticalAlign = CssVerticalAlign::Baseline;  // vertical-align (super/sub positioning)
  bool pageBreakBefore = false;
  bool pageBreakAfter = false;
  CssListStyleType listStyleType = CssListStyleType::Disc;

  // Four edge bits (top/right/bottom/left). Track style and width separately so
  // later declarations can restore one without losing suppression by the other.
  uint8_t borderStyleSuppressed = 0;
  uint8_t borderWidthSuppressed = 0;
  uint8_t borderStyleDefined = 0;
  uint8_t borderWidthDefined = 0;

  [[nodiscard]] bool suppressesHorizontalRule() const {
    return (borderStyleSuppressed | borderWidthSuppressed) == 0x0F;
  }

  CssPropertyFlags defined;  // Tracks which properties were explicitly set

  // Apply properties from another style, only overwriting if the other style
  // has that property explicitly defined
  void applyOver(const CssStyle& base) {
    if (base.defined.lineHeight) {
      lineHeightMultiplier = base.lineHeightMultiplier;
      defined.lineHeight = 1;
    }
    if (base.defined.colorTransparent) {
      colorTransparent = base.colorTransparent;
      defined.colorTransparent = 1;
    }
    if (base.defined.opacityZero) {
      opacityZero = base.opacityZero;
      defined.opacityZero = 1;
    }
    if (base.defined.visibilityHidden) {
      visibilityHidden = base.visibilityHidden;
      defined.visibilityHidden = 1;
    }
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
    if (base.defined.backgroundGray) {
      backgroundGray = base.backgroundGray;
      defined.backgroundGray = 1;
    }
    if (base.defined.breakInsideAvoid) {
      breakInsideAvoid = base.breakInsideAvoid;
      defined.breakInsideAvoid = 1;
      breakInsideInherit = base.breakInsideInherit;
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
    if (base.defined.cssFloat) {
      cssFloat = base.cssFloat;
      defined.cssFloat = 1;
      floatLeft = base.cssFloat == CssFloat::Left;
    }
    if (base.defined.backgroundImage) {
      backgroundImage = base.backgroundImage;
      defined.backgroundImage = 1;
    }
    if (base.defined.pseudoContent) {
      pseudoContent = base.pseudoContent;
      defined.pseudoContent = 1;
    }
    if (base.defined.backgroundRepeat) {
      backgroundRepeat = base.backgroundRepeat;
      defined.backgroundRepeat = 1;
    }

    if (base.defined.textTransform) {
      textTransform = base.textTransform;
      defined.textTransform = 1;
    }
    if (base.defined.letterSpacing) {
      letterSpacing = base.letterSpacing;
      defined.letterSpacing = 1;
    }
    if (base.defined.fontFamily) {
      fontFamily = base.fontFamily;
      defined.fontFamily = 1;
    }
    if (base.defined.shaded) {
      shaded = base.shaded;
      defined.shaded = 1;
    }
    if (base.defined.floatLeft) {
      floatLeft = base.floatLeft;
      defined.floatLeft = 1;
    }
    if (base.defined.initialLetter) {
      initialLetter = base.initialLetter;
      defined.initialLetter = 1;
    }
    if (base.hasWhiteSpace()) {
      preserveWhitespace = base.preserveWhitespace;
      whiteSpace = base.whiteSpace;
      defined.whiteSpace = 1;
    }
    if (base.defined.border) {
      for (size_t edge = 0; edge < 4; ++edge) {
        if (base.borderStyleDefined & (1u << edge)) borders[edge].style = base.borders[edge].style;
        if (base.borderWidthDefined & (1u << edge)) borders[edge].width = base.borders[edge].width;
      }
      borderStyleSuppressed = (borderStyleSuppressed & ~base.borderStyleDefined) | base.borderStyleSuppressed;
      borderWidthSuppressed = (borderWidthSuppressed & ~base.borderWidthDefined) | base.borderWidthSuppressed;
      borderStyleDefined |= base.borderStyleDefined;
      borderWidthDefined |= base.borderWidthDefined;
      defined.border = 1;
    }
    if (base.hasFontSize()) {
      fontSize = base.fontSize;
      defined.fontSize = 1;
    }
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
    if (base.hasImageHeight()) {
      imageHeight = base.imageHeight;
      defined.imageHeight = 1;
    }
    if (base.hasImageWidth()) {
      imageWidth = base.imageWidth;
      defined.imageWidth = 1;
    }
    if (base.hasDisplay()) {
      display = base.display;
      defined.display = 1;
    }
    if (base.hasBackgroundBlack()) {
      backgroundBlack = base.backgroundBlack;
      defined.backgroundBlack = 1;
    }
    if (base.hasDirection()) {
      direction = base.direction;
      defined.direction = 1;
    }
    if (base.hasVerticalAlign()) {
      verticalAlign = base.verticalAlign;
      defined.verticalAlign = 1;
    }
    if (base.hasPageBreakBefore()) {
      pageBreakBefore = base.pageBreakBefore;
      defined.pageBreakBefore = 1;
    }
    if (base.hasPageBreakAfter()) {
      pageBreakAfter = base.pageBreakAfter;
      defined.pageBreakAfter = 1;
    }
    if (base.hasFontVariantCaps()) {
      fontVariantCaps = base.fontVariantCaps;
      defined.fontVariantCaps = 1;
    }
    if (base.hasListStyleType()) {
      listStyleType = base.listStyleType;
      defined.listStyleType = 1;
    }
  }

  [[nodiscard]] bool hasWhiteSpace() const { return defined.whiteSpace; }
  [[nodiscard]] bool hasTextTransform() const { return defined.textTransform; }
  [[nodiscard]] bool hasLetterSpacing() const { return defined.letterSpacing; }
  [[nodiscard]] bool hasFontSize() const { return defined.fontSize; }
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
  [[nodiscard]] bool hasImageHeight() const { return defined.imageHeight; }
  [[nodiscard]] bool hasImageWidth() const { return defined.imageWidth; }
  [[nodiscard]] bool hasDisplay() const { return defined.display; }
  [[nodiscard]] bool hasBackgroundBlack() const { return defined.backgroundBlack; }
  [[nodiscard]] bool hasVerticalAlign() const { return defined.verticalAlign; }
  [[nodiscard]] bool hasDirection() const { return defined.direction; }
  [[nodiscard]] bool hasPageBreakBefore() const { return defined.pageBreakBefore; }
  [[nodiscard]] bool hasPageBreakAfter() const { return defined.pageBreakAfter; }
  [[nodiscard]] bool hasFontVariantCaps() const { return defined.fontVariantCaps; }
  [[nodiscard]] bool hasListStyleType() const { return defined.listStyleType; }

  [[nodiscard]] bool hasLineHeight() const { return defined.lineHeight; }
  [[nodiscard]] bool hasColorTransparent() const { return defined.colorTransparent; }
  [[nodiscard]] bool hasOpacityZero() const { return defined.opacityZero; }
  [[nodiscard]] bool hasVisibilityHidden() const { return defined.visibilityHidden; }
  [[nodiscard]] bool hasMinHeight() const { return defined.minHeight; }
  [[nodiscard]] bool hasMaxHeight() const { return defined.maxHeight; }
  [[nodiscard]] bool hasMaxWidth() const { return defined.maxWidth; }
  [[nodiscard]] bool hasBackgroundGray() const { return defined.backgroundGray; }
  [[nodiscard]] bool hasBreakInsideAvoid() const { return defined.breakInsideAvoid; }
  [[nodiscard]] bool hasCssClear() const { return defined.cssClear; }
  [[nodiscard]] bool hasHyphens() const { return defined.hyphens; }
  [[nodiscard]] bool hasLigatures() const { return defined.ligatures; }
  [[nodiscard]] bool hasHyphenateLimitBefore() const { return defined.hyphenateLimitBefore; }
  [[nodiscard]] bool hasHyphenateLimitAfter() const { return defined.hyphenateLimitAfter; }
  [[nodiscard]] bool hasHyphenateLimitLines() const { return defined.hyphenateLimitLines; }
  [[nodiscard]] bool hasCssFloat() const { return defined.cssFloat; }
  [[nodiscard]] bool hasBackgroundImage() const { return defined.backgroundImage; }
  [[nodiscard]] bool hasPseudoContent() const { return defined.pseudoContent; }
  [[nodiscard]] bool hasBackgroundRepeat() const { return defined.backgroundRepeat; }
  [[nodiscard]] bool isElementHidden() const {
    return (defined.opacityZero && opacityZero) || (defined.visibilityHidden && visibilityHidden);
  }
  [[nodiscard]] bool isTextTransparent() const { return defined.colorTransparent && colorTransparent; }
  static constexpr float kInvisibleFontSizeMultiplier = 0.1f;
  [[nodiscard]] bool isFontSizeInvisible(float parentCssPixels = 16.0f) const {
    if (!defined.fontSize) return false;
    const float pixels = fontSize.toPixels(parentCssPixels, parentCssPixels);
    return std::isfinite(pixels) && pixels >= 0 && pixels < 16.0f * kInvisibleFontSizeMultiplier;
  }

  void reset() { *this = CssStyle{}; }
};
