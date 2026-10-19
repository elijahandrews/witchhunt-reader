#!/usr/bin/env python3
"""Extract only original code-generated diagnostic fonts from the test EPUB."""
import argparse
from pathlib import Path
import struct
from zipfile import ZipFile
parser = argparse.ArgumentParser()
parser.add_argument('--epub', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
with ZipFile(args.epub) as archive:
    for name in archive.namelist():
        if name.startswith('OEBPS/fonts/Fixture') and name.endswith(('.ttf', '.otf')):
            (args.out / Path(name).name).write_bytes(archive.read(name))
# A larger, still valid GPOS table forces aggregate memory pressure without
# changing any outline or positioning instruction. OpenType allows unreferenced
# trailing bytes. Recompute sfnt table/global checksums after appending padding.
font = bytearray((args.out / 'FixtureForms-Regular.ttf').read_bytes())
head = None
for i in range(struct.unpack_from('>H', font, 4)[0]):
    record = 12 + i * 16
    tag, checksum, offset, length = struct.unpack_from('>4sIII', font, record)
    if tag == b'head':
        head = offset
    if tag == b'GPOS':
        table = bytes(font[offset:offset + length]).ljust(300000, b'\0')
        font.extend(bytes((-len(font)) % 4))
        new_offset = len(font)
        table_checksum = sum(struct.unpack('>' + 'I' * (len(table) // 4), table)) & 0xffffffff
        struct.pack_into('>III', font, record + 4, table_checksum, new_offset, len(table))
        font.extend(table)
assert head is not None
struct.pack_into('>I', font, head + 8, 0)
font.extend(bytes((-len(font)) % 4))
checksum = sum(struct.unpack('>' + 'I' * (len(font) // 4), font)) & 0xffffffff
struct.pack_into('>I', font, head + 8, (0xb1b0afba - checksum) & 0xffffffff)
(args.out / 'FixtureLargeLayout.ttf').write_bytes(font)
(args.out / 'corrupt.ttf').write_bytes(b'not an sfnt font')
(args.out / 'stamp').write_text('original fixture fonts extracted\n')
