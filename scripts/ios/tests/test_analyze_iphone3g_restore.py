from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "analyze-iphone3g-restore.py"
SPEC = importlib.util.spec_from_file_location(
    "analyze_iphone3g_restore", SCRIPT
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class AnalyzeIPhone3GRestoreTests(unittest.TestCase):
    GEOMETRY = MODULE.NANDGeometry(
        data_size=8,
        spare_size=12,
        pages_per_block=2,
        blocks_per_bank=2,
        banks=1,
    )

    @staticmethod
    def nand_page(data: bytes, lpn: int, type1: int = 0x40) -> bytes:
        spare = bytearray(b"\xff" * 12)
        spare[0:4] = lpn.to_bytes(4, "little")
        spare[9] = type1
        return data + spare

    def analyze(self, nand: bytes, source: bytes, **kwargs: object):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            nand_path = root / "nand.raw"
            source_path = root / "source.raw"
            nand_path.write_bytes(nand)
            source_path.write_bytes(source)
            return MODULE.analyze_restore(
                nand_path,
                source_path,
                geometry=self.GEOMETRY,
                scan_pages=2,
                **kwargs,
            )

    def test_reports_missing_wrong_and_ignored_pages(self) -> None:
        source = b"source00source01source02"
        nand = b"".join(
            (
                self.nand_page(b"source00", 63),
                self.nand_page(b"wrong001", 64),
                self.nand_page(b"outside0", 100),
                self.nand_page(b"ignored0", 65, type1=0x00),
            )
        )

        result = self.analyze(nand, source)

        self.assertFalse(result.complete)
        self.assertEqual(result.source_pages, 3)
        self.assertEqual(result.physical_user_pages, 3)
        self.assertEqual(result.seen_source_pages, 2)
        self.assertEqual(result.exact_source_pages, 1)
        self.assertEqual(result.missing_source_ranges, ((2, 2),))
        self.assertEqual(result.wrong_source_ranges, ((1, 1),))

    def test_accepts_multiple_versions_when_one_is_exact(self) -> None:
        source = b"source00source01source02"
        nand = b"".join(
            (
                self.nand_page(b"source00", 63),
                self.nand_page(b"stale001", 64),
                self.nand_page(b"source01", 64, type1=0x41),
                self.nand_page(b"source02", 65),
            )
        )

        result = self.analyze(nand, source)

        self.assertTrue(result.complete)
        self.assertEqual(result.exact_source_pages, 3)
        self.assertEqual(result.multiple_version_pages, 1)
        self.assertEqual(result.maximum_versions, 2)
        self.assertEqual(result.missing_source_ranges, ())
        self.assertEqual(result.wrong_source_ranges, ())

    def test_limits_comparison_to_complete_transferred_pages(self) -> None:
        source = b"source00source01tail"
        nand = b"".join(
            (
                self.nand_page(b"source00", 63),
                self.nand_page(b"source01", 64),
                self.nand_page(b"outside0", 65),
                self.nand_page(b"outside1", 66),
            )
        )

        result = self.analyze(nand, source, source_bytes=18)

        self.assertTrue(result.complete)
        self.assertEqual(result.source_pages, 2)
        self.assertEqual(result.ignored_source_tail_bytes, 2)

    def test_compares_partition_at_unaligned_source_offset(self) -> None:
        source = b"container" + b"source00source01" + b"tail"
        nand = b"".join(
            (
                self.nand_page(b"source00", 63),
                self.nand_page(b"source01", 64),
                self.nand_page(b"outside0", 65),
                self.nand_page(b"outside1", 66),
            )
        )

        result = self.analyze(
            nand,
            source,
            source_offset=9,
            source_bytes=16,
        )

        self.assertTrue(result.complete)
        self.assertEqual(result.source_offset, 9)

    def test_accepts_negative_source_page_bias(self) -> None:
        source = b"lead0000source00source01"
        nand = b"".join(
            (
                self.nand_page(b"source00", 0),
                self.nand_page(b"source01", 1),
                self.nand_page(b"outside0", 100),
                self.nand_page(b"ignored0", 2, type1=0x00),
            )
        )

        result = self.analyze(nand, source, lpn_base=-1)

        self.assertFalse(result.complete)
        self.assertEqual(result.exact_source_pages, 2)
        self.assertEqual(result.missing_source_ranges, ((0, 0),))

    def test_rejects_source_range_past_end(self) -> None:
        nand = b"".join(
            self.nand_page(b"outside0", 100 + index)
            for index in range(4)
        )

        with self.assertRaisesRegex(ValueError, "fit after source_offset"):
            self.analyze(
                nand,
                b"container-source",
                source_offset=10,
                source_bytes=20,
            )

    def test_rejects_incorrect_nand_size(self) -> None:
        with self.assertRaisesRegex(ValueError, "expected 80 bytes"):
            self.analyze(b"short", b"source00")

    def test_formats_ranges(self) -> None:
        self.assertEqual(MODULE.format_ranges(()), "none")
        self.assertEqual(
            MODULE.format_ranges(((1, 1), (3, 5))), "1,3..5"
        )


if __name__ == "__main__":
    unittest.main()
