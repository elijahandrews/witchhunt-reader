#include <CpFontFormat.h>
#include <FlashFontPartition.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <SdCardFont.h>
#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "FontIdNamespace.h"
#include "TtfBuilder.h"

namespace flash_double {
using Key = std::pair<std::string, uint8_t>;
std::map<Key, std::vector<uint8_t>> entries;
size_t capacity = 4 * 1024 * 1024;
int erases = 0, appends = 0, commits = 0, maps = 0;
bool mapped = false;
}  // namespace flash_double

namespace FlashFontPartition {
size_t fontUsableSize() { return flash_double::capacity; }
bool isMapped() { return flash_double::mapped; }
void unmap() { flash_double::mapped = false; }
bool hasEntry(const char* name, uint8_t size) { return flash_double::entries.count({name, size}); }
bool beginWrite(const char*) {
  ++flash_double::erases;
  flash_double::entries.clear();
  return true;
}
bool appendFile(const char* path, const char* name, uint8_t size) {
  ++flash_double::appends;
  std::ifstream source(path, std::ios::binary);
  if (!source) return false;
  std::vector<uint8_t> bytes(std::istreambuf_iterator<char>{source}, {});
  size_t used = HEADER_BYTES;
  for (const auto& entry : flash_double::entries) used += (entry.second.size() + 3) & ~size_t(3);
  if (used + ((bytes.size() + 3) & ~size_t(3)) > flash_double::capacity) return false;
  flash_double::entries[{name, size}] = std::move(bytes);
  return true;
}
bool finaliseWrite() {
  ++flash_double::commits;
  return !flash_double::entries.empty();
}
bool mmap(const char* name, uint8_t size, const uint8_t** ptr, size_t* length) {
  ++flash_double::maps;
  const auto it = flash_double::entries.find({name, size});
  if (it == flash_double::entries.end()) return false;
  *ptr = it->second.data();
  *length = it->second.size();
  flash_double::mapped = true;
  return true;
}
}  // namespace FlashFontPartition

