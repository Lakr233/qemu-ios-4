#!/usr/bin/env python3
"""Upload the iPhone 3G restore components through QEMU USB-over-IP."""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import struct
import time
from collections.abc import Callable
from functools import partial
from pathlib import Path

from usb.core import USBError

from gdbremote import GDBRemote, GDBRemoteError
from img3_extract import find_data_tag
from irecv_profiles import send_ios2_buffer, send_ios2_ibec_buffer
from usboip import Device, Endpoint, IRecvAdapter, USBRequestTimeout


DEFAULT_BOOT_ARGS_ADDRESS = 0x18020634
BOOT_ARGS_SIZE = 40
ORIGINAL_BOOT_ARGS = b"rd=md0 nand-enable-reformat=1 -progress\0"
IOS2_DEVICE_TREE_STATE_ADDRESS = 0x1801B780
IOS2_DEVICE_TREE_DESTINATION = 0x0BF00000
IOS2_DEVICE_TREE_STATE_TIMEOUT = 2
IOS2_DEVICE_TREE_COMMAND_ATTEMPTS = 2
IBEC_HANDOFF_RESET_DELAY = 5.0
IOS4_CRYPTO_COUNT_PATCH_ADDRESS = 0x805EDC50
IOS4_CRYPTO_COUNT_COMPLETION_ADDRESS = 0x805EDC54
IOS4_CRYPTO_COUNT_ORIGINAL = bytes.fromhex("043043e2")
IOS4_CRYPTO_COUNT_REPLACEMENT = bytes.fromhex("0330a0e1")
IOS4_CRYPTO_COUNT_ANCHOR_ADDRESS = 0x805EDC30
IOS4_CRYPTO_COUNT_ANCHOR = bytes.fromhex(
    "50319fe533ff2fe14c319fe54c119fe50520a0e1603084e50630a0e36430"
    "84e5043043e2683084e50139a0e36c3084e52cc19fe5"
)
IOS4_LOCKDOWND_PATCH_ADDRESS = 0x000100A0
IOS4_LOCKDOWND_ORIGINAL = bytes.fromhex("0500001a")
IOS4_LOCKDOWND_REPLACEMENT = bytes.fromhex("d60100ea")
IOS4_LOCKDOWND_PREFIX_ADDRESS = 0x00010090
IOS4_LOCKDOWND_PREFIX = bytes.fromhex(
    "f5d201eb00b0a0e13a2c00eb000050e3"
)
IOS4_LOCKDOWND_SUFFIX_ADDRESS = 0x000100A4
IOS4_LOCKDOWND_SUFFIX = bytes.fromhex(
    "e40b9fe5e41b9fe500008fe001108fe0"
)
IOS4_DEVICE_TREE_DESTINATION = 0x0BF00000
IOS4_DEVICE_TREE_MAX_SIZE = 1024 * 1024
IOS4_UNMODELED_BASEBAND_NODES = (
    ("baseband", b"baseband,n82\0"),
    ("arm-io/spi2", b"spi,s5l8900x,baseband\0"),
)
IOS4_RESTORED_EXTERNAL_ENTRY = 0x00004408
IOS4_RESTORED_EXTERNAL_ENTRY_ANCHOR = bytes.fromhex(
    "00009de504108de2014080e2042181e007d0cde30230a0e1"
    "044093e4000054e3fcffff1ad10100eb801e02ea"
)
IOS4_RESTORED_EXTERNAL_CALL_RETURN = 0x0008AE80
IOS4_RESTORED_EXTERNAL_STUBS = {
    "chown": 0x0008BE10,
    "execv": 0x0008BE34,
    "fclose": 0x0008BE3C,
    "fopen": 0x0008BE50,
    "fork": 0x0008BE54,
    "fread": 0x0008BE64,
    "fwrite": 0x0008BE78,
    "mkdir": 0x0008BED8,
    "unmount": 0x0008BFD8,
    "waitpid": 0x0008BFF8,
}
IOS4_RESTORED_EXTERNAL_SCRATCH = 0x0008ED58
IOS4_RESTORED_EXTERNAL_SCRATCH_SIZE = 0x7F0
IOS4_DATA_ARK_PATH = b"/mnt2/root/Library/Lockdown/data_ark.plist\0"
IOS4_DATA_ARK = (
    b'<?xml version="1.0" encoding="UTF-8"?>\n'
    b'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
    b'"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
    b'<plist version="1.0">\n<dict>\n'
    b'\t<key>-ActivationState</key>\n'
    b'\t<string>FactoryActivated</string>\n'
    b'\t<key>-BrickState</key>\n\t<false/>\n'
    b'</dict>\n</plist>\n'
)
IOS2_DEVICE_TREE_STATE_ANCHORS = (
    (
        0x1800DF40,
        bytes.fromhex(
            "f0b5464640b41b4c884604af2168061c002903d0184a1368002b20d1"
        ),
    ),
    (
        0x1800E05C,
        bytes.fromhex(
            "154dbf231b05154c2b604368291c2360221cf5f7e5ff002803da0023"
        ),
    ),
)


def existing_file(value: str) -> Path:
    path = Path(value)
    if not path.is_file():
        raise argparse.ArgumentTypeError(f"file does not exist: {path}")
    return path


def img3_data_logical_size(path: Path) -> int:
    with path.open("rb") as stream:
        return find_data_tag(stream, path.stat().st_size).logical_size


def connect(timeout: float):
    from pymobiledevice3.exceptions import IRecvNoDeviceConnectedError
    from pymobiledevice3.irecv import IRecv

    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("timed out waiting for the emulated iPhone")
        try:
            return IRecv(timeout=min(1, remaining))
        except (IRecvNoDeviceConnectedError, USBError):
            time.sleep(min(0.1, remaining))


def close(adapter: IRecvAdapter, client) -> None:
    with contextlib.suppress(Exception):
        adapter.dispose_resources(client.device)


