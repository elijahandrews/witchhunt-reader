// Image dimensions behind a large JPEG metadata block (witchhunt-reader #249).
//
// The image manifest probes the first 4 KB of an entry. A Photoshop-style export carries
// EXIF + IPTC + XMP + ICC in front of the frame header — ~29 KB in the reporter's book — so
// the probe never reaches SOF. Finding it then means a streaming walk of the DEFLATED entry
// through an inflate ring sized to the entry (up to 32 KB), which is exactly the block the
// C3 cannot promise mid-parse: the reporter's image sits on page 54 of a 58-page chapter,
// where the heap is at its most fragmented. When that ring failed, the image was cached as
// alt text for good, indistinguishable from a corrupt file.
//
// What has to hold:
//   1. A JPEG whose SOF lies beyond the probe window is DEFERRED — it is neither resolved nor
//      written off as unreadable.
//   2. A deferred image can be resolved later (resolvePending), once, and persists like any
//      other entry, so the walk is paid once per image per book.
//   3. Mid-parse, the walk runs only when contiguous heap actually covers its ring; a refusal
//      is latched as provisional (the build can be discarded and redone), never cached.
//   4. Resolving the pending images at build end makes the next build of the same section
//      clean — the mechanism the reader uses to show the image after one rebuild.
#include <gtest/gtest.h>

#include <filesystem>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>

#include "BuildArena.h"
#include "Epub.h"
#include "Epub/EpubImageManifest.h"
#include "Epub/Section.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/ImageToFramebufferDecoder.h"
#include "GfxRenderer.h"
#include "PipelineRunner.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

const char* kBook = CORPUS_DIR "/test_jpeg_metadata_heavy.epub";
const char* kEntry = "OEBPS/images/photo.jpg";
constexpr int16_t kPhotoWidth = 64;
constexpr int16_t kPhotoHeight = 48;
// SOF 12 KB into a 53 KB entry: inside the walk's first 16 KB stage, in an entry whose full-size
// ring would be 32 KB.
const char* kBigEntry = "OEBPS/images/photo_big.jpg";
constexpr int16_t kBigWidth = 96;
constexpr int16_t kBigHeight = 64;

// A header that IS within the probe window, for the inline control case.
const char* kPngBook = CORPUS_DIR "/test_png_images.epub";
const char* kPngEntry = "OEBPS/images/scaling_test.png";

struct ImageManifestFixture : testing::Test {
  fs::path work;
  std::string cacheDir;
  uint32_t savedFree = 0;
  uint32_t savedContig = 0;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("epub_imgmanifest_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
    cacheDir = (work / "cache").string();
    fs::create_directories(cacheDir);
    savedFree = ESP.getFreeHeap();
    savedContig = ESP.getMaxAllocHeap();
  }
  void TearDown() override {
    ESP.setFreeHeap(savedFree);
    ESP.setMaxAllocHeap(savedContig);
    fs::remove_all(work);
  }
};

// --- 1 + 2: the manifest's own contract --------------------------------------------------

TEST_F(ImageManifestFixture, SofBeyondTheProbeWindowIsDeferredNotUnreadable) {
  EpubImageManifest manifest;
  ASSERT_TRUE(manifest.load(cacheDir));

  ImageDimensions entry = {0, 0};
  EXPECT_EQ(manifest.resolve(kBook, kEntry, entry), EpubImageManifest::Resolve::Deferred)
      << "a valid JPEG whose SOF sits past the probe window is not unreadable; it needs the streaming walk";
  EXPECT_EQ(entry.width, 0);
  EXPECT_TRUE(manifest.hasPending());
  ImageDimensions none = {0, 0};
  EXPECT_FALSE(manifest.find(kEntry, none)) << "nothing may be recorded until the dimensions are known";
}

