#include "CssFontCatalog.h"

#include <HalStorage.h>
#include <Logging.h>
#include <MemoryBudget.h>
#include <PoolBudget.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <utility>

namespace {
bool catalogRoom(size_t bytes);
bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f'; }
std::string_view trim(std::string_view v) {
  while (!v.empty() && space(v.front())) v.remove_prefix(1);
  while (!v.empty() && space(v.back())) v.remove_suffix(1);
  return v;
}
std::string lower(std::string_view v) {
  std::string s(v);
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
int hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
void utf8(std::string& s, uint32_t c) {
  if (!c || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) c = 0xfffd;
  if (c < 0x80)
    s += static_cast<char>(c);
  else if (c < 0x800) {
    s += static_cast<char>(0xc0 | (c >> 6));
    s += static_cast<char>(0x80 | (c & 63));
  } else if (c < 0x10000) {
    s += static_cast<char>(0xe0 | (c >> 12));
    s += static_cast<char>(0x80 | ((c >> 6) & 63));
    s += static_cast<char>(0x80 | (c & 63));
  } else {
    s += static_cast<char>(0xf0 | (c >> 18));
    s += static_cast<char>(0x80 | ((c >> 12) & 63));
    s += static_cast<char>(0x80 | ((c >> 6) & 63));
    s += static_cast<char>(0x80 | (c & 63));
  }
}
// CSS strings and identifiers share escape syntax. Escaped newlines in strings disappear.
bool unescape(std::string_view in, std::string& out) {
  out.clear();
  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (c != '\\') {
      if (!c) return false;
      out += c;
      continue;
    }
    if (++i == in.size()) return false;
    c = in[i];
    if (c == '\n' || c == '\f') continue;
    if (c == '\r') {
      if (i + 1 < in.size() && in[i + 1] == '\n') ++i;
      continue;
    }
    if (hex(c) < 0) {
      out += c;
      continue;
    }
    uint32_t cp = 0;
    size_t n = 0;
    while (i < in.size() && n < 6 && hex(in[i]) >= 0) {
      cp = cp * 16 + hex(in[i++]);
      ++n;
    }
    if (i < in.size() && space(in[i])) {
      if (in[i] == '\r' && i + 1 < in.size() && in[i + 1] == '\n') ++i;
    } else
      --i;
    utf8(out, cp);
  }
  return true;
}
// Split only outside strings and functions. Unlike find(','), this preserves quoted family
// names and punctuation inside url()/format(). A false return rejects incomplete CSS tokens.
template <class Fn>
bool split(std::string_view value, char delimiter, Fn fn) {
  char quote = 0;
  unsigned depth = 0;
  size_t start = 0;
  for (size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if (c == '\\') {
      if (++i == value.size()) return false;
      continue;
    }
    if (quote) {
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
      continue;
    }
    if (c == '(') {
      ++depth;
      continue;
    }
    if (c == ')') {
      if (!depth) return false;
      --depth;
      continue;
    }
    if (c == delimiter && !depth) {
      if (!fn(trim(value.substr(start, i - start)))) return false;
      start = i + 1;
    }
  }
  return !quote && !depth && fn(trim(value.substr(start)));
}
uint32_t generic(std::string_view name) {
  if (name == "serif") return genericFont::Serif;
  if (name == "sans-serif" || name == "system-ui") return genericFont::SansSerif;
  if (name == "monospace") return genericFont::Monospace;
  return CssFontCatalog::Reader;
}
bool parseFamilies(std::string_view value, CssFontCatalog::FamilyStack& out, bool& oversized) {
  out = {};
  out.families.reserve(CssFontCatalog::MAX_FAMILIES_PER_STACK);
  return split(trim(value), ',', [&](std::string_view name) {
    if (name.empty()) return false;
    bool quoted = name.front() == '\'' || name.front() == '"';
    if (quoted) {
      if (name.size() < 2 || name.back() != name.front()) return false;
      const char quote = name.front();
      name = name.substr(1, name.size() - 2);
      for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '\\') {
          if (++i == name.size()) return false;
        } else if (name[i] == quote)
          return false;
      }
    } else {
      // Only CSS identifiers/whitespace belong in an unquoted family name.
      for (size_t i = 0; i < name.size(); ++i) {
        const auto c = static_cast<unsigned char>(name[i]);
        if (c == '\\') {
          if (++i == name.size()) return false;
          continue;
        }
        if (!(c >= 0x80 || std::isalnum(c) || c == '-' || c == '_' || space(c))) return false;
      }
    }
    std::string decoded;
    if (!unescape(name, decoded)) return false;
    std::string normalized;
    bool wasSpace = false;
    for (char c : decoded) {
      if (space(c)) {
        wasSpace = !normalized.empty();
        continue;
      }
      if (wasSpace) normalized += ' ';
      normalized += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      wasSpace = false;
    }
    if (normalized.empty()) return false;
    if (!quoted && std::isdigit(static_cast<unsigned char>(normalized.front()))) return false;
    const uint32_t fallback = quoted ? CssFontCatalog::Reader : generic(normalized);
    if (!quoted && (normalized == "inherit" || normalized == "initial" || normalized == "unset" ||
                    normalized == "revert" || normalized == "revert-layer" || normalized == "default"))
      return false;
    if (!out.fallback && fallback) out.fallback = fallback;
    if (normalized.size() > CssFontCatalog::MAX_NAME_BYTES ||
        out.families.size() >= CssFontCatalog::MAX_FAMILIES_PER_STACK) {
      oversized = true;
    } else
      out.families.push_back({std::move(normalized), 0, fallback});
    return true;
  });
}
std::string localPath(std::string_view dir, std::string_view raw) {
  raw = trim(raw);
  if (raw.size() >= 2 && (raw.front() == '\'' || raw.front() == '"') && raw.back() == raw.front())
    raw = raw.substr(1, raw.size() - 2);
  std::string path;
  if (!unescape(raw, path) || path.empty()) return {};
  // URLs are references to archive entries. Query/fragment select no separate font resource.
  const size_t suffix = path.find_first_of("?#");
  if (suffix != std::string::npos) path.resize(suffix);
  std::string decoded;
  for (size_t i = 0; i < path.size(); ++i) {
    unsigned char c = path[i];
    if (c == '%' && i + 2 < path.size() && hex(path[i + 1]) >= 0 && hex(path[i + 2]) >= 0) {
      c = static_cast<unsigned char>(hex(path[i + 1]) * 16 + hex(path[i + 2]));
      i += 2;
    }
    if (!c || c == '\\' || c == ':' || c < 32) return {};
    decoded += static_cast<char>(c);
  }
  if (decoded.empty() || decoded.rfind("//", 0) == 0) return {};
  path = decoded.front() == '/' ? decoded.substr(1) : std::string(dir) + decoded;
  std::vector<std::string_view> parts;
  parts.reserve(std::min<size_t>(path.size() / 2 + 1, 256));
  size_t start = 0;
  for (size_t i = 0; i <= path.size(); ++i) {
    if (i != path.size() && path[i] != '/') continue;
    const auto p = std::string_view(path).substr(start, i - start);
    start = i + 1;
    if (p.empty() || p == ".") continue;
    if (p == "..") {
      if (parts.empty()) return {};
      parts.pop_back();
    } else
      parts.push_back(p);
  }
  decoded.clear();
  for (const auto p : parts) {
    if (!decoded.empty()) decoded += '/';
    decoded.append(p);
  }
  return decoded.size() <= CssFontCatalog::MAX_PATH_BYTES ? decoded : std::string();
}
bool function(std::string_view value, size_t& pos, std::string& name, std::string_view& args) {
  while (pos < value.size() && space(value[pos])) ++pos;
  const size_t start = pos;
  while (pos < value.size() && (std::isalpha(static_cast<unsigned char>(value[pos])) || value[pos] == '-')) ++pos;
  name = lower(value.substr(start, pos - start));
  if (name.empty() || pos == value.size() || value[pos++] != '(') return false;
  const size_t body = pos;
  char quote = 0;
  unsigned depth = 1;
  for (; pos < value.size(); ++pos) {
    const char c = value[pos];
    if (c == '\\') {
      if (++pos == value.size()) return false;
      continue;
    }
    if (quote) {
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
      continue;
    }
    if (c == '(')
      ++depth;
    else if (c == ')' && --depth == 0) {
      args = value.substr(body, pos - body);
      ++pos;
      return true;
    }
  }
  return false;
}
constexpr uint8_t VERSION = 2;
std::string catalogFile(const std::string& dir, unsigned slot) {
  return dir + (slot ? "/css_fonts.1.bin" : "/css_fonts.0.bin");
}
// Streaming checked I/O keeps saving and reopening bounded by the catalog, not a second copy
// of its serialized bytes. The hash detects torn/truncated writes before IDs become visible.
struct Writer {
  FsFile& file;
  bool ok = true;
  uint32_t hash = 2166136261u;
  void bytes(const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    if (file.write(p, n) != n) ok = false;
    for (size_t i = 0; i < n; ++i) hash = (hash ^ p[i]) * 16777619u;
  }
  void u8(uint8_t v) { bytes(&v, 1); }
  void u16(uint16_t v) {
    uint8_t b[] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
    bytes(b, 2);
  }
  void u32(uint32_t v) {
    u16(v);
    u16(v >> 16);
  }
  void str(const std::string& v) {
    u16(v.size());
    bytes(v.data(), v.size());
  }
};
struct CatalogReader {
  FsFile& file;
  bool ok = true;
  uint32_t hash = 2166136261u;
  size_t text = 0;
  void bytes(void* data, size_t n) {
    auto* p = static_cast<uint8_t*>(data);
    if (!ok || file.read(p, n) != static_cast<int>(n)) {
      ok = false;
      return;
    }
    for (size_t i = 0; i < n; ++i) hash = (hash ^ p[i]) * 16777619u;
  }
  uint8_t u8() {
    uint8_t v = 0;
    bytes(&v, 1);
    return v;
  }
  uint16_t u16() {
    const uint16_t lo = u8();
    return lo | (static_cast<uint16_t>(u8()) << 8);
  }
  uint32_t u32() {
    const uint32_t lo = u16();
    return lo | (static_cast<uint32_t>(u16()) << 16);
  }
  std::string str(size_t max) {
    const size_t n = u16();
    if (!ok || n > max || text + n > CssFontCatalog::MAX_TEXT_BYTES) {
      ok = false;
      return {};
    }
    if (n && !catalogRoom(n + 33)) {
      ok = false;
      return {};
    }
    text += n;
    std::string value(n, '\0');
    if (n) bytes(&value[0], n);
    return value;
  }
};
}  // namespace

