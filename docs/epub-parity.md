# Witch Hunt EPUB behavior parity

Reference: Witch Hunt `master` at
`f216bfcce608ec32d85f3dfa9f74fbc7afdc0bce`. Target: this native CrossInk fork.
The initial replay lacked several EPUB features. The follow-up port implements
the observable behaviors inventoried below at native CrossInk interfaces,
rather than requiring identical internal code.
CrossInk's native UI, tables, RTL support, existing border handling and device
capabilities must remain intact.

## Line-spacing follow-up — 2026-11-09

The touch drawer's reflow preview omitted the publisher line-height multiplier,
which was also missing from cached TextBlock payloads. The Picco retained the
selected 105% correctly, but a publisher multiplier of 0.8 produced 24-pixel
reader lines while the preview showed 30 pixels. Preview and pagination now
share percentage rounding and publisher spacing; preview paragraph detection
uses each source line's actual advance. Full/partial cache versions 110/0xDF
retain the multiplier and invalidate older layouts without replacing settings
or saved reading positions.

All 278 focused parser, preview, raster and persistence tests pass, including
independent pixel comparisons before and after cache serialization. Two code
review passes and a separate flash-script review found no actionable defects.
Both firmware builds pass: Picco 6,009,152 bytes; X3/X4 6,526,688 bytes.

The Picco update was flashed after a fresh, hash-verified backup of its own
272 card files (92,951,197 bytes). The live rollback image, identity and active
partition were verified first. Independent verification matched the application
and both protected flash ranges; five runtime responses confirmed a successful
boot without a detected crash. No older state or X4 settings were restored.
Application SHA-256:
`0a7d2136e613f4a0717795dabe25cd9c07f0220a06b27facea4fc38578958a94`.
Private installation evidence lives under
`.local-device/cross-ink-fork/picco-spacing-install-20261110T014717Z/` in the
primary checkout (the directory timestamp is UTC). Physical verification is to
open an EPUB, change line spacing to 105%, close/reopen the drawer and confirm
that the preview and page retain matching line spacing. The automated pixel
tests cover this behavior; physical interaction still needs owner confirmation.

## Port checklist

Implementation and independent review are complete. Native tests, firmware builds
and simulator integration checks pass; the separate strict static-analysis
threshold and physical-device limitations are recorded below.
Existing bounded source behavior is the reference; this is not a general browser
CSS implementation.

