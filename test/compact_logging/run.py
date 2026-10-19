#!/usr/bin/env python3
"""Exercise the real Logging.h macros with the existing host serial shim."""
import argparse
from pathlib import Path
import subprocess


SOURCE = r'''#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
#include "Logging.h"
struct Event { std::string level, origin, message; };
std::vector<Event> events;
void logPrintf(const char* level, const char* origin, const char* format, ...) {
  char message[256];
  va_list args;
  va_start(args, format);
  const int bytes = vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  assert(bytes >= 0 && bytes < static_cast<int>(sizeof(message)));
  events.push_back({level, origin, message});
}
int originCalls = 0, numberCalls = 0, textCalls = 0, infoCalls = 0;
const char* origin() { ++originCalls; return "TEST"; }
int number() { ++numberCalls; return 42; }
const char* text() { ++textCalls; return "value"; }
int info() { ++infoCalls; return 7; }
int main() {
  struct Bits { unsigned value : 3; };
  [[maybe_unused]] Bits bits{5};
#line 200 "CompactProbe.cpp"
  LOG_ERR(origin(), "FULL_PAYLOAD_ZERO");
  LOG_ERR(origin(), "FULL_PAYLOAD_MULTI:%d:%d:%s", number(), bits.value, text());
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 0
  assert(originCalls == 2 && numberCalls == 1 && textCalls == 1);
  assert(events.size() == 2);
  assert(events[0].level == "ERR" && events[1].level == "ERR");
  assert(events[0].origin == "TEST" && events[1].origin == "TEST");
#if defined(WITCH_COMPACT_ERRORS) && WITCH_COMPACT_ERRORS
  assert(events[0].message == "CompactProbe.cpp:200\n");
  assert(events[1].message == "CompactProbe.cpp:201\n");
#else
  assert(events[0].message == "FULL_PAYLOAD_ZERO\n");
  assert(events[1].message == "FULL_PAYLOAD_MULTI:42:5:value\n");
#endif
#else
  assert(originCalls == 0 && numberCalls == 0 && textCalls == 0);
  assert(events.empty());
#endif
  const auto before = events.size();
  LOG_INF("TEST", "INFO:%d", info());
  LOG_DBG("TEST", "DEBUG:%d", info());
  LOG_TRC("TEST", "TRACE:%d", info());
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 3
  assert(infoCalls == 3 && events.size() == before + 3);
  assert(events[before].message == "INFO:7\n");
  assert(events[before + 1].message == "DEBUG:7\n");
  assert(events[before + 2].message == "TRACE:7\n");
#else
  assert(infoCalls == 0 && events.size() == before);
#endif
  puts("PASS: actual logging header evaluation, bitfields, payload and filtering");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    repo, out = args.repo.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = out / "probe.cpp"
    source.write_text(SOURCE)
    profiles = {
        "compact": ["-DENABLE_SERIAL_LOG", "-DLOG_LEVEL=0", "-DWITCH_COMPACT_ERRORS=1"],
        "full": ["-DENABLE_SERIAL_LOG", "-DLOG_LEVEL=0"],
        "explicit_full": ["-DENABLE_SERIAL_LOG", "-DLOG_LEVEL=0", "-DWITCH_COMPACT_ERRORS=0"],
        "disabled": ["-DLOG_LEVEL=0", "-DWITCH_COMPACT_ERRORS=1"],
        "negative": ["-DENABLE_SERIAL_LOG", "-DLOG_LEVEL=-1", "-DWITCH_COMPACT_ERRORS=1"],
        "compact_verbose": ["-DENABLE_SERIAL_LOG", "-DLOG_LEVEL=3", "-DWITCH_COMPACT_ERRORS=1"],
        "full_verbose": ["-DENABLE_SERIAL_LOG", "-DLOG_LEVEL=3"],
        "default_level": ["-DENABLE_SERIAL_LOG", "-DWITCH_COMPACT_ERRORS=1"],
    }
    for optimization in ("-O0", "-O2"):
        for name, flags in profiles.items():
            binary = out / f"{name}-{optimization[1:]}"
            command = [args.cxx, "-std=gnu++20", optimization, "-Wall", "-Wextra", *flags]
            if args.sanitize:
                command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            command += ["-I" + str(repo / "test/shims"), "-I" + str(repo / "lib/Logging"),
                        str(source), "-o", str(binary)]
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True)
            keeps_full_payload = name in ("full", "explicit_full", "full_verbose")
            assert (b"FULL_PAYLOAD_ZERO" in binary.read_bytes()) == keeps_full_payload
            assert (b"FULL_PAYLOAD_MULTI" in binary.read_bytes()) == keeps_full_payload
    print("PASS: 16 compact/full/disabled macro profiles; full format strings absent from compact binaries")


if __name__ == "__main__":
    main()