TEST_F(ImageManifestFixture, ResolvePendingWalksTheEntryOnceAndPersists) {
  {
    EpubImageManifest manifest;
    ASSERT_TRUE(manifest.load(cacheDir));
    ImageDimensions entry = {0, 0};
    ASSERT_EQ(manifest.resolve(kBook, kEntry, entry), EpubImageManifest::Resolve::Deferred);

    EXPECT_EQ(manifest.resolvePending(), 1u);
    EXPECT_FALSE(manifest.hasPending());
    ImageDimensions found = {0, 0};
    ASSERT_TRUE(manifest.find(kEntry, found));
    EXPECT_EQ(found.width, kPhotoWidth);
    EXPECT_EQ(found.height, kPhotoHeight);
    manifest.persistIfDirty();
  }
  // A fresh load sees the persisted entry: the walk is paid once per image per book.
  EpubImageManifest reloaded;
  ASSERT_TRUE(reloaded.load(cacheDir));
  ImageDimensions entry = {0, 0};
  EXPECT_EQ(reloaded.resolve(kBook, kEntry, entry), EpubImageManifest::Resolve::Resolved);
  EXPECT_EQ(entry.width, kPhotoWidth);
  EXPECT_EQ(entry.height, kPhotoHeight);
  EXPECT_FALSE(reloaded.hasPending());
}

TEST_F(ImageManifestFixture, ResolvePendingWalksFromAnArenaWhenTheHeapCannot) {
  // Device-measured (X3, #249): reader-time contig tops out at ~31.7 KB, so a heap ring of
  // 512 + 32 KB never fits at any moment of a session. The one region that size which IS idle
  // at a build's end is the borrowed secondary framebuffer, so the walk must be able to carve
  // its ring from there and leave the arena exactly as it found it.
  EpubImageManifest manifest;
  ASSERT_TRUE(manifest.load(cacheDir));
  ImageDimensions entry = {0, 0};
  ASSERT_EQ(manifest.resolve(kBook, kEntry, entry), EpubImageManifest::Resolve::Deferred);

  ESP.setMaxAllocHeap(16 * 1024);  // the heap path would refuse
  BuildArena arena(40 * 1024);
  ASSERT_TRUE(arena.valid());
  EXPECT_EQ(manifest.resolvePending(&arena), 1u);
  EXPECT_EQ(arena.used(), 0u) << "the walk's ring is a scoped block; nothing may linger in the arena";
  ImageDimensions found = {0, 0};
  ASSERT_TRUE(manifest.find(kEntry, found));
  EXPECT_EQ(found.width, kPhotoWidth);
}

// --- the staged walk ---------------------------------------------------------------------------
//
// Every image in "Strange Pictures" (78 JPEGs, SOF 9.7-18.4 KB in) was unresolvable on the X3
// because the walk sized its ring to the ENTRY -- 32 KB, which that heap never holds mid-parse
// and, after a fragmented build, not even at the build's end (28660 contig vs 33280 needed).
// The header sits in the first 16 KB of output, and a 16 KB ring reads those bytes exactly.

TEST_F(ImageManifestFixture, AHeaderInsideTheFirstStageResolvesWithAStageSizedRing) {
  EpubImageManifest manifest;
  ASSERT_TRUE(manifest.load(cacheDir));
  ImageDimensions entry = {0, 0};
  ASSERT_EQ(manifest.resolve(kBook, kBigEntry, entry), EpubImageManifest::Resolve::Deferred);

  // 20 KB: hosts a 16 KB stage ring plus its chunk, never a 32 KB one. The heap is closed off,
  // so the only way to the dimensions is the first stage from this arena.
  ESP.setMaxAllocHeap(4 * 1024);
  BuildArena arena(20 * 1024);
  ASSERT_TRUE(arena.valid());
  EXPECT_EQ(manifest.resolvePending(&arena), 1u);
  EXPECT_EQ(arena.used(), 0u);
  EXPECT_LE(arena.highWater(), 17u * 1024u) << "the first stage must not take an entry-sized ring";
  ImageDimensions found = {0, 0};
  ASSERT_TRUE(manifest.find(kBigEntry, found));
  EXPECT_EQ(found.width, kBigWidth);
  EXPECT_EQ(found.height, kBigHeight);
}

