#!/usr/bin/env python3
"""Standalone converter tests; requires freetype-py and fonttools.

Run: python test/fontconvert_sdcard/test_fontconvert_sdcard.py
All font outlines and pixels are generated in a temporary directory. No external
font files, licensed assets, device, or repository writes are required.
"""

import contextlib
import importlib.util
import io
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import freetype
from fontTools.fontBuilder import FontBuilder
from fontTools.feaLib.builder import addOpenTypeFeaturesFromString
from fontTools.pens.ttGlyphPen import TTGlyphPen

REPO = Path(__file__).resolve().parents[2]
CONVERTER = REPO / 'lib/EpdFont/scripts/fontconvert_sdcard.py'
spec = importlib.util.spec_from_file_location('fontconvert_sdcard', CONVERTER)
convert = importlib.util.module_from_spec(spec)
spec.loader.exec_module(convert)


def make_source(path):
    """Original rectangular/diagonal test outlines, including a >255 px glyph."""
    fb = FontBuilder(1000, isTTF=True)
    names = ['.notdef', 'space', 'H', 'N', 'T', 'wide']
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap({32: 'space', 72: 'H', 78: 'N', 84: 'T', 0x2E3B: 'wide'})
    polygons = {
        '.notdef': [[(0, 0), (500, 0), (500, 700), (0, 700)]],
        'space': [],
        'H': [[(0, 0), (100, 0), (100, 310), (500, 310), (500, 0),
               (600, 0), (600, 700), (500, 700), (500, 400), (100, 400),
               (100, 700), (0, 700)]],
        'N': [[(0, 0), (90, 0), (90, 520), (500, 0), (600, 0),
               (600, 700), (510, 700), (510, 180), (100, 700), (0, 700)]],
        'T': [[(-30, 620), (250, 620), (250, 0), (350, 0), (350, 620),
               (630, 620), (630, 730), (-30, 730)]],
        'wide': [[(0, 280), (2800, 280), (2800, 350), (0, 350)]],
    }
    glyphs = {}
    for name, paths in polygons.items():
        pen = TTGlyphPen(None)
        for points in paths:
            pen.moveTo(points[0])
            for point in points[1:]:
                pen.lineTo(point)
            pen.closePath()
        glyphs[name] = pen.glyph()
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics({n: (2900 if n == 'wide' else 650, -30 if n == 'T' else 0) for n in names})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({'familyName': 'Raster Test', 'styleName': 'Regular',
                       'uniqueFontIdentifier': 'Raster Test', 'fullName': 'Raster Test',
                       'psName': 'RasterTest-Regular'})
    fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
    fb.setupPost()
    fb.setupMaxp()
    addOpenTypeFeaturesFromString(fb.font, 'feature kern { pos H T -180; pos N H 220; } kern;')
    fb.save(path)


