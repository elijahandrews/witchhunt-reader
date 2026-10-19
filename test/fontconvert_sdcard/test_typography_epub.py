"""Original embedded-font EPUB and matching v6 preview consistency checks."""
import contextlib
import importlib.util
import io
import json
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from zipfile import ZipFile

from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
from test_fontconvert_features import read_caps

REPO = Path(__file__).resolve().parents[2]
GENERATOR = REPO / 'test/epubs/make_test_css_typography.py'


class TypographyEpubTests(unittest.TestCase):
    def test_embedded_original_faces_features_and_v6_preview(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            epub, preview = root / 'fixture.epub', root / 'preview'
            command = [sys.executable, str(GENERATOR), '--output', str(epub), '--preview-dir', str(preview)]
            result = subprocess.run(command, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            with ZipFile(epub) as archive:
                self.assertEqual(archive.namelist()[0], 'mimetype')
                fonts = {Path(name).name: archive.read(name) for name in archive.namelist() if name.endswith('.ttf')}
                self.assertEqual(len(fonts), 6)
                css = archive.read('OEBPS/style.css').decode()
                self.assertIn('font-variant-caps: all-small-caps', css)
                self.assertIn('Missing Fixture', css)
                opf = ET.fromstring(archive.read('OEBPS/content.opf'))
                ns = {'o': 'http://www.idpf.org/2007/opf'}
                items = opf.findall('o:manifest/o:item', ns)
                self.assertEqual(sum(item.get('media-type') == 'font/ttf' for item in items), 6)
                self.assertEqual(sum(item.get('media-type') == 'font/otf' for item in items), 1)
                self.assertTrue(archive.read('OEBPS/fonts/FixtureCff-Regular.otf').startswith(b'OTTO'))
                for item in items:
                    self.assertIn('OEBPS/' + item.get('href'), archive.namelist())
                for name in archive.namelist():
                    if name.endswith('.xhtml'):
                        ET.fromstring(archive.read(name))
            outlines = []
            for label in ('Regular', 'Bold', 'Italic', 'BoldItalic'):
                with TTFont(io.BytesIO(fonts[f'FixtureForms-{label}.ttf'])) as font:
                    cmap = font.getBestCmap()
                    self.assertTrue(all(ord(c) in cmap for c in 'HNT IF hnt if'))
                    self.assertTrue(all(c + '.sc' not in cmap.values() for c in 'HNTIF'))
                    feature_map = {}
                    table = font['GSUB'].table
                    for record in table.FeatureList.FeatureRecord:
                        if record.FeatureTag in ('smcp', 'c2sc'):
                            values = {}
                            for index in record.Feature.LookupListIndex:
                                for subtable in table.LookupList.Lookup[index].SubTable:
                                    values.update(subtable.mapping)
                            feature_map[record.FeatureTag] = values
                    self.assertEqual(feature_map['smcp'], {c.lower(): c + '.sc' for c in 'HNTIF'})
                    self.assertEqual(feature_map['c2sc'], {c: c + '.sc' for c in 'HNTIF'})
                    outlines.append(tuple(font['glyf']['H'].coordinates))
            self.assertEqual(len(set(outlines)), 4)
            with TTFont(io.BytesIO(fonts['FixtureForms-Regular.ttf'])) as regular, TTFont(io.BytesIO(fonts['FixtureWide-Regular.ttf'])) as wide:
                self.assertGreater(wide['hmtx']['H'][0], regular['hmtx']['H'][0])
            with TTFont(io.BytesIO(fonts['FixtureOptical-Variable.ttf'])) as font:
                self.assertEqual({axis.axisTag for axis in font['fvar'].axes}, {'opsz', 'wght', 'ital'})
                small = instantiateVariableFont(font, {'opsz': 8, 'wght': 400, 'ital': 0}, inplace=False)
                large = instantiateVariableFont(font, {'opsz': 72, 'wght': 400, 'ital': 0}, inplace=False)
                self.assertNotEqual(list(small['glyf']['H'].coordinates), list(large['glyf']['H'].coordinates))
                self.assertGreater(small['hmtx']['H'][0], large['hmtx']['H'][0])
            expected = json.loads((preview / 'expected.json').read_text())
            self.assertEqual((expected['format'], expected['density'], expected['styles']), (6, 2, 4))
            _, normal, styles = read_caps(preview / 'FixtureForms_14.cpfont')
            self.assertEqual(len(styles), 4)
            for caps in styles.values():
                self.assertEqual(set(caps['smcp']), {ord(c) for c in expected['smcp_sources']})
                self.assertEqual(set(caps['c2sc']), {ord(c) for c in expected['c2sc_sources']})
                self.assertEqual(len(caps['glyphs']), expected['alternate_count_per_style'])
                for lower, upper in zip('fhint', 'FHINT'):
                    self.assertEqual(caps['smcp'][ord(lower)], caps['c2sc'][ord(upper)])
            # Fixed source/ZIP timestamps make regeneration reviewable byte-for-byte.
            first = epub.read_bytes()
            result = subprocess.run(command[:-2], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(first, epub.read_bytes())


if __name__ == '__main__':
    unittest.main(verbosity=2)
