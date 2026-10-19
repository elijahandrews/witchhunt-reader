This probe compiles the actual `lib/Logging/Logging.h` with the existing host
serial shim. It checks zero and multiple arguments, bitfields, argument and origin
side effects exactly once, source basename/line output, full messages, disabled
logging, default/negative log levels, and unchanged INFO/DEBUG/TRACE filtering.
It also checks that compact binaries omit the original error format strings.

Run with `python3 test/compact_logging/run.py --out /tmp/compact-logging --sanitize`.
The 16 profiles cover unoptimized and optimized compilation. This test verifies
macro semantics; serial/RTC transport remains the responsibility of `Logging.cpp`.
Compact source locations require the exact release source snapshot to decode.