| Area | Behavior to preserve | Validation |
| --- | --- | --- |
| Font fallback | Ordered quoted/named/generic family stacks; ordered face source candidates and format hints | Missing/corrupt first source, quoted commas/generic names, cache round trips, publisher modes |
| Face selection | Numeric weight/style ranking, later equal declarations, variable regular face supplying four styles | Synthetic variable font instances and competing declarations |
| Monospace | Real regular/bold/italic/bold-italic family; defaults for pre/code/kbd/samp | Equal advances, explicit overrides, cold/warm layout |
| Typography | All-small-caps, optional ligatures, authored two/three-character substitutions, first-line overrides | Authored/synthetic caps, direct triple ligatures, disabled ligatures, cache/split/reflow |
| Publisher sizing | Fractional content sizes and matching optical size; final-size synthetic outline caps | Metrics/raster agreement, nested sizes, headings/tables/drop caps |
| Font robustness | Streamed GPOS/GDEF/GSUB, bounded large-font admission, temporary failure recovery, validated extraction | Resident/streamed equivalence, mutation/CRC, allocation/I/O failures and retry |
| Existing font quality | HD cpfont coverage and fractional metrics; Unicode case transforms and letter spacing | Real font loader, glyph pixels, layout/drawing/selection agreement |
| Reader controls | Publisher outer-margin override, spacing defaults, contact-bound dictionary holds and font-resize feedback | Persistence, native touch flow, clamped/failed size changes |
| Local CSS | Chapter-local head styles and font faces; linked sheets retain the source book-wide merge; local specificity and relative URLs | Per-spine isolation and cold/warm/interrupted compilation |
| Media retry | Retry permissive fallback after transient head-inventory failure | Reopen after recovered memory/I/O; screen media on links and local styles |
| Hidden text | Transparent OCR, opacity/visibility and tiny watermark text | Preserve images and source coordinates while suppressing hidden text |
| Whitespace | Distinct pre/pre-wrap/pre-line/nowrap, tabs and blank lines | Nested spans, wrapping, cache persistence and source mapping |
| Line height | Inherited publisher line-height, normal/reset semantics | Block/inline scopes, floats and continuation |
| Box layout | Min/max sizes, empty spacers, inline blocks, right floats/clear, keep-together groups | Short/oversized groups, image constraints and moved anchors |
| Backgrounds | Bounded CSS background images and gray backgrounds | Specificity, relative URLs, fragmentation and nested masks |
| Pseudo elements | Bounded literal before/after content and first-line typography | Generated content never displaces real source anchors |
| Hyphenation | CSS manual/none/auto, aliases and before/after/line limits | Inheritance/reset, global interaction and line breaking |
| HTML compatibility | Bounded HTML void-tag repair across content consumers with original byte mapping | Layout/search/body-font/footnote parity; strict package XML remains strict |
| Search responsiveness | Yield after bounded result-count growth | Dense matches remain cancellable before scanning an entire chapter |
| Reading anchors | Persist raw source/word positions across CSS/reflow; precise inline IDs | Resume, search, links, Go back and changed visible text |
| Go back access | Footer affordance and hit target alongside native navigation | One-shot expiration, orientation and touch geometry |
| SVG and math | Bounded SVG illustrations and accepted MathML fallback images | Synthetic SVG limits, failed-image text fallback and real converter output |
| Image compatibility | Byte-sniff misnamed/extensionless assets; SVG cover wrappers and missing-cover fallback | Synthetic cover/chapter assets and failure cases |
| Progressive JPEG | Full progressive coefficient output within a bounded workspace, native fallback before pixel output | Synthetic sampling/scale fixtures, late read/seek failures, cancellation and no partial PXC publication |
| Image warming | Prepare current and next five pages without writing the framebuffer, cancel for input, refresh recovered placeholders | Chapter spill, bounded retries, transfer cancellation and once-only redraw |
| Table images | Preserve an admitted image in each cell and in paragraph fallback | Native grid, row splitting, cache round trip, warming and source anchors |
| Navigation metadata | Sparse-nav/richer-NCX policy and unique-basename matching | Ambiguous paths, fragments and exact-match precedence |

General viewport-media evaluation and complex-script OpenType shaping are not
implemented by the source reference and are not additional parity requirements.
Native page-count bounds and direct dictionary hold behavior already provide
equivalent safeguards. Image warming uses the native render task after the current page is displayed,
with no additional worker or framebuffer.

## Integration and evidence

- Extend bounded native abstractions. Track persistent and peak memory costs;
  preserve constrained-device fallbacks and failure handling.
- Coordinate CSS flags/codecs and per-word metadata explicitly. Bump complete
  and partial layout/cache versions when their serialized contracts change.
- Use original synthetic fixtures; private books and device state stay outside
  the repository.
- Verify parsing, layout, drawing, persistence and raw source coordinates
  together, including cold/warm caches, interruption and resource failures.
- Re-run independent review after fixes until no actionable findings remain.
- Build Picco and constrained firmware targets before claiming the full port.
  Hardware appearance and physical input remain separate verification steps.

The final native checkpoint on 2026-11-09 registered **2,591 tests: 2,588 passed,
zero failed and three skipped**, with AddressSanitizer and UndefinedBehaviorSanitizer
enabled and no suite exclusions. The skips are two inherited negative HTTP
transport cases and an optional private-font fixture. The code was cleanly
rebuilt after shared headers were frozen, then refreshed for the final bounded
arena allocation. Focused parser coverage is 213 cases; these and the font,
image, cache, source-position and production-method harnesses are included in
the overall count rather than added to it.

Independent review/fix/rereview loops are clean. Deliberately restoring faults
in preview stopping, source-character mapping, script sizing, truncated floats,
arena sizing, CSS hydration, glyph arithmetic, ZIP validation, image decoding
and file-close handling makes the corresponding regression checks fail.

The strict `pio check -e default --fail-on-defect low --fail-on-defect medium
--fail-on-defect high` gate **did not pass**: cppcheck reports zero high,
eight medium and 50 low diagnostic instances. Independent triage found no new
actionable correctness defect. The eight medium warnings concern scratch arrays
in unchanged `AssetCompanionStore.cpp`; paths, buffers and hashes are populated
before use. Other void-pointer warnings mis-model the unchanged typed
`unique_ptr<Bookmark[]>`/`unique_ptr<Clipping[]>` owners. Remaining findings are
style/performance advisories, including suggestions to replace the new image
loops with STL algorithms and to pass the enlarged jump position by reference.
The existing by-value jump parameter deliberately preserves a snapshot when
its caller aliases a stack entry. No analyzer suppressions were added.

