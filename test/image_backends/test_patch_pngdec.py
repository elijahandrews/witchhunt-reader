"""Version/context/idempotence checks and production LZ-copy test extraction."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("patch_pngdec", ROOT / "scripts/patch_pngdec.py")
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)


class PatchPngdec(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / "src").mkdir()
        (self.root / "library.properties").write_text("name=PNGdec\nversion=1.1.6\n")
        self.source = self.root / "src/inflate.c"
        self.source.write_text("/* original license */\n" + patch.BEFORE + "/* unchanged suffix */\n")

    def tearDown(self):
        self.tmp.cleanup()

    def test_exact_patch_is_idempotent_and_keeps_surrounding_source(self):
        self.assertTrue(patch.apply(self.root))
        expected = "/* original license */\n" + patch.AFTER + "/* unchanged suffix */\n"
        self.assertEqual(self.source.read_text(), expected)
        self.assertFalse(patch.apply(self.root))
        self.assertEqual(self.source.read_text(), expected)

    def test_unknown_version_is_rejected_without_change(self):
        (self.root / "library.properties").write_text("version=1.1.7\n")
        original = self.source.read_bytes()
        with self.assertRaises(RuntimeError):
            patch.apply(self.root)
        self.assertEqual(self.source.read_bytes(), original)

    def test_diverged_or_duplicate_context_is_rejected_without_change(self):
        for source in [patch.BEFORE.replace("copy;", "copy + 1;", 1), patch.BEFORE * 2]:
            self.source.write_text(source)
            with self.assertRaises(RuntimeError):
                patch.apply(self.root)
            self.assertEqual(self.source.read_text(), source)


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--extract":
        text = Path(sys.argv[2]).read_text()
        start = text.index("#ifdef ALLOWS_UNALIGNED\n")
        end = text.index("#else\n", start)
        # Compile the dependency's actual copy routine, not a reimplementation.
        Path(sys.argv[3]).write_text(text[start + len("#ifdef ALLOWS_UNALIGNED\n"):end])
    else:
        unittest.main()
