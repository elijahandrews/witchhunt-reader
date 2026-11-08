#!/usr/bin/env python3
"""Small, conservative state snapshots for an application-only X4 Pro update.

Books, fonts and dictionaries are not traversed or changed. Unknown files under
.crosspoint are retained; only known reproducible caches are omitted. Use
--include-web when the release needs a different web bundle. See docs/flashing-x4pro.md.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess

BOOK_CACHE = re.compile(r"(?:epub|txt|xtc|md)_\d+\Z")
DERIVED_FILES = {
    "book.bin", "details.bin", "pages.bin", "fingerprint.bin", "index.bin",
    "images.bin", "cover.img", "css_rules.cache", "css_scope.bin",
}
DERIVED_NAME = re.compile(r"(?:thumb_\d+\.bmp|html_\d+\.bin|css_fonts\.\d+\.bin)\Z")


def omit(relative, include_web=False):
    parts = relative.parts
    if any(part == ".DS_Store" or part.startswith("._") for part in parts):
        return True
    if parts[0] in {"fonts", "fontprev"} or (parts[0] == "web" and not include_web):
        return True
    if BOOK_CACHE.fullmatch(parts[0]) and len(parts) >= 2:
        # The EPUB implementation owns these two trees exclusively as derived data.
        if parts[1] in {"spines", "doc_css"}:
            return True
        if len(parts) == 2 and (parts[1] in DERIVED_FILES or DERIVED_NAME.fullmatch(parts[1])):
            return True
    return False


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def selected_files(root, include_web):
    if root.is_symlink() or not root.is_dir():
        raise ValueError("Expected a real .crosspoint directory")
    def fail(error):
        raise error
    for folder, dirs, files in os.walk(root, followlinks=False, onerror=fail):
        folder = Path(folder)
        for name in list(dirs):
            path = folder / name
            if path.is_symlink():
                raise ValueError("Symlink in state directory")
            if omit(path.relative_to(root), include_web):
                dirs.remove(name)
        for name in sorted(files):
            path = folder / name
            if path.is_symlink():
                raise ValueError("Symlink in state directory")
            if not omit(path.relative_to(root), include_web):
                if not path.is_file():
                    raise ValueError("Non-regular state file")
                yield path


def snapshot(mount, output, include_web=False, volume_uuid=None):
    mount, output = Path(mount).resolve(), Path(output).resolve()
    if output == mount or mount in output.parents:
        raise ValueError("Backup must be outside the reader volume")
    root = mount / ".crosspoint"
    if not (root / "settings.json").is_file():
        raise ValueError("Reader settings are missing; do not take an empty backup")
    output.mkdir(parents=True, exist_ok=False, mode=0o700)
    records = []
    for source in selected_files(root, include_web):
        relative = source.relative_to(mount)
        target = output / "files" / relative
        target.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        before = source.stat()
        digest = hashlib.sha256()
        with source.open("rb") as src, target.open("xb") as dst:
            for chunk in iter(lambda: src.read(1024 * 1024), b""):
                digest.update(chunk)
                dst.write(chunk)
        target.chmod(0o600)
        after = source.stat()
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise ValueError("State changed during backup")
        if target.stat().st_size != before.st_size or sha256(target) != digest.hexdigest():
            raise ValueError("Backup readback failed")
        records.append({"path": relative.as_posix(), "bytes": before.st_size, "sha256": digest.hexdigest()})
    manifest = {"schema": 1, "complete": True, "volume_uuid": volume_uuid,
                "scope": "Reader state only; no book/font/dictionary reads; known derived caches omitted",
                "include_web": include_web, "files": records}
    path = output / "manifest.json"
    path.write_text(json.dumps(manifest, indent=2) + "\n")
    path.chmod(0o600)
    return manifest


def mounted_reader(volume_uuid):
    info = plistlib.loads(subprocess.check_output(["diskutil", "info", "-plist", volume_uuid]))
    if (info.get("VolumeUUID", "").upper() != volume_uuid.upper() or info.get("Internal")
            or info.get("BusProtocol") != "USB" or not info.get("MountPoint")):
        raise ValueError("Expected reader UUID is not mounted as an external USB volume")
    return Path(info["MountPoint"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--volume-uuid", required=True)
    parser.add_argument("--output", type=Path, required=True, help="New private backup directory, outside the SD card")
    parser.add_argument("--include-web", action="store_true", help="Back up old web assets only when replacing the bundle")
    args = parser.parse_args()
    os.umask(0o077)
    manifest = snapshot(mounted_reader(args.volume_uuid), args.output, args.include_web, args.volume_uuid)
    print(f"Verified {len(manifest['files'])} state files, {sum(f['bytes'] for f in manifest['files'])} bytes")
    print("No books, fonts or dictionaries were read or modified. Safely eject before flashing.")


if __name__ == "__main__":
    main()