uint32_t CssFontCatalog::namedToken(std::string_view name) {
  uint32_t hash = 2166136261u;
  for (const unsigned char c : name) hash = (hash ^ static_cast<uint8_t>(std::tolower(c))) * 16777619u;
  if (genericFont::isGeneric(hash) || hash == Inherit || hash == INVALID_ID) hash ^= 0x01000000u;
  return hash ? hash : 1;
}
uint32_t CssFontCatalog::stackToken(const FamilyStack& stack) {
  if (stack.families.empty()) return Reader;
  if (stack.families.front().generic) return stack.families.front().generic;
  if (stack.families.size() == 1) return namedToken(stack.families.front().name);
  uint32_t hash = 2166136261u;
  const auto byte = [&](uint8_t b) { hash = (hash ^ b) * 16777619u; };
  for (const char c : std::string_view("stack:")) byte(c);
  for (const auto& family : stack.families) {
    byte(family.generic ? 1 : 0);
    for (const unsigned char c : family.name) byte(c);
    byte(0);
  }
  if (genericFont::isGeneric(hash) || hash == Inherit || hash == INVALID_ID) hash ^= 0x01000000u;
  return hash ? hash : 1;
}
namespace {
bool catalogRoom(size_t bytes = 8192) {
  if (MemoryBudget::canAllocateInternal(bytes, 64 * 1024, 8 * 1024)) return true;
  LOG_ERR("CSS", "Insufficient memory for bounded font catalog (%zu bytes)", bytes);
  return false;
}
}  // namespace
uint32_t CssFontCatalog::genericFallback(std::string_view value) {
  if (value.size() > 4096 || !catalogRoom()) return Reader;
  const std::string key = lower(trim(value));
  if (key == "inherit" || key == "unset") return Inherit;
  if (key == "initial") return Reader;
  FamilyStack families;
  bool oversized = false;
  if (!parseFamilies(value, families, oversized)) return INVALID_ID;
  return families.fallback ? families.fallback : INVALID_ID;
}
bool CssFontCatalog::admitStack(FamilyStack&& families, bool* added, Token& result) {
  families.token = stackToken(families);
  for (auto& member : families.families) member.token = member.generic ? member.generic : namedToken(member.name);
  result = families.token;
  if (families.families.front().generic) return true;
  for (const auto& existing : stacks_) {
    if (existing.families == families.families) {
      result = existing.token;
      return true;
    }
    if (existing.token == families.token) {
      truncated_ = true;
      result = families.fallback;
      return false;
    }
  }
  size_t bytes = 0;
  for (const auto& family : families.families) bytes += family.name.size();
  if (stacks_.size() >= MAX_STACKS || textBytes() + bytes > MAX_TEXT_BYTES ||
      !catalogRoom(bytes + sizeof(FamilyStack) * (stacks_.size() + 1))) {
    truncated_ = true;
    result = families.fallback;
    return false;
  }
  // Cold catalog growth is bounded and preflighted; no allocations occur in glyph lookup.
  stacks_.reserve(stacks_.size() + 1);
  stacks_.push_back(std::move(families));
  if (added) *added = true;
  return true;
}
uint32_t CssFontCatalog::intern(std::string_view value, bool* added) {
  if (added) *added = false;
  if (value.size() > 4096 || !catalogRoom()) {
    truncated_ = true;
    return Reader;
  }
  const std::string key = lower(trim(value));
  if (key == "inherit" || key == "unset") return Inherit;
  if (key == "initial") return Reader;
  FamilyStack families;
  bool oversized = false;
  if (!parseFamilies(value, families, oversized)) return INVALID_ID;
  if (oversized) {
    truncated_ = true;
    return families.fallback;
  }
  Token result = families.fallback;
  admitStack(std::move(families), added, result);
  return result;
}
uint32_t CssFontCatalog::scopeStack(Token id, const std::string& prefix, bool* added) {
  if (added) *added = false;
  const auto* original = stack(id);
  if (!original || prefix.empty()) return id;
  if (!catalogRoom()) {
    truncated_ = true;
    return original->fallback;
  }
  FamilyStack scoped = *original;
  bool changed = false;
  for (auto& family : scoped.families) {
    if (family.generic) continue;
    const auto name = prefix + family.name;
    for (const auto& face : faces_)
      if (face.familyName == name) {
        if (name.size() > MAX_NAME_BYTES) {
          truncated_ = true;
          return scoped.fallback;
        }
        family.name = name;
        changed = true;
        break;
      }
  }
  if (!changed) return id;
  Token result = scoped.fallback;
  admitStack(std::move(scoped), added, result);
  return result;
}
const CssFontCatalog::FamilyStack* CssFontCatalog::stack(Token id) const {
  for (const auto& stack : stacks_)
    if (stack.token == id) return &stack;
  return nullptr;
}
uint32_t CssFontCatalog::fallbackFamily(Token id) const {
  if (const auto* value = stack(id)) return value->fallback;
  return genericFont::isGeneric(id) ? id : Reader;
}
void CssFontCatalog::discardLastStack() {
  if (!stacks_.empty()) stacks_.pop_back();
}
void CssFontCatalog::parseFaceDeclaration(Face& face, std::string_view declaration, std::string_view stylesheetDir) {
  if (declaration.size() > 4096 || !catalogRoom()) {
    face.truncated = true;
    return;
  }
  const size_t colon = declaration.find(':');
  if (colon == std::string_view::npos) return;
  const std::string property = lower(trim(declaration.substr(0, colon)));
  const auto value = trim(declaration.substr(colon + 1));
  if (property == "font-family") {
    FamilyStack s;
    bool oversized = false;
    if (parseFamilies(value, s, oversized) && !oversized && s.families.size() == 1 && !s.families.front().generic)
      face.familyName = std::move(s.families.front().name);
  } else if (property == "font-style") {
    const auto v = lower(value);
    if (v == "normal")
      face.style = Style::Normal;
    else if (v == "italic")
      face.style = Style::Italic;
    else if (v == "oblique")
      face.style = Style::Oblique;
  } else if (property == "font-weight") {
    const auto v = lower(value);
    if (v == "normal")
      face.weight = 400;
    else if (v == "bold")
      face.weight = 700;
    else {
      unsigned weight = 0;
      bool valid = !v.empty() && v.size() <= 4;
      for (char c : v) {
        if (c < '0' || c > '9') {
          valid = false;
          break;
        }
        weight = weight * 10 + c - '0';
      }
      if (valid && weight >= 1 && weight <= 1000) face.weight = weight;
    }
  } else if (property == "src") {
    std::vector<Source> sources;
    sources.reserve(MAX_SOURCES_PER_FACE);
    const bool valid = split(value, ',', [&](std::string_view candidate) {
      size_t pos = 0;
      std::string name;
      std::string_view arg;
      if (!function(candidate, pos, name, arg)) return false;
      Source source;
      if (name == "url")
        source.path = localPath(stylesheetDir, arg);
      else if (name != "local")
        return false;
      while (pos < candidate.size()) {
        if (trim(candidate.substr(pos)).empty()) break;
        if (!function(candidate, pos, name, arg)) return false;
        if (name == "format") {
          arg = trim(arg);
          if (arg.size() >= 2 && (arg.front() == '\'' || arg.front() == '"') && arg.back() == arg.front())
            arg = arg.substr(1, arg.size() - 2);
          source.format = lower(arg);
          if (source.format.size() > MAX_NAME_BYTES) source.path.clear();
        } else if (name != "tech")
          return false;
      }
      if (!source.path.empty()) {
        if (sources.size() < MAX_SOURCES_PER_FACE)
          sources.push_back(std::move(source));
        else
          face.truncated = true;
      }
      return true;
    });
    if (valid) face.sources = std::move(sources);
  }
}
size_t CssFontCatalog::textBytes() const {
  size_t bytes = 0;
  for (const auto& s : stacks_)
    for (const auto& f : s.families) bytes += f.name.size();
  for (const auto& f : faces_) {
    bytes += f.familyName.size();
    for (const auto& s : f.sources) bytes += s.path.size() + s.format.size();
  }
  return bytes;
}
void CssFontCatalog::addFace(Face face) {
  truncated_ = truncated_ || face.truncated;
  if (face.familyName.empty() || face.sources.empty()) return;
  if (face.familyName.size() > MAX_NAME_BYTES) {
    truncated_ = true;
    return;
  }
  // Later equal declarations retain their source order for descriptor tie breaking.
  face.family = namedToken(face.familyName);
  face.declarationOrder = faces_.size() + 1;
  size_t bytes = face.familyName.size();
  for (const auto& s : face.sources) bytes += s.path.size() + s.format.size();
  if (faces_.size() >= MAX_FACES || textBytes() + bytes > MAX_TEXT_BYTES ||
      !catalogRoom(bytes + sizeof(Face) * (faces_.size() + 1))) {
    truncated_ = true;
    return;
  }
  faces_.reserve(faces_.size() + 1);
  faces_.push_back(std::move(face));
  ++faceRevision_;
}
void CssFontCatalog::reset() {
  ++faceRevision_;
  std::vector<FamilyStack>().swap(stacks_);
  std::vector<Face>().swap(faces_);
  truncated_ = false;
  generation_ = 0;
  slot_ = 1;
}
void CssFontCatalog::removeFiles(const std::string& directory) {
  if (directory.empty()) return;
  for (unsigned slot = 0; slot < 2; ++slot) {
    const auto path = catalogFile(directory, slot);
    if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
    if (Storage.exists((path + ".tmp").c_str())) Storage.remove((path + ".tmp").c_str());
  }
}
bool CssFontCatalog::write(HalFile& file) const {
  Writer out{file};
  out.bytes("CFNT", 4);
  out.u8(VERSION);
  out.u32(generation_ + 1);
  out.u8(truncated_ ? 1 : 0);
  out.u16(stacks_.size());
  out.u16(faces_.size());
  for (const auto& stack : stacks_) {
    out.u32(stack.token);
    out.u32(stack.fallback);
    out.u8(stack.families.size());
    for (const auto& family : stack.families) {
      out.u32(family.generic);
      out.str(family.name);
    }
  }
  for (const auto& face : faces_) {
    out.u32(face.family);
    out.str(face.familyName);
    out.u8(static_cast<uint8_t>(face.style));
    out.u16(face.weight);
    out.u32(face.declarationOrder);
    out.u8(face.sources.size());
    for (const auto& source : face.sources) {
      out.str(source.path);
      out.str(source.format);
    }
  }
  const uint32_t checksum = out.hash;
  out.u32(checksum);
  return out.ok;
}
bool CssFontCatalog::read(HalFile& file) {
  const auto start = file.position();
  size_t hydration = 0;
  if (!inspect(file, &hydration) || !file.seek(start) || (hydration && !catalogRoom(hydration + residentBytes())))
    return false;
  CatalogReader in{file};
  char magic[4]{};
  in.bytes(magic, 4);
  if (!in.ok || std::memcmp(magic, "CFNT", 4) || in.u8() != VERSION) return false;
  const auto generation = in.u32();
  const auto flags = in.u8();
  const auto stacks = in.u16(), faces = in.u16();
  if (!in.ok || !generation || flags > 1 || stacks > MAX_STACKS || faces > MAX_FACES) return false;
  CssFontCatalog candidate;
  candidate.stacks_.reserve(stacks);
  candidate.faces_.reserve(faces);
  for (size_t i = 0; in.ok && i < stacks; ++i) {
    FamilyStack stack;
    stack.token = in.u32();
    stack.fallback = in.u32();
    const auto count = in.u8();
    if ((stack.fallback && !genericFont::isGeneric(stack.fallback)) || !count || count > MAX_FAMILIES_PER_STACK)
      return false;
    if (!catalogRoom(count * sizeof(FamilyName) + 32)) return false;
    stack.families.reserve(count);
    for (size_t j = 0; in.ok && j < count; ++j) {
      FamilyName member;
      member.generic = in.u32();
      member.name = in.str(MAX_NAME_BYTES);
      if (member.name.empty() || member.name.find('\0') != std::string::npos ||
          (member.generic && generic(member.name) != member.generic))
        return false;
      member.token = member.generic ? member.generic : namedToken(member.name);
      stack.families.push_back(std::move(member));
    }
    if (stackToken(stack) != stack.token || candidate.stack(stack.token)) return false;
    candidate.stacks_.push_back(std::move(stack));
  }
  for (size_t i = 0; in.ok && i < faces; ++i) {
    Face face;
    face.family = in.u32();
    face.familyName = in.str(MAX_NAME_BYTES);
    const auto style = in.u8();
    face.style = static_cast<Style>(style);
    face.weight = in.u16();
    face.declarationOrder = in.u32();
    const auto count = in.u8();
    if (face.familyName.empty() || namedToken(face.familyName) != face.family || style > 2 || face.weight < 1 ||
        face.weight > 1000 || face.declarationOrder != i + 1 || !count || count > MAX_SOURCES_PER_FACE)
      return false;
    if (!catalogRoom(count * sizeof(Source) + 32)) return false;
    face.sources.reserve(count);
    for (size_t j = 0; in.ok && j < count; ++j) {
      Source source;
      source.path = in.str(MAX_PATH_BYTES);
      source.format = in.str(MAX_NAME_BYTES);
      if (source.path.empty() || !catalogRoom(8192) || source.path != localPath("", source.path)) return false;
      face.sources.push_back(std::move(source));
    }
    candidate.faces_.push_back(std::move(face));
  }
  const auto checksum = in.hash, stored = in.u32();
  if (!in.ok || checksum != stored) return false;
  stacks_ = std::move(candidate.stacks_);
  faces_ = std::move(candidate.faces_);
  truncated_ = flags;
  generation_ = generation;
  ++faceRevision_;
  return true;
}
uint32_t CssFontCatalog::signature() const {
  uint32_t h = 2166136261u;
  const auto byte = [&](uint8_t b) { h = (h ^ b) * 16777619u; };
  const auto u32 = [&](uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) byte(v >> (8 * i));
  };
  const auto str = [&](const std::string& v) {
    u32(v.size());
    for (unsigned char c : v) byte(c);
  };
  u32(truncated_);
  u32(stacks_.size());
  u32(faces_.size());
  for (const auto& stack : stacks_) {
    u32(stack.token);
    u32(stack.fallback);
    u32(stack.families.size());
    for (const auto& f : stack.families) {
      u32(f.generic);
      str(f.name);
    }
  }
  for (const auto& face : faces_) {
    u32(face.family);
    str(face.familyName);
    u32(static_cast<uint8_t>(face.style));
    u32(face.weight);
    u32(face.declarationOrder);
    u32(face.sources.size());
    for (const auto& source : face.sources) {
      str(source.path);
      str(source.format);
    }
  }
  return h;
}
bool CssFontCatalog::save(const std::string& directory) const {
  if (directory.empty() || generation_ == UINT32_MAX) return false;
  const uint8_t target = slot_ ^ 1;
  const auto path = catalogFile(directory, target), temp = path + ".tmp";
  if (Storage.exists(temp.c_str()) && !Storage.remove(temp.c_str())) return false;
  HalFile file;
  if (!Storage.openFileForWrite("CSS", temp, file)) return false;
  bool ok = write(file);
  ok = file.sync() && ok;
  ok = file.close() && ok;
  if (!ok || (Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) ||
      !Storage.rename(temp.c_str(), path.c_str())) {
    Storage.remove(temp.c_str());
    return false;
  }
  ++generation_;
  slot_ = target;
  return true;
}
bool CssFontCatalog::load(const std::string& directory, size_t retainedBytes) {
  reset();
  if (directory.empty()) return false;
  bool loaded = false;
  for (uint8_t slot = 0; slot < 2; ++slot) {
    HalFile file;
    if (!Storage.openFileForRead("CSS", catalogFile(directory, slot), file)) continue;
    size_t hydration = 0;
    const bool admitted = inspect(file, &hydration) && file.seek(0) &&
                          (!hydration || catalogRoom(hydration + residentBytes() + retainedBytes));
    CssFontCatalog candidate;
    const bool valid = admitted && candidate.read(file) && file.available() == 0;
    file.close();
    if (!valid || candidate.generation_ <= generation_) continue;
    stacks_ = std::move(candidate.stacks_);
    faces_ = std::move(candidate.faces_);
    truncated_ = candidate.truncated_;
    generation_ = candidate.generation_;
    slot_ = slot;
    loaded = true;
  }
  return loaded;
}

