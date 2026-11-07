#pragma once
#include <HalStorage.h>
#include <activities/UiListActivity.h>

#include <functional>
#include <map>
#include <string>
#include <vector>
class HttpDownloader {
 public:
  enum DownloadError { OK, HTTP_ERROR, FILE_ERROR, ABORTED };
  enum class TlsPolicy { Verified, Strict, Unverified };
  struct Session {
    std::string ca;
    explicit Session(const char* value) : ca(value) {}
  };
  inline static std::map<std::string, std::string> responses;
  inline static std::vector<std::string> requests;
  inline static bool abortBook = false, corruptBook = false, cancelBook = false;
  static DownloadError downloadToFile(Session& s, const std::string& url, const std::string& dest,
                                      std::function<bool(unsigned, unsigned)> progress, const std::string& user,
                                      const std::string& token, TlsPolicy tls) {
    if (s.ca.empty() || tls != TlsPolicy::Strict || user != "reader" || token.size() != 64) return HTTP_ERROR;
    requests.push_back(url);
    const auto found = responses.find(url);
    if (found == responses.end()) return HTTP_ERROR;
    auto body = found->second;
    const bool isBook = url.find("/v1/files/") != url.npos;
    if (isBook && corruptBook) body.back() ^= 1;
    Storage.put(dest + ".part", body.substr(0, body.size() / 2));
    if (isBook && cancelBook) injectCancel = true;
    if (!progress(body.size(), body.size()) || (isBook && abortBook)) return ABORTED;
    Storage.put(dest, body);
    Storage.remove((dest + ".part").c_str());
    return OK;
  }
};
