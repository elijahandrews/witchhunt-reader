#include "CssFontCatalog.h"

#include <HalStorage.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <utility>

namespace {
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
uint8_t generic(std::string_view name) {
  if (name == "serif") return wordTypography::Serif;
  if (name == "sans-serif") return wordTypography::SansSerif;
  if (name == "monospace") return wordTypography::Monospace;
  return wordTypography::Reader;
}
bool parseFamilies(std::string_view value, CssFontCatalog::FamilyStack& out, bool& oversized) {
  out = {};
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
    const uint8_t fallback = quoted ? wordTypography::Reader : generic(normalized);
    if (!quoted && (normalized == "inherit" || normalized == "initial" || normalized == "unset" ||
                    normalized == "revert" || normalized == "revert-layer" || normalized == "default"))
      return false;
    if (!out.fallback && fallback) out.fallback = fallback;
    if (normalized.size() > CssFontCatalog::MAX_NAME_BYTES ||
        out.families.size() >= CssFontCatalog::MAX_FAMILIES_PER_STACK) {
      oversized = true;
    } else
      out.families.push_back({std::move(normalized), fallback});
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
constexpr uint8_t VERSION = 1;
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
struct Reader {
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
    text += n;
    std::string value(n, '\0');
    if (n) bytes(&value[0], n);
    return value;
  }
};
}  // namespace

