#pragma once

#include <cctype>
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
}  // namespace cssMedia
