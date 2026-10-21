#!/usr/bin/env python3
"""Reject large FreeType frames and run the real backend on a small pthread stack."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def read_usage(directory):
    records = []
    for path in sorted(directory.glob('*.su')):
        for line in path.read_text().splitlines():
            fields = line.split('\t')
            if len(fields) != 3:
                raise RuntimeError(f'Unrecognized compiler stack record: {line}')
            location, size, kind = fields
            records.append({'function': location.rsplit(':', 1)[-1], 'bytes': int(size),
                            'kind': kind, 'location': location, 'report': str(path)})
    if not records:
        raise RuntimeError(f'Compiler emitted no stack-usage records in {directory}')
    return records


def violations(records, limit):
    result = []
    for record in records:
        if record['bytes'] > limit or ('dynamic' in record['kind'] and 'bounded' not in record['kind']):
            result.append(record)
    names = {record['function'] for record in records}
    # The small converter can inline into gray_raster_render after the fix.
    for required in (('cf2_interpT2CharString',), ('gray_convert_glyph', 'gray_raster_render'),
                     ('af_autofitter_load_glyph',)):
        if not names.intersection(required):
            raise RuntimeError(f'Stack report omitted required production function group: {required}')
    return sorted(result, key=lambda record: record['bytes'], reverse=True)


def compile_backend(repo, output, cc, target=False):
    output.mkdir(parents=True, exist_ok=True)
    ft = repo / 'lib/FreeType'
    flags = ['-Os', '-fstack-usage', '-DWITCH_FONT_AUTOHINT=1',
             '-I' + str(ft / 'config'), '-I' + str(ft / 'third_party/freetype/include')]
    if target:
        flags += ['-mlongcalls']
    objects, commands = [], []
    for source in sorted((ft / 'src').glob('*.c')):
        obj = output / (source.stem + '.o')
        # A stale .su file must never satisfy the guard after compiler failure.
        obj.with_suffix('.su').unlink(missing_ok=True)
        command = [cc, *flags, '-c', str(source), '-o', str(obj)]
        commands.append(command)
        subprocess.run(command, check=True)
        objects.append(obj)
    return flags, objects, commands


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fonts', nargs='*', type=Path, help='Local CFF/TTF fixtures for the constrained-stack runtime check')
    parser.add_argument('--repo-root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--cc', default='clang')
    parser.add_argument('--cxx', default='clang++')
    parser.add_argument('--target-cc', type=Path, help='Actual xtensa-esp32s3-elf-gcc; no emulator or device needed')
    parser.add_argument('--static-only', action='store_true', help='Compile stack reports without running any font')
    args = parser.parse_args()
    if not args.static_only and not args.fonts:
        parser.error('Provide local font fixtures or explicitly select --static-only')
    repo, output = args.repo_root.resolve(), args.out.resolve()
    output.mkdir(parents=True, exist_ok=True)
    flags, objects, commands = compile_backend(repo, output / 'host', args.cc)
    host = read_usage(output / 'host')
    reports = {'host': {'limit_per_frame': 4096, 'records': host, 'violations': violations(host, 4096)}}
    if args.target_cc:
        _, _, target_commands = compile_backend(repo, output / 'xtensa', str(args.target_cc.resolve()), True)
        commands += target_commands
        target = read_usage(output / 'xtensa')
        reports['xtensa'] = {'limit_per_frame': 2048, 'records': target, 'violations': violations(target, 2048)}
    source_paths = sorted(path for folder in ('lib/FreeType', 'lib/OutlineFont')
                          for path in (repo / folder).rglob('*') if path.suffix in ('.c', '.h', '.cpp'))
    source_paths.append(repo / 'test/real_renderer/stack_probe.cpp')
    source_hashes = {str(path.relative_to(repo)): hashlib.sha256(path.read_bytes()).hexdigest() for path in source_paths}
    report = {'reports': reports, 'commands': commands, 'source_sha256': source_hashes,
              'scope': 'Per-frame compile bound, not a whole-program maximum call-chain proof',
              'runtime': {'stack_bytes': 32768, 'reserved_caller_bytes': 8192, 'ran': False}}
    report_path = output / 'stack-usage.json'
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    failed = False
    for name, result in reports.items():
        largest = max(result['records'], key=lambda record: record['bytes'])
        print(f"{name}: {len(result['records'])} frames, largest {largest['function']}={largest['bytes']} bytes", flush=True)
        for record in result['violations']:
            failed = True
            print(f"FAIL {name}: {record['function']}={record['bytes']} ({record['kind']}), limit {result['limit_per_frame']}", flush=True)
    if failed:
        raise SystemExit(1)
    if args.static_only:
        return
    backend_flags = flags + ['-I' + str(repo / 'lib/EpdFont'), '-I' + str(repo / 'lib/OutlineFont')]
    for source in sorted((repo / 'lib/OutlineFont').glob('*.cpp')):
        obj = output / 'host' / ('outline_' + source.stem + '.o')
        command = [args.cxx, '-std=c++17', *backend_flags, '-c', str(source), '-o', str(obj)]
        commands.append(command)
        subprocess.run(command, check=True)
        objects.append(obj)
    exe = output / 'stack_probe'
    command = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', *backend_flags, '-pthread',
               str(repo / 'test/real_renderer/stack_probe.cpp'), *map(str, objects), '-o', str(exe)]
    commands.append(command)
    subprocess.run(command, check=True)
    fonts = [str(path.resolve()) for path in args.fonts]
    result = subprocess.run([str(exe), *fonts], check=False, text=True, capture_output=True)
    (output / 'runtime.log').write_text(result.stdout + result.stderr)
    print(result.stdout, end='')
    print(result.stderr, end='')
    report['runtime'].update(ran=True, exit_code=result.returncode,
                             font_sha256={path: hashlib.sha256(Path(path).read_bytes()).hexdigest() for path in fonts},
                             executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest())
    report['commands'] = commands
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    if result.returncode:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
