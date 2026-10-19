# Per-book override persistence checks

```sh
python test/recent_book_overrides/run.py --out /tmp/book-overrides --sanitize
```

Uses the production `RecentBooksStore.cpp`, the real settings defaults, and the
ArduinoJson dependency installed by PlatformIO for X4 Pro. The two recent-book
JSON functions, effective-margin getter, menu pending-state declarations and
menu result methods are extracted verbatim from their
production translation units, so unrelated credentials and UI dependencies do
not need to be linked into this host test. Extraction fails if its boundaries
change. No replacement serialization or override implementation is tested.

The SD service uses an isolated in-memory filesystem. Metadata readers, logs and
the activity shell are stubs; metadata parsing, SD hardware, menu rendering, and physical display
appearance are outside this test's scope. It checks JSON reloads, independent
books, reopening, metadata updates, every older setter, malformed or missing
preferences, explicit On/Off against both global defaults, and the fresh
Extra Paragraph Spacing default. Both full-menu exits and Quick Overrides must
retain Bionic Reading’s Default/On/Off value when changing margins. All specimen
names and contents are synthetic.

The existing `StylesheetMediaLayout.PublisherMargin*` pipeline tests cover layout,
source anchors, and pagination caches. Firmware builds and review cover the menu
and sleep-render plumbing.
