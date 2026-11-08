import importlib.util
import json
from pathlib import Path
import plistlib
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('reader_state_backup', Path(__file__).resolve().parents[2] / 'scripts/reader_state_backup.py')
backup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(backup)


class StateBackupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.mount = self.root / 'card'
        self.state = self.mount / '.crosspoint'
        self.state.mkdir(parents=True)
        self.put('settings.json', b'{"fontSize": 11}')

    def put(self, path, data=b'fixture'):
        target = self.state / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        return target

    def test_preserves_complete_progress_and_unknown_state_without_asset_reads(self):
        progress = b'1234567full-optional-source-anchor'
        self.put('epub_42/progress.bin', progress)
        for name in ['epub_42/bookmarks.bin', 'epub_42/overrides.json', 'epub_42/linkstack.bin',
                     'epub_42/new-state.format', 'annotations/book.hil', 'reading-stats.bin',
                     'book-sync.json', 'wifi.json', 'recent.json']:
            self.put(name)
        for name in ['epub_42/spines/0/html_0.bin', 'epub_42/doc_css/0/css_rules.cache',
                     'epub_42/cover.img', 'epub_42/css_rules.cache', 'fonts/family.cpfont',
                     'fontprev/preview.bmp', 'web/page.gz']:
            self.put(name)
        for folder in ['Books', 'fonts', 'dictionaries']:
            (self.mount / folder).mkdir()
            (self.mount / folder / 'large-asset').write_bytes(b'do not read')
        original = Path.open
        reads = []
        def tracked(path, mode='r', *args, **kwargs):
            if mode == 'rb' and self.mount in path.parents:
                reads.append(path.relative_to(self.mount).as_posix())
            return original(path, mode, *args, **kwargs)
        with patch.object(Path, 'open', tracked):
            result = backup.snapshot(self.mount, self.root / 'snapshot')
        copied = {entry['path'] for entry in result['files']}
        self.assertEqual(set(reads), copied)
        self.assertEqual(len(reads), len(copied))  # One SD read per protected file.
        self.assertIn('.crosspoint/epub_42/new-state.format', copied)
        self.assertEqual(len(copied), 11)
        saved = self.root / 'snapshot/files/.crosspoint/epub_42/progress.bin'
        self.assertEqual(saved.read_bytes(), progress)
        self.assertEqual(saved.stat().st_mode & 0o777, 0o600)

    def test_web_backup_is_explicit(self):
        self.put('web/bundle.id', b'old stamp')
        result = backup.snapshot(self.mount, self.root / 'snapshot', include_web=True)
        self.assertIn('.crosspoint/web/bundle.id', {f['path'] for f in result['files']})

    def test_unknown_files_with_cache_like_names_outside_book_cache_are_kept(self):
        self.put('annotations/index.bin')
        self.put('other/spines/important.bin')
        result = backup.snapshot(self.mount, self.root / 'snapshot')
        self.assertEqual(len(result['files']), 3)

    def test_no_backup_on_reader_volume(self):
        with self.assertRaises(ValueError):
            backup.snapshot(self.mount, self.mount / 'backup')

    def test_existing_snapshot_is_never_overwritten(self):
        destination = self.root / 'snapshot'
        backup.snapshot(self.mount, destination)
        original = (destination / 'manifest.json').read_bytes()
        with self.assertRaises(FileExistsError):
            backup.snapshot(self.mount, destination)
        self.assertEqual((destination / 'manifest.json').read_bytes(), original)

    def test_symlink_refused_and_no_completed_manifest(self):
        (self.state / 'linked').symlink_to(self.root / 'outside')
        with self.assertRaises(ValueError):
            backup.snapshot(self.mount, self.root / 'snapshot')
        self.assertFalse((self.root / 'snapshot/manifest.json').exists())

    def test_missing_settings_refused(self):
        (self.state / 'settings.json').unlink()
        with self.assertRaises(ValueError):
            backup.snapshot(self.mount, self.root / 'snapshot')
        self.assertFalse((self.root / 'snapshot').exists())

    def test_wrong_volume_or_internal_disk_is_refused(self):
        for value in [{'VolumeUUID': 'wrong', 'Internal': False, 'BusProtocol': 'USB', 'MountPoint': '/card'},
                      {'VolumeUUID': 'expected', 'Internal': True, 'BusProtocol': 'USB', 'MountPoint': '/card'},
                      {'VolumeUUID': 'expected', 'Internal': False, 'BusProtocol': 'USB', 'MountPoint': ''}]:
            with patch.object(backup.subprocess, 'check_output', return_value=plistlib.dumps(value)):
                with self.assertRaises(ValueError):
                    backup.mounted_reader('expected')


if __name__ == '__main__':
    unittest.main()
