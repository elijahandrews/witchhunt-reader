# SD font format

`fontconvert_sdcard.py` writes uncompressed, four-level grayscale `.cpfont`
files. Version 4 uses one raster pixel per logical pixel. Version 5 stores a
higher resolution outline raster so CSS sizing can resample it without first
shrinking a bitmap hinted at the display's base size. The currently supported
version 5 density is 2.

All integers are little-endian. Tables are packed without implicit alignment.
Offsets in the style table are absolute file offsets; glyph bitmap offsets are
relative to their style's bitmap data. A file contains a 32-byte global header,
a 32-byte entry for each included style, then the style data in table order.

## Global header

| Byte | Type | Meaning |
| --- | --- | --- |
| 0 | 8 bytes | `CPFONT\0\0` |
| 8 | uint16 | Version: 4 or 5 |
| 10 | uint16 | Flags: bit 0 means 2-bit coverage; other bits reserved |
| 12 | uint8 | Style count, 1–4 |
| 13 | uint8 | v5 raster density, currently exactly 2; v4 reserved |
| 14 | uint32 | v5 CRC-32 of every byte from offset 32 through EOF; v4 reserved |
| 18 | 14 bytes | Reserved; zero in v5 |

Version 4 writers emit zero in bytes 13–31; legacy readers need not interpret
those bytes. Version 5 readers reject unsupported density, flags, or nonzero
reserved bytes. The CRC uses the standard reflected CRC-32 exposed by
`zlib.crc32`, with its normal initialization and finalization. It covers the
style table, glyph metadata, kerning, ligatures, and all bitmap bytes. It is a
content identity/checksum, not a signature. A loader can include the header and
style table in its cache key to distinguish changes to a same-named font.

## Style table entry

Styles are 0 regular, 1 bold, 2 italic, and 3 bold italic. IDs must be unique.

| Byte | Type | Meaning |
| --- | --- | --- |
| 0 | uint8 | Style ID |
| 1 | 3 bytes | Reserved, zero |
| 4 | uint32 | Unicode interval count |
| 8 | uint32 | Glyph count |
| 12 | uint8 | Line advance, in raster pixels |
| 13 | int16 | Ascender, in raster pixels |
| 15 | int16 | Descender, in raster pixels |
| 17 | uint16 | Left kerning entry count |
| 19 | uint16 | Right kerning entry count |
| 21 | uint8 | Left kerning class count |
| 22 | uint8 | Right kerning class count |
| 23 | uint8 | Ligature entry count |
| 24 | uint32 | Absolute offset of this style's data |
| 28 | 4 bytes | Reserved, zero |

Each style stores these sections consecutively:

1. Unicode intervals: `uint32 first, last, firstGlyphIndex`, 12 bytes each.
2. Glyph records: 16 bytes each, as below.
3. Left kerning entries: `uint16 codepoint, uint8 class`, 3 bytes each.
4. Right kerning entries in the same format.
5. Kerning matrix, left-class-major: `leftClassCount * rightClassCount` elements.
   An element is signed **int8 in v4**, signed **int16 in v5**. Both use four
   fractional bits in **raster pixels**. Class IDs start at 1; class 0 means no
   kerning and has no matrix row or column.
6. Ligature entries: `uint32 packedPair, uint32 replacement`, 8 bytes each. The
   first input codepoint occupies the high 16 bits of `packedPair`, the second
   the low 16 bits. Inputs and replacement are BMP codepoints.
7. Concatenated glyph bitmaps.

Intervals and kerning/ligature lookup keys are sorted. The interval ranges cover
the glyph array exactly, without overlap. Formatting controls in the converter's
default-ignorable set have empty bitmaps and zero advances.

## Glyph record

Version 5 retains every version 4 field offset and uses its two padding bytes
for the high dimension bytes. This permits mapped glyph tables without converting
the whole array. **Version 4 padding must never be decoded as dimensions.**

| Byte | Type | Meaning |
| --- | --- | --- |
| 0 | uint8 | Low byte of width |
| 1 | uint8 | Low byte of height |
| 2 | uint16 | Horizontal advance, unsigned fixed point with four fractional bits |
| 4 | int16 | Left bearing, raster pixels |
| 6 | int16 | Top bearing above the baseline, raster pixels |
| 8 | uint16 | Bitmap byte length |
| 10 | uint8 | v5 high byte of width; v4 padding |
| 11 | uint8 | v5 high byte of height; v4 padding |
| 12 | uint32 | Offset within the style's bitmap data |

For v5, `width = record[0] + 256 * record[10]` and
`height = record[1] + 256 * record[11]`. The byte length still limits each glyph
to 65,535 bytes. The converter rejects dimensions, advances, bearings, bitmap
lengths, or kerning values outside their field ranges. v5 does not clamp kerning
or silently drop excessive kerning classes or ligature entries. Existing v4
kerning clamping is retained for byte compatibility and reported as a warning.

Pixels are packed in scan order, four per byte, most significant pair first.
Rows are **not** padded to byte boundaries. Codes 0, 1, 2, 3 represent increasing
coverage from transparent to full ink. The byte length is
`ceil(width * height / 4)`; unused low bits of the last byte are zero. Display
plane encoding and physical e-ink gray levels are separate from this coverage.

## Raster density and conversion

The requested point size remains the logical size in the filename and font menu.
With density 2, a logical 14-point font is rasterized at 28 points, at 150 DPI.
All stored metrics, including kerning, describe this larger raster. The renderer
uses `CSS scale / raster density` for bitmap geometry and scales advances,
kerning, and line metrics consistently. Bearings define a common baseline;
transparent cropping must not change pixel phase.

The default invocation remains v4-compatible, with forced autohinting and the
historical four coverage bins. `--raster-density 2` selects outline-only FreeType
LIGHT rendering and nearest-level quantization to coverages 0, 85, 170, 255.
LIGHT limits horizontal grid fitting before resampling. This is a practical
improvement over shrinking an already hinted low-resolution bitmap; it does not
promise pixel identity with rasterizing outlines at every final CSS size.
See FreeType's [load target documentation](https://freetype.org/freetype2/docs/reference/ft2-glyph_retrieval.html#ft_load_target_xxx)
and [grid-fitting explanation](https://freetype.org/freetype2/docs/glyphs/glyphs-3.html).
No LCD gamma assumption is applied to the e-ink panel.

```sh
python lib/EpdFont/scripts/fontconvert_sdcard.py \
  --intervals reading,greek --sizes 10,12,14,16,18,20,22,24 \
  --raster-density 2 --name ExampleSerif \
  --regular /path/to/Example-Regular.ttf \
  --bold /path/to/Example-Bold.ttf \
  --italic /path/to/Example-Italic.ttf \
  --bolditalic /path/to/Example-BoldItalic.ttf \
  --output-dir /path/to/output/ExampleSerif
```

Run the standalone converter checks with an environment containing `freetype-py`
and `fonttools`:

```sh
python test/fontconvert_sdcard/test_fontconvert_sdcard.py
```

The tests construct original outlines in a temporary directory, independently
read the emitted byte layout, and compare glyphs with direct FreeType output.
They also exercise wide glyphs/kerning, CRC coverage, overflow errors, signed
bitmap pitch, logical-size naming, and unchanged default v4 output.
