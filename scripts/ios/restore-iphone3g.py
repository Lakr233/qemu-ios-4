#!/usr/bin/env python3
"""Install an iPhone 3G system image through restored and ASR."""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import json
import os
import plistlib
import uuid
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any
from zipfile import ZipFile

from pymobiledevice3.exceptions import ConnectionFailedError
from pymobiledevice3.restore.asr import ASRClient, DEFAULT_ASR_SYNC_PORT
from pymobiledevice3.service_connection import ServiceConnection
from pymobiledevice3.usbmux import MuxDevice, select_devices_by_connection_type

from gdbremote import GDBRemote, GDBRemoteError
from usboip import Endpoint


NAND_DRIVER_STARTED = b"Apple NAND Driver (AND) RW"
NAND_READY = b"[FTL:MSG] FTL_Open            [OK]"
LEGACY_NAND_DRIVER_STARTED = b"Apple NAND Driver (AND) 0x"
LEGACY_NAND_READY = b"[FTL:MSG] FTL_Open"
PROGRESS_REPEAT_INTERVAL = 10
STATUS_LOG_TAIL_BYTES = 4096
# A cold 8C148 boot can need more than a minute to publish AppleUSBMux under
# single-threaded TCG on a slow Host.  Keep discovery alive across that boot
# boundary while retaining bounded retries for a genuinely absent service.
RESTORED_CONNECT_TIMEOUT = 120
RESTORED_CONNECT_RETRY_INTERVAL = 0.25
RESTORED_CONNECT_ATTEMPT_TIMEOUT = 5
IOS2_ASR_CHECKSUM_ENTRY_ADDRESS = 0x90FC
IOS2_ASR_EPILOGUE_ADDRESS = 0x93AE
IOS4_ASR_SIGNATURE_FAILURE_ADDRESS = 0x14D9E
IOS4_ASR_SIGNATURE_EPILOGUE_ADDRESS = 0x14E50
IOS2_RESTORED_PARTITION_GATE_ADDRESS = 0x62E4
IOS2_RESTORED_PARTITION_EPILOGUE_ADDRESS = 0x6884
IOS2_RESTORED_PARTITION_WIPE_CALL_ADDRESS = 0x67EC
IOS2_RESTORED_IMAGE_GATE_ADDRESS = 0x6934
IOS2_RESTORED_IMAGE_EPILOGUE_ADDRESS = 0x6A08
IOS2_NOR_FLASH_ENTRY_ADDRESS = 0xC0399D94
IOS2_NOR_FLASH_EPILOGUE_ADDRESS = 0xC039A120


@dataclass(frozen=True)
class GuestAnchor:
    address: int
    value: bytes
    name: str


@dataclass(frozen=True)
class GuestPatch:
    address: int
    original: bytes
    replacement: bytes
    name: str

    def __post_init__(self) -> None:
        if not self.original or len(self.original) != len(self.replacement):
            raise ValueError(
                "Guest patch original and replacement must have equal, "
                "nonzero sizes"
            )


@dataclass(frozen=True)
class GuestObservation:
    address: int
    name: str
    registers: tuple[int, ...] = (0,)
    memory: tuple[int, int] | None = None

    def __post_init__(self) -> None:
        if not self.registers or any(
            register not in range(16) for register in self.registers
        ):
            raise ValueError("Guest observation registers must be R0 through R15")
        if self.memory is not None:
            address_register, size = self.memory
            if address_register not in range(16) or not 1 <= size <= 4096:
                raise ValueError(
                    "Guest observation memory must use R0 through R15 and "
                    "contain 1 through 4096 bytes"
                )


@dataclass(frozen=True)
class GuestObservationResult:
    observation: GuestObservation
    register_values: tuple[int, ...]
    memory: bytes | None
    return_address: int


class AuthenticatedRuntimePatch:
    """Apply Guest instructions only between two authenticated breakpoints."""

    def __init__(
        self,
        endpoint: Endpoint,
        name: str,
        entry_address: int,
        completion_address: int,
        anchors: tuple[GuestAnchor, ...],
        patches: tuple[GuestPatch, ...],
        observations: tuple[GuestObservation, ...] = (),
        timeout: float = 300,
        entry_observation: GuestObservation | None = None,
        transaction_count: int = 1,
    ) -> None:
        self.endpoint = endpoint
        self.name = name
        self.entry_address = entry_address
        self.completion_address = completion_address
        self.anchors = anchors
        self.patches = patches
        self.observations = {
            observation.address: observation for observation in observations
        }
        if len(self.observations) != len(observations):
            raise ValueError("Guest observation addresses must be unique")
        if (
            entry_observation is not None
            and entry_observation.address != entry_address
        ):
            raise ValueError("Guest entry observation must use the entry address")
        if transaction_count < 1:
            raise ValueError("Guest transaction count must be positive")
        if not any(
            anchor.address <= completion_address < anchor.address + len(anchor.value)
            for anchor in anchors
        ):
            raise ValueError(
                "Guest completion address must belong to an authenticated anchor"
            )
        self.timeout = timeout
        self.entry_observation = entry_observation
        self.transaction_count = transaction_count
        self.remote: GDBRemote | None = None
        self.running = False
        self.entry_breakpoint = False
        self.completion_breakpoint = False
        self.observation_breakpoints: set[int] = set()
        self.observation_results: list[GuestObservationResult] = []
        self.patched_addresses: set[int] = set()

    @staticmethod
    def _require_pc(remote: GDBRemote, expected: int) -> None:
        pc = remote.read_register(15)
        if pc & ~1 != expected:
            raise RuntimeError(
                f"Guest breakpoint stopped at 0x{pc:x}, expected 0x{expected:x}"
            )

    def _restore(self, remote: GDBRemote, patch: GuestPatch) -> None:
        observed = remote.read_memory(patch.address, len(patch.replacement))
        if observed != patch.replacement:
            raise RuntimeError(
                f"{self.name} {patch.name} changed before restore: "
                f"{observed.hex()}"
            )
        remote.write_memory(patch.address, patch.original)
        if remote.read_memory(patch.address, len(patch.original)) != patch.original:
            raise RuntimeError(f"{self.name} {patch.name} restore failed")
        self.patched_addresses.discard(patch.address)

    def _enter_and_authenticate(self, remote: GDBRemote) -> None:
        while True:
            remote.wait_for_stop()
            self.running = False
            self._require_pc(remote, self.entry_address)

            owned = True
            for anchor in self.anchors:
                try:
                    observed = remote.read_memory(
                        anchor.address, len(anchor.value)
                    )
                except GDBRemoteError:
                    owned = False
                    break
                if observed != anchor.value:
                    owned = False
                    break
            if owned:
                break

            remote.remove_breakpoint(self.entry_address)
            self.entry_breakpoint = False
            remote.step()
            stepped_pc = remote.read_register(15) & ~1
            if stepped_pc == self.entry_address:
                raise RuntimeError(
                    f"{self.name} could not step over an unowned entry "
                    f"mapping at 0x{stepped_pc:x}"
                )
            remote.insert_breakpoint(self.entry_address)
            self.entry_breakpoint = True
            remote.resume()
            self.running = True

        remote.remove_breakpoint(self.entry_address)
        self.entry_breakpoint = False
        for patch in self.patches:
            observed = remote.read_memory(patch.address, len(patch.original))
            if observed != patch.original:
                raise RuntimeError(
                    f"{self.name} {patch.name} is not original: "
                    f"{observed.hex()}"
                )

    def _apply(self, remote: GDBRemote) -> None:
        for patch in self.patches:
            self.patched_addresses.add(patch.address)
            remote.write_memory(patch.address, patch.replacement)
            if (
                remote.read_memory(patch.address, len(patch.replacement))
                != patch.replacement
            ):
                raise RuntimeError(
                    f"{self.name} {patch.name} patch readback failed"
                )

    def _capture_observation(
        self,
        remote: GDBRemote,
        observation: GuestObservation,
        transaction: int | None = None,
    ) -> None:
        memory = None
        if observation.memory is not None:
            address_register, size = observation.memory
            memory = remote.read_memory(
                remote.read_register(address_register), size
            )
        if transaction is not None:
            observation = GuestObservation(
                observation.address,
                f"{observation.name} {transaction}",
                observation.registers,
                observation.memory,
            )
        self.observation_results.append(
            GuestObservationResult(
                observation,
                tuple(
                    remote.read_register(register)
                    for register in observation.registers
                ),
                memory,
                remote.read_register(14),
            )
        )

    def _completion_mapping_is_owned(self, remote: GDBRemote) -> bool:
        for anchor in self.anchors:
            if not (
                anchor.address
                <= self.completion_address
                < anchor.address + len(anchor.value)
            ):
                continue
            expected = bytearray(anchor.value)
            for patch in self.patches:
                start = max(anchor.address, patch.address)
                end = min(
                    anchor.address + len(anchor.value),
                    patch.address + len(patch.replacement),
                )
                if start < end:
                    expected[start - anchor.address : end - anchor.address] = (
                        patch.replacement[
                            start - patch.address : end - patch.address
                        ]
                    )
            try:
                observed = remote.read_memory(anchor.address, len(anchor.value))
            except GDBRemoteError:
                return False
            if observed != expected:
                return False
        return True

    def arm(self) -> None:
        if self.remote is not None:
            raise RuntimeError(f"{self.name} is already armed")
        self.remote = GDBRemote(
            self.endpoint.host,
            self.endpoint.port,
            timeout=self.timeout,
        )
        try:
            self.remote.stop()
            self.remote.insert_breakpoint(self.entry_address)
            self.entry_breakpoint = True
            self.remote.resume()
            self.running = True
        except BaseException:
            self.cancel()
            raise

    def complete(self) -> None:
        remote = self.remote
        if remote is None or not self.running:
            raise RuntimeError(f"{self.name} is not armed")
        try:
            for transaction in range(1, self.transaction_count + 1):
                self._enter_and_authenticate(remote)
                if self.entry_observation is not None:
                    self._capture_observation(
                        remote, self.entry_observation, transaction
                    )

                remote.insert_breakpoint(self.completion_address)
                self.completion_breakpoint = True
                for address in self.observations:
                    remote.insert_breakpoint(address)
                    self.observation_breakpoints.add(address)
                self._apply(remote)

                remote.resume()
                self.running = True
                while True:
                    remote.wait_for_stop()
                    self.running = False
                    pc = remote.read_register(15) & ~1
                    if pc == self.completion_address:
                        if self._completion_mapping_is_owned(remote):
                            break
                        remote.remove_breakpoint(self.completion_address)
                        self.completion_breakpoint = False
                        remote.step()
                        stepped_pc = remote.read_register(15) & ~1
                        if stepped_pc == self.completion_address:
                            raise RuntimeError(
                                f"{self.name} could not step over an unowned "
                                f"completion mapping at 0x{stepped_pc:x}"
                            )
                        remote.insert_breakpoint(self.completion_address)
                        self.completion_breakpoint = True
                        remote.resume()
                        self.running = True
                        continue
                    observation = self.observations.get(pc)
                    if observation is None:
                        raise RuntimeError(
                            f"Guest stopped at unexpected breakpoint 0x{pc:x}"
                        )
                    self._capture_observation(remote, observation)
                    remote.remove_breakpoint(pc)
                    self.observation_breakpoints.discard(pc)
                    remote.resume()
                    self.running = True

                for patch in reversed(self.patches):
                    self._restore(remote, patch)
                for address in tuple(self.observation_breakpoints):
                    remote.remove_breakpoint(address)
                    self.observation_breakpoints.discard(address)
                remote.remove_breakpoint(self.completion_address)
                self.completion_breakpoint = False
                if transaction < self.transaction_count:
                    remote.insert_breakpoint(self.entry_address)
                    self.entry_breakpoint = True
                    remote.resume()
                    self.running = True

            remote.detach()
            remote.close()
            self.remote = None
        except BaseException:
            if self.observation_results:
                print_guest_observations(self.name, self.observation_results)
            self.cancel()
            raise

    def activate(self) -> None:
        """Keep authenticated patches active until an explicit deactivate."""

        remote = self.remote
        if remote is None or not self.running:
            raise RuntimeError(f"{self.name} is not armed")
        if not self.patches:
            raise RuntimeError(f"{self.name} has no persistent patches")
        try:
            self._enter_and_authenticate(remote)
            if self.entry_observation is not None:
                self._capture_observation(remote, self.entry_observation, 1)
            self._apply(remote)
            remote.resume()
            self.running = True
        except BaseException:
            self.cancel()
            raise

    def deactivate(self) -> None:
        """Stop the Guest, authenticate and restore every active patch."""

        remote = self.remote
        if remote is None or not self.running:
            raise RuntimeError(f"{self.name} is not active")
        try:
            remote.interrupt()
            self.running = False
            for patch in reversed(self.patches):
                self._restore(remote, patch)
            remote.detach()
            remote.close()
            self.remote = None
        except BaseException:
            self.cancel()
            raise

    def handoff(self, successor: "AuthenticatedRuntimePatch") -> None:
        """Atomically restore this owner and arm a successor before resume."""

        remote = self.remote
        if remote is None or not self.running:
            raise RuntimeError(f"{self.name} is not active")
        if successor.remote is not None:
            raise RuntimeError(f"{successor.name} is already armed")
        try:
            remote.interrupt()
            self.running = False
            for patch in reversed(self.patches):
                self._restore(remote, patch)
            remote.insert_breakpoint(successor.entry_address)
            remote.set_timeout(successor.timeout)
            successor.remote = remote
            successor.entry_breakpoint = True
            remote.resume()
            successor.running = True
            self.remote = None
        except BaseException:
            self.cancel()
            successor.cancel()
            raise

    def cancel(self) -> None:
        remote = self.remote
        if remote is None:
            return
        if self.running:
            with contextlib.suppress(Exception):
                remote.interrupt()
            self.running = False
        for patch in reversed(self.patches):
            if patch.address in self.patched_addresses:
                with contextlib.suppress(Exception):
                    self._restore(remote, patch)
        if self.entry_breakpoint:
            with contextlib.suppress(Exception):
                remote.remove_breakpoint(self.entry_address)
            self.entry_breakpoint = False
        if self.completion_breakpoint:
            with contextlib.suppress(Exception):
                remote.remove_breakpoint(self.completion_address)
            self.completion_breakpoint = False
        for address in tuple(self.observation_breakpoints):
            with contextlib.suppress(Exception):
                remote.remove_breakpoint(address)
            self.observation_breakpoints.discard(address)
        with contextlib.suppress(Exception):
            remote.detach()
        remote.close()
        self.remote = None


