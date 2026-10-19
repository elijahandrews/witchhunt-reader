#include "SvgRasterizer.h"

#include <CooperativeAbort.h>
#include <HalSystem.h>
#include <Logging.h>
#include <yxml.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float FLATNESS = 0.1f;  // final display pixels, not source units
constexpr int MAX_DEPTH = 16;
constexpr int SUBPIXELS = 4;
struct Point {
  float x, y;
};
struct Edge {
  Point a, b;
};
struct Paint {
  uint8_t gray = 0, alpha = 255;
  bool none = false, evenOdd = false, hidden = false;
};
struct Shape {
  uint16_t first = 0, count = 0;
  Paint paint;
};
struct Crossing {
  float x;
  int winding;
};

bool aborted() {
  if (!CooperativeAbort::shouldAbortLongTask()) return false;
  CooperativeAbort::markAborted();
  return true;
}
bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
std::string_view trim(std::string_view s) {
  while (!s.empty() && space(s.front())) s.remove_prefix(1);
  while (!s.empty() && space(s.back())) s.remove_suffix(1);
  return s;
}
std::string_view local(const char* name) {
  const char* colon = strrchr(name, ':');
  return colon ? colon + 1 : name;
}
bool equal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char c = a[i];
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (c != b[i]) return false;
  }
  return true;
}
void separators(const char*& p) {
  while (space(*p) || *p == ',') ++p;
}
bool number(const char*& p, float& value) {
  separators(p);
  const char* begin = p;
  if (*p == '+' || *p == '-') ++p;
  bool digits = false;
  while (*p >= '0' && *p <= '9') {
    ++p;
    digits = true;
  }
  if (*p == '.') {
    ++p;
    while (*p >= '0' && *p <= '9') {
      ++p;
      digits = true;
    }
  }
  if (!digits) return false;
  if (*p == 'e' || *p == 'E') {
    ++p;
    if (*p == '+' || *p == '-') ++p;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') ++p;
  }
  const size_t size = static_cast<size_t>(p - begin);
  if (size >= 48) return false;
  char token[48];
  memcpy(token, begin, size);
  token[size] = 0;
  value = strtof(token, nullptr);
  return std::isfinite(value) && std::abs(value) <= 1000000.0f;
}
bool scalar(std::string_view text, float& value) {
  if (text.size() >= 64) return false;
  char token[64];
  memcpy(token, text.data(), text.size());
  token[text.size()] = 0;
  const char* p = token;
  if (!number(p, value)) return false;
  while (space(*p)) ++p;
  return *p == 0;
}
bool length(const char* text, float& value) {
  const char* p = text;
  if (!number(p, value) || value <= 0) return false;
  auto suffix = trim(p);
  if (suffix.empty() || suffix == "px") return true;
  if (suffix == "%") {
    value = 0;
    return true;
  }  // intrinsic ratio comes from viewBox
  if (suffix == "pt")
    value *= 96.0f / 72;
  else if (suffix == "pc")
    value *= 16;
  else if (suffix == "in")
    value *= 96;
  else if (suffix == "cm")
    value *= 96.0f / 2.54f;
  else if (suffix == "mm")
    value *= 96.0f / 25.4f;
  else
    return false;
  return value <= 32767;
}
int hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
bool fill(std::string_view value, Paint& paint) {
  value = trim(value);
  if (equal(value, "none") || equal(value, "transparent")) {
    paint.none = true;
    return true;
  }
  if (equal(value, "inherit")) return true;
  unsigned r = 0, g = 0, b = 0;
  if (value.size() == 4 && value[0] == '#') {
    const int x = hex(value[1]), y = hex(value[2]), z = hex(value[3]);
    if (x < 0 || y < 0 || z < 0) return false;
    r = x * 17;
    g = y * 17;
    b = z * 17;
  } else if (value.size() == 7 && value[0] == '#') {
    unsigned rgb = 0;
    for (size_t i = 1; i < 7; ++i) {
      const int digit = hex(value[i]);
      if (digit < 0) return false;
      rgb = rgb * 16 + digit;
    }
    r = rgb >> 16;
    g = (rgb >> 8) & 255;
    b = rgb & 255;
  } else if (equal(value, "white"))
    r = g = b = 255;
  else if (equal(value, "gray") || equal(value, "grey"))
    r = g = b = 128;
  else if (!equal(value, "black"))
    return false;
  paint.gray = static_cast<uint8_t>((r * 77 + g * 150 + b * 29 + 128) >> 8);
  paint.none = false;
  return true;
}
bool property(std::string_view name, std::string_view value, Paint& paint) {
  name = trim(name);
  value = trim(value);
  if (name == "fill") return fill(value, paint);
  if (name == "fill-rule") {
    if (value == "inherit") return true;
    if (value != "evenodd" && value != "nonzero") return false;
    paint.evenOdd = value == "evenodd";
    return true;
  }
  if (name == "fill-opacity") {
    if (value == "inherit") return true;
    float v = 0;
    if (!scalar(value, v)) return false;
    paint.alpha = static_cast<uint8_t>(std::lround(std::max(0.0f, std::min(1.0f, v)) * 255));
    return true;
  }
  if (name == "display") {
    if (value == "none")
      paint.hidden = true;
    else if (value != "inline")
      return false;
    return true;
  }
  // Unsupported paint effects fail the whole image before any pixels/cache are published.
  if (name == "stroke" || name == "clip-path" || name == "mask" || name == "filter") return value == "none";
  if (name == "opacity") {
    float v = 0;
    return scalar(value, v) && v == 1;
  }
  if (name == "transform") return value.empty();
  return false;
}
Point midpoint(Point a, Point b) { return {(a.x + b.x) * .5f, (a.y + b.y) * .5f}; }
}  // namespace

