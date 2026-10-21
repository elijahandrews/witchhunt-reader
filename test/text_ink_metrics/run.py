#!/usr/bin/env python3
"""Exercise production vertical ink metrics with a counted glyph/raster provider."""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--renderer-source', type=Path, help='Use a frozen renderer method as a negative control')
    args = parser.parse_args()
    repo, out = args.repo.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = (args.renderer_source or repo / 'lib/GfxRenderer/GfxRenderer.cpp').read_text()
    start, end = 'bool GfxRenderer::getTextInkMetrics(', 'void GfxRenderer::drawCenteredText('
    assert source.count(start) == 1 and source.count(end) == 1
    method = start + source.split(start, 1)[1].split(end, 1)[0]
    code = (Path(__file__).parent / 'Probe.cpp').read_text().replace('// PRODUCTION_METHOD', method)
    (out / 'probe.cpp').write_text(code)
    command = ['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-O2', '-g']
    if args.sanitize:
        command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    command += ['-I' + str(repo / 'lib' / folder) for folder in ['GfxRenderer', 'EpdFont', 'Utf8']]
    command += [str(out / 'probe.cpp')] + [str(repo / 'lib' / path) for path in [
        'EpdFont/EpdFont.cpp', 'EpdFont/EpdFontFamily.cpp', 'EpdFont/GlyphFallback.cpp', 'Utf8/Utf8.cpp']]
    command += ['-o', str(out / 'probe')]
    subprocess.run(command, check=True)
    subprocess.run([str(out / 'probe')], check=True)


if __name__ == '__main__':
    main()