class IOS2ASRChecksumBypass(AuthenticatedRuntimePatch):
    def __init__(self, endpoint: Endpoint, timeout: float = 300) -> None:
        super().__init__(
            endpoint,
            "5A347 ASR post-copy bypass",
            IOS2_ASR_CHECKSUM_ENTRY_ADDRESS,
            IOS2_ASR_EPILOGUE_ADDRESS,
            (
                GuestAnchor(
                    0x90FC,
                    bytes.fromhex("d99ae99b9a421ad05f480ef07aef"),
                    "checksum gate",
                ),
                GuestAnchor(
                    0x92D8,
                    bytes.fromhex(
                        "0ff096e900b2e790e79b002b63d13d48ea990468201c00f0"
                    ),
                    "volume-resize gate",
                ),
                GuestAnchor(
                    0x93AE,
                    bytes.fromhex("eb98002801d00ff098e8ea98"),
                    "cleanup epilogue",
                ),
                GuestAnchor(
                    0x91D0,
                    bytes.fromhex(
                        "0123009380225b460193ea980221920002230ff088e984b2"
                        "00b2e790002c00d0dde05d46584602f060fa2a88294b9a42"
                    ),
                    "pre-resize volume-header read",
                ),
                GuestAnchor(
                    0x9428,
                    bytes.fromhex(
                        "0194d84780b2002802d0002398464ae0281c02f03ef9"
                        "2388304a934235d17c232f4aeb5a934202d02d3293424ad1"
                    ),
                    "primary volume-header reader",
                ),
                GuestAnchor(
                    0x948C,
                    bytes.fromhex(
                        "50460221d84780b2002801d000b229e0201c02f02cf8"
                        "2088184b984213d02d3398421dd10fe0"
                    ),
                    "embedded volume-header reader",
                ),
                GuestAnchor(
                    0x98F8,
                    bytes.fromhex(
                        "fff77efd80b2002811d102a8fff7c8ff02a801f063fb"
                        "0122829b009202aa019280220233201c03219200a84780b2"
                    ),
                    "volume-header update operation",
                ),
            ),
            (
                GuestPatch(
                    0x9102,
                    bytes.fromhex("1ad0"),
                    bytes.fromhex("1ae0"),
                    "checksum branch",
                ),
                GuestPatch(
                    0x92E4,
                    bytes.fromhex("63d1"),
                    bytes.fromhex("c046"),
                    "volume-resize branch",
                ),
            ),
            (
                GuestObservation(0x9100, "block checksum", (2, 3)),
                GuestObservation(
                    0x91E6,
                    "pre-resize primary-header media read",
                    (0, 11),
                    (11, 64),
                ),
                GuestObservation(
                    0x91FC,
                    "pre-resize primary-header signature",
                    (2,),
                ),
                GuestObservation(0x92DC, "VResizeVolume"),
                GuestObservation(
                    0x942C,
                    "primary-header media read",
                    (0, 4),
                    (4, 64),
                ),
                GuestObservation(0x9444, "primary-header signature", (3,)),
                GuestObservation(0x9454, "HFS-wrapper signature", (3,)),
                GuestObservation(0x9492, "embedded-header media read"),
                GuestObservation(0x94A6, "embedded-header signature"),
                GuestObservation(0x9924, "primary-header media write"),
                GuestObservation(0x92F2, "volume-header update"),
                GuestObservation(0x9318, "HFS wrapper conversion"),
                GuestObservation(0x9338, "volume-size read"),
                GuestObservation(0x934E, "alternate-header copy"),
            ),
            timeout,
        )


class IOS4ASRSignatureBypass(AuthenticatedRuntimePatch):
    """Redirect only 8C148 ASR's authenticated signature-failure path."""

    def __init__(self, endpoint: Endpoint, timeout: float = 900) -> None:
        super().__init__(
            endpoint,
            "8C148 ASR image-signature bypass",
            IOS4_ASR_SIGNATURE_FAILURE_ADDRESS,
            IOS4_ASR_SIGNATURE_EPILOGUE_ADDRESS,
            (
                GuestAnchor(
                    0x14C2C,
                    bytes.fromhex(
                        "654b0899cb5c002b09d0644a56a8891814220ff046e9"
                        "002800d19fe0a9e0119b002b00d1a5e0181c0ef03aef"
                        "5c4c8025"
                    ),
                    "signature digest gate",
                ),
                GuestAnchor(
                    0x14D88,
                    bytes.fromhex(
                        "164b7b441b681b68002b59d0144878440ff028e954e0"
                        "1348502478440ff022e9a34648e0"
                    ),
                    "signature result branches",
                ),
                GuestAnchor(
                    0x14DF0,
                    bytes.fromhex(
                        "0f985d460ff01ce8002d29d00024184b7b441b78002b"
                        "12d1584600f075fa0899144b5a460a20ca5013497944"
                        "096809680ff086e81149584679440ff0dee8002c0ed0"
                        "09980ef0ccef0ae00c239b460f980ef0f6ef0124dae7"
                        "0f980ef0f2eff0e75eb000201cbc90469a46a346f0bd"
                        "c046"
                    ),
                    "signature cleanup epilogue",
                ),
            ),
            (
                GuestPatch(
                    IOS4_ASR_SIGNATURE_FAILURE_ADDRESS,
                    bytes.fromhex("1348"),
                    bytes.fromhex("f3e7"),
                    "failure-to-success branch",
                ),
            ),
            timeout=timeout,
        )


class IOS2PartitionReuseBypass(AuthenticatedRuntimePatch):
    def __init__(self, endpoint: Endpoint, timeout: float = 300) -> None:
        super().__init__(
            endpoint,
            "5A347 partition-creation bypass",
            IOS2_RESTORED_PARTITION_GATE_ADDRESS,
            IOS2_RESTORED_PARTITION_EPILOGUE_ADDRESS,
            (
                GuestAnchor(
                    0x62E4,
                    bytes.fromhex("7000efe6000050e30040a0016301000a0b10a0e3"),
                    "partition gate",
                ),
                GuestAnchor(
                    0x6884,
                    bytes.fromhex("0400a0e118d047e2000dbde8f080bde8"),
                    "partition epilogue",
                ),
            ),
            (
                GuestPatch(
                    0x62EC,
                    bytes.fromhex("0040a001"),
                    bytes.fromhex("0040a0e3"),
                    "zero-result instruction",
                ),
                GuestPatch(
                    0x62F0,
                    bytes.fromhex("6301000a"),
                    bytes.fromhex("630100ea"),
                    "partition skip branch",
                ),
            ),
            (),
            timeout,
        )


class IOS2FilesystemReuseBypass(AuthenticatedRuntimePatch):
    def __init__(self, endpoint: Endpoint, timeout: float = 300) -> None:
        super().__init__(
            endpoint,
            "5A347 filesystem-reuse bypass",
            IOS2_RESTORED_PARTITION_GATE_ADDRESS,
            IOS2_RESTORED_IMAGE_EPILOGUE_ADDRESS,
            (
                GuestAnchor(
                    0x62E4,
                    bytes.fromhex("7000efe6000050e30040a0016301000a0b10a0e3"),
                    "partition gate",
                ),
                GuestAnchor(
                    0x6884,
                    bytes.fromhex("0400a0e118d047e2000dbde8f080bde8"),
                    "partition epilogue",
                ),
                GuestAnchor(
                    0x6934,
                    bytes.fromhex(
                        "f0402de90c708de220d04de20140a0e10050a0e1"
                        "c0109fe5c0009fe50260a0e16ef6ffeb0030a0e3"
                    ),
                    "system-image entry",
                ),
                GuestAnchor(
                    0x697C,
                    bytes.fromhex(
                        "c20000eb000000ea0100a0e37000efe6010050e3"
                        "0000a0131b00001a"
                    ),
                    "system-image decision",
                ),
                GuestAnchor(
                    0x6A08,
                    bytes.fromhex("0cd047e2f080bde8"),
                    "system-image epilogue",
                ),
            ),
            (
                GuestPatch(
                    0x62EC,
                    bytes.fromhex("0040a001"),
                    bytes.fromhex("0040a0e3"),
                    "partition zero-result instruction",
                ),
                GuestPatch(
                    0x62F0,
                    bytes.fromhex("6301000a"),
                    bytes.fromhex("630100ea"),
                    "partition skip branch",
                ),
                GuestPatch(
                    0x6990,
                    bytes.fromhex("0000a013"),
                    bytes.fromhex("0000a0e3"),
                    "system-image zero-result instruction",
                ),
                GuestPatch(
                    0x6994,
                    bytes.fromhex("1b00001a"),
                    bytes.fromhex("1b0000ea"),
                    "system-image skip branch",
                ),
            ),
            (),
            timeout,
        )


