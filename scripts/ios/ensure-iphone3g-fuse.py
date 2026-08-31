#!/usr/bin/env python3
"""Create or validate one persistent 16-byte virtual iPhone fuse key."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import secrets
from typing import Callable


FUSE_KEY_SIZE = 16


def validate_fuse_key(path: Path, label: str) -> bytes:
    key = path.read_bytes()
    if len(key) != FUSE_KEY_SIZE:
        raise ValueError(
            f"{path}: {label} key must contain exactly {FUSE_KEY_SIZE} bytes"
        )
    return key


def ensure_fuse_key(
    path: Path,
    label: str,
    generate: Callable[[int], bytes] = secrets.token_bytes,
) -> bool:
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        descriptor = os.open(
            path,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL,
            0o600,
        )
    except FileExistsError:
        validate_fuse_key(path, label)
        return False

    try:
        key = generate(FUSE_KEY_SIZE)
        if len(key) != FUSE_KEY_SIZE:
            raise ValueError(f"{label} key generator returned an invalid length")
        with os.fdopen(descriptor, "wb") as stream:
            descriptor = -1
            stream.write(key)
            stream.flush()
            os.fsync(stream.fileno())
    except BaseException:
        if descriptor >= 0:
            os.close(descriptor)
        path.unlink(missing_ok=True)
        raise
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("label", choices=("gid", "uid"))
    parser.add_argument("path", type=Path)
    args = parser.parse_args()
    try:
        created = ensure_fuse_key(args.path, args.label)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(
        f"{args.label}_key={'created' if created else 'preserved'} "
        f"path={args.path}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
