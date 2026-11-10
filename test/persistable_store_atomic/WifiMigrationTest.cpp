#include <HalStorage.h>
#include <ObfuscationUtils.h>
#include <WifiCredentialStore.h>

#include <cassert>
#include <cstring>
// Deterministic reversible encoding: test store migration, not device-key crypto.
namespace obfuscation {
String obfuscateToBase64(const std::string& p) { return "encoded:" + p; }
std::string deobfuscateFromBase64(const char* p, size_t max, DecodeStatus* status) {
  std::string value(p);
  if (value.rfind("encoded:", 0) != 0) {
    *status = DecodeStatus::INVALID;
    return {};
  }
  value.erase(0, 8);
  *status = value.size() > max ? DecodeStatus::TOO_LONG : DecodeStatus::VALIDATED;
  return value.size() > max ? std::string{} : value;
}
}  // namespace obfuscation
int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string legacy =
      R"({"lastConnectedSsid":"fixture-net","credentials":[{"ssid":"fixture-net","password":"fixture-secret"}],"witchOnly":42})";
  const char* oldPath = strcmp(argv[1], "backup") == 0 ? "/.crosspoint/wifi.json.bak" : "/.crosspoint/wifi.json";
  const char* ownPath = WifiCredentialStore::getFilePath();
  Storage.put(oldPath, legacy);
  const bool corrupt = strcmp(argv[1], "corrupt") == 0;
  const bool current = strcmp(argv[1], "current") == 0;
  if (corrupt) Storage.put(ownPath, "{broken");
  if (current) Storage.put(ownPath, R"({"credentials":[{"ssid":"new-net","password_obf":"encoded:new-secret"}]})");
  auto& store = WifiCredentialStore::getInstance();
  if (corrupt)
    assert(store.getCredentialCount() == 0);
  else if (current) {
    assert(store.findCredential("new-net")->password == "new-secret");
    assert(!store.findCredential("fixture-net"));
  } else {
    assert(store.findCredential("fixture-net")->password == "fixture-secret");
    assert(store.getLastConnectedSsid() == "fixture-net");
    assert(Storage.exists(ownPath));
    assert(store.addCredential("new-net", "new-secret"));
  }
  assert(Storage.readFile(oldPath) == legacy);
  assert(!Storage.exists(strcmp(argv[1], "backup") == 0 ? "/.crosspoint/wifi.json" : "/.crosspoint/wifi.json.bak"));
}
