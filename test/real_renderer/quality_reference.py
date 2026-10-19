#!/usr/bin/env python3
"""Compare bitmap scaling models with a final-size FreeType outline reference.

Requires clang/clang++ and freetype-py. The default input is the repository's
Bookerly Regular 14 source and actual shipped bitmap, decoded by the production
EpdFont/FontDecompressor code. Optional local font/cpfont pairs must be Regular,
14pt at 150 DPI. No fonts are regenerated, installed, or modified.

The scaling columns are explicitly mathematical MODELS, not screenshots of the
production renderer. Use run.py separately for production rendering regression
checks. Final-size LIGHT rendering is a useful comparison, not an absolute
quality oracle: hinting preferences vary, and intentional font contrast and
overshoots must remain. Image grays represent ideal linear coverage, not a
calibrated e-ink waveform or monitor gamma. L1 differences are not perceptual
quality scores. Generated images and glyph dumps can contain licensed local
font pixels; keep --out outside version control.

Primary references:
https://freetype.org/freetype2/docs/glyphs/glyphs-3.html
https://freetype.org/freetype2/docs/reference/ft2-glyph_retrieval.html#ft_load_target_xxx
https://freetype.org/freetype2/docs/hinting/text-rendering-general.html
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import zlib

# Set before importing FreeType: the experiment fixes this variable explicitly.
# This changes only this process and child processes, never installed fonts.
os.environ['FREETYPE_PROPERTIES'] = (
    'autofitter:no-stem-darkening=1 cff:no-stem-darkening=1 '
    'type1:no-stem-darkening=1 t1cid:no-stem-darkening=1'
)

SCALE = 0.75
POINT_SIZE = 14
DPI = 150
CODEPOINTS = range(32, 127)
SAMPLE = 'HINTING NTHHE ANMT'
SOURCES = [
    'https://freetype.org/freetype2/docs/glyphs/glyphs-3.html',
    'https://freetype.org/freetype2/docs/reference/ft2-glyph_retrieval.html#ft_load_target_xxx',
    'https://freetype.org/freetype2/docs/hinting/text-rendering-general.html',
]

BOOKERLY_DUMP_CPP = r'''
#include <FontDecompressor.h>
#include <EpdFont.h>
#include <builtinFonts/bookerly_14_regular.h>
#include <iostream>
int main() {
  EpdFont font(&bookerly_14_regular);
  FontDecompressor decompressor;
  if (!decompressor.init()) return 2;
  std::cout << "{";
  for (unsigned cp = 32; cp < 127; ++cp) {
    const auto glyph = font.getGlyph(cp);
    if (!glyph) return 3;
    const auto* bits = decompressor.getBitmap(font.data, glyph, glyph.index);
    if (!bits && glyph.width && glyph.height) return 4;
    if (cp != 32) std::cout << ",";
    std::cout << "\"" << cp << "\":{\"w\":" << int(glyph.width)
              << ",\"h\":" << int(glyph.height)
              << ",\"left\":" << int(glyph.left)
              << ",\"top\":" << int(glyph.top)
              << ",\"advance\":" << glyph.advanceX / 16.0
              << ",\"pixels\":[";
    for (int pixel = 0; pixel < int(glyph.width) * glyph.height; ++pixel) {
      if (pixel) std::cout << ",";
      std::cout << int((bits[pixel / 4] >> ((3 - pixel % 4) * 2)) & 3);
    }
    std::cout << "]}";
  }
  std::cout << "}\n";
}
'''


def dump_bookerly(repo, output):
    """Compile only the existing production font decoder, not a font converter."""
    source = output / 'bookerly_dump.cpp'
    source.write_text(BOOKERLY_DUMP_CPP)
    c_command = [
        'clang', '-c', str(repo / 'lib/uzlib/src/tinflate.c'),
        '-o', str(output / 'tinflate.o'),
    ]
    includes = [repo / 'test/real_renderer', repo / 'test/shims'] + [
        repo / 'lib' / name
        for name in ('EpdFont', 'Memory', 'Logging', 'Utf8', 'InflateReader', 'uzlib/src')
    ]
    sources = [source] + [
        repo / 'lib' / name
        for name in (
            'EpdFont/EpdFont.cpp', 'EpdFont/FontDecompressor.cpp',
            'EpdFont/GlyphFallback.cpp', 'Utf8/Utf8.cpp',
            'InflateReader/InflateReader.cpp',
        )
    ]
    command = ['clang++', '-std=c++20', '-O1', '-ffunction-sections', '-fdata-sections']
    command.append('-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections')
    command += ['-I' + str(path) for path in includes]
    command += [str(path) for path in sources]
    command += [str(output / 'tinflate.o'), '-o', str(output / 'bookerly_dump')]
    (output / 'build-command.json').write_text(json.dumps([c_command, command], indent=2) + '\n')
    with (output / 'build.log').open('w') as log:
        subprocess.run(c_command, check=True, stdout=log, stderr=subprocess.STDOUT)
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
    raw = subprocess.check_output([str(output / 'bookerly_dump')])
    (output / 'bookerly14-bitmap.json').write_bytes(raw)
    return {int(key): value for key, value in json.loads(raw).items()}


def read_cpfont(path):
    """Read the Regular face of a v4 2-bit cpfont, checking section bounds."""
    data = path.read_bytes()

    def unsigned(at, count):
        if at < 0 or at + count > len(data):
            raise ValueError(f'Truncated cpfont: {path}')
        return int.from_bytes(data[at:at + count], 'little')

    if (len(data) < 32 or data[:8] != b'CPFONT\0\0' or
            unsigned(8, 2) != 4 or unsigned(10, 2) != 1):
        raise ValueError(f'Expected v4 2-bit cpfont: {path}')
    count = unsigned(12, 1)
    if not 1 <= count <= 4 or len(data) < 32 + count * 32:
        raise ValueError(f'Invalid cpfont style table: {path}')
    matches = [32 + i * 32 for i in range(count) if unsigned(32 + i * 32, 1) == 0]
    if len(matches) != 1:
        raise ValueError(f'Expected one Regular face: {path}')
    toc = matches[0]
    at = unsigned(toc + 24, 4)
    if at < 32 + count * 32:
        raise ValueError(f'Invalid cpfont data offset: {path}')

    def records(fmt, number):
        nonlocal at
        length = struct.calcsize(fmt)
        if number > (len(data) - at) // length:
            raise ValueError(f'Truncated cpfont records: {path}')
        result = [struct.unpack_from(fmt, data, at + i * length) for i in range(number)]
        at += number * length
        return result

    intervals = records('<III', unsigned(toc + 4, 4))
    glyphs = records('<BBHhhH2xI', unsigned(toc + 8, 4))
    at += 3 * (unsigned(toc + 17, 2) + unsigned(toc + 19, 2))
    at += unsigned(toc + 21, 1) * unsigned(toc + 22, 1) + 8 * unsigned(toc + 23, 1)
    if at > len(data):
        raise ValueError(f'Truncated cpfont bitmap offset: {path}')
    result = {}
    for cp in CODEPOINTS:
        matching = [offset + cp - start for start, end, offset in intervals if start <= cp <= end]
        if len(matching) != 1 or matching[0] >= len(glyphs):
            raise ValueError(f'Missing or ambiguous ASCII U+{cp:04X}: {path}')
        width, height, advance, left, top, length, offset = glyphs[matching[0]]
        if length != (width * height + 3) // 4 or at + offset + length > len(data):
            raise ValueError(f'Invalid cpfont bitmap U+{cp:04X}: {path}')
        bitmap = data[at + offset:at + offset + length]
        pixels = [(bitmap[i // 4] >> ((3 - i % 4) * 2)) & 3 for i in range(width * height)]
        result[cp] = dict(w=width, h=height, left=left, top=top, advance=advance / 16, pixels=pixels)
    return result


def outline_font(path, point_size=POINT_SIZE, quantize=True):
    face = freetype.Face(str(path))
    face.set_char_size(round(point_size * 64), 0, DPI, DPI)
    flags = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_NO_BITMAP | freetype.FT_LOAD_TARGET_LIGHT
    result = {}
    for cp in CODEPOINTS:
        if face.get_char_index(cp) == 0:
            raise ValueError(f'Missing ASCII U+{cp:04X}: {path}')
        face.load_char(cp, flags)
        glyph, bitmap = face.glyph, face.glyph.bitmap
        buffer = bitmap.buffer
        pixels = []
        for y in range(bitmap.rows):
            row = y * bitmap.pitch if bitmap.pitch >= 0 else (bitmap.rows - 1 - y) * -bitmap.pitch
            pixels += [buffer[row + x] * 3 / 255 for x in range(bitmap.width)]
        if quantize:
            pixels = [math.floor(value + 0.5) for value in pixels]
        result[cp] = dict(
            w=bitmap.width, h=bitmap.rows, left=glyph.bitmap_left,
            top=glyph.bitmap_top, advance=glyph.linearHoriAdvance / 65536, pixels=pixels,
        )
    return result


def scale_model(glyph, baseline, scale=SCALE):
    """Independent area-coverage model, intentionally not production C++ code.

    The legacy model retains old crop-local sampling and truncating bearing
    rounding, including its incorrect behavior for negative left bearings.
    The models use the exact scale rather than the production 16.16 reciprocal
    approximation; this experiment measures geometry, not bitwise equivalence.
    """
    width, height = glyph['w'], glyph['h']
    left, top = glyph['left'], glyph['top']
    if baseline:
        x0, y0 = math.floor(left * scale), math.floor(-top * scale)
        x1, y1 = math.ceil((left + width) * scale), math.ceil((height - top) * scale)
        source_x, source_y = (x0 - left * scale) / scale, (y0 + top * scale) / scale
    else:
        x0, y0 = int(left * scale + 0.5), -int(top * scale + 0.5)
        x1, y1 = x0 + int(width * scale + 0.5), y0 + int(height * scale + 0.5)
        source_x = source_y = 0
    scaled_width, scaled_height = x1 - x0, y1 - y0
    pixels = []
    for y in range(scaled_height):
        ya, yb = source_y + y / scale, source_y + (y + 1) / scale
        for x in range(scaled_width):
            xa, xb = source_x + x / scale, source_x + (x + 1) / scale
            ink = 0
            for sy in range(max(0, math.floor(ya)), min(height, math.ceil(yb))):
                for sx in range(max(0, math.floor(xa)), min(width, math.ceil(xb))):
                    overlap = max(0, min(xb, sx + 1) - max(xa, sx))
                    overlap *= max(0, min(yb, sy + 1) - max(ya, sy))
                    ink += glyph['pixels'][sy * width + sx] * overlap
            coverage = ink * scale * scale
            if baseline:
                # Keep the modern model's exact ties consistent with the renderer.
                # Snap only floating-point projection noise before Python's
                # nearest-even rounding; retain the historical model below.
                half = math.floor(coverage) + 0.5
                if math.isclose(coverage, half, rel_tol=0, abs_tol=1e-10):
                    coverage = half
                pixels.append(min(3, round(coverage)))
            else:
                pixels.append(min(3, math.floor(coverage + 0.5 + 1e-10)))
    return dict(w=scaled_width, h=scaled_height, left=x0, top=-y0,
                advance=glyph['advance'] * scale, pixels=pixels)


def sparse_ink(glyph):
    return {
        (glyph['left'] + x, y - glyph['top']): glyph['pixels'][y * glyph['w'] + x]
        for y in range(glyph['h']) for x in range(glyph['w'])
        if glyph['pixels'][y * glyph['w'] + x] != 0
    }


def measure(glyph, reference):
    actual, expected = sparse_ink(glyph), sparse_ink(reference)
    total = sum(expected.values())
    difference = sum(abs(actual.get(p, 0) - expected.get(p, 0)) for p in actual.keys() | expected.keys())
    return dict(ink_area=sum(actual.values()) / 3, reference_ink_area=total / 3,
                ink_ratio=sum(actual.values()) / total if total else None,
                relative_L1=difference / total if total else None)


def padding_check(font):
    result = {}
    for baseline in (False, True):
        failures = []
        tested = 0
        for cp, glyph in font.items():
            if not glyph['w'] or not glyph['h']:
                continue
            tested += 1
            padded = dict(glyph, w=glyph['w'] + 1, h=glyph['h'] + 1,
                          left=glyph['left'] - 1, top=glyph['top'] + 1,
                          pixels=[0] * (glyph['w'] + 1))
            for row in range(glyph['h']):
                padded['pixels'].extend([0] + glyph['pixels'][row * glyph['w']:(row + 1) * glyph['w']])
            if sparse_ink(scale_model(glyph, baseline)) != sparse_ink(scale_model(padded, baseline)):
                failures.append(cp)
        result['baseline_model' if baseline else 'legacy_model'] = dict(tested=tested, failures=failures)
    return result


class Image:
    def __init__(self, width, height):
        self.width, self.height = width, height
        self.pixels = bytearray([255]) * (width * height * 3)

    def pixel(self, x, y, color):
        if 0 <= x < self.width and 0 <= y < self.height:
            at = (y * self.width + x) * 3
            self.pixels[at:at + 3] = bytes(color)

    def glyph(self, glyph, x, baseline, zoom=1):
        for gy in range(glyph['h']):
            for gx in range(glyph['w']):
                value = round(255 * (1 - glyph['pixels'][gy * glyph['w'] + gx] / 3))
                if value == 255:
                    continue
                for dy in range(zoom):
                    for dx in range(zoom):
                        self.pixel(x + (glyph['left'] + gx) * zoom + dx,
                                   baseline + (gy - glyph['top']) * zoom + dy,
                                   (value, value, value))

    def text(self, text, font, x, baseline):
        for character in text:
            glyph = font[ord(character)]
            self.glyph(glyph, x, baseline)
            x += round(glyph['advance'])

    def save(self, path):
        def chunk(kind, data):
            return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
        rows = b''.join(b'\0' + self.pixels[y * self.width * 3:(y + 1) * self.width * 3] for y in range(self.height))
        path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', self.width, self.height, 8, 2, 0, 0, 0)) +
                         chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--repo', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    for name in ('adobe', 'eb'):
        parser.add_argument(f'--{name}-font', type=Path, help='Optional local Regular outline font')
        parser.add_argument(f'--{name}-cpfont', type=Path, help='Matching local Regular 14pt/150 DPI v4 bitmap font')
    args = parser.parse_args()
    for name in ('adobe', 'eb'):
        if bool(getattr(args, name + '_font')) != bool(getattr(args, name + '_cpfont')):
            parser.error(f'--{name}-font and --{name}-cpfont must be supplied together')
    global freetype
    try:
        import freetype
    except ImportError:
        parser.error('freetype-py is required in the selected Python environment')
    repo, output = args.repo.expanduser().resolve(), args.out.expanduser().resolve()
    if output == repo or repo in output.parents:
        parser.error('--out must be outside the repository; generated images contain local font pixels')
    bookerly = repo / 'lib/EpdFont/builtinFonts/source/Bookerly/Bookerly-Regular.ttf'
    inputs = [('BOOKERLY', bookerly, None)]
    for name, label in (('adobe', 'ADOBE GARAMOND PRO'), ('eb', 'EB GARAMOND')):
        outline, bitmap = getattr(args, name + '_font'), getattr(args, name + '_cpfont')
        if outline:
            inputs.append((label, outline.expanduser().resolve(), bitmap.expanduser().resolve()))
    for _, outline, bitmap in inputs:
        for path in (outline, bitmap):
            if path is not None and not path.is_file():
                parser.error(f'File does not exist: {path}')
    output.mkdir(parents=True, exist_ok=True)
    current_bookerly = dump_bookerly(repo, output)
    label_font = outline_font(bookerly, 7, False)
    image = Image(1520, 100 + len(inputs) * 306)
    image.text('14PT X 75% / REGULAR / 150 DPI / MODELS COMPARED WITH FINAL-SIZE LIGHT OUTLINES', label_font, 20, 25)
    image.text('IDEALIZED LINEAR GRAYS; NOT A PANEL CALIBRATION OR A UNIVERSAL QUALITY SCORE', label_font, 20, 44)
    metrics, padding, provenance = {}, {}, {}
    for row, (name, outline, bitmap) in enumerate(inputs):
        current = read_cpfont(bitmap) if bitmap else current_bookerly
        light = outline_font(outline)
        # V4 lacks a point-size field. Reject mismatched pairs rather than silently
        # comparing e.g. 12pt bitmap ink against a 14pt outline's final-size reference.
        if bitmap and any(abs(current[cp]['advance'] - light[cp]['advance']) > 1 / 16 for cp in CODEPOINTS):
            raise ValueError(f'{name}: cpfont advances do not match this Regular outline at 14pt/150 DPI')
        reference = outline_font(outline, POINT_SIZE * SCALE, False)
        native = outline_font(outline, POINT_SIZE * SCALE)
        dense = outline_font(outline, POINT_SIZE * 2)
        methods = [
            ('LEGACY CROP MODEL', {cp: scale_model(g, False) for cp, g in current.items()}),
            ('BASELINE MODEL', {cp: scale_model(g, True) for cp, g in current.items()}),
            ('2X LIGHT SOURCE MODEL', {cp: scale_model(g, True, SCALE / 2) for cp, g in dense.items()}),
            ('FINAL-SIZE LIGHT', native),
        ]
        y = 80 + row * 306
        image.text(name, label_font, 20, y)
        metrics[name] = {}
        for column, (title, font) in enumerate(methods):
            x = 20 + column * 375
            image.text(title, label_font, x, y + 26)
            image.text('4X PIXEL VIEW', label_font, x, y + 47)
            baseline = y + 166
            for xx in range(x, x + 348):
                if xx % 5 < 2:
                    image.pixel(xx, baseline, (170, 200, 240))
            for i, character in enumerate('NTHHE'):
                image.glyph(font[ord(character)], x + i * 65, baseline, 4)
            image.text('ACTUAL PIXELS:', label_font, x, y + 193)
            image.text(SAMPLE, font, x, y + 224)
            metrics[name][title] = {character: measure(font[ord(character)], reference[ord(character)])
                                    for character in 'NTHHEAM'}
        padding[name] = padding_check(current)
        provenance[name] = {
            'outline_name': outline.name, 'outline_sha256': hashlib.sha256(outline.read_bytes()).hexdigest(),
            'bitmap_name': bitmap.name if bitmap else 'bookerly_14_regular.h',
            'bitmap_sha256': hashlib.sha256((bitmap or repo / 'lib/EpdFont/builtinFonts/bookerly_14_regular.h').read_bytes()).hexdigest(),
        }
    image.save(output / 'comparison.png')
    metadata = {
        'freetype_version': freetype.version(), 'scale': SCALE, 'source_point_size': POINT_SIZE, 'dense_source_multiplier': 2, 'dpi': DPI,
        'inputs': provenance, 'sources': SOURCES,
        'outline_load_flags': 'FT_LOAD_RENDER | FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_LIGHT',
        'stem_darkening': False, 'quantizer': 'nearest coverage levels 0, 85, 170, 255',
        'limitations': [
            'Scaling columns are independent models, not production renderer screenshots.',
            'Final-size LIGHT is a comparison reference, not universal aesthetic truth.',
            'Gray values assume ideal linear coverage, not measured panel reflectance or gamma.',
            'L1 is a pixel difference measure, not a perceptual quality score.',
            'Only Regular ASCII at 14pt scaled to 75% is evaluated here.',
        ],
    }
    for filename, value in [('metrics.json', metrics), ('padding-invariance.json', padding), ('metadata.json', metadata)]:
        (output / filename).write_text(json.dumps(value, indent=2) + '\n')
    for name, result in padding.items():
        legacy, aligned = result['legacy_model'], result['baseline_model']
        print(f"{name}: padding changes {len(legacy['failures'])}/{legacy['tested']} legacy, "
              f"{len(aligned['failures'])}/{aligned['tested']} baseline glyphs")
    print('Comparison:', output / 'comparison.png')
    print('Measurements:', output / 'metrics.json')


if __name__ == '__main__':
    main()
