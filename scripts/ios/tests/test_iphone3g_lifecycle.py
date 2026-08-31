from __future__ import annotations

import argparse
import asyncio
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import AsyncMock, patch


ROOT = Path(__file__).resolve().parents[3]
IOS_SCRIPTS = ROOT / "scripts/ios"
sys.path.insert(0, str(IOS_SCRIPTS))


def load_script(module_name: str, filename: str):
    spec = importlib.util.spec_from_file_location(module_name, IOS_SCRIPTS / filename)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


SHUTDOWN = load_script("shutdown_iphone3g", "shutdown-iphone3g.py")
PYMOBILEDEVICE = load_script(
    "pymobiledevice3_usboip",
    "pymobiledevice3-usboip.py",
)
CHECKPOINT = load_script("checkpoint_iphone3g", "checkpoint-iphone3g.py")


class IPhone3GLifecycleTests(unittest.TestCase):
    def test_ios4_lockdown_requests_an_authenticated_session(self) -> None:
        paired = SimpleNamespace(session_id="session")
        create = AsyncMock(return_value=paired)

        client = asyncio.run(
            PYMOBILEDEVICE.open_ios4_lockdown_session(create, "127.0.0.1:27015")
        )

        self.assertIs(client, paired)
        create.assert_awaited_once_with(
            autopair=True,
            usbmux_address="127.0.0.1:27015",
        )

    def test_ios4_lockdown_rejects_a_missing_session(self) -> None:
        unpaired = SimpleNamespace(session_id=None, close=AsyncMock())
        create = AsyncMock(return_value=unpaired)

        with self.assertRaises(RuntimeError):
            asyncio.run(
                PYMOBILEDEVICE.open_ios4_lockdown_session(
                    create,
                    "127.0.0.1:27015",
                )
            )

        unpaired.close.assert_awaited_once_with()

    def test_legacy_pair_record_omits_absent_optional_value(self) -> None:
        self.assertEqual(
            PYMOBILEDEVICE.omit_none_mapping_values(
                {"HostID": "host", "WiFiMACAddress": None}
            ),
            {"HostID": "host"},
        )

    def test_parse_address_validates_port_bounds(self) -> None:
        self.assertEqual(
            SHUTDOWN.parse_address("127.0.0.1:27015"),
            ("127.0.0.1", 27015),
        )
        with self.assertRaises(argparse.ArgumentTypeError):
            SHUTDOWN.parse_address("127.0.0.1:0")

    def test_clean_ftl_witness_must_be_fresh(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "trace.log"
            log.write_bytes(SHUTDOWN.CLEAN_FTL_ROOT_WITNESS)
            with self.assertRaises(TimeoutError):
                SHUTDOWN.wait_for_clean_ftl_commit(
                    log,
                    log.stat().st_size,
                    0.01,
                )

    def test_clean_ftl_witness_rejects_a_prior_write_failure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "trace.log"
            log.write_bytes(
                SHUTDOWN.FTL_FAILURE_WITNESSES[1]
                + b"\n"
                + SHUTDOWN.CLEAN_FTL_ROOT_WITNESS
            )
            with self.assertRaises(RuntimeError):
                SHUTDOWN.wait_for_clean_ftl_commit(log, 0, 0.01)

    def test_clean_ftl_witness_accepts_a_fresh_root(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "trace.log"
            log.write_bytes(SHUTDOWN.CLEAN_FTL_ROOT_WITNESS)
            SHUTDOWN.wait_for_clean_ftl_commit(log, 0, 0.01)

    def test_ios4_power_action_rejects_unknown_actions(self) -> None:
        with self.assertRaises(ValueError):
            asyncio.run(PYMOBILEDEVICE.ios4_power_action("127.0.0.1:27015", "off"))

    def test_checkpoint_requires_postmigration_pause(self) -> None:
        class Client:
            def __init__(self, state: dict[str, object]) -> None:
                self.state = state

            def execute(self, command: str):
                if command == "query-migrate":
                    return {"status": "completed"}
                self.assert_command(command)
                return self.state

            @staticmethod
            def assert_command(command: str) -> None:
                if command != "query-status":
                    raise AssertionError(command)

        CHECKPOINT.wait_for_migration(
            Client({"status": "postmigrate", "running": False}), 0.1
        )
        with self.assertRaises(RuntimeError):
            CHECKPOINT.wait_for_migration(
                Client({"status": "running", "running": True}), 0.1
            )

    def test_checkpoint_resumes_source_after_publication(self) -> None:
        class Client:
            def __init__(self) -> None:
                self.running = False
                self.commands: list[str] = []

            def execute(self, command: str):
                self.commands.append(command)
                if command == "cont":
                    self.running = True
                    return {}
                if command == "query-status":
                    return {"status": "running", "running": self.running}
                raise AssertionError(command)

        client = Client()
        CHECKPOINT.resume_source(client)

        self.assertEqual(client.commands, ["query-status", "cont", "query-status"])

    def test_checkpoint_pauses_running_source_before_migration(self) -> None:
        class Client:
            def __init__(self) -> None:
                self.running = True
                self.commands: list[str] = []

            def execute(self, command: str):
                self.commands.append(command)
                if command == "stop":
                    self.running = False
                    return {}
                if command == "query-status":
                    return {"status": "running", "running": self.running}
                raise AssertionError(command)

        client = Client()

        self.assertTrue(CHECKPOINT.pause_source(client))
        self.assertEqual(client.commands, ["query-status", "stop", "query-status"])

    def test_checkpoint_preserves_already_paused_source(self) -> None:
        class Client:
            def __init__(self) -> None:
                self.commands: list[str] = []

            def execute(self, command: str):
                self.commands.append(command)
                if command == "query-status":
                    return {"status": "paused", "running": False}
                raise AssertionError(command)

        client = Client()

        self.assertFalse(CHECKPOINT.pause_source(client))
        self.assertEqual(client.commands, ["query-status"])

    def test_checkpoint_resume_waits_for_incoming_migration(self) -> None:
        class Client:
            def __init__(self, ready: bool) -> None:
                self.ready = ready
                self.running = False
                self.closed = False

            def execute(self, command: str):
                if command == "query-status":
                    return {
                        "status": "paused" if self.ready else "inmigrate",
                        "running": self.running,
                    }
                if command == "cont" and self.ready:
                    self.running = True
                    return {}
                raise RuntimeError("incoming migration is not complete")

            def close(self) -> None:
                self.closed = True

        waiting = Client(ready=False)
        ready = Client(ready=True)
        with patch.object(CHECKPOINT, "QMPClient", side_effect=(waiting, ready)):
            with patch.object(CHECKPOINT.time, "sleep"):
                CHECKPOINT.resume_vm_when_ready(Path("qmp.sock"), 1)

        self.assertTrue(waiting.closed)
        self.assertTrue(ready.closed)
        self.assertTrue(ready.running)

    def test_checkpoint_manifest_detects_mutation(self) -> None:
        with tempfile.TemporaryDirectory() as directory_name:
            directory = Path(directory_name)
            for index, name in enumerate(CHECKPOINT.CHECKPOINT_FILES):
                (directory / name).write_bytes(bytes([index]) * (index + 1))
            manifest = CHECKPOINT.checkpoint_manifest(directory)
            (directory / CHECKPOINT.MANIFEST_NAME).write_text(
                json.dumps(manifest)
            )
            CHECKPOINT.validate_checkpoint(directory)
            (directory / "nand.raw").write_bytes(b"changed")
            with self.assertRaises(RuntimeError):
                CHECKPOINT.validate_checkpoint(directory)

    def test_checkpoint_manifest_rejects_malformed_file_record(self) -> None:
        with tempfile.TemporaryDirectory() as directory_name:
            directory = Path(directory_name)
            for name in CHECKPOINT.CHECKPOINT_FILES:
                (directory / name).write_bytes(b"x")
            manifest = CHECKPOINT.checkpoint_manifest(directory)
            manifest["files"]["nand.raw"] = None
            (directory / CHECKPOINT.MANIFEST_NAME).write_text(
                json.dumps(manifest)
            )
            with self.assertRaises(RuntimeError):
                CHECKPOINT.validate_checkpoint(directory)


if __name__ == "__main__":
    unittest.main()
