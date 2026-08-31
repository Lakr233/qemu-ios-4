from __future__ import annotations

import struct
import sys
import tempfile
import unittest
from pathlib import Path

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts/ios"))

from img3_extract import extract_img3  # noqa: E402


IMG3_HEADER = struct.Struct("<4sIII4s")
TAG_HEADER = struct.Struct("<4sII")
KEY = bytes.fromhex("00112233445566778899aabbccddeeff")
IV = bytes.fromhex("ffeeddccbbaa99887766554433221100")


def tag(magic: bytes, logical: bytes, stored: bytes | None = None) -> bytes:
    payload = logical if stored is None else stored
    header = TAG_HEADER.pack(
        magic, TAG_HEADER.size + len(payload), len(logical)
    )
    return header + payload


def image(data_tag: bytes) -> bytes:
    type_tag = tag(b"EPYT", b"lnrk")
    tags = type_tag + data_tag
    full_size = IMG3_HEADER.size + len(tags)
    payload_size = full_size - IMG3_HEADER.size
    return IMG3_HEADER.pack(
        b"3gmI", full_size, payload_size, payload_size, b"lnrk"
    ) + tags


class Img3ExtractTests(unittest.TestCase):
    def test_decrypts_stored_padding_then_trims_logical_data(self) -> None:
        plaintext = b"complzss" + bytes(range(31))
        padded = plaintext + bytes((-len(plaintext)) % 16)
        encryptor = Cipher(algorithms.AES(KEY), modes.CBC(IV)).encryptor()
        encrypted = encryptor.update(padded) + encryptor.finalize()

        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "kernel.img3"
            destination = Path(directory) / "kernel.payload"
            source.write_bytes(image(tag(b"ATAD", plaintext, encrypted)))
            extract_img3(source, destination, KEY, IV)
            self.assertEqual(destination.read_bytes(), plaintext)

    def test_extracts_unencrypted_logical_data(self) -> None:
        plaintext = b"device-tree"
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "tree.img3"
            destination = Path(directory) / "tree.payload"
            source.write_bytes(image(tag(b"ATAD", plaintext)))
            extract_img3(source, destination)
            self.assertEqual(destination.read_bytes(), plaintext)

    def test_decrypts_block_prefix_and_preserves_plain_tail(self) -> None:
        prefix = bytes(range(32))
        tail = b"tree-tail-12"
        encryptor = Cipher(algorithms.AES(KEY), modes.CBC(IV)).encryptor()
        stored = encryptor.update(prefix) + encryptor.finalize() + tail
        plaintext = prefix + tail

        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "tree.img3"
            destination = Path(directory) / "tree.payload"
            source.write_bytes(image(tag(b"ATAD", plaintext, stored)))
            extract_img3(source, destination, KEY, IV)
            self.assertEqual(destination.read_bytes(), plaintext)

    def test_rejects_logical_size_beyond_storage(self) -> None:
        invalid = TAG_HEADER.pack(b"ATAD", TAG_HEADER.size + 4, 5) + b"data"
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "invalid.img3"
            destination = Path(directory) / "output"
            source.write_bytes(image(invalid))
            with self.assertRaisesRegex(ValueError, "invalid logical size"):
                extract_img3(source, destination)
            self.assertFalse(destination.exists())

    def test_refuses_to_overwrite_destination(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "input.img3"
            destination = Path(directory) / "output"
            source.write_bytes(image(tag(b"ATAD", b"new")))
            destination.write_bytes(b"existing")
            with self.assertRaises(FileExistsError):
                extract_img3(source, destination)
            self.assertEqual(destination.read_bytes(), b"existing")


if __name__ == "__main__":
    unittest.main()
