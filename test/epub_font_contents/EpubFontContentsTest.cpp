#include <gtest/gtest.h>

#include <optional>

#include "EpubFontHarness.h"
#include "StoredZipWriter.h"

namespace {
constexpr char fontPath[] = "OPS/font.ttf";
constexpr char manifestPath[] = "META-INF/encryption.xml";
constexpr char packagePath[] = "OPS/package.opf";
const std::string manifest =
    "<encryption><EncryptedData><EncryptionMethod Algorithm='http://ns.adobe.com/pdf/enc#RC'/>"
    "<CipherData><CipherReference URI='OPS/font.ttf'/></CipherData></EncryptedData></encryption>";
const std::string package =
    "<package xmlns='http://www.idpf.org/2007/opf'><metadata>"
    "<dc:identifier xmlns:dc='http://purl.org/dc/elements/1.1/'>"
    "urn:uuid:00112233-4455-6677-8899-aabbccddeeff</dc:identifier></metadata></package>";
std::string payload() {
  std::string result(3107, '\0');
  for (size_t i = 0; i < result.size(); ++i) result[i] = static_cast<char>(i * 37 + 19);
  return result;
}
std::string adobe(const std::string& plain) {
  std::string encoded = plain;
  for (size_t i = 0; i < std::min<size_t>(1024, encoded.size()); ++i)
    encoded[i] ^= static_cast<unsigned char>((i % 16) * 0x11);
  return encoded;
}
std::string archive(const std::string& font, const std::optional<std::string>& encryption = manifest,
                    const std::string& opf = package, bool compressed = false) {
  test_zip::StoredZipWriter writer;
  const auto add = [&](const std::string& name, const std::string& value) {
    if (compressed)
      writer.addDeflated(name, value);
    else
      writer.add(name, value);
  };
  add(fontPath, font);
  if (encryption) add(manifestPath, *encryption);
  add(packagePath, opf);
  return writer.bytes();
}
void install(Epub& book, const std::string& bytes) { Storage.put(book.filepath, {bytes.begin(), bytes.end()}); }
void corruptPayload(std::string& bytes, const std::string& member, size_t offset) {
  size_t position = 0;
  while ((position = bytes.find(std::string("PK\3\4", 4), position)) != std::string::npos) {
    const size_t length =
        static_cast<unsigned char>(bytes[position + 26]) | (static_cast<unsigned char>(bytes[position + 27]) << 8);
    if (bytes.compare(position + 30, length, member) == 0) {
      bytes[position + 30 + length + offset] ^= 1;
      return;
    }
    ++position;
  }
  FAIL() << "Missing synthetic member";
}
class Sink : public Print {
 public:
  std::string bytes;
  size_t limit = SIZE_MAX;
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* data, size_t n) override {
    const size_t accepted = std::min(n, limit - bytes.size());
    bytes.append(reinterpret_cast<const char*>(data), accepted);
    return accepted;
  }
};
// Mirror the manager's staging/publication contract: a failed wrapper may have
// streamed bytes, but those bytes are never a published, reusable font.
bool publish(Epub& book, std::string& published, size_t chunk = 17) {
  Sink candidate;
  if (!book.readFontContentsValidatedToStream(fontPath, candidate, chunk)) return false;
  published = candidate.bytes;
  return true;
}
class EpubFontContentsTest : public ::testing::Test {
  void SetUp() override { Storage.reset(); }
};
}  // namespace

