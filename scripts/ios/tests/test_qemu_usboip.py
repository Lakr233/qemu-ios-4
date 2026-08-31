from __future__ import annotations

import socket
import subprocess
import sys
import tempfile
import time
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
QEMU = ROOT / "build/qemu-system-arm"
sys.path.insert(0, str(ROOT / "scripts/ios"))

from usboip import (  # noqa: E402
    CONTROL_REQUEST,
    DEVICE_DESCRIPTOR,
    Device,
    Endpoint,
    IRecvAdapter,
    Opcode,
)


USB_BASE = 0x38400000
USB_GAHBCFG = USB_BASE + 0x008
USB_GINTSTS = USB_BASE + 0x014
USB_IN_EP = USB_BASE + 0x900
USB_OUT_EP = USB_BASE + 0xB00
EP_STRIDE = 0x20
EP_CONTROL = 0x00
EP_INTERRUPT = 0x08
EP_TRANSFER_SIZE = 0x10
EP_DMA_ADDRESS = 0x14
EP_TRANSFER_BYTES = (1 << 19) - 1
EP_PACKET_COUNT = 0x3FF << 19
EP_SETUP_COUNT = 3 << 29
EP_ENABLE = 1 << 31
EP_CLEAR_NAK = 1 << 26
EP_ACTIVE = 1 << 15
USB_RESET = 1 << 12
USB_ENUM_DONE = 1 << 13


class USBOverIPUnitTests(unittest.TestCase):
    def test_device_request_timeout_override_reaches_wire_and_socket(self) -> None:
        class FakeTransport:
            def __init__(self) -> None:
                self.requests: list[tuple[Opcode, bytes, int | None]] = []

            def request(
                self,
                opcode: Opcode,
                payload: bytes = b"",
                timeout_ms: int | None = None,
            ) -> bytes:
                self.requests.append((opcode, payload, timeout_ms))
                if opcode == Opcode.ENUMERATE:
                    return DEVICE_DESCRIPTOR.pack(0x05AC, 0x1281, 0)
                return b""

            def finish_enumeration(self) -> None:
                pass

            def close(self) -> None:
                pass

        transport = FakeTransport()
        device = Device(transport, request_timeout_ms=120_000)  # type: ignore[arg-type]
        self.assertEqual(
            bytes(device.ctrl_transfer(0xA1, 5, data_or_wLength=0)), b""
        )
        opcode, payload, socket_timeout = transport.requests[-1]
        self.assertEqual(opcode, Opcode.CONTROL)
        self.assertEqual(CONTROL_REQUEST.unpack(payload)[4], 120_000)
        self.assertEqual(socket_timeout, 120_000)