TEST_F(ImageManifestFixture, TheHeapBudgetAdmitsExactlyTheStagesThatFit) {
  EpubImageManifest manifest;
  ASSERT_TRUE(manifest.load(cacheDir));
  ImageDimensions entry = {0, 0};
  ASSERT_EQ(manifest.resolve(kBook, kBigEntry, entry), EpubImageManifest::Resolve::Deferred);
  ASSERT_EQ(manifest.resolve(kBook, kEntry, entry), EpubImageManifest::Resolve::Deferred);

  // 18 KB of contiguous heap: the 16 KB first stage fits, so photo_big (SOF at 12 KB) resolves.
  // photo.jpg's SOF sits at 20.5 KB: its first stage ends short with the header still running,
  // and the second stage -- the whole 20.9 KB entry -- does not fit. It must stay queued, not be
  // written off: the reader retries the queue from the borrowed framebuffer before its next
  // build, and a walk dropped here would be a chapter cached without that image.
  ESP.setMaxAllocHeap(18 * 1024);
  EXPECT_EQ(manifest.resolvePending(), 1u);
  {
    ImageDimensions d = {0, 0};
    EXPECT_TRUE(manifest.find(kBigEntry, d));
  }
  {
    ImageDimensions d = {0, 0};
    EXPECT_FALSE(manifest.find(kEntry, d));
  }
  EXPECT_TRUE(manifest.hasPending()) << "a walk short of memory must stay queued";

  // Resolved once a caller can host its second stage.
  ESP.setMaxAllocHeap(100 * 1024);
  EXPECT_EQ(manifest.resolvePending(), 1u);
  EXPECT_FALSE(manifest.hasPending());
  ImageDimensions found = {0, 0};
  ASSERT_TRUE(manifest.find(kEntry, found));
  EXPECT_EQ(found.width, kPhotoWidth);
}

TEST_F(ImageManifestFixture, ResolveDeferredNowReportsWhichStageItLacked) {
  EpubImageManifest manifest;
  ASSERT_TRUE(manifest.load(cacheDir));
  ImageDimensions entry = {0, 0};
  ASSERT_EQ(manifest.resolve(kBook, kEntry, entry), EpubImageManifest::Resolve::Deferred);

  // The parser's mid-parse path, with what the heap can spare passed in explicitly.
  EXPECT_EQ(manifest.resolveDeferredNow(kBook, kEntry, entry, 8 * 1024), EpubImageManifest::Walk::NeedsHeap)
      << "not even the first stage fits";
  EXPECT_EQ(manifest.resolveDeferredNow(kBook, kEntry, entry, 18 * 1024), EpubImageManifest::Walk::NeedsHeap)
      << "the first stage ran and ended with the header still open; the second does not fit";
  EXPECT_TRUE(manifest.hasPending()) << "a walk short of heap leaves the image queued";
  EXPECT_EQ(manifest.resolveDeferredNow(kBook, kEntry, entry, 24 * 1024), EpubImageManifest::Walk::Resolved);
  EXPECT_EQ(entry.width, kPhotoWidth);
  EXPECT_FALSE(manifest.hasPending());
}

TEST_F(ImageManifestFixture, ResolvePendingFallsBackToTheHeapWhenTheArenaIsTooSmall) {
  EpubImageManifest manifest;
  ASSERT_TRUE(manifest.load(cacheDir));
  ImageDimensions entry = {0, 0};
  ASSERT_EQ(manifest.resolve(kBook, kEntry, entry), EpubImageManifest::Resolve::Deferred);

  ESP.setMaxAllocHeap(100 * 1024);
  BuildArena arena(8 * 1024);  // cannot host the ~21 KB ring
  ASSERT_TRUE(arena.valid());
  EXPECT_EQ(manifest.resolvePending(&arena), 1u);
  EXPECT_EQ(arena.used(), 0u);
  {
    ImageDimensions d = {0, 0};
    EXPECT_TRUE(manifest.find(kEntry, d));
  }
}

TEST_F(ImageManifestFixture, AMissingEntryIsUnreadableNotDeferred) {
  EpubImageManifest manifest;
  ASSERT_TRUE(manifest.load(cacheDir));
  ImageDimensions entry = {0, 0};
  EXPECT_EQ(manifest.resolve(kBook, "OEBPS/images/absent.jpg", entry), EpubImageManifest::Resolve::Unreadable)
      << "an image that cannot exist must not be queued for a walk that can never succeed";
  EXPECT_EQ(entry.width, 0);
  EXPECT_FALSE(manifest.hasPending());
}

