#!/usr/bin/env python3
"""Compare restored iPhone 3G NAND user pages with an HFS source image."""

from __future__ import annotations

import argparse
from array import array
from dataclasses import dataclass
import json
import mmap
import os
from pathlib import Path
from typing import Iterable


@dataclass(frozen=True)
class NANDGeometry:
    data_size: int = 4096
    spare_size: int = 216
    pages_per_block: int = 128
    blocks_per_bank: int = 4096
    banks: int = 4

    @property
    def page_size(self) -> int:
        return self.data_size + self.spare_size

    @property
    def page_count(self) -> int:
        return self.pages_per_block * self.blocks_per_bank * self.banks

    @property
    def image_size(self) -> int:
        return self.page_size * self.page_count


@dataclass(frozen=True)
class RestoreAnalysis:
    source_offset: int
    source_bytes: int
    source_pages: int
    ignored_source_tail_bytes: int
    physical_user_pages: int
    seen_source_pages: int
    exact_source_pages: int
    missing_source_ranges: tuple[tuple[int, int], ...]
    wrong_source_ranges: tuple[tuple[int, int], ...]
    multiple_version_pages: int
    maximum_versions: int
    lpn_base: int

    @property
    def complete(self) -> bool:
        return self.exact_source_pages == self.source_pages

    def as_dict(self) -> dict[str, object]:
        return {
            "complete": self.complete,
            "source_offset": self.source_offset,
            "source_bytes": self.source_bytes,
            "source_pages": self.source_pages,
            "ignored_source_tail_bytes": self.ignored_source_tail_bytes,
            "physical_user_pages": self.physical_user_pages,
            "seen_source_pages": self.seen_source_pages,
            "exact_source_pages": self.exact_source_pages,
            "missing_source_ranges": self.missing_source_ranges,
            "wrong_source_ranges": self.wrong_source_ranges,
            "multiple_version_pages": self.multiple_version_pages,
            "maximum_versions": self.maximum_versions,
            "lpn_base": self.lpn_base,
        }


def contiguous_ranges(indices: Iterable[int]) -> tuple[tuple[int, int], ...]:
    ranges: list[tuple[int, int]] = []
    start: int | None = None
    end: int | None = None

    for index in indices:
        if start is None:
            start = end = index
        elif index == end + 1:
            end = index
        else:
            ranges.append((start, end))
            start = end = index
    if start is not None:
        assert end is not None
        ranges.append((start, end))
    return tuple(ranges)


