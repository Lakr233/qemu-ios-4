from __future__ import annotations

import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import zlib


ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts/ios/reset-iphone3g-nor.py"
SPEC = importlib.util.spec_from_file_location("reset_iphone3g_nor", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
nor = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(nor)


def checksum(header: bytes) -> int:
    value = header[0]
    for byte in header[2:16]:
        value += byte
        value = (value + (value >> 8)) & 0xFF
    return value


def partitions(bank: bytes) -> list[tuple[int, bytes, bytes]]:
    result = []
    offset = 0
    while offset < len(bank):
        header = bank[offset:offset + nor.HEADER_SIZE]
        size = struct.unpack_from("<H", header, 2)[0] * nor.HEADER_SIZE
        if size < nor.HEADER_SIZE or offset + size > len(bank):
            raise AssertionError("invalid CHRP partition extent")
        result.append(
            (
                header[0],
                header[4:16].rstrip(b"\0"),
                bank[offset:offset + size],
            )
        )
        offset += size
    return result


class ResetIPhone3GNorTests(unittest.TestCase):
    def test_creates_two_valid_apple_chrp_banks(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            image_path = Path(directory) / "nor.raw"
            nor.reset_image(image_path)
            image = image_path.read_bytes()

        self.assertEqual(len(image), nor.IMAGE_SIZE)
        self.assertEqual(image[:nor.BANK_OFFSETS[0]], b"\xff" * nor.BANK_OFFSETS[0])
        for generation, offset in enumerate(nor.BANK_OFFSETS, start=1):
            bank = image[offset:offset + nor.BANK_SIZE]
            records = partitions(bank)
            self.assertEqual(
                [
                    (signature, name, len(record))
                    for signature, name, record in records
                ],
                [
                    (0x5A, b"nvram", 0x20),
                    (0x70, b"common", 0x800),
                    (0xA1, b"APL,OSXPanic", 0x810),
                    (0x7F, b"wwwwwwwwwwww", 0xFD0),
                ],
            )
            self.assertEqual(
                [record[1] for _, _, record in records],
                [0x82, 0x7C, 0x15, 0x17],
            )
            for _, _, record in records:
                self.assertEqual(record[1], checksum(record))
            self.assertEqual(
                struct.unpack_from("<I", bank, 16)[0],
                zlib.adler32(bank[20:]) & 0xFFFFFFFF,
            )
            self.assertEqual(struct.unpack_from("<I", bank, 20)[0], generation)
            common = records[1][2]
            self.assertTrue(
                common[nor.HEADER_SIZE:].startswith(nor.COMMON_VARIABLES)
            )

    def test_rejects_invalid_generation(self) -> None:
        for generation in (0, -1, 1 << 32):
            with self.subTest(generation=generation):
                with self.assertRaises(ValueError):
                    nor.format_bank(generation)

    def test_replaces_an_existing_image_atomically(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            image_path = Path(directory) / "nor.raw"
            image_path.write_bytes(b"invalid")
            nor.reset_image(image_path)
            self.assertEqual(image_path.stat().st_size, nor.IMAGE_SIZE)
            self.assertFalse(list(image_path.parent.glob(f".{image_path.name}.*")))


if __name__ == "__main__":
    unittest.main()
