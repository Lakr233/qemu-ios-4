"""Client-side USB transaction transport for the virtual iPhone device.

This is an application-level USB-over-IP protocol, not the Linux USB/IP
kernel protocol.  It carries the exact control and bulk transfers used by
pymobiledevice3's irecv layer while QEMU owns the emulated device controller.
"""

from __future__ import annotations

import socket
import struct
from array import array
from dataclasses import dataclass
from enum import IntEnum
from itertools import count
from types import SimpleNamespace
from typing import Optional
from weakref import ReferenceType, finalize, ref

from usb.core import USBError


MAGIC = b"IOSU"
VERSION = 1
MAX_PAYLOAD = 16 * 1024 * 1024
DEFAULT_TIMEOUT_MS = 10_000
RESPONSE = 1 << 0
ERROR = 1 << 1
HEADER = struct.Struct("!4sBBHII")
DEVICE_DESCRIPTOR = struct.Struct("!HHH")
CONTROL_REQUEST = struct.Struct("!BBHHII")
BULK_REQUEST = struct.Struct("!BII")
TRANSFER_RESULT = struct.Struct("!I")


class Opcode(IntEnum):
    ENUMERATE = 1
    CONTROL = 2
    BULK_OUT = 3
    BULK_IN = 4
    SET_CONFIGURATION = 5
    SET_INTERFACE = 6
    RESET = 7


class USBRequestTimeout(USBError):
    """A framed request expired and its connection can no longer be reused."""


@dataclass(frozen=True)
class Endpoint:
    host: str
    port: int

    @classmethod
    def parse(cls, value: str) -> "Endpoint":
        host, separator, port_text = value.rpartition(":")
        if not separator or not host:
            raise ValueError("USB-over-IP endpoint must be HOST:PORT")
        port = int(port_text)
        if not 1 <= port <= 65535:
            raise ValueError("USB-over-IP port must be between 1 and 65535")
        return cls(host, port)