def analyze_restore(
    nand_path: Path,
    source_path: Path,
    *,
    source_offset: int = 0,
    source_bytes: int | None = None,
    lpn_base: int = 63,
    geometry: NANDGeometry = NANDGeometry(),
    scan_pages: int = 1024,
) -> RestoreAnalysis:
    if geometry.data_size <= 0 or geometry.spare_size < 10:
        raise ValueError(
            "NAND data must be nonempty and spare must contain type1"
        )
    if geometry.page_count <= 0 or scan_pages <= 0:
        raise ValueError("NAND geometry and scan_pages must be positive")
    # Some restored images omit leading container pages before publishing
    # LPN zero.  Represent that source-page bias with a negative base instead
    # of forcing callers to copy or truncate the authoritative source image.
    if lpn_base < -0xFFFFFFFF or lpn_base > 0xFFFFFFFF:
        raise ValueError("lpn_base must fit in signed source-page mapping")

    with nand_path.open("rb", buffering=0) as nand_file, source_path.open(
        "rb", buffering=0
    ) as source_file:
        nand_fd = nand_file.fileno()
        source_fd = source_file.fileno()
        nand_size = os.fstat(nand_fd).st_size
        if nand_size != geometry.image_size:
            raise ValueError(
                f"{nand_path}: expected {geometry.image_size} bytes, "
                f"found {nand_size}"
            )
        source_size = os.fstat(source_fd).st_size
        if source_offset < 0 or source_offset >= source_size:
            raise ValueError(
                f"source_offset must be between 0 and {source_size - 1}, "
                f"found {source_offset}"
            )
        if source_bytes is None:
            source_bytes = source_size - source_offset
        if source_bytes <= 0 or source_bytes > source_size - source_offset:
            raise ValueError(
                "source_bytes must fit after source_offset and be between "
                f"1 and {source_size - source_offset}, "
                f"found {source_bytes}"
            )
        source_pages, ignored_tail = divmod(source_bytes, geometry.data_size)
        if source_pages == 0:
            raise ValueError(
                "source_bytes does not contain one complete data page"
            )
        if lpn_base + source_pages > 0x100000000:
            raise ValueError("source logical-page interval exceeds uint32")

        mapped_bytes = source_pages * geometry.data_size
        map_offset = source_offset - source_offset % mmap.ALLOCATIONGRANULARITY
        map_prefix = source_offset - map_offset
        source_map = mmap.mmap(
            source_fd,
            map_prefix + mapped_bytes,
            access=mmap.ACCESS_READ,
            offset=map_offset,
        )
        try:
            versions = array("I", [0]) * source_pages
            exact = bytearray(source_pages)
            physical_user_pages = 0
            chunk_size = scan_pages * geometry.page_size

            for chunk_offset in range(0, nand_size, chunk_size):
                length = min(chunk_size, nand_size - chunk_offset)
                chunk = os.pread(nand_fd, length, chunk_offset)
                if len(chunk) != length:
                    raise OSError(
                        f"short NAND read at {chunk_offset}: "
                        f"{len(chunk)} of {length}"
                    )
                for page_offset in range(0, length, geometry.page_size):
                    spare_offset = page_offset + geometry.data_size
                    type1 = chunk[spare_offset + 9]
                    if type1 not in (0x40, 0x41):
                        continue
                    physical_user_pages += 1
                    lpn = int.from_bytes(
                        chunk[spare_offset : spare_offset + 4], "little"
                    )
                    source_index = lpn - lpn_base
                    if source_index < 0 or source_index >= source_pages:
                        continue
                    versions[source_index] += 1
                    source_page_offset = (
                        map_prefix + source_index * geometry.data_size
                    )
                    if chunk[
                        page_offset : page_offset + geometry.data_size
                    ] == source_map[
                        source_page_offset
                        : source_page_offset + geometry.data_size
                    ]:
                        exact[source_index] = 1
        finally:
            source_map.close()

        missing = contiguous_ranges(
            index for index, count in enumerate(versions) if count == 0
        )
        wrong = contiguous_ranges(
            index
            for index, count in enumerate(versions)
            if count and not exact[index]
        )
        return RestoreAnalysis(
            source_offset=source_offset,
            source_bytes=source_bytes,
            source_pages=source_pages,
            ignored_source_tail_bytes=ignored_tail,
            physical_user_pages=physical_user_pages,
            seen_source_pages=sum(count != 0 for count in versions),
            exact_source_pages=sum(exact),
            missing_source_ranges=missing,
            wrong_source_ranges=wrong,
            multiple_version_pages=sum(count > 1 for count in versions),
            maximum_versions=max(versions, default=0),
            lpn_base=lpn_base,
        )


def format_ranges(ranges: tuple[tuple[int, int], ...]) -> str:
    if not ranges:
        return "none"
    return ",".join(
        str(start) if start == end else f"{start}..{end}"
        for start, end in ranges
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nand", required=True, type=Path)
    parser.add_argument("--source-hfs", required=True, type=Path)
    parser.add_argument(
        "--source-offset",
        type=int,
        default=0,
        help="byte offset of the HFS partition inside the source image",
    )
    parser.add_argument(
        "--source-bytes",
        type=int,
        help="compare only this transferred source prefix",
    )
    parser.add_argument("--lpn-base", type=int, default=63)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    result = analyze_restore(
        args.nand,
        args.source_hfs,
        source_offset=args.source_offset,
        source_bytes=args.source_bytes,
        lpn_base=args.lpn_base,
    )
    if args.json:
        print(json.dumps(result.as_dict(), sort_keys=True))
    else:
        print(
            f"complete={str(result.complete).lower()} "
            f"source_offset={result.source_offset} "
            f"source_pages={result.source_pages} "
            f"seen={result.seen_source_pages} "
            f"exact={result.exact_source_pages} "
            f"physical_user_pages={result.physical_user_pages} "
            f"multiple_versions={result.multiple_version_pages} "
            f"maximum_versions={result.maximum_versions} "
            f"ignored_source_tail_bytes={result.ignored_source_tail_bytes}"
        )
        print(
            "missing_source_ranges="
            + format_ranges(result.missing_source_ranges)
        )
        print(
            "wrong_source_ranges=" + format_ranges(result.wrong_source_ranges)
        )
    return 0 if result.complete else 1


if __name__ == "__main__":
    raise SystemExit(main())