TEST_F(ImageManifestFixture, AJpegShorterThanTheWindowWithNoSofIsUnreadable) {
  // SOI, a complete APP0, then an APP1 whose declared length runs past the end of the file: the
  // marker walk runs out of bytes, but the whole entry was in the window, so no walk can ever
  // find a SOF. Deferring it would queue a walk that fails at every build's end.
  std::string jpeg = "\xFF\xD8\xFF\xE0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00";
  jpeg += "\xFF\xE1\x40\x00Exif\x00\x00";
  jpeg += std::string(200, '\x5A');
  test_zip::StoredZipWriter zip;
  zip.add("mimetype", "application/epub+zip");
  zip.add("OEBPS/images/cut.jpg", jpeg);
  const std::string book = (work / "cut.epub").string();
  zip.write(book);

  EpubImageManifest manifest;
  ASSERT_TRUE(manifest.load(cacheDir));
  ImageDimensions entry = {0, 0};
  EXPECT_EQ(manifest.resolve(book, "OEBPS/images/cut.jpg", entry), EpubImageManifest::Resolve::Unreadable);
  EXPECT_EQ(entry.width, 0);
  EXPECT_FALSE(manifest.hasPending());
}

TEST_F(ImageManifestFixture, AHeaderWithinTheWindowResolvesInline) {
  EpubImageManifest manifest;
  ASSERT_TRUE(manifest.load(cacheDir));
  ImageDimensions entry = {0, 0};
  EXPECT_EQ(manifest.resolve(kPngBook, kPngEntry, entry), EpubImageManifest::Resolve::Resolved);
  EXPECT_GT(entry.width, 0);
  EXPECT_FALSE(manifest.hasPending());
}

// --- a large number of images ----------------------------------------------------------------
//
// Nothing per image lives in memory with a cap: the queue of deferred images is a file on the
// card (it used to be 16 in memory, past which images were laid out as alt text with no retry),
// and a resolved image is a 12-byte record in an array that grows in steps.

// A minimal JPEG whose SOF sits behind `metadataBytes` of APP1: past the 4 KB probe window, so
// the manifest must defer it. Stored (not deflated) in the archive, so its walk needs no ring
// and the tests below are about the queue, not the heap.
std::string jpegWithLateSof(const size_t metadataBytes, const uint16_t width, const uint16_t height) {
  // Built byte by byte: a string literal with an embedded NUL would stop at the NUL under +=.
  std::string j;
  auto put = [&](const std::initializer_list<int> bytes) {
    for (const int b : bytes) j.push_back(static_cast<char>(b));
  };
  put({0xFF, 0xD8});
  const size_t payload = metadataBytes - 4;
  put({0xFF, 0xE1, static_cast<int>(((payload + 2) >> 8) & 0xFF), static_cast<int>((payload + 2) & 0xFF)});
  j += std::string(payload, '\x5A');
  put({0xFF, 0xC0, 0x00, 0x0B, 0x08, height >> 8, height & 0xFF, width >> 8, width & 0xFF, 0x01, 0x01, 0x11, 0x00});
  put({0xFF, 0xD9});
  return j;
}

struct ManyImagesFixture : ImageManifestFixture {
  static constexpr int kImages = 40;  // well past the old in-memory cap of 16 and the array's first growth step
  std::string book;

  void SetUp() override {
    ImageManifestFixture::SetUp();
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    for (int i = 0; i < kImages; ++i) {
      zip.add(entry(i), jpegWithLateSof(6000, static_cast<uint16_t>(100 + i), static_cast<uint16_t>(50 + i)));
    }
    book = (work / "many.epub").string();
    zip.write(book);
  }
  static std::string entry(const int i) { return "OEBPS/images/p" + std::to_string(i) + ".jpg"; }
};

