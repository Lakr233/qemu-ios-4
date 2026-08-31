"""Minimal GDB remote client for authenticated guest-memory updates."""

from __future__ import annotations

import socket
from collections.abc import Callable


class GDBRemoteError(RuntimeError):
    pass


def read_exactly(stream: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = stream.recv(size - len(data))
        if not chunk:
            raise GDBRemoteError("GDB peer closed the connection")
        data.extend(chunk)
    return bytes(data)


class GDBRemote:
    def __init__(self, host: str, port: int, timeout: float = 10) -> None:
        self._socket = socket.create_connection((host, port), timeout=timeout)

    def set_timeout(self, timeout: float) -> None:
        """Change the bound for the next remote response wait."""

        self._socket.settimeout(timeout)

    def close(self) -> None:
        self._socket.close()

    def __enter__(self) -> "GDBRemote":
        return self

    def __exit__(self, *args: object) -> None:
        self.close()

    @staticmethod
    def _frame(payload: bytes) -> bytes:
        checksum = sum(payload) & 0xFF
        return b"$" + payload + b"#" + f"{checksum:02x}".encode()

    def _receive(self) -> bytes:
        while read_exactly(self._socket, 1) != b"$":
            continue
        payload = bytearray()
        while True:
            byte = read_exactly(self._socket, 1)
            if byte == b"#":
                break
            payload.extend(byte)
        checksum_text = read_exactly(self._socket, 2)
        try:
            checksum = int(checksum_text, 16)
        except ValueError as error:
            raise GDBRemoteError("GDB response checksum is invalid") from error
        if checksum != sum(payload) & 0xFF:
            self._socket.sendall(b"-")
            raise GDBRemoteError("GDB response checksum does not match")
        self._socket.sendall(b"+")
        return bytes(payload)

    def _send(self, payload: str) -> None:
        self._socket.sendall(self._frame(payload.encode("ascii")))

    def command(self, payload: str) -> bytes:
        self._send(payload)
        response = self._receive()
        if response.startswith(b"E"):
            raise GDBRemoteError(
                f"GDB command {payload!r} failed: {response.decode()}"
            )
        return response

    def stop(self) -> None:
        response = self.command("?")
        if not response.startswith((b"S", b"T")):
            raise GDBRemoteError(f"unexpected GDB stop response: {response!r}")

    def interrupt(self) -> None:
        self._socket.sendall(b"\x03")
        response = self._receive()
        if not response.startswith((b"S", b"T")):
            raise GDBRemoteError(
                f"unexpected GDB interrupt response: {response!r}"
            )

    def insert_breakpoint(self, address: int, size: int = 2) -> None:
        response = self.command(f"Z0,{address:x},{size:x}")
        if response != b"OK":
            raise GDBRemoteError(
                f"unexpected breakpoint response: {response!r}"
            )

    def remove_breakpoint(self, address: int, size: int = 2) -> None:
        response = self.command(f"z0,{address:x},{size:x}")
        if response != b"OK":
            raise GDBRemoteError(
                f"unexpected breakpoint removal response: {response!r}"
            )

    def resume(self, armed: Callable[[], None] | None = None) -> None:
        self._send("c")
        if armed is not None:
            armed()

    def step(self) -> bytes:
        self._send("s")
        return self.wait_for_stop()

    def wait_for_stop(self) -> bytes:
        response = self._receive()
        if not response.startswith((b"S", b"T")):
            raise GDBRemoteError(f"unexpected GDB stop response: {response!r}")
        return response

    def read_register(self, index: int) -> int:
        response = self.command(f"p{index:x}")
        try:
            data = bytes.fromhex(response.decode("ascii"))
        except (UnicodeDecodeError, ValueError) as error:
            raise GDBRemoteError(
                f"GDB register {index} response is not hexadecimal"
            ) from error
        if not data:
            raise GDBRemoteError(f"GDB register {index} response is empty")
        return int.from_bytes(data, "little")

    def write_register(self, index: int, value: int, size: int = 4) -> None:
        if value < 0 or value >= 1 << (size * 8):
            raise ValueError(f"GDB register value does not fit in {size} bytes")
        data = value.to_bytes(size, "little")
        response = self.command(f"P{index:x}={data.hex()}")
        if response != b"OK":
            raise GDBRemoteError(
                f"unexpected GDB register write response: {response!r}"
            )

    def read_memory(self, address: int, size: int) -> bytes:
        response = self.command(f"m{address:x},{size:x}")
        try:
            data = bytes.fromhex(response.decode("ascii"))
        except (UnicodeDecodeError, ValueError) as error:
            raise GDBRemoteError("GDB memory response is not hexadecimal") from error
        if len(data) != size:
            raise GDBRemoteError(
                f"GDB returned {len(data)} memory bytes, expected {size}"
            )
        return data

    def write_memory(self, address: int, data: bytes) -> None:
        response = self.command(f"M{address:x},{len(data):x}:{data.hex()}")
        if response != b"OK":
            raise GDBRemoteError(f"unexpected GDB write response: {response!r}")

    def detach(self) -> None:
        response = self.command("D;1")
        if response != b"OK":
            raise GDBRemoteError(f"unexpected GDB detach response: {response!r}")
