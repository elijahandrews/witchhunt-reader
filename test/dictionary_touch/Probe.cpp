// Host I/O for production methods inserted by run.py. No device or private text.
#include <ReaderWordHold.h>
#include <TouchTransform.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#define CP_TOUCH_UI 1
unsigned long nowMs = 2000;
unsigned long millis() { return nowMs; }
constexpr unsigned long POPUP_DURATION_MS = 1500;
struct Settings {
  bool touchReaderControls = true;
} SETTINGS;
struct RenderLock {
  enum class Mode { Try, Blocking };
  explicit RenderLock(Mode) {}
  bool ownsLock() const { return true; }
};
struct MappedInputManager {
  enum class Button { Back, Confirm };
  enum class Direction { Left, Right, Up, Down };
  bool touch = true, held = false, pressed = false, longEdge = false, back = false, confirm = false;
  bool suppressed = false;
  float nx = 0, ny = 0;
  int suppressionCalls = 0;
  unsigned long heldMs = 10;
  bool moved = false, cancelOnSecondSample = false, delayOnSecondSample = false;
  mutable unsigned candidateSamples = 0;
  bool screenTouchCandidateHeldMs(unsigned long& age) const {
    ++candidateSamples;
    if (cancelOnSecondSample && candidateSamples == 2) return false;
    age = heldMs;
    if (delayOnSecondSample && candidateSamples == 2) {
      nowMs += 1000;
      age += 1000;
    }
    return held && !suppressed && !moved;
  }
  bool hasTouch() const { return touch; }
  bool wasScreenTouchPressed(int&, int&) const { return pressed; }
  bool isScreenTouchHeld(int&, int&) const { return held && !suppressed; }
  bool peekScreenLongPressIn(touchtransform::Orientation orientation, int& x, int& y) const {
    if (!longEdge) return false;
    touchtransform::tapToLogical(orientation, 800, 480, nx, ny, x, y);
    return true;
  }
  void suppressTouchContact() {
    suppressed = true;
    ++suppressionCalls;
  }
  bool wasReleased(Button b) const { return b == Button::Back ? back : confirm; }
  bool wasLogicalPressed(Direction) const { return false; }
};
struct FontCacheManager {
  int scans = 0, prewarms = 0;
  struct Scope {
    FontCacheManager& manager;
    void endScanAndPrewarm() { ++manager.prewarms; }
  };
  Scope createPrewarmScope() {
    ++scans;
    return {*this};
  }
};
struct GfxRenderer {
  FontCacheManager cache;
  uint8_t orientation = 0;
  FontCacheManager* getFontCacheManager() { return &cache; }
  uint8_t getHeldOrientation() const { return orientation; }
  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }
  int getLineHeight(int) const { return 20; }
  struct Range {
    int x, right;
  };
  Range getTextRangeSpaced(int, const char*, int, float, float, int first, int last, bool) {
    return {first * 6, (last + 1) * 6};
  }
};
struct EpdFontFamily {
  enum Style { REGULAR, BOLD, ITALIC };
};
namespace wordTypography {
float tracking(uint8_t) { return 0; }
bool ligatures(uint8_t) { return false; }
}  // namespace wordTypography
struct TextBlock {
  struct WordBox {
    int16_t x, y, width, height;
    int fontId;
    EpdFontFamily::Style style;
    float scale;
  };
  struct Token {
    std::string text;
    int x;
    bool continuation = false;
  };
  std::vector<Token> tokens;
  bool valid() const { return true; }
  uint16_t wordCount() const { return static_cast<uint16_t>(tokens.size()); }
  bool wordContinues(int i) const { return tokens[i].continuation; }
  const char* wordText(int i) const { return tokens[i].text.c_str(); }
  size_t wordTextLen(int i) const { return tokens[i].text.size(); }
  uint8_t wordTypography(int) const { return 0; }
  WordBox wordBox(GfxRenderer&, int i, int font, int x, int y) const {
    return {static_cast<int16_t>(tokens[i].x + x),
            static_cast<int16_t>(y),
            static_cast<int16_t>(tokens[i].text.size() * 6),
            20,
            font,
            EpdFontFamily::REGULAR,
            1.0f};
  }
};
enum { TAG_PageLine, TAG_PageTable, TAG_PageImage };
constexpr int TABLE_CELL_PADDING = 5;
struct PageElement {
  int xPos = 0, yPos = 0;
  virtual int getTag() const = 0;
  virtual ~PageElement() = default;
};
struct PageLine : PageElement {
  std::unique_ptr<TextBlock> block;
  const TextBlock* getBlock() const { return block.get(); }
  int getTag() const override { return TAG_PageLine; }
};
struct TableCell {
  std::vector<std::unique_ptr<TextBlock>> lines;
  uint8_t colSpan = 1;
  uint16_t lineAdvance = 0;
};
struct TableRow {
  std::vector<TableCell> cells;
  int height = 0;
};
struct PageTableFragment : PageElement {
  int width = 0;
  uint8_t columns = 0;
  std::vector<TableRow> rows;
  int getTag() const override { return TAG_PageTable; }
  uint8_t getColumnCount() const { return columns; }
  int getTotalWidth() const { return width; }
  const std::vector<TableRow>& getRows() const { return rows; }
};
struct Page {
  std::vector<std::unique_ptr<PageElement>> elements;
  int scans = 0;
  void renderTextOnly(GfxRenderer&, int, int, int) { ++scans; }
};
struct DictionarySwitchResult {
  int8_t direction;
};
struct ActivityResult {
  std::variant<std::monostate, DictionarySwitchResult> data;
};
class DictionaryWordSelectActivity {
 public:
  struct DrawStyle {
    float scale;
    int fontId;
    int16_t height;
  };
  struct Fragment {
    const char* text;
    int16_t x, y, width;
    uint16_t length;
    uint8_t drawStyle;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  };
  struct Word {
    uint16_t firstFragment;
    uint8_t fragmentCount, hyphenFragment;
    uint16_t row = 0;
    int16_t x = 0, width = 0;
  };
  enum class Popup { None, Busy, NotFound, Error };
  enum class WordTouch { None, Consumed, Moved, Activate };
  static constexpr uint8_t NO_HYPHEN = 0xFF;
  GfxRenderer renderer;
  MappedInputManager mappedInput;
  std::unique_ptr<Page> page = std::make_unique<Page>();
  int fontId = 1, marginLeft = 0, marginTop = 0, selected = 0;
  std::vector<DrawStyle> drawStyles{{1.0f, 1, 20}, {1.0f, 1, 12}};
  std::vector<Fragment> fragments;
  std::vector<Word> words;
  bool wordsExtracted = true, directLookup = false, lookupPending = false;
  int8_t pendingDictionarySwitch = 0;
  int snapshotIdx = 3;
  Popup popup = Popup::None;
  unsigned long popupTime = 0, lastMoveMs = 0;
  int lookups = 0, finishes = 0, updates = 0, switches = 0, renders = 0;
  WordTouch touch = WordTouch::None;
  uint16_t rowCount = 0;
  static constexpr size_t WORD_TEXT_CAPACITY = 128;
  using WordLineVisitor = void (*)(void*, const TextBlock&, int, int, bool);
  void visitWordLines(void*, WordLineVisitor) const;
  size_t countPageTokens(size_t&) const;
  uint8_t internDrawStyle(const TextBlock::WordBox&);
  void extractWords();
  void extractLineWords(const TextBlock&, int, int, bool);
  void joinHyphenatedWords();
  int closestInRow(uint16_t, int) const;
  const DrawStyle& drawStyleOf(const Fragment& f) const { return drawStyles[f.drawStyle]; }
  bool prepareLookupAt(int, int);
  size_t buildWordText(int, char*, size_t, bool) const;
  int wordAt(int, int) const;
  void handleDefinitionResult(const ActivityResult&);
  void loop();
  void render(RenderLock&&);
  void switchDictionary(int) { ++switches; }
  void performLookup() { ++lookups; }
  void requestUpdate() { ++updates; }
  void finish() { ++finishes; }
  WordTouch consumeWordTouch() { return touch; }
  void moveVertical(int) {}
  void speculateForSelection() {}
};
struct Section {
  int currentPage = 4, pageCount = 10;
};
class EpubReaderActivity {
 public:
  MappedInputManager mappedInput;
  GfxRenderer renderer;
  std::unique_ptr<Section> section = std::make_unique<Section>();
  int currentSpineIndex = 2, lastRenderedSpineIndex_ = 2, lastRenderedPageIndex_ = 4;
  unsigned long lastPageOnScreenMs_ = 1000;
  ReaderWordHold dictionaryHold_;
  struct Buttons {
    bool active = false;
    bool isGestureInFlight() const { return active; }
  } buttonEvents;
  bool wordHit = true;
  int opens = 0, lookupX = -1, lookupY = -1;
  ReaderWordHold::Page dictionaryTouchPage() const;
  bool handleReaderWordHold();
  bool openDictionary(int x, int y) {
    if (!dictionaryHold_.consume(dictionaryTouchPage())) return true;
    if (!wordHit) return false;
    ++opens;
    lookupX = x;
    lookupY = y;
    return true;
  }
};

