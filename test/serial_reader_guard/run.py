#!/usr/bin/env python3
"""Compile the production serial ACT dispatcher against host I/O shims.

Checks the released-canvas crash path without opening a real book or device.
"""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    source = (args.repo / 'src/main.cpp').read_text()
    start, end = '  if (cmd.startsWith("ACT:")) {', '  if (cmd == "STATE") {'
    assert source.count(start) == 1 and source.count(end) == 1
    dispatch = start + source.split(start, 1)[1].split(end, 1)[0]
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    code = r'''#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

struct String {
  std::string text;
  String(const char* s) : text(s) {}
  String(std::string s) : text(std::move(s)) {}
  bool startsWith(const char* p) const { return text.rfind(p, 0) == 0; }
  String substring(size_t at) const { return text.substr(at); }
  size_t length() const { return text.size(); }
  const char* c_str() const { return text.c_str(); }
  bool operator==(const char* s) const { return text == s; }
  operator std::string() const { return text; }
};
struct Renderer {
  unsigned char canvas = 0;
  bool live = true;
  unsigned char* getFrameBuffer() { return live ? &canvas : nullptr; }
} renderer;
struct Store {
  bool present = true;
  int calls = 0;
  bool exists(const char*) { ++calls; return present; }
} Storage;
struct Logger {
  std::string text;
  void printf(const char* literal) { text += literal; }
  template<typename... T> void printf(const char* format, T... args) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), format, args...);
    text += buf;
  }
} logSerial;
enum class LibraryTab { Books, Recent, New, Authors };
struct Manager {
  int reader = 0, home = 0, other = 0;
  std::string path;
  void goToReader(std::string p) { ++reader; path = std::move(p); }
  void goHome() { ++home; }
  void goToSettings() { ++other; }
  void goToGlobalBookmarks() { ++other; }
  void goToLibrary(LibraryTab, std::string = {}) { ++other; }
} activityManager;
static int checks = 0;
void check(bool good, const char* why) {
  ++checks;
  if (!good) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
bool dispatch(const String& cmd) {
// PRODUCTION_DISPATCH
  return false;
}
void reset() {
  renderer.live = true;
  Storage = {};
  activityManager = {};
  logSerial.text.clear();
}
int main() {
  for (bool present : {true, false}) {
    reset(); renderer.live = false; Storage.present = present;
    check(dispatch("ACT:READER:/test.epub"), "released-canvas command is handled");
    check(activityManager.reader == 0, "released canvas cannot enter reader/font loading");
    check(Storage.calls == 0, "released canvas is rejected before storage access");
    check(logSerial.text == "ACT_ERROR:leave transfer mode with CMD:ACT:HOME then retry\n",
          "clear recovery instruction and no success acknowledgement");
  }
  reset(); renderer.live = false;
  check(dispatch("ACT:HOME") && activityManager.home == 1, "Home recovery remains available without canvas");
  check(logSerial.text == "ACT_OK\n", "Home recovery acknowledges normally");
  reset();
  check(dispatch("ACT:READER:/test.epub"), "reader command accepted after framebuffer recovery");
  check(activityManager.reader == 1 && activityManager.path == "/test.epub", "valid path reaches reader once");
  check(Storage.calls == 1 && logSerial.text == "ACT_OK\n", "normal file validation and acknowledgement preserved");
  reset(); Storage.present = false;
  check(dispatch("ACT:READER:/missing.epub") && activityManager.reader == 0, "missing file still rejected");
  check(logSerial.text == "ACT_ERROR:no such file /missing.epub\n", "missing-file diagnostic unchanged");
  reset();
  check(dispatch("ACT:SETTINGS") && activityManager.other == 1, "other activity commands unaffected");
  check(logSerial.text == "ACT_OK\n", "other command acknowledgement unaffected");
  reset();
  check(!dispatch("OTHER"), "non-activity commands remain unhandled");
  std::printf("%d serial reader framebuffer guard checks passed\n", checks);
}
'''
    (out / 'probe.cpp').write_text(code.replace('// PRODUCTION_DISPATCH', dispatch))
    command = ['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-g',
               str(out / 'probe.cpp'), '-o', str(out / 'probe')]
    if args.sanitize:
        command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(command, check=True)
    subprocess.run([str(out / 'probe')], check=True)


if __name__ == '__main__':
    main()
