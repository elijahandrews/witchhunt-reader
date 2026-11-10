#pragma once
#include <ZipFile.h>

#include <memory>
#include <string>

// A transparent SDK-boundary double: it makes a visible XOR transform only on
// the selected font member. The production wrapper must CRC-check the raw ZIP
// stream before it calls this unvalidated SDK decryption path.
struct FontDecryptorDouble {
  std::string member;
  bool isEncrypted(const std::string& path) const { return path == member; }
};
class Epub {
 public:
  std::string filepath = "/font-wrapper.epub";
  mutable ZipDirectoryCache zipDirectoryCache;
  std::unique_ptr<FontDecryptorDouble> decryptor;
  mutable unsigned unvalidatedReads = 0;
  bool opfAvailable = true;
  size_t protectedPlaintextSize = 0;
  bool protectedSizeFailure = false;
  int getFontItemSize(const std::string&, size_t*) const;
  bool readFontContentsToStream(const std::string&, Print&, size_t) const;
  bool readFontContentsValidatedToStream(const std::string&, Print&, size_t) const;
  bool readFontContentsToStreamImpl(const std::string&, Print&, size_t, bool) const;
  bool readItemContentsToStream(const std::string& member, Print& out, size_t chunk) const {
    ++unvalidatedReads;
    class Transform final : public Print {
     public:
      Print& sink;
      explicit Transform(Print& sink) : sink(sink) {}
      size_t write(uint8_t value) override { return sink.write(value ^ 0x5a); }
      size_t write(const uint8_t* bytes, size_t n) override {
        size_t written = 0;
        for (; written < n && write(bytes[written]); ++written) {
        }
        return written;
      }
    } transformed(out);
    ZipFile zip(filepath, &zipDirectoryCache);
    Print& destination = decryptor && decryptor->isEncrypted(member) ? static_cast<Print&>(transformed) : out;
    return zip.readFileToStream(member.c_str(), destination, chunk);
  }
  bool getItemSize(const std::string& member, size_t* size) const {
    if (decryptor && decryptor->isEncrypted(member)) {
      if (protectedSizeFailure) return false;
      *size = protectedPlaintextSize;
      return true;
    }
    ZipFile zip(filepath, &zipDirectoryCache);
    return zip.getInflatedFileSize(member.c_str(), size);
  }
  bool findContentOpfFile(std::string* path) const {
    *path = "OPS/package.opf";
    return opfAvailable;
  }
};
