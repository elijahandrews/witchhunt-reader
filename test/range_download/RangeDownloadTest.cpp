// Host tests for the download loop (lib/SecureNet RangeDownload) over a scripted network.
//
// The harness is ported from Free-Ink/freeink-sdk f80a99c (libs/network/SecureNet/test/host,
// Justin Mitchell): a fake WiFiClient with scripted server replies and a virtual clock (stubs/).
// Adapted here to SecureHttpClient's keep-alive and to https: the stub SecureClient below runs over
// the same fake transport, without TLS, and reports an out-of-memory read as wolfSSL's MEMORY_E.

#include <HttpRange.h>
#include <RangeDownload.h>
#include <SecureClient.h>
#include <SecureHttpClient.h>
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace crosspoint {
namespace {
constexpr int FAKE_MEMORY_E = -125;  // wolfSSL MEMORY_E
}  // namespace

SecureClient::~SecureClient() { stop(); }
bool SecureClient::tls13Available() { return false; }
void SecureClient::clearSessionCache() {}
int SecureClient::connect(IPAddress, uint16_t) { return 0; }
int SecureClient::connect(const char* host, uint16_t port) {
  _lastReadErr = 0;
  _connected = _transport.connect(host, port) == 1;
  return _connected ? 1 : 0;
}
size_t SecureClient::write(uint8_t b) { return write(&b, 1); }
size_t SecureClient::write(const uint8_t* buf, size_t size) { return _connected ? _transport.write(buf, size) : 0; }
int SecureClient::available() { return _connected ? _transport.available() : 0; }
int SecureClient::read() {
  uint8_t b;
  return read(&b, 1) == 1 ? b : -1;
}
// Like the real read(): data, 0 for "nothing yet" or a peer close (then disconnected), -1 for a hard
// failure, here only the scripted out-of-memory one.
int SecureClient::read(uint8_t* buf, size_t size) {
  if (!_connected) return -1;
  if (_transport.available() > 0) return _transport.read(buf, size);
  if (_transport.outOfMemoryNow()) {
    _lastReadErr = FAKE_MEMORY_E;
    _connected = false;
    return -1;
  }
  if (!_transport.connected()) _connected = false;
  return 0;
}
int SecureClient::peek() { return -1; }
void SecureClient::flush() {}
void SecureClient::stop() {
  _transport.stop();
  _connected = false;
}
uint8_t SecureClient::connected() { return _connected && _transport.connected(); }
bool SecureClient::lastReadWasOutOfMemory() const { return _lastReadErr == FAKE_MEMORY_E; }
}  // namespace crosspoint

namespace hr = crosspoint::http_range;

namespace {

size_t g_largest = 16372;  // the X3's largest free block after a resumed handshake
size_t fakeLargest() { return g_largest; }

std::string makeResource(size_t size, char seed = 'a') {
  std::string s(size, '\0');
  for (size_t i = 0; i < size; ++i) s[i] = static_cast<char>(seed + (i * 7 + i / 251) % 26);
  return s;
}

// "bytes=a-b" of request i, or "" when it had no Range header.
std::string rangeOf(size_t i) {
  const std::string& r = FakeNet::requests().at(i);
  const size_t at = r.find("Range: ");
  if (at == std::string::npos) return "";
  return r.substr(at + 7, r.find("\r\n", at) - at - 7);
}

struct Fixture : ::testing::Test {
  crosspoint::SecureHttpClient http;
  crosspoint::ChunkSession session;
  std::string file;
  int rewinds = 0;
  std::pair<size_t, size_t> lastProgress{0, 0};

  void SetUp() override {
    FakeNet::reset();
    g_largest = 16372;
    http.setTimeout(5000);
  }

  void serve(const std::string& resource) {
    FakeNet::resource() = resource;
    FakeNet::serving() = true;
  }

  crosspoint::DownloadSink fileSink() {
    file.clear();
    crosspoint::DownloadSink sink;
    sink.write = [this](const uint8_t* d, size_t n) {
      file.append(reinterpret_cast<const char*>(d), n);
      return true;
    };
    sink.rewind = [this] {
      ++rewinds;
      file.clear();
      return true;
    };
    sink.progress = [this](size_t done, size_t total) {
      lastProgress = {done, total};
      return true;
    };
    return sink;
  }

  int download(const std::string& url = "https://raw.example/fonts/f.cpfont") {
    crosspoint::DownloadSink sink = fileSink();
    return crosspoint::downloadToSink(http, url, sink, session, &fakeLargest);
  }

