from __future__ import annotations

import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "boot-iphone3g.py"
sys.path.insert(0, str(SCRIPT.parent))
SPEC = importlib.util.spec_from_file_location("boot_iphone3g", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class BootIPhone3GTests(unittest.TestCase):
    @staticmethod
    def device_tree_property(name: str, value: bytes) -> bytes:
        raw_name = name.encode("ascii").ljust(32, b"\0")
        padding = b"\0" * (-len(value) & 3)
        return raw_name + struct.pack("<I", len(value)) + value + padding

    @classmethod
    def device_tree_node(
        cls,
        name: str,
        *,
        compatible: bytes | None = None,
        children: tuple[bytes, ...] = (),
    ) -> bytes:
        properties = [cls.device_tree_property("name", name.encode() + b"\0")]
        if compatible is not None:
            properties.append(cls.device_tree_property("compatible", compatible))
        return (
            struct.pack("<II", len(properties), len(children))
            + b"".join(properties)
            + b"".join(children)
        )

    class FakeGDBRemote:
        state = (MODULE.IOS2_DEVICE_TREE_DESTINATION, 0x9B5C)

        def __init__(self, host: str, port: int) -> None:
            self.host = host
            self.port = port
            self.detached = False

        def __enter__(self):
            return self

        def __exit__(self, *args: object) -> None:
            pass

        def stop(self) -> None:
            pass

        def read_memory(self, address: int, size: int) -> bytes:
            for anchor_address, value in MODULE.IOS2_DEVICE_TREE_STATE_ANCHORS:
                if address == anchor_address:
                    return value
            if address == MODULE.IOS2_DEVICE_TREE_STATE_ADDRESS:
                return struct.pack("<II", *self.state)
            raise AssertionError(f"unexpected read at 0x{address:x}")

        def detach(self) -> None:
            self.detached = True

    def setUp(self) -> None:
        self.endpoint = MODULE.Endpoint("127.0.0.1", 1234)

    def test_reads_authenticated_device_tree_state(self) -> None:
        with patch.object(MODULE, "GDBRemote", self.FakeGDBRemote):
            self.assertEqual(
                MODULE.read_ios2_device_tree_state(self.endpoint),
                (MODULE.IOS2_DEVICE_TREE_DESTINATION, 0x9B5C),
            )

    def test_reset_bus_uses_a_disposable_transport(self) -> None:
        events: list[object] = []

        class FakeDevice:
            @classmethod
            def connect(
                cls,
                endpoint: object,
                request_timeout_ms: int | None = None,
            ):
                events.append(("connect", endpoint, request_timeout_ms))
                return cls()

            def reset(self) -> None:
                events.append("reset")

            def close(self) -> None:
                events.append("close")

        with patch.object(MODULE, "Device", FakeDevice):
            MODULE.reset_bus(self.endpoint)

        self.assertEqual(
            events,
            [("connect", self.endpoint, None), "reset", "close"],
        )

    def test_reconnect_resets_bus_before_irecv_discovery(self) -> None:
        events: list[object] = []
        adapter = object()
        old_client = object()
        new_client = object()

        with (
            patch.object(
                MODULE,
                "close",
                side_effect=lambda actual_adapter, client: events.append(
                    ("close", actual_adapter, client)
                ),
            ),
            patch.object(
                MODULE.time,
                "sleep",
                side_effect=lambda delay: events.append(("sleep", delay)),
            ),
            patch.object(
                MODULE,
                "reset_bus",
                side_effect=lambda endpoint, **kwargs: events.append(
                    ("reset", endpoint, kwargs)
                ),
            ),
            patch.object(
                MODULE,
                "connect",
                side_effect=lambda timeout: events.append(("connect", timeout))
                or new_client,
            ),
        ):
            result = MODULE.reconnect(
                self.endpoint,
                adapter,
                old_client,
                30,
            )

        self.assertIs(result, new_client)
        self.assertEqual(
            events,
            [
                ("close", adapter, old_client),
                ("sleep", 0.5),
                ("reset", self.endpoint, {"discovery_timeout": 30}),
                ("connect", 30),
            ],
        )

    def test_reads_img3_data_logical_size(self) -> None:
        data = b"abc" + b"\0" * 13
        full_size = 20 + 12 + len(data)
        image = (
            struct.pack(
                "<4sIII4s",
                b"3gmI",
                full_size,
                full_size,
                full_size,
                b"ertd",
            )
            + struct.pack("<4sII", b"ATAD", 12 + len(data), 3)
            + data
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "DeviceTree.img3"
            path.write_bytes(image)
            self.assertEqual(MODULE.img3_data_logical_size(path), 3)

    def test_retries_command_until_device_tree_state_is_published(self) -> None:
        clients = [object(), object(), object()]
        with (
            patch.object(
                MODULE,
                "send_ios2_stage_command",
                side_effect=clients[1:],
            ) as send,
            patch.object(
                MODULE,
                "wait_for_ios2_device_tree_state",
                side_effect=[False, True],
            ) as wait,
        ):
            result = MODULE.register_ios2_device_tree(
                self.endpoint,
                MODULE.Endpoint("127.0.0.1", 4321),
                object(),
                clients[0],
                0x9B5C,
                30,
            )
        self.assertIs(result, clients[2])
        self.assertEqual(send.call_count, 2)
        self.assertEqual(wait.call_count, 2)
        self.assertEqual(send.call_args_list[0].args[0], self.endpoint)
        self.assertEqual(wait.call_args_list[0].args[0].port, 4321)

    def test_rejects_unexpected_published_state(self) -> None:
        self.FakeGDBRemote.state = (0x0BE00000, 0x9B5C)
        try:
            with patch.object(MODULE, "GDBRemote", self.FakeGDBRemote):
                with self.assertRaisesRegex(RuntimeError, "unexpected DeviceTree"):
                    MODULE.wait_for_ios2_device_tree_state(
                        self.endpoint, 0x9B5C, timeout=0
                    )
        finally:
            self.FakeGDBRemote.state = (
                MODULE.IOS2_DEVICE_TREE_DESTINATION,
                0x9B5C,
            )

    def test_unmatches_only_authenticated_ios4_baseband_nodes(self) -> None:
        spi2 = self.device_tree_node(
            "spi2", compatible=b"spi,s5l8900x,baseband\0"
        )
        arm_io = self.device_tree_node("arm-io", children=(spi2,))
        baseband = self.device_tree_node(
            "baseband", compatible=b"baseband,n82\0"
        )
        tree = self.device_tree_node(
            "device-tree", children=(arm_io, baseband)
        )
        instances: list[object] = []

        class FakeRemote:
            def __init__(self, host: str, port: int) -> None:
                self.memory = bytearray(tree)
                self.writes: list[tuple[int, bytes]] = []
                self.detached = False
                instances.append(self)

            def __enter__(self):
                return self

            def __exit__(self, *args: object) -> None:
                pass

            def stop(self) -> None:
                pass

            def read_memory(self, address: int, size: int) -> bytes:
                offset = address - MODULE.IOS4_DEVICE_TREE_DESTINATION
                return bytes(self.memory[offset : offset + size])

            def write_memory(self, address: int, data: bytes) -> None:
                offset = address - MODULE.IOS4_DEVICE_TREE_DESTINATION
                self.memory[offset : offset + len(data)] = data
                self.writes.append((address, data))

            def detach(self) -> None:
                self.detached = True

        with patch.object(MODULE, "GDBRemote", FakeRemote):
            digest = MODULE.unmatch_ios4_unmodeled_baseband(
                self.endpoint, len(tree)
            )

        remote = instances[0]
        self.assertEqual(len(digest), 64)
        self.assertEqual(len(remote.writes), 2)
        self.assertTrue(remote.detached)
        for path, _ in MODULE.IOS4_UNMODELED_BASEBAND_NODES:
            node = MODULE._device_tree_path(remote.memory, path)
            compatible = MODULE._device_tree_property(
                remote.memory, node, "compatible"
            )
            self.assertIsNotNone(compatible)
            assert compatible is not None
            self.assertEqual(compatible[1][:1], b"x")

    def test_baseband_unmatch_rejects_wrong_compatible_without_writes(self) -> None:
        spi2 = self.device_tree_node(
            "spi2", compatible=b"spi,s5l8900x,baseband\0"
        )
        arm_io = self.device_tree_node("arm-io", children=(spi2,))
        baseband = self.device_tree_node(
            "baseband", compatible=b"baseband,n81\0"
        )
        tree = self.device_tree_node(
            "device-tree", children=(arm_io, baseband)
        )
        instances: list[object] = []

        class WrongRemote:
            def __init__(self, host: str, port: int) -> None:
                self.memory = bytearray(tree)
                self.writes: list[tuple[int, bytes]] = []
                self.detached = False
                instances.append(self)

            def __enter__(self):
                return self

            def __exit__(self, *args: object) -> None:
                pass

            def stop(self) -> None:
                pass

            def read_memory(self, address: int, size: int) -> bytes:
                offset = address - MODULE.IOS4_DEVICE_TREE_DESTINATION
                return bytes(self.memory[offset : offset + size])

            def write_memory(self, address: int, data: bytes) -> None:
                self.writes.append((address, data))

            def detach(self) -> None:
                self.detached = True

        with patch.object(MODULE, "GDBRemote", WrongRemote):
            with self.assertRaisesRegex(ValueError, "compatible mismatch"):
                MODULE.unmatch_ios4_unmodeled_baseband(
                    self.endpoint, len(tree)
                )

        self.assertEqual(instances[0].writes, [])
        self.assertTrue(instances[0].detached)

    def test_ios4_restore_crypto_owner_restores_instruction(self) -> None:
        events: list[object] = []
        instances: list[object] = []

        class FakeRemote:
            def __init__(self, host: str, port: int) -> None:
                self.base = MODULE.IOS4_CRYPTO_COUNT_ANCHOR_ADDRESS
                self.memory = bytearray(MODULE.IOS4_CRYPTO_COUNT_ANCHOR)
                self.stops = iter(
                    (
                        MODULE.IOS4_CRYPTO_COUNT_PATCH_ADDRESS,
                        MODULE.IOS4_CRYPTO_COUNT_COMPLETION_ADDRESS,
                    )
                )
                self.pc = 0
                instances.append(self)

            def __enter__(self):
                return self

            def __exit__(self, *args: object) -> None:
                events.append("close")

            def stop(self) -> None:
                events.append("stop")

            def set_timeout(self, timeout: float) -> None:
                events.append(("timeout", timeout))

            def insert_breakpoint(self, address: int, size: int = 2) -> None:
                events.append(("insert", address, size))

            def remove_breakpoint(self, address: int, size: int = 2) -> None:
                events.append(("remove", address, size))

            def resume(self) -> None:
                events.append("resume")

            def wait_for_stop(self) -> bytes:
                self.pc = next(self.stops)
                events.append(("wait", self.pc))
                return b"T05"

            def read_register(self, index: int) -> int:
                self.assert_register(index)
                return self.pc

            @staticmethod
            def assert_register(index: int) -> None:
                if index != 15:
                    raise AssertionError(f"unexpected register {index}")

            def read_memory(self, address: int, size: int) -> bytes:
                offset = address - self.base
                if offset < 0 or offset + size > len(self.memory):
                    raise AssertionError(f"unexpected read at 0x{address:x}")
                return bytes(self.memory[offset : offset + size])

            def write_memory(self, address: int, data: bytes) -> None:
                offset = address - self.base
                self.memory[offset : offset + len(data)] = data
                events.append(("write", address, data))

            def interrupt(self) -> None:
                events.append("interrupt")

            def detach(self) -> None:
                events.append("detach")

        class FakeClient:
            def send_command(self, command: str) -> None:
                events.append(("command", command))

        with patch.object(MODULE, "GDBRemote", FakeRemote):
            MODULE.boot_ios4_with_restore_crypto_keys(
                self.endpoint, FakeClient(), 45
            )

        remote = instances[0]
        self.assertEqual(
            remote.read_memory(
                MODULE.IOS4_CRYPTO_COUNT_PATCH_ADDRESS,
                len(MODULE.IOS4_CRYPTO_COUNT_ORIGINAL),
            ),
            MODULE.IOS4_CRYPTO_COUNT_ORIGINAL,
        )
        self.assertLess(events.index("resume"), events.index(("command", "bootx")))
        self.assertEqual(events.count("resume"), 2)
        self.assertIn(
            (
                "write",
                MODULE.IOS4_CRYPTO_COUNT_PATCH_ADDRESS,
                MODULE.IOS4_CRYPTO_COUNT_REPLACEMENT,
            ),
            events,
        )
        self.assertEqual(events[-2:], ["detach", "close"])

    def test_ios4_restore_crypto_owner_rejects_wrong_kernel(self) -> None:
        events: list[object] = []

        class WrongKernelRemote:
            def __init__(self, host: str, port: int) -> None:
                self.pc = MODULE.IOS4_CRYPTO_COUNT_PATCH_ADDRESS

            def __enter__(self):
                return self

            def __exit__(self, *args: object) -> None:
                pass

            def stop(self) -> None:
                pass

            def insert_breakpoint(self, address: int, size: int = 2) -> None:
                events.append(("insert", address, size))

            def remove_breakpoint(self, address: int, size: int = 2) -> None:
                events.append(("remove", address, size))

            def set_timeout(self, timeout: float) -> None:
                pass

            def resume(self) -> None:
                pass

            def wait_for_stop(self) -> bytes:
                return b"T05"

            def read_register(self, index: int) -> int:
                return self.pc

            def read_memory(self, address: int, size: int) -> bytes:
                return b"\0" * size

            def interrupt(self) -> None:
                events.append("interrupt")

            def detach(self) -> None:
                events.append("detach")

        class FakeClient:
            def send_command(self, command: str) -> None:
                pass

        with patch.object(MODULE, "GDBRemote", WrongKernelRemote):
            with self.assertRaisesRegex(RuntimeError, "anchor mismatch"):
                MODULE.boot_ios4_with_restore_crypto_keys(
                    self.endpoint, FakeClient(), 45
                )

        self.assertIn(
            ("remove", MODULE.IOS4_CRYPTO_COUNT_PATCH_ADDRESS, 4), events
        )
        self.assertEqual(events[-1], "detach")

    def test_ios4_hacktivation_patches_authenticated_lockdownd(self) -> None:
        events: list[object] = []
        instances: list[object] = []

        class FakeRemote:
            def __init__(self, host: str, port: int) -> None:
                self.pc = MODULE.IOS4_LOCKDOWND_PATCH_ADDRESS
                self.instruction = bytearray(MODULE.IOS4_LOCKDOWND_ORIGINAL)
                instances.append(self)

            def __enter__(self):
                return self

            def __exit__(self, *args: object) -> None:
                events.append("close")

            def stop(self) -> None:
                events.append("stop")

            def set_timeout(self, timeout: float) -> None:
                events.append(("timeout", timeout))

            def insert_breakpoint(self, address: int, size: int = 2) -> None:
                events.append(("insert", address, size))

            def remove_breakpoint(self, address: int, size: int = 2) -> None:
                events.append(("remove", address, size))

            def resume(self) -> None:
                events.append("resume")

            def wait_for_stop(self) -> bytes:
                events.append(("wait", self.pc))
                return b"T05"

            def read_register(self, index: int) -> int:
                if index != 15:
                    raise AssertionError(f"unexpected register {index}")
                return self.pc

            def read_memory(self, address: int, size: int) -> bytes:
                if address == MODULE.IOS4_LOCKDOWND_PREFIX_ADDRESS:
                    return MODULE.IOS4_LOCKDOWND_PREFIX
                if address == MODULE.IOS4_LOCKDOWND_SUFFIX_ADDRESS:
                    return MODULE.IOS4_LOCKDOWND_SUFFIX
                if address == MODULE.IOS4_LOCKDOWND_PATCH_ADDRESS:
                    return bytes(self.instruction)
                raise AssertionError(f"unexpected read at 0x{address:x}")

            def write_memory(self, address: int, data: bytes) -> None:
                if address != MODULE.IOS4_LOCKDOWND_PATCH_ADDRESS:
                    raise AssertionError(f"unexpected write at 0x{address:x}")
                self.instruction[:] = data
                events.append(("write", address, data))

            def interrupt(self) -> None:
                events.append("interrupt")

            def detach(self) -> None:
                events.append("detach")

        class FakeClient:
            def send_command(self, command: str) -> None:
                events.append(("command", command))

        with patch.object(MODULE, "GDBRemote", FakeRemote):
            MODULE.boot_ios4_hacktivated(
                self.endpoint, FakeClient(), 180
            )

        self.assertEqual(
            bytes(instances[0].instruction),
            MODULE.IOS4_LOCKDOWND_REPLACEMENT,
        )
        self.assertLess(events.index("resume"), events.index(("command", "bootx")))
        self.assertIn(
            (
                "write",
                MODULE.IOS4_LOCKDOWND_PATCH_ADDRESS,
                MODULE.IOS4_LOCKDOWND_REPLACEMENT,
            ),
            events,
        )
        self.assertEqual(events[-2:], ["detach", "close"])

    def test_ios4_hacktivation_rejects_wrong_lockdownd(self) -> None:
        events: list[object] = []

        class WrongRemote:
            def __init__(self, host: str, port: int) -> None:
                pass

            def __enter__(self):
                return self

            def __exit__(self, *args: object) -> None:
                pass

            def stop(self) -> None:
                pass

            def set_timeout(self, timeout: float) -> None:
                pass

            def insert_breakpoint(self, address: int, size: int = 2) -> None:
                events.append(("insert", address, size))

            def remove_breakpoint(self, address: int, size: int = 2) -> None:
                events.append(("remove", address, size))

            def resume(self) -> None:
                pass

            def wait_for_stop(self) -> bytes:
                return b"T05"

            def read_register(self, index: int) -> int:
                return MODULE.IOS4_LOCKDOWND_PATCH_ADDRESS

            def read_memory(self, address: int, size: int) -> bytes:
                return b"\0" * size

            def write_memory(self, address: int, data: bytes) -> None:
                raise AssertionError("wrong binary must not be patched")

            def step(self) -> bytes:
                events.append("step")
                return b"T05"

            def interrupt(self) -> None:
                events.append("interrupt")

            def detach(self) -> None:
                events.append("detach")

        class FakeClient:
            def send_command(self, command: str) -> None:
                pass

        with patch.object(MODULE, "GDBRemote", WrongRemote):
            with self.assertRaisesRegex(RuntimeError, "could not step"):
                MODULE.boot_ios4_hacktivated(
                    self.endpoint, FakeClient(), 180
                )

        self.assertIn(
            ("remove", MODULE.IOS4_LOCKDOWND_PATCH_ADDRESS, 4), events
        )
        self.assertEqual(events[-1], "detach")

    def test_ios4_data_ark_installer_uses_authenticated_guest_io(self) -> None:
        events: list[object] = []
        instances: list[object] = []

        class FakeRemote:
            def __init__(self, host: str, port: int) -> None:
                self.registers = {index: index + 0x20 for index in range(16)}
                self.registers[13] = 0x09000000
                self.registers[15] = MODULE.IOS4_RESTORED_EXTERNAL_ENTRY
                self.memory: dict[int, int] = {}
                self.file = b""
                self.waitpid_pending = False
                self.initial_resume = True
                instances.append(self)

            def __enter__(self):
                return self

            def __exit__(self, *args: object) -> None:
                events.append("close")

            def stop(self) -> None:
                events.append("stop")

            def set_timeout(self, timeout: float) -> None:
                events.append(("timeout", timeout))

            def insert_breakpoint(self, address: int, size: int = 2) -> None:
                events.append(("insert", address, size))

            def remove_breakpoint(self, address: int, size: int = 2) -> None:
                events.append(("remove", address, size))

            def read_register(self, index: int) -> int:
                return self.registers[index]

            def write_register(
                self, index: int, value: int, size: int = 4
            ) -> None:
                self.registers[index] = value

            def read_memory(self, address: int, size: int) -> bytes:
                if address == MODULE.IOS4_RESTORED_EXTERNAL_ENTRY:
                    return MODULE.IOS4_RESTORED_EXTERNAL_ENTRY_ANCHOR[:size]
                return bytes(
                    self.memory.get(address + offset, 0)
                    for offset in range(size)
                )

            def write_memory(self, address: int, data: bytes) -> None:
                for offset, value in enumerate(data):
                    self.memory[address + offset] = value

            def resume(self) -> None:
                pc = self.registers[15]
                if self.initial_resume:
                    self.initial_resume = False
                    events.append("boot-resume")
                    return
                function = next(
                    name
                    for name, address in MODULE.IOS4_RESTORED_EXTERNAL_STUBS.items()
                    if address == pc
                )
                events.append(("call", function))
                if function == "fork":
                    result = 42
                elif function == "waitpid" and not self.waitpid_pending:
                    self.waitpid_pending = True
                    result = 0
                elif function == "execv":
                    status = self.registers[1] + 16
                    # The test argv is four words immediately before status.
                    self.write_memory(status, struct.pack("<I", 0))
                    result = 42
                elif function == "fopen":
                    mode = self.read_memory(self.registers[1], 3)
                    result = 0x1234 if mode.startswith(b"wb") else 0x5678
                elif function == "fwrite":
                    size = self.registers[1]
                    self.file = self.read_memory(self.registers[0], size)
                    result = 1
                elif function == "fread":
                    self.write_memory(self.registers[0], self.file)
                    result = 1
                else:
                    result = 0
                self.registers[0] = result
                self.registers[15] = MODULE.IOS4_RESTORED_EXTERNAL_CALL_RETURN

            def wait_for_stop(self) -> bytes:
                return b"T05"

            def interrupt(self) -> None:
                events.append("interrupt")

            def detach(self) -> None:
                events.append("detach")

        class FakeClient:
            def send_command(self, command: str) -> None:
                events.append(("command", command))

        with patch.object(MODULE, "GDBRemote", FakeRemote):
            MODULE.boot_ios4_install_data_ark(
                self.endpoint, FakeClient(), 180
            )

        self.assertEqual(instances[0].file, MODULE.IOS4_DATA_ARK)
        self.assertLess(
            events.index("boot-resume"), events.index(("command", "bootx"))
        )
        self.assertIn(("call", "unmount"), events)
        self.assertEqual(events[-2:], ["detach", "close"])

    def test_ios4_hacktivation_steps_over_colliding_process(self) -> None:
        events: list[object] = []

        class CollidingRemote:
            def __init__(self, host: str, port: int) -> None:
                self.owned = False
                self.pc = MODULE.IOS4_LOCKDOWND_PATCH_ADDRESS
                self.instruction = bytearray(MODULE.IOS4_LOCKDOWND_ORIGINAL)

            def __enter__(self):
                return self

            def __exit__(self, *args: object) -> None:
                pass

            def stop(self) -> None:
                pass

            def set_timeout(self, timeout: float) -> None:
                pass

            def insert_breakpoint(self, address: int, size: int = 2) -> None:
                events.append(("insert", address, size))

            def remove_breakpoint(self, address: int, size: int = 2) -> None:
                events.append(("remove", address, size))

            def resume(self) -> None:
                events.append("resume")

            def wait_for_stop(self) -> bytes:
                self.pc = MODULE.IOS4_LOCKDOWND_PATCH_ADDRESS
                return b"T05"

            def step(self) -> bytes:
                self.owned = True
                self.pc = MODULE.IOS4_LOCKDOWND_PATCH_ADDRESS + 4
                events.append("step")
                return b"T05"

            def read_register(self, index: int) -> int:
                return self.pc

            def read_memory(self, address: int, size: int) -> bytes:
                if address == MODULE.IOS4_LOCKDOWND_PATCH_ADDRESS:
                    return bytes(self.instruction)
                if not self.owned:
                    return b"\0" * size
                if address == MODULE.IOS4_LOCKDOWND_PREFIX_ADDRESS:
                    return MODULE.IOS4_LOCKDOWND_PREFIX
                if address == MODULE.IOS4_LOCKDOWND_SUFFIX_ADDRESS:
                    return MODULE.IOS4_LOCKDOWND_SUFFIX
                raise AssertionError(f"unexpected read at 0x{address:x}")

            def write_memory(self, address: int, data: bytes) -> None:
                self.instruction[:] = data

            def interrupt(self) -> None:
                events.append("interrupt")

            def detach(self) -> None:
                events.append("detach")

        class FakeClient:
            def send_command(self, command: str) -> None:
                pass

        with patch.object(MODULE, "GDBRemote", CollidingRemote):
            MODULE.boot_ios4_hacktivated(
                self.endpoint, FakeClient(), 180
            )

        self.assertEqual(events.count("step"), 1)
        self.assertEqual(events.count("resume"), 2)
        self.assertEqual(events[-1], "detach")


if __name__ == "__main__":
    unittest.main()
