#!/usr/bin/env python3
"""Exercise production reader exit/OOM cleanup with bounded host PSRAM."""
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
p.add_argument('--case', choices=('all', 'exit', 'oom'), default='all')
p.add_argument('--sanitize', action='store_true')
a = p.parse_args()
repo, out = a.repo.resolve(), a.out.resolve()
out.mkdir(parents=True, exist_ok=True)
reader = (repo / 'src/activities/reader/EpubReaderActivity.cpp').read_text()
exit_body = function(reader, 'void EpubReaderActivity::onExit() {').split('{', 1)[1].split(
    '  // Record the paragraph under the saved page,', 1)[0]
oom_body = function(reader, 'if (pageLoadFailStage_ == 1) {')
restore = function(reader, 'void EpubReaderActivity::restoreCurrentPageToBufferIfPreRendered(')
content = function(reader, 'uint16_t EpubReaderActivity::renderPageContentOnly(')
(out / 'esp_heap_caps.h').write_text('''#pragma once
#include <cstddef>
#include <cstdint>
constexpr uint32_t MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
void* heap_caps_malloc(size_t, uint32_t);
void heap_caps_free(void*);
size_t heap_caps_get_free_size(uint32_t);
size_t heap_caps_get_largest_free_block(uint32_t);
''')
probe = Path(__file__).with_name('Probe.cpp.in').read_text()
for key, value in [('EXIT_BODY', exit_body), ('OOM_BODY', oom_body), ('RESTORE', restore), ('CONTENT', content)]:
    probe = probe.replace('// ' + key, value)
source = out / 'probe.cpp'
source.write_text(probe)
binary = out / 'reader-memory'
flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if a.sanitize else []
subprocess.run(['c++', '-std=c++20', '-O1', '-g', *flags, '-DBOARD_HAS_PSRAM=1',
                '-I' + str(out), '-I' + str(repo / 'src'), str(source), '-o', str(binary)], check=True)
result = subprocess.run([str(binary), a.case], capture_output=True, text=True)
(out / 'run.log').write_text(result.stdout + result.stderr)
print(result.stdout + result.stderr, end='')
assert result.returncode == 0, 'Production reader optional-AA memory regression failed'