  // The size the next chunked request will use, given the session's ceiling and g_largest.
  size_t firstChunk() const { return hr::ChunkSizer(session.ceiling).next(g_largest); }
};

TEST_F(Fixture, ChunksAFileOverOneKeptAliveConnection) {
  const std::string resource = makeResource(20000);
  serve(resource);
  const size_t c = firstChunk();

  EXPECT_EQ(download(), 200);
  EXPECT_EQ(file, resource);
  EXPECT_EQ(FakeNet::connects(), 1);
  const size_t expectedRequests = (resource.size() + c - 1) / c;
  ASSERT_EQ(FakeNet::requests().size(), expectedRequests);
  for (size_t i = 0; i < expectedRequests; ++i) {
    const size_t first = i * c;
    const size_t last = std::min(first + c, resource.size()) - 1;
    EXPECT_EQ(rangeOf(i), "bytes=" + std::to_string(first) + "-" + std::to_string(last)) << i;
  }
  EXPECT_EQ(lastProgress, std::make_pair(resource.size(), resource.size()));  // whole-file progress
}

TEST_F(Fixture, StreamsWhenTheLargestBlockIsLarge) {
  const std::string resource = makeResource(20000);
  serve(resource);
  g_largest = 64 * 1024;
  EXPECT_EQ(download(), 200);
  EXPECT_EQ(file, resource);
  ASSERT_EQ(FakeNet::requests().size(), 1u);
  EXPECT_EQ(rangeOf(0), "");
}

TEST_F(Fixture, StreamsPlainHttp) {
  const std::string resource = makeResource(20000);
  serve(resource);
  EXPECT_EQ(download("http://opds.example/book.epub"), 200);
  EXPECT_EQ(file, resource);
  ASSERT_EQ(FakeNet::requests().size(), 1u);
  EXPECT_EQ(rangeOf(0), "");
}

TEST_F(Fixture, AnOutOfMemoryReadReconnectsAndResumesWithASmallerChunk) {
  const std::string resource = makeResource(30000);
  serve(resource);
  const size_t c = firstChunk();
  hr::ChunkSizer probe(session.ceiling);
  probe.next(g_largest);
  probe.onOutOfMemory();
  const size_t lower = probe.ceiling();
  ASSERT_LT(lower, c);
  // The first chunk delivers 1000 bytes, then its next record cannot be allocated.
  FakeNet::faults()[0] = {1000, FakeReply::End::OutOfMemory};

  EXPECT_EQ(download(), 200);
  EXPECT_EQ(file, resource);
  EXPECT_EQ(FakeNet::connects(), 2);  // one reconnect
  // Resumed at exactly the bytes written, one size smaller, and the session remembers it.
  EXPECT_EQ(rangeOf(1), "bytes=1000-" + std::to_string(1000 + lower - 1));
  EXPECT_EQ(session.ceiling, lower);
}

TEST_F(Fixture, TheCeilingCarriesToTheNextFileOfTheSession) {
  serve(makeResource(20000));
  FakeNet::faults()[0] = {1000, FakeReply::End::OutOfMemory};
  ASSERT_EQ(download(), 200);
  const size_t lowered = session.ceiling;
  ASSERT_LT(lowered, hr::ChunkSizer().next(g_largest));

  // The second file, on the same session and connection, starts at the lowered size although the
  // largest block reads as before.
  const size_t before = FakeNet::requests().size();
  const std::string second = makeResource(15000, 'k');
  FakeNet::resource() = second;
  EXPECT_EQ(download("https://raw.example/fonts/g.cpfont"), 200);
  EXPECT_EQ(file, second);
  EXPECT_EQ(rangeOf(before), "bytes=0-" + std::to_string(lowered - 1));
}

TEST_F(Fixture, StallsInARowEndTheDownload) {
  serve(makeResource(20000));
  // Every request gets its headers and then the connection drops before any body byte.
  for (size_t i = 0; i < 50; ++i) FakeNet::faults()[i] = {0, FakeReply::End::Close};

  EXPECT_LT(download(), 0);
  // The first failure and MAX_STALLED_RETRIES retries, then it gives up.
  EXPECT_EQ(FakeNet::requests().size(), 1u + hr::MAX_STALLED_RETRIES);
  EXPECT_TRUE(file.empty());
}

TEST_F(Fixture, FailuresThatMakeProgressKeepGoing) {
  const std::string resource = makeResource(30000);
  serve(resource);
  // Every second request drops after 500 body bytes: progress each time, so no stall is counted.
  for (size_t i = 0; i < 40; i += 2) FakeNet::faults()[i] = {500, FakeReply::End::Close};

  EXPECT_EQ(download(), 200);
  EXPECT_EQ(file, resource);
}

}  // namespace
