#!/usr/bin/env python3
"""Validate and inventory the supported iPhone 3G restore bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import plistlib
import sys
import zipfile
from pathlib import Path


EXPECTED_NAME = "iPhone1,2_4.2.1_8C148_Restore.ipsw"
EXPECTED_SIZE = 338_579_762
EXPECTED_SHA256 = "98e5969c3baed660c9a26e94cd7ed4b3cdb7175900f448bcc2223bf885835ce0"
EXPECTED_RESTORE = {
    "ProductType": "iPhone1,2",
    "ProductVersion": "4.2.1",
    "ProductBuildVersion": "8C148",
}
EXPECTED_DEVICE = {
    "BoardConfig": "n82ap",
    "CPID": 0x8900,
    "BDID": 4,
    "Platform": "s5l8900x",
}
LEGACY_NAME = "iPhone1,2_2.0_5A347_Restore.ipsw"
LEGACY_SIZE = 235_957_125
LEGACY_SHA256 = (
    "76fd34606cda0e2943766878c2cad4e9ee38e15084094240fba68e4391cba8f1"
)
LEGACY_RESTORE = {
    "ProductType": "iPhone1,2",
    "ProductVersion": "2.0",
    "ProductBuildVersion": "5A347",
}
LEGACY_COMPONENT_PATHS = {
    "LLB": "Firmware/all_flash/all_flash.n82ap.production/LLB.n82ap.RELEASE.img3",
    "iBoot": "Firmware/all_flash/all_flash.n82ap.production/iBoot.n82ap.RELEASE.img3",
    "iBSS": "Firmware/dfu/iBSS.n82ap.RELEASE.dfu",
    "iBEC": "Firmware/dfu/iBEC.n82ap.RELEASE.dfu",
    "DeviceTree": "Firmware/all_flash/all_flash.n82ap.production/DeviceTree.n82ap.img3",
    "KernelCache": "kernelcache.release.s5l8900x",
    "RestoreRamDisk": "018-3783-2.dmg",
    "OS": "018-3782-2.dmg",
}
REQUIRED_COMPONENTS = (
    "LLB",
    "iBoot",
    "iBSS",
    "iBEC",
    "DeviceTree",
    "KernelCache",
    "RestoreRamDisk",
    "OS",
)


def fail(message: str) -> None:
    raise ValueError(message)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def require_equal(actual: object, expected: object, label: str) -> None:
    if actual != expected:
        fail(f"{label}: got {actual!r}, expected {expected!r}")


def load_plist(archive: zipfile.ZipFile, name: str) -> dict[str, object]:
    try:
        with archive.open(name) as stream:
            value = plistlib.load(stream)
    except KeyError:
        fail(f"missing {name}")
    if not isinstance(value, dict):
        fail(f"{name} is not a dictionary")
    return value


def validate_legacy(path: Path) -> dict[str, object]:
    require_equal(path.stat().st_size, LEGACY_SIZE, "firmware size")
    firmware_sha256 = sha256(path)
    require_equal(firmware_sha256, LEGACY_SHA256, "firmware SHA-256")

    with zipfile.ZipFile(path) as archive:
        restore = load_plist(archive, "Restore.plist")
        for key, expected in LEGACY_RESTORE.items():
            require_equal(restore.get(key), expected, f"Restore.plist {key}")

        devices = [
            device
            for device in restore.get("DeviceMap", [])
            if isinstance(device, dict)
            and all(device.get(key) == value for key, value in EXPECTED_DEVICE.items())
        ]
        if len(devices) != 1:
            fail(
                f"Restore.plist must describe one N82 device, found {len(devices)}"
            )
        require_equal(
            restore.get("RestoreKernelCaches", {}).get("Release"),
            LEGACY_COMPONENT_PATHS["KernelCache"],
            "RestoreKernelCaches Release",
        )
        require_equal(
            restore.get("RestoreRamDisks", {}).get("User"),
            LEGACY_COMPONENT_PATHS["RestoreRamDisk"],
            "RestoreRamDisks User",
        )
        require_equal(
            restore.get("SystemRestoreImages", {}).get("User"),
            LEGACY_COMPONENT_PATHS["OS"],
            "SystemRestoreImages User",
        )

        members = set(archive.namelist())
        inventory: dict[str, dict[str, object]] = {}
        for component_name, member_path in LEGACY_COMPONENT_PATHS.items():
            if member_path not in members:
                fail(f"missing firmware member: {member_path}")
            member = archive.getinfo(member_path)
            inventory[component_name] = {
                "path": member_path,
                "size": member.file_size,
                "crc32": f"{member.CRC:08x}",
            }

    return {
        "file": str(path.resolve()),
        "name": LEGACY_NAME,
        "size": LEGACY_SIZE,
        "sha256": firmware_sha256,
        "product_type": LEGACY_RESTORE["ProductType"],
        "product_version": LEGACY_RESTORE["ProductVersion"],
        "product_build": LEGACY_RESTORE["ProductBuildVersion"],
        "board_config": EXPECTED_DEVICE["BoardConfig"],
        "chip_id": f"0x{EXPECTED_DEVICE['CPID']:04x}",
        "board_id": EXPECTED_DEVICE["BDID"],
        "build_identities": [
            {
                "restore_behavior": "Erase",
                "variant": "Legacy Restore",
                "components": inventory,
            }
        ],
    }


def validate(path: Path, profile: str = "8C148") -> dict[str, object]:
    if not path.is_file():
        fail(f"firmware does not exist: {path}")
    if profile == "5A347":
        return validate_legacy(path)
    require_equal(path.stat().st_size, EXPECTED_SIZE, "firmware size")
    firmware_sha256 = sha256(path)
    require_equal(firmware_sha256, EXPECTED_SHA256, "firmware SHA-256")

    with zipfile.ZipFile(path) as archive:
        restore = load_plist(archive, "Restore.plist")
        manifest = load_plist(archive, "BuildManifest.plist")

        for key, expected in EXPECTED_RESTORE.items():
            require_equal(restore.get(key), expected, f"Restore.plist {key}")

        device_map = restore.get("DeviceMap")
        if not isinstance(device_map, list) or len(device_map) != 1:
            fail("Restore.plist must describe exactly one device")
        device = device_map[0]
        if not isinstance(device, dict):
            fail("Restore.plist DeviceMap entry is not a dictionary")
        for key, expected in EXPECTED_DEVICE.items():
            require_equal(device.get(key), expected, f"DeviceMap {key}")

        identities = manifest.get("BuildIdentities")
        if not isinstance(identities, list) or not identities:
            fail("BuildManifest.plist has no build identities")

        archive_members = set(archive.namelist())
        validated_identities: list[dict[str, object]] = []
        for identity in identities:
            if not isinstance(identity, dict):
                fail("BuildManifest identity is not a dictionary")
            require_equal(identity.get("ApChipID"), "0x8900", "ApChipID")
            require_equal(identity.get("ApBoardID"), "0x04", "ApBoardID")

            info = identity.get("Info")
            components = identity.get("Manifest")
            if not isinstance(info, dict) or not isinstance(components, dict):
                fail("BuildManifest identity lacks Info or Manifest")
            require_equal(info.get("DeviceClass"), "n82ap", "DeviceClass")
            require_equal(info.get("BuildNumber"), "8C148", "BuildNumber")

            inventory: dict[str, dict[str, object]] = {}
            for component_name in REQUIRED_COMPONENTS:
                component = components.get(component_name)
                if not isinstance(component, dict):
                    fail(f"identity lacks {component_name}")
                component_info = component.get("Info")
                if not isinstance(component_info, dict):
                    fail(f"{component_name} lacks Info")
                member_path = component_info.get("Path")
                if not isinstance(member_path, str):
                    fail(f"{component_name} lacks a path")
                if member_path not in archive_members:
                    fail(f"missing firmware member: {member_path}")
                member = archive.getinfo(member_path)
                inventory[component_name] = {
                    "path": member_path,
                    "size": member.file_size,
                    "crc32": f"{member.CRC:08x}",
                }

            validated_identities.append(
                {
                    "restore_behavior": info.get("RestoreBehavior"),
                    "variant": info.get("Variant"),
                    "components": inventory,
                }
            )

    return {
        "file": str(path.resolve()),
        "name": EXPECTED_NAME,
        "size": EXPECTED_SIZE,
        "sha256": firmware_sha256,
        "product_type": EXPECTED_RESTORE["ProductType"],
        "product_version": EXPECTED_RESTORE["ProductVersion"],
        "product_build": EXPECTED_RESTORE["ProductBuildVersion"],
        "board_config": EXPECTED_DEVICE["BoardConfig"],
        "chip_id": f"0x{EXPECTED_DEVICE['CPID']:04x}",
        "board_id": EXPECTED_DEVICE["BDID"],
        "build_identities": validated_identities,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--profile", choices=("8C148", "5A347"), default="8C148"
    )
    parser.add_argument("ipsw", type=Path)
    args = parser.parse_args()
    try:
        report = validate(args.ipsw, args.profile)
    except (OSError, ValueError, zipfile.BadZipFile, plistlib.InvalidFileException) as error:
        print(f"firmware validation failed: {error}", file=sys.stderr)
        return 1
    json.dump(report, sys.stdout, indent=2, sort_keys=True)
    print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
