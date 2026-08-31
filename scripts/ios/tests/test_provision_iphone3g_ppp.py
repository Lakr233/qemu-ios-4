from __future__ import annotations

import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "provision-iphone3g-ppp.py"
SPEC = importlib.util.spec_from_file_location("provision_iphone3g_ppp", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class ProvisionIPhone3GPPPTests(unittest.TestCase):
    def setUp(self) -> None:
        self.geometry = patch.multiple(
            MODULE,
            PAGE_COUNT=4,
            IMAGE_SIZE=MODULE.PAGE_SIZE * 4,
            SCAN_PAGES=2,
        )
        self.geometry.start()
        self.addCleanup(self.geometry.stop)

    def make_image(self, *, versions: int = 1) -> tuple[Path, tempfile.TemporaryDirectory[str]]:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "nand.raw"
        image = bytearray(b"\xff" * MODULE.IMAGE_SIZE)
        for page in range(versions):
            base = page * MODULE.PAGE_SIZE
            image[base + 31:base + 31 + len(MODULE.STOCK_JOB)] = MODULE.STOCK_JOB
            spare = base + MODULE.DATA_SIZE
            image[spare:spare + 4] = (77).to_bytes(4, "little")
            image[spare + 9] = MODULE.USER_PAGE_TYPES[0]
        path.write_bytes(image)
        return path, directory

    def test_records_are_size_neutral(self) -> None:
        self.assertEqual(len(MODULE.STOCK_JOB), 530)
        self.assertEqual(len(MODULE.PPP_JOB), 530)
        self.assertIn(b"/usr/sbin/pppd", MODULE.PPP_JOB)
        self.assertIn(b"usepeerdns", MODULE.PPP_JOB)

    def test_enable_disable_is_idempotent_and_preserves_spare(self) -> None:
        path, _ = self.make_image()
        before = path.read_bytes()
        enabled = MODULE.provision(path, True)
        self.assertEqual(enabled.state, "enabled")
        once = path.read_bytes()
        self.assertEqual(MODULE.provision(path, True), enabled)
        self.assertEqual(path.read_bytes(), once)
        disabled = MODULE.provision(path, False)
        self.assertEqual(disabled.state, "disabled")
        self.assertEqual(path.read_bytes(), before)

    def test_refuses_multiple_physical_versions(self) -> None:
        path, _ = self.make_image(versions=2)
        with self.assertRaisesRegex(ValueError, "expected exactly one"):
            MODULE.provision(path, True)

    def test_refuses_nonmatching_stale_version_of_same_logical_page(self) -> None:
        path, _ = self.make_image()
        image = bytearray(path.read_bytes())
        base = MODULE.PAGE_SIZE
        image[base:base + MODULE.DATA_SIZE] = b"old".ljust(
            MODULE.DATA_SIZE, b"\0"
        )
        spare = base + MODULE.DATA_SIZE
        image[spare:spare + 4] = (77).to_bytes(4, "little")
        image[spare + 9] = MODULE.USER_PAGE_TYPES[1]
        path.write_bytes(image)
        with self.assertRaisesRegex(ValueError, "physical versions"):
            MODULE.provision(path, True)

    def test_refuses_wrong_image_size(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "nand.raw"
            path.write_bytes(b"too small")
            with self.assertRaisesRegex(ValueError, "expected"):
                MODULE.provision(path, True)


if __name__ == "__main__":
    unittest.main()
