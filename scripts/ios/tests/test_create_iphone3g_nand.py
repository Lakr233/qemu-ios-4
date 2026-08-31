from __future__ import annotations

import importlib.util
import stat
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "create-iphone3g-nand.py"
SPEC = importlib.util.spec_from_file_location("create_iphone3g_nand", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class CreateIPhone3GNANDTests(unittest.TestCase):
    def test_creates_complete_read_only_erased_image(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "nand.raw"
            digest = MODULE.ensure_erased_nand(
                path,
                image_size=100,
                chunk_size=17,
                free_space_margin=0,
            )

            self.assertEqual(path.read_bytes(), b"\xff" * 100)
            self.assertIsNotNone(digest)
            self.assertFalse(path.stat().st_mode & stat.S_IWUSR)
            self.assertIsNone(
                MODULE.ensure_erased_nand(
                    path,
                    image_size=100,
                    chunk_size=17,
                    free_space_margin=0,
                )
            )

    def test_rejects_existing_wrong_sized_image(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "nand.raw"
            path.write_bytes(b"\xff" * 99)
            path.chmod(0o444)

            with self.assertRaisesRegex(ValueError, "expected 100 bytes"):
                MODULE.ensure_erased_nand(
                    path,
                    image_size=100,
                    chunk_size=17,
                    free_space_margin=0,
                )

    def test_rejects_writable_baseline(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "nand.raw"
            path.write_bytes(b"\xff" * 100)

            with self.assertRaisesRegex(ValueError, "must be read-only"):
                MODULE.ensure_erased_nand(
                    path,
                    image_size=100,
                    chunk_size=17,
                    free_space_margin=0,
                )


if __name__ == "__main__":
    unittest.main()
