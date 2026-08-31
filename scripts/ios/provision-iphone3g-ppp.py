#!/usr/bin/env python3
"""Enable or disable the iPhone 3G development PPP launchd job in NAND.

The restored 8C148 root filesystem contains an inert 530-byte pilotfish job.
This tool replaces only those 530 bytes with an equal-sized pppd job.  It does
not edit HFS metadata or NAND spare data and refuses ambiguous FTL histories.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from dataclasses import dataclass
from pathlib import Path


DATA_SIZE = 4096
SPARE_SIZE = 216
PAGE_SIZE = DATA_SIZE + SPARE_SIZE
PAGE_COUNT = 128 * 4096 * 4
IMAGE_SIZE = PAGE_SIZE * PAGE_COUNT
SCAN_PAGES = 1024
USER_PAGE_TYPES = (0x40, 0x41)

STOCK_JOB = b"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple Inc.//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
        <key>Label</key>
        <string>com.apple.chud.pilotfish</string>
        <key>MachServices</key>
        <dict>
                <key>com.apple.chud.pilotfish</key>
\t\t\t\t<true/>
        </dict>
        <key>ProgramArguments</key>
        <array>
                <string>/Developer/usr/libexec/pilotfish</string>
        </array>
</dict>
</plist>
"""

# Keep this compact so the options needed for a useful routed link still fit
# inside the stock extent.  Inter-element spaces at the end are XML whitespace.
_PPP_JOB_BODY = b"""<?xml version="1.0"?><plist version="1.0"><dict><key>Label</key><string>com.apple.chud.pilotfish</string><key>RunAtLoad</key><true/><key>StandardOutPath</key><string>/dev/console</string><key>ProgramArguments</key><array><string>/usr/sbin/pppd</string><string>/dev/tty.debug</string><string>local</string><string>nocrtscts</string><string>nodetach</string><string>noauth</string><string>defaultroute</string><string>usepeerdns</string><string>persist</string><string>115200</string></array></dict></plist>"""
PPP_JOB = _PPP_JOB_BODY + b" " * (529 - len(_PPP_JOB_BODY)) + b"\n"

if len(STOCK_JOB) != 530 or len(PPP_JOB) != len(STOCK_JOB):
    raise RuntimeError("PPP launchd records must both be exactly 530 bytes")


@dataclass(frozen=True)
class Match:
    offset: int
    physical_page: int
    logical_page: int
    state: str


def _write_all(fd: int, data: bytes, offset: int) -> None:
    written = 0
    while written < len(data):
        count = os.pwrite(fd, data[written:], offset + written)
        if count <= 0:
            raise OSError(f"short write at offset {offset + written}")
        written += count


def _scan_matches(fd: int, size: int) -> list[Match]:
    matches: list[Match] = []
    chunk_size = PAGE_SIZE * SCAN_PAGES
    for chunk_offset in range(0, size, chunk_size):
        length = min(chunk_size, size - chunk_offset)
        chunk = os.pread(fd, length, chunk_offset)
        if len(chunk) != length:
            raise OSError(f"short NAND read at offset {chunk_offset}")
        for page_offset in range(0, length, PAGE_SIZE):
            spare = page_offset + DATA_SIZE
            if chunk[spare + 9] not in USER_PAGE_TYPES:
                continue
            data = chunk[page_offset:spare]
            for state, pattern in (("disabled", STOCK_JOB), ("enabled", PPP_JOB)):
                start = 0
                while True:
                    position = data.find(pattern, start)
                    if position < 0:
                        break
                    physical_page = (chunk_offset + page_offset) // PAGE_SIZE
                    matches.append(
                        Match(
                            offset=chunk_offset + page_offset + position,
                            physical_page=physical_page,
                            logical_page=int.from_bytes(
                                chunk[spare:spare + 4], "little"
                            ),
                            state=state,
                        )
                    )
                    start = position + 1
    return matches


def _logical_page_versions(fd: int, size: int, logical_page: int) -> list[int]:
    versions: list[int] = []
    chunk_size = PAGE_SIZE * SCAN_PAGES
    encoded = logical_page.to_bytes(4, "little")
    for chunk_offset in range(0, size, chunk_size):
        length = min(chunk_size, size - chunk_offset)
        chunk = os.pread(fd, length, chunk_offset)
        if len(chunk) != length:
            raise OSError(f"short NAND read at offset {chunk_offset}")
        for page_offset in range(0, length, PAGE_SIZE):
            spare = page_offset + DATA_SIZE
            if (
                chunk[spare + 9] in USER_PAGE_TYPES
                and chunk[spare:spare + 4] == encoded
            ):
                versions.append((chunk_offset + page_offset) // PAGE_SIZE)
    return versions


def provision(path: Path, enabled: bool) -> Match:
    fd = os.open(path, os.O_RDWR)
    try:
        size = os.fstat(fd).st_size
        if size != IMAGE_SIZE:
            raise ValueError(
                f"{path}: expected {IMAGE_SIZE} bytes, found {size}"
            )
        matches = _scan_matches(fd, size)
        if len(matches) != 1:
            detail = ", ".join(
                f"{match.state}@0x{match.offset:x}" for match in matches
            ) or "none"
            raise ValueError(
                "expected exactly one authenticated pilotfish record; "
                f"found {len(matches)} ({detail})"
            )
        match = matches[0]
        versions = _logical_page_versions(fd, size, match.logical_page)
        if versions != [match.physical_page]:
            raise ValueError(
                f"logical page {match.logical_page} has physical versions "
                f"{versions}; refusing in-place FTL edit"
            )

        target_state = "enabled" if enabled else "disabled"
        if match.state != target_state:
            replacement = PPP_JOB if enabled else STOCK_JOB
            _write_all(fd, replacement, match.offset)
            os.fsync(fd)
            actual = os.pread(fd, len(replacement), match.offset)
            if actual != replacement:
                raise OSError("NAND verification failed after PPP job write")
            match = Match(
                match.offset,
                match.physical_page,
                match.logical_page,
                target_state,
            )
        return match
    finally:
        os.close(fd)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("enable", "disable", "status"))
    parser.add_argument("nand", type=Path)
    args = parser.parse_args()

    if args.mode == "status":
        fd = os.open(args.nand, os.O_RDONLY)
        try:
            size = os.fstat(fd).st_size
            if size != IMAGE_SIZE:
                raise ValueError(
                    f"{args.nand}: expected {IMAGE_SIZE} bytes, found {size}"
                )
            matches = _scan_matches(fd, size)
            if len(matches) != 1:
                raise ValueError(f"expected one record, found {len(matches)}")
            match = matches[0]
        finally:
            os.close(fd)
    else:
        match = provision(args.nand, args.mode == "enable")

    record = PPP_JOB if match.state == "enabled" else STOCK_JOB
    print(
        f"state={match.state} offset=0x{match.offset:x} "
        f"physical_page={match.physical_page} logical_page={match.logical_page} "
        f"sha256={hashlib.sha256(record).hexdigest()}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
