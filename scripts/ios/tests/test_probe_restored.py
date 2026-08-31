from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "probe-restored.py"
sys.path.insert(0, str(SCRIPT.parent))
SPEC = importlib.util.spec_from_file_location("probe_restored", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class RestoredProbeTests(unittest.TestCase):
    def test_validates_iphone3g_identity(self) -> None:
        result = MODULE.validate_restored(
            {
                "Result": "Success",
                "Type": "com.apple.mobile.restored",
                "RestoreProtocolVersion": 12,
                "HardwareModel": "N82AP",
            },
            {
                "HardwareInfo": {
                    "BoardID": 4,
                    "ChipID": 0x8900,
                    "UniqueChipID": 0,
                    "ProductionMode": False,
                }
            },
        )
        self.assertEqual(result["HardwareModel"], "N82AP")
        self.assertEqual(result["RestoreProtocolVersion"], 12)

    def test_rejects_normal_lockdown_mode(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "not running restored"):
            MODULE.validate_restored(
                {
                    "Result": "Success",
                    "Type": "com.apple.mobile.lockdown",
                },
                {"HardwareInfo": {}},
            )

    def test_validates_protocol_11_identity_from_usb_serial(self) -> None:
        result = MODULE.validate_restored(
            {
                "Result": "Success",
                "Type": "com.apple.mobile.restored",
                "RestoreProtocolVersion": 11,
                "HardwareModel": "N82AP",
            },
            None,
            "CPID:8900 BDID:04 ECID:1 SRNM:[N82AP]",
        )
        self.assertEqual(result["ChipID"], 0x8900)
        self.assertEqual(result["BoardID"], 4)

    def test_rejects_wrong_board_identity(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "unexpected restored"):
            MODULE.validate_restored(
                {
                    "Result": "Success",
                    "Type": "com.apple.mobile.restored",
                    "HardwareModel": "N82AP",
                },
                {"HardwareInfo": {"BoardID": 6, "ChipID": 0x8900}},
            )