TEST_F(ManyImagesFixture, TheQueueHasNoCapAndSurvivesAReload) {
  {
    EpubImageManifest manifest;
    ASSERT_TRUE(manifest.load(cacheDir));
    for (int i = 0; i < kImages; ++i) {
      ImageDimensions d = {0, 0};
      ASSERT_EQ(manifest.resolve(book, entry(i), d), EpubImageManifest::Resolve::Deferred) << entry(i);
    }
    EXPECT_TRUE(manifest.hasPending());
    manifest.releaseMemory();  // what the reader does to unpin the heap: the queue must not be in it
  }
  EpubImageManifest reloaded;
  ASSERT_TRUE(reloaded.load(cacheDir));
  EXPECT_TRUE(reloaded.hasPending()) << "the queue is on the card, not in the object that was dropped";
  ImageDimensions before = {0, 0};
  EXPECT_FALSE(reloaded.find(entry(kImages - 1), before));

  // Every one of them resolves, including those past the old cap, and the records outlive both
  // a persist and a reload.
  EXPECT_EQ(reloaded.resolvePending(nullptr, book), static_cast<size_t>(kImages));
  EXPECT_FALSE(reloaded.hasPending());
  EXPECT_EQ(reloaded.entryCount(), static_cast<size_t>(kImages));
  reloaded.persistIfDirty();

  EpubImageManifest again;
  ASSERT_TRUE(again.load(cacheDir));
  EXPECT_EQ(again.entryCount(), static_cast<size_t>(kImages));
  EXPECT_FALSE(again.hasPending());
  for (int i = 0; i < kImages; ++i) {
    ImageDimensions d = {0, 0};
    ASSERT_TRUE(again.find(entry(i), d)) << entry(i);
    EXPECT_EQ(d.width, 100 + i);
    EXPECT_EQ(d.height, 50 + i);
  }
}

TEST_F(ManyImagesFixture, AnImageResolvedInlineLeavesTheQueueAndTheCompactionDropsIt) {
  EpubImageManifest manifest;
  ASSERT_TRUE(manifest.load(cacheDir));
  ImageDimensions d = {0, 0};
  ASSERT_EQ(manifest.resolve(book, entry(0), d), EpubImageManifest::Resolve::Deferred);
  ASSERT_EQ(manifest.resolve(book, entry(1), d), EpubImageManifest::Resolve::Deferred);
  ASSERT_EQ(manifest.resolve(book, entry(0), d), EpubImageManifest::Resolve::Deferred) << "queued once, not twice";

  EXPECT_EQ(manifest.resolveDeferredNow(book, entry(0), d, 64 * 1024), EpubImageManifest::Walk::Resolved);
  EXPECT_EQ(d.width, 100);
  EXPECT_TRUE(manifest.hasPending()) << "entry(1) is still queued";

  // The build-end pass finds one live record; entry(0)'s is dead and must not be walked again.
  EXPECT_EQ(manifest.resolvePending(), 1u);
  EXPECT_FALSE(manifest.hasPending());
  EXPECT_EQ(manifest.entryCount(), 2u);
  EXPECT_EQ(manifest.resolvePending(), 0u) << "an empty queue is a no-op";

  EpubImageManifest reloaded;
  ASSERT_TRUE(reloaded.load(cacheDir));
  EXPECT_FALSE(reloaded.hasPending()) << "a compacted-empty queue leaves nothing behind";
}

// --- 3 + 4: the parser and the build ------------------------------------------------------

struct DeferredImageBuildFixture : ImageManifestFixture {
  std::shared_ptr<Epub> epub;
  GfxRenderer renderer;

  void TearDown() override {
    epub.reset();  // release book.bin and the manifest's resolve handle before the dir goes
    ImageManifestFixture::TearDown();
  }

  Section::BuildParams params() const {
    Section::BuildParams p;
    p.viewportWidth = 480;
    p.viewportHeight = 800;
    p.lineCompression = 1.0f;
    return p;
  }

  void openBook() {
    epub = std::make_shared<Epub>(kBook, cacheDir);
    ASSERT_TRUE(epub->load(true));
    epub->loadImageManifest();  // as the reader does before its first section build
    ASSERT_NE(epub->getImageManifest(), nullptr);
  }