TEST_F(EpubFontContentsTest, AdobeWrapperChecksRawCrcThenDecodesExactlyTheFirst1024Bytes) {
  for (bool compressed : {false, true})
    for (size_t chunk : {1u, 17u, 128u, 1023u, 1024u, 4096u}) {
      Epub book;
      install(book, archive(adobe(payload()), manifest, package, compressed));
      std::string published;
      ASSERT_TRUE(publish(book, published, chunk));
      EXPECT_EQ(published, payload());
      EXPECT_EQ(book.unvalidatedReads, 0u);
      EXPECT_EQ(Storage.reopenWhileOpen, 0u);
    }
}
TEST_F(EpubFontContentsTest, FontSizeAdmissionSeparatesAbsenceFromStorageFailure) {
  for (bool compressed : {false, true}) {
    Epub book;
    install(book, archive(payload(), std::nullopt, package, compressed));
    size_t size = 99;
    EXPECT_EQ(book.getFontItemSize(fontPath, &size), 1);
    EXPECT_EQ(size, payload().size());
    EXPECT_EQ(book.getFontItemSize("OPS/missing.ttf", &size), 0);
    EXPECT_EQ(size, 0u);
    EXPECT_EQ(book.getFontItemSize(fontPath, nullptr), -1);
    EXPECT_EQ(book.getFontItemSize("", &size), 0);
    EXPECT_EQ(book.getFontItemSize(std::string(65536, 'x'), &size), 0);
    Storage.failReadsAt(book.filepath, 50);
    EXPECT_EQ(book.getFontItemSize(fontPath, &size), -1);
    EXPECT_EQ(size, 0u);
    Storage.failReadsAt(book.filepath, SIZE_MAX);
    EXPECT_EQ(book.getFontItemSize(fontPath, &size), 1);
    EXPECT_EQ(size, payload().size());
    EXPECT_EQ(Storage.reopenWhileOpen, 0u);
  }
}
TEST_F(EpubFontContentsTest, MissingAndValidEmptyManifestsKeepPlainFontsButZeroByteManifestFailsClosed) {
  for (auto value : {std::optional<std::string>{}, std::optional<std::string>{"<encryption/>"}}) {
    Epub book;
    install(book, archive(payload(), value));
    std::string published;
    EXPECT_TRUE(publish(book, published));
    EXPECT_EQ(published, payload());
  }
  Epub book;
  install(book, archive(payload(), ""));
  std::string published = "existing";
  EXPECT_FALSE(publish(book, published));
  EXPECT_EQ(published, "existing");
}
TEST_F(EpubFontContentsTest, ProtectedFontSizeUsesPlaintextAndRetriesFailedMeasurement) {
  Epub book;
  install(book, archive(payload(), "<encryption/>"));
  book.decryptor = std::make_unique<FontDecryptorDouble>();
  book.decryptor->member = fontPath;
  book.protectedPlaintextSize = payload().size() - 27;
  size_t size = 0;
  EXPECT_EQ(book.getFontItemSize(fontPath, &size), 1);
  EXPECT_EQ(size, book.protectedPlaintextSize);
  book.protectedSizeFailure = true;
  EXPECT_EQ(book.getFontItemSize(fontPath, &size), -1);
  EXPECT_EQ(size, 0u);
  book.protectedSizeFailure = false;
  EXPECT_EQ(book.getFontItemSize(fontPath, &size), 1);
  EXPECT_EQ(size, book.protectedPlaintextSize);
}
TEST_F(EpubFontContentsTest, ManifestCorruptionNeverBecomesEvidenceOfNoObfuscation) {
  for (bool compressed : {false, true}) {
    auto bytes = archive(adobe(payload()), manifest, package, compressed);
    // Mutate a valid attribute character, so XML alone would accept this data.
    corruptPayload(bytes, manifestPath, (compressed ? 5 : 0) + manifest.find("adobe"));
    Epub book;
    install(book, bytes);
    std::string published = "existing";
    EXPECT_FALSE(publish(book, published));
    EXPECT_EQ(published, "existing");
    EXPECT_EQ(book.unvalidatedReads, 0u);
  }
}
TEST_F(EpubFontContentsTest, PackageCorruptionAndMissingUuidNeverPublishObfuscatedBytes) {
  for (const auto& opf : {std::string("<package><metadata/></package>"), std::string("<package>")}) {
    Epub book;
    install(book, archive(adobe(payload()), manifest, opf));
    std::string published = "existing";
    EXPECT_FALSE(publish(book, published));
    EXPECT_EQ(published, "existing");
  }
  auto bytes = archive(adobe(payload()));
  corruptPayload(bytes, packagePath, package.find("00112233"));
  Epub book;
  install(book, bytes);
  std::string published = "existing";
  EXPECT_FALSE(publish(book, published));
  EXPECT_EQ(published, "existing");
}
TEST_F(EpubFontContentsTest, CorruptFontPayloadAndShortWritesNeverPublishThenFreshRetrySucceeds) {
  for (bool compressed : {false, true}) {
    auto bytes = archive(adobe(payload()), manifest, package, compressed);
    corruptPayload(bytes, fontPath, (compressed ? 5 : 0) + 2000);
    Epub book;
    install(book, bytes);
    std::string published = "existing";
    EXPECT_FALSE(publish(book, published));
    EXPECT_EQ(published, "existing");
    install(book, archive(adobe(payload()), manifest, package, compressed));
    Sink shortSink;
    shortSink.limit = 1025;
    EXPECT_FALSE(book.readFontContentsValidatedToStream(fontPath, shortSink, 173));
    EXPECT_TRUE(publish(book, published));
    EXPECT_EQ(published, payload());
  }
}
TEST_F(EpubFontContentsTest, DirectoryAndManifestReadFailuresAreRetryableAndNeverPublish) {
  Epub book;
  const auto bytes = archive(adobe(payload()));
  install(book, bytes);
  std::string published = "existing";
  Storage.failReadsAt(book.filepath, 50);
  EXPECT_FALSE(publish(book, published));
  EXPECT_EQ(published, "existing");
  Storage.failReadsAt(book.filepath, SIZE_MAX);
  EXPECT_TRUE(publish(book, published));
  EXPECT_EQ(published, payload());
  EXPECT_EQ(Storage.reopenWhileOpen, 0u);
}
TEST_F(EpubFontContentsTest, EveryWrapperReadBoundaryFailsClosedAndCanRetry) {
  // Includes central-directory admission, encryption.xml, OPF and font reads.
  for (bool compressed : {false, true}) {
    const auto bytes = archive(adobe(payload()), manifest, package, compressed);
    Epub baseline;
    install(baseline, bytes);
    std::string success;
    ASSERT_TRUE(publish(baseline, success));
    const size_t readCount = Storage.readCalls(baseline.filepath);
    ASSERT_GT(readCount, 4u);
    for (size_t failCall = 1; failCall <= readCount; ++failCall) {
      SCOPED_TRACE(failCall);
      Epub book;
      install(book, bytes);
      Storage.failReadCall(book.filepath, failCall);
      std::string published = "existing";
      EXPECT_FALSE(publish(book, published));
      EXPECT_EQ(published, "existing");
      Storage.failReadCall(book.filepath, 0);
      EXPECT_TRUE(publish(book, published));
      EXPECT_EQ(published, payload());
    }
  }
}
TEST_F(EpubFontContentsTest, SdkDecryptionRunsOnlyAfterRawMemberValidation) {
  auto encoded = payload();
  for (auto& byte : encoded) byte ^= 0x5a;
  for (bool compressed : {false, true}) {
    Epub book;
    book.decryptor = std::make_unique<FontDecryptorDouble>();
    book.decryptor->member = fontPath;
    auto bytes = archive(encoded, "<encryption/>", package, compressed);
    install(book, bytes);
    std::string published;
    ASSERT_TRUE(publish(book, published));
    EXPECT_EQ(published, payload());
    EXPECT_EQ(book.unvalidatedReads, 1u);
    corruptPayload(bytes, fontPath, (compressed ? 5 : 0) + 1500);
    install(book, bytes);
    published = "existing";
    EXPECT_FALSE(publish(book, published));
    EXPECT_EQ(published, "existing");
    EXPECT_EQ(book.unvalidatedReads, 1u);
  }
}
