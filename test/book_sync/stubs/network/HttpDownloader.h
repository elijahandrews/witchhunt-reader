#pragma once
#include <HalStorage.h>
#include <activities/Activity.h>

#include <functional>
#include <map>
#include <string>
#include <vector>
class HttpDownloader {
 public:
  enum DownloadError { OK, HTTP_ERROR, FILE_ERROR, ABORTED };
  struct Session {};
  struct DownloadOptions {
    Session* session = nullptr;
    std::function<bool()> shouldCancel;
    const char* privateCa = nullptr;
    unsigned maxRedirects = 0;
    std::string_view authorizationOrigin;
    size_t maxBytes = 0, expectedBytes = 0;
    DownloadOptions(bool, bool, std::function<bool()> cancel, size_t) : shouldCancel(cancel) {}
  };
  inline static std::map<std::string, std::string> responses;
  inline static std::vector<std::string> requests;
  inline static bool abortBook = false, corruptBook = false, cancelBook = false;
  static DownloadError downloadToFile(const std::string& url, const std::string& dest, std::nullptr_t, bool*,
                                      const std::string& user, const std::string& token, DownloadOptions options) {
    if (!options.privateCa || !*options.privateCa || options.maxRedirects || user != "reader" || token.size() != 64)
      return HTTP_ERROR;
    requests.push_back(url);
    const auto found = responses.find(url);
    if (found == responses.end()) return HTTP_ERROR;
    auto body = found->second;
    const bool isBook = url.find("/v1/files/") != url.npos;
    if (isBook && corruptBook) body.back() ^= 1;
    Storage.put(dest + ".part", body.substr(0, body.size() / 2));
    if (isBook && cancelBook) injectCancel = true;
    if (options.shouldCancel() || body.size() > options.maxBytes ||
        (options.expectedBytes && body.size() != options.expectedBytes) || (isBook && abortBook))
      return ABORTED;
    Storage.put(dest, body);
    Storage.remove((dest + ".part").c_str());
    return OK;
  }
};
