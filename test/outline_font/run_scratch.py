#!/usr/bin/env python3
"""Test retained FreeType scratch, OOM recovery and independent face concurrency.

Run with a Python environment containing fontTools. Additional font files are
optional; the source-generated CFF and bundled Bookerly are always exercised.
"""
import argparse
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--out', required=True, type=Path)
parser.add_argument('--sanitize', action='store_true')
parser.add_argument('fonts', nargs='*')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=True)
subprocess.run([sys.executable, str(Path(__file__).with_name('make_stack_fixtures.py')), str(out)], check=True)
fonts = [out / 'stack-cff.otf', repo / 'lib/EpdFont/builtinFonts/source/Bookerly/Bookerly-Regular.ttf']
fonts += [Path(p).resolve() for p in args.fonts]
flags = ['--sanitize'] if args.sanitize else []
subprocess.run([sys.executable, str(Path(__file__).with_name('run.py')), '--out', str(out / 'build'),
                '--raster-only', *flags, *map(str, fonts)], check=True)
ft = repo / 'lib/FreeType'
compile_flags = ['-std=c++17', '-g', '-O1' if args.sanitize else '-Os',
                 '-DFT_CONFIG_OPTIONS_H=<witch_ftoption.h>', '-I' + str(ft / 'config'),
                 '-I' + str(ft / 'third_party/freetype/include'),
                 '-I' + str(ft / 'third_party/freetype/src/autofit'),
                 '-I' + str(repo / 'lib/OutlineFont'), '-I' + str(repo / 'lib/EpdFont')]
if args.sanitize:
    compile_flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
exe = out / 'scratch-ownership'
subprocess.run(['clang++', *compile_flags, str(Path(__file__).with_name('ScratchOwnershipTest.cpp')),
                *map(str, sorted((out / 'build').glob('*.o'))), '-o', str(exe)], check=True)
subprocess.run([str(exe), str(out / 'stack-nested-cff.otf'), *map(str, fonts)], check=True)
