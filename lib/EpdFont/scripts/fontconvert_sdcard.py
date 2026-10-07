#!/usr/bin/env python3
"""Generate .cpfont binary files for SD card font loading.

Outputs binary .cpfont files containing glyph metadata and uncompressed
2-bit bitmaps, matching the EpdFontData/EpdGlyph/EpdUnicodeInterval struct
layout on the supported devices (little-endian).

The default density preserves v4 output. --raster-density 2 writes v5
with a 2x LIGHT-hinted outline raster for baseline-aligned resampling.
--small-caps adds v6 authored smcp/c2sc alternates. Variable-font optical/style
instancing is explicit and uses logical point sizes before rasterization.
See docs/cpfont-format.md for the byte layout and metric units.

Usage:
    # Single file with specific presets
    python fontconvert_sdcard.py \\
      --intervals latin-ext,greek,cyrillic \\
      --size 14 --style regular \\
      NotoSans-Regular.ttf \\
      -o NotoSansExt_14.cpfont

    # All 4 sizes at once
    python fontconvert_sdcard.py \\
      --intervals cjk \\
      --sizes 12,14,16,18 --style regular \\
      NotoSansCJKsc-Regular.otf \\
      --output-dir NotoSansCJK/

"""

import freetype
import zlib
import struct
import sys
import os
import math
import argparse
import binascii
import tempfile
from contextlib import contextmanager
from collections import namedtuple

from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

try:
    from PIL import Image
except ImportError:
    Image = None

# --- Unicode interval presets ---

INTERVAL_PRESETS = {
    "ascii":       [(0x0020, 0x007E)],
    "latin1":      [(0x0080, 0x00FF)],
    "latin-ext":      [(0x0020, 0x007E), (0x0080, 0x00FF), (0x0100, 0x024F),
                        (0x1E00, 0x1EFF), (0x2000, 0x206F)],
    "latin-cyrillic": [(0x0020, 0x007E), (0x0080, 0x00FF), (0x0100, 0x024F),
                        (0x1E00, 0x1EFF), (0x2000, 0x206F),
                        (0x0400, 0x04FF), (0x0500, 0x052F),
                        (0x1C80, 0x1C8F), (0x2DE0, 0x2DFF), (0xA640, 0xA69F)],
    # Pronunciation notation. Dictionaries write it in IPA Extensions plus the
    # stress and length marks from Spacing Modifier Letters, and reach into Greek
    # for theta and chi; the rest of IPA's letters are already in Latin-1 and
    # Latin Extended-A/B. A dictionary using it is unreadable without these --
    # measured on PONS En-De, U+02C8 (primary stress) alone appears 73k times.
    "ipa":            [(0x0250, 0x02AF), (0x02B0, 0x02FF), (0x03B2, 0x03B2),
                        (0x03B8, 0x03B8), (0x03C7, 0x03C7)],
    "greek":          [(0x0370, 0x03FF), (0x1F00, 0x1FFF)],
    "greek-letters":  [(0x0370, 0x03FF)],
    "musical-symbols": [(0x2660, 0x266F), (0x1D100, 0x1D1FF)],
    "cyrillic":       [(0x0400, 0x04FF), (0x0500, 0x052F),
                        (0x1C80, 0x1C8F), (0x2DE0, 0x2DFF), (0xA640, 0xA69F)],
    "georgian":       [(0x10A0, 0x10FF), (0x2D00, 0x2D2F)],
    "armenian":    [(0x0530, 0x058F)],
    "ethiopic":    [(0x1200, 0x137F), (0x1380, 0x139F), (0x2D80, 0x2DDF)],
    "vietnamese":  [(0x01A0, 0x01B0), (0x1EA0, 0x1EF9)],
    "punctuation": [(0x2000, 0x206F)],
    "cjk":         [(0x3000, 0x303F), (0x3040, 0x309F), (0x30A0, 0x30FF),
                    (0x4E00, 0x9FFF), (0xF900, 0xFAFF), (0xFF00, 0xFFEF)],
    "hangul":      [(0xAC00, 0xD7AF), (0x1100, 0x11FF), (0x3130, 0x318F)],
    # Matches the built-in font intervals from fontconvert.py exactly
    "builtin":     [(0x0000, 0x007F), (0x0080, 0x00FF), (0x0100, 0x017F),
                    (0x01A0, 0x01A1), (0x01AF, 0x01B0), (0x01C4, 0x021F),
                    (0x0300, 0x036F), (0x0400, 0x04FF),
                    (0x1EA0, 0x1EF9), (0x2000, 0x206F), (0x20A0, 0x20CF),
                    (0x2070, 0x209F), (0x2190, 0x21FF), (0x2200, 0x22FF),
                    (0xFB00, 0xFB06)],
    # Greek for physics terms, math operators, geometric shapes, uncommon
    # dialogue punctuation, CJK quote marks, miscellaneous symbols (♪♫♬), dingbats.
    # Runs to 0x02FF rather than 0x024F so dictionary pronunciation notation
    # (IPA Extensions + the stress marks) renders too -- see the "ipa" preset.
    "reading":     [(0x0020, 0x02FF), (0x0300, 0x036F), (0x0370, 0x03FF),
                    (0x0400, 0x04FF), (0x1E00, 0x1EFF), (0x2000, 0x206F),
                    (0x2070, 0x209F), (0x20A0, 0x20CF), (0x2150, 0x218F),
                    (0x2190, 0x21FF), (0x2200, 0x22FF), (0x2500, 0x257F),
                    (0x25A0, 0x25FF), (0x2600, 0x26FF), (0x2700, 0x27BF),
                    (0x2900, 0x29FF), (0x2E00, 0x2E7F), (0x3000, 0x303F),
                    (0xFB00, 0xFB06)],                    
}


# Unicode Default_Ignorable_Code_Point ranges (BMP) — must never produce ink.
# Kept byte-identical to DEFAULT_IGNORABLE_RANGES in fontconvert.py; see the long
# explanation there for why fonts cannot be trusted to leave these blank.
DEFAULT_IGNORABLE_RANGES = (
    (0x034F, 0x034F), (0x061C, 0x061C), (0x115F, 0x1160), (0x17B4, 0x17B5),
    (0x180B, 0x180F), (0x200B, 0x200F), (0x202A, 0x202E), (0x2060, 0x2064),
    (0x2065, 0x2065), (0x206A, 0x206F), (0x3164, 0x3164), (0xFE00, 0xFE0F),
    (0xFEFF, 0xFEFF), (0xFFA0, 0xFFA0), (0xFFF0, 0xFFF8),
)

def is_default_ignorable(code_point):
    """True for codepoints that must render as nothing, whatever the font says."""
    for lo, hi in DEFAULT_IGNORABLE_RANGES:
        if lo <= code_point <= hi:
            return True
        if code_point < lo:
            break
    return False

def resolve_intervals(preset_str):
    """Resolve comma-separated preset names into a merged, sorted, deduplicated interval list."""
    all_intervals = []
    for name in preset_str.split(","):
        name = name.strip().lower()
        if name not in INTERVAL_PRESETS:
            print(f"Error: unknown interval preset '{name}'", file=sys.stderr)
            print(f"Available presets: {', '.join(sorted(INTERVAL_PRESETS.keys()))}", file=sys.stderr)
            sys.exit(1)
        all_intervals.extend(INTERVAL_PRESETS[name])

    # Always add replacement character
    all_intervals.append((0xFFFD, 0xFFFD))

    # Sort and merge overlapping/adjacent intervals
    all_intervals.sort()
    merged = []
    for start, end in all_intervals:
        if merged and start <= merged[-1][1] + 1:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    return merged


GlyphProps = namedtuple("GlyphProps", [
    "width", "height", "advance_x", "left", "top", "data_length", "data_offset", "code_point"
])

# Intermediate data from rasterizing one font style
StyleRasterData = namedtuple("StyleRasterData", [
    "style_id",                # 0=regular, 1=bold, 2=italic, 3=bolditalic
    "intervals",               # validated intervals [(start, end), ...]
    "all_glyphs",              # [(GlyphProps, packed_bytes), ...]
    "total_bitmap_size",       # int
    "advanceY", "ascender", "descender",
    "kern_left_classes", "kern_right_classes", "kern_matrix",
    "kern_left_class_count", "kern_right_class_count",
    "ligature_pairs",
    "caps_data",              # optional v6 SmallCapsData
], defaults=[None])

SmallCapsData = namedtuple("SmallCapsData", [
    "smcp", "c2sc", "all_glyphs", "kern_pairs", "total_bitmap_size"
])
ALTERNATE_KEY_BASE = 0x110000

