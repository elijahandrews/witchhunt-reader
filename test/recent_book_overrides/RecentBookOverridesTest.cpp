#include <cassert>
#include <iostream>
#include <string>

#include "CrossPointSettings.h"
#include "MenuProbe.h"
#include "ReaderMarginProbe.h"
#include "RecentBooksStore.h"

CrossPointSettings CrossPointSettings::instance;

namespace {
constexpr const char* recentFile = "/.crosspoint/recent.json";
constexpr const char* firstBook = "/specimen-a.epub";
constexpr const char* secondBook = "/specimen-b.epub";

void freshBooks() {
  Storage.files.clear();
  RECENT_BOOKS.loadFromFile();
  Storage.files[firstBook] = "synthetic";
  Storage.files[secondBook] = "synthetic";
  RECENT_BOOKS.addBook(firstBook, "First specimen", "", "", "");
  RECENT_BOOKS.addBook(secondBook, "Second specimen", "", "", "");
}

void setMargins(const std::string& path, int8_t value) {
  assert(RECENT_BOOKS.setReaderOverrides(path, -1, -1, -1, "", -1, int8_t{-1}, -1, -1, -1, -1, -1, -1, value));
}

int margins(const std::string& path) { return RECENT_BOOKS.getBookByPath(path).ignorePublisherPageMarginsOverride; }
}  // namespace

