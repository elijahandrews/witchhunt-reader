#include <FsHelpers.h>
#include <Logging.h>
#include <ZipFile.h>

#include "Epub.h"
#include "Epub/parsers/AdobeFontObfuscation.h"

int Epub::getFontItemSize(const std::string& itemHref, size_t* size) const {
  if (!size) return -1;
  *size = 0;
  const auto path = FsHelpers::normalisePath(itemHref);
  if (path.empty() || path.size() > UINT16_MAX) return 0;
  ZipFile zip(filepath);
  const ZipFile::EntryTarget target{ZipFile::fnvHash64(path.data(), path.size()),
                                    static_cast<uint16_t>(path.size()), 0, path.c_str()};
  ZipFile::EntryIdentity identity;
  const int found = zip.fillEntryIdentities(&target, 1, &identity, 1);
  if (found < 0) return -1;
  if (!found || !identity.found) return 0;
  if (decryptor && decryptor->isEncrypted(path)) {
    // Protected resources may have a different decrypted length; the ZIP
    // identity only establishes presence and validates directory I/O here.
    return getItemSize(path, size) ? 1 : -1;
  }
  *size = identity.uncompressedSize;
  return 1;
}

bool Epub::readFontContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize) const {
  return readFontContentsToStreamImpl(itemHref, out, chunkSize, false);
}

bool Epub::readFontContentsValidatedToStream(const std::string& itemHref, Print& out, size_t chunkSize) const {
  return readFontContentsToStreamImpl(itemHref, out, chunkSize, true);
}

bool Epub::readFontContentsToStreamImpl(const std::string& itemHref, Print& out, size_t chunkSize,
                                      bool validate) const {
  const std::string path = FsHelpers::normalisePath(itemHref);
  auto streamMember = [&](const std::string& member, Print& destination, size_t chunk) {
    if (!validate) return readItemContentsToStream(member, destination, chunk);
    ZipFile zip(filepath, &zipDirectoryCache);
    if (decryptor && decryptor->isEncrypted(member)) {
      // ZIP CRC describes the encrypted member, not the decrypted font. Verify
      // that raw stream first; the SDK then performs its normal decryption.
      class Discard final : public Print {
       public:
        size_t write(uint8_t) override { return 1; }
        size_t write(const uint8_t*, size_t size) override { return size; }
      } discard;
      return zip.readFileToStreamValidated(member.c_str(), discard, chunk) &&
             readItemContentsToStream(member, destination, chunk);
    }
    return zip.readFileToStreamValidated(member.c_str(), destination, chunk);
  };
  const auto streamFont = [&](Print& destination) { return streamMember(path, destination, chunkSize); };
  {
    constexpr char manifestPath[] = "META-INF/encryption.xml";
    if (validate) {
      // A failed directory read is retryable, not proof the manifest is absent.
      ZipFile zip(filepath);
      const ZipFile::EntryTarget target{ZipFile::fnvHash64(manifestPath, sizeof(manifestPath) - 1),
                                        sizeof(manifestPath) - 1, 0, manifestPath};
      ZipFile::EntryIdentity identity;
      const int found = zip.fillEntryIdentities(&target, 1, &identity, 1);
      if (found < 0) { LOG_ERR("EBP", "Cannot inspect font obfuscation manifest"); return false; }
      if (found == 0) return streamFont(out);
    } else {
      size_t size = 0;
      if (!getItemSize(manifestPath, &size)) return streamFont(out);
    }
    AdobeFontObfuscation manifest(&path);
    if (!manifest.setup() || !streamMember(manifestPath, manifest, 512) ||
        !manifest.finish()) {
      LOG_ERR("EBP", "Cannot read font obfuscation manifest");
      return false;
    }
    if (!manifest.isAdobeFont()) return streamFont(out);
  }

  AdobeFontObfuscation::Key key{};
  {
    // Read independently of the metadata cache so previously opened books also
    // work, without adding persistent UUID state or changing the cache format.
    std::string opfPath;
    if (!findContentOpfFile(&opfPath)) return false;
    AdobeFontObfuscation package;
    if (!package.setup() || !streamMember(opfPath, package, 512) || !package.finish() ||
        !package.hasKey()) {
      LOG_ERR("EBP", "Cannot find UUID for Adobe-obfuscated font: %s", path.c_str());
      return false;
    }
    key = package.key();
  }
  AdobeFontStream font(out, key);
  return streamFont(font);
}