bool CssFontCatalog::isExtensionOf(const CssFontCatalog& previous) const {
  if (stacks_.size() < previous.stacks_.size() || faces_.size() < previous.faces_.size()) return false;
  for (size_t i = 0; i < previous.stacks_.size(); ++i)
    if (stacks_[i].token != previous.stacks_[i].token || stacks_[i].families != previous.stacks_[i].families)
      return false;
  for (size_t i = 0; i < previous.faces_.size(); ++i) {
    const auto& a = faces_[i];
    const auto& b = previous.faces_[i];
    if (a.family != b.family || a.familyName != b.familyName || a.style != b.style || a.weight != b.weight ||
        a.sources != b.sources || a.declarationOrder != b.declarationOrder)
      return false;
  }
  return true;
}
bool CssFontCatalog::inspect(HalFile& file, size_t* hydrationBytes) {
  if (hydrationBytes) *hydrationBytes = 0;
  CatalogReader in{file};
  char magic[4]{};
  in.bytes(magic, 4);
  if (!in.ok || std::memcmp(magic, "CFNT", 4) || in.u8() != VERSION) return false;
  if (!in.u32() || in.u8() > 1) return false;
  const auto stacks = in.u16(), faces = in.u16();
  if (stacks > MAX_STACKS || faces > MAX_FACES) return false;
  size_t hydration = stacks * sizeof(FamilyStack) + faces * sizeof(Face);
  if (hydration) hydration += 8192 + 64;  // bounded path-normalization scratch and container allocation headers
  const auto string = [&](size_t maximum, bool required) {
    size_t n = in.u16();
    if (!in.ok || n > maximum || (required && !n) || in.text + n > MAX_TEXT_BYTES) {
      in.ok = false;
      return;
    }
    in.text += n;
    hydration += n + 33;  // capacity terminator and conservative allocator overhead, including SSO values
    uint8_t bytes[128];
    while (n && in.ok) {
      size_t count = std::min(n, sizeof(bytes));
      in.bytes(bytes, count);
      if (in.ok && std::memchr(bytes, 0, count)) in.ok = false;
      n -= count;
    }
  };
  for (size_t i = 0; in.ok && i < stacks; ++i) {
    auto token = in.u32(), fallback = in.u32();
    auto n = in.u8();
    if (!token || token == INVALID_ID || token == Inherit || (fallback && !genericFont::isGeneric(fallback)) || !n ||
        n > MAX_FAMILIES_PER_STACK)
      return false;
    hydration += n * sizeof(FamilyName) + 32;
    for (size_t j = 0; in.ok && j < n; ++j) {
      auto generic = in.u32();
      if (generic && !genericFont::isGeneric(generic)) return false;
      string(MAX_NAME_BYTES, true);
    }
  }
  for (size_t i = 0; in.ok && i < faces; ++i) {
    if (!in.u32()) return false;
    string(MAX_NAME_BYTES, true);
    auto style = in.u8();
    auto weight = in.u16();
    auto order = in.u32();
    auto n = in.u8();
    if (style > 2 || !weight || weight > 1000 || order != i + 1 || !n || n > MAX_SOURCES_PER_FACE) return false;
    hydration += n * sizeof(Source) + 32;
    for (size_t j = 0; in.ok && j < n; ++j) {
      string(MAX_PATH_BYTES, true);
      string(MAX_NAME_BYTES, false);
    }
  }
  const auto checksum = in.hash, stored = in.u32();
  const bool valid = in.ok && checksum == stored;
  if (valid && hydrationBytes) *hydrationBytes = hydration;
  return valid;
}
size_t CssFontCatalog::residentBytes() const {
  size_t bytes = stacks_.capacity() * sizeof(FamilyStack) + faces_.capacity() * sizeof(Face);
  for (const auto& stack : stacks_) {
    bytes += stack.families.capacity() * sizeof(FamilyName) + 32;
    for (const auto& family : stack.families) bytes += family.name.capacity() + 33;
  }
  for (const auto& face : faces_) {
    bytes += face.sources.capacity() * sizeof(Source) + face.familyName.capacity() + 65;
    for (const auto& source : face.sources) bytes += source.path.capacity() + source.format.capacity() + 66;
  }
  return bytes;
}

std::string CssFontCatalog::resolvePath(std::string_view directory, std::string_view value) {
  if (value.size() > MAX_PATH_BYTES * 2 || directory.size() > MAX_PATH_BYTES || !catalogRoom()) return {};
  return localPath(directory, value);
}