# Defaults deliberately preserve existing v4/v5 output. Pinning is opt-in.
FontInstanceOptions = namedtuple("FontInstanceOptions", [
    "optical_size", "instance_styles", "axes", "style_axes"
], defaults=["default", False, None, None])


def norm_floor(val):
    return int(math.floor(val / (1 << 6)))


def norm_ceil(val):
    return int(math.ceil(val / (1 << 6)))


# Fixed-point (fp4) output conventions (must match EpdFontData.h / fp4 namespace):
#
#   advanceX    12.4 unsigned fixed-point (uint16_t).
#               12 integer bits, 4 fractional bits = 1/16-pixel resolution.
#               Encoded from FreeType's 16.16 linearHoriAdvance.
#
#   kernMatrix  v4: 4.4 signed fixed-point (int8_t).
#               4 integer bits, 4 fractional bits = 1/16-pixel resolution.
#               Range: -8.0 to +7.9375 pixels.
#               v5: 12.4 signed fixed-point (int16_t), in raster pixels.
#               Encoded from font design-unit kerning values.
#
# Both share 4 fractional bits so the renderer can add them directly into a
# single int32_t accumulator and defer rounding until pixel placement.

def fp4_from_ft16_16(val):
    """Convert FreeType 16.16 fixed-point to 12.4 fixed-point with rounding."""
    return (val + (1 << 11)) >> 12

def fp4_from_design_units(du, scale, wide=False):
    """Round design units to raster pixels with four fractional bits.

    v4 retains its historical int8 clamp. wide=True validates the int16 v5
    range and preserves spacing rather than silently clipping adjustments.
    """
    raw = round(du * scale * 16)
    if wide:
        validate_integer("kerning adjustment", raw, -32768, 32767)
        return raw
    return max(-128, min(127, raw))


def validate_integer(label, value, minimum, maximum):
    """Reject values that cannot be represented instead of wrapping on disk."""
    if not isinstance(value, int) or not minimum <= value <= maximum:
        raise ValueError(f"{label} {value} is outside [{minimum}, {maximum}]")


def validate_raster_density(raster_density):
    if raster_density not in (1, 2):
        raise ValueError("raster density must be 1 (v4) or 2 (v5)")


def quantize_coverage(value, raster_density=1):
    """Map FreeType coverage to four levels; density 1 retains the v4 thresholds."""
    return value // 64 if raster_density == 1 else (value * 3 + 127) // 255


def pack_freetype_bitmap(bitmap, raster_density=1):
    """Pack row-major pixels continuously, including FreeType's signed row pitch."""
    if bitmap.width == 0 or bitmap.rows == 0:
        return b""
    if bitmap.pixel_mode != freetype.FT_PIXEL_MODE_GRAY or bitmap.num_grays != 256:
        raise ValueError("expected an 8-bit FreeType grayscale bitmap")
    # freetype-py copies the whole bitmap on every buffer property access.
    buffer = bitmap.buffer
    pitch = abs(bitmap.pitch)
    if pitch < bitmap.width or len(buffer) < pitch * bitmap.rows:
        raise ValueError("FreeType bitmap pitch or buffer is shorter than its dimensions")
    pixels = bytearray(bitmap.width * bitmap.rows)
    for y in range(bitmap.rows):
        row = y if bitmap.pitch >= 0 else bitmap.rows - 1 - y
        for x in range(bitmap.width):
            pixels[y * bitmap.width + x] = quantize_coverage(buffer[row * pitch + x], raster_density)
    return pack_2bit_bitmap(bitmap.width, bitmap.rows, pixels)


def write_png_gray(path, width, height, pixels):
    def png_chunk(chunk_type, data):
        chunk = chunk_type + data
        return struct.pack("!I", len(data)) + chunk + struct.pack("!I", binascii.crc32(chunk) & 0xffffffff)

    ihdr = struct.pack("!IIBBBBB", width, height, 8, 0, 0, 0, 0)
    raw_scanlines = bytearray()
    for y in range(height):
        raw_scanlines.append(0)
        raw_scanlines.extend(pixels[y * width:(y + 1) * width])

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(png_chunk(b"IHDR", ihdr))
        f.write(png_chunk(b"IDAT", zlib.compress(bytes(raw_scanlines), level=9)))
        f.write(png_chunk(b"IEND", b""))


STYLE_LABELS = {0: "regular", 1: "bold", 2: "italic", 3: "bolditalic"}


