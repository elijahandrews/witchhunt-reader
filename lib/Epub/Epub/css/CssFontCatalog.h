#pragma once

#include <WordTypography.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Per-book CSS font names. IDs belong to ordered fallback stacks, not individual faces:
// a declaration can precede its @font-face rule, and its first named face may be absent.
class CssFontCatalog {
 public:
  static constexpr uint8_t FIRST_NAMED_ID = 5;
  static constexpr uint8_t INVALID_ID = 255;
  static constexpr size_t MAX_STACKS = 250;
  static constexpr size_t MAX_FACES = 64;
  static constexpr size_t MAX_FAMILIES_PER_STACK = 16;
  static constexpr size_t MAX_SOURCES_PER_FACE = 8;
  static constexpr size_t MAX_NAME_BYTES = 128;
  static constexpr size_t MAX_PATH_BYTES = 512;
  static constexpr size_t MAX_TEXT_BYTES = 16384;

  enum class Style : uint8_t { Normal = 0, Italic = 1, Oblique = 2 };
  struct FamilyName {
    std::string name;                          // CSS-unescaped, whitespace-normalized and ASCII case-folded
    uint8_t generic = wordTypography::Reader;  // Reader means a named face, including quoted "serif"
    bool operator==(const FamilyName& rhs) const { return name == rhs.name && generic == rhs.generic; }
  };
  struct FamilyStack {
    std::vector<FamilyName> families;
    uint8_t fallback = wordTypography::Reader;
  };
  struct Source {
    std::string path;    // normalized, case-preserving path INSIDE the EPUB; never a remote URL
    std::string format;  // optional CSS format() hint
    bool operator==(const Source& rhs) const { return path == rhs.path && format == rhs.format; }
  };
  struct Face {
    std::string family;
    Style style = Style::Normal;
    uint16_t weight = 400;
    std::vector<Source> sources;
    bool truncated = false;  // a descriptor exceeded the bounded parser limits
  };

  // INVALID_ID means an invalid declaration. Limits return the stack's generic fallback
  // and mark truncated(). Newly interned IDs are append-only until reset().
  uint8_t intern(std::string_view value, bool* added = nullptr);
  static uint8_t genericFallback(std::string_view value);
  // Remap only families declared by this document; all other fallback members stay intact.
  uint8_t scopeStack(uint8_t id, const std::string& prefix, bool* added = nullptr);
  const FamilyStack* stack(uint8_t id) const;
  uint8_t fallbackFamily(uint8_t id) const;
  const std::vector<Face>& faces() const { return faces_; }
  size_t stackCount() const { return stacks_.size(); }
  uint32_t faceRevision() const { return faceRevision_; }
  bool truncated() const { return truncated_; }
  void markTruncated() { truncated_ = true; }
  void discardLastStack();  // rollback when an inline declaration cannot be persisted
  static void parseFaceDeclaration(Face& face, std::string_view declaration, std::string_view stylesheetDir);
  void addFace(Face face);

  void reset();
  // Two alternating, checksummed snapshots: publishing a new inline stack never removes
  // the last valid catalog, even on FAT where rename cannot replace an existing file.
  bool save(const std::string& directory) const;
  bool load(const std::string& directory);
  static void removeFiles(const std::string& directory);

 private:
  std::vector<FamilyStack> stacks_;
  std::vector<Face> faces_;
  bool truncated_ = false;
  uint32_t faceRevision_ = 1;
  mutable uint32_t generation_ = 0;
  mutable uint8_t slot_ = 1;
  size_t textBytes() const;
};
