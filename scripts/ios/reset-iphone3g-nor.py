#!/usr/bin/env python3
"""Create a valid erased iPhone 3G application-processor SPI NOR image."""

import argparse
import os
from pathlib import Path
import struct
import tempfile
import zlib


IMAGE_SIZE = 1 << 20
BANK_SIZE = 0x2000
BANK_OFFSETS = (0xFC000, 0xFE000)
HEADER_SIZE = 0x10
APPLE_HEADER_SIZE = 0x20
COMMON_PARTITION_SIZE = 0x800
PANIC_PARTITION_SIZE = 0x810

APPLE_HEADER_NAME = b"nvram"
COMMON_PARTITION_NAME = b"common"
PANIC_PARTITION_NAME = b"APL,OSXPanic"
FREE_PARTITION_NAME = b"wwwwwwwwwwww"

COMMON_VARIABLES = b"auto-boot=true\0boot-args=\0\0"


def header_checksum(header: bytes) -> int:
    checksum = header[0]
    for value in header[2:16]:
        checksum += value
        checksum = (checksum + (checksum >> 8)) & 0xFF
    return checksum


def format_partition(signature: int, size: int, name: bytes) -> bytearray:
    if not 0 <= signature <= 0xFF:
        raise ValueError("partition signature must fit in one byte")
    if (
        size < HEADER_SIZE
        or size % HEADER_SIZE
        or size // HEADER_SIZE > 0xFFFF
    ):
        raise ValueError("partition size must fit in a nonzero 16-bit block count")
    if len(name) > 12:
        raise ValueError("partition name must fit in 12 bytes")

    partition = bytearray(size)
    partition[0] = signature
    partition[2:4] = struct.pack("<H", size // HEADER_SIZE)
    partition[4:4 + len(name)] = name
    partition[1] = header_checksum(partition)
    return partition


def format_bank(generation: int) -> bytes:
    if not 0 < generation <= 0xFFFFFFFF:
        raise ValueError("generation must fit in a nonzero 32-bit value")

    free_partition_size = (
        BANK_SIZE
        - APPLE_HEADER_SIZE
        - COMMON_PARTITION_SIZE
        - PANIC_PARTITION_SIZE
    )
    partitions = (
        format_partition(0x5A, APPLE_HEADER_SIZE, APPLE_HEADER_NAME),
        format_partition(0x70, COMMON_PARTITION_SIZE, COMMON_PARTITION_NAME),
        format_partition(0xA1, PANIC_PARTITION_SIZE, PANIC_PARTITION_NAME),
        format_partition(0x7F, free_partition_size, FREE_PARTITION_NAME),
    )
    bank = bytearray().join(partitions)
    common_data_offset = APPLE_HEADER_SIZE + HEADER_SIZE
    if len(COMMON_VARIABLES) > COMMON_PARTITION_SIZE - HEADER_SIZE:
        raise ValueError("initial variables do not fit in the common partition")
    bank[
        common_data_offset:common_data_offset + len(COMMON_VARIABLES)
    ] = (
        COMMON_VARIABLES
    )
    bank[20:24] = struct.pack("<I", generation)
    bank[16:20] = struct.pack("<I", zlib.adler32(bank[20:]) & 0xFFFFFFFF)
    bank[1] = header_checksum(bank)
    return bytes(bank)


def reset_image(path: Path) -> None:
    image = bytearray(b"\xff" * IMAGE_SIZE)
    for generation, offset in enumerate(BANK_OFFSETS, start=1):
        image[offset:offset + BANK_SIZE] = format_bank(generation)

    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = tempfile.NamedTemporaryFile(
        dir=path.parent, prefix=f".{path.name}.", delete=False
    )
    temporary_path = Path(temporary.name)
    try:
        with temporary:
            temporary.write(image)
            temporary.flush()
            os.fsync(temporary.fileno())
        os.replace(temporary_path, path)
    except BaseException:
        temporary_path.unlink(missing_ok=True)
        raise


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    reset_image(args.image)
    print(f"initialized_banks={len(BANK_OFFSETS)} image_size={IMAGE_SIZE}")


if __name__ == "__main__":
    main()