struct SvgRasterizer::State {
  std::unique_ptr<Edge[]> edges;
  Shape shapes[MAX_PATHS];
  size_t edgeCount = 0, capacity = 0, shapeCount = 0;
  int width = 0, height = 0;
  float sourceWidth = 0, sourceHeight = 0, view[4] = {};
  float sx = 1, sy = 1, tx = 0, ty = 0;
  bool hasView = false, stretch = false;

  void resetGeometry() {
    edgeCount = shapeCount = 0;
    sourceWidth = sourceHeight = 0;
    hasView = stretch = false;
    sx = sy = 1;
    tx = ty = 0;
    memset(view, 0, sizeof(view));
  }
  bool viewport() {
    if (sourceWidth <= 0 && hasView) sourceWidth = view[2];
    if (sourceHeight <= 0 && hasView) sourceHeight = view[3];
    if (!(sourceWidth > 0 && sourceHeight > 0 && sourceWidth <= 32767 && sourceHeight <= 32767)) return false;
    if (width <= 0 || height <= 0) return true;  // dimension probe
    if (!hasView) {
      view[0] = view[1] = 0;
      view[2] = sourceWidth;
      view[3] = sourceHeight;
    }
    sx = width / view[2];
    sy = height / view[3];
    if (!stretch) sx = sy = std::min(sx, sy);
    tx = (width - view[2] * sx) * .5f - view[0] * sx;
    ty = (height - view[3] * sy) * .5f - view[1] * sy;
    return std::isfinite(sx) && std::isfinite(sy) && std::isfinite(tx) && std::isfinite(ty);
  }
  Point map(Point p) const { return {p.x * sx + tx, p.y * sy + ty}; }
  bool edge(Point a, Point b) {
    if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y) ||
        std::abs(a.x) > 10000000 || std::abs(a.y) > 10000000 || std::abs(b.x) > 10000000 || std::abs(b.y) > 10000000)
      return false;
    if (a.y == b.y) return true;
    if (edgeCount >= MAX_EDGES || (edges && edgeCount >= capacity)) return false;
    if (edges) edges[edgeCount] = {a, b};
    ++edgeCount;
    return true;
  }
  bool cubic(Point a, Point b, Point c, Point d, unsigned depth = 0) {
    const float dx = d.x - a.x, dy = d.y - a.y;
    const float chord = dx * dx + dy * dy;
    const float db = std::abs((b.x - a.x) * dy - (b.y - a.y) * dx);
    const float dc = std::abs((c.x - a.x) * dy - (c.y - a.y) * dx);
    // Control-polygon excess also catches collinear reversals, which a cross product misses.
    const float polygon =
        std::hypot(b.x - a.x, b.y - a.y) + std::hypot(c.x - b.x, c.y - b.y) + std::hypot(d.x - c.x, d.y - c.y);
    if ((db + dc) * (db + dc) <= FLATNESS * FLATNESS * chord && polygon - std::sqrt(chord) <= FLATNESS)
      return edge(a, d);
    if (depth >= 12) return false;  // do not cache an unreported coarse approximation
    const Point ab = midpoint(a, b), bc = midpoint(b, c), cd = midpoint(c, d);
    const Point abc = midpoint(ab, bc), bcd = midpoint(bc, cd), middle = midpoint(abc, bcd);
    return cubic(a, ab, abc, middle, depth + 1) && cubic(middle, bcd, cd, d, depth + 1);
  }
  bool arc(Point start, float rx, float ry, float rotation, bool large, bool sweep, Point end) {
    if (start.x == end.x && start.y == end.y) return true;
    rx = std::abs(rx);
    ry = std::abs(ry);
    if (rx == 0 || ry == 0) return edge(map(start), map(end));
    // SVG 1.1 appendix F.6 endpoint-to-center conversion, including radii correction.
    const float phi = std::fmod(rotation, 360.0f) * kPi / 180;
    const float co = std::cos(phi), si = std::sin(phi);
    const float dx = (start.x - end.x) * .5f, dy = (start.y - end.y) * .5f;
    const float xp = co * dx + si * dy, yp = -si * dx + co * dy;
    const float lambda = xp * xp / (rx * rx) + yp * yp / (ry * ry);
    if (lambda > 1) {
      const float scale = std::sqrt(lambda);
      rx *= scale;
      ry *= scale;
    }
    const float ux = xp / rx, uy = yp / ry;
    const float denominator = ux * ux + uy * uy;
    if (!(denominator > 0) || !std::isfinite(denominator)) return false;
    const float coefficient =
        (large == sweep ? -1.0f : 1.0f) * std::sqrt(std::max(0.0f, (1 - denominator) / denominator));
    const float cxp = coefficient * rx * uy, cyp = -coefficient * ry * ux;
    const float cx = co * cxp - si * cyp + (start.x + end.x) * .5f;
    const float cy = si * cxp + co * cyp + (start.y + end.y) * .5f;
    const float ax = (xp - cxp) / rx, ay = (yp - cyp) / ry;
    const float bx = (-xp - cxp) / rx, by = (-yp - cyp) / ry;
    const float angle = std::atan2(ay, ax);
    float delta = std::atan2(ax * by - ay * bx, ax * bx + ay * by);
    if (!sweep && delta > 0) delta -= 2 * kPi;
    if (sweep && delta < 0) delta += 2 * kPi;
    const float radius = std::max(rx, ry) * std::max(std::abs(sx), std::abs(sy));
    const float step = std::min(kPi / 4, std::sqrt(8 * FLATNESS / std::max(radius, FLATNESS)));
    const float count = std::ceil(std::abs(delta) / step);
    if (!std::isfinite(count) || count > MAX_EDGES) return false;
    const unsigned segments = std::max(1u, static_cast<unsigned>(count));
    Point previous = map(start);
    for (unsigned i = 1; i <= segments; ++i) {
      const float theta = angle + delta * (static_cast<float>(i) / segments);
      const Point point = i == segments ? map(end)
                                        : map({cx + co * rx * std::cos(theta) - si * ry * std::sin(theta),
                                               cy + si * rx * std::cos(theta) + co * ry * std::sin(theta)});
      if (!edge(previous, point)) return false;
      previous = point;
    }
    return true;
  }
  bool path(const char* data, Paint paint) {
    if (shapeCount >= MAX_PATHS) return false;
    const size_t first = edgeCount;
    Point current{}, begin{}, control{};
    bool open = false, moved = false;
    char command = 0, previous = 0;
    const auto close = [&]() { return !open || edge(map(current), map(begin)); };
    while (true) {
      separators(data);
      if (!*data) break;
      if ((*data >= 'A' && *data <= 'Z') || (*data >= 'a' && *data <= 'z'))
        command = *data++;
      else if (!command || command == 'Z' || command == 'z')
        return false;
      const bool relative = command >= 'a' && command <= 'z';
      const char kind = relative ? command - ('a' - 'A') : command;
      if (!moved && kind != 'M') return false;
      if (kind == 'Z') {
        if (!close()) return false;
        current = begin;
        open = false;
        previous = kind;
        continue;
      }
      float values[7] = {};
      int count = kind == 'H' || kind == 'V' ? 1 : (kind == 'C' ? 6 : (kind == 'S' || kind == 'Q' ? 4 : 2));
      if (kind == 'A') count = 7;
      if (kind != 'M' && kind != 'L' && kind != 'H' && kind != 'V' && kind != 'C' && kind != 'S' && kind != 'Q' &&
          kind != 'T' && kind != 'A')
        return false;
      for (int i = 0; i < count; ++i) {
        if (kind == 'A' && (i == 3 || i == 4)) {
          separators(data);
          if (*data != '0' && *data != '1') return false;
          values[i] = *data++ - '0';
        } else if (!number(data, values[i]))
          return false;
      }
      const auto point = [&](int i) {
        return Point{values[i] + (relative ? current.x : 0), values[i + 1] + (relative ? current.y : 0)};
      };
      Point end = current;
      if (kind == 'M') {
        if (!close()) return false;
        current = begin = point(0);
        moved = open = true;
        command = relative ? 'l' : 'L';
        previous = kind;
        continue;
      }
      if (kind == 'H')
        end.x = values[0] + (relative ? current.x : 0);
      else if (kind == 'V')
        end.y = values[0] + (relative ? current.y : 0);
      else
        end = point(count - 2);
      if (kind == 'C' || kind == 'S') {
        const Point b = kind == 'C' ? point(0)
                                    : ((previous == 'C' || previous == 'S')
                                           ? Point{2 * current.x - control.x, 2 * current.y - control.y}
                                           : current);
        const Point c = point(count - 4);
        if (!cubic(map(current), map(b), map(c), map(end))) return false;
        control = c;
      } else if (kind == 'Q' || kind == 'T') {
        const Point q = kind == 'Q' ? point(0)
                                    : ((previous == 'Q' || previous == 'T')
                                           ? Point{2 * current.x - control.x, 2 * current.y - control.y}
                                           : current);
        const Point b{current.x + (q.x - current.x) * (2.0f / 3), current.y + (q.y - current.y) * (2.0f / 3)};
        const Point c{end.x + (q.x - end.x) * (2.0f / 3), end.y + (q.y - end.y) * (2.0f / 3)};
        if (!cubic(map(current), map(b), map(c), map(end))) return false;
        control = q;
      } else if (kind == 'A') {
        if (!arc(current, values[0], values[1], values[2], values[3] != 0, values[4] != 0, end)) return false;
      } else if (!edge(map(current), map(end)))
        return false;
      current = end;
      previous = kind;
      open = true;
    }
    if (!close()) return false;
    shapes[shapeCount++] = {static_cast<uint16_t>(first), static_cast<uint16_t>(edgeCount - first), paint};
    return true;
  }

  struct Parser {
    State& state;
    yxml_t xml{};
    char names[1024];
    char value[256]{};
    char inlineStyle[256]{};
    // A separate, lazy 8 KiB attribute buffer is necessary: SaxParser's 384 B
    // attribute cap truncates otherwise valid vector paths. Reused across paths.
    std::unique_ptr<char[]> pathData;
    Paint styles[MAX_DEPTH];
    bool ignored[MAX_DEPTH]{};
    size_t pathSize = 0, valueSize = 0, consumed = 0;
    int depth = 0;
    bool headerOnly, pending = false, inAttribute = false, ready = false, valid = true;
    bool pathElement = false, rootElement = false, capturePath = false, captureValue = false;
    bool hasPath = false;
    std::string_view attribute;
    Parser(State& s, bool header) : state(s), headerOnly(header) { yxml_init(&xml, names, sizeof(names)); }
    bool finishStart() {
      if (!pending) return true;
      pending = false;
      // Inline CSS wins over presentation attributes regardless of XML order.
      if (!ignored[depth - 1] && !applyStyle()) return false;
      if (rootElement) {
        if (!state.viewport()) return false;
        ready = true;
      }
      if (!headerOnly && pathElement && !ignored[depth - 1]) {
        if (!hasPath) return true;
        pathData[pathSize] = 0;
        if (!state.path(pathData.get(), styles[depth - 1])) return false;
      }
      return true;
    }
    bool applyAttribute() {
      if (capturePath) {
        hasPath = true;
        return true;
      }
      if (!captureValue) return true;
      value[valueSize] = 0;
      Paint& paint = styles[depth - 1];
      if (rootElement && attribute == "width") return length(value, state.sourceWidth);
      if (rootElement && attribute == "height") return length(value, state.sourceHeight);
      if (rootElement && attribute == "viewBox") {
        const char* p = value;
        for (float& v : state.view)
          if (!number(p, v)) return false;
        separators(p);
        state.hasView = true;
        return !*p && state.view[2] > 0 && state.view[3] > 0;
      }
      if (rootElement && attribute == "preserveAspectRatio") {
        const auto s = trim(value);
        if (s == "none")
          state.stretch = true;
        else if (s != "xMidYMid" && s != "xMidYMid meet")
          return false;
        return true;
      }
      if (attribute == "style") {
        memcpy(inlineStyle, value, valueSize + 1);
        return true;
      }
      return applyPaint(attribute, value, paint);
    }
    bool applyPaint(std::string_view name, std::string_view value, Paint& paint) {
      name = trim(name);
      value = trim(value);
      if (value == "inherit") {
        const Paint inherited = depth > 1 ? styles[depth - 2] : Paint{};
        if (name == "fill") {
          paint.gray = inherited.gray;
          paint.none = inherited.none;
          return true;
        }
        if (name == "fill-opacity") {
          paint.alpha = inherited.alpha;
          return true;
        }
        if (name == "fill-rule") {
          paint.evenOdd = inherited.evenOdd;
          return true;
        }
      }
      return property(name, value, paint);
    }
    bool applyStyle() {
      std::string_view s(inlineStyle);
      while (!s.empty()) {
        const size_t end = s.find(';');
        const auto d = s.substr(0, end);
        const size_t colon = d.find(':');
        if (!trim(d).empty() && (colon == std::string_view::npos ||
                                 !applyPaint(d.substr(0, colon), d.substr(colon + 1), styles[depth - 1])))
          return false;
        if (end == std::string_view::npos) break;
        s.remove_prefix(end + 1);
      }
      return true;
    }
    bool byte(uint8_t c) {
      if (++consumed > MAX_SOURCE_BYTES) return false;
      const auto token = yxml_parse(&xml, c);
      if (token < 0) return false;
      if (token == YXML_ELEMSTART) {
        if (!finishStart() || depth == MAX_DEPTH) return false;
        const auto tag = local(xml.elem);
        rootElement = depth == 0;
        pathElement = tag == "path";
        if (rootElement && tag != "svg") return false;
        const bool skip = (depth && ignored[depth - 1]) || tag == "title" || tag == "desc" || tag == "metadata";
        if (!skip && !rootElement && tag != "g" && !pathElement) return false;
        styles[depth] = depth ? styles[depth - 1] : Paint{};
        ignored[depth] = skip;
        ++depth;
        pending = true;
        hasPath = false;
        pathSize = 0;
        inlineStyle[0] = 0;
      } else if (token == YXML_ATTRSTART) {
        if (!pending || !depth) return false;
        inAttribute = true;
        attribute = xml.attr;
        valueSize = 0;
        capturePath = pathElement && attribute == "d" && !headerOnly && !ignored[depth - 1];
        captureValue =
            !ignored[depth - 1] &&
            (attribute == "style" || attribute == "fill" || attribute == "fill-rule" || attribute == "fill-opacity" ||
             attribute == "opacity" || attribute == "stroke" || attribute == "transform" || attribute == "clip-path" ||
             attribute == "mask" || attribute == "filter" || attribute == "display" ||
             (rootElement && (attribute == "width" || attribute == "height" || attribute == "viewBox" ||
                              attribute == "preserveAspectRatio")));
        if (capturePath && !pathData) {
          pathData.reset(new (std::nothrow) char[MAX_PATH_BYTES + 1]);
          if (!pathData) return false;
        }
      } else if (token == YXML_ATTRVAL) {
        for (const char* p = xml.data; *p; ++p) {
          if (capturePath) {
            if (pathSize >= MAX_PATH_BYTES) return false;
            pathData[pathSize++] = *p;
          } else if (captureValue) {
            if (valueSize + 1 >= sizeof(value)) return false;
            value[valueSize++] = *p;
          }
        }
      } else if (token == YXML_ATTREND) {
        if (!applyAttribute()) return false;
        inAttribute = false;
      } else if (token == YXML_ELEMEND) {
        if (!finishStart() || !depth) return false;
        --depth;
      }
      if (pending && c == '>' && !inAttribute && token != YXML_ATTRVAL && !finishStart()) return false;
      return true;
    }
    bool file(FsFile& source) {
      if (!source.seek(0)) return false;
      uint8_t buffer[128];
      while (source.available()) {
        if (aborted()) return false;
        HalSystem::feedWatchdog();
        const int n = source.read(buffer, sizeof(buffer));
        if (n <= 0) return false;
        for (int i = 0; i < n; ++i) {
          if (!byte(buffer[i])) return false;
          if (headerOnly && ready) return true;
        }
      }
      return ready && depth == 0 && yxml_eof(&xml) >= 0;
    }
  };
};

