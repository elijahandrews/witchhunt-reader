#pragma once
#include <string>
enum class StrId {
  STR_BOOK_SYNC,
  STR_BOOK_SYNC_CONNECTING,
  STR_BOOK_SYNC_LIST,
  STR_BOOK_SYNC_CHECKING,
  STR_BOOK_SYNC_DOWNLOADING,
  STR_BOOK_SYNC_DONE,
  STR_BOOK_SYNC_FAILED,
  STR_BOOK_SYNC_CANCELLED,
  STR_BOOK_SYNC_COUNTS,
  STR_BOOK_SYNC_SCAN_PROGRESS,
  STR_CANCEL
};
struct Translations {
  const char* get(StrId id) const {
    switch (id) {
      case StrId::STR_BOOK_SYNC_DONE:
        return "done";
      case StrId::STR_BOOK_SYNC_FAILED:
        return "failed";
      case StrId::STR_BOOK_SYNC_CANCELLED:
        return "cancelled";
      case StrId::STR_BOOK_SYNC_COUNTS:
        return "%u/%u/%u";
      case StrId::STR_BOOK_SYNC_SCAN_PROGRESS:
        return "Checked: %u  Hashed: %u MiB";
      case StrId::STR_CANCEL:
        return "cancel";
      default:
        return "working";
    }
  }
};
inline Translations I18N;
#define tr(id) I18N.get(StrId::id)