class IOS2PartitionWipeBypass(AuthenticatedRuntimePatch):
    def __init__(self, endpoint: Endpoint, timeout: float = 300) -> None:
        super().__init__(
            endpoint,
            "5A347 partition-wipe bypass",
            IOS2_RESTORED_PARTITION_GATE_ADDRESS,
            IOS2_RESTORED_PARTITION_EPILOGUE_ADDRESS,
            (
                GuestAnchor(
                    0x62E4,
                    bytes.fromhex("7000efe6000050e30040a0016301000a0b10a0e3"),
                    "partition gate",
                ),
                GuestAnchor(
                    0x67D8,
                    bytes.fromhex(
                        "005096e50000e0e30510a0e11420d6e5048016e5"
                        "4afaffeb004050e21100001a18319fe5"
                    ),
                    "partition wipe call",
                ),
                GuestAnchor(
                    0x6884,
                    bytes.fromhex("0400a0e118d047e2000dbde8f080bde8"),
                    "partition epilogue",
                ),
            ),
            (
                GuestPatch(
                    IOS2_RESTORED_PARTITION_WIPE_CALL_ADDRESS,
                    bytes.fromhex("4afaffeb"),
                    bytes.fromhex("0000a0e3"),
                    "partition wipe call",
                ),
            ),
            (),
            timeout,
        )


class IOS2NORFlashProbe(AuthenticatedRuntimePatch):
    """Observe authenticated 5A347 AppleImage3NORAccess transactions."""

    def __init__(
        self,
        endpoint: Endpoint,
        bypass_integrity: bool = False,
        timeout: float = 300,
    ) -> None:
        super().__init__(
            endpoint,
            (
                "5A347 NOR integrity bypass"
                if bypass_integrity
                else "5A347 NOR flash probe"
            ),
            IOS2_NOR_FLASH_ENTRY_ADDRESS,
            IOS2_NOR_FLASH_EPILOGUE_ADDRESS,
            (
                GuestAnchor(
                    0xC0399D94,
                    bytes.fromhex(
                        "f0402de90c708de2000d2de910d04de20140a0e10060a0e1"
                        "7c139fe5780090e578339fe50280a0e133ff2fe1005050e2"
                        "6c439f15d400001a010b58e3bd00009a"
                    ),
                    "transaction entry",
                ),
                GuestAnchor(
                    0xC0399F90,
                    bytes.fromhex(
                        "04309de5024b85e2201083e208309de50400a0e1082093e5"
                        "a0319fe533ff2fe10600a0e10410a0e10b20a0e1103085e2"
                        "54ffffeb000050e34500000a88219fe50c009de5a0119fe5"
                        "32ff2fe1002050e20230a0130800001a90319fe50600a0e1"
                        "00308de50410a0e10b30a0e1f7fbffeb000050e30130a013"
                        "3500000a4020a0e30730c5e50600a0e10510a0e1023085e0"
                        "3cffffeb000050e32d00000a50319fe57c0096e50510a0e1"
                        "0a20a0e133ff2fe1fc309fe5000050e30340a0010040a013"
                        "2b0000ea"
                    ),
                    "integrity and flash path",
                ),
                GuestAnchor(
                    0xC039A0D4,
                    bytes.fromhex(
                        "0040a0e304a0a0e10450a0e1070000ea60409fe5050000ea"
                        "58409fe50050a0e3020000ea4c409fe50050a0e305a0a0e1"
                        "04009de50810a0e17c309fe533ff2fe1000055e3e0ffff1a"
                        "e3ffffea0400a0e118d047e2000dbde8f080bde8"
                    ),
                    "transaction epilogue",
                ),
            ),
            (
                (
                    GuestPatch(
                        0xC0399FC8,
                        bytes.fromhex("4500000a"),
                        bytes.fromhex("0000a0e1"),
                        "first integrity failure branch",
                    ),
                    GuestPatch(
                        0xC039A028,
                        bytes.fromhex("2d00000a"),
                        bytes.fromhex("0000a0e1"),
                        "header integrity failure branch",
                    ),
                )
                if bypass_integrity
                else ()
            ),
            (
                GuestObservation(
                    0xC0399FC0,
                    "first integrity input",
                    (0, 1, 2, 3),
                    (1, 64),
                ),
                GuestObservation(0xC0399FC4, "first integrity result"),
                GuestObservation(0xC0399FDC, "SHSH lookup", (0,)),
                GuestObservation(0xC039A000, "optional decrypt result"),
                GuestObservation(
                    0xC039A020,
                    "header integrity input",
                    (0, 1, 2, 3),
                    (1, 64),
                ),
                GuestObservation(0xC039A024, "header integrity result"),
                GuestObservation(0xC039A040, "NOR write result"),
                GuestObservation(0xC039A104, "terminal state", (4, 5, 10)),
            ),
            timeout=timeout,
            entry_observation=GuestObservation(
                IOS2_NOR_FLASH_ENTRY_ADDRESS,
                "transaction entry",
                (1, 2),
                (1, 64),
            ),
            transaction_count=1 if bypass_integrity else 2,
        )


