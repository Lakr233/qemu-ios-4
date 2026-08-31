from __future__ import annotations

import binascii
import struct
import sys
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from irecv_profiles import (  # noqa: E402
    IOS2_CONTROL_TIMEOUT_MS,
    IOS2_DFU_SUFFIX,
    IOS2_IBEC_PACKET_SIZE,
    send_ios2_buffer,
    send_ios2_ibec_buffer,
)
from usboip import USBRequestTimeout  # noqa: E402


class FakeDevice:
    def __init__(self) -> None:
        self.reset_count = 0

    def reset(self) -> None:
        self.reset_count += 1


class FakeClient:
    def __init__(
        self,
        initial_state: int = 8,
        fail_packet: int | None = None,
    ) -> None:
        self.initial_state = initial_state
        self.device = FakeDevice()
        self.downloads: list[tuple[int, bytes]] = []
        self.status_reads = 0
        self.timeouts: list[int] = []
        self.fail_packet = fail_packet

    def ctrl_transfer(
        self,
        request_type: int,
        request: int,
        *,
        wValue: int = 0,
        data_or_wLength: object = None,
        timeout: int = 0,
    ) -> object:
        self.timeouts.append(timeout)
        if (request_type, request) == (0xA1, 5):
            return bytes((self.initial_state,))
        if (request_type, request) != (0x21, 1):
            raise AssertionError((request_type, request))
        if wValue == self.fail_packet:
            self.fail_packet = None
            raise USBRequestTimeout("injected timeout")
        data = bytes(data_or_wLength)
        self.downloads.append((wValue, data))
        return len(data)

    @property
    def status(self) -> int:
        self.status_reads += 1
        return 5


class IOS2IRecvProfileTests(unittest.TestCase):
    def test_small_image_appends_dfu_crc_and_notifies_finish(self) -> None:
        client = FakeClient()
        send_ios2_buffer(client, b"abc", show_progress=False)

        crc = binascii.crc32(b"abc", -1)
        crc = binascii.crc32(IOS2_DFU_SUFFIX, crc)
        self.assertEqual(
            client.downloads,
            [
                (0, b"abc" + IOS2_DFU_SUFFIX + struct.pack("<I", crc)),
                (1, b""),
            ],
        )
        self.assertEqual(client.status_reads, 3)
        self.assertEqual(client.device.reset_count, 1)
        self.assertEqual(
            client.timeouts,
            [IOS2_CONTROL_TIMEOUT_MS] * 3,
        )

    def test_full_packet_sends_crc_tail_at_same_block_index(self) -> None:
        client = FakeClient(initial_state=2)
        data = bytes(range(256)) * 8
        send_ios2_buffer(client, data, show_progress=False)

        self.assertEqual(client.downloads[0], (0, data))
        self.assertEqual(client.downloads[1][0], 0)
        self.assertEqual(len(client.downloads[1][1]), 16)
        self.assertEqual(client.downloads[2], (1, b""))
        self.assertEqual(client.status_reads, 3)

    def test_rejects_unexpected_initial_state(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "unexpected iOS 2 DFU state"):
            send_ios2_buffer(FakeClient(initial_state=3), b"abc", show_progress=False)

    def test_rejects_empty_image_before_usb_io(self) -> None:
        client = FakeClient()
        with self.assertRaisesRegex(ValueError, "image is empty"):
            send_ios2_buffer(client, b"", show_progress=False)
        self.assertEqual(client.downloads, [])

    def test_ibec_profile_uses_the_proven_eight_kib_stride(self) -> None:
        client = FakeClient(initial_state=2)
        data = bytes(range(256)) * 33

        send_ios2_ibec_buffer(client, data, show_progress=False)

        self.assertEqual(client.downloads[0], (0, data[:IOS2_IBEC_PACKET_SIZE]))
        self.assertEqual(client.downloads[1][0], 1)
        self.assertTrue(client.downloads[1][1].startswith(data[IOS2_IBEC_PACKET_SIZE:]))
        self.assertEqual(client.downloads[2], (2, b""))

    def test_reconnect_retries_the_same_indexed_packet(self) -> None:
        first = FakeClient(fail_packet=1)
        second = FakeClient(initial_state=5)
        reconnects: list[tuple[object, int]] = []

        def reconnect(client: object, packet_index: int) -> object:
            reconnects.append((client, packet_index))
            return second

        result = send_ios2_buffer(
            first,
            bytes(range(256)) * 16,
            show_progress=False,
            reconnect=reconnect,
        )

        self.assertIs(result, second)
        self.assertEqual(reconnects, [(first, 1)])
        self.assertEqual(first.downloads[0][0], 0)
        self.assertEqual(second.downloads[0][0], 1)
        self.assertEqual(second.downloads[-1], (2, b""))
        self.assertEqual(second.device.reset_count, 1)

    def test_rejects_packet_sizes_outside_control_length(self) -> None:
        for packet_size in (0, 0x10000):
            with self.subTest(packet_size=packet_size):
                with self.assertRaisesRegex(ValueError, "packet size is invalid"):
                    send_ios2_buffer(
                        FakeClient(),
                        b"data",
                        packet_size=packet_size,
                        show_progress=False,
                    )

if __name__ == "__main__":
    unittest.main()
