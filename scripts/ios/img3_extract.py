#!/usr/bin/env python3
"""Extract a logical DATA payload from an IMG3 container."""

from __future__ import annotations

import argparse
import os
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes


IMG3_HEADER = struct.Struct("<4sIII4s")
TAG_HEADER = struct.Struct("<4sII")
IMG3_MAGIC = b"3gmI"
DATA_MAGIC = b"ATAD"
BLOCK_SIZE = 16
CHUNK_SIZE = 1024 * 1024


@dataclass(frozen=True)
class DataTag:
    offset: int
    stored_size: int
    logical_size: int


def read_exact(stream: BinaryIO, size: int) -> bytes:
    data = stream.read(size)
    if len(data) != size:
        raise ValueError(f"unexpected end of file: wanted {size} bytes")
    return data


def find_data_tag(stream: BinaryIO, file_size: int) -> DataTag:
    header = read_exact(stream, IMG3_HEADER.size)
    magic, full_size, payload_size, signature_offset, _ = IMG3_HEADER.unpack(
        header
    )
    if magic != IMG3_MAGIC:
        raise ValueError(f"invalid IMG3 magic: {magic!r}")
    if full_size != file_size:
        raise ValueError(
            f"IMG3 size is {full_size}, but file size is {file_size}"
        )
    if not IMG3_HEADER.size <= signature_offset <= payload_size <= full_size:
        raise ValueError("invalid IMG3 root bounds")

    offset = IMG3_HEADER.size
    found: DataTag | None = None
    while offset < full_size:
        if full_size - offset < TAG_HEADER.size:
            raise ValueError("truncated IMG3 tag header")
        stream.seek(offset)
        magic, total_size, logical_size = TAG_HEADER.unpack(
            read_exact(stream, TAG_HEADER.size)
        )
        if total_size < TAG_HEADER.size:
            raise ValueError(f"invalid IMG3 tag size at 0x{offset:x}")
        end = offset + total_size
        if end > full_size:
            raise ValueError(f"IMG3 tag at 0x{offset:x} exceeds container")
        stored_size = total_size - TAG_HEADER.size
        if logical_size > stored_size:
            raise ValueError(
                f"IMG3 tag at 0x{offset:x} has an invalid logical size"
            )
        if magic == DATA_MAGIC:
            if found is not None:
                raise ValueError("IMG3 contains multiple DATA tags")
            found = DataTag(offset + TAG_HEADER.size, stored_size, logical_size)
        offset = end

    if offset != full_size:
        raise ValueError("IMG3 tags do not cover the container")
    if found is None:
        raise ValueError("IMG3 has no DATA tag")
    return found


def decode_hex(value: str, name: str, sizes: tuple[int, ...]) -> bytes:
    try:
        decoded = bytes.fromhex(value)
    except ValueError as error:
        raise ValueError(f"{name} is not hexadecimal") from error
    if len(decoded) not in sizes:
        expected = ", ".join(str(size) for size in sizes)
        raise ValueError(f"{name} must contain {expected} bytes")
    return decoded


def copy_payload(
    source: BinaryIO,
    destination: BinaryIO,
    tag: DataTag,
    key: bytes | None,
    iv: bytes | None,
) -> None:
    if (key is None) != (iv is None):
        raise ValueError("key and IV must be supplied together")
    decryptor = (
        Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
        if key is not None and iv is not None
        else None
    )
    source.seek(tag.offset)
    encrypted_remaining = (
        tag.stored_size - tag.stored_size % BLOCK_SIZE
        if decryptor is not None
        else 0
    )
    plain_remaining = tag.stored_size - encrypted_remaining
    logical_remaining = tag.logical_size
    while encrypted_remaining:
        chunk_size = min(CHUNK_SIZE, encrypted_remaining)
        chunk = read_exact(source, chunk_size)
        encrypted_remaining -= chunk_size
        decoded = decryptor.update(chunk)
        write_size = min(len(decoded), logical_remaining)
        destination.write(decoded[:write_size])
        logical_remaining -= write_size
    if decryptor is not None:
        final = decryptor.finalize()
        write_size = min(len(final), logical_remaining)
        destination.write(final[:write_size])
        logical_remaining -= write_size
    while plain_remaining:
        chunk_size = min(CHUNK_SIZE, plain_remaining)
        chunk = read_exact(source, chunk_size)
        plain_remaining -= chunk_size
        write_size = min(len(chunk), logical_remaining)
        destination.write(chunk[:write_size])
        logical_remaining -= write_size
    if logical_remaining:
        raise ValueError("IMG3 DATA storage is shorter than its logical size")


def extract_img3(
    source_path: Path,
    destination_path: Path,
    key: bytes | None = None,
    iv: bytes | None = None,
) -> None:
    if destination_path.exists():
        raise FileExistsError(f"destination already exists: {destination_path}")
    created = False
    try:
        with source_path.open("rb") as source:
            tag = find_data_tag(source, os.fstat(source.fileno()).st_size)
            with destination_path.open("xb") as destination:
                created = True
                copy_payload(source, destination, tag, key, iv)
    except Exception:
        if created:
            destination_path.unlink(missing_ok=True)
        raise


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--key")
    parser.add_argument("--iv")
    args = parser.parse_args()
    try:
        key = (
            decode_hex(args.key, "key", (16, 24, 32))
            if args.key is not None
            else None
        )
        iv = (
            decode_hex(args.iv, "IV", (BLOCK_SIZE,))
            if args.iv is not None
            else None
        )
        extract_img3(args.source, args.destination, key, iv)
    except (OSError, ValueError) as error:
        print(f"IMG3 extraction failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