def reset_bus(
    endpoint: Endpoint,
    request_timeout_ms: int | None = None,
    discovery_timeout: float = 0,
) -> None:
    deadline = time.monotonic() + discovery_timeout
    while True:
        try:
            device = Device.connect(endpoint, request_timeout_ms)
            break
        except (OSError, USBError):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise
            time.sleep(min(0.1, remaining))
    try:
        device.reset()
    finally:
        device.close()


def reconnect(
    endpoint: Endpoint,
    adapter: IRecvAdapter,
    client,
    timeout: float,
):
    close(adapter, client)
    time.sleep(0.5)
    request_timeout_ms = getattr(adapter, "request_timeout_ms", None)
    if request_timeout_ms is None:
        reset_bus(endpoint, discovery_timeout=timeout)
    else:
        reset_bus(
            endpoint,
            request_timeout_ms,
            discovery_timeout=timeout,
        )
    return connect(timeout)


def send_ios2_stage_command(
    endpoint: Endpoint,
    adapter: IRecvAdapter,
    client: object,
    command: str,
    reconnect_timeout: float,
):
    try:
        client.send_command(command)
        return client
    except USBRequestTimeout:
        close(adapter, client)
        deadline = time.monotonic() + reconnect_timeout
        while True:
            try:
                reset_device = Device.connect(endpoint)
                break
            except (OSError, USBError, TimeoutError):
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(
                        f"timed out waiting to reset USB after {command}"
                    )
                time.sleep(min(0.1, remaining))
        try:
            reset_device.reset()
        finally:
            reset_device.close()
        time.sleep(0.5)
        return connect(reconnect_timeout)


def read_ios2_device_tree_state(endpoint: Endpoint) -> tuple[int, int]:
    with GDBRemote(endpoint.host, endpoint.port) as remote:
        remote.stop()
        try:
            for address, expected in IOS2_DEVICE_TREE_STATE_ANCHORS:
                observed = remote.read_memory(address, len(expected))
                if observed != expected:
                    raise RuntimeError(
                        "5A347 iBEC DeviceTree state anchor mismatch at "
                        f"0x{address:x}: {observed.hex()}"
                    )
            state = remote.read_memory(IOS2_DEVICE_TREE_STATE_ADDRESS, 8)
        finally:
            remote.detach()
    return struct.unpack("<II", state)


def wait_for_ios2_device_tree_state(
    endpoint: Endpoint,
    expected_size: int,
    timeout: float = IOS2_DEVICE_TREE_STATE_TIMEOUT,
) -> bool:
    deadline = time.monotonic() + timeout
    while True:
        state = read_ios2_device_tree_state(endpoint)
        expected = (IOS2_DEVICE_TREE_DESTINATION, expected_size)
        if state == expected:
            time.sleep(0.05)
            if read_ios2_device_tree_state(endpoint) == expected:
                return True
        elif state != (0, 0):
            raise RuntimeError(
                "5A347 iBEC published an unexpected DeviceTree state: "
                f"address=0x{state[0]:x} size=0x{state[1]:x}"
            )
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.05)


def register_ios2_device_tree(
    usboip_endpoint: Endpoint,
    gdb_endpoint: Endpoint,
    adapter: IRecvAdapter,
    client: object,
    expected_size: int,
    reconnect_timeout: float,
) -> object:
    for attempt in range(IOS2_DEVICE_TREE_COMMAND_ATTEMPTS):
        client = send_ios2_stage_command(
            usboip_endpoint,
            adapter,
            client,
            "devicetree",
            reconnect_timeout,
        )
        if wait_for_ios2_device_tree_state(gdb_endpoint, expected_size):
            print(
                "5A347 iBEC published the DeviceTree state at "
                f"0x{IOS2_DEVICE_TREE_DESTINATION:x}",
                flush=True,
            )
            return client
        if attempt + 1 != IOS2_DEVICE_TREE_COMMAND_ATTEMPTS:
            print(
                "5A347 iBEC did not publish the DeviceTree state; "
                "retrying the command",
                flush=True,
            )
    raise TimeoutError("5A347 iBEC did not publish the DeviceTree state")


def send_standard_buffer(client: object, data: bytes) -> None:
    client.send_buffer(data)


def upload(
    client: object,
    label: str,
    path: Path,
    transfer: Callable[[object, bytes], object | None],
) -> object:
    size = path.stat().st_size
    print(f"uploading {label}: {path} ({size} bytes)", flush=True)
    data = path.read_bytes()
    result = transfer(client, data)
    return client if result is None else result


