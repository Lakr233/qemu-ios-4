from __future__ import annotations

import sys
import socket
import threading
import time
import unittest
from pathlib import Path

from usb.core import USBError


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from usboip import (  # noqa: E402
    CONTROL_REQUEST,
    DEVICE_DESCRIPTOR,
    BULK_REQUEST,
    TRANSFER_RESULT,
    Device,
    Endpoint,
    ERROR,
    HEADER,
    IRecvAdapter,
    MAGIC,
    Opcode,
    RESPONSE,
    USBRequestTimeout,
    VERSION,
)


DFU_SERIAL = b"CPID:8900 BDID:04 ECID:1 IBFL:0 SRNM:[N82AP] SRTG:[iBoot-931.71.16]"


class FakeTransport:
    def __init__(self, responses: list[tuple[Opcode, bytes]]):
        self.responses = responses
        self.requests: list[tuple[Opcode, bytes]] = []
        self.timeouts: list[int | None] = []

    def request(
        self,
        opcode: Opcode,
        payload: bytes = b"",
        timeout_ms: int | None = None,
    ) -> bytes:
        self.requests.append((opcode, payload))
        self.timeouts.append(timeout_ms)
        expected_opcode, response = self.responses.pop(0)
        if opcode != expected_opcode:
            raise AssertionError(f"got {opcode}, expected {expected_opcode}")
        return response

    def close(self) -> None:
        pass

    def finish_enumeration(self) -> None:
        pass


def make_device(*responses: tuple[Opcode, bytes]) -> tuple[Device, FakeTransport]:
    transport = FakeTransport(
        [
            (
                Opcode.ENUMERATE,
                DEVICE_DESCRIPTOR.pack(0x05AC, 0x1227, len(DFU_SERIAL))
                + DFU_SERIAL,
            ),
            *responses,
        ]
    )
    return Device(transport), transport  # type: ignore[arg-type]


