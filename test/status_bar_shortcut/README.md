# Status bar shortcut checks

Run `python3 test/status_bar_shortcut/run.py --sanitize --out /tmp/status-bar-check`.
The sanitizer harness compiles verbatim production layout and status-bar drawing with a draw-call recorder, the EPUB toggle body with an activity shell, and real line-reader source-anchor methods. It checks hidden drawing and reserved space, preference preservation, staged-frame invalidation, and repeated reflow anchors. It does not emulate the panel.

The footer checks execute production layout, corner-button handling and the
configured-tap branch in all four orientations. They cover both clock positions,
other footer items, a long translation falling back to an arrow, hidden/top-only
status bars and expired touch targets. No additional reading-space reservation
is introduced by the action.

The bottom-tap checks execute the actual EPUB and shared TXT/MD dispatch branches,
held-orientation inset mapping, and status-bar action. They cover the visible bar's
full height (including forced automatic-turn items and larger reader padding), the
32-pixel hidden reveal band, boundaries in all four orientations, disabled/non-touch
input, custom gesture precedence, Go back/link precedence, one action per contact,
and staged-page invalidation. When run against the single-pass AA implementation,
they also verify its captured-generation invalidation.

Run `python3 test/status_bar_shortcut/settings.py --sanitize --out /tmp/status-bar-settings`
for default-on migration and explicit off/on persistence through the actual setting
row and generic JSON loops. It uses ArduinoJson from `.pio/libdeps/x4pro/ArduinoJson/src`
by default; `--arduino-json PATH` selects another installed copy. Both harnesses
accept `--repo PATH` for the exact tree under review and offer `--sanitize` for ASan/UBSan.

CrossInk's corresponding bottom target uses a 32-pixel minimum; the reference reviewed
for this feature was [main at 9914146e](https://github.com/uxjulia/CrossInk/blob/9914146eeae7b46b300f475a16c32426fc02ec1f/src/activities/reader/ReaderStatusBarTapTarget.h).
These are software input/layout checks; they do not replace a device touch check.

CTest runs both harnesses as `StatusBarShortcut` and `StatusBarSettings`, using its
existing ArduinoJson dependency rather than requiring a firmware build. Ordinary
CTest leaves sanitizer flags off for hosts such as MinGW that may not provide them.