class IOS4NORFlashProbe(AuthenticatedRuntimePatch):
    """Observe the authenticated 8C148 AppleImage3NORAccess transaction."""

    def __init__(
        self,
        endpoint: Endpoint,
        timeout: float = 900,
        bypass_write_failure: bool = False,
        workaround_catalog_data_length: bool = False,
        transaction_count: int = 1,
    ) -> None:
        super().__init__(
            endpoint,
            (
                "8C148 NOR provider-failure bypass"
                if bypass_write_failure
                else (
                    "8C148 catalog DATA-length workaround"
                    if workaround_catalog_data_length
                    else "8C148 NOR flash probe"
                )
            ),
            0x805A66F8,
            0x805A67C4,
            (
                GuestAnchor(
                    0x805A66F8,
                    bytes.fromhex(
                        "f0b5464640b404af81b000230093041c0d1c806f"
                    ),
                    "transaction entry",
                ),
                GuestAnchor(
                    0x805A6776,
                    bytes.fromhex(
                        "0098fff73afe002805d1201c4146fff76efd0028"
                        "0cd0201cfff72ffb"
                    ),
                    "direct layout decision",
                ),
                GuestAnchor(
                    0x805A6264,
                    bytes.fromhex(
                        "f0b55646454660b405afc5b04b7b0a7b82461b02"
                        "1a438b7b1b041a43cb7b1b0613431433c818437a"
                    ),
                    "SHSH decrypt helper",
                ),
                GuestAnchor(
                    0x805A67A2,
                    bytes.fromhex(
                        "002400e0174c174b4046311c9847009b002b04d0"
                        "6846144b984700e0114c01b0201c04bc9046f0bd"
                    ),
                    "result paths",
                ),
                GuestAnchor(
                    0x805A67C0,
                    bytes.fromhex(
                        "01b0201c04bc9046f0bdc04678845a80"
                    ),
                    "transaction epilogue",
                ),
                GuestAnchor(
                    0x805A6810,
                    bytes.fromhex(
                        "f0b55e465546444670b406af87b000230693041c0d1c806f"
                        "9249934b161c9847002801d0914c15e18023db009e4200d8"
                    ),
                    "catalog transaction entry",
                ),
                GuestAnchor(
                    0x805A68A0,
                    bytes.fromhex(
                        "00237f4da847002800d0d4e07d4b06987d49019398470028"
                        "00d0cce0069802217a4b9847002800d0c5e00698fff790fd"
                        "051e00d0bfe0"
                    ),
                    "catalog IMG3 validation paths",
                ),
                GuestAnchor(
                    0x805A691C,
                    bytes.fromhex(
                        "9a4500d09be0e06f634b9847039000286bd0009506986149"
                        "05aa04ab604da8478246002800d08ae0049b8021039a0901"
                        "5b189a4200d882e0101c1021594b9847051e72d0"
                    ),
                    "catalog layout paths",
                ),
                GuestAnchor(
                    0x805A692E,
                    bytes.fromhex(
                        "00950698614905aa04ab604da8478246002800d08ae0"
                    ),
                    "catalog DATA lookup call site",
                ),
                GuestAnchor(
                    0x805A69AC,
                    bytes.fromhex(
                        "201c51465a46fff733fc002845d006984249019a90470028"
                        "0bd1414b201c5146009300225b46fff795fb002835d00123"
                        "00e00223eb712b1c"
                    ),
                    "catalog integrity paths",
                ),
                GuestAnchor(
                    0x805A69E4,
                    bytes.fromhex(
                        "4033201c291c4022fff716fc002828d0e06f291c039a"
                        "354b984700281fd1224c20e0201c4146fff72bfc0028"
                        "21d0e06f414652462d4b9847002801d000241be0194c"
                        "19e0"
                    ),
                    "catalog provider result paths",
                ),
                GuestAnchor(
                    0x805A6A28,
                    bytes.fromhex(
                        "03992a4b281c9847069b002b16d006a8274b984712e0154c"
                        "10e0002400e0134c4046311c234b9847002de9d1ece7224c"
                        "00e00e4c1f4b4046311ce4e707b0201c1cbc90469a46a346"
                        "f0bd"
                    ),
                    "catalog transaction epilogue",
                ),
                GuestAnchor(
                    0x805A6DEE,
                    bytes.fromhex(
                        "002e0dd14b46002b06d01b68002b01d09c421cd1"
                        "4a46146043461860002017e0"
                    ),
                    "IMG3 tag-length output path",
                ),
            ),
            (
                GuestPatch(
                    0x805A679C,
                    bytes.fromhex("01d1"),
                    bytes.fromhex("01e0"),
                    "provider-failure branch",
                ),
                GuestPatch(
                    0x805A678A,
                    bytes.fromhex("0cd0"),
                    bytes.fromhex("c046"),
                    "direct layout-failure branch",
                ),
                GuestPatch(
                    0x805A6A00,
                    bytes.fromhex("1fd1"),
                    bytes.fromhex("1fe0"),
                    "catalog provider-failure branch",
                ),
                GuestPatch(
                    0x805A6A1E,
                    bytes.fromhex("01d0"),
                    bytes.fromhex("c046"),
                    "catalog direct-provider failure branch",
                ),
            ) if bypass_write_failure else (),
            (
                GuestObservation(0x805A6714, "provider lookup result", (0,)),
                GuestObservation(0x805A675A, "IMG3 parse result", (0,)),
                GuestObservation(0x805A677C, "IMG3 integrity result", (0,)),
                GuestObservation(
                    0x805A62CC,
                    "SHSH decrypt input",
                    (4, 8),
                    (4, 128),
                ),
                GuestObservation(
                    0x805A62D0,
                    "SHSH decrypt result",
                    (0, 4, 8),
                    (8, 128),
                ),
                GuestObservation(0x805A6788, "direct layout result", (0,)),
                GuestObservation(0x805A679A, "provider write result", (0,)),
                GuestObservation(0x805A67A6, "post-parse rejection"),
                GuestObservation(0x805A67BE, "input-size rejection", (2,)),
                GuestObservation(0x805A67C0, "transaction result", (4,)),
            ),
            timeout=timeout,
            entry_observation=GuestObservation(
                0x805A66F8,
                "transaction entry",
                (1, 2),
            ),
            transaction_count=transaction_count,
        )

        self.catalog_entry = GuestObservation(
            0x805A6810, "catalog transaction entry", (1, 2)
        )
        self.catalog_completion_address = 0x805A6A64
        self.catalog_observations = {
            observation.address: observation
            for observation in (
                GuestObservation(0x805A6830, "catalog provider lookup result", (0,)),
                GuestObservation(0x805A6846, "catalog input-size comparison", (2, 6)),
                GuestObservation(0x805A6860, "catalog provider readiness result", (0,)),
                GuestObservation(0x805A687A, "catalog allocation result", (0,)),
                GuestObservation(
                    0x805A68A6,
                    "catalog IMG3 construction result",
                    (0,),
                    (8, 256),
                ),
                GuestObservation(0x805A68B6, "catalog IMG3 tag result", (0,)),
                GuestObservation(0x805A68C4, "catalog IMG3 type result", (0,)),
                GuestObservation(0x805A68D0, "catalog IMG3 integrity result", (0,)),
                GuestObservation(0x805A691C, "catalog rounded-size comparison", (3, 10)),
                GuestObservation(0x805A692A, "catalog provider object result", (0,)),
                GuestObservation(
                    0x805A6930,
                    "catalog post-validation IMG3 buffer",
                    (8,),
                    (8, 256),
                ),
                GuestObservation(
                    0x805A6938,
                    "catalog DATA lookup input",
                    (0, 1, 2, 3),
                    (0, 64),
                ),
                GuestObservation(
                    0x805A693C,
                    "catalog DATA lookup result",
                    (0,),
                    (13, 32),
                ),
                GuestObservation(
                    0x805A6DFE,
                    "catalog DATA length comparison",
                    (3, 4),
                ),
                GuestObservation(0x805A695C, "catalog work-buffer result", (0,)),
                GuestObservation(
                    0x805A69B2,
                    "catalog manifest-record preparation input",
                    (0, 1, 2, 3),
                    (1, 64),
                ),
                GuestObservation(
                    0x805A6228,
                    "catalog manifest-record alignment",
                    (1, 2, 3),
                ),
                GuestObservation(
                    0x805A6236,
                    "catalog manifest-record digest result",
                    (0,),
                ),
                GuestObservation(
                    0x805A610E,
                    "catalog AES preparation input",
                    (0, 1, 2, 3),
                ),
                GuestObservation(
                    0x805A611A,
                    "catalog cached AES accelerator",
                    (0, 3),
                ),
                GuestObservation(
                    0x805A6126,
                    "catalog AES class lookup result",
                    (0,),
                ),
                GuestObservation(
                    0x805A6130,
                    "catalog AES service lookup result",
                    (0,),
                ),
                GuestObservation(
                    0x805A613C,
                    "catalog AES accelerator result",
                    (0,),
                ),
                GuestObservation(
                    0x805A6166,
                    "catalog AES source descriptor result",
                    (0,),
                ),
                GuestObservation(
                    0x805A617C,
                    "catalog AES destination descriptor result",
                    (0,),
                ),
                GuestObservation(
                    0x805A61A6,
                    "catalog AES request input",
                    (0, 1, 2, 3),
                ),
                GuestObservation(
                    0x805A61A8,
                    "catalog AES request result",
                    (0,),
                ),
                GuestObservation(
                    0x805A6254,
                    "catalog manifest-record AES result",
                    (0,),
                ),
                GuestObservation(0x805A69B6, "catalog layout result", (0,)),
                GuestObservation(0x805A69C2, "catalog integrity-policy result", (0,)),
                GuestObservation(0x805A69D6, "catalog secondary-layout result", (0,)),
                GuestObservation(0x805A69F0, "catalog record-layout result", (0,)),
                GuestObservation(0x805A69FE, "catalog provider write result", (0,)),
                GuestObservation(0x805A6A0E, "catalog direct-layout result", (0,)),
                GuestObservation(0x805A6A1C, "catalog direct-provider result", (0,)),
                GuestObservation(0x805A6A3E, "catalog input-size rejection"),
                GuestObservation(0x805A6A46, "catalog provider rejection"),
                GuestObservation(0x805A6A56, "catalog IMG3 rejection"),
                GuestObservation(0x805A6A5A, "catalog layout rejection"),
                GuestObservation(0x805A6A64, "catalog transaction result", (4,)),
            )
        }
        self.catalog_data_length_workaround_address: int | None = None
        if workaround_catalog_data_length:
            observation = GuestObservation(
                0x805A692E,
                "catalog uninitialized DATA-length local",
                (5, 13),
                (13, 24),
            )
            self.catalog_observations[observation.address] = observation
            self.catalog_data_length_workaround_address = observation.address
        self.batch_completion_address: int | None = None
        self.catalog_entry_breakpoint = False
        self.batch_successor_active = False
        self.batch_ir_mcu_complete = False

    def _initialize_catalog_data_length(self, remote: GDBRemote) -> None:
        """Initialize only the malformed 8C148 catalog call's local input."""

        anchor = next(
            item
            for item in self.anchors
            if item.name == "catalog DATA lookup call site"
        )
        if remote.read_memory(anchor.address, len(anchor.value)) != anchor.value:
            raise RuntimeError(
                f"{self.name} {anchor.name} changed before data workaround"
            )
        stack_pointer = remote.read_register(13)
        length_address = stack_pointer + 0x10
        prior = remote.read_memory(length_address, 4)
        if prior != bytes(4):
            remote.write_memory(length_address, bytes(4))
        if remote.read_memory(length_address, 4) != bytes(4):
            raise RuntimeError(
                f"{self.name} catalog DATA-length initialization failed"
            )
        print(
            "authenticated 8C148 catalog DATA-length local initialized: "
            f"0x{int.from_bytes(prior, 'little'):08x} -> 0x00000000",
            flush=True,
        )

    def arm(self) -> None:
        """Arm both entry points because LLB may use the catalog handler."""

        if self.transaction_count == 1:
            super().arm()
            return
        if self.remote is not None:
            raise RuntimeError(f"{self.name} is already armed")
        self.remote = GDBRemote(
            self.endpoint.host, self.endpoint.port, timeout=self.timeout
        )
        try:
            self.remote.stop()
            self.remote.insert_breakpoint(self.entry_address)
            self.remote.insert_breakpoint(self.catalog_entry.address)
            self.entry_breakpoint = True
            self.catalog_entry_breakpoint = True
            self.remote.resume()
            self.running = True
        except BaseException:
            self.cancel()
            raise

    def _enter_batch_transaction(self, remote: GDBRemote) -> bool:
        """Authenticate and consume either direct or catalog entry."""

        remote.wait_for_stop()
        self.running = False
        pc = remote.read_register(15) & ~1
        if pc not in (self.entry_address, self.catalog_entry.address):
            raise RuntimeError(f"Guest stopped at unexpected NOR entry 0x{pc:x}")
        remote.remove_breakpoint(self.entry_address)
        remote.remove_breakpoint(self.catalog_entry.address)
        self.entry_breakpoint = False
        self.catalog_entry_breakpoint = False
        for anchor in self.anchors:
            if remote.read_memory(anchor.address, len(anchor.value)) != anchor.value:
                raise RuntimeError(
                    f"{self.name} {anchor.name} authentication failed"
                )
        for patch in self.patches:
            if remote.read_memory(patch.address, len(patch.original)) != patch.original:
                raise RuntimeError(
                    f"{self.name} {patch.name} original bytes changed"
                )
        return pc == self.catalog_entry.address

    def cancel(self) -> None:
        """Remove alternate-path breakpoints before common patch cleanup."""

        remote = self.remote
        if remote is not None and self.running:
            with contextlib.suppress(Exception):
                remote.interrupt()
            self.running = False
        if (
            remote is not None
            and self.completion_breakpoint
            and self.batch_completion_address == self.catalog_completion_address
        ):
            with contextlib.suppress(Exception):
                remote.remove_breakpoint(self.catalog_completion_address)
            self.completion_breakpoint = False
        if remote is not None and self.catalog_entry_breakpoint:
            with contextlib.suppress(Exception):
                remote.remove_breakpoint(self.catalog_entry.address)
            self.catalog_entry_breakpoint = False
        super().cancel()

    def probe_direct_batch(
        self,
        successor: "AuthenticatedRuntimePatch | None" = None,
        concurrent_ir_mcu: "AuthenticatedRuntimePatch | None" = None,
    ) -> None:
        """Trace a mixed direct/catalog batch under one patch owner."""

        remote = self.remote
        if remote is None or not self.running:
            raise RuntimeError(f"{self.name} is not armed")
        if concurrent_ir_mcu is not None and successor is None:
            raise ValueError("the concurrent IR-MCU owner requires a successor")

        successor_active = False
        ir_mcu_complete = concurrent_ir_mcu is None

        def mapping_is_owned(owner: AuthenticatedRuntimePatch) -> bool:
            for anchor in owner.anchors:
                try:
                    observed = remote.read_memory(anchor.address, len(anchor.value))
                except GDBRemoteError:
                    return False
                if observed != anchor.value:
                    return False
            return True

        def step_unowned_entry(owner: AuthenticatedRuntimePatch) -> None:
            remote.remove_breakpoint(owner.entry_address)
            owner.entry_breakpoint = False
            remote.step()
            stepped_pc = remote.read_register(15) & ~1
            if stepped_pc == owner.entry_address:
                raise RuntimeError(
                    f"{owner.name} could not step over an unowned entry "
                    f"mapping at 0x{stepped_pc:x}"
                )
            remote.insert_breakpoint(owner.entry_address)
            owner.entry_breakpoint = True
            remote.resume()
            self.running = True

        def handle_auxiliary_stop(pc: int) -> bool:
            nonlocal ir_mcu_complete, successor_active

            if (
                concurrent_ir_mcu is not None
                and concurrent_ir_mcu.entry_breakpoint
                and pc == concurrent_ir_mcu.entry_address
            ):
                if not mapping_is_owned(concurrent_ir_mcu):
                    step_unowned_entry(concurrent_ir_mcu)
                    return True
                remote.remove_breakpoint(concurrent_ir_mcu.entry_address)
                concurrent_ir_mcu.entry_breakpoint = False
                for patch in concurrent_ir_mcu.patches:
                    if remote.read_memory(
                        patch.address, len(patch.original)
                    ) != patch.original:
                        raise RuntimeError(
                            f"{concurrent_ir_mcu.name} {patch.name} "
                            "original bytes changed"
                        )
                concurrent_ir_mcu.accept_unsupported_result(remote)
                ir_mcu_complete = True
                print(
                    "authenticated 8C148 IR-MCU result accepted at entry",
                    flush=True,
                )
                remote.resume()
                self.running = True
                return True

            if (
                successor is not None
                and successor.entry_breakpoint
                and pc == successor.entry_address
            ):
                if not mapping_is_owned(successor):
                    step_unowned_entry(successor)
                    return True
                remote.remove_breakpoint(successor.entry_address)
                successor.entry_breakpoint = False
                for patch in successor.patches:
                    if remote.read_memory(
                        patch.address, len(patch.original)
                    ) != patch.original:
                        raise RuntimeError(
                            f"{successor.name} {patch.name} original bytes changed"
                        )
                successor._apply(remote)
                successor_active = True
                print(
                    "authenticated 8C148 external-baseband entry accepted",
                    flush=True,
                )
                remote.resume()
                self.running = True
                return True

            return False

        def wait_for_nor_stop(
            return_when_auxiliary_complete: bool = False,
        ) -> int | None:
            while True:
                remote.wait_for_stop()
                self.running = False
                pc = remote.read_register(15) & ~1
                if handle_auxiliary_stop(pc):
                    if (
                        return_when_auxiliary_complete
                        and successor_active
                        and ir_mcu_complete
                    ):
                        return None
                    continue
                return pc

        def cleanup_auxiliary(owner: AuthenticatedRuntimePatch | None) -> None:
            if owner is None:
                return
            for patch in reversed(owner.patches):
                if patch.address in owner.patched_addresses:
                    with contextlib.suppress(Exception):
                        owner._restore(remote, patch)
            if owner.entry_breakpoint:
                with contextlib.suppress(Exception):
                    remote.remove_breakpoint(owner.entry_address)
                owner.entry_breakpoint = False
            if owner.completion_breakpoint:
                with contextlib.suppress(Exception):
                    remote.remove_breakpoint(owner.completion_address)
                owner.completion_breakpoint = False

        try:
            catalog = self._enter_batch_transaction(remote)
            entry_observation = self.catalog_entry if catalog else self.entry_observation
            if entry_observation is not None:
                self._capture_observation(remote, entry_observation, 1)
            self._apply(remote)
            if successor is not None:
                if successor.remote is not None:
                    raise RuntimeError(f"{successor.name} is already armed")
                remote.insert_breakpoint(successor.entry_address)
                successor.entry_breakpoint = True
            if concurrent_ir_mcu is not None:
                remote.insert_breakpoint(concurrent_ir_mcu.entry_address)
                concurrent_ir_mcu.entry_breakpoint = True
                assert successor is not None
            for transaction in range(1, self.transaction_count + 1):
                completion_address = (
                    self.catalog_completion_address
                    if catalog
                    else self.completion_address
                )
                observations = (
                    self.catalog_observations if catalog else self.observations
                )
                remote.insert_breakpoint(completion_address)
                self.batch_completion_address = completion_address
                self.completion_breakpoint = True
                for address in observations:
                    if address == completion_address:
                        continue
                    remote.insert_breakpoint(address)
                    self.observation_breakpoints.add(address)
                remote.resume()
                self.running = True
                while True:
                    pc = wait_for_nor_stop()
                    if pc == completion_address:
                        observation = observations.get(pc)
                        if observation is not None:
                            self._capture_observation(
                                remote, observation, transaction
                            )
                        break
                    observation = observations.get(pc)
                    if observation is None:
                        raise RuntimeError(
                            f"Guest stopped at unexpected breakpoint 0x{pc:x}"
                        )
                    self._capture_observation(remote, observation, transaction)
                    if pc == self.catalog_data_length_workaround_address:
                        self._initialize_catalog_data_length(remote)
                    remote.remove_breakpoint(pc)
                    self.observation_breakpoints.discard(pc)
                    remote.resume()
                    self.running = True

                for address in tuple(self.observation_breakpoints):
                    remote.remove_breakpoint(address)
                    self.observation_breakpoints.discard(address)
                remote.remove_breakpoint(completion_address)
                self.completion_breakpoint = False
                self.batch_completion_address = None
                if transaction < self.transaction_count:
                    remote.insert_breakpoint(self.entry_address)
                    remote.insert_breakpoint(self.catalog_entry.address)
                    self.catalog_entry_breakpoint = True
                    self.entry_breakpoint = True
                    remote.resume()
                    self.running = True
                    pc = wait_for_nor_stop()
                    if pc not in (self.entry_address, self.catalog_entry.address):
                        raise RuntimeError(
                            f"Guest stopped at unexpected NOR entry 0x{pc:x}"
                        )
                    remote.remove_breakpoint(self.entry_address)
                    remote.remove_breakpoint(self.catalog_entry.address)
                    self.catalog_entry_breakpoint = False
                    self.entry_breakpoint = False
                    catalog = pc == self.catalog_entry.address
                    entry_anchor = self.anchors[4] if catalog else self.anchors[0]
                    if remote.read_memory(
                        entry_anchor.address, len(entry_anchor.value)
                    ) != entry_anchor.value:
                        raise RuntimeError(
                            f"{self.name} entry mapping changed"
                        )
                    entry_observation = (
                        self.catalog_entry if catalog else self.entry_observation
                    )
                    if entry_observation is not None:
                        self._capture_observation(remote, entry_observation, transaction + 1)

            for patch in reversed(self.patches):
                self._restore(remote, patch)
            if successor is None:
                remote.detach()
                remote.close()
                self.remote = None
            else:
                assert successor is not None
                remote.set_timeout(10)
                remote.resume()
                self.running = True
                post_nor_timeout = False
                while not (successor_active and ir_mcu_complete):
                    try:
                        pc = wait_for_nor_stop(
                            return_when_auxiliary_complete=True
                        )
                    except TimeoutError:
                        print(
                            "bounded post-NOR handoff expired: "
                            f"ir_mcu_complete={ir_mcu_complete} "
                            f"baseband_active={successor_active}",
                            flush=True,
                        )
                        post_nor_timeout = True
                        break
                    if pc is None:
                        break
                    raise RuntimeError(
                        f"Guest stopped at unexpected post-NOR breakpoint 0x{pc:x}"
                    )
                if post_nor_timeout:
                    remote.set_timeout(max(self.timeout, successor.timeout))
                    remote.interrupt()
                    self.running = False
                for owner in (concurrent_ir_mcu, successor):
                    if owner is None:
                        continue
                    if owner.entry_breakpoint:
                        remote.remove_breakpoint(owner.entry_address)
                        owner.entry_breakpoint = False
                self.batch_successor_active = successor_active
                self.batch_ir_mcu_complete = ir_mcu_complete
                if successor_active:
                    remote.set_timeout(successor.timeout)
                    successor.remote = remote
                    if not self.running:
                        remote.resume()
                    successor.running = True
                    self.remote = None
                else:
                    remote.detach()
                    remote.close()
                    self.remote = None
        except BaseException:
            if self.observation_results:
                print_guest_observations(self.name, self.observation_results)
            cleanup_auxiliary(concurrent_ir_mcu)
            cleanup_auxiliary(successor)
            self.cancel()
            raise