int main() {
  assert(SETTINGS.extraParagraphSpacing == 0);
  assert(RecentBook{}.ignorePublisherPageMarginsOverride == -1);
  EpubReaderActivity reader;
  // The compiled production getter must honor explicit Off even when the global
  // default is On, and explicit On even when the global default is Off.
  for (bool global : {false, true}) {
    SETTINGS.ignorePublisherPageMargins = global;
    for (int8_t value : {int8_t{-1}, int8_t{0}, int8_t{1}}) {
      reader.bookIgnorePublisherPageMarginsOverride = value;
      const bool expected = value == -1 ? global : value == 1;
      assert(reader.getEffectiveIgnorePublisherPageMargins() == expected);
    }
  }

  // Changing margins must preserve Bionic Reading's three-state choice on
  // every menu exit, including Back (which still applies pending settings).
  assert(MenuResult{}.bionicReadingOverride == -1);
  for (int8_t bionic : {int8_t{-1}, int8_t{0}, int8_t{1}}) {
    for (int8_t margin : {int8_t{-1}, int8_t{0}, int8_t{1}}) {
      for (bool back : {false, true}) {
        EpubReaderMenuActivity menu;
        menu.pendingBionicReadingOverride = bionic;
        menu.pendingIgnorePublisherPageMarginsOverride = margin;
        if (back)
          menu.onBackPressed();
        else
          menu.finishWithAction(EpubReaderMenuActivity::MenuAction::GO_HOME);
        assert(menu.finished);
        assert(menu.captured.isCancelled == back);
        const auto& result = std::get<MenuResult>(menu.captured.data);
        assert(result.bionicReadingOverride == bionic);
        assert(result.ignorePublisherPageMarginsOverride == margin);
        assert(result.hyphenationOverride == -1);
        assert(result.fontSizeNormalizationOverride == -1);
        assert(result.inlineFootnotePreviewsOverride == -1);
      }
      for (bool cancelled : {false, true}) {
        QuickOverridesActivity quick;
        quick.pendingBionicReadingOverride = bionic;
        quick.pendingIgnorePublisherPageMarginsOverride = margin;
        quick.finishWithResult(cancelled);
        assert(quick.finished);
        assert(quick.captured.isCancelled == cancelled);
        const auto& result = std::get<MenuResult>(quick.captured.data);
        assert(result.bionicReadingOverride == bionic);
        assert(result.ignorePublisherPageMarginsOverride == margin);
        assert(result.hyphenationOverride == -1);
        assert(result.fontSizeNormalizationOverride == -1);
        assert(result.inlineFootnotePreviewsOverride == -1);
      }
    }
  }

  freshBooks();
  assert(margins(firstBook) == -1 && margins(secondBook) == -1);
  for (int8_t value : {int8_t{1}, int8_t{0}, int8_t{-1}}) {
    setMargins(firstBook, value);
    // Parse fresh JSON after each write: no in-memory object can mask a missing
    // serialized property. The second book always retains its independent default.
    assert(RECENT_BOOKS.loadFromFile());
    assert(margins(firstBook) == value);
    assert(margins(secondBook) == -1);
  }
  setMargins(firstBook, 1);
  setMargins(secondBook, 0);
  RECENT_BOOKS.addBook(firstBook, "Reopened specimen", "", "", "");
  assert(margins(firstBook) == 1 && margins(secondBook) == 0);
  RECENT_BOOKS.updateBook(firstBook, "Updated metadata", "", "", "");
  assert(margins(firstBook) == 1);

  // Every older/narrower setter must preserve the newer margin choice.
  assert(RECENT_BOOKS.setReaderOverrides(firstBook, 1, 0));
  assert(RECENT_BOOKS.getBookByPath(firstBook).bionicReadingOverride == -1);
  assert(margins(firstBook) == 1);
  assert(RECENT_BOOKS.setReaderOverrides(firstBook, 1, 0, 0, 1));
  assert(RECENT_BOOKS.getBookByPath(firstBook).bionicReadingOverride == -1);
  assert(margins(firstBook) == 1);
  assert(RECENT_BOOKS.setReaderOverrides(firstBook, 1, 0, -1, "Synthetic family", 1));
  assert(RECENT_BOOKS.getBookByPath(firstBook).bionicReadingOverride == -1);
  assert(margins(firstBook) == 1);
  assert(RECENT_BOOKS.setReaderOverrides(firstBook, 1, 0, true));
  assert(margins(firstBook) == 1);
  assert(RECENT_BOOKS.setReaderOverrides(firstBook, 1, 0, 0, 1, false));
  assert(margins(firstBook) == 1);
  assert(RECENT_BOOKS.setReaderOverrides(firstBook, 1, 0, -1, "Synthetic family", 1, true));
  assert(margins(firstBook) == 1);
  assert(RECENT_BOOKS.setReaderOverrides(firstBook, 1, 0, -1, "Synthetic family", 1, false, 2));
  assert(margins(firstBook) == 1 && margins(secondBook) == 0);
  assert(!RECENT_BOOKS.setReaderOverrides("/absent.epub", 1, 0));
  assert(margins(firstBook) == 1);

  // Old recent.json files and corrupt values should inherit the global setting.
  for (const auto& property : {std::string{}, std::string{",\"ignorePublisherPageMarginsOverride\":-2"},
                               std::string{",\"ignorePublisherPageMarginsOverride\":2"},
                               std::string{",\"ignorePublisherPageMarginsOverride\":128"},
                               std::string{",\"ignorePublisherPageMarginsOverride\":\"bad\""},
                               std::string{",\"ignorePublisherPageMarginsOverride\":null"}}) {
    Storage.files[recentFile] = "{\"books\":[{\"path\":\"/specimen-a.epub\"" + property + "}]}";
    assert(RECENT_BOOKS.loadFromFile());
    assert(margins(firstBook) == -1);
  }
  // Upstream point-size migrations must coexist with the new per-book field.
  for (const auto& migration : {std::pair{0, 12}, std::pair{1, 10}, std::pair{2, 1}}) {
    Storage.files[recentFile] = "{\"fontSizeOrderV\":" + std::to_string(migration.first) +
                                ",\"books\":[{\"path\":\"/specimen-a.epub\",\"fontSizeOverride\":" +
                                std::to_string(migration.first == 2 ? 1 : 0) +
                                ",\"ignorePublisherPageMarginsOverride\":1}]}";
    assert(RECENT_BOOKS.loadFromFile());
    const RecentBooksStore::Hold hold;  // keep upstream lazy storage resident through the explicit save
    const auto book = RECENT_BOOKS.getBookByPath(firstBook);
    assert(book.fontSizeOverride == migration.second && book.ignorePublisherPageMarginsOverride == 1);
    assert(RECENT_BOOKS.saveToFile());
    assert(RECENT_BOOKS.loadFromFile());
    assert(RECENT_BOOKS.getBookByPath(firstBook).fontSizeOverride == migration.second);
  }
  // Relocation changes the path/cache prefix while retaining both new and old overrides.
  freshBooks();
  RECENT_BOOKS.addBook(firstBook, "Moved specimen", "", "", "/cache-old/cover.bmp");
  assert(RECENT_BOOKS.setReaderOverrides(firstBook, 1, 0, -1, "Synthetic family", 13, int8_t{-1}, 2, 1, 0, 1, 0, 1, 1));
  Storage.files["/relocated.epub"] = Storage.files[firstBook];
  Storage.files.erase(firstBook);
  RECENT_BOOKS.relocate(firstBook, "/relocated.epub", "/cache-old", "/cache-new");
  assert(RECENT_BOOKS.loadFromFile());
  const auto moved = RECENT_BOOKS.getBookByPath("/relocated.epub");
  assert(moved.path == "/relocated.epub" && moved.coverBmpPath == "/cache-new/cover.bmp");
  assert(moved.ignorePublisherPageMarginsOverride == 1 && moved.fontSizeOverride == 13);
  assert(moved.bionicReadingOverride == -1 && moved.paragraphAlignmentOverride == 2);

  std::cout << "PASS: paragraph default, effective override matrix, JSON round trips, book isolation, "
               "reopen, metadata updates, all legacy setters, missing/invalid values, both menu result paths, "
               "point-size migration and relocation\n";
}
