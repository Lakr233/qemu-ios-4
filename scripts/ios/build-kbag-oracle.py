#!/usr/bin/env python3
"""Build an S5L8900 GID KBAG oracle from signed IMG3 files."""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
from pathlib import Path


IMG3_HEADER = struct.Struct("<4sIII4s")
TAG_HEADER = struct.Struct("<4sII")
BUNDLE_HEADER = struct.Struct("<8sI")
RECORD_HEADER = struct.Struct("<I")
IMG3_MAGIC = b"3gmI"
KBAG_MAGIC = b"GABK"
BUNDLE_MAGIC = b"S5KBG01\0"
VALID_RECORD_LENGTHS = (32, 40, 48)


def read_exact(stream, size: int, path: Path) -> bytes:
    data = stream.read(size)
    if len(data) != size:
        raise ValueError(f"{path}: unexpected end of file")
    return data


def decode_hex(value: str, name: str, lengths: tuple[int, ...]) -> bytes:
    try:
        decoded = bytes.fromhex(value)
    except ValueError as error:
        raise ValueError(f"{name} is not hexadecimal") from error
    if len(decoded) not in lengths:
        expected = ", ".join(str(length) for length in lengths)
        raise ValueError(f"{name} must contain {expected} bytes")
    return decoded


def extract_wrapped_kbag(path: Path) -> bytes:
    with path.open("rb") as stream:
        file_size = os.fstat(stream.fileno()).st_size
        header = read_exact(stream, IMG3_HEADER.size, path)
        magic, full_size, payload_size, signature_offset, _ = (
            IMG3_HEADER.unpack(header)
        )
        if magic != IMG3_MAGIC or full_size != file_size:
            raise ValueError(f"{path}: invalid IMG3 header")
        if not IMG3_HEADER.size <= signature_offset <= payload_size <= full_size:
            raise ValueError(f"{path}: invalid IMG3 bounds")

        offset = IMG3_HEADER.size
        wrapped: bytes | None = None
        while offset < full_size:
            if full_size - offset < TAG_HEADER.size:
                raise ValueError(f"{path}: truncated IMG3 tag header")
            stream.seek(offset)
            tag_magic, total_size, _ = TAG_HEADER.unpack(
                read_exact(stream, TAG_HEADER.size, path)
            )
            if total_size < TAG_HEADER.size or total_size > full_size - offset:
                raise ValueError(f"{path}: invalid IMG3 tag size")
            if tag_magic == KBAG_MAGIC:
                modifier, key_bits = struct.unpack(
                    "<II", read_exact(stream, 8, path)
                )
                record_length = 16 + key_bits // 8
                if key_bits % 8 or record_length not in VALID_RECORD_LENGTHS:
                    raise ValueError(f"{path}: unsupported KBAG key length")
                if modifier == 1:
                    if wrapped is not None:
                        raise ValueError(f"{path}: multiple production KBAGs")
                    if total_size < TAG_HEADER.size + 8 + record_length:
                        raise ValueError(f"{path}: truncated KBAG payload")
                    wrapped = read_exact(stream, record_length, path)
            offset += total_size

    if offset != full_size:
        raise ValueError(f"{path}: IMG3 tags do not cover the container")
    if wrapped is None:
        raise ValueError(f"{path}: no production KBAG")
    return wrapped


def build_bundle(manifest_path: Path, firmware_dir: Path) -> bytes:
    manifest = json.loads(manifest_path.read_text())
    components = manifest.get("components")
    if not isinstance(components, list) or not components:
        raise ValueError("manifest must contain a non-empty components array")

    records: list[tuple[bytes, bytes]] = []
    seen: set[bytes] = set()
    for index, component in enumerate(components):
        if not isinstance(component, dict):
            raise ValueError(f"component {index} is not an object")
        try:
            relative_path = Path(component["path"])
            iv = decode_hex(component["iv"], f"component {index} IV", (16,))
            key = decode_hex(
                component["key"], f"component {index} key", (16, 24, 32)
            )
        except KeyError as error:
            raise ValueError(
                f"component {index} is missing {error.args[0]}"
            ) from error
        if relative_path.is_absolute() or ".." in relative_path.parts:
            raise ValueError(f"component {index} path must stay in firmware-dir")
        wrapped = extract_wrapped_kbag(firmware_dir / relative_path)
        clear = iv + key
        if len(wrapped) != len(clear):
            raise ValueError(f"component {index} key length disagrees with KBAG")
        if wrapped in seen:
            raise ValueError(f"component {index} duplicates a KBAG")
        seen.add(wrapped)
        records.append((wrapped, clear))

    bundle = bytearray(BUNDLE_HEADER.pack(BUNDLE_MAGIC, len(records)))
    for wrapped, clear in records:
        bundle += RECORD_HEADER.pack(len(wrapped))
        bundle += wrapped
        bundle += clear
    return bytes(bundle)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("firmware_dir", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        if args.output.exists():
            raise FileExistsError(f"destination already exists: {args.output}")
        bundle = build_bundle(args.manifest, args.firmware_dir)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(bundle)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"KBAG oracle build failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