def decode_file(path):
    """Independent reader: offsets are specified explicitly, not taken from writer constants."""
    data = path.read_bytes()
    assert data[:8] == b'CPFONT\0\0'
    version, flags = struct.unpack_from('<HH', data, 8)
    assert version in (4, 5) and flags == 1
    density = data[13] if version == 5 else 1
    if version == 5:
        assert density == 2 and data[18:32] == bytes(14)
        assert struct.unpack_from('<I', data, 14)[0] == zlib.crc32(data[32:])
    else:
        assert data[13:32] == bytes(19)
    styles = {}
    for i in range(data[12]):
        toc = 32 + i * 32
        sid = data[toc]
        interval_count, glyph_count = struct.unpack_from('<II', data, toc + 4)
        left_count, right_count = struct.unpack_from('<HH', data, toc + 17)
        left_classes, right_classes, lig_count = data[toc + 21:toc + 24]
        start = struct.unpack_from('<I', data, toc + 24)[0]
        cps = []
        for j in range(interval_count):
            low, high, offset = struct.unpack_from('<III', data, start + j * 12)
            assert offset == len(cps)
            cps.extend(range(low, high + 1))
        assert len(cps) == glyph_count
        glyph_start = start + interval_count * 12
        left_start = glyph_start + glyph_count * 16
        right_start = left_start + left_count * 3
        matrix_start = right_start + right_count * 3
        matrix_count = left_classes * right_classes
        matrix_size = matrix_count * (2 if version == 5 else 1)
        bitmap_start = matrix_start + matrix_size + lig_count * 8
        glyphs = {}
        for j, cp in enumerate(cps):
            g = glyph_start + j * 16
            width, height = data[g], data[g + 1]
            if version == 5:
                width += data[g + 10] * 256
                height += data[g + 11] * 256
            advance, left, top, length = struct.unpack_from('<HhhH', data, g + 2)
            offset = struct.unpack_from('<I', data, g + 12)[0]
            pixels = data[bitmap_start + offset:bitmap_start + offset + length]
            assert len(pixels) == length == (width * height + 3) // 4
            glyphs[cp] = dict(width=width, height=height, advance=advance,
                              left=left, top=top, pixels=pixels)
        left_map = dict(struct.unpack_from('<HB', data, left_start + j * 3) for j in range(left_count))
        right_map = dict(struct.unpack_from('<HB', data, right_start + j * 3) for j in range(right_count))
        matrix = struct.unpack_from('<' + ('h' if version == 5 else 'b') * matrix_count, data, matrix_start)
        pairs = {(lc, rc): matrix[(li - 1) * right_classes + ri - 1]
                 for lc, li in left_map.items() for rc, ri in right_map.items()}
        styles[sid] = dict(glyphs=glyphs, pairs=pairs)
    return data, version, density, styles


def source_oracle(source, cp, logical_size, density):
    """Direct FreeType oracle and independent level/bit packing, not converter helpers."""
    face = freetype.Face(str(source))
    face.set_char_size(logical_size * density * 64, 0, 150, 150)
    flags = freetype.FT_LOAD_RENDER
    if density == 2:
        flags |= freetype.FT_LOAD_TARGET_LIGHT | freetype.FT_LOAD_NO_BITMAP
    else:
        flags |= freetype.FT_LOAD_FORCE_AUTOHINT
    face.load_char(cp, flags)
    bm = face.glyph.bitmap
    levels = []
    raw = bm.buffer
    for y in range(bm.rows):
        row = y if bm.pitch >= 0 else bm.rows - 1 - y
        for x in range(bm.width):
            value = raw[row * abs(bm.pitch) + x]
            if density == 1:
                levels.append(sum(value >= t for t in (64, 128, 192)))
            else:
                levels.append(min(range(4), key=lambda n: abs(value - n * 85)))
    packed = bytearray()
    for i in range(0, len(levels), 4):
        chunk = levels[i:i + 4] + [0] * max(0, 4 - len(levels[i:i + 4]))
        packed.append(chunk[0] * 64 + chunk[1] * 16 + chunk[2] * 4 + chunk[3])
    return dict(width=bm.width, height=bm.rows,
                advance=(face.glyph.linearHoriAdvance + 2048) // 4096,
                left=face.glyph.bitmap_left, top=face.glyph.bitmap_top, pixels=bytes(packed))


class ConverterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)
        cls.source = cls.dir / 'RasterTest.ttf'
        make_source(cls.source)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def render(self, density=1, size=14, intervals=None, styles=(0,)):
        out = self.dir / f'font-{density}-{size}.cpfont'
        intervals = intervals or [(32, 32), (72, 72), (78, 78), (84, 84), (0x2E3B, 0x2E3B)]
        with contextlib.redirect_stderr(io.StringIO()):
            convert.generate_cpfont_multistyle({s: str(self.source) for s in styles}, size,
                                               intervals, str(out), raster_density=density)
        return decode_file(out)

    def fixture(self, width=3, height=2, kern=-128):
        pixels = bytes((width * height + 3) // 4)
        glyph = convert.GlyphProps(width, height, 160, -2, 8, len(pixels), 0, 72)
        return convert.StyleRasterData(0, [(72, 72)], [(glyph, pixels)], len(pixels),
                                       20, 16, -4, [(72, 1)], [(84, 1)], [kern], 1, 1, [])

    def test_v4_quantization_exhaustively_preserves_thresholds(self):
        for value in range(256):
            self.assertEqual(convert.quantize_coverage(value), sum(value >= t for t in (64, 128, 192)))

    def test_v5_quantization_chooses_nearest_coverage(self):
        for value in range(256):
            self.assertEqual(convert.quantize_coverage(value, 2),
                             min(range(4), key=lambda n: abs(value - n * 85)))

    def test_signed_pitch_and_continuous_row_packing(self):
        normal = SimpleNamespace(width=3, rows=2, pitch=4, buffer=[0, 85, 170, 99, 255, 85, 0, 99],
                                 pixel_mode=freetype.FT_PIXEL_MODE_GRAY, num_grays=256)
        reversed_rows = SimpleNamespace(**{**vars(normal), 'pitch': -4,
                                           'buffer': [255, 85, 0, 99, 0, 85, 170, 99]})
        for bitmap in (normal, reversed_rows):
            self.assertEqual(convert.pack_freetype_bitmap(bitmap, 2), b'\x1b\x40')

    def test_v4_all_bytes_match_explicit_fixture(self):
        sd = self.fixture()
        out = self.dir / 'fixture-v4.cpfont'
        with patch.object(convert, 'rasterize_font_style', return_value=sd), contextlib.redirect_stderr(io.StringIO()):
            convert.generate_cpfont_multistyle({0: 'unused'}, 14, [(72, 72)], str(out))
        expected = (b'CPFONT\0\0' + b'\x04\x00\x01\x00\x01' + bytes(19) +
                    struct.pack('<B3xIIBhhHHBBBI4x', 0, 1, 1, 20, 16, -4, 1, 1, 1, 1, 0, 64) +
                    struct.pack('<III', 72, 72, 0) +
                    b'\x03\x02\xa0\x00\xfe\xff\x08\x00\x02\x00\x00\x00\x00\x00\x00\x00' +
                    b'\x48\x00\x01\x54\x00\x01\x80\x00\x00')
        self.assertEqual(out.read_bytes(), expected)

    def test_v4_default_matches_direct_autohint_oracle(self):
        data, version, density, styles = self.render()
        self.assertEqual((version, density), (4, 1))
        for cp, glyph in styles[0]['glyphs'].items():
            self.assertEqual(glyph, source_oracle(self.source, cp, 14, 1), hex(cp))

    def test_v5_matches_direct_light_outline_oracle_all_styles(self):
        _, version, density, styles = self.render(2, styles=(0, 1, 2, 3))
        self.assertEqual((version, density), (5, 2))
        self.assertEqual(set(styles), {0, 1, 2, 3})
        for style in styles.values():
            for cp, glyph in style['glyphs'].items():
                self.assertEqual(glyph, source_oracle(self.source, cp, 14, 2), hex(cp))

    def test_v5_preserves_wide_glyph(self):
        _, _, _, styles = self.render(2, size=24)
        glyph = styles[0]['glyphs'][0x2E3B]
        self.assertGreater(glyph['width'], 255)
        self.assertEqual(glyph, source_oracle(self.source, 0x2E3B, 24, 2))

    def test_v5_preserves_negative_and_positive_wide_kerning(self):
        _, _, _, styles = self.render(2)
        pairs = styles[0]['pairs']
        self.assertEqual(pairs[(72, 84)], -168)
        self.assertEqual(pairs[(78, 72)], 205)
        self.assertLess(pairs[(72, 84)], -128)
        self.assertGreater(pairs[(78, 72)], 127)

    def test_v5_header_crc_covers_toc_and_bitmap(self):
        data, _, _, _ = self.render(2)
        crc = struct.unpack_from('<I', data, 14)[0]
        for offset in (32, len(data) - 1):
            changed = bytearray(data)
            changed[offset] ^= 1
            self.assertNotEqual(crc, zlib.crc32(changed[32:]))

    def test_v5_high_dimension_bytes_preserve_v4_field_offsets(self):
        sd = self.fixture(width=267, height=258, kern=-300)
        packed = convert.pack_style_sections(sd, 2)[1]
        self.assertEqual(packed[0:2], bytes((11, 2)))
        self.assertEqual(packed[10:12], b'\x01\x01')
        self.assertEqual(struct.unpack_from('<HhhH', packed, 2), (160, -2, 8, (267 * 258 + 3) // 4))
        self.assertEqual(struct.unpack_from('<I', packed, 12)[0], 0)

    def test_metrics_outside_binary_fields_are_rejected(self):
        cases = {'width': 65536, 'height': 65536, 'advance_x': 65536, 'left': -32769,
                 'top': 32768, 'data_length': 65536, 'data_offset': 0x100000000}
        for name, value in cases.items():
            with self.subTest(field=name):
                sd = self.fixture()
                glyph, pixels = sd.all_glyphs[0]
                sd = sd._replace(all_glyphs=[(glyph._replace(**{name: value}), pixels)])
                with self.assertRaises(ValueError):
                    convert.pack_style_sections(sd, 2)
        for kern in (-32769, 32768):
            with self.assertRaisesRegex(ValueError, 'kerning adjustment'):
                convert.pack_style_sections(self.fixture(kern=kern), 2)

    def test_v4_rejects_wide_dimensions_and_v5_excessive_payload(self):
        with self.assertRaisesRegex(ValueError, 'width'):
            convert.pack_style_sections(self.fixture(width=267))
        with self.assertRaisesRegex(ValueError, 'bitmap length'):
            convert.pack_style_sections(self.fixture(width=1024, height=1024), 2)

    def test_invalid_structural_metadata_is_rejected(self):
        sd = self.fixture()
        cases = [sd._replace(advanceY=256), sd._replace(ascender=32768),
                 sd._replace(kern_left_class_count=256), sd._replace(kern_matrix=[]),
                 sd._replace(intervals=[(72, 73)]), sd._replace(total_bitmap_size=500),
                 sd._replace(ligature_pairs=[(1, 2)] * 256)]
        for bad in cases:
            with self.assertRaises(ValueError):
                convert.pack_style_sections(bad, 2)

    def test_v5_class_limit_errors_instead_of_dropping_kerning(self):
        pairs = {(i, 1000): i + 1 for i in range(256)}
        with self.assertRaisesRegex(ValueError, 'class count'):
            convert.derive_kern_classes(pairs, strict=True)

    def test_kerning_conversion_preserves_v4_clamping_only(self):
        self.assertEqual(convert.fp4_from_design_units(-20, 1), -128)
        self.assertEqual(convert.fp4_from_design_units(-20, 1, wide=True), -320)
        with self.assertRaises(ValueError):
            convert.fp4_from_design_units(3000, 1, wide=True)

    def test_invalid_density_and_size_are_rejected_before_freetype(self):
        for density in (0, 3, 4):
            with self.assertRaises(ValueError):
                convert.rasterize_font_style('unused', 14, [], raster_density=density)
        for size in (-1, 0):
            with self.assertRaises(ValueError):
                convert.rasterize_font_style('unused', size, [])

    def test_default_ignorables_stay_empty_at_density_two(self):
        _, _, _, styles = self.render(2, intervals=[(0x200B, 0x200F)])
        for glyph in styles[0]['glyphs'].values():
            self.assertEqual(glyph, dict(width=0, height=0, advance=0, left=0, top=0, pixels=b''))

    def test_cli_names_files_using_logical_sizes(self):
        out = self.dir / 'cli'
        result = subprocess.run([sys.executable, str(CONVERTER), '--intervals', 'ascii',
                                 '--sizes', '10,14', '--raster-density', '2', '--name', 'OriginalTest',
                                 '--output-dir', str(out), str(self.source)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(sorted(p.name for p in out.iterdir()), ['OriginalTest_10.cpfont', 'OriginalTest_14.cpfont'])
        for path in out.iterdir():
            self.assertEqual(decode_file(path)[1:3], (5, 2))


if __name__ == '__main__':
    unittest.main(verbosity=2)