class IOS4BasebandBypass(AuthenticatedRuntimePatch):
    """Skip the external N82 baseband update after authenticating restored."""

    def __init__(self, endpoint: Endpoint, timeout: float = 900) -> None:
        super().__init__(
            endpoint,
            "8C148 external-baseband bypass",
            0x0000AB58,
            0x0000ADA8,
            (
                GuestAnchor(
                    0x0000AB58,
                    bytes.fromhex(
                        "f0402de90c708de2000d2de948d04de20140a0e1"
                        "0060a0e134129fe534029fe5"
                    ),
                    "baseband operation entry",
                ),
                GuestAnchor(
                    0x0000AB84,
                    bytes.fromhex(
                        "28129fe50400a0e10120a0e301108fe040f3ffeb"
                        "000050e30d00000a82f3ffeb005050e20400001a"
                    ),
                    "UpdateBaseband decision",
                ),
                GuestAnchor(
                    0x0000AD80,
                    bytes.fromhex(
                        "000050e30400000a810402eb0010a0e14c009fe5"
                        "00008fe0eb0b00eb0600a0e118d047e2000dbde8"
                        "f080bde8"
                    ),
                    "baseband operation epilogue",
                ),
            ),
            (
                GuestPatch(
                    0x0000AB98,
                    bytes.fromhex("000050e30d00000a"),
                    bytes.fromhex("0000a0e30d0000ea"),
                    "UpdateBaseband disabled result and branch",
                ),
            ),
            (),
            timeout=timeout,
        )


class IOS4IRMCUBypass(AuthenticatedRuntimePatch):
    """Accept the absent N82 IR MCU without retaining a code patch."""

    def __init__(self, endpoint: Endpoint, timeout: float = 1800) -> None:
        super().__init__(
            endpoint,
            "8C148 external IR-MCU bypass",
            # NOR programming and the IR-MCU updater can overlap in separate
            # restored processes.  Break after the real dynamically loaded
            # flasher returns and publish its consumed success convention in
            # r0 before the existing comparison.  No Guest instruction
            # remains modified while the parent reports terminal status.
            0x0000ECB8,
            0x0000ECFC,
            (
                GuestAnchor(
                    0x0000EB98,
                    bytes.fromhex(
                        "f0402de90c708de204802de514d04de268019fe5"
                        "0c108de50160a0e160119fe500008fe00340a0e1"
                        "01108fe010308de55ffcffeb"
                    ),
                    "IR-MCU updater entry",
                ),
                GuestAnchor(
                    0x0000ECA8,
                    bytes.fromhex(
                        "00008fe001108fe10230a0e135ff2fe1000050e3"
                        "0150a0030e00000a"
                    ),
                    "TiSerialFlasher result decision",
                ),
                GuestAnchor(
                    0x0000ECF8,
                    bytes.fromhex(
                        "2ffcffeb0050a0e30800a0e144f401eb0500a0e1"
                        "10d047e200018de8f080bde8"
                    ),
                    "IR-MCU updater epilogue",
                ),
            ),
            (
                GuestPatch(
                    0x0000ECFC,
                    bytes.fromhex("0050a0e3"),
                    bytes.fromhex("0150a0e3"),
                    "unsupported IR-MCU result",
                ),
            ),
            timeout=timeout,
        )

    def accept_unsupported_result(self, remote: GDBRemote) -> None:
        """Publish success for the existing comparison without changing code."""

        patch = self.patches[0]
        if remote.read_memory(patch.address, len(patch.original)) != patch.original:
            raise RuntimeError(
                f"{self.name} {patch.name} original bytes changed"
            )
        remote.write_register(0, 0)
        if remote.read_register(0) != 0:
            raise RuntimeError(f"{self.name} result register write failed")

    def complete(self) -> None:
        """Accept one authenticated updater result without changing its code."""

        remote = self.remote
        if remote is None or not self.running:
            raise RuntimeError(f"{self.name} is not armed")
        try:
            self._enter_and_authenticate(remote)
            self.accept_unsupported_result(remote)
            remote.detach()
            remote.close()
            self.remote = None
        except BaseException:
            self.cancel()
            raise


def restore_options(
    system_partition_padding: dict[str, Any],
    create_filesystem_partitions: bool = True,
    restore_system_image: bool = True,
) -> dict[str, Any]:
    """Return restore options that install the AP firmware and filesystems."""

    options = {
        "AutoBootDelay": 0,
        "BootImageType": "UserOrInternal",
        "DFUFileType": "RELEASE",
        "DataImage": False,
        "FirmwareDirectory": ".",
        # FlashNOR is a legacy action key and must be present to install the AP
        # images.  Peripheral firmware selection belongs to the later NORData
        # response rather than this dictionary.
        "FlashNOR": True,
        "KernelCacheType": "Release",
        "NORImageType": "production",
        "RestoreBundlePath": "/tmp/Per2.tmp",
        "RootToInstall": False,
        "SystemImage": restore_system_image,
        "SystemImageType": "User",
        "SystemPartitionPadding": system_partition_padding,
        "UUID": str(uuid.uuid4()).upper(),
    }
    options["CreateFilesystemPartitions"] = create_filesystem_partitions
    return options


def format_restore_progress(operation: Any, value: Any) -> str:
    if isinstance(value, int) and 0 <= value <= 100:
        width = 30
        filled = value * width // 100
        bar = "#" * filled + "-" * (width - filled)
        return f"restore operation={operation} [{bar}] {value}%"
    if operation == 28 and value == -1:
        return "restore operation=28 waiting for storage device"
    return f"restore operation={operation} value={value}"