uint8_t CssFontCatalog::genericFallback(std::string_view value) {
  const std::string key = lower(trim(value));
  if (key == "inherit" || key == "unset") return wordTypography::Inherit;
  if (key == "initial") return wordTypography::Reader;
  FamilyStack families;
  bool oversized = false;
  if (!parseFamilies(value, families, oversized)) return INVALID_ID;
  return families.fallback ? families.fallback : INVALID_ID;
}
uint8_t CssFontCatalog::intern(std::string_view value, bool* added) {
  if (added) *added = false;
  const std::string key = lower(trim(value));
  if (key == "inherit" || key == "unset") return wordTypography::Inherit;
  if (key == "initial") return wordTypography::Reader;
  FamilyStack families;
  bool oversized = false;
  if (!parseFamilies(value, families, oversized)) return INVALID_ID;
  if (!families.families.empty() && families.families.front().generic) return families.families.front().generic;
  if (oversized) {
    truncated_ = true;
    return families.fallback;
  }
  for (size_t i = 0; i < stacks_.size(); ++i)
    if (stacks_[i].families == families.families) return static_cast<uint8_t>(FIRST_NAMED_ID + i);
  size_t bytes = 0;
  for (const auto& f : families.families) bytes += f.name.size();
  if (stacks_.size() >= MAX_STACKS || textBytes() + bytes > MAX_TEXT_BYTES) {
    truncated_ = true;
    return families.fallback;
  }
  stacks_.push_back(std::move(families));
  if (added) *added = true;
  return static_cast<uint8_t>(FIRST_NAMED_ID + stacks_.size() - 1);
}
uint8_t CssFontCatalog::scopeStack(const uint8_t id, const std::string& prefix, bool* added) {
  if (added) *added = false;
  const auto* original = stack(id);
  if (!original || prefix.empty()) return id;
  FamilyStack scoped = *original;
  bool changed = false;
  for (auto& family : scoped.families) {
    if (family.generic) continue;
    const auto name = prefix + family.name;
    for (const auto& face : faces_) {
      if (face.family == name) {
        family.name = name;
        changed = true;
        break;
      }
    }
  }
  if (!changed) return id;
  size_t bytes = 0;
  for (const auto& family : scoped.families) {
    if (family.name.size() > MAX_NAME_BYTES) {
      truncated_ = true;
      return scoped.fallback;
    }
    bytes += family.name.size();
  }
  for (size_t i = 0; i < stacks_.size(); ++i)
    if (stacks_[i].families == scoped.families) return static_cast<uint8_t>(FIRST_NAMED_ID + i);
  if (stacks_.size() >= MAX_STACKS || textBytes() + bytes > MAX_TEXT_BYTES) {
    truncated_ = true;
    return scoped.fallback;
  }
  stacks_.push_back(std::move(scoped));
  if (added) *added = true;
  return static_cast<uint8_t>(FIRST_NAMED_ID + stacks_.size() - 1);
}
const CssFontCatalog::FamilyStack* CssFontCatalog::stack(uint8_t id) const {
  return id >= FIRST_NAMED_ID && static_cast<size_t>(id - FIRST_NAMED_ID) < stacks_.size()
             ? &stacks_[id - FIRST_NAMED_ID]
             : nullptr;
}
uint8_t CssFontCatalog::fallbackFamily(uint8_t id) const {
  if (const auto* s = stack(id)) return s->fallback;
  return id < FIRST_NAMED_ID ? id : static_cast<uint8_t>(wordTypography::Reader);
}
void CssFontCatalog::discardLastStack() {
  if (!stacks_.empty()) stacks_.pop_back();
}
void CssFontCatalog::parseFaceDeclaration(Face& face, std::string_view declaration, std::string_view stylesheetDir) {
  const size_t colon = declaration.find(':');
  if (colon == std::string_view::npos) return;
  const std::string property = lower(trim(declaration.substr(0, colon)));
  const auto value = trim(declaration.substr(colon + 1));
  if (property == "font-family") {
    FamilyStack s;
    bool oversized = false;
    if (parseFamilies(value, s, oversized) && !oversized && s.families.size() == 1 && !s.families.front().generic)
      face.family = std::move(s.families.front().name);
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
    bytes += f.family.size();
    for (const auto& s : f.sources) bytes += s.path.size() + s.format.size();
  }
  return bytes;
}
void CssFontCatalog::addFace(Face face) {
  truncated_ = truncated_ || face.truncated;
  if (face.family.empty() || face.sources.empty()) return;
  if (face.family.size() > MAX_NAME_BYTES) {
    truncated_ = true;
    return;
  }
  for (const auto& f : faces_)
    if (f.family == face.family && f.style == face.style && f.weight == face.weight && f.sources == face.sources)
      return;
  size_t bytes = face.family.size();
  for (const auto& s : face.sources) bytes += s.path.size() + s.format.size();
  if (faces_.size() >= MAX_FACES || textBytes() + bytes > MAX_TEXT_BYTES) {
    truncated_ = true;
    return;
  }
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
bool CssFontCatalog::save(const std::string& directory) const {
  if (directory.empty() || generation_ == std::numeric_limits<uint32_t>::max()) return false;
  const uint8_t target = slot_ ^ 1;
  const auto path = catalogFile(directory, target), temp = path + ".tmp";
  if (Storage.exists(temp.c_str())) Storage.remove(temp.c_str());
  FsFile file;
  if (!Storage.openFileForWrite("CSS", temp, file)) return false;
  Writer out{file};
  out.bytes("CFNT", 4);
  out.u8(VERSION);
  out.u32(generation_ + 1);
  out.u8(truncated_ ? 1 : 0);
  out.u16(stacks_.size());
  out.u16(faces_.size());
  for (const auto& s : stacks_) {
    out.u8(s.fallback);
    out.u8(s.families.size());
    for (const auto& f : s.families) {
      out.u8(f.generic);
      out.str(f.name);
    }
  }
  for (const auto& f : faces_) {
    out.str(f.family);
    out.u8(static_cast<uint8_t>(f.style));
    out.u16(f.weight);
    out.u8(f.sources.size());
    for (const auto& s : f.sources) {
      out.str(s.path);
      out.str(s.format);
    }
  }
  const uint32_t checksum = out.hash;
  out.u32(checksum);
  file.flush();
  file.close();
  if (!out.ok || (Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) ||
      !Storage.rename(temp.c_str(), path.c_str())) {
    if (Storage.exists(temp.c_str())) Storage.remove(temp.c_str());
    return false;
  }
  generation_ += 1;
  slot_ = target;
  return true;
}
bool CssFontCatalog::load(const std::string& directory) {
  reset();
  if (directory.empty()) return false;
  bool loaded = false;
  for (uint8_t slot = 0; slot < 2; ++slot) {
    const auto path = catalogFile(directory, slot);
    if (!Storage.exists(path.c_str())) continue;
    FsFile file;
    if (!Storage.openFileForRead("CSS", path, file)) continue;
    Reader in{file};
    char magic[4]{};
    in.bytes(magic, 4);
    if (!in.ok || std::memcmp(magic, "CFNT", 4) != 0 || in.u8() != VERSION) continue;
    const uint32_t generation = in.u32();
    const uint8_t flags = in.u8();
    const uint16_t stacks = in.u16(), faces = in.u16();
    if (!in.ok || !generation || flags > 1 || stacks > MAX_STACKS || faces > MAX_FACES || generation <= generation_)
      continue;
    CssFontCatalog candidate;
    candidate.stacks_.reserve(stacks);
    candidate.faces_.reserve(faces);
    for (size_t i = 0; in.ok && i < stacks; ++i) {
      FamilyStack s;
      s.fallback = in.u8();
      const uint8_t count = in.u8();
      if ((s.fallback != 0 && s.fallback != 1 && s.fallback != 2 && s.fallback != 4) || !count ||
          count > MAX_FAMILIES_PER_STACK) {
        in.ok = false;
        break;
      }
      for (size_t j = 0; in.ok && j < count; ++j) {
        FamilyName f;
        f.generic = in.u8();
        f.name = in.str(MAX_NAME_BYTES);
        if (f.name.empty() || (f.generic && generic(f.name) != f.generic)) in.ok = false;
        s.families.push_back(std::move(f));
      }
      candidate.stacks_.push_back(std::move(s));
    }
    for (size_t i = 0; in.ok && i < faces; ++i) {
      Face f;
      f.family = in.str(MAX_NAME_BYTES);
      const uint8_t style = in.u8();
      f.style = static_cast<Style>(style);
      f.weight = in.u16();
      const uint8_t count = in.u8();
      if (f.family.empty() || style > 2 || f.weight < 1 || f.weight > 1000 || !count || count > MAX_SOURCES_PER_FACE) {
        in.ok = false;
        break;
      }
      for (size_t j = 0; in.ok && j < count; ++j) {
        Source s;
        s.path = in.str(MAX_PATH_BYTES);
        s.format = in.str(MAX_NAME_BYTES);
        if (s.path.empty() || s.path.front() == '/' || s.path.find_first_of(":\\\\") != std::string::npos ||
            s.path.find('\0') != std::string::npos || s.path == ".." || s.path.rfind("../", 0) == 0 ||
            s.path.find("/../") != std::string::npos ||
            (s.path.size() >= 3 && s.path.compare(s.path.size() - 3, 3, "/..") == 0))
          in.ok = false;
        f.sources.push_back(std::move(s));
      }
      candidate.faces_.push_back(std::move(f));
    }
    const uint32_t checksum = in.hash, stored = in.u32();
    if (!in.ok || checksum != stored || file.available()) continue;
    stacks_ = std::move(candidate.stacks_);
    faces_ = std::move(candidate.faces_);
    truncated_ = flags != 0;
    generation_ = generation;
    slot_ = slot;
    loaded = true;
  }
  return loaded;
}
