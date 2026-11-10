#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../../../EpdFont/GenericFontFamily.h"

// Per-book CSS font names. Native 32-bit tokens belong to ordered fallback stacks, not individual faces:
// a declaration can precede its @font-face rule, and its first named face may be absent.
class CssFontCatalog {
 public:
  using Token = uint32_t;
  static constexpr Token Reader = 0;
  static constexpr Token Inherit = 0xfffffff0u;
  static constexpr Token INVALID_ID = 0xffffffffu;
  static constexpr size_t MAX_STACKS = 250;
  static constexpr size_t MAX_FACES = 64;
  static constexpr size_t MAX_FAMILIES_PER_STACK = 16;
  static constexpr size_t MAX_SOURCES_PER_FACE = 8;
  static constexpr size_t MAX_NAME_BYTES = 128;
  static constexpr size_t MAX_PATH_BYTES = 512;
  static constexpr size_t MAX_TEXT_BYTES = 16384;

  enum class Style : uint8_t { Normal = 0, Italic = 1, Oblique = 2 };
  struct FamilyName {
    std::string name;  // CSS-unescaped, whitespace-normalized and ASCII case-folded
    Token token = 0;
    Token generic = Reader;  // Reader means a named face, including quoted "serif"
    bool operator==(const FamilyName& rhs) const { return name == rhs.name && generic == rhs.generic; }
  };
  struct FamilyStack {
    std::vector<FamilyName> families;
    Token token = 0;
    Token fallback = Reader;
  };
  struct Source {
    std::string path;    // normalized, case-preserving path INSIDE the EPUB; never a remote URL
    std::string format;  // optional CSS format() hint
    bool operator==(const Source& rhs) const { return path == rhs.path && format == rhs.format; }
  };
  struct Face {
    Token family = 0;
    std::string familyName;
    Style style = Style::Normal;
    uint16_t weight = 400;
    uint32_t declarationOrder = 0;
    std::vector<Source> sources;
    bool truncated = false;  // a descriptor exceeded the bounded parser limits
  };

  // INVALID_ID means an invalid declaration. Limits return the stack's generic fallback
  // and mark truncated(). Newly interned tokens remain stable until reset().
  Token intern(std::string_view value, bool* added = nullptr);
  static Token genericFallback(std::string_view value);
  // Remap only families declared by this document; all other fallback members stay intact.
  Token scopeStack(Token id, const std::string& prefix, bool* added = nullptr);
  const FamilyStack* stack(Token id) const;
  Token fallbackFamily(Token id) const;
  const std::vector<Face>& faces() const { return faces_; }
  static std::string resolvePath(std::string_view directory, std::string_view value);
  static Token namedToken(std::string_view name);
  static Token stackToken(const FamilyStack& stack);
  uint32_t signature() const;
  bool write(HalFile& file) const;
  bool read(HalFile& file);
  static bool inspect(HalFile& file, size_t* hydrationBytes = nullptr);
  size_t residentBytes() const;
  bool isExtensionOf(const CssFontCatalog& previous) const;
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
  bool load(const std::string& directory, size_t retainedBytes = 0);
  static void removeFiles(const std::string& directory);

 private:
  std::vector<FamilyStack> stacks_;
  std::vector<Face> faces_;
  bool truncated_ = false;
  uint32_t faceRevision_ = 1;
  mutable uint32_t generation_ = 0;
  mutable uint8_t slot_ = 1;
  size_t textBytes() const;
  bool admitStack(FamilyStack&& stack, bool* added, Token& result);
};