  bool buildAndReportDegraded() {
    Section section(epub, 0, renderer);
    section.clearCache();
    EXPECT_TRUE(section.createSectionFile(params(), {}, /*skipEviction=*/true));
    EXPECT_GT(section.pageCount, 0);
    return section.isImageHeaderDegraded();
  }
};

TEST_F(DeferredImageBuildFixture, AmpleContiguousHeapResolvesInlineAndIsNotFlagged) {
  openBook();
  ESP.setMaxAllocHeap(100 * 1024);
  EXPECT_FALSE(buildAndReportDegraded());
  ImageDimensions d = {0, 0};
  EXPECT_TRUE(epub->getImageManifest()->find(kEntry, d)) << "an inline walk records its result like any other";
}

TEST_F(DeferredImageBuildFixture, RingRefusedMidParseIsLatchedAsProvisional) {
  openBook();
  // 16 KB clears the 8 KB header-read gate but cannot host this entry's ~21 KB inflate ring.
  // Before the fix the walk was attempted regardless (host malloc never fails), so this
  // reported clean — and on the device the failed ring was cached as permanent alt text.
  ESP.setMaxAllocHeap(16 * 1024);
  EXPECT_TRUE(buildAndReportDegraded()) << "a walk the heap cannot host must be latched, never cached as final";
  ImageDimensions d = {0, 0};
  EXPECT_FALSE(epub->getImageManifest()->find(kEntry, d));
  EXPECT_TRUE(epub->getImageManifest()->hasPending());
}

TEST_F(DeferredImageBuildFixture, ResolvingPendingAtBuildEndMakesTheNextBuildClean) {
  openBook();
  ESP.setMaxAllocHeap(16 * 1024);
  ASSERT_TRUE(buildAndReportDegraded());

  // Build end: the parser and its arena are gone, the heap is at its best. The reader calls
  // this after every completed build; it must report that a deferred image was resolved so
  // the caller knows a rebuild will now come out clean.
  ESP.setMaxAllocHeap(100 * 1024);
  EXPECT_TRUE(epub->persistImageManifest());
  EXPECT_FALSE(epub->persistImageManifest()) << "nothing left to resolve: the second call must not claim progress";

  ESP.setMaxAllocHeap(16 * 1024);  // the rebuild needs no ring at all: the manifest answers
  EXPECT_FALSE(buildAndReportDegraded());
}

// Exercise the real manifest dispatcher as well as the direct factory: Section
// layout uses the former, so a standalone SVG decoder test cannot cover it.
std::string makeSvgBook(const fs::path& work, bool deflated) {
  test_zip::StoredZipWriter zip;
  zip.add("mimetype", "application/epub+zip");
  zip.add("META-INF/container.xml",
          "<container><rootfiles><rootfile full-path='content.opf' "
          "media-type='application/oebps-package+xml'/></rootfiles></container>");
  zip.add(
      "content.opf",
      "<package xmlns='http://www.idpf.org/2007/opf' version='3.0'><metadata "
      "xmlns:dc='http://purl.org/dc/elements/1.1/'><dc:title>Synthetic vectors</dc:title></metadata><manifest><item "
      "id='c' href='chapter.xhtml' media-type='application/xhtml+xml'/><item id='a' href='images/ornament.svg' "
      "media-type='image/svg+xml'/><item id='b' href='images/icon.SVG' "
      "media-type='image/svg+xml'/></manifest><spine><itemref idref='c'/></spine></package>");
  zip.add(
      "chapter.xhtml",
      "<html xmlns='http://www.w3.org/1999/xhtml'><head><title>Vectors</title></head><body><p>Before</p><img "
      "src='images/ornament.svg' alt='MissingOrnament'/><p>Between</p><img src='images/icon.SVG' "
      "alt='MissingIcon'/><img src='images/ornament.svg' alt='MissingRepeatedOrnament'/><p>After</p></body></html>");
  const std::string ornament =
      "<svg xmlns='http://www.w3.org/2000/svg' width='72' height='24'><path fill='#606060' d='M4 12Q36 0 68 12Q36 24 4 "
      "12Z'/></svg>";
  const std::string icon =
      "<?xml version='1.0'?><svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 40 40'><g fill-rule='evenodd'><path "
      "d='M36 20A16 16 0 1 1 4 20A16 16 0 1 1 36 20Z M28 20A8 8 0 1 1 12 20A8 8 0 1 1 28 20Z'/></g></svg>";
  if (deflated) {
    zip.addDeflated("images/ornament.svg", ornament);
    zip.addDeflated("images/icon.SVG", icon);
  } else {
    zip.add("images/ornament.svg", ornament);
    zip.add("images/icon.SVG", icon);
  }
  const std::string path = (work / (deflated ? "deflated.epub" : "stored.epub")).string();
  zip.write(path);
  return path;
}