// PRODUCTION_METHODS

int checks = 0;
void check(bool ok, const char* message) {
  ++checks;
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}
void startHold(EpubReaderActivity& reader, int orientation = 0) {
  reader.mappedInput = {};
  reader.mappedInput.held = reader.mappedInput.pressed = true;
  reader.mappedInput.nx = .4f;
  reader.mappedInput.ny = .3f;
  reader.renderer.orientation = static_cast<uint8_t>(orientation);
  nowMs = 2000;
  reader.handleReaderWordHold();
  reader.mappedInput.pressed = false;
}
DictionaryWordSelectActivity pageWords() {
  DictionaryWordSelectActivity selection;
  // Two styled fragments and a word split over a line. Gaps are intentional.
  selection.fragments = {{"read", 20, 50, 28, 4, 0},
                         {"ing", 48, 50, 18, 3, 0},
                         {"care-", 200, 50, 30, 5, 0},
                         {"fully", 20, 80, 30, 5, 0},
                         {"small", 90, 80, 30, 5, 1}};
  selection.words = {{0, 2, 255}, {2, 2, 0}, {4, 1, 255}};
  return selection;
}
int main() {
  for (int orientation = 0; orientation < 4; ++orientation) {
    EpubReaderActivity reader;
    startHold(reader, orientation);
    check(!reader.handleReaderWordHold() && reader.opens == 0, "short contact does not open dictionary");
    reader.mappedInput.longEdge = true;
    int x, y;
    touchtransform::tapToLogical(orientation, 800, 480, .4f, .3f, x, y);
    check(reader.handleReaderWordHold() && reader.opens == 1, "hold opens once in each orientation");
    check(reader.lookupX == x && reader.lookupY == y, "held orientation maps the word point");
    check(!reader.handleReaderWordHold() && reader.opens == 1, "suppression blocks duplicate current edge");
    reader.mappedInput.held = false;
    reader.handleReaderWordHold();
    check(!reader.handleReaderWordHold() && reader.opens == 1, "release cannot reopen");
  }
  {
    EpubReaderActivity r;
    startHold(r);
    r.mappedInput.longEdge = true;
    r.mappedInput.suppressTouchContact();  // earlier configured global gesture
    r.handleReaderWordHold();
    check(!r.handleReaderWordHold() && r.opens == 0, "configured gesture wins even with SDK edge still present");
  }
  for (int change = 0; change < 5; ++change) {
    EpubReaderActivity r;
    startHold(r);
    r.mappedInput.longEdge = true;
    if (change == 0) {
      ++r.section->currentPage;
      ++r.lastRenderedPageIndex_;
    }
    if (change == 1) {
      ++r.currentSpineIndex;
      ++r.lastRenderedSpineIndex_;
    }
    if (change == 2) ++r.lastPageOnScreenMs_;
    if (change == 3) ++r.renderer.orientation;
    if (change == 4) {
      r.dictionaryHold_.reset();
      r.lastPageOnScreenMs_ = 1999;
    }  // return repaint
    r.handleReaderWordHold();
    r.handleReaderWordHold();
    check(r.opens == 0, "page/reflow/orientation/navigation change cancels hold");
  }
  {
    EpubReaderActivity r;
    startHold(r);
    r.mappedInput.held = false;
    r.handleReaderWordHold();
    r.mappedInput.held = true;
    r.mappedInput.moved = true;  // SDK disqualifies the rest of a moved/multi-contact sequence
    r.mappedInput.longEdge = true;
    r.handleReaderWordHold();
    check(r.opens == 0, "SDK movement/multitouch cancellation prevents lookup");
  }
  {
    EpubReaderActivity r;
    startHold(r);
    SETTINGS.touchReaderControls = false;
    r.mappedInput.longEdge = true;
    check(r.handleReaderWordHold() && r.opens == 1, "page turns off does not disable word lookup");
    SETTINGS.touchReaderControls = true;
    r.mappedInput.touch = false;
    check(!r.handleReaderWordHold(), "non-touch hardware stays inert");
  }
  {
    EpubReaderActivity r;
    startHold(r);
    r.lastRenderedPageIndex_ = -1;
    r.handleReaderWordHold();
    r.mappedInput.longEdge = true;
    r.handleReaderWordHold();
    check(r.opens == 0, "undisplayed page never supplies a word");
  }
  {
    EpubReaderActivity r;
    r.mappedInput.held = true;
    r.mappedInput.heldMs = 550;
    r.mappedInput.longEdge = true;
    nowMs = 2000;
    check(r.handleReaderWordHold() && r.opens == 1, "missed sampler press edge does not lose a hold");
  }
  {
    EpubReaderActivity r;
    r.mappedInput.held = true;
    r.mappedInput.heldMs = 550;
    r.mappedInput.longEdge = true;
    r.lastPageOnScreenMs_ = 1800;
    nowMs = 2000;
    check(r.handleReaderWordHold() && r.opens == 0,
          "first observation after page change cannot choose new page's word");
  }
  {
    EpubReaderActivity r;
    startHold(r);
    r.wordHit = false;
    r.mappedInput.longEdge = true;
    check(!r.handleReaderWordHold() && !r.mappedInput.suppressed && r.opens == 0,
          "margin/image miss leaves existing zone long-press action intact");
  }
  {
    EpubReaderActivity r;
    startHold(r);
    r.buttonEvents.active = true;
    r.mappedInput.longEdge = true;
    check(r.handleReaderWordHold() && r.mappedInput.suppressed && r.opens == 0,
          "physical button takes priority over simultaneous word hold");
  }
  {
    ReaderWordHold hold;
    ReaderWordHold::Page page{2, 3, 0xfffffff0u, 0};
    hold.observe(true, true, page, 0x100u, 200u);
    check(hold.consume(page), "contact age comparison works across millis wrap");
    check(!hold.consume(page), "consumption is one-shot even without reset");
  }
  {
    EpubReaderActivity r;
    r.mappedInput.held = r.mappedInput.longEdge = true;
    r.mappedInput.heldMs = 550;
    r.mappedInput.cancelOnSecondSample = true;
    nowMs = 2000;
    check(r.handleReaderWordHold() && r.opens == 0 && r.mappedInput.suppressed,
          "release/movement during a render lock wait cancels without zone fallback");
  }
  {
    EpubReaderActivity r;
    r.mappedInput.held = r.mappedInput.longEdge = true;
    r.mappedInput.heldMs = 550;
    r.mappedInput.delayOnSecondSample = true;
    r.lastPageOnScreenMs_ = 1800;
    nowMs = 2000;
    check(r.handleReaderWordHold() && r.opens == 0,
          "lock wait resamples age so an old contact cannot acquire a new page");
  }
  auto s = pageWords();
  for (int x = 20; x < 66; ++x) check(s.wordAt(x, 50) == 0, "both style fragments select full word");
  for (int x = 66; x < 200; ++x) check(s.wordAt(x, 50) == -1, "inter-word whitespace has no nearest fallback");
  check(s.wordAt(200, 50) == 1 && s.wordAt(20, 80) == 1, "both halves of a hyphenated word hit same entry");
  check(s.wordAt(100, 91) == 2 && s.wordAt(100, 92) == -1, "small text uses its own height");
  check(s.wordAt(20, 49) == -1 && s.wordAt(20, 70) == -1, "line gaps excluded");
  check(s.wordAt(0, 0) == -1 && s.wordAt(479, 799) == -1, "margins do not select a remote word");
  char word[128];
  s.buildWordText(0, word, sizeof(word), true);
  check(std::string(word) == "reading", "styled fragments join for lookup");
  s.buildWordText(1, word, sizeof(word), true);
  check(std::string(word) == "carefully", "line-break hyphen omitted for first lookup");
  s.buildWordText(1, word, sizeof(word), false);
  check(std::string(word) == "care-fully", "compound retry preserves hyphen");
  check(!s.prepareLookupAt(100, 50) && !s.lookupPending && !s.directLookup, "miss never arms lookup");
  check(s.prepareLookupAt(22, 80) && s.selected == 1 && s.lookupPending, "exact held fragment selects its word");
  check(s.renderer.cache.prewarms == 2 && s.page->scans == 2 && s.updates == 0, "preflight warms without refreshing");
  s.render(RenderLock(RenderLock::Mode::Blocking));
  check(s.renders == 0, "queued reader repaint does not draw selector before direct lookup");
  s.loop();
  s.loop();
  s.render(RenderLock(RenderLock::Mode::Blocking));
  check(s.renders == 0, "cleared lookup flag does not permit a concurrent idle selector repaint");
  s.popup = DictionaryWordSelectActivity::Popup::Busy;
  s.render(RenderLock(RenderLock::Mode::Blocking));
  check(s.renders == 1, "direct lookup busy rendering remains enabled");
  s.popup = DictionaryWordSelectActivity::Popup::None;
  check(s.lookups == 1 && !s.lookupPending, "lookup starts once without another tap or Confirm");
  s.handleDefinitionResult({});
  check(s.finishes == 1 && s.updates == 0 && s.snapshotIdx == -1,
        "Back from direct definition returns to book without selector redraw");
  auto menu = pageWords();
  menu.render(RenderLock(RenderLock::Mode::Blocking));
  check(menu.renders == 1, "normal menu selector still paints immediately");
  menu.handleDefinitionResult({});
  check(menu.finishes == 0 && menu.updates == 1, "menu lookup still returns to selection");
  auto switcher = pageWords();
  switcher.directLookup = true;
  switcher.handleDefinitionResult({DictionarySwitchResult{1}});
  switcher.render(RenderLock(RenderLock::Mode::Blocking));
  check(switcher.renders == 0, "direct dictionary switch keeps definition visible before deferred lookup");
  switcher.loop();
  switcher.render(RenderLock(RenderLock::Mode::Blocking));
  check(switcher.renders == 0, "cleared dictionary switch flag does not permit a concurrent idle repaint");
  switcher.popup = DictionaryWordSelectActivity::Popup::Busy;
  switcher.render(RenderLock(RenderLock::Mode::Blocking));
  check(switcher.renders == 1, "direct dictionary switch permits its busy repaint");
  check(switcher.finishes == 0 && switcher.switches == 1 && switcher.pendingDictionarySwitch == 0,
        "direct lookup supports switching dictionaries");
  auto menuSwitcher = pageWords();
  menuSwitcher.handleDefinitionResult({DictionarySwitchResult{-1}});
  menuSwitcher.render(RenderLock(RenderLock::Mode::Blocking));
  check(menuSwitcher.renders == 1, "normal dictionary selector switch preserves repaint behavior");
  for (auto failure : {DictionaryWordSelectActivity::Popup::NotFound, DictionaryWordSelectActivity::Popup::Error}) {
    auto missing = pageWords();
    missing.directLookup = true;
    missing.popup = failure;
    missing.render(RenderLock(RenderLock::Mode::Blocking));
    check(missing.renders == 1, "direct lookup errors and not-found messages remain visible");
    nowMs = 1499;
    missing.loop();
    check(missing.finishes == 0, "lookup message remains readable");
    nowMs = 1500;
    missing.loop();
    check(missing.finishes == 1 && missing.updates == 0, "direct miss/error returns to book after message");
  }
  auto cancelled = pageWords();
  cancelled.directLookup = true;
  cancelled.popup = DictionaryWordSelectActivity::Popup::Error;
  cancelled.mappedInput.back = true;
  nowMs = 1;
  cancelled.loop();
  check(cancelled.finishes == 1, "Back dismisses direct lookup error immediately");

  {
    auto block = [](const char* text) {
      auto result = std::make_unique<TextBlock>();
      result->tokens.push_back({text, 0, false});
      return result;
    };
    DictionaryWordSelectActivity tableSelection;
    tableSelection.marginLeft = 11;
    tableSelection.marginTop = 13;
    auto table = std::make_unique<PageTableFragment>();
    table->xPos = 7;
    table->yPos = 17;
    table->width = 301;
    table->columns = 3;
    TableRow first;
    first.height = 60;
    TableCell span;
    span.colSpan = 2;
    span.lineAdvance = 23;
    span.lines.push_back(block("care-"));
    span.lines.push_back(block("fully"));
    first.cells.push_back(std::move(span));
    TableCell right;
    right.lines.push_back(block("other-"));
    first.cells.push_back(std::move(right));
    // Extra cell lies past the table's grid and render() skips it too.
    TableCell hidden;
    hidden.lines.push_back(block("hidden"));
    first.cells.push_back(std::move(hidden));
    table->rows.push_back(std::move(first));
    TableRow second;
    second.height = 32;
    TableCell following;
    following.lines.push_back(block("next"));
    second.cells.push_back(std::move(following));
    table->rows.push_back(std::move(second));
    tableSelection.page->elements.push_back(std::move(table));
    tableSelection.wordsExtracted = false;
    tableSelection.extractWords();
    check(tableSelection.words.size() == 3, "table spans and skipped overflow cells match rendered words");
    check(tableSelection.wordAt(23, 35) == 0 && tableSelection.wordAt(23, 58) == 0,
          "table padding, page offsets and persisted line advance match both hyphen fragments");
    check(tableSelection.wordAt(22, 35) == -1 && tableSelection.wordAt(23, 34) == -1,
          "table padding is not selectable text");
    const int rightWord = tableSelection.wordAt(223, 35);  // floor(301 * 2 / 3) + 7 + 11 + 5
    check(rightWord == 1, "colspan positions use renderer integer column division");
    check(tableSelection.wordAt(23, 95) == 2, "next table row uses saved height");
    tableSelection.buildWordText(0, word, sizeof(word), true);
    check(std::string(word) == "carefully", "hyphen joins inside a table cell");
    tableSelection.buildWordText(rightWord, word, sizeof(word), true);
    check(std::string(word) == "other-", "hyphen never joins into an unrelated table cell");
    check(tableSelection.wordAt(480, 35) == -1 && tableSelection.wordAt(23, 800) == -1,
          "off-panel table overflow cannot be selected");
  }
  std::printf("%d dictionary touch routing, geometry, word assembly and lifecycle checks passed\n", checks);
}