class QTestConnection:
    def __init__(self, path: Path, process: subprocess.Popen[str]):
        self.socket = socket.socket(socket.AF_UNIX)
        deadline = time.monotonic() + 5
        try:
            while True:
                if process.poll() is not None:
                    assert process.stderr is not None
                    raise RuntimeError(process.stderr.read())
                try:
                    self.socket.connect(str(path))
                    break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(0.01)
        except Exception:
            self.socket.close()
            raise
        self.stream = self.socket.makefile("rwb", buffering=0)

    def close(self) -> None:
        self.stream.close()
        self.socket.close()

    def command(self, command: str) -> str:
        self.stream.write(command.encode("ascii") + b"\n")
        while True:
            response = self.stream.readline().decode("ascii").strip()
            if response.startswith("OK"):
                return response
            if not response.startswith("IRQ"):
                raise AssertionError(f"qtest command failed: {response}")

    def writel(self, address: int, value: int) -> None:
        self.command(f"writel 0x{address:x} 0x{value:x}")

    def readl(self, address: int) -> int:
        return int.from_bytes(self.read(address, 4), "little")

    def write(self, address: int, data: bytes) -> None:
        self.command(f"write 0x{address:x} 0x{len(data):x} 0x{data.hex()}")

    def read(self, address: int, size: int) -> bytes:
        response = self.command(f"read 0x{address:x} 0x{size:x}")
        return bytes.fromhex(response.split()[1][2:])

    def prepare_endpoint(
        self,
        base: int,
        endpoint: int,
        address: int,
        size: int,
        *,
        setup: bool = False,
        max_packet: int = 64,
    ) -> None:
        registers = base + endpoint * EP_STRIDE
        packet_count = max(1, (size + max_packet - 1) // max_packet)
        transfer_size = size | (packet_count << 19)
        if setup:
            transfer_size |= 1 << 29
        self.writel(registers + EP_DMA_ADDRESS, address)
        self.writel(registers + EP_TRANSFER_SIZE, transfer_size)
        self.writel(
            registers + EP_CONTROL,
            EP_ACTIVE | EP_ENABLE | EP_CLEAR_NAK | max_packet,
        )

    def clear_endpoint_interrupt(self, base: int, endpoint: int) -> None:
        self.writel(base + endpoint * EP_STRIDE + EP_INTERRUPT, 0xFFFFFFFF)

    def endpoint_transfer_size(self, base: int, endpoint: int) -> int:
        return self.readl(
            base + endpoint * EP_STRIDE + EP_TRANSFER_SIZE
        )

    def wait_for(self, address: int, expected: bytes) -> None:
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline:
            if self.read(address, len(expected)) == expected:
                return
            time.sleep(0.005)
        raise AssertionError(f"DMA data did not reach 0x{address:x}")


@unittest.skipUnless(QEMU.is_file(), "build/qemu-system-arm is not built")
class QemuUSBOverIPTests(unittest.TestCase):
    def test_pinned_irecv_enumerates_qemu_device(self) -> None:
        import pymobiledevice3.irecv as irecv

        probe = socket.socket()
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
        probe.close()

        tempdir = tempfile.TemporaryDirectory()
        qtest_path = Path(tempdir.name) / "qtest.sock"
        process = subprocess.Popen(
            [
                str(QEMU),
                "-machine",
                "iphone3g",
                "-accel",
                "qtest",
                "-display",
                "none",
                "-audio",
                "none",
                "-qtest",
                f"unix:{qtest_path},server=on,wait=off",
                "-chardev",
                (
                    "socket,id=usboip,host=127.0.0.1,"
                    f"port={port},server=on,wait=off,nodelay=on"
                ),
                "-global",
                "s5l8900-usb.chardev=usboip",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        original = (irecv.find, irecv.get_string, irecv.dispose_resources)
        client = None
        qtest = None
        try:
            qtest = QTestConnection(qtest_path, process)
            IRecvAdapter(Endpoint("127.0.0.1", port)).install()
            setup_address = 0x00100000
            data_address = 0x00101000
            qtest.prepare_endpoint(
                USB_OUT_EP, 0, setup_address, 8, setup=True
            )
            with ThreadPoolExecutor(max_workers=1) as executor:
                connect = executor.submit(irecv.IRecv, timeout=1)
                qtest.wait_for(
                    setup_address,
                    b"\x00\x09\x01\x00\x00\x00\x00\x00",
                )
                self.assertEqual(
                    qtest.endpoint_transfer_size(USB_OUT_EP, 0)
                    & (EP_TRANSFER_BYTES | EP_PACKET_COUNT | EP_SETUP_COUNT),
                    0,
                )
                qtest.prepare_endpoint(USB_IN_EP, 0, data_address, 0)
                client = connect.result(timeout=1)

            self.assertIsNotNone(client, "timed out waiting for QEMU")
            assert client is not None
            self.assertEqual(client.chip_id, 0x8900)
            self.assertEqual(client.board_id, 4)
            self.assertEqual(client.product_type, "iPhone1,2")
            self.assertEqual(
                client.device.get_active_configuration().bConfigurationValue,
                1,
            )
            self.assertEqual(client.device.idProduct, 0x1281)
            self.assertIsNone(client.device.set_configuration(1))

            # Dropping the framed Host transport does not remove VBUS or reset
            # the USB bus.  A newly discovered pinned IRecv handle replays the
            # active configuration/interface and must not need fresh Guest EP0
            # buffers to attach to that same configured device.
            client.device.close()
            client = irecv.IRecv(timeout=2)
            self.assertEqual(
                client.device.get_active_configuration().bConfigurationValue,
                1,
            )

            qtest.clear_endpoint_interrupt(USB_IN_EP, 0)
            qtest.clear_endpoint_interrupt(USB_OUT_EP, 0)
            qtest.prepare_endpoint(
                USB_OUT_EP, 0, setup_address, 8, setup=True
            )
            with ThreadPoolExecutor(max_workers=1) as executor:
                control_in = executor.submit(
                    client.device.ctrl_transfer,
                    0xA1,
                    5,
                    0,
                    0,
                    1,
                    1_000,
                )
                qtest.wait_for(
                    setup_address,
                    b"\xa1\x05\x00\x00\x00\x00\x01\x00",
                )
                self.assertEqual(
                    qtest.endpoint_transfer_size(USB_OUT_EP, 0)
                    & (EP_TRANSFER_BYTES | EP_PACKET_COUNT | EP_SETUP_COUNT),
                    0,
                )
                qtest.write(data_address, b"\x02")
                qtest.prepare_endpoint(USB_IN_EP, 0, data_address, 1)
                self.assertEqual(bytes(control_in.result(timeout=1)), b"\x02")

                qtest.clear_endpoint_interrupt(USB_IN_EP, 0)
                qtest.clear_endpoint_interrupt(USB_OUT_EP, 0)
                qtest.prepare_endpoint(
                    USB_OUT_EP, 0, setup_address, 8, setup=True
                )
                control_out = executor.submit(
                    client.device.ctrl_transfer,
                    0x21,
                    1,
                    3,
                    0,
                    b"abcd",
                    1_000,
                )
                qtest.wait_for(
                    setup_address,
                    b"\x21\x01\x03\x00\x00\x00\x04\x00",
                )
                self.assertEqual(
                    qtest.endpoint_transfer_size(USB_OUT_EP, 0)
                    & (EP_TRANSFER_BYTES | EP_PACKET_COUNT | EP_SETUP_COUNT),
                    0,
                )
                qtest.prepare_endpoint(USB_OUT_EP, 0, data_address, 4)
                qtest.wait_for(data_address, b"abcd")
                self.assertEqual(
                    qtest.endpoint_transfer_size(USB_OUT_EP, 0)
                    & (EP_TRANSFER_BYTES | EP_PACKET_COUNT),
                    0,
                )
                qtest.prepare_endpoint(USB_IN_EP, 0, data_address, 0)
                self.assertEqual(control_out.result(timeout=1), 4)

                bulk_out = executor.submit(
                    client.device.write, 4, b"bulk", 1_000
                )
                qtest.prepare_endpoint(USB_OUT_EP, 4, data_address, 4)
                self.assertEqual(bulk_out.result(timeout=1), 4)
                self.assertEqual(qtest.read(data_address, 4), b"bulk")

                zero_length_out = executor.submit(
                    client.device.write, 4, b"", 1_000
                )
                time.sleep(0.01)
                self.assertFalse(zero_length_out.done())
                qtest.prepare_endpoint(USB_OUT_EP, 4, data_address, 64)
                self.assertEqual(zero_length_out.result(timeout=1), 0)
                self.assertEqual(
                    qtest.endpoint_transfer_size(USB_OUT_EP, 4)
                    & EP_TRANSFER_BYTES,
                    64,
                )

                qtest.write(data_address, b"reply")
                bulk_in = executor.submit(client.device.read, 0x81, 5, 1_000)
                qtest.prepare_endpoint(USB_IN_EP, 1, data_address, 5)
                self.assertEqual(bytes(bulk_in.result(timeout=1)), b"reply")

                packet = b"z" * 64
                qtest.write(data_address, packet)
                bulk_in = executor.submit(client.device.read, 0x81, 128, 1_000)
                qtest.prepare_endpoint(USB_IN_EP, 1, data_address, len(packet))
                time.sleep(0.01)
                self.assertFalse(bulk_in.done())
                qtest.clear_endpoint_interrupt(USB_IN_EP, 1)
                qtest.prepare_endpoint(USB_IN_EP, 1, data_address, 0)
                self.assertEqual(bytes(bulk_in.result(timeout=1)), packet)

                qtest.clear_endpoint_interrupt(USB_IN_EP, 1)
                mux_frame = b"m" * 32_764
                qtest.command(
                    f"memset 0x{data_address:x} 0x{len(mux_frame):x} 0x6d"
                )
                qtest.prepare_endpoint(
                    USB_IN_EP,
                    1,
                    data_address,
                    len(mux_frame),
                    max_packet=512,
                )
                first = bytes(client.device.read(0x81, 16_384, 1_000))
                self.assertEqual(first, mux_frame[:16_384])
                self.assertEqual(
                    qtest.endpoint_transfer_size(USB_IN_EP, 1)
                    & EP_TRANSFER_BYTES,
                    16_380,
                )
                self.assertEqual(
                    qtest.readl(USB_IN_EP + EP_STRIDE + EP_DMA_ADDRESS),
                    data_address + 16_384,
                )
                self.assertTrue(
                    qtest.readl(USB_IN_EP + EP_STRIDE + EP_CONTROL)
                    & EP_ENABLE
                )
                second = bytes(client.device.read(0x81, 16_384, 1_000))
                self.assertEqual(first + second, mux_frame)
                self.assertEqual(
                    qtest.endpoint_transfer_size(USB_IN_EP, 1)
                    & EP_TRANSFER_BYTES,
                    0,
                )

                qtest.clear_endpoint_interrupt(USB_IN_EP, 0)
                qtest.clear_endpoint_interrupt(USB_OUT_EP, 0)
                qtest.prepare_endpoint(
                    USB_OUT_EP, 0, setup_address, 8, setup=True
                )
                set_configuration = executor.submit(
                    client.device.set_configuration, 4
                )
                qtest.wait_for(
                    setup_address,
                    b"\x00\x09\x04\x00\x00\x00\x00\x00",
                )
                qtest.prepare_endpoint(USB_IN_EP, 0, data_address, 0)
                self.assertIsNone(set_configuration.result(timeout=1))
                self.assertEqual(
                    client.device.get_active_configuration().bConfigurationValue,
                    4,
                )
                # A Host transport reconnect is not a USB bus reset.  Pinned
                # IRecv configures every newly discovered PyUSB-shaped handle,
                # so replaying the already active value must be idempotent and
                # must not wait for another Guest endpoint-zero transaction.
                self.assertIsNone(client.device.set_configuration(4))

            qtest.writel(USB_GINTSTS, USB_RESET | USB_ENUM_DONE)
            qtest.writel(USB_GAHBCFG, 1)
            self.assertNotEqual(
                qtest.readl(USB_OUT_EP + EP_INTERRUPT), 0
            )
            out_size = qtest.readl(USB_OUT_EP + EP_TRANSFER_SIZE)
            out_dma = qtest.readl(USB_OUT_EP + EP_DMA_ADDRESS)
            client.device.reset()
            self.assertEqual(qtest.readl(USB_GAHBCFG), 1)
            self.assertEqual(qtest.readl(USB_OUT_EP + EP_INTERRUPT), 0)
            self.assertEqual(
                qtest.readl(USB_OUT_EP + EP_TRANSFER_SIZE), out_size
            )
            self.assertEqual(qtest.readl(USB_OUT_EP + EP_DMA_ADDRESS), out_dma)
            self.assertEqual(
                qtest.readl(USB_GINTSTS) & (USB_RESET | USB_ENUM_DONE),
                USB_RESET | USB_ENUM_DONE,
            )
        finally:
            if client is not None:
                client.device.close()
            if qtest is not None:
                qtest.close()
            irecv.find, irecv.get_string, irecv.dispose_resources = original
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            assert process.stderr is not None
            process.stderr.close()
            tempdir.cleanup()


if __name__ == "__main__":
    unittest.main()