def recv_exactly(stream: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = stream.recv(size - len(data))
        if not chunk:
            raise EOFError
        data.extend(chunk)
    return bytes(data)


class ProtocolServer:
    def __init__(self) -> None:
        self.listener = socket.socket()
        try:
            self.listener.bind(("127.0.0.1", 0))
            self.listener.listen(1)
        except BaseException:
            self.listener.close()
            raise
        self.endpoint = Endpoint("127.0.0.1", self.listener.getsockname()[1])
        self.opcodes: list[Opcode] = []
        self.error: BaseException | None = None
        self.thread = threading.Thread(target=self._run, daemon=True)

    def __enter__(self) -> "ProtocolServer":
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
                    try:
                        header = recv_exactly(stream, HEADER.size)
                    except EOFError:
                        return
                    magic, version, raw_opcode, flags, request_id, size = (
                        HEADER.unpack(header)
                    )
                    if magic != MAGIC or version != VERSION or flags != 0:
                        raise AssertionError("invalid request header")
                    recv_exactly(stream, size)
                    opcode = Opcode(raw_opcode)
                    self.opcodes.append(opcode)
                    if opcode == Opcode.ENUMERATE:
                        response = (
                            DEVICE_DESCRIPTOR.pack(
                                0x05AC, 0x1227, len(DFU_SERIAL)
                            )
                            + DFU_SERIAL
                        )
                    elif opcode == Opcode.SET_CONFIGURATION:
                        response = b""
                    else:
                        raise AssertionError(f"unexpected opcode {opcode}")
                    stream.sendall(
                        HEADER.pack(
                            MAGIC,
                            VERSION,
                            opcode,
                            RESPONSE,
                            request_id,
                            len(response),
                        )
                        + response
                    )
        except BaseException as error:
            self.error = error


class ReconnectProtocolServer(ProtocolServer):
    def _run(self) -> None:
        try:
            for connection in range(2):
                stream, _ = self.listener.accept()
                with stream:
                    while True:
                        try:
                            header = recv_exactly(stream, HEADER.size)
                        except EOFError:
                            break
                        magic, version, raw_opcode, flags, request_id, size = (
                            HEADER.unpack(header)
                        )
                        if magic != MAGIC or version != VERSION or flags != 0:
                            raise AssertionError("invalid request header")
                        recv_exactly(stream, size)
                        opcode = Opcode(raw_opcode)
                        self.opcodes.append(opcode)
                        if opcode == Opcode.ENUMERATE:
                            response = (
                                DEVICE_DESCRIPTOR.pack(
                                    0x05AC, 0x1227, len(DFU_SERIAL)
                                )
                                + DFU_SERIAL
                            )
                            response_flags = RESPONSE
                        elif opcode == Opcode.SET_CONFIGURATION:
                            if connection == 0:
                                response = b"configuration failed"
                                response_flags = RESPONSE | ERROR
                            else:
                                response = b""
                                response_flags = RESPONSE
                        else:
                            raise AssertionError(f"unexpected opcode {opcode}")
                        stream.sendall(
                            HEADER.pack(
                                MAGIC,
                                VERSION,
                                opcode,
                                response_flags,
                                request_id,
                                len(response),
                            )
                            + response
                        )
        except BaseException as error:
            self.error = error


class EndpointTests(unittest.TestCase):
    def test_parse(self) -> None:
        self.assertEqual(Endpoint.parse("127.0.0.1:1337"), Endpoint("127.0.0.1", 1337))

    def test_rejects_missing_or_out_of_range_port(self) -> None:
        for value in ("127.0.0.1", "127.0.0.1:0", "127.0.0.1:65536"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                Endpoint.parse(value)


class DeviceTests(unittest.TestCase):
    def test_enumerates_apple_dfu_device(self) -> None:
        device, _ = make_device()
        self.assertEqual(device.idVendor, 0x05AC)
        self.assertEqual(device.idProduct, 0x1227)
        self.assertIn("CPID:8900", device.serial_number)

    def test_control_in_is_bounded(self) -> None:
        device, transport = make_device((Opcode.CONTROL, b"\x02"))
        result = device.ctrl_transfer(0xA1, 5, data_or_wLength=1, timeout=500)
        self.assertEqual(bytes(result), b"\x02")
        request = transport.requests[-1][1]
        self.assertEqual(CONTROL_REQUEST.unpack(request), (0xA1, 5, 0, 0, 500, 1))
        self.assertEqual(transport.timeouts[-1], 500)

    def test_control_out_returns_committed_prefix(self) -> None:
        device, transport = make_device((Opcode.CONTROL, TRANSFER_RESULT.pack(2)))
        result = device.ctrl_transfer(0x21, 1, 3, 0, b"abcd")
        self.assertEqual(result, 2)
        request = transport.requests[-1][1]
        self.assertEqual(
            CONTROL_REQUEST.unpack(request[: CONTROL_REQUEST.size]),
            (0x21, 1, 3, 0, 10_000, 4),
        )
        self.assertEqual(request[CONTROL_REQUEST.size :], b"abcd")

    def test_bulk_out_rejects_impossible_result(self) -> None:
        device, transport = make_device((Opcode.BULK_OUT, TRANSFER_RESULT.pack(5)))
        with self.assertRaisesRegex(USBError, "exceeds the request"):
            device.write(4, b"abcd")
        request = transport.requests[-1][1]
        self.assertEqual(BULK_REQUEST.unpack(request[: BULK_REQUEST.size]), (4, 10_000, 4))

    def test_default_configuration_selects_first_configuration(self) -> None:
        device, transport = make_device((Opcode.SET_CONFIGURATION, b""))
        device.set_configuration()
        self.assertEqual(transport.requests[-1],
                         (Opcode.SET_CONFIGURATION, b"\x01"))
        self.assertEqual(device.get_active_configuration().bConfigurationValue, 1)
        self.assertEqual(transport.timeouts[-1], 10_000)

    def test_stalled_enumeration_is_bounded(self) -> None:
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        endpoint = Endpoint("127.0.0.1", listener.getsockname()[1])

        def stall() -> None:
            with listener.accept()[0] as stream:
                recv_exactly(stream, HEADER.size)
                time.sleep(0.5)

        thread = threading.Thread(target=stall)
        thread.start()
        started = time.monotonic()
        try:
            with self.assertRaises(socket.timeout):
                Device.connect(endpoint)
            self.assertLess(time.monotonic() - started, 0.5)
        finally:
            listener.close()
            thread.join(timeout=1)

    def test_stalled_transfer_uses_request_timeout(self) -> None:
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        endpoint = Endpoint("127.0.0.1", listener.getsockname()[1])

        def stall() -> None:
            with listener.accept()[0] as stream:
                header = recv_exactly(stream, HEADER.size)
                _, _, raw_opcode, _, request_id, size = HEADER.unpack(header)
                recv_exactly(stream, size)
                descriptor = (
                    DEVICE_DESCRIPTOR.pack(0x05AC, 0x1227, len(DFU_SERIAL))
                    + DFU_SERIAL
                )
                stream.sendall(
                    HEADER.pack(
                        MAGIC,
                        VERSION,
                        raw_opcode,
                        RESPONSE,
                        request_id,
                        len(descriptor),
                    )
                    + descriptor
                )
                header = recv_exactly(stream, HEADER.size)
                *_, size = HEADER.unpack(header)
                recv_exactly(stream, size)
                time.sleep(0.2)

        thread = threading.Thread(target=stall)
        thread.start()
        device = Device.connect(endpoint)
        started = time.monotonic()
        try:
            with self.assertRaisesRegex(USBRequestTimeout, "request timed out"):
                device.ctrl_transfer(0x40, 1, timeout=20)
            self.assertLess(time.monotonic() - started, 0.2)
        finally:
            device.close()
            listener.close()
            thread.join(timeout=1)


class PymobiledeviceIntegrationTests(unittest.TestCase):
    def test_pinned_irecv_uses_network_transport(self) -> None:
        import pymobiledevice3.irecv as irecv

        original = (irecv.find, irecv.get_string, irecv.dispose_resources)
        with ProtocolServer() as server:
            IRecvAdapter(server.endpoint).install()
            client = None
            try:
                client = irecv.IRecv(timeout=1)
                self.assertEqual(client.chip_id, 0x8900)
                self.assertEqual(client.board_id, 4)
                self.assertEqual(client.product_type, "iPhone1,2")
            finally:
                if client is not None:
                    client.device.close()
                irecv.find, irecv.get_string, irecv.dispose_resources = original

        self.assertEqual(
            server.opcodes,
            [Opcode.ENUMERATE, Opcode.SET_CONFIGURATION],
        )

    def test_failed_constructor_releases_transport_before_retry(self) -> None:
        import pymobiledevice3.irecv as irecv

        original = (irecv.find, irecv.get_string, irecv.dispose_resources)
        with ReconnectProtocolServer() as server:
            IRecvAdapter(server.endpoint).install()
            client = None
            try:
                with self.assertRaisesRegex(USBError, "configuration failed"):
                    irecv.IRecv(timeout=1)
                client = irecv.IRecv(timeout=1)
                self.assertEqual(client.product_type, "iPhone1,2")
            finally:
                if client is not None:
                    client.device.close()
                irecv.find, irecv.get_string, irecv.dispose_resources = original

        self.assertEqual(
            server.opcodes,
            [
                Opcode.ENUMERATE,
                Opcode.SET_CONFIGURATION,
                Opcode.ENUMERATE,
                Opcode.SET_CONFIGURATION,
            ],
        )


if __name__ == "__main__":
    unittest.main()
