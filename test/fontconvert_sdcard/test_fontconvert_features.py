#!/usr/bin/env python3
"""Original generated variable/GSUB fixtures; no external font assets required."""
import contextlib
import copy
import hashlib
import io
import struct
import tempfile
import unittest
import zlib
from pathlib import Path
from types import SimpleNamespace

from fontTools.designspaceLib import DesignSpaceDocument, AxisDescriptor, SourceDescriptor
from fontTools.feaLib.builder import addOpenTypeFeaturesFromString
from fontTools.ttLib import TTFont
from fontTools.varLib import build
from fontTools.varLib.instancer import instantiateVariableFont

from test_fontconvert_sdcard import convert, make_source, decode_file, source_oracle


def make_caps_source(path):
    make_source(path)
    with TTFont(path) as font:
        cmap = {104: 'H', 110: 'N', 116: 'T', 0x127: 'H'}
        for table in font['cmap'].tables:
            if table.isUnicode():
                table.cmap.update(cmap)
        names = ['h.mid', 'h.sc', 'n.sc', 't.sc', 'H.sc']
        order = list(font.getGlyphOrder())
        for index, name in enumerate(names):
            source = 'H' if name.startswith(('h', 'H')) else name[0].upper()
            glyph = copy.deepcopy(font['glyf'][source])
            glyph.coordinates.scale((0.65 + 0.04 * index, 0.55 + 0.02 * index))
            font['glyf'][name] = glyph
            font['hmtx'].metrics[name] = (430 + 25 * index, 0)
        font.setGlyphOrder(order + names)
        del font['GPOS']
        addOpenTypeFeaturesFromString(font, """
            lookup First { sub H by h.mid; } First;
            lookup Second useExtension { sub h.mid by h.sc; } Second;
            feature smcp { lookup First; lookup Second; sub N by n.sc; sub T by t.sc; } smcp;
            feature c2sc { sub H by H.sc; } c2sc;
            feature kern { pos H T -180; pos h.sc t.sc -150;
                pos H t.sc -80; pos h.sc T 100; pos h.sc H.sc -40; } kern;
        """)
        font.save(path)


def make_variable_source(directory):
    """Compatible original masters include HVAR and variable GPOS kerning."""
    document = DesignSpaceDocument()
    for tag, low, default, high in [('opsz', 8, 12, 72), ('wght', 100, 400, 900), ('ital', 0, 0, 1)]:
        axis = AxisDescriptor()
        axis.name = axis.tag = tag
        axis.minimum, axis.default, axis.maximum = low, default, high
        document.addAxis(axis)
    for index, location in enumerate([{}, {'opsz': 8}, {'opsz': 72}, {'wght': 100}, {'wght': 900}, {'ital': 1}]):
        path = directory / f'master{index}.ttf'
        make_source(path)
        with TTFont(path) as font:
            coords = font['glyf']['H'].coordinates
            for i, (x, y) in enumerate(coords):
                coords[i] = (x + index * 20 if x > 300 else x, y)
            font['hmtx'].metrics['H'] = (650 + index * 20, 0)
            del font['GPOS']
            addOpenTypeFeaturesFromString(font, f'feature kern {{ pos H T {-180 + index * 15}; }} kern;')
            font.save(path)
        source = SourceDescriptor()
        source.path, source.name = str(path), f'master{index}'
        source.familyName, source.styleName = 'Variable Test', f'Master{index}'
        source.location = {'opsz': 12, 'wght': 400, 'ital': 0, **location}
        if index == 0:
            source.copyInfo = source.copyLib = source.copyFeatures = True
        document.addSource(source)
    font, _, _ = build(document)
    path = directory / 'VariableTest.ttf'
    font.save(path)
    font.close()
    return path


def read_caps(path):
    """Independent offset parser of the v6 extension (no production constants)."""
    data, version, density, normal = decode_file(path)
    assert (version, density) == (6, 2)
    result = {}
    for i in range(data[12]):
        toc = 32 + i * 32
        offset = struct.unpack_from('<I', data, toc + 28)[0]
        if not offset:
            result[data[toc]] = None
            continue
        assert data[offset:offset + 6] == b'SCAP\x01\0'
        smcp_count, c2sc_count, glyph_count, reserved0, reserved1 = struct.unpack_from('<HHHHH', data, offset + 6)
        assert reserved0 == reserved1 == 0
        pair_count, bitmap_size = struct.unpack_from('<II', data, offset + 16)
        cursor = offset + 24
        maps = []
        for count in (smcp_count, c2sc_count):
            entries = [struct.unpack_from('<IH', data, cursor + j * 6) for j in range(count)]
            assert entries == sorted(entries) and len(dict(entries)) == count
            maps.append(dict(entries))
            cursor += count * 6
        glyph_start = cursor
        cursor += glyph_count * 16
        entries = [struct.unpack_from('<IIh', data, cursor + j * 10) for j in range(pair_count)]
        assert entries == sorted(entries)
        pairs = {(left, right): value for left, right, value in entries}
        assert len(pairs) == pair_count
        cursor += pair_count * 10
        glyphs = {}
        for j in range(glyph_count):
            g = glyph_start + j * 16
            width, height = data[g] + 256 * data[g + 10], data[g + 1] + 256 * data[g + 11]
            advance, left, top, length = struct.unpack_from('<HhhH', data, g + 2)
            start = struct.unpack_from('<I', data, g + 12)[0]
            assert start + length <= bitmap_size
            pixels = data[cursor + start:cursor + start + length]
            assert len(pixels) == length == (width * height + 3) // 4
            glyphs[j] = dict(width=width, height=height, advance=advance, left=left, top=top, pixels=pixels)
        end = cursor + bitmap_size
        if i + 1 < data[12]:
            assert end == struct.unpack_from('<I', data, toc + 32 + 24)[0]
        else:
            assert end == len(data)
        result[data[toc]] = dict(smcp=maps[0], c2sc=maps[1], glyphs=glyphs, pairs=pairs, offset=offset)
    return data, normal, result


class FeatureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.temp.name)
        cls.static = cls.directory / 'StaticTest.ttf'
        cls.caps = cls.directory / 'CapsTest.ttf'
        make_source(cls.static)
        make_caps_source(cls.caps)
        cls.variable = make_variable_source(cls.directory)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def generate(self, source=None, size=14, density=2, styles=(0,), caps=False, options=None):
        output = self.directory / 'out.cpfont'
        with contextlib.redirect_stderr(io.StringIO()):
            convert.generate_cpfont_multistyle({i: str(source or self.caps) for i in styles}, size,
                [(32, 0x127)], str(output), raster_density=density, small_caps=caps, instance_options=options)
        return output

    def test_v6_uncoded_alternates_and_all_small_caps_are_distinct(self):
        path = self.generate(caps=True, styles=(0, 1, 2, 3))
        data, normal, styles = read_caps(path)
        with TTFont(self.caps) as font:
            cmap = font.getBestCmap()
            self.assertNotIn('h.sc', cmap.values())
            # Give only our independent FT oracle access to alternate outlines
            # through temporary cmap entries, without relying on exporter IDs.
            for table in font['cmap'].tables:
                if table.isUnicode():
                    table.cmap.update({0xE000: 'h.sc', 0xE001: 'H.sc', 0xE002: 't.sc'})
            oracle = self.directory / 'CapsOracle.ttf'
            font.save(oracle)
        for sid, caps in styles.items():
            self.assertIsNotNone(caps)
            small, all_small = caps['smcp'][104], caps['c2sc'][72]
            self.assertNotEqual(small, all_small)
            self.assertEqual(caps['smcp'][0x127], small)
            self.assertEqual(caps['glyphs'][small], source_oracle(oracle, 0xE000, 14, 2))
            self.assertEqual(caps['glyphs'][all_small], source_oracle(oracle, 0xE001, 14, 2))
            self.assertNotEqual(caps['glyphs'][small]['advance'], normal[sid]['glyphs'][72]['advance'])
            self.assertEqual(caps['glyphs'][caps['smcp'][116]], source_oracle(oracle, 0xE002, 14, 2))
        self.assertEqual(struct.unpack_from('<I', data, 14)[0], zlib.crc32(data[32:]))
        changed = bytearray(data)
        changed[styles[0]['offset'] + 24] ^= 1
        self.assertNotEqual(struct.unpack_from('<I', data, 14)[0], zlib.crc32(changed[32:]))

    def test_v6_mixed_genuine_and_normal_kerning_uses_resolved_keys(self):
        _, normal, styles = read_caps(self.generate(caps=True))
        caps = styles[0]
        h, t = 0x110000 + caps['smcp'][104], 0x110000 + caps['smcp'][116]
        self.assertEqual(caps['pairs'][(h, t)], -140)
        self.assertEqual(caps['pairs'][(72, t)], -75)
        self.assertEqual(caps['pairs'][(h, 84)], 93)
        self.assertEqual(caps['pairs'][(0x127, t)], -75)  # Unicode alias of H.
        self.assertEqual(normal[0]['pairs'][(72, 84)], -168)
        self.assertTrue(all(left >= 0x110000 or right >= 0x110000 for left, right in caps['pairs']))

    def test_missing_features_leave_zero_offset_and_normal_glyphs(self):
        path = self.generate(source=self.static, caps=True)
        _, _, features = read_caps(path)
        self.assertIsNone(features[0])
        with self.assertRaisesRegex(ValueError, 'density 2'):
            self.generate(caps=True, density=1)

    def test_default_options_do_not_change_v4_or_v5(self):
        for density in (1, 2):
            first = self.generate(source=self.variable, density=density).read_bytes()
            second = self.generate(source=self.variable, density=density, options=convert.FontInstanceOptions()).read_bytes()
            self.assertEqual(first, second)

    def test_logical_opsz_and_style_axes_control_outlines_advances_and_gpos(self):
        digest = hashlib.sha256(self.variable.read_bytes()).digest()
        options = convert.FontInstanceOptions('auto', True)
        for size in (10, 18):
            for density in (1, 2):
                _, _, _, styles = decode_file(self.generate(source=self.variable, size=size, density=density,
                                                            styles=(0, 1, 2, 3), options=options))
                for sid, output in styles.items():
                    coords = {'opsz': size, 'wght': 700 if sid & 1 else 400, 'ital': 1 if sid & 2 else 0}
                    with TTFont(self.variable) as font:
                        instance = instantiateVariableFont(font, coords, inplace=False)
                        oracle = self.directory / 'InstanceOracle.ttf'
                        instance.save(oracle)
                        self.assertNotIn('fvar', instance)
                        # FontTools applies variable GPOS before quantization.
                        value = instance['GPOS'].table.LookupList.Lookup[0].SubTable[0].PairSet[0].PairValueRecord[0].Value1.XAdvance
                    self.assertEqual(output['glyphs'][72], source_oracle(oracle, 72, size, density))
                    expected = round(value * size * density * 150 / 72 / 1000 * 16)
                    if density == 1:
                        expected = max(-128, min(127, expected))
                    self.assertEqual(output['pairs'][(72, 84)], expected)
        self.assertEqual(hashlib.sha256(self.variable.read_bytes()).digest(), digest)

    def test_axis_override_precedence_clamping_and_invalid_requests(self):
        with TTFont(self.variable) as font:
            auto = convert.FontInstanceOptions('auto', True)
            self.assertEqual(convert.instance_coordinates(font, 2, 0, auto)['opsz'], 8)
            self.assertEqual(convert.instance_coordinates(font, 200, 0, auto)['opsz'], 72)
            options = convert.FontInstanceOptions('auto', True, {'opsz': 20, 'wght': 450}, {1: {'wght': 550}})
            self.assertEqual(convert.instance_coordinates(font, 14, 1, options), {'opsz': 20, 'wght': 550, 'ital': 0})
            for bad in ({'bad!': 1}, {'wght': 1000}, {'opsz': float('nan')}):
                with self.assertRaises(ValueError):
                    convert.instance_coordinates(font, 14, 0, convert.FontInstanceOptions(axes=bad))
        for text in ('wght=400,wght=500', 'weight=400', 'opsz=nan', 'wght400'):
            with self.assertRaises(ValueError):
                convert.parse_axis_settings(text)
        self.assertEqual(convert.parse_axis_settings('wght=650,opsz=14'), {'wght': 650, 'opsz': 14})
        with self.assertRaisesRegex(ValueError, 'no opsz'):
            self.generate(source=self.static, options=convert.FontInstanceOptions(14))

    def test_contextual_and_conflicting_substitutions_fail_explicitly(self):
        # Deliberate minimal table objects isolate export decisions from feaLib.
        one = SimpleNamespace(LookupType=1, LookupFlag=0,
                              SubTable=[SimpleNamespace(mapping={'H': 'h.sc'})])
        two = SimpleNamespace(LookupType=1, LookupFlag=0,
                              SubTable=[SimpleNamespace(mapping={'H': 'H.sc'})])
        records = [SimpleNamespace(FeatureTag='smcp', Feature=SimpleNamespace(LookupListIndex=[i])) for i in (0, 1)]
        font = {'GSUB': SimpleNamespace(table=SimpleNamespace(
            FeatureList=SimpleNamespace(FeatureRecord=records), LookupList=SimpleNamespace(Lookup=[one, two])))}
        with self.assertRaisesRegex(ValueError, 'conflicting'):
            convert._single_substitution(font, 'smcp', {'H'})
        one.LookupType = 6
        with self.assertRaisesRegex(ValueError, 'unsupported GSUB'):
            convert._single_substitution(font, 'smcp', {'H'})

    def test_feature_packer_rejects_duplicates_bad_ids_keys_and_bounds(self):
        glyph = convert.GlyphProps(2, 2, 100, 0, 2, 1, 0, 0x110000)
        good = convert.SmallCapsData([(104, 0)], [(72, 0)], [(glyph, b'\xff')], [(72, 0x110000, -5)], 1)
        self.assertTrue(convert.pack_small_caps(good, {72, 104}))
        cases = [good._replace(smcp=[(104, 0), (104, 0)]), good._replace(smcp=[(104, 1)]),
                 good._replace(smcp=[(105, 0)]), good._replace(smcp=[], c2sc=[]),
                 good._replace(kern_pairs=[(72, 104, -1)]), good._replace(kern_pairs=[(72, 0x110001, -1)]),
                 good._replace(kern_pairs=[(72, 0x110000, 0)]), good._replace(kern_pairs=[(72, 0x110000, 32768)]),
                 good._replace(kern_pairs=good.kern_pairs * 2), good._replace(total_bitmap_size=0),
                 good._replace(all_glyphs=[(glyph._replace(width=65536), b'\xff')]),
                 good._replace(smcp=[(104, 0)] * 65536)]
        for bad in cases:
            with self.assertRaises(ValueError):
                convert.pack_small_caps(bad, {72, 104})


if __name__ == '__main__':
    unittest.main(verbosity=2)