def wait_for_kernel_uart(path: Path, offset: int, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with path.open("rb") as stream:
                stream.seek(offset)
                if b"iBoot version:" in stream.read():
                    return
        except FileNotFoundError:
            pass
        time.sleep(0.1)
    raise TimeoutError("timed out waiting for kernel UART output")


def patch_boot_args(endpoint: Endpoint, address: int, value: str) -> None:
    try:
        encoded = value.encode("ascii") + b"\0"
    except UnicodeEncodeError as error:
        raise ValueError("boot arguments must be ASCII") from error
    if len(encoded) > BOOT_ARGS_SIZE:
        raise ValueError(
            f"boot arguments require {len(encoded)} bytes; limit is {BOOT_ARGS_SIZE}"
        )
    encoded = encoded.ljust(BOOT_ARGS_SIZE, b"\0")

    with GDBRemote(endpoint.host, endpoint.port) as remote:
        remote.stop()
        original = remote.read_memory(address, BOOT_ARGS_SIZE)
        if original != ORIGINAL_BOOT_ARGS:
            raise ValueError(
                f"unexpected iBEC boot-args bytes at 0x{address:x}: "
                f"{original!r}"
            )
        remote.write_memory(address, encoded)
        if remote.read_memory(address, BOOT_ARGS_SIZE) != encoded:
            raise RuntimeError("iBEC boot-args readback does not match")
        remote.detach()


def _device_tree_header(data: bytes, offset: int) -> tuple[int, int, int]:
    if offset < 0 or offset + 8 > len(data):
        raise ValueError("Apple DeviceTree node header lies outside the blob")
    property_count, child_count = struct.unpack_from("<II", data, offset)
    if property_count > 4096 or child_count > 4096:
        raise ValueError("Apple DeviceTree node counts exceed the safety bound")
    return property_count, child_count, offset + 8


def _device_tree_properties_end(data: bytes, offset: int) -> int:
    property_count, _, cursor = _device_tree_header(data, offset)
    for _ in range(property_count):
        if cursor + 36 > len(data):
            raise ValueError("Apple DeviceTree property header is truncated")
        length = struct.unpack_from("<I", data, cursor + 32)[0] & 0x7FFFFFFF
        cursor += 36 + ((length + 3) & ~3)
        if cursor > len(data):
            raise ValueError("Apple DeviceTree property value is truncated")
    return cursor


def _device_tree_node_end(data: bytes, offset: int, depth: int = 0) -> int:
    if depth > 32:
        raise ValueError("Apple DeviceTree nesting exceeds the safety bound")
    _, child_count, _ = _device_tree_header(data, offset)
    cursor = _device_tree_properties_end(data, offset)
    for _ in range(child_count):
        cursor = _device_tree_node_end(data, cursor, depth + 1)
    return cursor


def _device_tree_property(
    data: bytes, node: int, name: str
) -> tuple[int, bytes] | None:
    property_count, _, cursor = _device_tree_header(data, node)
    encoded_name = name.encode("ascii")
    for _ in range(property_count):
        raw_name = data[cursor : cursor + 32]
        length = struct.unpack_from("<I", data, cursor + 32)[0] & 0x7FFFFFFF
        value = cursor + 36
        if raw_name.split(b"\0", 1)[0] == encoded_name:
            return value, data[value : value + length]
        cursor = value + ((length + 3) & ~3)
    return None


def _device_tree_path(data: bytes, path: str) -> int:
    node = 0
    for component in (item for item in path.split("/") if item):
        _, child_count, _ = _device_tree_header(data, node)
        child = _device_tree_properties_end(data, node)
        matches: list[int] = []
        for _ in range(child_count):
            name = _device_tree_property(data, child, "name")
            if name is not None:
                raw_name = name[1]
                if raw_name.split(b"\0", 1)[0] == component.encode("ascii"):
                    matches.append(child)
            child = _device_tree_node_end(data, child)
        if len(matches) != 1:
            raise ValueError(
                f"Apple DeviceTree path /{path} has {len(matches)} "
                f"matches for {component!r}"
            )
        node = matches[0]
    return node


def unmatch_ios4_unmodeled_baseband(
    endpoint: Endpoint, size: int
) -> str:
    """Tell stock XNU that this VM has no implemented cellular modem."""

    if size <= 0 or size > IOS4_DEVICE_TREE_MAX_SIZE:
        raise ValueError(f"unexpected 8C148 DeviceTree size: 0x{size:x}")

    with GDBRemote(endpoint.host, endpoint.port) as remote:
        remote.stop()
        writes: list[tuple[int, bytes]] = []
        try:
            data = remote.read_memory(IOS4_DEVICE_TREE_DESTINATION, size)
            if _device_tree_node_end(data, 0) != size:
                raise ValueError(
                    "8C148 DeviceTree parser did not consume its exact "
                    f"0x{size:x}-byte allocation"
                )

            replacements: list[tuple[int, bytes]] = []
            for path, expected in IOS4_UNMODELED_BASEBAND_NODES:
                node = _device_tree_path(data, path)
                compatible = _device_tree_property(data, node, "compatible")
                if compatible is None or compatible[1] != expected:
                    observed = None if compatible is None else compatible[1]
                    raise ValueError(
                        f"8C148 DeviceTree /{path}:compatible mismatch: "
                        f"{observed!r}"
                    )
                replacements.append(
                    (IOS4_DEVICE_TREE_DESTINATION + compatible[0], expected[:1])
                )

            digest = hashlib.sha256(data).hexdigest()
            for address, original in replacements:
                remote.write_memory(address, b"x")
                writes.append((address, original))
                if remote.read_memory(address, 1) != b"x":
                    raise RuntimeError(
                        f"8C148 DeviceTree writeback failed at 0x{address:x}"
                    )
        except Exception:
            for address, original in reversed(writes):
                with contextlib.suppress(Exception):
                    remote.write_memory(address, original)
            raise
        finally:
            remote.detach()

    print(
        "authenticated 8C148 DeviceTree and unmatched the absent "
        f"baseband/SPI2 nodes (sha256 {digest})",
        flush=True,
    )
    return digest


def boot_ios4_with_restore_crypto_keys(
    endpoint: Endpoint, client: object, timeout: float
) -> None:
    """Preserve all six official crypto selectors for one restore boot."""

    with GDBRemote(endpoint.host, endpoint.port) as remote:
        entry_breakpoint = False
        completion_breakpoint = False
        running = False
        patched = False
        detached = False
        try:
            remote.stop()
            remote.insert_breakpoint(IOS4_CRYPTO_COUNT_PATCH_ADDRESS, 4)
            entry_breakpoint = True
            remote.set_timeout(timeout)
            remote.resume()
            running = True
            with contextlib.suppress(USBError):
                client.send_command("bootx")

            remote.wait_for_stop()
            running = False
            pc = remote.read_register(15) & ~1
            if pc != IOS4_CRYPTO_COUNT_PATCH_ADDRESS:
                raise RuntimeError(
                    "iOS 4 crypto constructor stopped at unexpected "
                    f"address 0x{pc:x}"
                )
            remote.remove_breakpoint(IOS4_CRYPTO_COUNT_PATCH_ADDRESS, 4)
            entry_breakpoint = False

            anchor = remote.read_memory(
                IOS4_CRYPTO_COUNT_ANCHOR_ADDRESS,
                len(IOS4_CRYPTO_COUNT_ANCHOR),
            )
            if anchor != IOS4_CRYPTO_COUNT_ANCHOR:
                raise RuntimeError("iOS 4 crypto constructor anchor mismatch")
            original = remote.read_memory(
                IOS4_CRYPTO_COUNT_PATCH_ADDRESS,
                len(IOS4_CRYPTO_COUNT_ORIGINAL),
            )
            if original != IOS4_CRYPTO_COUNT_ORIGINAL:
                raise RuntimeError(
                    "iOS 4 crypto count instruction is not original: "
                    f"{original.hex()}"
                )

            remote.write_memory(
                IOS4_CRYPTO_COUNT_PATCH_ADDRESS,
                IOS4_CRYPTO_COUNT_REPLACEMENT,
            )
            patched = True
            if remote.read_memory(
                IOS4_CRYPTO_COUNT_PATCH_ADDRESS,
                len(IOS4_CRYPTO_COUNT_REPLACEMENT),
            ) != IOS4_CRYPTO_COUNT_REPLACEMENT:
                raise RuntimeError("iOS 4 crypto count patch readback failed")

            remote.insert_breakpoint(IOS4_CRYPTO_COUNT_COMPLETION_ADDRESS, 4)
            completion_breakpoint = True
            remote.resume()
            running = True
            remote.wait_for_stop()
            running = False
            pc = remote.read_register(15) & ~1
            if pc != IOS4_CRYPTO_COUNT_COMPLETION_ADDRESS:
                raise RuntimeError(
                    "iOS 4 crypto count completion stopped at unexpected "
                    f"address 0x{pc:x}"
                )
            remote.remove_breakpoint(
                IOS4_CRYPTO_COUNT_COMPLETION_ADDRESS, 4
            )
            completion_breakpoint = False

            if remote.read_memory(
                IOS4_CRYPTO_COUNT_PATCH_ADDRESS,
                len(IOS4_CRYPTO_COUNT_REPLACEMENT),
            ) != IOS4_CRYPTO_COUNT_REPLACEMENT:
                raise RuntimeError("iOS 4 crypto count patch changed before restore")
            remote.write_memory(
                IOS4_CRYPTO_COUNT_PATCH_ADDRESS,
                IOS4_CRYPTO_COUNT_ORIGINAL,
            )
            patched = False
            if remote.read_memory(
                IOS4_CRYPTO_COUNT_PATCH_ADDRESS,
                len(IOS4_CRYPTO_COUNT_ORIGINAL),
            ) != IOS4_CRYPTO_COUNT_ORIGINAL:
                raise RuntimeError("iOS 4 crypto count restore failed")
            remote.detach()
            detached = True
        finally:
            if not detached:
                if running:
                    with contextlib.suppress(Exception):
                        remote.interrupt()
                    running = False
                if entry_breakpoint:
                    with contextlib.suppress(Exception):
                        remote.remove_breakpoint(
                            IOS4_CRYPTO_COUNT_PATCH_ADDRESS, 4
                        )
                if completion_breakpoint:
                    with contextlib.suppress(Exception):
                        remote.remove_breakpoint(
                            IOS4_CRYPTO_COUNT_COMPLETION_ADDRESS, 4
                        )
                if patched:
                    observed = remote.read_memory(
                        IOS4_CRYPTO_COUNT_PATCH_ADDRESS,
                        len(IOS4_CRYPTO_COUNT_REPLACEMENT),
                    )
                    if observed != IOS4_CRYPTO_COUNT_REPLACEMENT:
                        raise RuntimeError(
                            "iOS 4 crypto count patch changed during cleanup"
                        )
                    remote.write_memory(
                        IOS4_CRYPTO_COUNT_PATCH_ADDRESS,
                        IOS4_CRYPTO_COUNT_ORIGINAL,
                    )
                    if remote.read_memory(
                        IOS4_CRYPTO_COUNT_PATCH_ADDRESS,
                        len(IOS4_CRYPTO_COUNT_ORIGINAL),
                    ) != IOS4_CRYPTO_COUNT_ORIGINAL:
                        raise RuntimeError(
                            "iOS 4 crypto count cleanup restore failed"
                        )
                with contextlib.suppress(Exception):
                    remote.detach()


def boot_ios4_hacktivated(
    endpoint: Endpoint, client: object, timeout: float
) -> None:
    """Apply the Legacy iOS Kit 8C148 lockdownd branch in memory.

    The on-disk binary remains Apple's signed original.  The breakpoint is
    armed before bootx so the first lockdownd instruction cannot race the
    debugger; the surrounding bytes authenticate the exact 8C148 consumer
    before the one-instruction update is made.
    """

    with GDBRemote(endpoint.host, endpoint.port) as remote:
        breakpoint = False
        running = False
        patched = False
        detached = False
        try:
            remote.stop()
            remote.insert_breakpoint(IOS4_LOCKDOWND_PATCH_ADDRESS, 4)
            breakpoint = True
            remote.set_timeout(timeout)
            remote.resume()
            running = True
            with contextlib.suppress(USBError):
                client.send_command("bootx")

            while True:
                remote.wait_for_stop()
                running = False
                pc = remote.read_register(15) & ~1
                if pc != IOS4_LOCKDOWND_PATCH_ADDRESS:
                    raise RuntimeError(
                        "iOS 4 lockdownd stopped at unexpected "
                        f"address 0x{pc:x}"
                    )

                try:
                    prefix = remote.read_memory(
                        IOS4_LOCKDOWND_PREFIX_ADDRESS,
                        len(IOS4_LOCKDOWND_PREFIX),
                    )
                    suffix = remote.read_memory(
                        IOS4_LOCKDOWND_SUFFIX_ADDRESS,
                        len(IOS4_LOCKDOWND_SUFFIX),
                    )
                except GDBRemoteError:
                    prefix = suffix = b""
                if (
                    prefix == IOS4_LOCKDOWND_PREFIX
                    and suffix == IOS4_LOCKDOWND_SUFFIX
                ):
                    break

                # Several launchd children map unrelated text at this same
                # virtual address.  Execute that instruction once with the
                # global raw-address breakpoint removed, then arm it again
                # before allowing the Guest to continue.
                remote.remove_breakpoint(IOS4_LOCKDOWND_PATCH_ADDRESS, 4)
                breakpoint = False
                remote.step()
                stepped_pc = remote.read_register(15) & ~1
                if stepped_pc == IOS4_LOCKDOWND_PATCH_ADDRESS:
                    raise RuntimeError(
                        "iOS 4 hacktivation could not step over an unowned "
                        f"mapping at 0x{stepped_pc:x}"
                    )
                remote.insert_breakpoint(IOS4_LOCKDOWND_PATCH_ADDRESS, 4)
                breakpoint = True
                remote.resume()
                running = True

            remote.remove_breakpoint(IOS4_LOCKDOWND_PATCH_ADDRESS, 4)
            breakpoint = False
            original = remote.read_memory(
                IOS4_LOCKDOWND_PATCH_ADDRESS,
                len(IOS4_LOCKDOWND_ORIGINAL),
            )
            if original != IOS4_LOCKDOWND_ORIGINAL:
                raise RuntimeError(
                    "iOS 4 lockdownd instruction is not original: "
                    f"{original.hex()}"
                )

            remote.write_memory(
                IOS4_LOCKDOWND_PATCH_ADDRESS,
                IOS4_LOCKDOWND_REPLACEMENT,
            )
            patched = True
            if remote.read_memory(
                IOS4_LOCKDOWND_PATCH_ADDRESS,
                len(IOS4_LOCKDOWND_REPLACEMENT),
            ) != IOS4_LOCKDOWND_REPLACEMENT:
                raise RuntimeError("iOS 4 lockdownd patch readback failed")

            # Detaching while stopped both preserves the private text-page
            # update and resumes the selected process without an outstanding
            # continue packet.
            remote.detach()
            detached = True
            print(
                "authenticated 8C148 lockdownd hacktivation branch applied",
                flush=True,
            )
        finally:
            if not detached:
                if running:
                    with contextlib.suppress(Exception):
                        remote.interrupt()
                    running = False
                if breakpoint:
                    with contextlib.suppress(Exception):
                        remote.remove_breakpoint(
                            IOS4_LOCKDOWND_PATCH_ADDRESS, 4
                        )
                if patched:
                    observed = remote.read_memory(
                        IOS4_LOCKDOWND_PATCH_ADDRESS,
                        len(IOS4_LOCKDOWND_REPLACEMENT),
                    )
                    if observed != IOS4_LOCKDOWND_REPLACEMENT:
                        raise RuntimeError(
                            "iOS 4 lockdownd patch changed during cleanup"
                        )
                    remote.write_memory(
                        IOS4_LOCKDOWND_PATCH_ADDRESS,
                        IOS4_LOCKDOWND_ORIGINAL,
                    )
                with contextlib.suppress(Exception):
                    remote.detach()


class GuestCallScratch:
    """Own the authenticated binary's bounded, restorable BSS scratch."""

    def __init__(self, remote: GDBRemote) -> None:
        self.remote = remote
        self.base = IOS4_RESTORED_EXTERNAL_SCRATCH
        self.size = IOS4_RESTORED_EXTERNAL_SCRATCH_SIZE
        self.original = remote.read_memory(self.base, self.size)
        self.cursor = 0

    def add(self, value: bytes, alignment: int = 4) -> int:
        self.cursor = (self.cursor + alignment - 1) & -alignment
        end = self.cursor + len(value)
        if end > self.size:
            raise RuntimeError("iOS 4 data-ark call scratch is exhausted")
        address = self.base + self.cursor
        self.remote.write_memory(address, value)
        self.cursor = end
        return address

    def add_cstring(self, value: bytes) -> int:
        if not value.endswith(b"\0") or b"\0" in value[:-1]:
            raise ValueError("Guest string must contain one trailing NUL")
        return self.add(value, 1)

    def add_words(self, *values: int) -> int:
        return self.add(struct.pack(f"<{len(values)}I", *values))

    def restore(self) -> None:
        self.remote.write_memory(self.base, self.original)


def _setup_ios4_guest_call(
    remote: GDBRemote, function: str, return_address: int, *arguments: int
) -> None:
    if len(arguments) > 4:
        raise ValueError("Guest call supports at most four register arguments")
    for index, value in enumerate(arguments):
        remote.write_register(index, value)
    remote.write_register(14, return_address)
    remote.write_register(15, IOS4_RESTORED_EXTERNAL_STUBS[function])


def _wait_ios4_guest_call(remote: GDBRemote, name: str) -> int:
    remote.wait_for_stop()
    pc = remote.read_register(15) & ~1
    if pc != IOS4_RESTORED_EXTERNAL_CALL_RETURN:
        raise RuntimeError(
            f"iOS 4 {name} call returned to unexpected address 0x{pc:x}"
        )
    return remote.read_register(0)


def _call_ios4_guest(
    remote: GDBRemote, function: str, *arguments: int
) -> int:
    _setup_ios4_guest_call(
        remote, function, IOS4_RESTORED_EXTERNAL_CALL_RETURN, *arguments
    )
    remote.resume()
    return _wait_ios4_guest_call(remote, function)


def _exec_ios4_program(
    remote: GDBRemote,
    executable: int,
    arguments: int,
    wait_status: int,
    label: str,
) -> None:
    """Fork, exec an Apple restore utility, and retain the stopped parent."""

    child_pid = _call_ios4_guest(remote, "fork")
    if child_pid == 0xFFFFFFFF:
        raise RuntimeError(f"iOS 4 {label} fork failed")

    if child_pid == 0:
        _setup_ios4_guest_call(
            remote,
            "execv",
            IOS4_RESTORED_EXTERNAL_CALL_RETURN,
            executable,
            arguments,
        )
        remote.resume()
        child_pid = _wait_ios4_guest_call(remote, "fork parent")
        if child_pid in (0, 0xFFFFFFFF):
            raise RuntimeError(f"iOS 4 {label} did not reach its parent")
        result = _call_ios4_guest(remote, "waitpid", child_pid, wait_status, 0)
    else:
        _setup_ios4_guest_call(
            remote,
            "waitpid",
            IOS4_RESTORED_EXTERNAL_CALL_RETURN,
            child_pid,
            wait_status,
            0,
        )
        remote.resume()
        child_result = _wait_ios4_guest_call(remote, "fork child")
        if child_result != 0:
            raise RuntimeError(
                f"iOS 4 {label} child returned an unexpected fork result "
                f"0x{child_result:x}"
            )
        _setup_ios4_guest_call(
            remote,
            "execv",
            IOS4_RESTORED_EXTERNAL_CALL_RETURN,
            executable,
            arguments,
        )
        remote.resume()
        result = _wait_ios4_guest_call(remote, "waitpid")

    if result != child_pid:
        raise RuntimeError(
            f"iOS 4 {label} waitpid returned 0x{result:x}, "
            f"expected 0x{child_pid:x}"
        )
    status = struct.unpack("<I", remote.read_memory(wait_status, 4))[0]
    if status != 0:
        raise RuntimeError(
            f"iOS 4 {label} exited with wait status 0x{status:x}"
        )


def boot_ios4_install_data_ark(
    endpoint: Endpoint, client: object, timeout: float
) -> None:
    """Persist Legacy iOS Kit's minimal activation record on disk0s2."""

    with GDBRemote(endpoint.host, endpoint.port) as remote:
        breakpoint = False
        call_breakpoint = False
        running = False
        scratch: GuestCallScratch | None = None
        saved_registers: tuple[int, ...] | None = None
        detached = False
        try:
            remote.stop()
            remote.insert_breakpoint(IOS4_RESTORED_EXTERNAL_ENTRY, 4)
            breakpoint = True
            remote.set_timeout(timeout)
            remote.resume()
            running = True
            with contextlib.suppress(USBError):
                client.send_command("bootx")

            while True:
                remote.wait_for_stop()
                running = False
                pc = remote.read_register(15) & ~1
                if pc != IOS4_RESTORED_EXTERNAL_ENTRY:
                    raise RuntimeError(
                        "iOS 4 restored_external stopped at unexpected "
                        f"address 0x{pc:x}"
                    )
                anchor = remote.read_memory(
                    IOS4_RESTORED_EXTERNAL_ENTRY,
                    len(IOS4_RESTORED_EXTERNAL_ENTRY_ANCHOR),
                )
                if anchor == IOS4_RESTORED_EXTERNAL_ENTRY_ANCHOR:
                    break

                # The raw address is reused by earlier launchd children.
                # Execute one instruction only in that unowned mapping and
                # re-arm before the authenticated restored image can enter.
                remote.remove_breakpoint(IOS4_RESTORED_EXTERNAL_ENTRY, 4)
                breakpoint = False
                remote.step()
                stepped_pc = remote.read_register(15) & ~1
                if stepped_pc == IOS4_RESTORED_EXTERNAL_ENTRY:
                    raise RuntimeError(
                        "iOS 4 data-ark installer could not step over an "
                        "unowned restored_external entry mapping"
                    )
                remote.insert_breakpoint(IOS4_RESTORED_EXTERNAL_ENTRY, 4)
                breakpoint = True
                remote.resume()
                running = True

            remote.remove_breakpoint(IOS4_RESTORED_EXTERNAL_ENTRY, 4)
            breakpoint = False
            remote.insert_breakpoint(IOS4_RESTORED_EXTERNAL_CALL_RETURN, 4)
            call_breakpoint = True
            saved_registers = tuple(remote.read_register(i) for i in range(16))
            scratch = GuestCallScratch(remote)
            fsck_hfs = scratch.add_cstring(b"/sbin/fsck_hfs\0")
            force_yes = scratch.add_cstring(b"-fy\0")
            mount_hfs = scratch.add_cstring(b"/sbin/mount_hfs\0")
            disk = scratch.add_cstring(b"/dev/disk0s2\0")
            mount_point = scratch.add_cstring(b"/mnt2\0")
            fsck_argv = scratch.add_words(fsck_hfs, force_yes, disk, 0)
            mount_argv = scratch.add_words(mount_hfs, disk, mount_point, 0)
            wait_status = scratch.add_words(0)
            directories = tuple(
                scratch.add_cstring(path)
                for path in (
                    b"/mnt2/root\0",
                    b"/mnt2/root/Library\0",
                    b"/mnt2/root/Library/Lockdown\0",
                )
            )
            data_ark_path = scratch.add_cstring(IOS4_DATA_ARK_PATH)
            write_mode = scratch.add_cstring(b"wb\0")
            read_mode = scratch.add_cstring(b"rb\0")
            data_ark = scratch.add(IOS4_DATA_ARK, 1)
            readback = scratch.add(bytes(len(IOS4_DATA_ARK)), 1)

            _exec_ios4_program(
                remote, fsck_hfs, fsck_argv, wait_status, "fsck_hfs"
            )
            _exec_ios4_program(
                remote, mount_hfs, mount_argv, wait_status, "mount_hfs"
            )
            for directory in directories:
                # EEXIST is expected on a previously booted data volume.
                _call_ios4_guest(remote, "mkdir", directory, 0o755)

            stream = _call_ios4_guest(
                remote, "fopen", data_ark_path, write_mode
            )
            if stream == 0:
                raise RuntimeError("iOS 4 data_ark.plist fopen for write failed")
            written = _call_ios4_guest(
                remote, "fwrite", data_ark, len(IOS4_DATA_ARK), 1, stream
            )
            if written != 1:
                raise RuntimeError(
                    f"iOS 4 data_ark.plist fwrite returned {written}"
                )
            if _call_ios4_guest(remote, "fclose", stream) != 0:
                raise RuntimeError("iOS 4 data_ark.plist fclose failed")
            if _call_ios4_guest(remote, "chown", data_ark_path, 0, 0) != 0:
                raise RuntimeError("iOS 4 data_ark.plist chown failed")

            stream = _call_ios4_guest(
                remote, "fopen", data_ark_path, read_mode
            )
            if stream == 0:
                raise RuntimeError("iOS 4 data_ark.plist fopen for verify failed")
            read = _call_ios4_guest(
                remote, "fread", readback, len(IOS4_DATA_ARK), 1, stream
            )
            close_result = _call_ios4_guest(remote, "fclose", stream)
            if read != 1 or close_result != 0:
                raise RuntimeError("iOS 4 data_ark.plist readback failed")
            observed = remote.read_memory(readback, len(IOS4_DATA_ARK))
            if observed != IOS4_DATA_ARK:
                raise RuntimeError("iOS 4 data_ark.plist content mismatch")
            if _call_ios4_guest(remote, "unmount", mount_point, 0) != 0:
                raise RuntimeError("iOS 4 data volume unmount failed")

            scratch.restore()
            scratch = None
            for index, value in enumerate(saved_registers):
                remote.write_register(index, value)
            saved_registers = None
            remote.remove_breakpoint(IOS4_RESTORED_EXTERNAL_CALL_RETURN, 4)
            call_breakpoint = False
            remote.detach()
            detached = True
            print(
                "authenticated 8C148 restored_external and persisted "
                "Legacy data_ark.plist on disk0s2",
                flush=True,
            )
        finally:
            if not detached:
                if running:
                    with contextlib.suppress(Exception):
                        remote.interrupt()
                if scratch is not None:
                    with contextlib.suppress(Exception):
                        scratch.restore()
                if saved_registers is not None:
                    for index, value in enumerate(saved_registers):
                        with contextlib.suppress(Exception):
                            remote.write_register(index, value)
                if breakpoint:
                    with contextlib.suppress(Exception):
                        remote.remove_breakpoint(
                            IOS4_RESTORED_EXTERNAL_ENTRY, 4
                        )
                if call_breakpoint:
                    with contextlib.suppress(Exception):
                        remote.remove_breakpoint(
                            IOS4_RESTORED_EXTERNAL_CALL_RETURN, 4
                        )
                with contextlib.suppress(Exception):
                    remote.detach()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--usboip", required=True, metavar="HOST:PORT")
    parser.add_argument("--ibec", required=True, type=existing_file)
    parser.add_argument("--ramdisk", required=True, type=existing_file)
    parser.add_argument("--device-tree", required=True, type=existing_file)
    parser.add_argument("--kernelcache", required=True, type=existing_file)
    parser.add_argument("--gdb", required=True, metavar="HOST:PORT")
    parser.add_argument(
        "--boot-args-address",
        type=lambda value: int(value, 0),
        default=DEFAULT_BOOT_ARGS_ADDRESS,
    )
    parser.add_argument(
        "--boot-args",
        default="rd=md0 nand-enable-reformat=1 serial=3",
    )
    parser.add_argument("--uart-log", type=Path)
    parser.add_argument("--kernel-timeout", type=float, default=30)
    parser.add_argument("--reconnect-timeout", type=float, default=30)
    parser.add_argument(
        "--resume-ibec",
        action="store_true",
        help=(
            "resume at an already-running iBEC stage after a failed USB "
            "re-enumeration instead of uploading iBEC through iBSS again"
        ),
    )
    parser.add_argument(
        "--usb-request-timeout",
        type=int,
        metavar="MILLISECONDS",
        help=(
            "override each USB-over-IP control and bulk request timeout; "
            "useful for instruction-counted single-threaded TCG"
        ),
    )
    parser.add_argument(
        "--ios2-control-upload",
        action="store_true",
        help="use the DFU-compatible control upload used by iPhone OS 2",
    )
    parser.add_argument(
        "--wait-before-bootx",
        action="store_true",
        help="wait for Enter after loading the kernel so a debugger can attach",
    )
    parser.add_argument(
        "--workaround-ios4-restore-crypto-keys",
        action="store_true",
        help=(
            "authenticate the 8C148 crypto constructor and preserve its six "
            "selectors for this restore-ramdisk boot"
        ),
    )
    parser.add_argument(
        "--hacktivate-ios4",
        action="store_true",
        help=(
            "authenticate and apply the 8C148 Legacy iOS Kit lockdownd "
            "branch in memory before its first execution"
        ),
    )
    parser.add_argument(
        "--hide-unmodeled-baseband",
        action="store_true",
        help=(
            "authenticate the staged 8C148 DeviceTree and unmatch only the "
            "cellular baseband plus its SPI2 transport"
        ),
    )
    parser.add_argument(
        "--hacktivation-timeout",
        type=float,
        default=300,
        metavar="SECONDS",
        help="wait bound for the first 8C148 lockdownd execution",
    )
    parser.add_argument(
        "--install-ios4-data-ark",
        action="store_true",
        help=(
            "boot the authenticated 8C148 restore ramdisk and persist the "
            "Legacy factory-activation record on disk0s2"
        ),
    )
    parser.add_argument(
        "--data-ark-timeout",
        type=float,
        default=300,
        metavar="SECONDS",
        help="wait bound for the 8C148 restored_external activation helper",
    )
    args = parser.parse_args()
    if args.usb_request_timeout is not None and args.usb_request_timeout <= 0:
        parser.error("--usb-request-timeout must be positive")
    if args.hacktivation_timeout <= 0:
        parser.error("--hacktivation-timeout must be positive")
    if args.data_ark_timeout <= 0:
        parser.error("--data-ark-timeout must be positive")
    if args.hacktivate_ios4 and args.workaround_ios4_restore_crypto_keys:
        parser.error(
            "--hacktivate-ios4 and --workaround-ios4-restore-crypto-keys "
            "both own bootx"
        )
    if args.install_ios4_data_ark and (
        args.hacktivate_ios4 or args.workaround_ios4_restore_crypto_keys
    ):
        parser.error(
            "--install-ios4-data-ark cannot share bootx with another "
            "runtime boot owner"
        )
    if args.hide_unmodeled_baseband and args.ios2_control_upload:
        parser.error("--hide-unmodeled-baseband currently supports only 8C148")

    usboip_endpoint = Endpoint.parse(args.usboip)
    gdb_endpoint = Endpoint.parse(args.gdb)
    adapter = IRecvAdapter(usboip_endpoint, args.usb_request_timeout)
    adapter.install()

    def retry_ios2_packet(current: object, packet_index: int) -> object:
        print(
            f"iOS 2 packet {packet_index} timed out; re-enumerating and retrying",
            flush=True,
        )
        return reconnect(
            usboip_endpoint,
            adapter,
            current,
            args.reconnect_timeout,
        )

    if not args.resume_ibec:
        print("waiting for iBSS", flush=True)
        client = connect(args.reconnect_timeout)
        try:
            ibss_transfer = (
                partial(send_ios2_buffer, reconnect=retry_ios2_packet)
                if args.ios2_control_upload
                else send_standard_buffer
            )
            client = upload(client, "iBEC", args.ibec, ibss_transfer)
            if args.ios2_control_upload:
                client = reconnect(
                    usboip_endpoint,
                    adapter,
                    client,
                    args.reconnect_timeout,
                )
            with contextlib.suppress(USBError):
                client.send_command(
                    "go", b_request=0 if args.ios2_control_upload else 1
                )
        finally:
            close(adapter, client)

        # The iBSS go request completes before authentication and the branch
        # into iBEC.  Give that handoff a bounded interval before assigning
        # this one reset to the new stage under single-threaded TCG.
        time.sleep(IBEC_HANDOFF_RESET_DELAY)
        print("waiting for iBEC re-enumeration", flush=True)
    else:
        print("resuming at existing iBEC", flush=True)
    reset_bus(
        usboip_endpoint,
        args.usb_request_timeout,
        discovery_timeout=args.reconnect_timeout,
    )
    client = connect(args.reconnect_timeout)
    try:
        ibec_transfer = (
            partial(send_ios2_ibec_buffer, reconnect=retry_ios2_packet)
            if args.ios2_control_upload
            else send_standard_buffer
        )
        client = upload(
            client,
            "restore ramdisk",
            args.ramdisk,
            ibec_transfer,
        )
        if args.ios2_control_upload:
            client = reconnect(
                usboip_endpoint,
                adapter,
                client,
                args.reconnect_timeout,
            )
            client = send_ios2_stage_command(
                usboip_endpoint,
                adapter,
                client,
                "ramdisk",
                args.reconnect_timeout,
            )
        else:
            client.send_command("ramdisk")

        client = upload(
            client,
            "DeviceTree",
            args.device_tree,
            ibec_transfer,
        )
        if args.ios2_control_upload:
            client = reconnect(
                usboip_endpoint,
                adapter,
                client,
                args.reconnect_timeout,
            )
            client = register_ios2_device_tree(
                usboip_endpoint,
                gdb_endpoint,
                adapter,
                client,
                img3_data_logical_size(args.device_tree),
                args.reconnect_timeout,
            )
        else:
            client.send_command("devicetree")

        if args.hide_unmodeled_baseband:
            unmatch_ios4_unmodeled_baseband(
                gdb_endpoint, img3_data_logical_size(args.device_tree)
            )

        client = upload(
            client,
            "kernelcache",
            args.kernelcache,
            ibec_transfer,
        )
        if args.ios2_control_upload:
            client = reconnect(
                usboip_endpoint,
                adapter,
                client,
                args.reconnect_timeout,
            )
        patch_boot_args(gdb_endpoint, args.boot_args_address, args.boot_args)
        uart_offset = (
            args.uart_log.stat().st_size
            if args.uart_log is not None and args.uart_log.exists()
            else 0
        )
        if args.wait_before_bootx:
            input("kernel loaded; attach the debugger, then press Enter for bootx: ")
        if args.workaround_ios4_restore_crypto_keys:
            if not args.boot_args.startswith("rd=md"):
                raise ValueError(
                    "the iOS 4 restore crypto workaround requires an md root"
                )
            boot_ios4_with_restore_crypto_keys(
                gdb_endpoint, client, args.kernel_timeout
            )
        elif args.hacktivate_ios4:
            if not args.boot_args.startswith("rd=disk0s1"):
                raise ValueError(
                    "iOS 4 hacktivation requires the installed system root"
                )
            boot_ios4_hacktivated(
                gdb_endpoint, client, args.hacktivation_timeout
            )
        elif args.install_ios4_data_ark:
            if not args.boot_args.startswith("rd=md"):
                raise ValueError(
                    "the iOS 4 data-ark installer requires a restore ramdisk root"
                )
            boot_ios4_install_data_ark(
                gdb_endpoint, client, args.data_ark_timeout
            )
        else:
            with contextlib.suppress(USBError):
                client.send_command("bootx")
    finally:
        close(adapter, client)

    if args.uart_log is not None:
        wait_for_kernel_uart(args.uart_log, uart_offset, args.kernel_timeout)
    print("bootx submitted; follow the UART log with make -C build iphone3g-log")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