namespace {
void put16(std::vector<uint8_t>& b, size_t p, uint16_t value) {
  b[p] = value;
  b[p + 1] = value >> 8;
}
void put32(std::vector<uint8_t>& b, size_t p, uint32_t value) {
  for (int i = 0; i < 4; ++i) b[p + i] = value >> (8 * i);
}
std::vector<uint8_t> fixture(bool v5, uint8_t marker = 0x88, size_t padding = 0) {
  // One style, two intervals, two glyphs, and separate one-byte bitmap markers.
  std::vector<uint8_t> bytes(122 + padding, 0);
  const char magic[] = "CPFONT\0\0";
  std::copy(magic, magic + 8, bytes.begin());
  put16(bytes, 8, v5 ? 5 : 4);
  put16(bytes, 10, 1);
  bytes[12] = 1;
  bytes[13] = v5 ? 2 : 0;
  put32(bytes, 36, 2);
  put32(bytes, 40, 2);
  bytes[44] = v5 ? 40 : 20;
  put32(bytes, 56, 64);
  put32(bytes, 64, 'A');
  put32(bytes, 68, 'A');
  put32(bytes, 76, 0xFFFD);
  put32(bytes, 80, 0xFFFD);
  put32(bytes, 84, 1);
  for (size_t glyph = 0; glyph < 2; ++glyph) {
    const size_t at = 88 + 16 * glyph;
    bytes[at] = 2;
    bytes[at + 1] = 1;
    put16(bytes, at + 2, v5 ? 320 : 160);
    put16(bytes, at + 6, 2);
    put16(bytes, at + 8, 1);
    put32(bytes, at + 12, glyph);
  }
  bytes[120] = marker;
  bytes[121] = 0xCC;
  if (v5) put32(bytes, 14, cpfont::crc32Update(0xFFFFFFFFu, bytes.data() + 32, bytes.size() - 32) ^ 0xFFFFFFFFu);
  return bytes;
}
class SdFontManager : public testing::Test {
 protected:
  GfxRenderer renderer;
  std::filesystem::path path;
  std::filesystem::path secondPath;
  void SetUp() override {
    flash_double::entries.clear();
    flash_double::capacity = 4 * 1024 * 1024;
    flash_double::erases = flash_double::appends = flash_double::commits = flash_double::maps = 0;
    flash_double::mapped = false;
    path = std::filesystem::temp_directory_path() /
           (std::string("sd_manager_") + testing::UnitTest::GetInstance()->current_test_info()->name() + ".cpfont");
    secondPath = path.string() + ".second";
  }
  void TearDown() override {
    std::filesystem::remove(path);
    std::filesystem::remove(secondPath);
  }
  SdCardFontFamilyInfo write(const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    out.close();
    return {"Synthetic", {{path.string(), 14, 0, static_cast<uint32_t>(bytes.size())}}};
  }
  SdCardFontFamilyInfo writeTwoSizes() {
    auto family = write(fixture(true, 0x88, 16));
    const auto smaller = fixture(true, 0x44, 48);
    std::ofstream out(secondPath, std::ios::binary);
    out.write(reinterpret_cast<const char*>(smaller.data()), smaller.size());
    out.close();
    family.files.push_back({secondPath.string(), 12, 0, static_cast<uint32_t>(smaller.size())});
    return family;
  }
  void expectNative(SdCardFontManager& manager, float scale, uint8_t marker) { expectSize(manager, 14, scale, marker); }
  void expectSize(SdCardFontManager& manager, uint8_t pointSize, float scale, uint8_t marker) {
    const int id = manager.getFontId("Synthetic", pointSize);
    ASSERT_NE(id, 0);
    ASSERT_EQ(renderer.native.count(id), 1);
    EXPECT_FLOAT_EQ(renderer.fontBaseScale(id), scale);
    auto* font = renderer.native.at(id);
    ASSERT_EQ(font->prewarm("A", 1), 0);
    const auto glyph = font->getEpdFont()->getGlyph('A');
    ASSERT_TRUE(glyph);
    EXPECT_EQ(font->getEpdFont()->data->bitmap[glyph.sdRecord->dataOffset], marker);
  }
};

TEST_F(SdFontManager, NativeDensityAndAliasComposeExactlyOnce) {
  for (bool v5 : {false, true}) {
    auto family = write(fixture(v5));
    SdCardFontManager manager;
    ASSERT_TRUE(manager.loadFamily(family, renderer, 22));
    expectNative(manager, v5 ? .5f : 1.f, 0x88);
    EXPECT_TRUE(FlashFontPartition::isMapped());
    const int id = manager.getFontId("Synthetic", 22);
    ASSERT_NE(id, 0);
    EXPECT_EQ(renderer.aliases.at(id), renderer.native.begin()->second);
    EXPECT_FLOAT_EQ(renderer.fontBaseScale(id), v5 ? 22.f / 28.f : 22.f / 14.f);
    ASSERT_TRUE(manager.ensureSizeAlias(renderer, 14));
    EXPECT_EQ(manager.getFontId("Synthetic", 22), 0);
    EXPECT_TRUE(renderer.aliases.empty());
    manager.unloadAll(renderer);
    EXPECT_TRUE(renderer.fonts.empty());
    EXPECT_TRUE(renderer.scales.empty());
    EXPECT_TRUE(renderer.native.empty());
    EXPECT_FALSE(FlashFontPartition::isMapped());
  }
}
TEST_F(SdFontManager, StaleV4FlashIsReplacedByV5) {
  flash_double::entries[{"Synthetic", 14}] = fixture(false);
  const auto family = write(fixture(true));
  SdCardFontManager manager;
  int coldLoads = 0;
  ASSERT_TRUE(manager.loadFamily(family, renderer, 14, [&] { ++coldLoads; }));
  expectNative(manager, .5f, 0x88);
  EXPECT_EQ(flash_double::erases, 1);
  EXPECT_EQ(flash_double::appends, 1);
  EXPECT_EQ(flash_double::commits, 1);
  EXPECT_EQ(coldLoads, 1);
  EXPECT_EQ(flash_double::entries.at({"Synthetic", 14})[8], 5);
}
TEST_F(SdFontManager, V5PayloadCrcRefreshesCacheAndFontId) {
  SdCardFontManager manager;
  ASSERT_TRUE(manager.loadFamily(write(fixture(true)), renderer, 14));
  const int original = manager.getFontId("Synthetic", 14);
  ASSERT_TRUE(manager.loadFamily(write(fixture(true, 0x44)), renderer, 14));
  expectNative(manager, .5f, 0x44);
  EXPECT_NE(manager.getFontId("Synthetic", 14), original);
  EXPECT_EQ(flash_double::erases, 2);
  // A subsequent unchanged source is a read-only flash hit, not an erase.
  ASSERT_TRUE(manager.loadFamily(write(fixture(true, 0x44)), renderer, 14));
  EXPECT_EQ(flash_double::erases, 2);
  EXPECT_TRUE(FlashFontPartition::isMapped());
}
TEST_F(SdFontManager, ReadOnlyStaleCacheLoadsSdWithoutWrites) {
  const auto stale = fixture(false);
  flash_double::entries[{"Synthetic", 14}] = stale;
  SdCardFontManager manager;
  int coldLoads = 0;
  ASSERT_TRUE(
      manager.loadFamily(write(fixture(true, 0x44)), renderer, 14, [&] { ++coldLoads; }, FlashCachePolicy::ReadOnly));
  expectNative(manager, .5f, 0x44);
  EXPECT_EQ(flash_double::erases, 0);
  EXPECT_EQ(flash_double::appends, 0);
  EXPECT_EQ(flash_double::commits, 0);
  EXPECT_EQ(coldLoads, 0);
  EXPECT_EQ(flash_double::entries.at({"Synthetic", 14}), stale);
  EXPECT_FALSE(FlashFontPartition::isMapped());
}
TEST_F(SdFontManager, OversizeSourceDoesNotEraseExistingCache) {
  const auto prior = fixture(false);
  flash_double::entries[{"Existing", 12}] = prior;
  const auto source = fixture(true, 0x44, 1024);
  flash_double::capacity = FlashFontPartition::HEADER_BYTES + source.size() - 1;
  SdCardFontManager manager;
  int coldLoads = 0;
  ASSERT_TRUE(manager.loadFamily(write(source), renderer, 14, [&] { ++coldLoads; }));
  expectNative(manager, .5f, 0x44);
  EXPECT_EQ(flash_double::erases, 0);
  EXPECT_EQ(flash_double::appends, 0);
  EXPECT_EQ(flash_double::commits, 0);
  EXPECT_EQ(coldLoads, 0);
  EXPECT_EQ(flash_double::entries.at({"Existing", 12}), prior);
}

TEST_F(SdFontManager, ResizeLargeFamilyAvoidsFlashWrites) {
  const auto family = writeTwoSizes();
  // Each size fits, but both together do not, as with dense bitmap families.
  flash_double::capacity = FlashFontPartition::HEADER_BYTES + 172;
  SdCardFontManager manager;
  int coldLoads = 0;
  const auto onColdLoad = [&] { ++coldLoads; };
  ASSERT_TRUE(manager.loadFamily(family, renderer, 14, onColdLoad));
  ASSERT_TRUE(FlashFontPartition::isMapped());
  ASSERT_EQ(flash_double::entries.size(), 1u);
  const auto original = flash_double::entries.at({"Synthetic", 14});
  const int originalId = manager.getFontId("Synthetic", 14);
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    ASSERT_TRUE(manager.loadFamily(family, renderer, 12, onColdLoad));
    expectSize(manager, 12, .5f, 0x44);
    EXPECT_FALSE(FlashFontPartition::isMapped());
    EXPECT_EQ(manager.getFontId("Synthetic", 14), 0);
    EXPECT_EQ(renderer.fonts.count(originalId), 0u);
    ASSERT_TRUE(manager.loadFamily(family, renderer, 14, onColdLoad));
    expectNative(manager, .5f, 0x88);
    EXPECT_TRUE(FlashFontPartition::isMapped());
    EXPECT_EQ(manager.getFontId("Synthetic", 14), originalId);
    EXPECT_EQ(manager.getFontId("Synthetic", 12), 0);
    EXPECT_EQ(flash_double::erases, 1);
    EXPECT_EQ(flash_double::appends, 1);
    EXPECT_EQ(flash_double::commits, 1);
    EXPECT_EQ(coldLoads, 1);
    EXPECT_EQ(flash_double::entries.size(), 1u);
    EXPECT_EQ(flash_double::entries.at({"Synthetic", 14}), original);
  }
  // Reopening another book at an uncached size preserves the same decision.
  manager.unloadAll(renderer);
  ASSERT_TRUE(manager.loadFamily(family, renderer, 12, onColdLoad));
  expectSize(manager, 12, .5f, 0x44);
  EXPECT_FALSE(FlashFontPartition::isMapped());
  EXPECT_EQ(flash_double::erases, 1);
  EXPECT_EQ(coldLoads, 1);
}

