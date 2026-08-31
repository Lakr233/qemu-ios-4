from __future__ import annotations

import socket
import sys
import threading
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from gdbremote import GDBRemote  # noqa: E402


def read_exactly(stream: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = stream.recv(size - len(data))
        if not chunk:
            raise EOFError
        data.extend(chunk)
    return bytes(data)


def read_packet(stream: socket.socket) -> bytes:
    while (first := read_exactly(stream, 1)) != b"$":
        if first == b"\x03":
            return first
    payload = bytearray()
    while (byte := read_exactly(stream, 1)) != b"#":
        payload.extend(byte)
    checksum = read_exactly(stream, 2)
    if int(checksum, 16) != sum(payload) & 0xFF:
        raise AssertionError("request checksum mismatch")
    return bytes(payload)


def frame(payload: bytes) -> bytes:
    return b"+" + b"$" + payload + b"#" + f"{sum(payload) & 0xFF:02x}".encode()


class GDBServer:
    def __init__(self, memory: bytes) -> None:
        self.memory = memory
        self.commands: list[bytes] = []
        self.error: BaseException | None = None
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.thread = threading.Thread(target=self._run, daemon=True)

    def __enter__(self) -> "GDBServer":
        self.thread.start()
        return self

    def __exit__(self, *args: object) -> None:
        self.listener.close()
        self.thread.join(timeout=1)
        if self.error is not None:
            raise self.error

    def _run(self) -> None:
        try:
            stream, _ = self.listener.accept()
            with stream:
                while True:
                    command = read_packet(stream)
                    self.commands.append(command)
                    if command == b"?":
                        response = b"T05thread:1;"
                    elif command == b"\x03":
                        response = b"T02thread:1;"
                    elif command.startswith((b"Z0,", b"z0,")):
                        response = b"OK"
                    elif command == b"c":
                        response = b"T05thread:1;"
                    elif command == b"s":
                        response = b"T05thread:1;"
                    elif command == b"pf":
                        response = (0x9100).to_bytes(4, "little").hex().encode()
                    elif command.startswith(b"m"):
                        response = self.memory.hex().encode()
                    elif command.startswith(b"M"):
                        self.memory = bytes.fromhex(command.split(b":", 1)[1].decode())
                        response = b"OK"
                    elif command == b"D;1":
                        stream.sendall(frame(b"OK"))
                        return
                    else:
                        raise AssertionError(f"unexpected command: {command!r}")
                    stream.sendall(frame(response))
        except BaseException as error:
            self.error = error


class GDBRemoteTests(unittest.TestCase):
    def test_read_write_readback_and_detach(self) -> None:
        original = bytes(range(40))
        replacement = bytes(reversed(original))
        with GDBServer(original) as server:
            with GDBRemote("127.0.0.1", server.port) as remote:
                remote.stop()
                self.assertEqual(remote.read_memory(0x18020634, 40), original)
                remote.write_memory(0x18020634, replacement)
                self.assertEqual(remote.read_memory(0x18020634, 40), replacement)
                remote.detach()
        self.assertEqual(server.memory, replacement)
        self.assertEqual(server.commands[0], b"?")
        self.assertEqual(server.commands[-1], b"D;1")

    def test_interrupt_breakpoint_resume_and_register_read(self) -> None:
        with GDBServer(b"anchor") as server:
            with GDBRemote("127.0.0.1", server.port) as remote:
                remote.interrupt()
                remote.insert_breakpoint(0x9100)
                remote.resume()
                self.assertTrue(remote.wait_for_stop().startswith(b"T05"))
                self.assertEqual(remote.read_register(15), 0x9100)
                remote.remove_breakpoint(0x9100)
                remote.detach()
        self.assertEqual(
            server.commands,
            [b"\x03", b"Z0,9100,2", b"c", b"pf", b"z0,9100,2", b"D;1"],
        )

    def test_single_step_waits_for_the_next_stop(self) -> None:
        with GDBServer(b"anchor") as server:
            with GDBRemote("127.0.0.1", server.port) as remote:
                remote.stop()
                self.assertTrue(remote.step().startswith(b"T05"))
                remote.detach()

        self.assertEqual(server.commands, [b"?", b"s", b"D;1"])


if __name__ == "__main__":
    unittest.main()
