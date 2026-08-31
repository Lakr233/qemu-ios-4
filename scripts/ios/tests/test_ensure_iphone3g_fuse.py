from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts/ios/ensure-iphone3g-fuse.py"
SPEC = importlib.util.spec_from_file_location("ensure_iphone3g_fuse", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
fuse = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(fuse)


class EnsureIPhone3GFuseTests(unittest.TestCase):
    def test_creates_once_and_preserves_the_virtual_identity(self) -> None:
        expected = bytes(range(fuse.FUSE_KEY_SIZE))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "gid-key.bin"
            self.assertTrue(
                fuse.ensure_fuse_key(path, "gid", lambda size: expected)
            )
            self.assertEqual(path.read_bytes(), expected)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertFalse(
                fuse.ensure_fuse_key(path, "gid", lambda size: b"x" * size)
            )
            self.assertEqual(path.read_bytes(), expected)

    def test_rejects_an_invalid_existing_identity(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "uid-key.bin"
            path.write_bytes(b"short")
            with self.assertRaisesRegex(
                ValueError, "uid key must contain exactly 16 bytes"
            ):
                fuse.ensure_fuse_key(path, "uid")

    def test_removes_a_failed_first_creation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "gid-key.bin"
            with self.assertRaisesRegex(
                ValueError, "gid key generator returned an invalid length"
            ):
                fuse.ensure_fuse_key(path, "gid", lambda size: b"")
            self.assertFalse(path.exists())


if __name__ == "__main__":
    unittest.main()
