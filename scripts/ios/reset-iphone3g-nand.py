#!/usr/bin/env python3
"""Restore modified pages in an iPhone 3G raw NAND image to erased state."""

import argparse
import os
from pathlib import Path


PAGE_SIZE = 4096 + 216
PAGES_PER_BLOCK = 128
BLOCKS_PER_BANK = 4096
BANKS = 4
IMAGE_SIZE = PAGE_SIZE * PAGES_PER_BLOCK * BLOCKS_PER_BANK * BANKS
SCAN_PAGES = 1024


def write_all(fd: int, data: bytes, offset: int) -> None:
    written = 0
    while written < len(data):
        count = os.pwrite(fd, data[written:], offset + written)
        if count <= 0:
            raise OSError(f"short write at offset {offset + written}")
        written += count


def reset_image(path: Path) -> int:
    erased_page = b"\xff" * PAGE_SIZE
    repaired = 0
    fd = os.open(path, os.O_RDWR)

    try:
        size = os.fstat(fd).st_size
        if size != IMAGE_SIZE:
            raise ValueError(
                f"{path}: expected {IMAGE_SIZE} bytes, found {size}"
            )

        chunk_size = PAGE_SIZE * SCAN_PAGES
        for offset in range(0, size, chunk_size):
            length = min(chunk_size, size - offset)
            chunk = os.pread(fd, length, offset)
            if len(chunk) != length:
                raise OSError(
                    f"short read at offset {offset}: {len(chunk)} of {length}"
                )
            if chunk.count(0xFF) == length:
                continue

            for page_offset in range(0, length, PAGE_SIZE):
                page = chunk[page_offset:page_offset + PAGE_SIZE]
                if page != erased_page:
                    write_all(fd, erased_page, offset + page_offset)
                    repaired += 1

        os.fsync(fd)
    finally:
        os.close(fd)
    return repaired


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    repaired = reset_image(args.image)
    print(f"repaired_pages={repaired} image_size={IMAGE_SIZE}")


if __name__ == "__main__":
    main()
