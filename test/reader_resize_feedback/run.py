#!/usr/bin/env python3
"""Exercise production EPUB resize routing and feedback ordering with host hardware shims."""
import argparse
from pathlib import Path
import subprocess


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
p.add_argument('--out', type=Path, required=True)
p.add_argument('--sanitize', action='store_true')
a = p.parse_args()
r, out = a.repo.resolve(), a.out.resolve()
out.mkdir(parents=True, exist_ok=True)
source = (r / 'src/activities/reader/EpubReaderActivity.cpp').read_text()
methods = [function(source, 'void EpubReaderActivity::applyBookReaderOverrides(const int8_t'),
           function(source, 'void EpubReaderActivity::applyBookReaderOverrides(\n')]
declarations = '\n'.join(m.split('{', 1)[0].replace('EpubReaderActivity::', '').replace(
    'const bool acknowledgeResize)', 'const bool acknowledgeResize = false)') + ';' for m in methods)
routing = source.split('    case BA::BTN_CYCLE_FONT_SIZE:', 1)[1].split('    case BA::BTN_CYCLE_ORIENTATION:', 1)[0]
routing = 'case BA::BTN_CYCLE_FONT_SIZE:' + routing
probe = Path(__file__).with_name('Probe.cpp.in').read_text()
for marker, replacement in [('DECLARATIONS', declarations), ('METHODS', '\n'.join(methods)), ('ROUTING', routing)]:
    probe = probe.replace('// ' + marker, replacement)
(out / 'HalStorage.h').write_text('#pragma once\nclass FsFile;\n')
path = out / 'probe.cpp'
path.write_text(probe)
binary = out / 'reader-resize-feedback'
flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if a.sanitize else []
subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-g', *flags,
                '-I' + str(out), '-I' + str(r / 'src'), str(path), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
