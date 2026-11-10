#include <ZipFile.h>
#include <gtest/gtest.h>
#include "StoredZipWriter.h"

namespace {
const std::string path = "/font-book.epub";
constexpr char name[] = "OPS/font.ttf";
class Sink : public Print {
 public:
  std::string bytes;
  size_t limit = SIZE_MAX, maxChunk = 0;
  bool transform = false;
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* data, size_t n) override {
    maxChunk = std::max(maxChunk, n);
    const auto count = std::min(n, limit - bytes.size());
    for (size_t i = 0; i < count; ++i) bytes.push_back(data[i] ^ (transform ? 0x5a : 0));
    return count;
  }
};
std::string fixture(bool deflated, const std::string& data) {
  test_zip::StoredZipWriter writer;
  if (deflated) writer.addDeflated(name, data); else writer.add(name, data);
  return writer.bytes();
}
void install(const std::string& bytes) { Storage.put(path, {bytes.begin(), bytes.end()}); }
class ZipValidationTest : public ::testing::Test {
  void SetUp() override { Storage.reset(); }
};
}
TEST_F(ZipValidationTest, StoredAndDeflatedPayloadsValidateAtEveryChunkBoundary) {
  std::string data;
  for (int i = 0; i < 17000; ++i) data.push_back(static_cast<char>(i * 71));
  for (bool compressed : {false, true}) for (size_t chunk : {1u, 7u, 256u, 4096u, 100000u}) {
    install(fixture(compressed, data));
    ZipFile zip(path); Sink sink;
    ASSERT_TRUE(zip.readFileToStreamValidated(name, sink, chunk));
    EXPECT_EQ(sink.bytes, data);
    EXPECT_LE(sink.maxChunk, 4096u);
    EXPECT_EQ(Storage.reopenWhileOpen, 0u);
  }
}
TEST_F(ZipValidationTest, SameLengthCorruptionIsRejectedBeforePublication) {
  for (bool compressed : {false, true}) {
    auto bytes = fixture(compressed, std::string(7000, 'a'));
    bytes[30 + sizeof(name) - 1 + (compressed ? 5 : 0) + 123] ^= 0x20;
    install(bytes);
    ZipFile zip(path); Sink sink;
    EXPECT_FALSE(zip.readFileToStreamValidated(name, sink, 256));
    EXPECT_EQ(Storage.reopenWhileOpen, 0u);
  }
}
TEST_F(ZipValidationTest, ChecksRawBytesBeforeTransformingSink) {
  const std::string data(3000, 'f'); install(fixture(true, data));
  ZipFile zip(path); Sink sink; sink.transform = true;
  ASSERT_TRUE(zip.readFileToStreamValidated(name, sink, 19));
  EXPECT_EQ(sink.bytes, std::string(data.size(), 'f' ^ 0x5a));
}
TEST_F(ZipValidationTest, CentralDirectoryCrcAndSizeAreBothRequired) {
  for (size_t field : {16u, 24u}) {
    auto bytes = fixture(true, std::string(2000, 'a'));
    const auto central = bytes.find(std::string("PK\1\2", 4));
    ASSERT_NE(central, std::string::npos);
    bytes[central + field] ^= 1;
    install(bytes);
    ZipFile zip(path); Sink sink;
    EXPECT_FALSE(zip.readFileToStreamValidated(name, sink, 19));
  }
}
TEST_F(ZipValidationTest, ShortWritesAndReadErrorsCanRetryWithFreshStream) {
  for (bool compressed : {false, true}) {
    install(fixture(compressed, std::string(6000, 'z')));
    ZipFile zip(path); Sink shortSink; shortSink.limit = 300;
    EXPECT_FALSE(zip.readFileToStreamValidated(name, shortSink, 256));
    Sink retry;
    EXPECT_TRUE(zip.readFileToStreamValidated(name, retry, 256));
    Storage.failReadsAt(path, 100);
    Sink failed;
    EXPECT_FALSE(zip.readFileToStreamValidated(name, failed, 256));
    Storage.failReadsAt(path, SIZE_MAX);
    Sink recovered;
    EXPECT_TRUE(zip.readFileToStreamValidated(name, recovered, 256));
    EXPECT_EQ(Storage.reopenWhileOpen, 0u);
  }
}
TEST_F(ZipValidationTest, MissingTruncatedAndInvalidRequestsFail) {
  install(fixture(false, "font"));
  ZipFile zip(path); Sink sink;
  EXPECT_FALSE(zip.readFileToStreamValidated("missing", sink, 256));
  EXPECT_FALSE(zip.readFileToStreamValidated(name, sink, 0));
  EXPECT_FALSE(zip.readFileToStreamValidated(nullptr, sink, 256));
  EXPECT_FALSE(zip.readFileToStreamValidated("", sink, 256));
  auto bytes = fixture(false, "font"); bytes.resize(bytes.size() - 10); install(bytes);
  EXPECT_FALSE(zip.readFileToStreamValidated(name, sink, 256));
  EXPECT_TRUE(sink.bytes.empty());
}
TEST_F(ZipValidationTest, EmptyEntryRequiresItsCorrectCrc) {
  for (bool compressed : {false, true}) {
    install(fixture(compressed, "")); ZipFile zip(path); Sink sink;
    EXPECT_TRUE(zip.readFileToStreamValidated(name, sink, 256));
    EXPECT_TRUE(sink.bytes.empty());
  }
}
