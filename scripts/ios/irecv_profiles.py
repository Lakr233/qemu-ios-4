"""Firmware-generation-specific irecv transfers."""

from __future__ import annotations

import binascii
import struct
import time
from collections.abc import Callable

from usboip import USBRequestTimeout


IOS2_DFU_PACKET_SIZE = 0x800
IOS2_IBEC_PACKET_SIZE = 0x2000
IOS2_CONTROL_TIMEOUT_MS = 60_000
IOS2_PACKET_RETRY_LIMIT = 3
IOS2_DFU_SUFFIX = bytes.fromhex("ffffffffac05000155464410")
IOS2_DFU_IDLE = 2
IOS2_DFU_DOWNLOAD_IDLE = 5
IOS2_DFU_WAIT_RESET = 8


def _ios2_dfu_tail(crc: int) -> bytes:
    crc = binascii.crc32(IOS2_DFU_SUFFIX, crc)
    return IOS2_DFU_SUFFIX + struct.pack("<I", crc)


def _wait_for_download_idle(
    client: object,
    sleep: Callable[[float], None],
) -> None:
    for attempt in range(21):
        if client.status == IOS2_DFU_DOWNLOAD_IDLE:
            return
        if attempt != 20:
            sleep(1)
    raise TimeoutError("iOS 2 DFU download did not become idle")


def send_ios2_buffer(
    client: object,
    data: bytes,
    *,
    packet_size: int = IOS2_DFU_PACKET_SIZE,
    show_progress: bool = True,
    sleep: Callable[[float], None] = time.sleep,
    reconnect: Callable[[object, int], object] | None = None,
) -> object:
    """Upload one iOS 2 recovery image through its DFU-compatible protocol."""
    if not data:
        raise ValueError("iOS 2 DFU image is empty")
    if packet_size <= 0 or packet_size > 0xFFFF:
        raise ValueError("iOS 2 DFU packet size is invalid")
    state = bytes(
        client.ctrl_transfer(
            0xA1,
            5,
            data_or_wLength=1,
            timeout=IOS2_CONTROL_TIMEOUT_MS,
        )
    )
    if len(state) != 1 or state[0] not in (IOS2_DFU_IDLE, IOS2_DFU_WAIT_RESET):
        raise RuntimeError(f"unexpected iOS 2 DFU state: {state!r}")

    packet_count = (len(data) + packet_size - 1) // packet_size
    offsets = range(0, len(data), packet_size)
    if show_progress:
        from tqdm import tqdm

        offsets = tqdm(
            offsets,
            total=packet_count,
            unit="packet",
            mininterval=1.0,
        )

    crc = -1
    for packet_index, offset in enumerate(offsets):
        chunk = data[offset : offset + packet_size]
        crc = binascii.crc32(chunk, crc)
        chunks = [chunk]
        if packet_index + 1 == packet_count:
            tail = _ios2_dfu_tail(crc)
            if len(chunk) + len(tail) > packet_size:
                chunks.append(tail)
            else:
                chunks[0] += tail
        for attempt in range(IOS2_PACKET_RETRY_LIMIT + 1):
            try:
                for part in chunks:
                    transferred = client.ctrl_transfer(
                        0x21,
                        1,
                        wValue=packet_index,
                        data_or_wLength=part,
                        timeout=IOS2_CONTROL_TIMEOUT_MS,
                    )
                    if transferred != len(part):
                        raise OSError("short iOS 2 DFU data transfer")
                break
            except USBRequestTimeout:
                if reconnect is None or attempt == IOS2_PACKET_RETRY_LIMIT:
                    raise
                client = reconnect(client, packet_index)

    _wait_for_download_idle(client, sleep)
    for attempt in range(IOS2_PACKET_RETRY_LIMIT + 1):
        try:
            client.ctrl_transfer(
                0x21,
                1,
                wValue=packet_count,
                data_or_wLength=b"",
                timeout=IOS2_CONTROL_TIMEOUT_MS,
            )
            break
        except USBRequestTimeout:
            if reconnect is None or attempt == IOS2_PACKET_RETRY_LIMIT:
                raise
            client = reconnect(client, packet_count)
    _ = client.status
    _ = client.status
    client.device.reset()
    return client


def send_ios2_ibec_buffer(
    client: object,
    data: bytes,
    *,
    show_progress: bool = True,
    reconnect: Callable[[object, int], object] | None = None,
) -> object:
    """Upload a 5A347 iBEC-stage image at its proven 8 KiB stride."""
    return send_ios2_buffer(
        client,
        data,
        packet_size=IOS2_IBEC_PACKET_SIZE,
        show_progress=show_progress,
        reconnect=reconnect,
    )
