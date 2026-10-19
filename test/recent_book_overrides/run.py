#!/usr/bin/env python3
"""Run the production recent-book store/JSON and margin getter with host service shims."""
import argparse
from pathlib import Path
import subprocess


SHIMS = {
    "HalStorage.h": r'''#pragma once
#include <cstdint>
#include <map>
#include <string>
class String : public std::string {
 public:
  using std::string::string;
  explicit String(const std::string& text) : std::string(text) {}
  bool isEmpty() const { return empty(); }
  size_t write(uint8_t c) { push_back(static_cast<char>(c)); return 1; }
  size_t write(const uint8_t* data, size_t n) {
    append(reinterpret_cast<const char*>(data), n); return n;
  }
};
struct HostStorage {
  std::map<std::string, std::string> files;
  bool exists(const char* path) const { return files.count(path) != 0; }
  bool mkdir(const char*) { return true; }
  String readFile(const char* path) const {
    const auto it = files.find(path);
    return String(it == files.end() ? std::string{} : it->second);
  }
  bool writeFile(const char* path, const String& data) { files[path] = data; return true; }
};
inline HostStorage Storage;
''',
    "FsHelpers.h": r'''#pragma once
#include <string_view>
namespace FsHelpers {
inline bool hasEpubExtension(std::string_view s) { return s.ends_with(".epub"); }
inline bool hasXtcExtension(std::string_view s) { return s.ends_with(".xtc"); }
inline bool hasTxtExtension(std::string_view s) { return s.ends_with(".txt"); }
inline bool hasMarkdownExtension(std::string_view s) { return s.ends_with(".md"); }
}
''',
    "SidecarFiles.h": r'''#pragma once
#include <cstdint>
#include <string>
namespace SidecarFiles { inline uint32_t metadataStamp(const std::string&) { return 0; } }
''',
    "Epub.h": r'''#pragma once
#include <string>
class Epub {
 public:
  Epub(const std::string&, const std::string&) {}
  bool load(bool = true, bool = false) { return true; }
  bool loadForMetadata() { return true; }
  std::string getTitle() const { return {}; }
  std::string getAuthor() const { return {}; }
  std::string getSeries() const { return {}; }
  std::string getSeriesIndex() const { return {}; }
  std::string getThumbBmpPath() const { return {}; }
};
''',
    "Xtc.h": '#pragma once\n#include "Epub.h"\nusing Xtc = Epub;\n',
    "Logging.h": '#pragma once\n#define LOG_DBG(...) ((void)0)\n#define LOG_ERR(...) ((void)0)\n',
    "ReaderMarginProbe.h": r'''#pragma once
#include <cstdint>
// Only the state touched by the verbatim production getter; no device/UI code.
class EpubReaderActivity {
 public:
  int8_t bookIgnorePublisherPageMarginsOverride = -1;
  bool getEffectiveIgnorePublisherPageMargins() const;
};
''',
}


def between(source, start, end):
    assert source.count(start) == 1 and source.count(end) == 1
    return source.split(start, 1)[1].split(end, 1)[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--arduino-json", type=Path, help="ArduinoJson src directory (defaults to X4 Pro PIO dependency)")
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    repo, out = args.repo.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    shim = out / "shims"
    shim.mkdir(exist_ok=True)
    for name, contents in SHIMS.items():
        (shim / name).write_text(contents)
    reader_dir = repo / "src/activities/reader"
    menu_header = (reader_dir / "EpubReaderMenuActivity.h").read_text()
    quick_header = (reader_dir / "QuickOverridesActivity.h").read_text()
    menu_fields = between(menu_header, "// Pending state (mutated locally, returned to parent on finish)",
                          "  static constexpr const char* pageTurnLabels[]")
    quick_start = "  int8_t pendingEmbeddedStyleOverride = -1;"
    quick_fields = quick_start + between(quick_header, quick_start, "\n};")
    # State declarations and exit methods come from the real menus. Only the
    # activity shell is replaced, so payload type/default bugs remain observable.
    (shim / "MenuProbe.h").write_text(
        '#pragma once\n#include "activities/ActivityResult.h"\nstruct ReaderSizeList;\n'
        'struct MenuProbeBase {\n'
        '  ActivityResult captured; bool finished = false;\n'
        '  void setResult(ActivityResult&& r) { captured = std::move(r); }\n'
        '  void setResult(MenuResult&& r) { captured = ActivityResult(std::move(r)); }\n'
        '  void finish() { finished = true; }\n};\n'
        'class EpubReaderMenuActivity : public MenuProbeBase { public:\n'
        '  enum class MenuAction { NONE, GO_HOME };\n'
        '  void finishWithAction(MenuAction action); void onBackPressed();\n' + menu_fields + '\n};\n'
        'class QuickOverridesActivity : public MenuProbeBase { public:\n'
        '  void finishWithResult(bool cancelled);\n' + quick_fields + '\n};\n')
    menu_source = (reader_dir / "EpubReaderMenuActivity.cpp").read_text()
    quick_source = (reader_dir / "QuickOverridesActivity.cpp").read_text()
    exit_methods = ""
    for source, start, end in [
        (menu_source, "void EpubReaderMenuActivity::finishWithAction(MenuAction action) {",
         "void EpubReaderMenuActivity::onActionSelected(int index) {"),
        (menu_source, "void EpubReaderMenuActivity::onBackPressed() {",
         "std::string EpubReaderMenuActivity::getItemValueString(int index) const {"),
        (quick_source, "void QuickOverridesActivity::finishWithResult(bool cancelled) {",
         "void QuickOverridesActivity::render(RenderLock&&) {")]:
        exit_methods += start + between(source, start, end) + "\n"
    json_source = repo / "src/JsonSettingsIO.cpp"
    recent_json = between(json_source.read_text(), "// ---- RecentBooksStore ----", "// ---- OpdsServerStore ----")
    generated = out / "RecentBookProbe.cpp"
    reader_source = (repo / "src/activities/reader/EpubReaderActivity.cpp").read_text()
    signature = "bool EpubReaderActivity::getEffectiveIgnorePublisherPageMargins() const {"
    getter = signature + between(reader_source, signature, "bool EpubReaderActivity::getEffectiveEmbeddedStyle() const {")
    generated.write_text('#include <ArduinoJson.h>\n#include <Logging.h>\n#include "JsonSettingsIO.h"\n'
                         '#include "RecentBooksStore.h"\n#include "CrossPointSettings.h"\n'
                         '''#include "ReaderMarginProbe.h"
#include "MenuProbe.h"
#include "activities/home/BookDetails.h"
// No sidecar metadata service in this in-memory persistence probe.
namespace BookDetailsLookup {
bool cached(const std::string&, uint32_t, BookDetails&) { return false; }
bool parse(const std::string&, uint32_t, BookDetails&, BuildArena*) { return false; }
}
'''
                         + recent_json + '\n' + getter + '\n' + exit_methods)
    arduino_json = args.arduino_json or repo / ".pio/libdeps/x4pro/ArduinoJson/src"
    if not (arduino_json / "ArduinoJson.h").is_file():
        parser.error("ArduinoJson not found; run pio pkg install -e x4pro or pass --arduino-json")
    sanitizer = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
    binary = out / "recent-book-overrides"
    command = [args.cxx, "-std=c++20", "-O1", "-g", *sanitizer,
               "-I" + str(shim), "-I" + str(repo / "src"), "-I" + str(arduino_json),
               str(repo / "src/RecentBooksStore.cpp"), str(generated),
               str(Path(__file__).with_name("RecentBookOverridesTest.cpp")), "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
