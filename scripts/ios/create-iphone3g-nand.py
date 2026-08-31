#!/usr/bin/env python3
"""Create one complete explicitly erased iPhone 3G raw NAND baseline."""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import stat
import sys
from pathlib import Path


PAGE_SIZE = 4096 + 216
PAGES_PER_BLOCK = 128
BLOCKS_PER_BANK = 4096
BANKS = 4
IMAGE_SIZE = PAGE_SIZE * PAGES_PER_BLOCK * BLOCKS_PER_BANK * BANKS
CHUNK_SIZE = 8 * 1024 * 1024
FREE_SPACE_MARGIN = 1024 * 1024 * 1024


def ensure_erased_nand(
    path: Path,
    image_size: int = IMAGE_SIZE,
    chunk_size: int = CHUNK_SIZE,
    free_space_margin: int = FREE_SPACE_MARGIN,
) -> str | None:
    if image_size <= 0 or chunk_size <= 0 or free_space_margin < 0:
        raise ValueError("NAND sizes must be positive and margin nonnegative")
    if path.exists():
        size = path.stat().st_size
        if size != image_size:
            raise ValueError(f"{path}: expected {image_size} bytes, found {size}")
        if path.stat().st_mode & stat.S_IWUSR:
            raise ValueError(f"{path}: erased NAND baseline must be read-only")
        return None

    path.parent.mkdir(parents=True, exist_ok=True)
    available = shutil.disk_usage(path.parent).free
    required = image_size + free_space_margin
    if available < required:
        raise OSError(
            f"insufficient free space for erased NAND: "
            f"{available} available, {required} required"
        )

    temporary = path.with_name(path.name + ".tmp")
    temporary.unlink(missing_ok=True)
    erased = b"\xff" * min(chunk_size, image_size)
    digest = hashlib.sha256()
    try:
        with temporary.open("xb") as stream:
            remaining = image_size
            while remaining:
                block = erased[: min(len(erased), remaining)]
                stream.write(block)
                digest.update(block)
                remaining -= len(block)
            stream.flush()
            os.fsync(stream.fileno())
        temporary.chmod(0o444)
        temporary.replace(path)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    try:
        digest = ensure_erased_nand(args.image)
    except (OSError, ValueError) as error:
        print(f"erased NAND creation failed: {error}", file=sys.stderr)
        return 1
    if digest is None:
        print(f"preserved erased NAND baseline: {args.image}")
    else:
        print(
            f"created erased NAND baseline: {args.image} "
            f"bytes={IMAGE_SIZE} sha256={digest}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
