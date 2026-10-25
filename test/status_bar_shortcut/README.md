# Status bar shortcut checks

Run `python3 test/status_bar_shortcut/run.py --out /tmp/status-bar-check`.
The sanitizer harness compiles verbatim production layout and status-bar drawing with a draw-call recorder, the EPUB toggle body with an activity shell, and real line-reader source-anchor methods. It checks hidden drawing and reserved space, preference preservation, staged-frame invalidation, and repeated reflow anchors. It does not emulate the panel.

The footer checks execute production layout, corner-button handling and the
configured-tap branch in all four orientations. They cover both clock positions,
other footer items, a long translation falling back to an arrow, hidden/top-only
status bars and expired touch targets. No additional reading-space reservation
is introduced by the action.