TEST_F(ImageManifestFixture, SvgZipHeadersResolveThroughFactoryAndPersistedManifest) {
  for (bool deflated : {false, true}) {
    SCOPED_TRACE(deflated);
    const auto book = makeSvgBook(work, deflated);
    const auto cache = (work / (deflated ? "deflated-cache" : "stored-cache")).string();
    fs::create_directories(cache);
    EpubImageManifest manifest;
    ASSERT_TRUE(manifest.load(cache));
    BuildArena arena(16 * 1024);
    for (const auto& entry : {"images/ornament.svg", "images/icon.SVG"}) {
      const bool ornament = std::string(entry).find("ornament") != std::string::npos;
      ImageDimensions dimensions{};
      ASSERT_TRUE(ImageDecoderFactory::getDimensionsFromZipEntry(book, entry, dimensions));
      EXPECT_EQ(dimensions.width, ornament ? 72 : 40);
      EXPECT_EQ(dimensions.height, ornament ? 24 : 40);
      ASSERT_EQ(manifest.resolve(book, entry, dimensions, &arena), EpubImageManifest::Resolve::Resolved);
      EXPECT_EQ(dimensions.width, ornament ? 72 : 40);
      EXPECT_EQ(dimensions.height, ornament ? 24 : 40);
      EXPECT_EQ(arena.used(), 0u);
      EXPECT_EQ(arena.releaseFailures(), 0u);
    }
    EXPECT_EQ(manifest.entryCount(), 2u);
    EXPECT_FALSE(manifest.hasPending());
    manifest.persistIfDirty();
    EpubImageManifest warm;
    ASSERT_TRUE(warm.load(cache));
    EXPECT_EQ(warm.entryCount(), 2u);
    ImageDimensions dimensions{};
    // No source file needed on a warm hit, proving the dimensions persisted.
    EXPECT_EQ(warm.resolve("absent.epub", "images/ornament.svg", dimensions), EpubImageManifest::Resolve::Resolved);
    EXPECT_EQ(dimensions.width, 72);
    EXPECT_EQ(dimensions.height, 24);
  }
}

TEST_F(ImageManifestFixture, SvgFullSectionsContainEveryInstanceColdAndWarm) {
  for (bool deflated : {false, true}) {
    SCOPED_TRACE(deflated);
    const auto book = makeSvgBook(work, deflated);
    const auto cache = (work / (deflated ? "deflated-sections" : "stored-sections")).string();
    pipeline_harness::Profile profile;
    profile.lentArenaBytes = 48 * 1024;
    profile.fontSizeNormalization = false;
    std::string cold;
    for (int pass = 0; pass < 2; ++pass) {
      std::ostringstream out;
      ASSERT_TRUE(pipeline_harness::runAndDump(book, cache, profile, out));
      const auto dump = out.str();
      size_t images = 0, offset = 0;
      while ((offset = dump.find("  IMG ", offset)) != std::string::npos) {
        ++images;
        offset += 6;
      }
      EXPECT_EQ(images, 3u);
      EXPECT_NE(dump.find("w=72 h=24"), std::string::npos);
      EXPECT_NE(dump.find("w=40 h=40"), std::string::npos);
      EXPECT_EQ(dump.find("t=[Image:"), std::string::npos);
      EXPECT_EQ(dump.find("truncated=1"), std::string::npos);
      EXPECT_EQ(dump.find("cssFallback=1"), std::string::npos);
      if (!pass)
        cold = dump;
      else
        EXPECT_EQ(dump, cold);
    }
  }
}

}  // namespace