TEST_F(SdFontManager, WakeAndBookSwitchesReusePersistentFamilyCache) {
  const auto family = writeTwoSizes();
  flash_double::capacity = FlashFontPartition::HEADER_BYTES + 172;
  int coldLoads = 0;
  const auto onColdLoad = [&] { ++coldLoads; };
  {
    SdCardFontManager manager;
    ASSERT_TRUE(manager.loadFamily(family, renderer, 14, onColdLoad));
  }
  const auto original = flash_double::entries.at({"Synthetic", 14});
  // Deep sleep loses every manager field. Only the flash index survives.
  for (uint8_t size : {12, 14, 12, 14}) {
    SdCardFontManager manager;
    ASSERT_TRUE(manager.loadFamily(family, renderer, size, onColdLoad));
    expectSize(manager, size, .5f, size == 14 ? 0x88 : 0x44);
    EXPECT_EQ(FlashFontPartition::isMapped(), size == 14);
    EXPECT_EQ(flash_double::erases, 1);
    EXPECT_EQ(flash_double::appends, 1);
    EXPECT_EQ(flash_double::commits, 1);
    EXPECT_EQ(coldLoads, 1);
    EXPECT_EQ(flash_double::entries.at({"Synthetic", 14}), original);
  }
}

TEST_F(SdFontManager, StaleCachedSizeRefreshesEvenWithOtherSizePresent) {
  const auto family = writeTwoSizes();
  {
    SdCardFontManager manager;
    ASSERT_TRUE(manager.loadFamily(family, renderer, 14));
  }
  ASSERT_EQ(flash_double::entries.size(), 2u);
  write(fixture(true, 0x66, 16));
  SdCardFontManager manager;
  int coldLoads = 0;
  ASSERT_TRUE(manager.loadFamily(family, renderer, 14, [&] { ++coldLoads; }));
  expectNative(manager, .5f, 0x66);
  EXPECT_TRUE(FlashFontPartition::isMapped());
  EXPECT_EQ(flash_double::erases, 2);
  EXPECT_EQ(coldLoads, 1);
}

