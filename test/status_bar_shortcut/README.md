# Status bar shortcut checks

Run `python3 test/status_bar_shortcut/run.py --out /tmp/status-bar-check`.
The sanitizer harness compiles verbatim production layout and status-bar drawing with a draw-call recorder, the EPUB toggle body with an activity shell, and real line-reader source-anchor methods. It checks hidden drawing and reserved space, preference preservation, staged-frame invalidation, and repeated reflow anchors. It does not emulate the panel.
