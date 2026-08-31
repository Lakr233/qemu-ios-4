#!/usr/bin/env python3
"""Probe the restored service exposed by the emulated iPhone over usbmuxd."""

from __future__ import annotations

import argparse
import asyncio
import json
import re
from typing import Any

from pymobiledevice3.service_connection import ServiceConnection
from pymobiledevice3.usbmux import select_devices_by_connection_type

from usboip import Endpoint


def validate_restored(
    type_info: Any, hardware_response: Any, usb_serial: str = ""
) -> dict[str, Any]:
    if not isinstance(type_info, dict) or type_info.get("Result") != "Success":
        raise RuntimeError(f"restored QueryType failed: {type_info!r}")
    if type_info.get("Type") != "com.apple.mobile.restored":
        raise RuntimeError(f"device is not running restored: {type_info!r}")
    hardware = None
    if isinstance(hardware_response, dict):
        hardware = hardware_response.get("HardwareInfo")
    if not isinstance(hardware, dict):
        legacy_identity = re.search(
            r"(?:^| )CPID:([0-9a-fA-F]+) BDID:([0-9a-fA-F]+)(?: |$)",
            usb_serial,
        )
        if type_info.get("RestoreProtocolVersion") != 11 or legacy_identity is None:
            raise RuntimeError(
                f"restored HardwareInfo is missing: {hardware_response!r}"
            )
        hardware = {
            "ChipID": int(legacy_identity.group(1), 16),
            "BoardID": int(legacy_identity.group(2), 16),
        }
    expected = {"HardwareModel": "N82AP", "BoardID": 4, "ChipID": 0x8900}
    actual = {
        "HardwareModel": type_info.get("HardwareModel"),
        "BoardID": hardware.get("BoardID"),
        "ChipID": hardware.get("ChipID"),
    }
    if actual != expected:
        raise RuntimeError(f"unexpected restored hardware identity: {actual!r}")
    return {
        "Type": type_info["Type"],
        "RestoreProtocolVersion": type_info.get("RestoreProtocolVersion"),
        **actual,
        "UniqueChipID": hardware.get("UniqueChipID"),
        "ProductionMode": hardware.get("ProductionMode"),
    }


async def probe(address: str) -> dict[str, Any]:
    devices = await select_devices_by_connection_type(
        "USB", usbmux_address=address
    )
    if len(devices) != 1:
        raise RuntimeError(f"expected one IOSU USB device, found {len(devices)}")
    device = devices[0]
    service = await ServiceConnection.create_using_usbmux(
        device.serial,
        62078,
        connection_type=device.connection_type,
        usbmux_address=address,
    )
    await service.start()
    try:
        type_info = await service.send_recv_plist({"Request": "QueryType"})
        if type_info.get("RestoreProtocolVersion") == 11:
            hardware = None
        else:
            hardware = await service.send_recv_plist(
                {
                    "Request": "QueryValue",
                    "Label": "pymobiledevice3",
                    "QueryKey": "HardwareInfo",
                }
            )
    finally:
        await service.close()
    result = validate_restored(type_info, hardware, device.serial)
    result["USBMuxDeviceID"] = device.devid
    result["ConnectionType"] = device.connection_type
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--usbmuxd", default="127.0.0.1:27015")
    options = parser.parse_args()
    Endpoint.parse(options.usbmuxd)
    print(json.dumps(asyncio.run(probe(options.usbmuxd)), indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