def _read_exactly(stream: socket.socket, size: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        chunk = stream.recv(size - len(chunks))
        if not chunk:
            raise USBError("USB-over-IP peer closed the connection")
        chunks.extend(chunk)
    return bytes(chunks)


class Transport:
    def __init__(self, endpoint: Endpoint, connect_timeout: float = 0.25):
        self._endpoint = endpoint
        self._socket = socket.create_connection(
            (endpoint.host, endpoint.port), timeout=connect_timeout
        )
        self._request_ids = count(1)

    def finish_enumeration(self) -> None:
        self._socket.settimeout(None)

    def close(self) -> None:
        self._socket.close()

    def request(
        self,
        opcode: Opcode,
        payload: bytes = b"",
        timeout_ms: Optional[int] = None,
    ) -> bytes:
        if len(payload) > MAX_PAYLOAD:
            raise USBError("USB-over-IP request exceeds the payload limit")
        request_id = next(self._request_ids)
        previous_timeout = self._socket.gettimeout()
        if timeout_ms is not None:
            self._socket.settimeout(timeout_ms / 1000)
        try:
            self._socket.sendall(
                HEADER.pack(MAGIC, VERSION, opcode, 0, request_id, len(payload))
                + payload
            )

            header = _read_exactly(self._socket, HEADER.size)
            magic, version, response_opcode, flags, response_id, size = (
                HEADER.unpack(header)
            )
            if magic != MAGIC or version != VERSION:
                raise USBError(
                    "USB-over-IP peer returned an incompatible frame"
                )
            if response_opcode != opcode or response_id != request_id:
                raise USBError("USB-over-IP response does not match its request")
            if not flags & RESPONSE or size > MAX_PAYLOAD:
                raise USBError("USB-over-IP peer returned an invalid response")
            response = _read_exactly(self._socket, size)
            if flags & ERROR:
                message = response.decode("utf-8", errors="replace")
                raise USBError(message or "USB-over-IP transfer failed")
            return response
        except TimeoutError as error:
            if timeout_ms is None:
                raise
            self.close()
            raise USBRequestTimeout("USB-over-IP request timed out") from error
        finally:
            if self._socket.fileno() >= 0:
                self._socket.settimeout(previous_timeout)


class Device:
    """The PyUSB subset consumed by pymobiledevice3.irecv.IRecv."""

    def __init__(
        self,
        transport: Transport,
        request_timeout_ms: Optional[int] = None,
    ):
        self._transport = transport
        self._request_timeout_ms = request_timeout_ms
        self._finalizer = finalize(self, transport.close)
        try:
            descriptor = transport.request(Opcode.ENUMERATE)
            if len(descriptor) < DEVICE_DESCRIPTOR.size:
                raise USBError("USB-over-IP device descriptor is truncated")
            self.idVendor, self.idProduct, serial_size = (
                DEVICE_DESCRIPTOR.unpack_from(descriptor)
            )
            if len(descriptor) != DEVICE_DESCRIPTOR.size + serial_size:
                raise USBError(
                    "USB-over-IP device descriptor has an invalid serial length"
                )
            try:
                self.serial_number = descriptor[
                    DEVICE_DESCRIPTOR.size :
                ].decode("ascii")
            except UnicodeDecodeError as error:
                raise USBError(
                    "USB-over-IP serial number is not ASCII"
                ) from error
        except Exception:
            self.close()
            raise
        transport.finish_enumeration()
        self._configuration: Optional[int] = None

    @classmethod
    def connect(
        cls,
        endpoint: Endpoint,
        request_timeout_ms: Optional[int] = None,
    ) -> "Device":
        return cls(Transport(endpoint), request_timeout_ms)

    def _request_timeout(self, timeout_ms: int) -> int:
        if self._request_timeout_ms is not None:
            return self._request_timeout_ms
        return timeout_ms

    def close(self) -> None:
        self._finalizer()

    def ctrl_transfer(
        self,
        bmRequestType: int,
        bRequest: int,
        wValue: int = 0,
        wIndex: int = 0,
        data_or_wLength: object = None,
        timeout: int = 10_000,
    ) -> object:
        timeout = self._request_timeout(timeout)
        is_in = bool(bmRequestType & 0x80)
        if is_in:
            if data_or_wLength is None:
                length = 0
            elif isinstance(data_or_wLength, int):
                length = data_or_wLength
            else:
                length = len(data_or_wLength)  # type: ignore[arg-type]
            data = b""
        else:
            if data_or_wLength is None or isinstance(data_or_wLength, int):
                data = b""
            else:
                data = bytes(data_or_wLength)  # type: ignore[arg-type]
            length = len(data)
        if length > MAX_PAYLOAD - CONTROL_REQUEST.size:
            raise USBError("USB control transfer exceeds the payload limit")
        payload = CONTROL_REQUEST.pack(
            bmRequestType, bRequest, wValue, wIndex, timeout, length
        ) + data
        response = self._transport.request(Opcode.CONTROL, payload, timeout)
        if is_in:
            if len(response) > length:
                raise USBError("USB control response exceeds the requested size")
            return array("B", response)
        if len(response) != TRANSFER_RESULT.size:
            raise USBError("USB-over-IP control result has an invalid size")
        transferred = TRANSFER_RESULT.unpack(response)[0]
        if transferred > len(data):
            raise USBError("USB-over-IP control result exceeds the request")
        return transferred

    def write(self, endpoint: int, data: object, timeout: int = 10_000) -> int:
        timeout = self._request_timeout(timeout)
        body = bytes(data)  # type: ignore[arg-type]
        if len(body) > MAX_PAYLOAD - BULK_REQUEST.size:
            raise USBError("USB bulk transfer exceeds the payload limit")
        response = self._transport.request(
            Opcode.BULK_OUT,
            BULK_REQUEST.pack(endpoint, timeout, len(body)) + body,
            timeout,
        )
        if len(response) != TRANSFER_RESULT.size:
            raise USBError("USB-over-IP bulk result has an invalid size")
        transferred = TRANSFER_RESULT.unpack(response)[0]
        if transferred > len(body):
            raise USBError("USB-over-IP bulk result exceeds the request")
        return transferred

    def read(self, endpoint: int, size: int, timeout: int = 10_000) -> array[int]:
        timeout = self._request_timeout(timeout)
        if size > MAX_PAYLOAD:
            raise USBError("USB bulk read exceeds the payload limit")
        response = self._transport.request(
            Opcode.BULK_IN,
            BULK_REQUEST.pack(endpoint, timeout, size),
            timeout,
        )
        if len(response) > size:
            raise USBError("USB-over-IP bulk read exceeds the requested size")
        return array("B", response)

    def set_configuration(self, configuration: Optional[int] = None) -> None:
        value = 1 if configuration is None else configuration
        timeout = self._request_timeout(DEFAULT_TIMEOUT_MS)
        self._transport.request(
            Opcode.SET_CONFIGURATION,
            bytes((value,)),
            timeout,
        )
        self._configuration = value

    def get_active_configuration(self) -> SimpleNamespace:
        if self._configuration is None:
            raise USBError("USB device is not configured")
        return SimpleNamespace(bConfigurationValue=self._configuration)

    def set_interface_altsetting(
        self,
        interface: Optional[int] = None,
        alternate_setting: Optional[int] = None,
    ) -> None:
        timeout = self._request_timeout(DEFAULT_TIMEOUT_MS)
        self._transport.request(
            Opcode.SET_INTERFACE,
            bytes(((interface or 0), (alternate_setting or 0))),
            timeout,
        )

    def reset(self) -> None:
        timeout = self._request_timeout(DEFAULT_TIMEOUT_MS)
        self._transport.request(Opcode.RESET, timeout_ms=timeout)


class IRecvAdapter:
    def __init__(
        self,
        endpoint: Endpoint,
        request_timeout_ms: Optional[int] = None,
    ):
        self._endpoint = endpoint
        self.request_timeout_ms = request_timeout_ms
        self._device: Optional[ReferenceType[Device]] = None

    def find(self, find_all: bool = False, **_: object) -> object:
        device = self._device() if self._device is not None else None
        if device is not None:
            device.close()
        self._device = None
        try:
            device = Device.connect(self._endpoint, self.request_timeout_ms)
        except (OSError, USBError):
            return [] if find_all else None
        self._device = ref(device)
        return [device] if find_all else device

    @staticmethod
    def get_string(device: Device, index: int, langid: object = None) -> str:
        del index, langid
        return device.serial_number

    def dispose_resources(self, device: Device) -> None:
        device.close()
        if self._device is not None and device is self._device():
            self._device = None

    def install(self) -> None:
        import pymobiledevice3.irecv as irecv

        irecv.find = self.find
        irecv.get_string = self.get_string
        irecv.dispose_resources = self.dispose_resources