Both firmware environments pass clean PlatformIO builds. Binary sizes include
the application image headers and fit the existing partitions; no themes were
removed.

| Target | Application bytes | Partition bytes | Remaining bytes | Static internal RAM |
| --- | ---: | ---: | ---: | ---: |
| BOOX Picco | 6,008,256 | 15,138,816 | 9,130,560 | 122,604 |
| X3/X4 (`default`) | 6,526,384 | 6,553,600 | 27,216 | 74,520 |

Picco application SHA-256:
`95e11e6c04814b9be118507c26d1456c7fe2c150563e190b48e1a4d7ecc4fa81`.
X3/X4 application SHA-256:
`878239023c13aae4e02efc1998a7c6894c4dfc69f49013e75894180d42f50bac`.
The Picco image was **flashed on 2026-11-09** after a fresh hash-verified backup
of its current 230 card files (89,979,531 bytes). Its existing firmware was
verified against the saved rollback image before writing. Only the application
partition was written; independent verification matched the new image and both
protected flash ranges remained byte-identical. The card was kept in place;
no older state or X4 settings were restored.

Five runtime status responses confirmed Picco firmware `1.6.1-picco`, with
178,216 bytes free internal heap and a 131,060-byte largest block, without a
detected crash. Private backup, rollback and installation evidence are in the
shared `.local-device/cross-ink-fork/picco-parity-install-20261109T125010Z/`
directory; `picco-current-install-path.txt` points to it. The X3/X4 candidate
has not been flashed. Physical book rendering and touch checks remain separate
from the verified installation and boot.

The button simulator passes its full book/menu/settings flow and its explicit
bitmap-size, page-cache and selection checks. The S3 touch simulator passes
exact content sizing, serialized metrics, reading-position reflow, settings
return, every drawer tab, percentage and auto-turn panes, Home-button routing
and swipe dismissal. Captured synthetic font and reader pages were also visually
checked. Simulator assertions were updated for exact sizing and the added Search
row; production UI behavior was retained.

After an authorized install on BOOX Picco, compare the same books with publisher
fonts Off/Accents/All and Embedded Style enabled. Exercise fractional headings,
fallback fonts, shaded/float/table-image pages, search, reflow, Go back and sleep
resume. Check device heap and render-task stack headroom with publisher fonts
loaded. Section/CSS/metadata versions rebuild their derived caches automatically;
for decoder-quality comparison, clear only the relevant old `.pxc` pixels as
described in the image document. Physical appearance, timing and sustained memory
behavior remain hardware checks.

### Resource and persistence changes

- Keep-together groups retain at most one prior page while laying out the next
  page. Admission needs 64 KiB free / 16 KiB contiguous internal heap. A fresh
  merge budget check can move the group directly to the next page without
  allocating another array; page, footnote and publisher-marker limits remain
  native. Oversized groups release their lookahead at the first page boundary.
- Publisher background compositing uses a 128-byte row tile and clips work to
  the active e-ink strip. Explicit white boxes mask parent shading; solid ink and
  opaque pictures are protected during text antialiasing passes.
- The source page index grows by 16 bytes per page, allocated in 2 KiB chunks.
  Optional reading-position extensions preserve original source coordinates;
  existing progress records remain readable. Section and CSS caches are versioned
  separately and rebuild automatically, without deleting books or progress.

### Simulator dependency

`platformio.ini` pins `elijahandrews/crossink-simulator` at
`7837ae22a8e81555b9aedf3e5715503068a35f29`, based on upstream
`be92ada7de24437fb5ba0cf692f17e1069792c08`. Compatibility changes add the current storage,
display and HTTP interfaces and keep WebSocket client IDs reserved through their
queued disconnect callbacks. The callback-lifetime regression passes sanitizer
checks and rejects a deliberate early-release implementation.

The companion changes are committed in the user's simulator fork, so a fresh
checkout selects the tested app/simulator pair without a personal override.
Firmware builds do not depend on the simulator.
The simulator uses real raster image decoders but does not model the physical
text-AA panel path. Its storage adapter declines trusted content-hash scans;
live Pluto sync and hardware timing still require device verification.
