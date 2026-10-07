# Publisher font manager integration checks

This target compiles the production `EpubFontManager`, `CssFontCatalog`,
`OutlineFontFace`, vendored FreeType modules, OpenType helpers, and bundled
monospace faces. It extracts original diagnostic font fixtures from the checked-in
typography test EPUB using Python's standard library. No downloaded commercial
fonts or system FreeType library are used.

```sh
cmake -S test -B /tmp/witch-host
cmake --build /tmp/witch-host --target PublisherFontsTest -j4
/tmp/witch-host/publisher_fonts/PublisherFontsTest
```

The EPUB boundary and renderer registration API are small test doubles: extraction
copies real files and font registration retains the real `EpdFontFamily` objects.
`CssParser` is a catalog-accessor double; catalog parsing/storage and all font
selection, loading, shaping, metrics, and rasterization below the manager are
production code. These tests complement the real renderer and EPUB parser tests;
they do not verify ZIP parsing, physical pixels, or the device's SD timing.

Coverage includes four faces, absent/corrupt source fallbacks, transient extraction
recovery with fingerprint invalidation, ordered family
fallback, static-face reuse, variable weight, actual CSS-size optical coordinates,
source size bounds, independent TrueType/CFF design-unit geometry, byte-sensitive fingerprints, same-count catalog replacements,
identical catalog reloads, equal monospace advances, face-count and aggregate-memory-pressure revisits with
identical pixels/advances, book reopening, and resolver/registration cleanup. A checksum-valid padded GPOS fixture exercises
the real 3 MiB budget without mocking the allocator.

For ASan/UBSan, configure a separate build with
`-DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'` and matching
`CMAKE_CXX_FLAGS` and `CMAKE_EXE_LINKER_FLAGS`. This instruments the vendored C
rasterizer as well as the C++ manager. Python `fonttools` is needed only to
regenerate the original EPUB fixture, not to build or run this target.
