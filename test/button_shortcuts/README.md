# Shortcut integration checks

Run `python3 test/button_shortcuts/run.py --out /tmp/shortcut-tests --sanitize`.
Requires a C++20 host compiler and the X4 Pro ArduinoJson dependency installed by
PlatformIO. The IO shims use in-memory files and synthetic button events.

The harness compiles the production button classifier, real settings types and
settings JSON functions, and verbatim production shortcut rows, dispatch loop,
and pending-navigation input gate. Coverage includes stable saved action IDs,
all light/USB capability combinations, device/web picker accessors, JSON reload,
single/double presses, alias dispatch, and stale input during navigation.
The settings list is restricted to the real button rows and status-bar visibility; unrelated UI/hardware
services are shimmed. It does not emulate an e-ink screen or physical USB.

Add `--negative-control` with a separate output directory to restore the old
input gate in the generated host probe. The buffered Confirm regression must
then fail, demonstrating that the test distinguishes the navigation fix.