SvgRasterizer::SvgRasterizer() = default;
SvgRasterizer::~SvgRasterizer() = default;

bool SvgRasterizer::dimensionsFromBuffer(const uint8_t* data, size_t size, Dimensions& out) {
  if (!data || !size) return false;
  // Parser/state exceed the embedded task's stack budget; short-lived heap objects
  // are shared with the streaming decode rather than adding another XML parser.
  std::unique_ptr<State> state(new (std::nothrow) State());
  if (!state) return false;
  std::unique_ptr<State::Parser> parser(new (std::nothrow) State::Parser(*state, true));
  if (!parser) return false;
  for (size_t i = 0; i < size; ++i) {
    if (!parser->byte(data[i])) return false;
    if (parser->ready) {
      out = {static_cast<int>(std::ceil(state->sourceWidth)), static_cast<int>(std::ceil(state->sourceHeight))};
      return true;
    }
  }
  return false;
}
bool SvgRasterizer::dimensionsFromFile(FsFile& file, Dimensions& out) {
  std::unique_ptr<State> state(new (std::nothrow) State());
  if (!state) return false;
  std::unique_ptr<State::Parser> parser(new (std::nothrow) State::Parser(*state, true));
  if (!parser || !parser->file(file)) return false;
  out = {static_cast<int>(std::ceil(state->sourceWidth)), static_cast<int>(std::ceil(state->sourceHeight))};
  return true;
}
bool SvgRasterizer::load(FsFile& file, int outputWidth, int outputHeight) {
  state_.reset();
  if (!file || file.size() > MAX_SOURCE_BYTES || outputWidth <= 0 || outputHeight <= 0 ||
      outputWidth > MAX_OUTPUT_DIMENSION || outputHeight > MAX_OUTPUT_DIMENSION ||
      static_cast<size_t>(outputWidth) * outputHeight > 1024 * 1024)
    return false;
  std::unique_ptr<State> state(new (std::nothrow) State());
  if (!state) return false;
  state->width = outputWidth;
  state->height = outputHeight;
  // Count first, allocate exactly once, then fill. Ordinary small icons do not
  // pay the 24 KiB worst-case edge budget; each pass uses one bounded XML/path buffer.
  size_t count = 0;
  for (int pass = 0; pass < 2; ++pass) {
    state->resetGeometry();
    std::unique_ptr<State::Parser> parser(new (std::nothrow) State::Parser(*state, false));
    if (!parser || !parser->file(file)) {
      LOG_ERR("SVG", "Unsupported, malformed, or over-limit SVG");
      return false;
    }
    if (pass == 0) {
      count = state->edgeCount;
      state->capacity = count;
      if (count) {
        state->edges.reset(new (std::nothrow) Edge[count]);
        if (!state->edges) return false;
      }
    } else if (state->edgeCount != count)
      return false;
  }
  state_ = std::move(state);
  return true;
}
bool SvgRasterizer::render(RowSink sink, void* context) const {
  if (!state_ || !sink) return false;
  const State& s = *state_;
  size_t crossings = 0;
  for (size_t i = 0; i < s.shapeCount; ++i) crossings = std::max(crossings, static_cast<size_t>(s.shapes[i].count));
  std::unique_ptr<Crossing[]> hits(new (std::nothrow) Crossing[std::max<size_t>(1, crossings)]);
  // Seven bytes/output column: four subsamples, a 2-byte accumulator, and final gray.
  // This replaces an image-sized RGBA/grayscale surface; rows are immediately emitted.
  std::unique_ptr<uint16_t[]> sums(new (std::nothrow) uint16_t[s.width]);
  std::unique_ptr<uint8_t[]> row(new (std::nothrow) uint8_t[s.width * (SUBPIXELS + 1)]);
  if (!hits || !sums || !row) return false;
  auto* samples = row.get();
  auto* gray = samples + s.width * SUBPIXELS;
  for (int y = 0; y < s.height; ++y) {
    if (aborted()) return false;
    HalSystem::feedWatchdog();
    memset(sums.get(), 0, s.width * sizeof(uint16_t));
    for (int sub = 0; sub < SUBPIXELS; ++sub) {
      memset(samples, 255, s.width * SUBPIXELS);
      const float scanY = y + (sub + .5f) / SUBPIXELS;
      for (size_t i = 0; i < s.shapeCount; ++i) {
        const Shape& shape = s.shapes[i];
        if (shape.paint.none || shape.paint.hidden || !shape.paint.alpha) continue;
        size_t count = 0;
        for (size_t j = shape.first; j < static_cast<size_t>(shape.first) + shape.count; ++j) {
          const Edge& e = s.edges[j];
          if (scanY < std::min(e.a.y, e.b.y) || scanY >= std::max(e.a.y, e.b.y)) continue;
          hits[count++] = {e.a.x + (scanY - e.a.y) * (e.b.x - e.a.x) / (e.b.y - e.a.y), e.b.y > e.a.y ? 1 : -1};
        }
        std::sort(hits.get(), hits.get() + count, [](const Crossing& a, const Crossing& b) { return a.x < b.x; });
        int winding = 0;
        float previous = 0;
        for (size_t j = 0; j < count; ++j) {
          if (shape.paint.evenOdd ? (winding & 1) : winding != 0) {
            const float maxSample = static_cast<float>(s.width * SUBPIXELS);
            const int left =
                static_cast<int>(std::max(0.0f, std::min(maxSample, std::ceil(previous * SUBPIXELS - .5f))));
            const int right =
                static_cast<int>(std::max(0.0f, std::min(maxSample, std::ceil(hits[j].x * SUBPIXELS - .5f))));
            for (int x = left; x < right; ++x)
              samples[x] = static_cast<uint8_t>(
                  (samples[x] * (255 - shape.paint.alpha) + shape.paint.gray * shape.paint.alpha + 127) / 255);
          }
          winding += hits[j].winding;
          previous = hits[j].x;
        }
      }
      for (int x = 0; x < s.width; ++x)
        for (int q = 0; q < SUBPIXELS; ++q) sums[x] += samples[x * SUBPIXELS + q];
    }
    for (int x = 0; x < s.width; ++x) gray[x] = static_cast<uint8_t>((sums[x] + 8) / 16);
    if (!sink(context, y, gray, s.width)) return false;
  }
  return true;
}
