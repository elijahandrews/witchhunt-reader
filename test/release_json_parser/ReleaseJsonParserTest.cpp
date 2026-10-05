#include <ReleaseJsonParser.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr char DIGEST_HEX[] = "8745d7dc37c60052691a2ef124663e43ffe60a28bf51500615634058b1d1ab1f";

std::string asset(const std::string& name, const std::string& digestField) {
  return R"({"name":")" + name + R"(","size":6133040,)" + digestField +
         R"("browser_download_url":"https://github.com/o/r/releases/download/2.37/)" + name + R"("})";
}

void feedAll(ReleaseJsonParser& parser, const std::string& json) {
  // A byte at a time, as a stream may split it anywhere.
  for (char c : json) parser.feed(&c, 1);
}

bool sameAsDigestHex(const uint8_t* bytes) {
  for (size_t i = 0; i < 32; ++i) {
    char hex[3];
    snprintf(hex, sizeof(hex), "%02x", bytes[i]);
    if (memcmp(hex, DIGEST_HEX + 2 * i, 2) != 0) return false;
  }
  return true;
}

TEST(ReleaseJsonParserDigest, ReadsTheFirmwareAssetsSha256) {
  ReleaseJsonParser parser;
  const std::string digest = std::string(R"("digest":"sha256:)") + DIGEST_HEX + R"(",)";
  feedAll(parser, R"({"tag_name":"2.37","assets":[)" +
                      asset("firmware-x4pro.bin", R"("digest":"sha256:)" + std::string(64, 'a') + R"(",)") + "," +
                      asset("firmware.bin", digest) + "]}");
  ASSERT_TRUE(parser.foundFirmware());
  EXPECT_EQ(parser.getFirmwareSize(), 6133040u);
  ASSERT_TRUE(parser.hasFirmwareSha256());
  EXPECT_TRUE(sameAsDigestHex(parser.getFirmwareSha256()));
}

TEST(ReleaseJsonParserDigest, AnotherAssetsDigestIsNotTaken) {
  ReleaseJsonParser parser;
  feedAll(parser, R"({"tag_name":"2.37","assets":[)" +
                      asset("firmware-x4pro.bin", std::string(R"("digest":"sha256:)") + DIGEST_HEX + R"(",)") + "," +
                      asset("firmware.bin", "") + "]}");
  ASSERT_TRUE(parser.foundFirmware());
  EXPECT_FALSE(parser.hasFirmwareSha256());
}

TEST(ReleaseJsonParserDigest, ANullOrMalformedDigestIsNone) {
  for (const std::string& field : {std::string(R"("digest":null,)"), std::string(R"("digest":"sha512:abcd",)"),
                                   std::string(R"("digest":"sha256:)") + std::string(63, 'a') + R"(",)",
                                   std::string(R"("digest":"sha256:)") + std::string(63, 'a') + R"(g",)"}) {
    ReleaseJsonParser parser;
    feedAll(parser, R"({"tag_name":"2.37","assets":[)" + asset("firmware.bin", field) + "]}");
    ASSERT_TRUE(parser.foundFirmware()) << field;
    EXPECT_FALSE(parser.hasFirmwareSha256()) << field;
  }
}

TEST(ReleaseJsonParserDigest, TheBetaListFormCarriesTheDigestToo) {
  ReleaseJsonParser parser;
  feedAll(parser, R"([{"tag_name":"2.38.0-rc.1","assets":[)" +
                      asset("firmware.bin", std::string(R"("digest":"sha256:)") + DIGEST_HEX + R"(",)") + "]}]");
  ASSERT_TRUE(parser.foundFirmware());
  EXPECT_STREQ(parser.getTagName(), "2.38.0-rc.1");
  ASSERT_TRUE(parser.hasFirmwareSha256());
  EXPECT_TRUE(sameAsDigestHex(parser.getFirmwareSha256()));
}

TEST(ReleaseJsonParserDigest, ResetForgetsTheDigest) {
  ReleaseJsonParser parser;
  feedAll(parser, R"({"tag_name":"2.37","assets":[)" +
                      asset("firmware.bin", std::string(R"("digest":"sha256:)") + DIGEST_HEX + R"(",)") + "]}");
  ASSERT_TRUE(parser.hasFirmwareSha256());
  parser.reset();
  EXPECT_FALSE(parser.hasFirmwareSha256());
}

}  // namespace