TEST_F(SdFontManager, RepeatedUncacheableLoadsNeverAnnounceFlashWrite) {
  const auto family = write(fixture(true));
  // Missing partition, metadata-only capacity, and a file that fits before
  // alignment but exceeds the actual four-byte-aligned storage budget.
  for (size_t capacity :
       {size_t(0), FlashFontPartition::HEADER_BYTES, FlashFontPartition::HEADER_BYTES + fixture(true).size()}) {
    flash_double::capacity = capacity;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
      SdCardFontManager manager;
      int coldLoads = 0;
      ASSERT_TRUE(manager.loadFamily(family, renderer, 14, [&] { ++coldLoads; }));
      expectNative(manager, .5f, 0x88);
      EXPECT_EQ(coldLoads, 0);
      EXPECT_EQ(flash_double::erases, 0);
      EXPECT_EQ(flash_double::appends, 0);
      EXPECT_EQ(flash_double::commits, 0);
      EXPECT_FALSE(FlashFontPartition::isMapped());
    }
  }
}

TEST_F(SdFontManager, ResizeWithinCachedFamilyStillUsesMappedFont) {
  const auto family = writeTwoSizes();
  SdCardFontManager manager;
  ASSERT_TRUE(manager.loadFamily(family, renderer, 14));
  ASSERT_EQ(flash_double::entries.size(), 2u);
  ASSERT_TRUE(manager.loadFamily(family, renderer, 12));
  expectSize(manager, 12, .5f, 0x44);
  EXPECT_TRUE(FlashFontPartition::isMapped());
  EXPECT_EQ(flash_double::erases, 1);
  EXPECT_EQ(flash_double::appends, 2);
  EXPECT_EQ(flash_double::commits, 1);
}

