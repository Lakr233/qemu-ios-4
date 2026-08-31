from __future__ import annotations

import asyncio
import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "restore-iphone3g.py"
sys.path.insert(0, str(SCRIPT.parent))
SPEC = importlib.util.spec_from_file_location("restore_iphone3g", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class RestoreIPhone3GTests(unittest.TestCase):
    class FakeGDBRemote:
        def __init__(
            self, transaction: MODULE.AuthenticatedRuntimePatch
        ) -> None:
            self.memory = {
                anchor.address: bytearray(anchor.value)
                for anchor in transaction.anchors
            }
            self.memory[0x50000000] = bytearray(range(64))
            self.pcs = iter(
                address
                for _ in range(transaction.transaction_count)
                for address in (
                    transaction.entry_address,
                    *(item.address for item in transaction.observations.values()),
                    transaction.completion_address,
                )
            )
            self.breakpoints: set[int] = set()
            self.writes: list[tuple[int, bytes]] = []
            self.detached = False
            self.closed = False
            self.current_pc: int | None = None
            self.steps = 0
            self.timeout: float | None = transaction.timeout
            self.timeout_history: list[float] = []
            self.register_writes: list[tuple[int, int]] = []
            self.register_values: dict[int, int] = {}

        def set_timeout(self, timeout: float) -> None:
            self.timeout = timeout
            self.timeout_history.append(timeout)

        def interrupt(self) -> None:
            pass

        def stop(self) -> None:
            pass

        def insert_breakpoint(self, address: int, size: int = 2) -> None:
            self.breakpoints.add(address)

        def remove_breakpoint(self, address: int, size: int = 2) -> None:
            self.breakpoints.remove(address)

        def resume(self) -> None:
            pass

        def step(self) -> bytes:
            self.steps += 1
            return b"T05thread:1;"

        def wait_for_stop(self) -> bytes:
            return b"T05thread:1;"

        def read_register(self, index: int) -> int:
            if index == 15:
                self.current_pc = next(self.pcs)
                return self.current_pc
            if index == 0:
                return self.register_values.get(0, 0)
            if index == 1:
                return 0x50000000
            if index == 2:
                return 0x22222222
            if index == 3:
                return 0x33333333
            if index == 4:
                return 0x50000000
            if index == 5:
                return self.register_values.get(5, 0x55555555)
            if index == 8:
                return 0x50000000
            if index == 10:
                return 0xAAAAAAAA
            if index == 11:
                return 0x50000000
            if index == 13:
                return 0x60000000
            if index == 14:
                return 0x12345678
            raise AssertionError(f"unexpected register {index}")

        def write_register(self, index: int, value: int, size: int = 4) -> None:
            self.register_values[index] = value
            self.register_writes.append((index, value))

        def read_memory(self, address: int, size: int) -> bytes:
            for base, data in self.memory.items():
                offset = address - base
                if 0 <= offset and offset + size <= len(data):
                    return bytes(data[offset : offset + size])
            raise AssertionError(f"unmapped fake GDB read at 0x{address:x}")

        def write_memory(self, address: int, data: bytes) -> None:
            for base, memory in self.memory.items():
                offset = address - base
                if 0 <= offset and offset + len(data) <= len(memory):
                    memory[offset : offset + len(data)] = data
                    break
            else:
                raise AssertionError(f"unmapped fake GDB write at 0x{address:x}")
            self.writes.append((address, data))

        def detach(self) -> None:
            self.detached = True

        def close(self) -> None:
            self.closed = True

    def test_formats_numeric_restore_progress_as_bar(self) -> None:
        self.assertEqual(
            MODULE.format_restore_progress(14, 50),
            "restore operation=14 [###############---------------] 50%",
        )

    def test_guest_observation_rejects_unbounded_memory(self) -> None:
        with self.assertRaisesRegex(ValueError, "1 through 4096 bytes"):
            MODULE.GuestObservation(0x1000, "bad", memory=(4, 4097))

    def test_labels_indeterminate_storage_wait(self) -> None:
        self.assertEqual(
            MODULE.format_restore_progress(28, -1),
            "restore operation=28 waiting for storage device",
        )

    def test_bounds_terminal_restore_log_to_recent_utf8_bytes(self) -> None:
        message = {
            "MsgType": "StatusMsg",
            "Status": -1,
            "Log": "old" * 2000 + "\N{SNOWMAN}" + "recent failure",
        }

        summary = MODULE.summarize_status_message(message)

        self.assertEqual(summary["Status"], -1)
        self.assertGreater(summary["LogBytes"], MODULE.STATUS_LOG_TAIL_BYTES)
        self.assertTrue(summary["LogTruncated"])
        self.assertLessEqual(
            len(summary["LogTail"].encode("utf-8")),
            MODULE.STATUS_LOG_TAIL_BYTES,
        )
        self.assertTrue(summary["LogTail"].endswith("recent failure"))
        self.assertNotEqual(summary["LogTail"], message["Log"])

    def test_preserves_short_terminal_restore_log(self) -> None:
        summary = MODULE.summarize_status_message(
            {"MsgType": "StatusMsg", "Status": 0, "Log": "complete"}
        )

        self.assertEqual(summary["LogBytes"], 8)
        self.assertEqual(summary["LogTail"], "complete")
        self.assertNotIn("LogTruncated", summary)

    def test_loads_pre_build_manifest_restore_inputs(self) -> None:
        class FakeArchive:
            members = {
                "Restore.plist": MODULE.plistlib.dumps(
                    {
                        "DeviceMap": [
                            {
                                "BoardConfig": "m68ap",
                                "CPID": 0x8900,
                                "BDID": 0,
                            },
                            {
                                "BoardConfig": "n82ap",
                                "CPID": 0x8900,
                                "BDID": 4,
                            },
                        ],
                        "RestoreKernelCaches": {
                            "Release": "kernelcache.release.s5l8900x"
                        },
                        "SystemRestoreImages": {"User": "018-3782-2.dmg"},
                    }
                ),
                "kernelcache.release.s5l8900x": b"kernel",
                "018-3782-2.dmg": b"rootfs",
            }

            def read(self, member: str) -> bytes:
                return self.members[member]

            def namelist(self) -> list[str]:
                return list(self.members)

        rootfs, kernelcache, padding, legacy = MODULE.load_restore_inputs(
            FakeArchive()
        )
        self.assertEqual(rootfs, "018-3782-2.dmg")
        self.assertEqual(kernelcache, "kernelcache.release.s5l8900x")
        self.assertEqual(padding, {"8": 80, "16": 160, "32": 320, "64": 640})
        self.assertTrue(legacy)

    def test_legacy_options_flash_ap_nor(self) -> None:
        with patch.object(MODULE.uuid, "uuid4", return_value="test-uuid"):
            options = MODULE.restore_options({"8": 80})
        self.assertTrue(options["FlashNOR"])
        self.assertNotIn("UpdateBaseband", options)
        self.assertTrue(options["CreateFilesystemPartitions"])
        self.assertEqual(options["BootImageType"], "UserOrInternal")
        self.assertEqual(options["SystemPartitionPadding"], {"8": 80})
        self.assertEqual(options["UUID"], "TEST-UUID")

    def test_loads_manifest_ordered_n82_nor_data_with_iboot_first(self) -> None:
        prefix = "Firmware/all_flash/all_flash.n82ap.production"

        class FakeArchive:
            members = {
                f"{prefix}/manifest": (
                    b"LLB.n82ap.RELEASE.img3\n"
                    b"DeviceTree.n82ap.img3\n"
                    b"iBoot.n82ap.RELEASE.img3\n"
                    b"applelogo.s5l8900x.img3\n"
                ),
                f"{prefix}/LLB.n82ap.RELEASE.img3": b"llb",
                f"{prefix}/DeviceTree.n82ap.img3": b"device-tree",
                f"{prefix}/iBoot.n82ap.RELEASE.img3": b"iboot",
                f"{prefix}/applelogo.s5l8900x.img3": b"apple-logo",
            }

            def read(self, member: str) -> bytes:
                return self.members[member]

        self.assertEqual(
            MODULE.load_nor_data(
                FakeArchive(),
                {"FirmwareDirectory": "Firmware"},
                "N82AP",
            ),
            {
                "LlbImageData": b"llb",
                "NorImageData": [b"iboot", b"device-tree", b"apple-logo"],
                "UpdateBaseband": False,
            },
        )

    def test_nor_manifest_rejects_path_traversal(self) -> None:
        prefix = "Firmware/all_flash/all_flash.n82ap.production"

        class FakeArchive:
            members = {
                f"{prefix}/manifest": (
                    b"LLB.n82ap.RELEASE.img3\n"
                    b"iBoot.n82ap.RELEASE.img3\n"
                    b"../outside.img3\n"
                ),
            }

            def read(self, member: str) -> bytes:
                return self.members[member]

        with self.assertRaisesRegex(RuntimeError, "invalid member"):
            MODULE.load_nor_data(
                FakeArchive(),
                {"FirmwareDirectory": "Firmware"},
                "N82AP",
            )

    def test_connect_restored_retries_device_and_service_readiness(self) -> None:
        self.assertEqual(MODULE.RESTORED_CONNECT_TIMEOUT, 120)
        device = SimpleNamespace(serial="N82AP")

        class FakeService:
            def __init__(self) -> None:
                self.closed = False

            async def start(self) -> None:
                pass

            async def send_recv_plist(
                self, request: dict[str, str]
            ) -> dict[str, str]:
                self.request = request
                return {"Type": "com.apple.mobile.restored", "ProtocolVersion": 11}

            async def close(self) -> None:
                self.closed = True

        class FakeServiceConnection:
            attempts = 0

            @classmethod
            async def create_using_usbmux(
                cls, *args: object, **kwargs: object
            ) -> FakeService:
                cls.attempts += 1
                if cls.attempts == 1:
                    raise MODULE.ConnectionFailedError("port not open")
                return FakeService()

        selections = iter(([], [device], [device]))

        async def select_devices(*args: object, **kwargs: object) -> list[object]:
            return next(selections)

        with (
            patch.object(MODULE, "select_devices_by_connection_type", select_devices),
            patch.object(MODULE, "ServiceConnection", FakeServiceConnection),
        ):
            selected, service, info = asyncio.run(
                MODULE.connect_restored("127.0.0.1:27015", retry_interval=0)
            )

        self.assertIs(selected, device)
        self.assertFalse(service.closed)
        self.assertEqual(service.request, {"Request": "QueryType"})
        self.assertEqual(info["ProtocolVersion"], 11)
        self.assertEqual(FakeServiceConnection.attempts, 2)

    def test_connect_restored_retries_a_hung_usbmux_connect(self) -> None:
        device = SimpleNamespace(serial="N82AP")

        class FakeService:
            closed = False

            async def start(self) -> None:
                pass

            async def send_recv_plist(
                self, request: dict[str, str]
            ) -> dict[str, str]:
                return {"Type": "com.apple.mobile.restored"}

            async def close(self) -> None:
                self.closed = True

        class FakeServiceConnection:
            attempts = 0
            first_cancelled = False

            @classmethod
            async def create_using_usbmux(
                cls, *args: object, **kwargs: object
            ) -> FakeService:
                cls.attempts += 1
                if cls.attempts == 1:
                    try:
                        await asyncio.Event().wait()
                    except asyncio.CancelledError:
                        cls.first_cancelled = True
                        raise
                return FakeService()

        async def select_devices(*args: object, **kwargs: object) -> list[object]:
            return [device]

        with (
            patch.object(MODULE, "select_devices_by_connection_type", select_devices),
            patch.object(MODULE, "ServiceConnection", FakeServiceConnection),
        ):
            selected, service, info = asyncio.run(
                MODULE.connect_restored(
                    "127.0.0.1:27015",
                    timeout=1,
                    retry_interval=0,
                    attempt_timeout=0.01,
                )
            )

        self.assertIs(selected, device)
        self.assertFalse(service.closed)
        self.assertEqual(info["Type"], "com.apple.mobile.restored")
        self.assertEqual(FakeServiceConnection.attempts, 2)
        self.assertTrue(FakeServiceConnection.first_cancelled)

    def test_existing_filesystem_partitions_can_be_reused(self) -> None:
        options = MODULE.restore_options(
            {"8": 80},
            create_filesystem_partitions=False,
            restore_system_image=False,
        )
        self.assertIs(options["CreateFilesystemPartitions"], False)
        self.assertIs(options["SystemImage"], False)

    def test_ios4_ir_mcu_can_be_disabled_in_native_firmware_data(self) -> None:
        prefix = "Firmware/all_flash/all_flash.n82ap.production"

        class FakeArchive:
            members = {
                f"{prefix}/manifest": (
                    b"LLB.n82ap.RELEASE.img3\n"
                    b"iBoot.n82ap.RELEASE.img3\n"
                ),
                f"{prefix}/LLB.n82ap.RELEASE.img3": b"llb",
                f"{prefix}/iBoot.n82ap.RELEASE.img3": b"iboot",
            }

            def read(self, member: str) -> bytes:
                return self.members[member]

        data = MODULE.load_nor_data(
            FakeArchive(),
            {"FirmwareDirectory": "Firmware"},
            "N82AP",
            disable_ir_mcu=True,
        )
        self.assertIs(data["IR MCU"], False)

    def test_nand_readiness_belongs_to_latest_driver_start(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            uart_log = Path(directory) / "uart.log"
            uart_log.write_bytes(
                MODULE.NAND_DRIVER_STARTED
                + b"\n"
                + MODULE.NAND_READY
                + b"\n"
                + MODULE.NAND_DRIVER_STARTED
                + b"\n"
            )
            with self.assertRaises(TimeoutError):
                asyncio.run(
                    asyncio.wait_for(
                        MODULE.wait_for_nand_ready(uart_log), timeout=0.02
                    )
                )

            with uart_log.open("ab") as stream:
                stream.write(MODULE.NAND_READY + b"\n")
            asyncio.run(MODULE.wait_for_nand_ready(uart_log, timeout=0.1))

    def test_legacy_nand_readiness_uses_ftl_open(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            uart_log = Path(directory) / "uart.log"
            uart_log.write_bytes(
                MODULE.LEGACY_NAND_DRIVER_STARTED
                + b"43303034\n"
                + b"[FTL:MSG] FTL_Init            [OK]\n"
            )
            with self.assertRaises(TimeoutError):
                asyncio.run(MODULE.wait_for_nand_ready(
                    uart_log,
                    MODULE.LEGACY_NAND_DRIVER_STARTED,
                    MODULE.LEGACY_NAND_READY,
                    timeout=0.01,
                ))

            with uart_log.open("ab") as stream:
                stream.write(
                    MODULE.LEGACY_NAND_READY
                    + b"            [OK]\n"
                )
            asyncio.run(
                MODULE.wait_for_nand_ready(
                    uart_log,
                    MODULE.LEGACY_NAND_DRIVER_STARTED,
                    MODULE.LEGACY_NAND_READY,
                    timeout=0.1,
                )
            )

    def test_start_restore_gate_holds_until_path_exists(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            gate = Path(directory) / "start-restore"

            async def exercise() -> None:
                waiter = asyncio.create_task(
                    MODULE.wait_for_start_restore_gate(gate)
                )
                await asyncio.sleep(0.01)
                self.assertFalse(waiter.done())
                gate.touch()
                await asyncio.wait_for(waiter, timeout=1.0)

            asyncio.run(exercise())

    def test_start_restore_without_gate_does_not_wait(self) -> None:
        asyncio.run(MODULE.wait_for_start_restore_gate(None))

    def test_system_image_stream_closes_after_guest_completion_boundary(self) -> None:
        class FakeASR:
            def __init__(self) -> None:
                self.closed = False

            async def connect(self, port: int) -> None:
                self.port = port

            async def perform_validation(self, filesystem: io.BytesIO) -> None:
                self.validated = filesystem.read()

            async def send_payload(self, filesystem: io.BytesIO) -> None:
                filesystem.seek(0)
                self.payload = filesystem.read()

            async def close(self) -> None:
                self.closed = True

        class FakeArchive:
            def open(self, member: str) -> io.BytesIO:
                self.member = member
                return io.BytesIO(b"filesystem")

        client = FakeASR()
        with patch.object(MODULE, "ASRClient", return_value=client):
            result = asyncio.run(
                MODULE.send_system_image(
                    SimpleNamespace(serial="N82AP"),
                    FakeArchive(),
                    "rootfs.dmg",
                    12345,
                )
            )

        self.assertIsNone(result)
        self.assertTrue(client.closed)
        self.assertEqual(client.port, 12345)
        self.assertEqual(client.payload, b"filesystem")

    def test_system_image_stream_closes_on_transfer_failure(self) -> None:
        class FakeASR:
            def __init__(self) -> None:
                self.closed = False

            async def connect(self, port: int) -> None:
                pass

            async def perform_validation(self, filesystem: io.BytesIO) -> None:
                pass

            async def send_payload(self, filesystem: io.BytesIO) -> None:
                raise RuntimeError("transfer failed")

            async def close(self) -> None:
                self.closed = True

        class FakeArchive:
            def open(self, member: str) -> io.BytesIO:
                return io.BytesIO(b"filesystem")

        client = FakeASR()
        with patch.object(MODULE, "ASRClient", return_value=client):
            with self.assertRaisesRegex(RuntimeError, "transfer failed"):
                asyncio.run(
                    MODULE.send_system_image(
                        SimpleNamespace(serial="N82AP"),
                        FakeArchive(),
                        "rootfs.dmg",
                        12345,
                    )
                )

        self.assertTrue(client.closed)

    def test_system_image_can_use_local_udif_override(self) -> None:
        class FakeASR:
            async def connect(self, port: int) -> None:
                pass

            async def perform_validation(
                self, filesystem: io.BufferedReader
            ) -> None:
                self.validated = filesystem.read()

            async def send_payload(self, filesystem: io.BufferedReader) -> None:
                filesystem.seek(0)
                self.payload = filesystem.read()

            async def close(self) -> None:
                pass

        class RejectArchive:
            def open(self, member: str) -> io.BytesIO:
                raise AssertionError("IPSW member must not be opened")

        client = FakeASR()
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "activated.dmg"
            image.write_bytes(b"local-udif")
            with patch.object(MODULE, "ASRClient", return_value=client):
                asyncio.run(
                    MODULE.send_system_image(
                        SimpleNamespace(serial="N82AP"),
                        RejectArchive(),
                        "rootfs.dmg",
                        12345,
                        local_image=image,
                    )
                )

        self.assertEqual(client.validated, b"local-udif")
        self.assertEqual(client.payload, b"local-udif")

    def test_ios2_asr_bypass_restores_both_authenticated_gates(self) -> None:
        bypass = MODULE.IOS2ASRChecksumBypass(
            MODULE.Endpoint.parse("127.0.0.1:1234")
        )
        remote = self.FakeGDBRemote(bypass)
        original = {
            address: bytes(data) for address, data in remote.memory.items()
        }
        with patch.object(MODULE, "GDBRemote", return_value=remote):
            bypass.arm()
            bypass.complete()

        self.assertEqual(
            {address: bytes(data) for address, data in remote.memory.items()},
            original,
        )
        self.assertEqual(
            remote.writes,
            [(item.address, item.replacement) for item in bypass.patches]
            + [
                (item.address, item.original)
                for item in reversed(bypass.patches)
            ],
        )
        self.assertFalse(remote.breakpoints)
        self.assertTrue(remote.detached)
        self.assertTrue(remote.closed)
        self.assertEqual(
            bypass.observation_results,
            [
                MODULE.GuestObservationResult(
                    observation,
                    tuple(
                        {
                            0: 0,
                            2: 0x22222222,
                            3: 0x33333333,
                            4: 0x50000000,
                            11: 0x50000000,
                        }[register]
                        for register in observation.registers
                    ),
                    (
                        bytes(range(64))
                        if observation.memory is not None
                        else None
                    ),
                    0x12345678,
                )
                for observation in bypass.observations.values()
            ],
        )

    def test_ios2_asr_bypass_rejects_an_unknown_instruction_window(self) -> None:
        bypass = MODULE.IOS2ASRChecksumBypass(
            MODULE.Endpoint.parse("127.0.0.1:1234")
        )
        remote = self.FakeGDBRemote(bypass)
        remote.memory[bypass.anchors[0].address][0] ^= 0xFF
        with patch.object(MODULE, "GDBRemote", return_value=remote):
            bypass.arm()
            with self.assertRaisesRegex(
                RuntimeError, "Guest breakpoint stopped"
            ):
                bypass.complete()

        self.assertEqual(remote.writes, [])
        self.assertFalse(remote.breakpoints)
        self.assertTrue(remote.detached)
        self.assertTrue(remote.closed)

    def test_ios4_asr_signature_bypass_is_transient(self) -> None:
        bypass = MODULE.IOS4ASRSignatureBypass(
            MODULE.Endpoint.parse("127.0.0.1:1234")
        )
        remote = self.FakeGDBRemote(bypass)
        original = {
            address: bytes(data) for address, data in remote.memory.items()
        }
        with patch.object(MODULE, "GDBRemote", return_value=remote):
            bypass.arm()
            bypass.complete()

        self.assertEqual(
            {address: bytes(data) for address, data in remote.memory.items()},
            original,
        )
        self.assertEqual(
            remote.writes,
            [(item.address, item.replacement) for item in bypass.patches]
            + [
                (item.address, item.original)
                for item in reversed(bypass.patches)
            ],
        )
        self.assertFalse(remote.breakpoints)
        self.assertTrue(remote.detached)
        self.assertTrue(remote.closed)

    def test_ios2_partition_reuse_bypass_is_transient(self) -> None:
        bypass = MODULE.IOS2PartitionReuseBypass(
            MODULE.Endpoint.parse("127.0.0.1:1234")
        )
        remote = self.FakeGDBRemote(bypass)
        original = {
            address: bytes(data) for address, data in remote.memory.items()
        }
        with patch.object(MODULE, "GDBRemote", return_value=remote):
            bypass.arm()
            bypass.complete()

        self.assertEqual(
            {address: bytes(data) for address, data in remote.memory.items()},
            original,
        )
        self.assertEqual(
            remote.writes,
            [(item.address, item.replacement) for item in bypass.patches]
            + [
                (item.address, item.original)
                for item in reversed(bypass.patches)
            ],
        )
        self.assertFalse(remote.breakpoints)
        self.assertTrue(remote.detached)
        self.assertTrue(remote.closed)

    def test_ios2_filesystem_reuse_bypass_is_transient(self) -> None:
        bypass = MODULE.IOS2FilesystemReuseBypass(
            MODULE.Endpoint.parse("127.0.0.1:1234")
        )
        remote = self.FakeGDBRemote(bypass)
        original = {
            address: bytes(data) for address, data in remote.memory.items()
        }
        with patch.object(MODULE, "GDBRemote", return_value=remote):
            bypass.arm()
            bypass.complete()

        self.assertEqual(
            {address: bytes(data) for address, data in remote.memory.items()},
            original,
        )
        self.assertEqual(
            remote.writes,
            [(item.address, item.replacement) for item in bypass.patches]
            + [
                (item.address, item.original)
                for item in reversed(bypass.patches)
            ],
        )
        self.assertFalse(remote.breakpoints)
        self.assertTrue(remote.detached)
        self.assertTrue(remote.closed)

    def test_ios2_partition_wipe_bypass_is_transient(self) -> None:
        bypass = MODULE.IOS2PartitionWipeBypass(
            MODULE.Endpoint.parse("127.0.0.1:1234")
        )
        remote = self.FakeGDBRemote(bypass)
        original = {
            address: bytes(data) for address, data in remote.memory.items()
        }
        with patch.object(MODULE, "GDBRemote", return_value=remote):
            bypass.arm()
            bypass.complete()

        self.assertEqual(
            {address: bytes(data) for address, data in remote.memory.items()},
            original,
        )
        self.assertEqual(
            remote.writes,
            [(item.address, item.replacement) for item in bypass.patches]
            + [
                (item.address, item.original)
                for item in reversed(bypass.patches)
            ],
        )
        self.assertFalse(remote.breakpoints)
        self.assertTrue(remote.detached)
        self.assertTrue(remote.closed)

    def test_runtime_patch_ignores_an_unowned_completion_mapping(self) -> None:
        bypass = MODULE.IOS2PartitionWipeBypass(
            MODULE.Endpoint.parse("127.0.0.1:1234")
        )

        class CollidingGDBRemote(self.FakeGDBRemote):
            def __init__(self) -> None:
                super().__init__(bypass)
                self.pcs = iter(
                    (
                        bypass.entry_address,
                        bypass.completion_address,
                        bypass.completion_address + 4,
                        bypass.completion_address,
                    )
                )
                self.collisions = 0

            def read_memory(self, address: int, size: int) -> bytes:
                if (
                    self.current_pc == bypass.completion_address
                    and address == bypass.completion_address
                    and not self.collisions
                ):
                    self.collisions += 1
                    return bytes(size)
                return super().read_memory(address, size)

        remote = CollidingGDBRemote()
        with patch.object(MODULE, "GDBRemote", return_value=remote):
            bypass.arm()
            bypass.complete()

        self.assertEqual(remote.collisions, 1)
        self.assertEqual(remote.steps, 1)
        self.assertFalse(remote.breakpoints)
        self.assertTrue(remote.detached)
        self.assertTrue(remote.closed)

    def test_ios2_nor_probe_authenticates_without_guest_writes(self) -> None:
        probe = MODULE.IOS2NORFlashProbe(
            MODULE.Endpoint.parse("127.0.0.1:1234")
        )
        remote = self.FakeGDBRemote(probe)
        with patch.object(MODULE, "GDBRemote", return_value=remote):
            probe.arm()
            probe.complete()

        self.assertEqual(remote.writes, [])
        self.assertFalse(remote.breakpoints)
        self.assertTrue(remote.detached)
        self.assertTrue(remote.closed)
        self.assertEqual(
            [result.observation.name for result in probe.observation_results],
            [
                "transaction entry 1",
                *(item.name for item in probe.observations.values()),
                "transaction entry 2",
                *(item.name for item in probe.observations.values()),
            ],
        )
        entries = [
            result
            for result in probe.observation_results
            if result.observation.address == probe.entry_address
        ]
        self.assertEqual(
            [item.register_values for item in entries],
            [
                (0x50000000, 0x22222222),
                (0x50000000, 0x22222222),
            ],
        )
        self.assertEqual(
            [item.memory for item in entries],
            [bytes(range(64)), bytes(range(64))],
        )

    def test_ios4_nor_probe_owns_first_manifest_transaction(self) -> None:
        probe = MODULE.IOS4NORFlashProbe(MODULE.Endpoint("127.0.0.1", 1234))
        remote = self.FakeGDBRemote(probe)
        remote.memory[0x50000000] = bytearray(256)

        with patch.object(MODULE, "GDBRemote", return_value=remote):
            probe.arm()
            probe.complete()

        entries = [
            result
            for result in probe.observation_results
            if result.observation.address == probe.entry_address
        ]
        self.assertEqual(probe.transaction_count, 1)
        self.assertEqual(len(entries), 1)
        self.assertEqual(
            [item.observation.name for item in entries],
            ["transaction entry 1"],
        )
        self.assertIsNone(entries[0].memory)
        self.assertTrue(remote.detached)
        self.assertTrue(remote.closed)

    def test_ios4_nor_provider_failure_bypass_restores_authenticated_branch(
        self,
    ) -> None:
        bypass = MODULE.IOS4NORFlashProbe(
            MODULE.Endpoint.parse("127.0.0.1:1234"),
            bypass_write_failure=True,
        )
        self.assertEqual(len(bypass.patches), 4)
        self.assertEqual(bypass.patches[0].address, 0x805A679C)
        self.assertEqual(bypass.patches[0].original, bytes.fromhex("01d1"))
        self.assertEqual(bypass.patches[0].replacement, bytes.fromhex("01e0"))
        self.assertEqual(bypass.patches[1].address, 0x805A678A)
        self.assertEqual(bypass.patches[1].original, bytes.fromhex("0cd0"))
        self.assertEqual(bypass.patches[1].replacement, bytes.fromhex("c046"))
        self.assertEqual(bypass.patches[2].address, 0x805A6A00)
        self.assertEqual(bypass.patches[2].original, bytes.fromhex("1fd1"))
        self.assertEqual(bypass.patches[2].replacement, bytes.fromhex("1fe0"))
        self.assertEqual(bypass.patches[3].address, 0x805A6A1E)
        self.assertEqual(bypass.patches[3].original, bytes.fromhex("01d0"))
        self.assertEqual(bypass.patches[3].replacement, bytes.fromhex("c046"))
        remote = self.FakeGDBRemote(bypass)
        remote.memory[0x805A679C] = bytearray(bytes.fromhex("01d1"))
        original = {
            address: bytes(data) for address, data in remote.memory.items()
        }

        with patch.object(MODULE, "GDBRemote", return_value=remote):
            bypass.arm()
            bypass.activate()
            bypass.deactivate()

        self.assertEqual(
            {address: bytes(data) for address, data in remote.memory.items()},
            original,
        )
        self.assertEqual(
            remote.writes,
            [(item.address, item.replacement) for item in bypass.patches]
            + [
                (item.address, item.original)
                for item in reversed(bypass.patches)
            ],
        )
        self.assertFalse(remote.breakpoints)
        self.assertTrue(remote.detached)
        self.assertTrue(remote.closed)

    def test_ios4_baseband_bypass_restores_authenticated_branch(self) -> None:
        bypass = MODULE.IOS4BasebandBypass(
            MODULE.Endpoint.parse("127.0.0.1:1234")
        )
        self.assertEqual(len(bypass.patches), 1)
        self.assertEqual(bypass.patches[0].address, 0xAB98)
        self.assertEqual(
            bypass.patches[0].original, bytes.fromhex("000050e30d00000a")
        )
        self.assertEqual(
            bypass.patches[0].replacement, bytes.fromhex("0000a0e30d0000ea")
        )
        remote = self.FakeGDBRemote(bypass)
        original = {
            address: bytes(data) for address, data in remote.memory.items()
        }

        with patch.object(MODULE, "GDBRemote", return_value=remote):
            bypass.arm()
            bypass.activate()
            bypass.deactivate()

        self.assertEqual(
            {address: bytes(data) for address, data in remote.memory.items()},
            original,
        )
        self.assertEqual(
            remote.writes,
            [(0xAB98, bytes.fromhex("0000a0e30d0000ea")),
             (0xAB98, bytes.fromhex("000050e30d00000a"))],
        )
        self.assertFalse(remote.breakpoints)
        self.assertTrue(remote.detached)
        self.assertTrue(remote.closed)

    def test_ios4_ir_mcu_bypass_restores_authenticated_result(self) -> None:
        bypass = MODULE.IOS4IRMCUBypass(
            MODULE.Endpoint.parse("127.0.0.1:1234")
        )
        self.assertEqual(bypass.entry_address, 0xECB8)
        self.assertEqual(bypass.completion_address, 0xECFC)
        self.assertEqual(len(bypass.patches), 1)
        self.assertEqual(bypass.patches[0].address, 0xECFC)
        self.assertEqual(bypass.patches[0].original, bytes.fromhex("0050a0e3"))
        self.assertEqual(bypass.patches[0].replacement, bytes.fromhex("0150a0e3"))
        remote = self.FakeGDBRemote(bypass)
        original = {
            address: bytes(data) for address, data in remote.memory.items()
        }

        with patch.object(MODULE, "GDBRemote", return_value=remote):
            bypass.arm()
            bypass.complete()

        self.assertEqual(
            {address: bytes(data) for address, data in remote.memory.items()},
            original,
        )
        self.assertEqual(remote.writes, [])
        self.assertEqual(remote.register_writes, [(0, 0)])
        self.assertFalse(remote.breakpoints)
        self.assertTrue(remote.detached)
        self.assertTrue(remote.closed)

    def test_ios4_batch_probe_restores_provider_branches(self) -> None:
        probe = MODULE.IOS4NORFlashProbe(
            MODULE.Endpoint.parse("127.0.0.1:1234"),
            bypass_write_failure=True,
            transaction_count=11,
        )
        remote = self.FakeGDBRemote(probe)
        self.assertNotIn(0x805A6E00, {item.address for item in probe.patches})
        remote.memory[0x805A679C] = bytearray(bytes.fromhex("01d1"))
        remote.memory[0x50000000] = bytearray(256)
        remote.pcs = iter(
            tuple(
                address
                for _ in range(11)
                for address in (
                    probe.entry_address,
                    0x805A67C0,
                    probe.completion_address,
                )
            )
        )

        with patch.object(MODULE, "GDBRemote", return_value=remote):
            probe.arm()
            probe.probe_direct_batch()

        self.assertEqual(
            remote.writes,
            [(item.address, item.replacement) for item in probe.patches]
            + [
                (item.address, item.original)
                for item in reversed(probe.patches)
            ],
        )
        self.assertEqual(
            [result.observation.name for result in probe.observation_results],
            [
                name
                for transaction in range(1, 12)
                for name in (
                    f"transaction entry {transaction}",
                    f"transaction result {transaction}",
                )
            ],
        )
        self.assertFalse(remote.breakpoints)
        self.assertTrue(remote.detached)
        self.assertTrue(remote.closed)

    def test_ios4_catalog_workaround_initializes_only_call_local(self) -> None:
        probe = MODULE.IOS4NORFlashProbe(
            MODULE.Endpoint.parse("127.0.0.1:1234"),
            timeout=120,
            workaround_catalog_data_length=True,
            transaction_count=2,
        )
        remote = self.FakeGDBRemote(probe)
        stack = bytearray(24)
        stack[0x10:0x14] = (1).to_bytes(4, "little")
        remote.memory[0x60000000] = stack
        remote.pcs = iter(
            (
                probe.catalog_entry.address,
                probe.catalog_data_length_workaround_address,
                probe.catalog_completion_address,
                probe.entry_address,
                probe.completion_address,
            )
        )

        with patch.object(MODULE, "GDBRemote", return_value=remote):
            probe.arm()
            probe.probe_direct_batch()

        self.assertEqual(remote.writes, [(0x60000010, bytes(4))])
        self.assertEqual(remote.memory[0x60000000][0x10:0x14], bytes(4))
        result = next(
            item
            for item in probe.observation_results
            if item.observation.address == 0x805A692E
        )
        self.assertEqual(result.memory[0x10:0x14], (1).to_bytes(4, "little"))
        self.assertFalse(probe.patches)
        self.assertFalse(remote.breakpoints)
        self.assertTrue(remote.detached)
        self.assertTrue(remote.closed)

    def test_ios4_batch_probe_follows_catalog_path_switch(self) -> None:
        probe = MODULE.IOS4NORFlashProbe(
            MODULE.Endpoint.parse("127.0.0.1:1234"),
            bypass_write_failure=True,
            transaction_count=11,
        )
        remote = self.FakeGDBRemote(probe)
        remote.memory[0x805A679C] = bytearray(bytes.fromhex("01d1"))
        remote.memory[0x50000000] = bytearray(256)
        remote.pcs = iter(
            tuple(
                address
                for transaction in range(11)
                for address in (
                    (
                        probe.entry_address
                        if transaction < 9
                        else probe.catalog_entry.address
                    ),
                    *((0x805A67C0, probe.completion_address)
                      if transaction < 9
                      else (probe.catalog_completion_address,)),
                )
            )
        )

        with patch.object(MODULE, "GDBRemote", return_value=remote):
            probe.arm()
            probe.probe_direct_batch()

        names = [item.observation.name for item in probe.observation_results]
        self.assertIn("catalog transaction entry 10", names)
        self.assertIn("catalog transaction result 10", names)
        self.assertIn("catalog transaction entry 11", names)
        self.assertIn("catalog transaction result 11", names)
        self.assertFalse(remote.breakpoints)
        self.assertTrue(remote.detached)
        self.assertTrue(remote.closed)

    def test_ios4_batch_multiplexes_ir_mcu_and_baseband(self) -> None:
        probe = MODULE.IOS4NORFlashProbe(
            MODULE.Endpoint.parse("127.0.0.1:1234"),
            bypass_write_failure=True,
            transaction_count=2,
        )
        successor = MODULE.IOS4BasebandBypass(
            MODULE.Endpoint.parse("127.0.0.1:1234")
        )
        ir_mcu = MODULE.IOS4IRMCUBypass(
            MODULE.Endpoint.parse("127.0.0.1:1234")
        )
        remote = self.FakeGDBRemote(probe)
        remote.memory[0x805A679C] = bytearray(bytes.fromhex("01d1"))
        for owner in (ir_mcu, successor):
            for anchor in owner.anchors:
                remote.memory[anchor.address] = bytearray(anchor.value)
        remote.pcs = iter(
            (probe.entry_address,
             successor.entry_address,
             0x805A67C0, probe.completion_address,
             probe.entry_address,
             ir_mcu.entry_address,
             0x805A67C0, probe.completion_address,
             )
        )

        with patch.object(MODULE, "GDBRemote", return_value=remote):
            probe.arm()
            probe.probe_direct_batch(successor, ir_mcu)
            self.assertTrue(probe.batch_successor_active)
            self.assertTrue(probe.batch_ir_mcu_complete)
            self.assertEqual(remote.breakpoints, set())
            self.assertEqual(remote.timeout, successor.timeout)
            self.assertEqual(remote.timeout_history, [10, successor.timeout])
            self.assertFalse(remote.detached)
            successor.deactivate()

        self.assertFalse(remote.breakpoints)
        self.assertTrue(remote.detached)
        self.assertTrue(remote.closed)

    def test_authenticated_entry_breakpoint_steps_unowned_process(self) -> None:
        bypass = MODULE.IOS4BasebandBypass(
            MODULE.Endpoint.parse("127.0.0.1:1234")
        )

        class CollidingRemote(self.FakeGDBRemote):
            collision = True

            def read_memory(self, address: int, size: int) -> bytes:
                if self.collision and address == bypass.anchors[0].address:
                    self.collision = False
                    raise MODULE.GDBRemoteError("unmapped colliding process")
                return super().read_memory(address, size)

        remote = CollidingRemote(bypass)
        remote.pcs = iter(
            (bypass.entry_address, bypass.entry_address + 2,
             bypass.entry_address)
        )

        with patch.object(MODULE, "GDBRemote", return_value=remote):
            bypass.arm()
            bypass.activate()
            bypass.deactivate()

        self.assertEqual(remote.steps, 1)
        self.assertFalse(remote.breakpoints)
        self.assertTrue(remote.detached)
        self.assertTrue(remote.closed)

    def test_authenticated_patch_handoff_restores_then_arms_successor(
        self,
    ) -> None:
        nor = MODULE.IOS4NORFlashProbe(
            MODULE.Endpoint.parse("127.0.0.1:1234"),
            bypass_write_failure=True,
        )
        baseband = MODULE.IOS4BasebandBypass(
            MODULE.Endpoint.parse("127.0.0.1:1234")
        )
        remote = self.FakeGDBRemote(nor)
        remote.memory[0x805A679C] = bytearray(bytes.fromhex("01d1"))
        for anchor in baseband.anchors:
            remote.memory[anchor.address] = bytearray(anchor.value)

        with patch.object(MODULE, "GDBRemote", return_value=remote):
            nor.arm()
            nor.activate()
            nor.handoff(baseband)

            self.assertIsNone(nor.remote)
            self.assertIs(baseband.remote, remote)
            self.assertTrue(baseband.running)
            self.assertEqual(remote.breakpoints, {baseband.entry_address})
            self.assertFalse(remote.detached)
            self.assertFalse(remote.closed)

            remote.pcs = iter((baseband.entry_address,))
            baseband.activate()
            baseband.deactivate()

        self.assertEqual(
            remote.writes,
            [(item.address, item.replacement) for item in nor.patches]
            + [(item.address, item.original) for item in reversed(nor.patches)]
            + [(item.address, item.replacement) for item in baseband.patches]
            + [
                (item.address, item.original)
                for item in reversed(baseband.patches)
            ],
        )
        self.assertFalse(remote.breakpoints)
        self.assertTrue(remote.detached)
        self.assertTrue(remote.closed)

    def test_ios2_nor_integrity_bypass_restores_authenticated_branches(
        self,
    ) -> None:
        bypass = MODULE.IOS2NORFlashProbe(
            MODULE.Endpoint.parse("127.0.0.1:1234"),
            bypass_integrity=True,
        )
        remote = self.FakeGDBRemote(bypass)
        original = {
            address: bytes(data) for address, data in remote.memory.items()
        }
        with patch.object(MODULE, "GDBRemote", return_value=remote):
            bypass.arm()
            bypass.activate()
            self.assertEqual(
                remote.writes,
                [(item.address, item.replacement) for item in bypass.patches],
            )
            bypass.deactivate()

        self.assertEqual(
            {address: bytes(data) for address, data in remote.memory.items()},
            original,
        )
        self.assertEqual(
            remote.writes,
            [(item.address, item.replacement) for item in bypass.patches]
            + [
                (item.address, item.original)
                for item in reversed(bypass.patches)
            ],
        )
        self.assertFalse(remote.breakpoints)
        self.assertTrue(remote.detached)
        self.assertTrue(remote.closed)
