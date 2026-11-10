#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace cssMedia {
inline bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f'; }
inline std::string_view trim(std::string_view value) {
  while (!value.empty() && space(value.front())) value.remove_prefix(1);
  while (!value.empty() && space(value.back())) value.remove_suffix(1);
  return value;
}
inline bool equal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != b[i]) return false;
  return true;
}
inline std::string_view token(std::string_view& value) {
  value = trim(value);
  size_t end = 0;
  while (end < value.size() && !space(value[end]) && value[end] != '(') ++end;
  const auto result = value.substr(0, end);
  value.remove_prefix(end);
  value = trim(value);
  return result;
}

// Reject only a media TYPE that cannot apply to this screen reader. Feature queries
// depend on the viewport/settings and are deliberately left unresolved: this filter
// runs once when compiling the book, not whenever the reader changes orientation.
// Unknown media types are nonmatching (CSS Media Queries); unknown syntax is kept
// conservatively, so this does not claim to implement general media queries.
inline bool typeMayMatchScreen(std::string_view query) {
  query = trim(query);
  if (query.empty() || query.front() == '(') return true;
  auto type = token(query);
  const bool negate = equal(type, "not");
  if (negate || equal(type, "only")) type = token(query);
  if (type.empty()) return true;
  for (char c : type)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return true;
  if (equal(type, "and") || equal(type, "or") || equal(type, "not") || equal(type, "only")) return true;
  const bool matches = equal(type, "screen") || equal(type, "all");
  // `not screen and (...)` can match when its feature expression is false.
  if (negate) return !matches || !query.empty();
  return matches;
}

inline bool mayMatchScreen(std::string_view media) {
  if (trim(media).empty()) return true;
  size_t start = 0;
  unsigned depth = 0;
  for (size_t i = 0; i <= media.size(); ++i) {
    if (i < media.size() && media[i] == '(') ++depth;
    if (i < media.size() && media[i] == ')' && depth) --depth;
    if (i == media.size() || (media[i] == ',' && depth == 0)) {
      if (typeMayMatchScreen(media.substr(start, i - start))) return true;
      start = i + 1;
    }
  }
  return false;
}
// Canonical archive-relative reference used only for stylesheet inventory.
// Invalid/external references make the caller retain the native CSS merge.
inline bool resolveArchiveHref(std::string_view directory, std::string_view reference, std::string& out) {
  reference = reference.substr(0, reference.find_first_of("?#"));
  if (reference.empty() || reference.size() > 4096 || directory.size() > 4096) return false;
  std::string decoded;
  decoded.reserve(reference.size());
  const auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < reference.size(); ++i) {
    char c = reference[i];
    if (c == '%') {
      if (i + 2 >= reference.size() || hex(reference[i + 1]) < 0 || hex(reference[i + 2]) < 0) return false;
      c = static_cast<char>((hex(reference[i + 1]) << 4) | hex(reference[i + 2]));
      i += 2;
    }
    if (c == 0 || c == '\\') return false;
    decoded.push_back(c);
  }
  if (decoded.rfind("//", 0) == 0 || decoded.find(':') < decoded.find('/')) return false;
  std::string joined = decoded.front() == '/' ? decoded : std::string(directory) + decoded;
  out.clear();
  out.reserve(joined.size());
  for (size_t begin = 0; begin < joined.size();) {
    const size_t end = joined.find('/', begin);
    const auto component = std::string_view(joined).substr(begin, end == std::string::npos ? end : end - begin);
    if (component == "..") {
      if (out.empty()) return false;
      const size_t slash = out.find_last_of('/');
      out.resize(slash == std::string::npos ? 0 : slash);
    } else if (!component.empty() && component != ".") {
      if (!out.empty()) out.push_back('/');
      out.append(component.data(), component.size());
    }
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return !out.empty();
}
}  // namespace cssMedia
