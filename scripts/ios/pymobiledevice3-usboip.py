#!/usr/bin/env python3
"""Run the pinned pymobiledevice3 through the virtual iPhone transports."""

from __future__ import annotations

import argparse
import asyncio
import os
import sys
from typing import Any

from usboip import Endpoint, IRecvAdapter


def omit_none_mapping_values(value: dict[str, Any]) -> dict[str, Any]:
    """Return a plist-safe mapping without absent optional values."""
    return {key: item for key, item in value.items() if item is not None}


def install_legacy_pairing_compatibility() -> None:
    """Let pre-Wi-Fi lockdown identities omit WiFiMACAddress when pairing."""
    from pymobiledevice3.lockdown import LockdownClient

    request_pair = LockdownClient._request_pair
    save_pair_record = LockdownClient.save_pair_record

    async def request_pair_without_absent_values(
        self: LockdownClient,
        pair_options: dict[str, Any],
        timeout: float | None = None,
    ) -> dict[str, Any]:
        pair_record = pair_options.get("PairRecord")
        if isinstance(pair_record, dict):
            pair_options = dict(pair_options)
            pair_options["PairRecord"] = omit_none_mapping_values(pair_record)
        return await request_pair(self, pair_options, timeout)

    async def save_pair_record_without_absent_values(self: LockdownClient) -> None:
        if self.pair_record is not None:
            self.pair_record = omit_none_mapping_values(self.pair_record)
        await save_pair_record(self)

    LockdownClient._request_pair = request_pair_without_absent_values
    LockdownClient.save_pair_record = save_pair_record_without_absent_values


async def open_ios4_lockdown_session(
    create_using_usbmux: Any,
    usbmuxd_address: str,
) -> Any:
    """Return an iOS 4 lockdown client with an active paired session."""
    client = await create_using_usbmux(
        autopair=True,
        usbmux_address=usbmuxd_address,
    )
    if client.session_id is None:
        await client.close()
        raise RuntimeError("lockdown pairing did not establish a session")
    return client


async def ios4_power_action(usbmuxd_address: str, action: str) -> None:
    """Submit one supported diagnostics power action through iOS 4 lockdown."""
    if action not in {"sleep", "shutdown"}:
        raise ValueError(f"unsupported iOS 4 power action: {action}")

    from pymobiledevice3.lockdown import create_using_usbmux
    from pymobiledevice3.services.diagnostics import DiagnosticsService

    print(
        f"iOS 4 {action}: opening an authenticated lockdown session",
        flush=True,
    )
    client = await open_ios4_lockdown_session(
        create_using_usbmux,
        usbmuxd_address,
    )
    try:
        print(f"iOS 4 {action}: starting diagnostics relay", flush=True)
        diagnostics = DiagnosticsService(client)
        try:
            if action == "sleep":
                await diagnostics.sleep()
            else:
                await diagnostics.shutdown()
            print(
                f"iOS 4 {action}: diagnostics accepted the request",
                flush=True,
            )
        finally:
            await diagnostics.close()
    finally:
        await client.close()


def main() -> int:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--usboip", required=True, metavar="HOST:PORT")
    parser.add_argument("--usbmuxd", metavar="HOST:PORT")
    options, pymobiledevice_args = parser.parse_known_args()
    if not pymobiledevice_args:
        parser.error("a pymobiledevice3 command is required")

    endpoint = Endpoint.parse(options.usboip)
    IRecvAdapter(endpoint).install()
    install_legacy_pairing_compatibility()
    if options.usbmuxd:
        Endpoint.parse(options.usbmuxd)
        os.environ["USBMUXD_SOCKET_ADDRESS"] = options.usbmuxd

    if pymobiledevice_args in (["ios4-sleep"], ["ios4-shutdown"]):
        if options.usbmuxd is None:
            parser.error(f"{pymobiledevice_args[0]} requires --usbmuxd")
        action = pymobiledevice_args[0].removeprefix("ios4-")
        asyncio.run(ios4_power_action(options.usbmuxd, action))
        return 0

    sys.argv = ["pymobiledevice3", *pymobiledevice_args]
    from pymobiledevice3.__main__ import main as pymobiledevice3_main

    result = pymobiledevice3_main()
    return result if isinstance(result, int) else 0


if __name__ == "__main__":
    raise SystemExit(main())
