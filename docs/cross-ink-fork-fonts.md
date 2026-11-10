# Font behavior in the CrossInk fork

This adapts the font and typography behavior from Witch Hunt `master`
`f216bfcce608ec32d85f3dfa9f74fbc7afdc0bce` to CrossInk's font providers,
renderer and publisher-font modes. It does not replace the reader or settings UI.
The initial replay's font limitations are superseded by this parity work.

## Selection and loading

`BookFontSystem` resolves the CSS catalog's ordered family stacks. It tries
named members and their declared sources in order, ranks face style/weight,
and stops at the first usable family or generic fallback. Quoted family names
remain distinct from generic tokens. Serif preserves an available reader serif;
sans uses the native built-in provider. Monospace uses the four licensed
[ReaderMono](../lib/ReaderMono/README.md) faces, generated from pinned Roboto Mono
sources with the included SIL Open Font License. Exhausted stacks use the reader
font. Publisher Off/Accents/All selection remains native CrossInk behavior.

Publisher files are extracted through the fresh ZIP directory, size and CRC
validation path before registration. Adobe obfuscation is length preserving;
SDK-protected resources are validated in their raw ZIP form and measured after
decryption. Missing sources and unsupported formats can fall through; transient
read, write, close and allocation failures remain eligible for explicit retry.
The catalog itself is not treated as empty after a failed cold load.

The bounded manager admits 64 face descriptors, 32 open sources and 32 family-size
registrations. A registration publishes all available styles atomically. Failure
rolls back descriptors created by the attempt without removing a size still
borrowed by another registration. Eviction protects the two most recently used
registrations. Requested/loaded face masks are 64 bits, and layout identities
include the catalog, selected source fingerprints and rendering revision.

## Outline fonts and shaping

Native FreeType rendering supports the source's TTF/CFF/CFF2 and variable-font
paths. Selected point size controls optical sizing; publisher weight/style select
the corresponding supported axes. GDEF variation deltas contribute to GPOS
kerning. Streamed publisher faces retain bounded GSUB/GPOS/GDEF tables instead of
silently disabling kerning and substitutions. Their combined table admission is
512 KiB per face; publisher files are limited to 8 MiB. Small publisher files may
reside in memory up to 256 KiB; larger ones use the native stream provider and
its heap reserve.

Authored `smcp` and `c2sc` support `small-caps` and `all-small-caps`, with the
source's synthetic fallback where an alternate is absent. Standard ligatures
include direct two- and three-component substitutions as well as native pair
chaining; `font-variant-ligatures:none` disables them consistently during
measurement and drawing. This is the bounded source feature set, not a general
complex-script shaping engine.

On scalable-font builds, point sizes are carried at 1/64-point precision from CSS through pagination,
cache records, drawing, dictionary geometry and zoom previews. CSS may resolve
from 1/64 to 128 points independently of the reader's selectable body-size bank.
Inherited sizing, first-line overrides, caps and ligature policy are retained.
Superscripts/subscripts carry their computed size so the renderer does not halve
it a second time; ruby retains its own native reduction. Preview baselines and
line heights account for the resolved word fonts.

## Bitmap and raster quality

Existing cpfont v4 remains readable. V5/v6 add validated HD source metrics and
pixels, 1/32-pixel advances, wide kerning and authored small-cap mappings. SD
prewarming includes needed cap alternates, and optional small content faces
admit the normalized bitmap size rather than rejecting a larger HD source buffer.

On PSRAM/scalable-font builds, fractional CSS sizes also work for built-in and SD bitmap families. A bounded
PSRAM adapter borrows existing source families and retains 32 size registrations,
128 coverage-cache entries and a 64 KiB coverage arena. Removing a source first
invalidates its borrowers. SD prewarming still targets the original source.
Constrained C3 builds retain the native nearest-installed bitmap-size fallback;
they do not allocate this PSRAM adapter or open publisher outline fonts.

Bitmap resampling starts from the original source raster, including raw HD
pixels, and combines CSS sizing with synthetic caps or script reduction before
one coverage quantization. It integrates pixel area and uses nearest-even ties.
Outline reductions rasterize directly on the final pixel grid while preserving
the selected optical instance. Fractional pen phase, bearings and advance
remainders are shared by measurement and drawing. Off-strip glyphs are culled
before requesting an outline raster. The widened fixed-point implementation
handles tiny sizes and extreme valid bearings without signed overflow.

Native scalable size caches also use bounded, atomic registration. Layout and
selection retain the stable reader ID, family token and exact point size rather
than storing an evictable derived ID. Network transitions release optional font
and bitmap workspaces through the native lifecycle. Temporary failures advance
the renderer's failure epoch and participate in the section retry-marker policy;
a graceful fallback page is not accepted later as a definitive publisher layout.

## Verification

The final focused font checkpoint passed 48 ASan/UBSan checks covering actual
cpfont loading, production renderer pixels, resident and streamed outline paths,
variable/caps instances, exact bitmap sizing, first-line/script metadata,
preview zoom, bounded size caches and publisher registration. Pixel comparisons
include odd bearings, fractional phases, reduced coverage and raw HD source
sampling. Adversarial fixtures exercise tiny sizes and extreme bearings.

A later 61-case layout/raster checkpoint also passed, including the unchanged
native legacy script-offset and linear baseline-measurement assertions, plus
resolved-script and first-line regressions. It overlaps the earlier raster
checkpoint; these counts are not additive.

Fault injection covers partial style registration, borrowed-source rollback,
catalog/source recovery, allocation refusal and explicit file-close failure.
Isolated negative controls reproduce the former bitmap inverse-step overflow
and the ignored file-close failure. A separate production dictionary
measurement/highlight harness passed with all
three typography bits. C3 selection records remain 44 bytes; scalable builds use
52 bytes. Synthetic fixtures are original and generated locally; no private font
or book is needed. Full branch validation is recorded separately in
[EPUB parity](epub-parity.md) and the main fork document.

These checks establish software behavior and bounded host paths. They do not
measure physical e-ink appearance, device latency, long-run heap behavior or
perceived rendering quality on the reader.
