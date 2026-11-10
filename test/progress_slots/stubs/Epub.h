#pragma once
#include <string>
class Epub {
 public:
  std::string getWitchCachePath() const { return "/witch"; }
  const std::string& getCachePath() const {
    static const std::string path = "/epub";
    return path;
  }
};
