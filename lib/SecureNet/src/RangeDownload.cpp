#include "RangeDownload.h"

#include <Arduino.h>  // millis()
#include <Logging.h>

#include <algorithm>

namespace crosspoint {

namespace {

namespace hr = http_range;
using C = SecureHttpClient;

// The response as one streamed body: every byte the server sends goes to the sink.
int streamGet(SecureHttpClient& http, const std::string& url, DownloadSink& sink) {
  auto body = [&](const uint8_t* data, size_t len) -> bool {
    if (!sink.write(data, len)) return false;  // abort
    sink.downloaded += len;
    if (sink.progress && sink.total > 0) {
      if (!sink.progress(sink.downloaded, sink.total)) return false;
    }
    return true;
  };
  auto progress = [&](size_t /*downloaded*/, size_t total) -> bool {
    sink.total = total;
    return true;
  };
  return http.get(url, body, progress);
}

bool isTransportFailure(int rc) {
  return rc == C::ERR_CONNECT || rc == C::ERR_SEND || rc == C::ERR_TIMEOUT || rc == C::ERR_TRUNCATED;
}

// Fetches url in Range requests, so that no response, and so no TLS record, is larger than a chunk
// plus its headers (HttpRange.h has why and the sizing). Each request is sized from the largest free
// block just before it, starting from firstLargest, which the caller measured, and never above the
// session's ceiling. An out-of-memory read lowers that ceiling for good. Returns 200 once the whole
// file has been written, else the failing HTTP status or SecureHttpError.
//
// The handling of a server that ignores Range (200 instead of 206: rewind the sink, take the whole
// body) and the resume from the bytes already received are adapted from Free-Ink/freeink-sdk f80a99c
// (ResumableFetch.h, Justin Mitchell). Different here: every request is a bounded range on the same
// kept-alive connection, the total comes from Content-Range, and chunks after the first go straight
// to the URL the first one was redirected to.
int chunkedGet(SecureHttpClient& http, const std::string& url, DownloadSink& sink, size_t firstLargest,
               ChunkSession& session, LargestFreeBlockFn largestFreeBlock) {
  struct RequestModeGuard {
    C& client;
    ~RequestModeGuard() {
      client.clearRange();
      client.setQuietRequests(false);
    }
  } guard{http};
  http.setQuietRequests(true);

  const unsigned long startMs = millis();
  std::string target = url;
  size_t offset = 0;
  bool totalKnown = false;
  size_t total = 0;
  unsigned requests = 0;
  bool wholeBody = false;
  hr::ChunkSizer sizer(session.ceiling);
  hr::RetryBudget retries;
  size_t chunkSize = sizer.next(firstLargest);
  size_t smallest = chunkSize;
  size_t largestUsed = chunkSize;
  bool outOfMemory = false;

  hr::Chunk chunk;
  while (true) {
    if (requests > 0) {
      // The heap fragments as the download runs: size this request from the block there is now.
      const size_t largest = largestFreeBlock();
      const size_t previous = chunkSize;
      chunkSize = sizer.next(largest);
      if (chunkSize != previous) {
        LOG_DBG("HTTP", "chunk size %u -> %u B at %u B (largest free block %u B%s)", static_cast<unsigned>(previous),
                static_cast<unsigned>(chunkSize), static_cast<unsigned>(offset), static_cast<unsigned>(largest),
                outOfMemory ? ", after an out-of-memory read" : "");
      }
      smallest = std::min(smallest, chunkSize);
      largestUsed = std::max(largestUsed, chunkSize);
      outOfMemory = false;
    }
    if (!hr::nextChunk(offset, chunkSize, totalKnown, total, chunk)) break;
    http.setRange(chunk.first, chunk.last);
    hr::RangeReply reply = hr::RangeReply::Error;
    bool classified = false;
    size_t received = 0;
    auto body = [&](const uint8_t* data, size_t len) -> bool {
      if (!classified) {
        classified = true;
        reply = hr::classifyReply(http.lastStatus(), http.lastHasContentRange(), http.lastContentRange(), chunk.first);
        if (reply == hr::RangeReply::WholeBody) {
          // The server ignored Range: this body is the whole file from byte 0.
          if (offset > 0 && (!sink.rewind || !sink.rewind())) return false;
          offset = 0;
          totalKnown = http.lastContentLength() >= 0;
          total = totalKnown ? static_cast<size_t>(http.lastContentLength()) : 0;
        } else if (reply == hr::RangeReply::Partial && http.lastContentRange().totalKnown) {
          totalKnown = true;
          total = http.lastContentRange().total;
        }
      }
      // An error page (or a range we did not ask for) is drained, never written into the file.
      if (reply != hr::RangeReply::Partial && reply != hr::RangeReply::WholeBody) return true;
      if (!sink.write(data, len)) return false;
      offset += len;
      received += len;
      sink.downloaded = offset;
      sink.total = totalKnown ? total : 0;
      if (sink.progress && sink.total > 0 && !sink.progress(sink.downloaded, sink.total)) return false;
      return true;
    };

    const int rc = http.get(target, body, nullptr);
    ++requests;
    if (rc == C::ERR_ABORTED) return rc;
    if (rc < 0) {
      // The connection broke mid-chunk. What was written stays; reconnect (a resumed TLS session,
      // so no certificate chain) and continue from there. A record that did not fit the heap makes
      // the next requests smaller, whatever the largest block then reads.
      outOfMemory = http.lastReadOutOfMemory();
      if (outOfMemory) {
        sizer.onOutOfMemory();
        if (sizer.ceiling() != session.ceiling) {
          LOG_DBG("HTTP", "chunk ceiling %u -> %u B for the rest of the session (out-of-memory read at %u B)",
                  static_cast<unsigned>(session.ceiling), static_cast<unsigned>(sizer.ceiling()),
                  static_cast<unsigned>(offset));
          session.ceiling = sizer.ceiling();
        }
      }
      if (!isTransportFailure(rc) || !retries.reconnectAfterFailure(received > 0, totalKnown ? total : offset)) {
        LOG_ERR("HTTP", "chunked download stopped at %u B after %u requests, %u reconnect(s): rc=%d%s",
                static_cast<unsigned>(offset), requests, retries.reconnects(), rc,
                outOfMemory ? " (out of memory)" : "");
        return rc;
      }
      http.close();
      http.open(target);  // measured at the top of the loop with the new connection in place
      continue;
    }
    if (!classified) {  // a response without a body: a 416, or an empty one
      reply = hr::classifyReply(rc, http.lastHasContentRange(), http.lastContentRange(), chunk.first);
    }
    if (reply == hr::RangeReply::WholeBody) {
      wholeBody = true;
      break;
    }
    if (reply == hr::RangeReply::Unsatisfiable && offset > 0 && (!totalKnown || offset == total)) {
      break;  // the previous chunk ended exactly at the end of a file of unknown size
    }
    if (reply != hr::RangeReply::Partial) {
      LOG_ERR("HTTP", "chunked download: unexpected reply %d to bytes %u-%u", rc, static_cast<unsigned>(chunk.first),
              static_cast<unsigned>(chunk.last));
      return rc;
    }
    target = http.lastUrl();  // follow a redirect once, not once per chunk
    if (received > 0) retries.onProgress();
    if (hr::transferComplete(offset, totalKnown, total, chunk.length(), received)) break;
  }

  LOG_DBG("HTTP", "chunked download done: %u B in %u requests, %lu ms, %u reconnect(s), chunks %u-%u B%s",
          static_cast<unsigned>(offset), requests, millis() - startMs, retries.reconnects(),
          static_cast<unsigned>(smallest), static_cast<unsigned>(largestUsed),
          wholeBody ? " (server ignored Range: whole file streamed)" : "");
  return 200;
}

}  // namespace

int downloadToSink(SecureHttpClient& http, const std::string& url, DownloadSink& sink, ChunkSession& session,
                   LargestFreeBlockFn largestFreeBlock) {
  if (!sink.rewind) return streamGet(http, url, sink);
  if (url.compare(0, 8, "https://") != 0) {
    LOG_DBG("HTTP", "download mode: streamed (plain http)");
    return streamGet(http, url, sink);
  }
  if (!http.open(url)) return C::ERR_CONNECT;
  const size_t largest = largestFreeBlock();
  if (!hr::shouldChunk(largest)) {
    LOG_DBG("HTTP", "download mode: streamed (largest free block %u B)", static_cast<unsigned>(largest));
    return streamGet(http, url, sink);
  }
  LOG_DBG("HTTP", "download mode: chunked %u B (largest free block %u B, ceiling %u B)",
          static_cast<unsigned>(hr::ChunkSizer(session.ceiling).next(largest)), static_cast<unsigned>(largest),
          static_cast<unsigned>(session.ceiling));
  return chunkedGet(http, url, sink, largest, session, largestFreeBlock);
}

}  // namespace crosspoint
