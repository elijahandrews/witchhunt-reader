# CrossInk fork: local improvements

This branch starts at CrossInk `development` commit
`4a39788657825ce470d5674b8df0da206a8eb75f`, with FreeInk SDK base
`684ea6073f209825bacf81459902298e9cf139b4`. The source for the local
Witch Hunt improvements is `f216bfcce608ec32d85f3dfa9f74fbc7afdc0bce`.
CrossInk's native drawer, settings panels, home themes, publisher-font modes,
activities, HAL and EPUB engine remain the base of the fork.

The integrated SDK changes are committed at
`9ab08c0890d1d10c5ea661aeca4954657464fd3b` in
`elijahandrews/freeink-sdk`; the parent submodule URL and gitlink select that
revision. The matching simulator is pinned separately in `platformio.ini`.

The histories have no common Git merge base. Improvements are adapted at their
native CrossInk seams; copying entire Witch Hunt implementation commits would
replace unrelated upstream behavior. Source review follows the reachable
user-authored history, excluding the reverted dark-mode experiment and
unmerged experiments.

## Port map

| Improvement | CrossInk integration |
| --- | --- |
| HD bitmap fonts and glyph coverage | cpfont v5/v6 decoding, integrity checks, bearing-aligned area coverage, single shade quantization, fractional metrics, wide kerning and authored small capitals. Existing v4 fonts keep their format. |
| Scalable font quality | Fractional origin phase in the SDK rasterizer and CrossInk glyph cache; authored small capitals and light hinting by default for unset profiles. Existing per-family rendering choices remain. |
| Single-pass text refresh | The measured X4 Pro UC8279 LUT68 text path is adapted to CrossInk's synchronous single-buffer facade and retained grayscale planes. Text-only EPUB/TXT pages use it; images and unsupported panels retain native paths. Clean-refresh cadence, inversion, allocation failures and uncertain controller history retain ordinary refresh fallbacks. |
| Pluto library sync | Native File Transfer entry with the existing private HTTPS helper protocol, `/Books` downloads, complete content verification, no overwrite/deletion, cancellation, hardware SHA, size filtering, bounded I/O and conservatively sealed scan reuse. See [book-sync.md](book-sync.md). The reader does not speak SMB directly. |
| Reading location after reflow | Witch progress is imported read-only and mapped from raw source offsets into native CrossInk visible-text coordinates. New layouts and progress use their own cache namespace. |
| Publisher page margins | Global and per-book control for publisher html/body margins and padding; ordinary nested block spacing remains. Extra paragraph spacing defaults off. |
| Touch lookup and navigation | Page/contact-bound word holds and native navigation improvements are ported into CrossInk's interaction flow. |
| Font-size responsiveness | Resident native font/cache reuse is retained. Every effective gesture/button size change shows CrossInk's existing loading feedback before font reload, persistence and reflow. Clamped or failed changes stay silent. |
| Existing equivalents | CrossInk already has status-bar tap toggling, single input sampling in USB mode, publisher-font modes, scalable size registration and batched visible-position lookups. Font caching and image warming use CrossInk's own cache architecture; the later EPUB parity port adds cancellable lookahead image preparation there. |

Full-book Unicode phrase search and precise Go back navigation use CrossInk's
native screen stack, ZIP streaming and visible-text coordinates. See
[reader behavior and migration](cross-ink-fork-reader.md). Variable TTF/CFF2
publisher fonts select optical size, weight and italic axes through the native
FreeType engine; GDEF variation deltas are included in GPOS kerning.

Publisher typography includes inherited Unicode upper/lowercase transforms,
computed letter spacing shared by pagination/drawing/touch geometry, ordered
named-family fallback, serif/sans/monospace selection, authored small/all-small
capitals, bounded general ligatures and fractional CSS sizes for outline and
bitmap fonts. Streamed outline faces retain bounded layout tables. Chapter-local
styles and font faces use the native CSS catalog. See
[font behavior and resource limits](cross-ink-fork-fonts.md).

General viewport-media evaluation and complex-script OpenType shaping are outside
the source reference's behavior. The complete behavior inventory, integration
status and validation are tracked in [EPUB parity](epub-parity.md). The initial
replay validation below remains distinct from that later parity work.

