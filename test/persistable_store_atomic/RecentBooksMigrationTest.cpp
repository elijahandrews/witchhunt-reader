#include <HalStorage.h>
#include <RecentBooksStore.h>

#include <cassert>
#include <string>

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::string scenario = argv[1];
  const std::string legacy =
      R"({"books":[{"path":"/fixture.epub","title":"Synthetic legacy title","author":"Fixture author","coverBmpPath":"/.crosspoint/epub_123/thumb.bmp","witchOnly":42}],"witchSettings":{"font":14}})";
  const std::string newer =
      R"({"books":[{"path":"/new.epub","title":"New synthetic title","author":"Fixture author"}]})";
  const char* canonical = "/.crosspoint/recent.json";
  const char* backup = "/.crosspoint/recent.json.bak";
  const char* own = RecentBooksStore::getFilePath();
  const std::string ownBackup = std::string(own) + ".bak";
  const bool backupOnly = scenario != "legacy" && scenario != "both";
  Storage.put(backupOnly ? backup : canonical, legacy);
  if (scenario == "both") Storage.put(backup, newer);
  // A recoverable JSON backup must win over a stale binary empty-list snapshot.
  const std::string binary("\x03\x00", 2);
  Storage.put("/.crosspoint/recent.bin", binary);
  if (scenario == "current") Storage.put(own, newer);
  if (scenario == "own_backup") Storage.put(ownBackup, newer);
  if (scenario == "corrupt") Storage.put(own, "{broken");
  if (scenario == "copy_failure") Storage.failNextWrite();
  auto& store = RecentBooksStore::getInstance();
  const bool loaded = store.loadFromFile();
  assert(loaded == (scenario != "corrupt" && scenario != "copy_failure"));
  if (scenario == "corrupt")
    assert(store.getCount() == 0);
  else {
    assert(store.getCount() == 1);
    const bool useOwn = scenario == "current" || scenario == "own_backup";
    assert(store.getBooks()[0].path == (useOwn ? "/new.epub" : "/fixture.epub"));
    if (!useOwn) assert(store.getBooks()[0].coverBmpPath == "/.crosspoint/epub_123/thumb.bmp");
    if (loaded) assert(Storage.exists(own));
  }
  assert(Storage.readFile(backupOnly ? backup : canonical) == legacy);
  assert(Storage.readFile("/.crosspoint/recent.bin") == binary);
  if (scenario == "both")
    assert(Storage.readFile(backup) == newer);
  else
    assert(!Storage.exists(backupOnly ? canonical : backup));
}