def summarize_status_message(message: dict[str, Any]) -> dict[str, Any]:
    """Keep restored's terminal status while bounding its cumulative log."""

    summary = {key: value for key, value in message.items() if key != "Log"}
    log = message.get("Log")
    if not isinstance(log, str):
        if log is not None:
            summary["Log"] = log
        return summary

    encoded = log.encode("utf-8", errors="replace")
    summary["LogBytes"] = len(encoded)
    if len(encoded) > STATUS_LOG_TAIL_BYTES:
        tail = encoded[-STATUS_LOG_TAIL_BYTES:]
        while tail and tail[0] & 0xC0 == 0x80:
            tail = tail[1:]
        summary["LogTruncated"] = True
    else:
        tail = encoded
    summary["LogTail"] = tail.decode("utf-8", errors="replace")
    return summary


def print_guest_observations(
    prefix: str,
    results: list[GuestObservationResult],
) -> None:
    for result in results:
        observation = result.observation
        registers = " ".join(
            f"r{register}=0x{value:08x}"
            for register, value in zip(
                observation.registers,
                result.register_values,
                strict=True,
            )
        )
        memory = (
            f" memory={result.memory.hex()}"
            if result.memory is not None
            else ""
        )
        print(
            f"{prefix} {observation.name}: {registers}"
            f"{memory} lr=0x{result.return_address:08x}",
            flush=True,
        )


def load_restore_inputs(
    archive: ZipFile,
) -> tuple[str, str, dict[str, Any], bool]:
    restore = plistlib.loads(archive.read("Restore.plist"))
    rootfs = restore["SystemRestoreImages"]["User"]
    if "BuildManifest.plist" in archive.namelist():
        legacy = False
        kernelcache = restore["KernelCachesByTarget"]["n82"]["Release"]
        manifest = plistlib.loads(archive.read("BuildManifest.plist"))
        identities = [
            identity
            for identity in manifest["BuildIdentities"]
            if identity["Info"].get("DeviceClass") == "n82ap"
            and identity["Info"].get("RestoreBehavior") == "Erase"
        ]
        if len(identities) != 1:
            raise RuntimeError(
                f"expected one N82 erase identity, found {len(identities)}"
            )
        padding = identities[0]["Info"].get("SystemPartitionPadding")
    else:
        legacy = True
        devices = [
            device
            for device in restore.get("DeviceMap", [])
            if device.get("BoardConfig") == "n82ap"
            and device.get("CPID") == 0x8900
            and device.get("BDID") == 4
        ]
        if len(devices) != 1:
            raise RuntimeError(
                f"expected one legacy N82 device, found {len(devices)}"
            )
        kernelcache = restore["RestoreKernelCaches"]["Release"]
        padding = None
    if not isinstance(padding, dict):
        padding = {"8": 80, "16": 160, "32": 320, "64": 640}
    if rootfs not in archive.namelist() or kernelcache not in archive.namelist():
        raise RuntimeError("Restore.plist refers to a missing IPSW member")
    return rootfs, kernelcache, padding, legacy


def load_nor_data(
    archive: ZipFile,
    restore_info: dict[str, Any],
    hardware_model: Any,
    disable_ir_mcu: bool = False,
) -> dict[str, Any]:
    """Build restored's production NORData reply from the IPSW manifest."""

    if not isinstance(hardware_model, str) or hardware_model.lower() != "n82ap":
        raise RuntimeError(f"unexpected NOR hardware model: {hardware_model!r}")
    firmware_directory = restore_info.get("FirmwareDirectory")
    if not isinstance(firmware_directory, str) or not firmware_directory:
        raise RuntimeError("Restore.plist has no FirmwareDirectory")
    firmware_path = PurePosixPath(firmware_directory)
    if firmware_path.is_absolute() or any(
        part in ("", ".", "..") for part in firmware_path.parts
    ):
        raise RuntimeError(f"invalid FirmwareDirectory: {firmware_directory!r}")

    all_flash = (
        firmware_path
        / "all_flash"
        / f"all_flash.{hardware_model.lower()}.production"
    )
    manifest_member = str(all_flash / "manifest")
    try:
        manifest = archive.read(manifest_member).decode("ascii").splitlines()
    except (KeyError, UnicodeDecodeError) as error:
        raise RuntimeError(
            f"cannot read production NOR manifest {manifest_member}"
        ) from error
    if not manifest or len(set(manifest)) != len(manifest):
        raise RuntimeError("production NOR manifest is empty or has duplicates")
    if any(
        not name or PurePosixPath(name).name != name
        for name in manifest
    ):
        raise RuntimeError("production NOR manifest contains an invalid member")

    llb = [name for name in manifest if name.startswith("LLB.")]
    iboot = [name for name in manifest if name.startswith("iBoot.")]
    if len(llb) != 1 or len(iboot) != 1:
        raise RuntimeError(
            "production NOR manifest must contain exactly one LLB and iBoot"
        )
    nor_members = iboot + [
        name for name in manifest if name not in (llb[0], iboot[0])
    ]
    try:
        llb_data = archive.read(str(all_flash / llb[0]))
        nor_images = [
            archive.read(str(all_flash / member)) for member in nor_members
        ]
    except KeyError as error:
        raise RuntimeError(
            f"production NOR manifest refers to missing member {error.args[0]}"
        ) from error
    result: dict[str, Any] = {
        "LlbImageData": llb_data,
        "NorImageData": nor_images,
        # The update_baseband wrapper consumes this key from NORData with a
        # default of true.  N82's external baseband is outside this VM.
        "UpdateBaseband": False,
    }
    if disable_ir_mcu:
        # The generic device-firmware dispatcher consumes updater enablement
        # from this NORData dictionary.  A false CFBoolean is its native
        # "update disabled" path; RestoreOptions is not consulted here.
        result["IR MCU"] = False
    return result


async def connect_restored(
    address: str,
    timeout: float = RESTORED_CONNECT_TIMEOUT,
    retry_interval: float = RESTORED_CONNECT_RETRY_INTERVAL,
    attempt_timeout: float = RESTORED_CONNECT_ATTEMPT_TIMEOUT,
) -> tuple[MuxDevice, ServiceConnection, dict[str, Any]]:
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout
    last_error: BaseException | None = None

    while True:
        remaining = deadline - loop.time()
        if remaining <= 0:
            raise TimeoutError(
                f"restored did not open on the IOSU device within {timeout:g}s"
            ) from last_error
        operation_timeout = min(attempt_timeout, remaining)
        try:
            devices = await asyncio.wait_for(
                select_devices_by_connection_type(
                    "USB", usbmux_address=address
                ),
                timeout=operation_timeout,
            )
        except (
            ConnectionFailedError,
            OSError,
            asyncio.IncompleteReadError,
            TimeoutError,
        ) as error:
            last_error = error
            devices = []
        if len(devices) > 1:
            raise RuntimeError(
                f"expected one IOSU USB device, found {len(devices)}"
            )
        if devices:
            device = devices[0]
            service: ServiceConnection | None = None
            try:
                service = await asyncio.wait_for(
                    ServiceConnection.create_using_usbmux(
                        device.serial,
                        62078,
                        connection_type="USB",
                        usbmux_address=address,
                    ),
                    timeout=min(attempt_timeout, deadline - loop.time()),
                )
                await asyncio.wait_for(
                    service.start(),
                    timeout=min(attempt_timeout, deadline - loop.time()),
                )
                info = await asyncio.wait_for(
                    service.send_recv_plist({"Request": "QueryType"}),
                    timeout=min(attempt_timeout, deadline - loop.time()),
                )
            except (
                ConnectionFailedError,
                OSError,
                asyncio.IncompleteReadError,
                TimeoutError,
            ) as error:
                last_error = error
                if service is not None:
                    await service.close()
            else:
                if info.get("Type") != "com.apple.mobile.restored":
                    await service.close()
                    raise RuntimeError(f"device is not running restored: {info!r}")
                return device, service, info

        now = loop.time()
        if now >= deadline:
            raise TimeoutError(
                f"restored did not open on the IOSU device within {timeout:g}s"
            ) from last_error
        await asyncio.sleep(min(retry_interval, deadline - now))


async def wait_for_nand_ready(
    uart_log: Path,
    driver_marker: bytes = NAND_DRIVER_STARTED,
    ready_marker: bytes = NAND_READY,
    timeout: float = 900,
) -> None:
    """Wait for the current boot's NAND driver to publish FTL readiness."""

    deadline = asyncio.get_running_loop().time() + timeout
    offset = 0
    driver_started = False
    ready = False
    while True:
        with uart_log.open("rb") as stream:
            stream.seek(offset)
            for line in stream:
                if driver_marker in line:
                    driver_started = True
                    ready = False
                elif driver_started and ready_marker in line:
                    ready = True
            offset = stream.tell()
        if ready:
            return
        if asyncio.get_running_loop().time() >= deadline:
            raise TimeoutError(f"NAND did not become ready within {timeout:g}s")
        await asyncio.sleep(0.1)


async def wait_for_start_restore_gate(gate: Path | None) -> None:
    """Hold the live restored session until an external debugger is ready."""

    if gate is None:
        return
    print(f"waiting for StartRestore gate: {gate}", flush=True)
    while not gate.exists():
        await asyncio.sleep(0.1)
    print("StartRestore gate opened", flush=True)


async def send_system_image(
    device: MuxDevice,
    archive: ZipFile,
    member: str,
    port: int,
    checksum_bypass_endpoint: Endpoint | None = None,
    local_image: Path | None = None,
    signature_bypass_endpoint: Endpoint | None = None,
) -> None:
    asr = ASRClient(device.serial)
    deadline = asyncio.get_running_loop().time() + 30
    while True:
        try:
            await asr.connect(port)
            break
        except ConnectionFailedError:
            if asyncio.get_running_loop().time() >= deadline:
                raise
            await asyncio.sleep(0.25)
    source_name = str(local_image) if local_image is not None else member
    print(f"ASR connected on port {port}; validating {source_name}", flush=True)
    if (
        checksum_bypass_endpoint is not None
        and signature_bypass_endpoint is not None
    ):
        raise RuntimeError("ASR bypass generations are mutually exclusive")
    asr_bypass: AuthenticatedRuntimePatch | None = None
    try:
        source = (
            local_image.open("rb")
            if local_image is not None
            else archive.open(member)
        )
        with source as filesystem:
            await asr.perform_validation(filesystem)
            if checksum_bypass_endpoint is not None:
                asr_bypass = IOS2ASRChecksumBypass(
                    checksum_bypass_endpoint
                )
                await asyncio.to_thread(asr_bypass.arm)
                print(
                    "armed authenticated 5A347 ASR checksum bypass",
                    flush=True,
                )
            elif signature_bypass_endpoint is not None:
                asr_bypass = IOS4ASRSignatureBypass(
                    signature_bypass_endpoint
                )
                await asyncio.to_thread(asr_bypass.arm)
                print(
                    "armed authenticated 8C148 ASR image-signature bypass",
                    flush=True,
                )
            print("ASR validation complete; sending system image", flush=True)
            await asr.send_payload(filesystem)
            if asr_bypass is not None:
                print(
                    "ASR payload sent; waiting for the authenticated final gate",
                    flush=True,
                )
                await asyncio.to_thread(asr_bypass.complete)
                print_guest_observations(
                    "ASR post-copy",
                    asr_bypass.observation_results,
                )
                print(
                    "ASR final gate bypassed; original bytes restored",
                    flush=True,
                )
    except BaseException:
        if asr_bypass is not None:
            await asyncio.to_thread(asr_bypass.cancel)
        raise
    finally:
        await asr.close()
    print("ASR system image transfer complete; data connection closed", flush=True)