## State ownership

CrossInk already uses `/.crosspoint/crossink-settings.json`. This fork also uses
`crossink-state.json`, `crossink-wifi.json` and `crossink-recent.json`, and
`crossink_epub_`, `crossink_txt_` and `crossink_xtc_` cache directories.
Legacy files are read as migration sources, including an interrupted legacy
state/Wi-Fi `.bak` when its canonical file is absent. They are not renamed or
overwritten. An existing new-format store takes precedence even when damaged,
so a corrupt new save cannot silently resurrect stale reading state.

The Pluto pairing remains `/.crosspoint/book-sync.json` and
`/.crosspoint/book-sync-ca.pem`. Both, and the Wi-Fi stores, are excluded from
web/plugin file access. Books, fonts, dictionaries and private device data do
not belong in this repository.

## Initial replay validation and installed firmware

The following results describe the earlier replay and installed firmware. The
later EPUB parity changes are built and tested separately in
[EPUB parity](epub-parity.md); that update was subsequently installed on the Picco
on 2026-11-09 with a fresh backup of its current books and settings.

The reviewed X4 Pro candidate is 5,771,136 bytes, with
782,464 bytes of headroom in the unchanged 6,553,600-byte application
slot. Static RAM is 117,820 bytes. SHA-256:
`83ed29c6337eec86178244f699e644de5fd71cf0cbb2e62fca3bcd27ccce3aef`.

The full host suite completed with 2,344 passes, zero failures and three skips
(two inherited negative-transport cases and an optional private-font test).
An additional 198 parser/search checks and 28 rendering/layout checks passed
under ASan/UBSan. Focused font, HTTP, storage and SDK driver checks also passed.
Independent review/fix/review loops returned clean for every ported unit.
Configured cppcheck completed successfully; it retains style advisories and
8 pre-existing member-initialization advisories in unchanged upstream
`AssetCompanionStore.cpp`. No new correctness finding remains in the port.

Test-only compatibility fixes include stale image-cache fixture paths, missing
native test stubs, boot-path extraction and generated-language key completeness.
The image fixture failure was reproduced on the untouched CrossInk base before
fixing the fixture; the production image path was not altered for it.

Host checks include production cpfont loading, synthetic glyph/outline pixel
comparisons, SD mutation/handle accounting, strict-CA HTTP session behavior,
reader refresh routing, migration and SDK controller command/selector tests.
The display selector oracle exercises 80 frames, exact LUT gating, transitions,
allocation failures and failed-completion recovery in both buffer configurations.

The Picco build was installed on 2026-11-09. Its 5,756,640-byte application has
SHA-256 `5a66b01eb2602635c74964e617599f730039258850f4f93bc42aa92600900390`.
The application and all protected flash outside the app partition were verified
independently; runtime reported `picco`, version `1.6.1-picco`, without a detected
crash. BOOX recovery, books and that device's own settings remained in place.
Its compatible native progress, reader preferences, stats and sync records were
copied into the new cache namespace from a verified backup of that BOOX only.

The first installed candidate exposed a storage-wrapper regression: newly
created files were write-only, while incremental EPUB layout reads pages back
through its open writer. The corrected image restores the SDK's read/write
contract. Nine production HAL sanitizer tests pass, and deliberately restoring
the bad access mode reproduces the exact page-header read failure.

Physical book rendering, touch behavior, display appearance/timing, long-run
ghosting and live sync performance still need device validation. The X4
installation and `crossink-ui` worktree remain separate.

## Source state

The application pins the committed SDK integration through its submodule
gitlink and the simulator companion through `platformio.ini`. Both dependencies
are published in the user's forks. Exact source overlays and build evidence are
retained in the ignored shared device workspace.

## Hardware check after an authorized install

Open the same book in the native drawer and settings screens, change font size,
and exercise dictionary selection on tracked/italic text. Compare publisher
fonts Off/Accents/All, search and Go back after reflow, and a Pluto sync followed
by a second zero-addition sync. Check heap and task-stack high-water marks with
both builtin families and publisher faces loaded. Observe real text/image page
transitions, grayscale density and ghosting; host framebuffers do not establish
panel quality or timing. The new cache namespace rebuilds layouts automatically.