def expand_processed_bitmap(width, height, packed, bits):
    raw = bytearray(width * height)
    pixels_per_byte = 8 // bits
    mask = (1 << bits) - 1
    for idx in range(width * height):
        byte = packed[idx // pixels_per_byte]
        shift = (pixels_per_byte - 1 - (idx % pixels_per_byte)) * bits
        raw[idx] = (byte >> shift) & mask
    return raw


def pack_2bit_bitmap(width, height, pixels):
    packed = bytearray(((width * height) + 3) // 4)
    for idx, value in enumerate(pixels):
        shift = (3 - (idx % 4)) * 2
        packed[idx // 4] |= (value & 3) << shift
    return bytes(packed)


def dilate_2bit_bitmap(width, height, pixels):
    out = bytearray(len(pixels))
    for y in range(height):
        for x in range(width):
            maxv = 0
            for dy in (-1, 0, 1):
                ny = y + dy
                if ny < 0 or ny >= height:
                    continue
                for dx in (-1, 0, 1):
                    nx = x + dx
                    if nx < 0 or nx >= width:
                        continue
                    maxv = max(maxv, pixels[ny * width + nx])
            out[y * width + x] = maxv
    return out


def glyph_pixels_2bit(entry):
    glyph, packed = entry
    return expand_processed_bitmap(glyph.width, glyph.height, packed, 2)


def glyph_entries_by_codepoint(sd, code_point):
    return (entry for entry in sd.all_glyphs if entry[0].code_point == code_point)


def style_uses_synthetic_bold(base_sd, target_sd):
    sample_codepoints = [ord('A'), ord('a'), ord('g'), ord('0')]
    for code_point in sample_codepoints:
        base_entry = next(glyph_entries_by_codepoint(base_sd, code_point), None)
        target_entry = next(glyph_entries_by_codepoint(target_sd, code_point), None)
        if base_entry is None or target_entry is None:
            return False
        base_glyph, base_packed = base_entry
        target_glyph, target_packed = target_entry
        if base_glyph.width != target_glyph.width or base_glyph.height != target_glyph.height:
            return False
        base_pixels = expand_processed_bitmap(base_glyph.width, base_glyph.height, base_packed, 2)
        target_pixels = expand_processed_bitmap(target_glyph.width, target_glyph.height, target_packed, 2)
        if base_pixels != target_pixels:
            return False
    return True


def apply_synthetic_bold(sd, style_label):
    print(f"  Debug: synthetic bold applied for style {style_label}", file=sys.stderr)
    groups = [sd.all_glyphs] + ([sd.caps_data.all_glyphs] if sd.caps_data else [])
    for entries in groups:
        for idx, (glyph, packed) in enumerate(entries):
            if glyph.width == 0 or glyph.height == 0:
                continue
            pixels = expand_processed_bitmap(glyph.width, glyph.height, packed, 2)
            bold_pixels = dilate_2bit_bitmap(glyph.width, glyph.height, pixels)
            entries[idx] = (glyph, pack_2bit_bitmap(glyph.width, glyph.height, bold_pixels))


def save_debug_glyph_image(output_path, raster_data):
    base = os.path.splitext(output_path)[0]
    found = False
    for style_id, sd in raster_data.items():
        style_label = STYLE_LABELS.get(style_id, str(style_id))
        png_path = f"{base}_A_{style_label}.png"
        glyph_entry = next(((g, p) for g, p in sd.all_glyphs if g.code_point == ord('A')), None)
        if glyph_entry is None:
            print(f"  Debug: letter 'A' not found for style {style_label}", file=sys.stderr)
            continue
        glyph, packed = glyph_entry
        if glyph.width == 0 or glyph.height == 0:
            print(f"  Debug: letter 'A' has empty bitmap for style {style_label}", file=sys.stderr)
            continue
        unpacked = expand_processed_bitmap(glyph.width, glyph.height, packed, 2)
        img_pixels = bytes(255 - p * 85 for p in unpacked)
        if Image:
            img = Image.frombytes("L", (glyph.width, glyph.height), img_pixels)
            img.save(png_path)
        else:
            write_png_gray(png_path, glyph.width, glyph.height, img_pixels)
        print(f"  Debug: saved letter 'A' bitmap to {png_path}", file=sys.stderr)
        found = True
    if not found:
        print(f"  Debug: no letter 'A' glyphs were saved for {output_path}", file=sys.stderr)


# Standard Unicode ligature codepoints for known input sequences.
# Used as a fallback when the GSUB substitute glyph has no cmap entry.
STANDARD_LIGATURE_MAP = {
    (0x66, 0x66):       0xFB00,  # ff
    (0x66, 0x69):       0xFB01,  # fi
    (0x66, 0x6C):       0xFB02,  # fl
    (0x66, 0x66, 0x69): 0xFB03,  # ffi
    (0x66, 0x66, 0x6C): 0xFB04,  # ffl
    (0x17F, 0x74):      0xFB05,  # long-s + t
    (0x73, 0x74):       0xFB06,  # st
}


def _extract_pairpos_subtable(subtable, glyph_to_cp, raw_kern):
    """Extract kerning from a PairPos subtable (Format 1 or 2)."""
    if subtable.Format == 1:
        # Individual pairs
        for i, coverage_glyph in enumerate(subtable.Coverage.glyphs):
            if coverage_glyph not in glyph_to_cp:
                continue
            pair_set = subtable.PairSet[i]
            for pvr in pair_set.PairValueRecord:
                if pvr.SecondGlyph not in glyph_to_cp:
                    continue
                xa = 0
                if hasattr(pvr, 'Value1') and pvr.Value1:
                    xa = getattr(pvr.Value1, 'XAdvance', 0) or 0
                if xa != 0:
                    key = (coverage_glyph, pvr.SecondGlyph)
                    raw_kern[key] = raw_kern.get(key, 0) + xa
    elif subtable.Format == 2:
        # Class-based pairs — iterate by class, not by glyph, to avoid
        # O(glyphs²) explosion for CJK fonts with many requested glyphs.
        class_def1 = subtable.ClassDef1.classDefs if subtable.ClassDef1 else {}
        class_def2 = subtable.ClassDef2.classDefs if subtable.ClassDef2 else {}
        coverage_set = set(subtable.Coverage.glyphs)

        # Build reverse mappings: class_id -> list of glyph names
        left_by_class = {}   # only glyphs in coverage AND glyph_to_cp
        for glyph in glyph_to_cp:
            if glyph not in coverage_set:
                continue
            c1 = class_def1.get(glyph, 0)
            left_by_class.setdefault(c1, []).append(glyph)

        right_by_class = {}  # all glyphs in glyph_to_cp
        for glyph in glyph_to_cp:
            c2 = class_def2.get(glyph, 0)
            right_by_class.setdefault(c2, []).append(glyph)

        # Iterate class pairs (typically << glyph pairs)
        for c1, class1_rec in enumerate(subtable.Class1Record):
            if c1 not in left_by_class:
                continue
            for c2, c2_rec in enumerate(class1_rec.Class2Record):
                xa = 0
                if hasattr(c2_rec, 'Value1') and c2_rec.Value1:
                    xa = getattr(c2_rec.Value1, 'XAdvance', 0) or 0
                if xa == 0:
                    continue
                if c2 not in right_by_class:
                    continue
                for lg in left_by_class[c1]:
                    for rg in right_by_class[c2]:
                        key = (lg, rg)
                        raw_kern[key] = raw_kern.get(key, 0) + xa


def extract_kerning_fonttools(font_path, codepoints, ppem, wide=False,
                              glyph_aliases=None, alternate_only=False):
    """Extract kerning pairs from a font file using fonttools.

    Returns dict of {(leftCp, rightCp): pixel_adjust} for the given
    codepoints. Values have four fractional bits in raster pixels at ppem.
    v4 clamps to int8 for compatibility; v5 validates int16 without clamping.
    """
    font = TTFont(font_path)
    units_per_em = font['head'].unitsPerEm
    cmap = font.getBestCmap() or {}

    # Build glyph_name -> [codepoints] map (preserves aliases where multiple
    # codepoints share a glyph, e.g. space/nbsp)
    glyph_to_cps = {}
    for cp in codepoints:
        gname = cmap.get(cp)
        if gname:
            glyph_to_cps.setdefault(gname, []).append(cp)
    if glyph_aliases is not None:
        glyph_to_cps = glyph_aliases
    # Flat dict for membership checks and subtable extraction (uses keys only)
    glyph_to_cp = glyph_to_cps

    # Collect raw kerning values in font design units
    raw_kern = {}  # (left_glyph_name, right_glyph_name) -> design_units

    # 1. Legacy kern table
    if 'kern' in font:
        for subtable in font['kern'].kernTables:
            if hasattr(subtable, 'kernTable'):
                for (lg, rg), val in subtable.kernTable.items():
                    if lg in glyph_to_cp and rg in glyph_to_cp:
                        raw_kern[(lg, rg)] = raw_kern.get((lg, rg), 0) + val

    # 2. GPOS 'kern' feature
    if 'GPOS' in font:
        gpos = font['GPOS'].table
        kern_lookup_indices = set()
        if gpos.FeatureList:
            for fr in gpos.FeatureList.FeatureRecord:
                if fr.FeatureTag == 'kern':
                    kern_lookup_indices.update(fr.Feature.LookupListIndex)
        for li in kern_lookup_indices:
            lookup = gpos.LookupList.Lookup[li]
            for st in lookup.SubTable:
                actual = st
                # Unwrap Extension (lookup type 9) wrappers. After unwrapping,
                # `lookup.LookupType` is still 9 and the unwrapped subtable
                # carries `Format` rather than `LookupType`, so the *effective*
                # type for the dispatch below comes from `st.ExtensionLookupType`.
                if lookup.LookupType == 9 and hasattr(st, 'ExtSubTable'):
                    actual = st.ExtSubTable
                effective_type = getattr(st, 'ExtensionLookupType', lookup.LookupType)
                if hasattr(actual, 'Format'):
                    if effective_type == 2:
                        _extract_pairpos_subtable(actual, glyph_to_cp, raw_kern)
                    else:
                        print(f"  Debug: skipping unsupported GPOS kern lookupType="
                              f"{effective_type} (outer={lookup.LookupType}, Format={actual.Format})",
                              file=sys.stderr)

    font.close()

    # Scale design-unit kerning values to 4.4 fixed-point pixels.
    # Expand glyph aliases: if multiple codepoints share a glyph, emit kern
    # pairs for all codepoint combinations.
    scale = ppem / units_per_em
    result = {}  # (leftCp, rightCp) -> 4.4 fixed-point adjust
    clamped_pairs = 0
    for (lg, rg), du in raw_kern.items():
        adjust = fp4_from_design_units(du, scale, wide=wide)
        if not wide and adjust != round(du * scale * 16):
            clamped_pairs += 1
        if adjust != 0:
            for lcp in glyph_to_cps[lg]:
                for rcp in glyph_to_cps[rg]:
                    if not alternate_only or lcp >= ALTERNATE_KEY_BASE or rcp >= ALTERNATE_KEY_BASE:
                        result[(lcp, rcp)] = adjust
    if clamped_pairs:
        print(f"  WARNING: v4 clamped {clamped_pairs} glyph kerning pairs to int8; "
              "--raster-density 2 uses the wider v5 matrix", file=sys.stderr)
    return result


def derive_kern_classes(kern_map, strict=False):
    """Derive class-based kerning from a pair map.

    Returns (kern_left_classes, kern_right_classes, kern_matrix,
             kern_left_class_count, kern_right_class_count) where:
    - kern_left_classes: sorted list of (codepoint, classId) tuples
    - kern_right_classes: sorted list of (codepoint, classId) tuples
    - kern_matrix: flat list of signed fixed-point values (left_class_count * right_class_count)
    - kern_left_class_count: number of distinct left classes
    - kern_right_class_count: number of distinct right classes
    """
    if not kern_map:
        return [], [], [], 0, 0

    all_left_cps = {lcp for lcp, _ in kern_map}
    all_right_cps = {rcp for _, rcp in kern_map}

    sorted_right_cps = sorted(all_right_cps)
    sorted_left_cps = sorted(all_left_cps)

    # Group left codepoints by identical adjustment row
    left_profile_to_class = {}
    left_class_map = {}
    left_class_id = 1
    for lcp in sorted(all_left_cps):
        row = tuple(kern_map.get((lcp, rcp), 0) for rcp in sorted_right_cps)
        if row not in left_profile_to_class:
            left_profile_to_class[row] = left_class_id
            left_class_id += 1
        left_class_map[lcp] = left_profile_to_class[row]

    # Group right codepoints by identical adjustment column
    right_profile_to_class = {}
    right_class_map = {}
    right_class_id = 1
    for rcp in sorted(all_right_cps):
        col = tuple(kern_map.get((lcp, rcp), 0) for lcp in sorted_left_cps)
        if col not in right_profile_to_class:
            right_profile_to_class[col] = right_class_id
            right_class_id += 1
        right_class_map[rcp] = right_profile_to_class[col]

    kern_left_class_count = left_class_id - 1
    kern_right_class_count = right_class_id - 1

    if kern_left_class_count > 255 or kern_right_class_count > 255:
        if strict:
            raise ValueError(f"kerning class count exceeds uint8 range: "
                             f"left={kern_left_class_count}, right={kern_right_class_count}")
        print(f"WARNING: kerning class count exceeds uint8_t range "
              f"(left={kern_left_class_count}, right={kern_right_class_count}), "
              f"dropping kerning for this style",
              file=sys.stderr)
        return ([], [], [], 0, 0)

    # Build the class x class matrix
    kern_matrix = [0] * (kern_left_class_count * kern_right_class_count)
    for (lcp, rcp), adjust in kern_map.items():
        lc = left_class_map[lcp] - 1
        rc = right_class_map[rcp] - 1
        kern_matrix[lc * kern_right_class_count + rc] = adjust

    # Build sorted class entry lists
    kern_left_classes = sorted(left_class_map.items())
    kern_right_classes = sorted(right_class_map.items())

    return (kern_left_classes, kern_right_classes, kern_matrix,
            kern_left_class_count, kern_right_class_count)


def extract_ligatures_fonttools(font_path, codepoints):
    """Extract ligature substitution pairs from a font file using fonttools.

    Returns list of (packed_pair, ligature_codepoint) for the given codepoints.
    Multi-character ligatures are decomposed into chained pairs.
    """
    font = TTFont(font_path)
    cmap = font.getBestCmap() or {}

    # Build glyph_name -> codepoint and codepoint -> glyph_name maps
    glyph_to_cp = {}
    cp_to_glyph = {}
    for cp, gname in cmap.items():
        glyph_to_cp[gname] = cp
        cp_to_glyph[cp] = gname

    # Collect raw ligature rules: (sequence_of_codepoints) -> ligature_codepoint
    raw_ligatures = {}  # tuple of codepoints -> ligature codepoint

    if 'GSUB' in font:
        gsub = font['GSUB'].table

        LIGATURE_FEATURES = ('liga', 'rlig')
        liga_lookup_indices = set()
        if gsub.FeatureList:
            for fr in gsub.FeatureList.FeatureRecord:
                if fr.FeatureTag in LIGATURE_FEATURES:
                    liga_lookup_indices.update(fr.Feature.LookupListIndex)

        for li in liga_lookup_indices:
            lookup = gsub.LookupList.Lookup[li]
            for st in lookup.SubTable:
                actual = st
                # Unwrap Extension (lookup type 7) wrappers
                if lookup.LookupType == 7 and hasattr(st, 'ExtSubTable'):
                    actual = st.ExtSubTable
                # LigatureSubst is lookup type 4
                if not hasattr(actual, 'ligatures'):
                    continue
                for first_glyph, ligature_list in actual.ligatures.items():
                    if first_glyph not in glyph_to_cp:
                        continue
                    first_cp = glyph_to_cp[first_glyph]
                    for lig in ligature_list:
                        component_cps = []
                        valid = True
                        for comp_glyph in lig.Component:
                            if comp_glyph not in glyph_to_cp:
                                valid = False
                                break
                            component_cps.append(glyph_to_cp[comp_glyph])
                        if not valid:
                            continue
                        seq = tuple([first_cp] + component_cps)
                        if lig.LigGlyph in glyph_to_cp:
                            lig_cp = glyph_to_cp[lig.LigGlyph]
                        elif seq in STANDARD_LIGATURE_MAP:
                            lig_cp = STANDARD_LIGATURE_MAP[seq]
                        else:
                            seq_str = ', '.join(f'U+{cp:04X}' for cp in seq)
                            print(f"ligatures: WARNING: dropping ligature ({seq_str}) -> "
                                  f"glyph '{lig.LigGlyph}': output glyph has no cmap entry "
                                  f"and input sequence is not in STANDARD_LIGATURE_MAP",
                                  file=sys.stderr)
                            continue
                        raw_ligatures[seq] = lig_cp

    font.close()

    # Filter: only keep ligatures where all input and output codepoints are
    # in our generated glyph set
    codepoints_set = set(codepoints)
    filtered = {}
    for seq, lig_cp in raw_ligatures.items():
        if lig_cp not in codepoints_set or lig_cp > 0xFFFF:
            continue
        # The on-disk format packs each ligature component as a uint16. Drop
        # any seq with an SMP component here so the chained 3+ char path —
        # which uses `intermediate_cp = filtered[prefix].lig_cp` — also stays
        # 16-bit safe by construction.
        if any(cp > 0xFFFF for cp in seq):
            continue
        if all(cp in codepoints_set for cp in seq):
            filtered[seq] = lig_cp

    # Decompose into chained pairs
    pairs = []
    # First pass: collect all 2-codepoint ligatures
    two_char = {seq: lig_cp for seq, lig_cp in filtered.items() if len(seq) == 2}
    for seq, lig_cp in two_char.items():
        packed = (seq[0] << 16) | seq[1]
        pairs.append((packed, lig_cp))

    # Second pass: decompose 3+ codepoint ligatures into chained pairs
    for seq, lig_cp in filtered.items():
        if len(seq) < 3:
            continue
        prefix = seq[:-1]
        last_cp = seq[-1]
        if prefix in filtered:
            intermediate_cp = filtered[prefix]
            packed = (intermediate_cp << 16) | last_cp
            pairs.append((packed, lig_cp))
        else:
            print(f"ligatures: skipping {len(seq)}-char ligature "
                  f"({', '.join(f'U+{cp:04X}' for cp in seq)}) -> U+{lig_cp:04X}: "
                  f"no intermediate ligature for prefix", file=sys.stderr)

    # Sort by packed pair key — on-device lookup uses binary search
    pairs.sort(key=lambda p: p[0])
    return pairs


def parse_axis_settings(text):
    """Parse explicit OpenType axis coordinates; reject typos and duplicate tags."""
    result = {}
    if not text:
        return result
    for assignment in text.split(","):
        fields = assignment.strip().split("=")
        if len(fields) != 2 or len(fields[0]) != 4 or not fields[0].isascii():
            raise ValueError(f"invalid axis assignment {assignment!r}; expected TAG=VALUE")
        tag, raw = fields
        if tag in result:
            raise ValueError(f"duplicate axis {tag}")
        value = float(raw)
        if not math.isfinite(value):
            raise ValueError(f"axis {tag} must be finite")
        result[tag] = value
    return result


def instance_coordinates(font, logical_size, style_id, options):
    """Return fully pinned design coordinates, never the enlarged raster size.

    Optical size describes intended reading size, not raster density:
    https://learn.microsoft.com/en-us/typography/opentype/spec/dvaraxistag_opsz
    https://fonttools.readthedocs.io/en/latest/varLib/instancer.html
    """
    axes = {axis.axisTag: axis for axis in font['fvar'].axes} if 'fvar' in font else {}
    explicit = dict(options.axes or {})
    explicit.update((options.style_axes or {}).get(style_id, {}))
    optical = options.optical_size
    if optical not in ('default', 'auto'):
        optical = float(optical)
        if not math.isfinite(optical):
            raise ValueError("optical size must be finite")
        explicit.setdefault('opsz', optical)
    for tag, value in explicit.items():
        if tag not in axes:
            raise ValueError(f"font has no {tag} variation axis")
        axis = axes[tag]
        if not math.isfinite(value) or not axis.minValue <= value <= axis.maxValue:
            raise ValueError(f"axis {tag}={value} outside [{axis.minValue}, {axis.maxValue}]")
    coords = {tag: axis.defaultValue for tag, axis in axes.items()}
    if optical == 'auto' and 'opsz' in axes:
        coords['opsz'] = max(axes['opsz'].minValue, min(axes['opsz'].maxValue, logical_size))
    if options.instance_styles:
        for tag, value in [('wght', 700 if style_id & 1 else 400), ('ital', 1 if style_id & 2 else 0)]:
            if tag in axes:
                coords[tag] = max(axes[tag].minValue, min(axes[tag].maxValue, value))
    coords.update(explicit)
    return coords


@contextmanager
def instanced_font_path(fontfile, logical_size, style_id=0, options=None):
    """Use one static instance for outlines, advances, GPOS, and GSUB.

    Source files are never rewritten. With no options the original path is used
    without opening or resaving it, preserving default v4/v5 bytes.
    """
    options = options or FontInstanceOptions()
    if options == FontInstanceOptions():
        yield fontfile
        return
    with TTFont(fontfile) as font:
        coords = instance_coordinates(font, logical_size, style_id, options)
        if not coords:
            yield fontfile  # Static fonts have no automatic axes to select.
            return
        with tempfile.TemporaryDirectory(prefix='cpfont-instance-') as directory:
            instance = instantiateVariableFont(font, coords, inplace=False)
            try:
                path = os.path.join(directory, 'instance.otf')
                instance.save(path)
                print(f"  [{STYLE_LABELS[style_id]}] Instance axes: {coords}", file=sys.stderr)
                yield path
            finally:
                instance.close()


def _single_substitution(font, feature, glyph_names):
    """Resolve single-glyph GSUB features in lookup order, with first-match rules.

    Language-specific results cannot be represented by a cpfont's language-free
    sparse map. Conflicting results and contextual substitutions are rejected
    instead of silently exporting a different design.
    """
    if 'GSUB' not in font or not font['GSUB'].table.FeatureList:
        return {}
    table = font['GSUB'].table
    result = {}
    for record in table.FeatureList.FeatureRecord:
        if record.FeatureTag != feature:
            continue
        current = {name: name for name in glyph_names}
        for index in record.Feature.LookupListIndex:
            lookup = table.LookupList.Lookup[index]
            if lookup.LookupFlag:
                raise ValueError(f"{feature} lookup flags {lookup.LookupFlag} are unsupported")
            mappings = []
            for subtable in lookup.SubTable:
                actual = subtable
                kind = lookup.LookupType
                if kind == 7:
                    kind, actual = subtable.ExtensionLookupType, subtable.ExtSubTable
                if kind != 1 or not hasattr(actual, 'mapping'):
                    raise ValueError(f"{feature} requires unsupported GSUB lookup type {kind}")
                mappings.append(actual.mapping)
            for original, name in current.items():
                for mapping in mappings:
                    if name in mapping:
                        current[original] = mapping[name]
                        break
        for original, alternate in current.items():
            if original == alternate:
                continue
            if original in result and result[original] != alternate:
                raise ValueError(f"conflicting language-specific {feature} alternates for {original}")
            result[original] = alternate
    return result


def rasterize_small_caps(fontfile, face, all_cps, ppem, load_flags):
    """Export authored smcp/c2sc glyphs, including alternates without cmap entries."""
    with TTFont(fontfile) as font:
        cmap = font.getBestCmap() or {}
        source = {cp: cmap[cp] for cp in all_cps if cp in cmap and not is_default_ignorable(cp)}
        maps = []
        for tag in ('smcp', 'c2sc'):
            substitutions = _single_substitution(font, tag, set(source.values()))
            maps.append({cp: substitutions[name] for cp, name in source.items() if name in substitutions})
        names = sorted(set(maps[0].values()) | set(maps[1].values()))
        if not names:
            return None
        validate_integer("alternate glyph count", len(names), 1, 65535)
        ids = {name: i for i, name in enumerate(names)}
        glyphs, bitmap_size = [], 0
        aliases = {}
        for cp, name in source.items():
            aliases.setdefault(name, []).append(cp)
        for name, index in ids.items():
            face.load_glyph(font.getGlyphID(name), load_flags)
            bitmap = face.glyph.bitmap
            packed = pack_freetype_bitmap(bitmap, 2)
            key = ALTERNATE_KEY_BASE + index
            glyph = GlyphProps(bitmap.width, bitmap.rows, fp4_from_ft16_16(face.glyph.linearHoriAdvance),
                               face.glyph.bitmap_left, face.glyph.bitmap_top, len(packed), bitmap_size, key)
            glyphs.append((glyph, packed))
            bitmap_size += len(packed)
            aliases.setdefault(name, []).append(key)
        pairs = extract_kerning_fonttools(fontfile, all_cps, ppem, wide=True,
                                         glyph_aliases=aliases, alternate_only=True)
        return SmallCapsData(*[sorted((cp, ids[name]) for cp, name in mapping.items()) for mapping in maps],
                             glyphs, sorted((left, right, value) for (left, right), value in pairs.items()), bitmap_size)


def rasterize_font_style(fontfile, size, intervals, style_id=0, raster_density=1,
                         small_caps=False, instance_options=None):
    validate_raster_density(raster_density)
    validate_integer("logical point size", size, 1, 65535)
    if small_caps and raster_density != 2:
        raise ValueError("--small-caps requires --raster-density 2")
    with instanced_font_path(fontfile, size, style_id, instance_options) as path:
        return _rasterize_font_style(path, size, intervals, style_id, raster_density, small_caps)


def _rasterize_font_style(fontfile, size, intervals, style_id=0, raster_density=1, small_caps=False):
    """Rasterize all glyphs for one font style. Returns StyleRasterData."""
    validate_raster_density(raster_density)
    validate_integer("logical point size", size, 1, 65535)
    style_label = STYLE_LABELS.get(style_id, str(style_id))
    raster_size = size * raster_density

    face = freetype.Face(fontfile)
    # Set font size at 150 DPI (matching fontconvert.py) BEFORE any glyph load
    # — load_glyph() with FT_LOAD_RENDER renders at the active size, so calling
    # it before set_char_size() would waste work at the default size and risk
    # Invalid_Size_Handle on some fonts.
    face.set_char_size(raster_size << 6, raster_size << 6, 150, 150)

    # Default v4 output always auto-hints, matching fontconvert.py — see the long comment there. Without
    # grid-fitting, a stem's subpixel phase decides whether it quantises to a 3px or a
    # 4px ink footprint, so otherwise identical letters ship at different weights
    # (issue #149). advanceX comes from linearHoriAdvance (unhinted), so this changes
    # ink boxes only and never reflows text.
    load_flags = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_FORCE_AUTOHINT
    if raster_density == 2:
        # LIGHT avoids horizontal grid fitting before later downsampling; NO_BITMAP
        # ensures an embedded strike cannot bypass the outline rasterization.
        # https://freetype.org/freetype2/docs/reference/ft2-glyph_retrieval.html#ft_load_target_xxx
        load_flags = (freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_LIGHT |
                      freetype.FT_LOAD_NO_BITMAP)

    def load_glyph(code_point):
        glyph_index = face.get_char_index(code_point)
        if glyph_index > 0:
            face.load_glyph(glyph_index, load_flags)
            return face
        return None

    # Validate intervals: remove codepoints not present in the font
    print(f"  [{style_label}] Validating intervals against font...", file=sys.stderr)
    validated_intervals = []
    for i_start, i_end in intervals:
        start = i_start
        for code_point in range(i_start, i_end + 1):
            # Kept even when the font has no cmap entry: a gap sends getGlyph() to
            # REPLACEMENT_GLYPH (U+FFFD) and draws a box. Emitted empty below.
            if is_default_ignorable(code_point):
                continue
            f = load_glyph(code_point)
            if f is None:
                if start < code_point:
                    validated_intervals.append((start, code_point - 1))
                start = code_point + 1
        if start <= i_end:
            validated_intervals.append((start, i_end))

    intervals = validated_intervals
    total_glyphs = sum(end - start + 1 for start, end in intervals)
    print(f"  [{style_label}] Validated: {len(intervals)} intervals, {total_glyphs} glyphs", file=sys.stderr)

    # Rasterize all glyphs
    total_bitmap_size = 0
    all_glyphs = []

    for i_start, i_end in intervals:
        for code_point in range(i_start, i_end + 1):
            # Formatting controls carry no ink, whatever outline the font maps them to.
            if is_default_ignorable(code_point):
                glyph = GlyphProps(0, 0, 0, 0, 0, 0, total_bitmap_size, code_point)
                all_glyphs.append((glyph, b''))
                continue

            f = load_glyph(code_point)
            if f is None:
                glyph = GlyphProps(0, 0, 0, 0, 0, 0, total_bitmap_size, code_point)
                all_glyphs.append((glyph, b''))
                continue

            bitmap = f.glyph.bitmap
            packed = pack_freetype_bitmap(bitmap, raster_density)
            glyph = GlyphProps(
                width=bitmap.width,
                height=bitmap.rows,
                advance_x=fp4_from_ft16_16(f.glyph.linearHoriAdvance),
                left=f.glyph.bitmap_left,
                top=f.glyph.bitmap_top,
                data_length=len(packed),
                data_offset=total_bitmap_size,
                code_point=code_point,
            )
            total_bitmap_size += len(packed)
            all_glyphs.append((glyph, packed))

    # Get font metrics from pipe character (same heuristic as fontconvert.py)
    load_glyph(ord('|'))

    advanceY = norm_ceil(face.size.height)
    ascender = norm_ceil(face.size.ascender)
    descender = norm_floor(face.size.descender)

    print(f"  [{style_label}] Metrics: advanceY={advanceY}, ascender={ascender}, descender={descender}", file=sys.stderr)
    print(f"  [{style_label}] Bitmap: {total_bitmap_size} bytes ({total_bitmap_size / 1024:.1f} KB)", file=sys.stderr)

    # --- Extract kerning and ligatures ---
    ppem = raster_size * 150.0 / 72.0
    all_cps = set(g.code_point for g, _ in all_glyphs)

    kern_map = extract_kerning_fonttools(fontfile, all_cps, ppem, wide=raster_density == 2)
    # SMP codepoints (> U+FFFF) cannot be stored in the uint16 kern codepoint
    # field; drop them before class derivation to avoid struct.error.
    kern_map = {(lcp, rcp): v for (lcp, rcp), v in kern_map.items() if lcp <= 0xFFFF and rcp <= 0xFFFF}
    print(f"  [{style_label}] Kerning: {len(kern_map)} pairs extracted", file=sys.stderr)

    (kern_left_classes, kern_right_classes, kern_matrix,
     kern_left_class_count, kern_right_class_count) = derive_kern_classes(kern_map, strict=raster_density == 2)

    if kern_map:
        matrix_size = kern_left_class_count * kern_right_class_count * (2 if raster_density == 2 else 1)
        entries_size = (len(kern_left_classes) + len(kern_right_classes)) * 3
        print(f"  [{style_label}] Kerning classes: {kern_left_class_count} left, {kern_right_class_count} right, "
              f"{matrix_size + entries_size} bytes", file=sys.stderr)

    # SMP codepoints in ligature inputs / outputs are filtered inside
    # extract_ligatures_fonttools (see the codepoints_set filter), so every
    # entry returned here is already 16-bit safe.
    ligature_pairs = extract_ligatures_fonttools(fontfile, all_cps)
    if len(ligature_pairs) > 255:
        if raster_density == 2:
            raise ValueError(f"[{style_label}] {len(ligature_pairs)} ligature pairs exceed uint8 range")
        print(f"  [{style_label}] WARNING: {len(ligature_pairs)} ligature pairs exceeds uint8_t max (255), truncating",
              file=sys.stderr)
        ligature_pairs = ligature_pairs[:255]
    print(f"  [{style_label}] Ligatures: {len(ligature_pairs)} pairs", file=sys.stderr)

    return StyleRasterData(
        style_id=style_id,
        intervals=intervals,
        all_glyphs=all_glyphs,
        total_bitmap_size=total_bitmap_size,
        advanceY=advanceY,
        ascender=ascender,
        descender=descender,
        kern_left_classes=kern_left_classes,
        kern_right_classes=kern_right_classes,
        kern_matrix=kern_matrix,
        kern_left_class_count=kern_left_class_count,
        kern_right_class_count=kern_right_class_count,
        ligature_pairs=ligature_pairs,
        caps_data=rasterize_small_caps(fontfile, face, all_cps, ppem, load_flags) if small_caps else None,
    )


# --- Binary packing helpers ---

# EpdGlyph struct: 16 bytes, little-endian
GLYPH_STRUCT_FORMAT = "<BBHhhH2xI"
GLYPH_V5_STRUCT_FORMAT = "<BBHhhHBBI"
assert struct.calcsize(GLYPH_STRUCT_FORMAT) == struct.calcsize(GLYPH_V5_STRUCT_FORMAT) == 16


def pack_style_sections(sd, raster_density=1):
    """Pack one StyleRasterData into binary section bytearrays.
    Returns (intervals_data, glyphs_data, kern_left, kern_right, kern_matrix, ligatures, bitmaps)."""
    validate_raster_density(raster_density)
    validate_integer("style ID", sd.style_id, 0, 3)
    validate_integer("interval count", len(sd.intervals), 1, 0xFFFFFFFF)
    validate_integer("glyph count", len(sd.all_glyphs), 1, 0xFFFFFFFF)
    validate_integer("line advance", sd.advanceY, 0, 255)
    validate_integer("ascender", sd.ascender, -32768, 32767)
    validate_integer("descender", sd.descender, -32768, 32767)
    validate_integer("left kerning entries", len(sd.kern_left_classes), 0, 65535)
    validate_integer("right kerning entries", len(sd.kern_right_classes), 0, 65535)
    validate_integer("left kerning classes", sd.kern_left_class_count, 0, 255)
    validate_integer("right kerning classes", sd.kern_right_class_count, 0, 255)
    validate_integer("ligature count", len(sd.ligature_pairs), 0, 255)
    if len(sd.kern_matrix) != sd.kern_left_class_count * sd.kern_right_class_count:
        raise ValueError("kerning matrix dimensions do not match its class counts")
    intervals_data = bytearray()
    offset = 0
    previous_end = -1
    for i_start, i_end in sd.intervals:
        validate_integer("interval start", i_start, previous_end + 1, 0x10FFFF)
        validate_integer("interval end", i_end, i_start, 0x10FFFF)
        previous_end = i_end
        intervals_data += struct.pack("<III", i_start, i_end, offset)
        offset += i_end - i_start + 1

    if offset != len(sd.all_glyphs):
        raise ValueError("interval coverage does not match glyph count")
    glyphs_data = bytearray()
    bitmap_offset = 0
    max_dimension = 255 if raster_density == 1 else 65535
    for glyph, packed in sd.all_glyphs:
        label = f"style {sd.style_id} U+{glyph.code_point:04X}"
        for field in ("width", "height"):
            validate_integer(f"{label} {field}", getattr(glyph, field), 0, max_dimension)
        validate_integer(f"{label} advance", glyph.advance_x, 0, 65535)
        validate_integer(f"{label} left bearing", glyph.left, -32768, 32767)
        validate_integer(f"{label} top bearing", glyph.top, -32768, 32767)
        validate_integer(f"{label} bitmap length", glyph.data_length, 0, 65535)
        validate_integer(f"{label} bitmap offset", glyph.data_offset, 0, 0xFFFFFFFF)
        expected_length = (glyph.width * glyph.height + 3) // 4
        if glyph.data_length != len(packed) or glyph.data_length != expected_length:
            raise ValueError(f"{label} bitmap length does not match its dimensions")
        if glyph.data_offset != bitmap_offset:
            raise ValueError(f"{label} bitmap offset is not contiguous")
        bitmap_offset += len(packed)
        if raster_density == 1:
            glyphs_data += struct.pack(GLYPH_STRUCT_FORMAT,
                                      glyph.width, glyph.height, glyph.advance_x,
                                      glyph.left, glyph.top,
                                      glyph.data_length, glyph.data_offset)
        else:
            # Keep every v4 field at its original offset; use the old padding
            # for the high dimension bytes, interpreted only for v5 records.
            glyphs_data += struct.pack(GLYPH_V5_STRUCT_FORMAT,
                                      glyph.width & 255, glyph.height & 255, glyph.advance_x,
                                      glyph.left, glyph.top, glyph.data_length,
                                      glyph.width >> 8, glyph.height >> 8, glyph.data_offset)

    kern_left_data = bytearray()
    for cp, cls in sd.kern_left_classes:
        validate_integer("left kerning codepoint", cp, 0, 65535)
        validate_integer("left kerning class", cls, 1, sd.kern_left_class_count)
        kern_left_data += struct.pack("<HB", cp, cls)

    kern_right_data = bytearray()
    for cp, cls in sd.kern_right_classes:
        validate_integer("right kerning codepoint", cp, 0, 65535)
        validate_integer("right kerning class", cls, 1, sd.kern_right_class_count)
        kern_right_data += struct.pack("<HB", cp, cls)

    kern_matrix_data = bytearray()
    if sd.kern_matrix:
        minimum, maximum = (-128, 127) if raster_density == 1 else (-32768, 32767)
        for value in sd.kern_matrix:
            validate_integer("kerning adjustment", value, minimum, maximum)
        element_format = "b" if raster_density == 1 else "h"
        kern_matrix_data = bytearray(struct.pack(f"<{len(sd.kern_matrix)}{element_format}", *sd.kern_matrix))

    ligature_data = bytearray()
    for packed_pair, lig_cp in sd.ligature_pairs:
        validate_integer("ligature pair", packed_pair, 0, 0xFFFFFFFF)
        validate_integer("ligature codepoint", lig_cp, 0, 65535)
        ligature_data += struct.pack("<II", packed_pair, lig_cp)

    bitmap_data = bytearray()
    for glyph, packed in sd.all_glyphs:
        bitmap_data += packed
    if len(bitmap_data) != sd.total_bitmap_size:
        raise ValueError("style bitmap size does not match its glyph data")

    return (intervals_data, glyphs_data, kern_left_data, kern_right_data,
            kern_matrix_data, ligature_data, bitmap_data)


def pack_small_caps(caps, valid_codepoints):
    """Pack SCAP v1 with strict bounds and unique sorted lookup keys."""
    if caps is None:
        return b''
    count = len(caps.all_glyphs)
    validate_integer("alternate glyph count", count, 1, 65535)
    validate_integer("alternate bitmap size", caps.total_bitmap_size, 0, 0xFFFFFFFF)
    maps = bytearray()
    used_ids = set()
    for tag, entries in [('smcp', caps.smcp), ('c2sc', caps.c2sc)]:
        validate_integer(f"{tag} map count", len(entries), 0, 65535)
        previous = -1
        for cp, index in entries:
            validate_integer(f"{tag} codepoint", cp, previous + 1, 0x10FFFF)
            if cp not in valid_codepoints or is_default_ignorable(cp):
                raise ValueError(f"{tag} codepoint is absent or default-ignorable")
            validate_integer(f"{tag} alternate ID", index, 0, count - 1)
            previous = cp
            used_ids.add(index)
            maps += struct.pack('<IH', cp, index)
    if used_ids != set(range(count)):
        raise ValueError("alternate glyphs must all be referenced by a feature map")
    # Reuse the exact v5 glyph/bitmap validator without serializing fake Unicode
    # intervals. The alternate IDs use opaque keys, never EPUB codepoints.
    fake = StyleRasterData(0, [(0, count - 1)], caps.all_glyphs, caps.total_bitmap_size,
                           0, 0, 0, [], [], [], 0, 0, [])
    sections = pack_style_sections(fake, 2)
    for index, (glyph, _) in enumerate(caps.all_glyphs):
        if glyph.code_point != ALTERNATE_KEY_BASE + index:
            raise ValueError("alternate glyph keys must match their array indices")
    validate_integer("alternate kerning pair count", len(caps.kern_pairs), 0, 0xFFFFFFFF)
    pairs = bytearray()
    previous = (-1, -1)
    for left, right, value in caps.kern_pairs:
        for key in (left, right):
            validate_integer("alternate kerning key", key, 0, 0xFFFFFFFF)
            if key not in valid_codepoints and not ALTERNATE_KEY_BASE <= key < ALTERNATE_KEY_BASE + count:
                raise ValueError("alternate kerning key is absent")
        if left < ALTERNATE_KEY_BASE and right < ALTERNATE_KEY_BASE:
            raise ValueError("alternate kerning pair must involve an alternate")
        if (left, right) <= previous:
            raise ValueError("alternate kerning pairs must be unique and sorted")
        validate_integer("alternate kerning adjustment", value, -32768, 32767)
        if value == 0:
            raise ValueError("alternate kerning pairs must be nonzero")
        previous = (left, right)
        pairs += struct.pack('<IIh', left, right, value)
    total = 24 + len(maps) + len(sections[1]) + len(pairs) + len(sections[-1])
    validate_integer("feature block size", total, 0, 0xFFFFFFFF)
    header = struct.pack('<4sBBHHHHHII', b'SCAP', 1, 0, len(caps.smcp), len(caps.c2sc),
                         count, 0, 0, len(caps.kern_pairs), caps.total_bitmap_size)
    return header + maps + sections[1] + pairs + sections[-1]


def style_sections_total_size(sections):
    """Total byte size of all sections returned by pack_style_sections()."""
    return sum(len(s) for s in sections)


# --- File writers ---

def generate_cpfont_multistyle(style_fonts, size, intervals, output_path,
                               synthetic_bold=False, debug_images=False, raster_density=1,
                               small_caps=False, instance_options=None):
    """Generate a multi-style v4/v5 file, or v6 when small_caps is enabled.

    style_fonts: dict of {style_id: fontfile_path} e.g. {0: "Regular.ttf", 2: "Italic.ttf"}
    """
    MAGIC = b"CPFONT\x00\x00"
    validate_raster_density(raster_density)
    if small_caps and raster_density != 2:
        raise ValueError("--small-caps requires --raster-density 2")
    VERSION = 6 if small_caps else (4 if raster_density == 1 else 5)
    HEADER_SIZE = 32
    STYLE_TOC_ENTRY_SIZE = 32
    flags = 1  # always 2-bit greyscale
    style_count = len(style_fonts)
    validate_integer("style count", style_count, 1, 4)
    for style_id in style_fonts:
        validate_integer("style ID", style_id, 0, 3)

    # Rasterize each style
    raster_data = {}  # style_id -> StyleRasterData
    for style_id in sorted(style_fonts.keys()):
        fontfile = style_fonts[style_id]
        print(f"  Rasterizing style {style_id}...", file=sys.stderr)
        raster_data[style_id] = rasterize_font_style(
            fontfile, size, intervals, style_id=style_id, raster_density=raster_density,
            small_caps=small_caps, instance_options=instance_options)

    if synthetic_bold:
        # If a bold-style output is identical to its base style, apply a synthetic
        # bold bitmap transformation so the style still appears heavier.
        regular_sd = raster_data.get(0)
        italic_sd = raster_data.get(2)
        if regular_sd is not None and 1 in raster_data:
            style_label = STYLE_LABELS.get(1, "1")
            if style_uses_synthetic_bold(regular_sd, raster_data[1]):
                apply_synthetic_bold(raster_data[1], style_label)
        if italic_sd is not None and 3 in raster_data:
            style_label = STYLE_LABELS.get(3, "3")
            if style_uses_synthetic_bold(italic_sd, raster_data[3]):
                apply_synthetic_bold(raster_data[3], style_label)
        elif regular_sd is not None and 3 in raster_data:
            style_label = STYLE_LABELS.get(3, "3")
            if style_uses_synthetic_bold(regular_sd, raster_data[3]):
                apply_synthetic_bold(raster_data[3], style_label)

    # Pack binary sections for each style
    packed_sections = {}  # style_id -> tuple of section bytearrays
    feature_sizes = {}
    for style_id, sd in raster_data.items():
        sections = pack_style_sections(sd, raster_density=raster_density)
        features = pack_small_caps(sd.caps_data, {g.code_point for g, _ in sd.all_glyphs}) if small_caps else b''
        feature_sizes[style_id] = len(features)
        packed_sections[style_id] = sections + (features,)

    # Calculate data offsets (after header + TOC)
    data_start = HEADER_SIZE + style_count * STYLE_TOC_ENTRY_SIZE
    current_offset = data_start

    style_offsets = {}  # style_id -> absolute file offset
    for style_id in sorted(packed_sections.keys()):
        style_offsets[style_id] = current_offset
        current_offset += style_sections_total_size(packed_sections[style_id])
        validate_integer("file size", current_offset, 0, 0xFFFFFFFF)

    # Build style TOC entries
    # Each entry: styleId(1) + pad(3) + intervalCount(4) + glyphCount(4) +
    #   advanceY(1) + ascender(2) + descender(2) + kernL(2) + kernR(2) +
    #   kernLCls(1) + kernRCls(1) + ligCount(1) + dataOffset(4) + reserved(4) = 32
    STYLE_TOC_FORMAT = "<B3xIIBhhHHBBBII"
    assert struct.calcsize(STYLE_TOC_FORMAT) == STYLE_TOC_ENTRY_SIZE

    toc_data = bytearray()
    for style_id in sorted(raster_data.keys()):
        sd = raster_data[style_id]
        toc_data += struct.pack(STYLE_TOC_FORMAT,
                                style_id,
                                len(sd.intervals), len(sd.all_glyphs),
                                sd.advanceY, sd.ascender, sd.descender,
                                len(sd.kern_left_classes), len(sd.kern_right_classes),
                                sd.kern_left_class_count, sd.kern_right_class_count,
                                len(sd.ligature_pairs),
                                style_offsets[style_id],
                                (style_offsets[style_id] + style_sections_total_size(packed_sections[style_id])
                                 - feature_sizes[style_id]) if feature_sizes[style_id] else 0)

    # The v5 payload CRC covers the exact TOC and section bytes in file order.
    # It also changes the cache identity when outlines or conversion settings change.
    if VERSION == 4:
        header = struct.pack("<8sHHB19s", MAGIC, VERSION, flags, style_count, bytes(19))
    else:
        payload_crc = zlib.crc32(toc_data)
        for style_id in sorted(packed_sections):
            for section in packed_sections[style_id]:
                payload_crc = zlib.crc32(section, payload_crc)
        header = struct.pack("<8sHHBBI14s", MAGIC, VERSION, flags, style_count,
                             raster_density, payload_crc & 0xFFFFFFFF, bytes(14))
    assert len(header) == HEADER_SIZE

    # Write output
    os.makedirs(os.path.dirname(output_path) if os.path.dirname(output_path) else ".", exist_ok=True)
    total_file_size = 0
    with open(output_path, "wb") as f:
        f.write(header)
        f.write(toc_data)
        for style_id in sorted(packed_sections.keys()):
            for section in packed_sections[style_id]:
                f.write(section)
        total_file_size = f.tell()

    # Print summary
    print(f"  Output: {output_path} (v{VERSION}, {style_count} styles, raster density {raster_density})", file=sys.stderr)
    print(f"    Header+TOC: {HEADER_SIZE + len(toc_data)} bytes", file=sys.stderr)
    for style_id in sorted(raster_data.keys()):
        sd = raster_data[style_id]
        secs = packed_sections[style_id]
        sname = STYLE_LABELS.get(style_id, str(style_id))
        ssize = style_sections_total_size(secs)
        print(f"    {sname}: {len(sd.all_glyphs)} glyphs, {len(sd.intervals)} intervals, "
              f"{ssize} bytes", file=sys.stderr)
    print(f"    Total: {total_file_size} bytes ({total_file_size / 1024 / 1024:.2f} MB)", file=sys.stderr)
    if debug_images:
        save_debug_glyph_image(output_path, raster_data)
    return total_file_size


def main():
    parser = argparse.ArgumentParser(
        description="Generate .cpfont files for SD card font loading.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=f"Available interval presets: {', '.join(sorted(INTERVAL_PRESETS.keys()))}"
    )

    # Font file (positional, optional for multi-style mode)
    parser.add_argument("fontfile", nargs="?", default=None,
                        help="Path to the font file (single-style mode).")
    parser.add_argument("--intervals", dest="intervals",
                        help="Comma-separated interval presets (e.g., 'latin-ext,greek,cyrillic').")
    parser.add_argument("--size", type=int, dest="size",
                        help="Single font size to generate.")
    parser.add_argument("--sizes", dest="sizes",
                        help="Comma-separated sizes (e.g., '12,14,16,18').")
    parser.add_argument("--raster-density", type=int, choices=(1, 2), default=1,
                        help="Raster pixels per logical pixel: 1 preserves v4; 2 writes v5 "
                             "with LIGHT-hinted outlines for higher quality resampling.")
    parser.add_argument("--small-caps", action="store_true",
                        help="Write v6 with genuine OpenType smcp/c2sc alternates (requires density 2).")
    parser.add_argument("--optical-size", default="default", metavar="default|auto|NUMBER",
                        help="Variable opsz: default unchanged, auto uses logical size, or explicit coordinate.")
    parser.add_argument("--instance-styles", action="store_true",
                        help="Pin variable wght to 400/700 and ital to 0/1 for each requested style.")
    parser.add_argument("--axes", type=parse_axis_settings, default={}, metavar="TAG=VALUE,...",
                        help="Explicit variation axes; overrides automatic optical/style selection.")
    for label in STYLE_LABELS.values():
        parser.add_argument(f"--{label}-axes", type=parse_axis_settings, default={}, metavar="TAG=VALUE,...",
                            help=f"Variation overrides for the {label} style; overrides --axes.")
    parser.add_argument("--style", dest="style", default="regular",
                        choices=["regular", "bold", "italic", "bolditalic"],
                        help="Font style for single-style mode (default: regular).")
    parser.add_argument("--name", dest="name",
                        help="Font family name for output filenames (default: derived from font filename).")
    parser.add_argument("--synthetic-bold", dest="synthetic_bold", action="store_true",
                        help="Apply synthetic boldening when bold style equals its source style.")
    parser.add_argument("--debug-images", dest="debug_images", action="store_true",
                        help="Save debug A glyph PNGs for each generated style.")
    parser.add_argument("-o", "--output", dest="output",
                        help="Output file path (for single-size mode).")
    parser.add_argument("--output-dir", dest="output_dir",
                        help="Output directory for multi-size mode.")
    parser.add_argument("--list-presets", action="store_true",
                        help="List available interval presets and exit.")

    # Multi-style mode: per-style font file arguments
    parser.add_argument("--regular", dest="font_regular",
                        help="Font file for regular style (enables multi-style mode).")
    parser.add_argument("--bold", dest="font_bold",
                        help="Font file for bold style.")
    parser.add_argument("--italic", dest="font_italic",
                        help="Font file for italic style.")
    parser.add_argument("--bolditalic", dest="font_bolditalic",
                        help="Font file for bold-italic style.")

    args = parser.parse_args()
    instance_options = FontInstanceOptions(args.optical_size, args.instance_styles, args.axes,
                                           {sid: getattr(args, label + "_axes") for sid, label in STYLE_LABELS.items()})
    if not args.axes and not any(instance_options.style_axes.values()) and not args.instance_styles and args.optical_size == "default":
        instance_options = None

    if args.list_presets:
        print("Available interval presets:")
        for name, ranges in sorted(INTERVAL_PRESETS.items()):
            total = sum(e - s + 1 for s, e in ranges)
            print(f"  {name:15s}  {len(ranges)} range(s), ~{total} codepoints")
        sys.exit(0)

    # Detect multi-style mode
    style_fonts = {}
    if args.font_regular:
        style_fonts[0] = args.font_regular
    if args.font_bold:
        style_fonts[1] = args.font_bold
    if args.font_italic:
        style_fonts[2] = args.font_italic
    if args.font_bolditalic:
        style_fonts[3] = args.font_bolditalic

    is_multistyle = len(style_fonts) > 0
    fontfile = args.fontfile

    # Require --intervals
    if not args.intervals:
        print("Error: --intervals is required (e.g., --intervals latin-ext,greek,cyrillic)", file=sys.stderr)
        print(f"Available presets: {', '.join(sorted(INTERVAL_PRESETS.keys()))}", file=sys.stderr)
        sys.exit(1)

    intervals = resolve_intervals(args.intervals)

    # Determine sizes
    if args.sizes:
        sizes = [int(s.strip()) for s in args.sizes.split(",")]
    elif args.size:
        sizes = [args.size]
    else:
        print("Error: --size or --sizes is required", file=sys.stderr)
        sys.exit(1)

    # Validate early: single-style mode requires a font file
    if not is_multistyle and not fontfile:
        print("Error: fontfile is required in single-style mode", file=sys.stderr)
        sys.exit(1)

    # Determine font name
    if args.name:
        font_name = args.name
    elif is_multistyle:
        # Derive from the regular font file
        ref_file = style_fonts[min(style_fonts.keys())]
        base = os.path.splitext(os.path.basename(ref_file))[0]
        for suffix in ["-Regular", "-Bold", "-Italic", "-BoldItalic",
                       "-regular", "-bold", "-italic", "-bolditalic"]:
            if base.endswith(suffix):
                base = base[:-len(suffix)]
                break
        font_name = base
    else:
        base = os.path.splitext(os.path.basename(fontfile))[0]
        for suffix in ["-Regular", "-Bold", "-Italic", "-BoldItalic",
                       "-regular", "-bold", "-italic", "-bolditalic"]:
            if base.endswith(suffix):
                base = base[:-len(suffix)]
                break
        font_name = base

    if not is_multistyle:
        # Single font file provided: wrap as a single-style font
        style_map = {"regular": 0, "bold": 1, "italic": 2, "bolditalic": 3}
        style_fonts[style_map[args.style]] = fontfile

    # Filenames keep the logical size, regardless of raster density.
    if args.output and len(sizes) != 1:
        print("Error: --output can only be used with a single size", file=sys.stderr)
        sys.exit(1)
    output_dir = args.output_dir if args.output_dir else f"{font_name}/"
    total_size = 0
    for sz in sizes:
        if args.output and len(sizes) == 1:
            output_path = args.output
        else:
            filename = f"{font_name}_{sz}.cpfont"
            output_path = os.path.join(output_dir, filename)
        print(f"Generating {output_path} (logical size {sz}, {len(style_fonts)} style(s), "
              f"raster density {args.raster_density})...", file=sys.stderr)
        total_size += generate_cpfont_multistyle(
            style_fonts, sz, intervals, output_path,
            synthetic_bold=args.synthetic_bold,
            debug_images=args.debug_images, raster_density=args.raster_density,
            small_caps=args.small_caps, instance_options=instance_options)
    print(f"\nTotal: {len(sizes)} files, {total_size / 1024 / 1024:.2f} MB", file=sys.stderr)


if __name__ == "__main__":
    try:
        main()
    except ValueError as error:
        print(f"Error: {error}", file=sys.stderr)
        sys.exit(1)