TEST_F(SdFontManager, DifferentFamilyAtNewSizeStillInstalls) {
  auto family = writeTwoSizes();
  flash_double::capacity = FlashFontPartition::HEADER_BYTES + 172;
  SdCardFontManager manager;
  ASSERT_TRUE(manager.loadFamily(family, renderer, 14));
  family.name = "Another synthetic family";
  ASSERT_TRUE(manager.loadFamily(family, renderer, 12));
  EXPECT_NE(manager.getFontId(family.name, 12), 0);
  EXPECT_EQ(manager.getFontId("Synthetic", 14), 0);
  EXPECT_TRUE(FlashFontPartition::isMapped());
  EXPECT_EQ(flash_double::erases, 2);
  EXPECT_EQ(flash_double::appends, 2);
  EXPECT_EQ(flash_double::commits, 2);
}

TEST_F(SdFontManager, LargeV5ChecksumUsesBatchedReadsAndStillRejectsCorruption) {
  auto bytes = fixture(true, 0x88, 64 * 1024);
  write(bytes);
  HalFile::readCalls = 0;
  {
    SdCardFont font;
    ASSERT_TRUE(font.load(path.c_str()));
    // Header, TOC and intervals add a few reads beyond the checksum payload.
    const size_t checksumReads = (bytes.size() - cpfont::HEADER_SIZE + 4095) / 4096;
    EXPECT_LE(HalFile::readCalls, checksumReads + 4);
  }
  {
    SdCardFont mapped;
    ASSERT_TRUE(mapped.loadFromMmap(bytes.data(), bytes.size(), nullptr));
  }
  bytes.back() ^= 1;
  write(bytes);
  SdCardFont corruptSd, corruptMapped;
  EXPECT_FALSE(corruptSd.load(path.c_str()));
  EXPECT_FALSE(corruptMapped.loadFromMmap(bytes.data(), bytes.size(), nullptr));
}
}  // namespace

TEST_F(SdFontManager, NativeTtfLazySizesRetainLogicalPointsAndSeparatePublisherIds) {
  ttf_test::Font source;
  source.glyphs.resize(2);
  source.glyphs[1].contours = {{{0, 0, true}, {500, 0, true}, {500, 700, true}, {0, 700, true}}};
  source.cmap['A'] = 1;
  const auto bytes = ttf_test::buildFont(source);
  auto family = write(bytes);
  family.files[0].pointSize = 0;
  SdCardFontManager manager;
  ASSERT_TRUE(manager.loadFamily(family, renderer, 10));
  ASSERT_TRUE(manager.ttfLoaded());
  const int body = manager.getFontId("Synthetic", 10);
  ASSERT_NE(body, 0);
  EXPECT_FALSE(fontIdNamespace::isPublisher(static_cast<uint32_t>(body)));
  EXPECT_FLOAT_EQ(renderer.pointSizes.at(body), 10.f);
  const auto sizes = manager.ttfNativeSizes(body);
  ASSERT_TRUE(sizes.has_value());
  for (const uint8_t px : {sizes->minPx, sizes->maxPx}) {
    const int id = sizes->base | px;
    EXPECT_FALSE(fontIdNamespace::isPublisher(static_cast<uint32_t>(id)));
    EXPECT_EQ(renderer.fonts.count(id), 0u);
    const auto* resolved = renderer.findFontFamily(id);
    ASSERT_NE(resolved, nullptr);
    EXPECT_FLOAT_EQ(renderer.pointSizes.at(id), px * 72.f / 150.f);
    EXPECT_TRUE(resolved->getGlyph('A'));
  }
  ASSERT_TRUE(manager.setTtfBodySize(renderer, 14));
  const int resized = manager.getFontId("Synthetic", 14);
  ASSERT_NE(resized, 0);
  EXPECT_FLOAT_EQ(renderer.pointSizes.at(resized), 14.f);
  manager.unloadAll(renderer);
  EXPECT_FALSE(renderer.hook.resolve);
  EXPECT_TRUE(renderer.fonts.empty());
}