async def restore(
    ipsw: Path,
    address: str,
    uart_log: Path,
    reuse_filesystem_partitions: bool = False,
    start_restore_gate: Path | None = None,
    checksum_bypass_endpoint: Endpoint | None = None,
    runtime_patch_endpoint: Endpoint | None = None,
    skip_filesystem_wipe: bool = False,
    reuse_system_image: bool = False,
    nor_probe_endpoint: Endpoint | None = None,
    bypass_nor_integrity: bool = False,
    connect_timeout: float = RESTORED_CONNECT_TIMEOUT,
    probe_ios4_nor: bool = False,
    nor_image_limit: int | None = None,
    omit_llb: bool = False,
    bypass_ios4_nor_write_failure: bool = False,
    workaround_ios4_catalog_data_length: bool = False,
    bypass_ios4_baseband: bool = False,
    probe_ios4_nor_batch: bool = False,
    disable_ios4_ir_mcu: bool = False,
    system_image: Path | None = None,
    ios4_asr_signature_bypass_endpoint: Endpoint | None = None,
) -> int:
    os.environ["USBMUXD_SOCKET_ADDRESS"] = address
    with ZipFile(ipsw) as archive:
        rootfs, kernelcache, padding, legacy = load_restore_inputs(archive)
        restore_info = plistlib.loads(archive.read("Restore.plist"))
        build = restore_info.get("ProductBuildVersion")
        if checksum_bypass_endpoint is not None:
            if not legacy or build != "5A347":
                raise RuntimeError(
                    "the authenticated ASR checksum bypass only supports 5A347"
                )
        if ios4_asr_signature_bypass_endpoint is not None:
            if legacy or build != "8C148":
                raise RuntimeError(
                    "the authenticated ASR signature bypass only supports 8C148"
                )
            if system_image is None:
                raise RuntimeError(
                    "the authenticated ASR signature bypass requires a local "
                    "system image"
                )
        if reuse_filesystem_partitions and legacy and build != "5A347":
            raise RuntimeError(
                "the authenticated partition bypass only supports 5A347"
            )
        if skip_filesystem_wipe and (not legacy or build != "5A347"):
            raise RuntimeError(
                "the authenticated partition-wipe bypass only supports 5A347"
            )
        if reuse_filesystem_partitions and skip_filesystem_wipe:
            raise RuntimeError(
                "partition reuse and partition-wipe bypass are mutually exclusive"
            )
        if reuse_system_image and not reuse_filesystem_partitions:
            raise RuntimeError(
                "system-image reuse requires existing filesystem partitions"
            )
        if reuse_system_image and (not legacy or build != "5A347"):
            raise RuntimeError(
                "system-image reuse is authenticated only for 5A347"
            )
        if (probe_ios4_nor or probe_ios4_nor_batch) and (
            legacy or build != "8C148"
        ):
            raise RuntimeError("the authenticated iOS 4 NOR probe only supports 8C148")
        if bypass_ios4_nor_write_failure and (legacy or build != "8C148"):
            raise RuntimeError(
                "the authenticated NOR provider-failure bypass only supports 8C148"
            )
        if workaround_ios4_catalog_data_length and (
            legacy or build != "8C148"
        ):
            raise RuntimeError(
                "the authenticated catalog DATA-length workaround only "
                "supports 8C148"
            )
        if workaround_ios4_catalog_data_length and not probe_ios4_nor_batch:
            raise RuntimeError(
                "the catalog DATA-length workaround requires the iOS 4 "
                "NOR batch owner"
            )
        if bypass_ios4_baseband and (legacy or build != "8C148"):
            raise RuntimeError(
                "the authenticated external-baseband bypass only supports 8C148"
            )
        if disable_ios4_ir_mcu and (legacy or build != "8C148"):
            raise RuntimeError(
                "the native IR-MCU disable option only supports 8C148"
            )
        if (
            nor_probe_endpoint is not None
            and not (
                probe_ios4_nor
                or probe_ios4_nor_batch
                or bypass_ios4_nor_write_failure
                or workaround_ios4_catalog_data_length
            )
            and (not legacy or build != "5A347")
        ):
            raise RuntimeError("the authenticated iOS 2 NOR probe only supports 5A347")
        if (
            bypass_nor_integrity
            or bypass_ios4_nor_write_failure
            or workaround_ios4_catalog_data_length
        ) and nor_probe_endpoint is None:
            raise RuntimeError("the NOR bypass requires a GDB endpoint")
        device, service, type_info = await connect_restored(
            address, timeout=connect_timeout
        )
        system_image_sent = False
        nor_data_sent = False
        partition_bypass: AuthenticatedRuntimePatch | None = None
        nor_probe: IOS2NORFlashProbe | IOS4NORFlashProbe | None = None
        baseband_bypass: IOS4BasebandBypass | None = None
        baseband_activation_task: asyncio.Task[None] | None = None
        baseband_bypass_active = False
        last_progress: tuple[Any, Any] | None = None
        repeated_progress = 0
        try:
            print(json.dumps(type_info, sort_keys=True), flush=True)
            print("waiting for NAND readiness", flush=True)
            if legacy:
                await wait_for_nand_ready(
                    uart_log,
                    LEGACY_NAND_DRIVER_STARTED,
                    LEGACY_NAND_READY,
                )
            else:
                await wait_for_nand_ready(uart_log)
            print("NAND is ready", flush=True)
            await wait_for_start_restore_gate(start_restore_gate)
            request = {
                "Request": "StartRestore",
                "Label": "pymobiledevice3",
                "RestoreProtocolVersion": type_info["RestoreProtocolVersion"],
                "RestoreOptions": restore_options(
                    padding,
                    create_filesystem_partitions=not reuse_filesystem_partitions,
                    restore_system_image=not reuse_system_image,
                ),
            }
            if (reuse_filesystem_partitions or skip_filesystem_wipe) and legacy:
                if runtime_patch_endpoint is None:
                    raise RuntimeError(
                        "legacy partition patching requires a GDB endpoint"
                    )
                if reuse_system_image:
                    partition_bypass = IOS2FilesystemReuseBypass(
                        runtime_patch_endpoint
                    )
                elif reuse_filesystem_partitions:
                    partition_bypass = IOS2PartitionReuseBypass(
                        runtime_patch_endpoint
                    )
                else:
                    partition_bypass = IOS2PartitionWipeBypass(
                        runtime_patch_endpoint
                    )
                await asyncio.to_thread(partition_bypass.arm)
                action = (
                    "filesystem-reuse"
                    if reuse_system_image
                    else (
                        "partition-creation"
                        if reuse_filesystem_partitions
                        else "partition-wipe"
                    )
                )
                print(f"armed authenticated 5A347 {action} bypass", flush=True)
            await service.send_plist(request)
            print("StartRestore submitted", flush=True)
            if partition_bypass is not None:
                await asyncio.to_thread(partition_bypass.complete)
                result = (
                    "partition creation and system-image restore skipped"
                    if reuse_system_image
                    else (
                        "partition creation skipped"
                        if reuse_filesystem_partitions
                        else "partition wipe skipped; partition map and newfs completed"
                    )
                )
                print(f"{result}; original bytes restored", flush=True)
            while True:
                message = await service.recv_plist()
                if (
                    baseband_activation_task is not None
                    and baseband_activation_task.done()
                ):
                    await baseband_activation_task
                    baseband_activation_task = None
                    baseband_bypass_active = True
                    print(
                        "external-baseband bypass active",
                        flush=True,
                    )
                message_type = message.get("MsgType")
                if message_type == "ProgressMsg":
                    progress = (message.get("Operation"), message.get("Progress"))
                    if (
                        progress[0] == 46
                        and bypass_ios4_baseband
                        and baseband_bypass is None
                    ):
                        if nor_probe is not None:
                            if runtime_patch_endpoint is None:
                                raise RuntimeError(
                                    "the baseband bypass requires a GDB endpoint"
                                )
                            baseband_bypass = IOS4BasebandBypass(
                                runtime_patch_endpoint
                            )
                            await asyncio.to_thread(
                                nor_probe.handoff, baseband_bypass
                            )
                            nor_probe = None
                            print(
                                "NOR bypass removed; original bytes restored",
                                flush=True,
                            )
                        else:
                            if runtime_patch_endpoint is None:
                                raise RuntimeError(
                                    "the baseband bypass requires a GDB endpoint"
                                )
                            baseband_bypass = IOS4BasebandBypass(
                                runtime_patch_endpoint
                            )
                            await asyncio.to_thread(baseband_bypass.arm)
                        print(
                            "armed authenticated 8C148 external-baseband bypass",
                            flush=True,
                        )
                        baseband_activation_task = asyncio.create_task(
                            asyncio.to_thread(baseband_bypass.activate)
                        )
                    if progress == last_progress:
                        repeated_progress += 1
                        if repeated_progress % PROGRESS_REPEAT_INTERVAL:
                            continue
                    else:
                        last_progress = progress
                        repeated_progress = 0
                    suffix = (
                        f" ({repeated_progress + 1} status updates)"
                        if repeated_progress
                        else ""
                    )
                    print(format_restore_progress(*progress) + suffix, flush=True)
                elif message_type == "PreviousRestoreLogMsg":
                    print("restored reported a previous restore log", flush=True)
                elif message_type == "DataRequestMsg":
                    data_type = message.get("DataType")
                    print(f"data request {data_type}", flush=True)
                    if data_type == "SystemImageData":
                        if system_image_sent:
                            raise RuntimeError("received duplicate SystemImageData request")
                        await send_system_image(
                            device,
                            archive,
                            rootfs,
                            int(message.get("DataPort", DEFAULT_ASR_SYNC_PORT)),
                            checksum_bypass_endpoint,
                            system_image,
                            ios4_asr_signature_bypass_endpoint,
                        )
                        system_image_sent = True
                    elif data_type == "KernelCache":
                        await service.send_plist(
                            {"KernelCacheFile": archive.read(kernelcache)}
                        )
                        print("kernelcache sent", flush=True)
                    elif data_type == "NORData":
                        if nor_data_sent:
                            raise RuntimeError("received duplicate NORData request")
                        arguments = message.get("Arguments", {})
                        if not isinstance(arguments, dict) or arguments.get(
                            "FlashVersion1", False
                        ):
                            raise RuntimeError(
                                f"unsupported NORData arguments: {arguments!r}"
                            )
                        nor_data = load_nor_data(
                            archive,
                            restore_info,
                            type_info.get("HardwareModel"),
                            disable_ir_mcu=disable_ios4_ir_mcu,
                        )
                        if nor_image_limit is not None:
                            nor_data["NorImageData"] = nor_data[
                                "NorImageData"
                            ][:nor_image_limit]
                        if omit_llb:
                            del nor_data["LlbImageData"]
                        if nor_probe_endpoint is not None:
                            nor_probe = (
                                IOS4NORFlashProbe(
                                    nor_probe_endpoint,
                                    # Fail a truncated NOR batch promptly.
                                    # The same connection switches to the
                                    # successor's longer timeout atomically at
                                    # the final transaction handoff.
                                    timeout=120,
                                    bypass_write_failure=
                                        bypass_ios4_nor_write_failure,
                                    workaround_catalog_data_length=
                                        workaround_ios4_catalog_data_length,
                                    transaction_count=(
                                        len(nor_data["NorImageData"])
                                        + (1 if "LlbImageData" in nor_data else 0)
                                        if probe_ios4_nor_batch else 1
                                    ),
                                )
                                if (probe_ios4_nor or
                                    probe_ios4_nor_batch or
                                    bypass_ios4_nor_write_failure or
                                    workaround_ios4_catalog_data_length)
                                else IOS2NORFlashProbe(
                                    nor_probe_endpoint,
                                    bypass_integrity=bypass_nor_integrity,
                                )
                            )
                            await asyncio.to_thread(nor_probe.arm)
                            print(
                                "armed authenticated "
                                + (
                                    "8C148"
                                    if (probe_ios4_nor or
                                        probe_ios4_nor_batch or
                                        bypass_ios4_nor_write_failure or
                                        workaround_ios4_catalog_data_length)
                                    else "5A347"
                                )
                                + " NOR "
                                + (
                                    (
                                        f"{len(nor_data['NorImageData']) + (1 if 'LlbImageData' in nor_data else 0)}"
                                        "-transaction batch probe"
                                    )
                                    if probe_ios4_nor_batch
                                    else "provider-failure bypass"
                                    if bypass_ios4_nor_write_failure
                                    else (
                                        "integrity bypass"
                                        if bypass_nor_integrity
                                        else "flash probe"
                                    )
                                ),
                                flush=True,
                            )
                        await service.send_plist(nor_data)
                        nor_data_sent = True
                        print(
                            "NORData sent: "
                            + ("no LLB, " if omit_llb else "LLB plus ")
                            + f"{len(nor_data['NorImageData'])} images",
                            flush=True,
                        )
                        if nor_probe is not None:
                            if probe_ios4_nor_batch:
                                baseband_bypass = (
                                    IOS4BasebandBypass(runtime_patch_endpoint)
                                    if bypass_ios4_baseband
                                    and runtime_patch_endpoint is not None
                                    else None
                                )
                                await asyncio.to_thread(
                                    nor_probe.probe_direct_batch,
                                    baseband_bypass,
                                    None,
                                )
                                print_guest_observations(
                                    "NOR direct transactions",
                                    nor_probe.observation_results,
                                )
                                if disable_ios4_ir_mcu:
                                    print(
                                        "8C148 IR-MCU update disabled through "
                                        "native NORData firmware selection",
                                        flush=True,
                                    )
                                batch_successor_active = (
                                    nor_probe.batch_successor_active
                                )
                                batch_ir_mcu_complete = (
                                    nor_probe.batch_ir_mcu_complete
                                )
                                nor_probe = None
                                if bypass_ios4_baseband:
                                    if runtime_patch_endpoint is None:
                                        raise RuntimeError(
                                            "the baseband bypass requires a GDB endpoint"
                                        )
                                    if baseband_bypass is None:
                                        raise RuntimeError(
                                            "the baseband bypass handoff was not created"
                                        )
                                    if (
                                        not disable_ios4_ir_mcu
                                        and batch_ir_mcu_complete
                                    ):
                                        print(
                                            "authenticated 8C148 IR-MCU result "
                                            "accepted without code modification",
                                            flush=True,
                                        )
                                    if batch_successor_active:
                                        baseband_bypass_active = True
                                        print(
                                            "external-baseband bypass active",
                                            flush=True,
                                        )
                                    else:
                                        baseband_bypass = None
                                        print(
                                            "8C148 external-baseband entry was not "
                                            "observed",
                                            flush=True,
                                        )
                            elif (bypass_nor_integrity or
                                    bypass_ios4_nor_write_failure):
                                await asyncio.to_thread(nor_probe.activate)
                                print(
                                    "NOR authenticated gates active for complete "
                                    "NORData batch",
                                    flush=True,
                                )
                            else:
                                await asyncio.to_thread(nor_probe.complete)
                                print_guest_observations(
                                    "NOR transaction probe",
                                    nor_probe.observation_results,
                                )
                    else:
                        raise RuntimeError(f"unsupported restored data request: {message!r}")
                elif message_type == "StatusMsg":
                    if baseband_activation_task is not None:
                        await baseband_activation_task
                        baseband_activation_task = None
                        baseband_bypass_active = True
                        print(
                            "external-baseband bypass active",
                            flush=True,
                        )
                    if baseband_bypass is not None and baseband_bypass_active:
                        await asyncio.to_thread(baseband_bypass.deactivate)
                        baseband_bypass = None
                        baseband_bypass_active = False
                        print(
                            "baseband bypass removed; original bytes restored",
                            flush=True,
                        )
                    if nor_probe is not None and (
                        bypass_nor_integrity or
                        bypass_ios4_nor_write_failure
                    ):
                        await asyncio.to_thread(nor_probe.deactivate)
                        print(
                            "NOR bypass removed; original bytes restored",
                            flush=True,
                        )
                    print(
                        json.dumps(
                            summarize_status_message(message),
                            default=str,
                            sort_keys=True,
                        ),
                        flush=True,
                    )
                    return 0 if message.get("Status") == 0 else 1
                else:
                    print(json.dumps(message, default=str, sort_keys=True), flush=True)
        finally:
            await service.close()
            if baseband_activation_task is not None:
                with contextlib.suppress(Exception):
                    await baseband_activation_task
            if partition_bypass is not None:
                await asyncio.to_thread(partition_bypass.cancel)
            if nor_probe is not None:
                await asyncio.to_thread(nor_probe.cancel)
            if baseband_bypass is not None:
                await asyncio.to_thread(baseband_bypass.cancel)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ipsw", required=True, type=Path)
    parser.add_argument("--usbmuxd", default="127.0.0.1:27015")
    parser.add_argument("--uart-log", required=True, type=Path)
    parser.add_argument(
        "--system-image",
        type=Path,
        help=(
            "send this local UDIF system image through restored/ASR instead "
            "of the IPSW member"
        ),
    )
    parser.add_argument(
        "--connect-timeout",
        type=float,
        default=RESTORED_CONNECT_TIMEOUT,
        help=(
            "seconds to wait for restored over AppleUSBMux "
            f"(default: {RESTORED_CONNECT_TIMEOUT:g})"
        ),
    )
    parser.add_argument(
        "--reuse-filesystem-partitions",
        action="store_true",
        help="reuse existing partitions with the authenticated legacy bypass",
    )
    parser.add_argument(
        "--skip-filesystem-wipe",
        action="store_true",
        help="create and format legacy partitions without the security wipe",
    )
    parser.add_argument(
        "--reuse-system-image",
        action="store_true",
        help="reuse a verified 5A347 system image and run only later restore stages",
    )
    parser.add_argument(
        "--start-restore-gate",
        type=Path,
        help="wait for this initially absent path before sending StartRestore",
    )
    parser.add_argument(
        "--bypass-ios2-asr-checksum",
        action="store_true",
        help="continue through authenticated 5A347 ASR post-copy repair",
    )
    parser.add_argument(
        "--bypass-ios4-asr-signature",
        action="store_true",
        help=(
            "authenticate and redirect only 8C148 ASR's final local-image "
            "signature failure"
        ),
    )
    parser.add_argument(
        "--probe-ios2-nor",
        action="store_true",
        help="observe the first two authenticated 5A347 NOR transactions",
    )
    parser.add_argument(
        "--probe-ios4-nor",
        action="store_true",
        help="observe the first authenticated 8C148 NOR transaction",
    )
    parser.add_argument(
        "--probe-ios4-nor-batch",
        action="store_true",
        help=(
            "trace all eleven authenticated 8C148 direct NOR "
            "transactions while retaining only the provider-result bypass"
        ),
    )
    parser.add_argument(
        "--nor-image-limit",
        type=int,
        help=(
            "send only the first N manifest-ordered AP NOR images (zero "
            "sends none); intended "
            "for authenticated boot-critical restore diagnostics"
        ),
    )
    parser.add_argument(
        "--omit-llb",
        action="store_true",
        help=(
            "omit the already-installed LLB from NORData; intended for "
            "authenticated incremental AP NOR restore diagnostics"
        ),
    )
    parser.add_argument(
        "--bypass-ios4-nor-write-failure",
        action="store_true",
        help=(
            "authenticate and bypass only 8C148's NOR provider failure "
            "branch while retaining each real provider call"
        ),
    )
    parser.add_argument(
        "--workaround-ios4-catalog-data-length",
        action="store_true",
        help=(
            "authenticate the 8C148 catalog call site and initialize only "
            "its otherwise-uninitialized DATA-length stack local"
        ),
    )
    parser.add_argument(
        "--bypass-ios4-baseband",
        action="store_true",
        help=(
            "authenticate and skip only the unavailable N82 baseband update "
            "after the AP restore completes"
        ),
    )
    parser.add_argument(
        "--disable-ios4-ir-mcu",
        action="store_true",
        help=(
            "disable the unavailable IR-MCU update through restored's native "
            "'IR MCU' NORData firmware selector"
        ),
    )
    parser.add_argument(
        "--bypass-ios2-nor-integrity",
        action="store_true",
        help=(
            "continue through authenticated 5A347 NOR IMG3 integrity gates "
            "while retaining the real flash result"
        ),
    )
    parser.add_argument("--gdb", default="127.0.0.1:1234", metavar="HOST:PORT")
    options = parser.parse_args()
    Endpoint.parse(options.usbmuxd)
    if options.start_restore_gate is not None and options.start_restore_gate.exists():
        parser.error("--start-restore-gate must not exist when restore starts")
    if options.probe_ios2_nor and (
        options.probe_ios4_nor or options.probe_ios4_nor_batch
    ):
        parser.error("the iOS 2 and iOS 4 NOR probes are mutually exclusive")
    if options.nor_image_limit is not None and options.nor_image_limit < 0:
        parser.error("--nor-image-limit must be nonnegative")
    runtime_patch_endpoint = Endpoint.parse(options.gdb)
    checksum_bypass_endpoint = (
        runtime_patch_endpoint if options.bypass_ios2_asr_checksum else None
    )
    ios4_asr_signature_bypass_endpoint = (
        runtime_patch_endpoint if options.bypass_ios4_asr_signature else None
    )
    return asyncio.run(
        restore(
            options.ipsw,
            options.usbmuxd,
            options.uart_log,
            options.reuse_filesystem_partitions,
            options.start_restore_gate,
            checksum_bypass_endpoint,
            runtime_patch_endpoint,
            options.skip_filesystem_wipe,
            options.reuse_system_image,
            (
                runtime_patch_endpoint
                if options.probe_ios2_nor
                or options.probe_ios4_nor
                or options.probe_ios4_nor_batch
                or options.bypass_ios2_nor_integrity
                or options.bypass_ios4_nor_write_failure
                or options.workaround_ios4_catalog_data_length
                or options.bypass_ios4_baseband
                else None
            ),
            options.bypass_ios2_nor_integrity,
            options.connect_timeout,
            options.probe_ios4_nor,
            options.nor_image_limit,
            options.omit_llb,
            options.bypass_ios4_nor_write_failure,
            options.workaround_ios4_catalog_data_length,
            options.bypass_ios4_baseband,
            options.probe_ios4_nor_batch,
            options.disable_ios4_ir_mcu,
            options.system_image,
            ios4_asr_signature_bypass_endpoint,
        )
    )


if __name__ == "__main__":
    raise SystemExit(main())
