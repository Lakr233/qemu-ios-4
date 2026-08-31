#!/usr/bin/env python3
"""Commit the iPhone 3G FTL context, then stop its QEMU owner."""

from __future__ import annotations

import argparse
import json
import os
import signal
import socket
import subprocess
import time
from pathlib import Path


CLEAN_FTL_ROOT_WITNESS = b"spare[8]=0xffff43ff"
FTL_FAILURE_WITNESSES = (
    b"s5l8900_adm_fmc_write_skipped",
    b"s5l8900_adm_fmc_write_failure",
    b"s5l8900_adm_fmc_target_failure",
)
FTL_TRACE_EVENTS = (
    "s5l8900_adm_fmc_write_page",
    "s5l8900_adm_fmc_write_skipped",
    "s5l8900_adm_fmc_write_failure",
    "s5l8900_adm_fmc_target_failure",
)


def wait_for_clean_ftl_commit(path: Path, offset: int, timeout: float) -> None:
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        if path.is_file():
            with path.open("rb") as stream:
                stream.seek(offset)
                observed = stream.read()
            root_index = observed.find(CLEAN_FTL_ROOT_WITNESS)
            for witness in FTL_FAILURE_WITNESSES:
                failure_index = observed.find(witness)
                if failure_index >= 0 and (
                    root_index < 0 or failure_index < root_index
                ):
                    raise RuntimeError(
                        "FTL commit failed before its clean root: "
                        f"{witness.decode('ascii')}"
                    )
            if root_index >= 0:
                return
        time.sleep(0.2)
    raise TimeoutError(
        f"guest did not write a fresh clean FTL root within {timeout:g}s"
    )


def receive_monitor_prompt(client: socket.socket) -> bytes:
    response = bytearray()
    while not response.endswith(b"(qemu) "):
        chunk = client.recv(4096)
        if not chunk:
            raise RuntimeError("QEMU monitor closed before its command completed")
        response.extend(chunk)
    return bytes(response)


def run_monitor_command(path: Path, command: str) -> bytes:
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(2)
        client.connect(str(path))
        receive_monitor_prompt(client)
        client.sendall(command.encode("ascii") + b"\n")
        return receive_monitor_prompt(client)


def enable_ftl_commit_trace(path: Path) -> None:
    for event in FTL_TRACE_EVENTS:
        response = run_monitor_command(path, f"trace-event {event} on")
        if b"Error" in response or b"unknown" in response:
            raise RuntimeError(
                f"QEMU rejected trace event {event}: "
                f"{response.decode('ascii', errors='replace')}"
            )


def disable_ftl_commit_trace(path: Path) -> None:
    for event in FTL_TRACE_EVENTS:
        try:
            run_monitor_command(path, f"trace-event {event} off")
        except (OSError, RuntimeError):
            pass


def tcp_server_is_ready(address: tuple[str, int]) -> bool:
    try:
        with socket.create_connection(address, timeout=0.2):
            return True
    except OSError:
        return False


def wait_for_tcp_server(
    address: tuple[str, int],
    process: subprocess.Popen[bytes],
    timeout: float,
) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"usbmuxd exited with status {process.returncode}")
        if tcp_server_is_ready(address):
            return
        time.sleep(0.1)
    raise TimeoutError(f"usbmuxd did not listen on {address[0]}:{address[1]}")


def request_monitor_quit(path: Path) -> bool:
    try:
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(1)
            client.connect(str(path))
            client.sendall(b"quit\n")
        return True
    except OSError:
        return False


def validated_qemu_pid(pidfile: Path, qemu: Path) -> int:
    try:
        pid = int(pidfile.read_text().strip())
    except (OSError, ValueError) as error:
        raise RuntimeError(f"cannot read QEMU pid from {pidfile}") from error
    if pid <= 1:
        raise RuntimeError(f"refusing invalid QEMU pid {pid}")

    command = subprocess.run(
        ["ps", "-p", str(pid), "-o", "command="],
        check=False,
        capture_output=True,
        text=True,
    ).stdout.strip()
    if str(qemu.resolve()) not in command or "-M iphone3g" not in command:
        raise RuntimeError(f"pid {pid} is not the expected iPhone 3G QEMU process")
    return pid


def terminate_qemu(pidfile: Path, qemu: Path, monitor: Path) -> None:
    pid = validated_qemu_pid(pidfile, qemu)
    request_monitor_quit(monitor)

    monitor_deadline = time.monotonic() + 1
    while time.monotonic() < monitor_deadline:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return
        time.sleep(0.1)
    os.kill(pid, signal.SIGTERM)

    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return
        time.sleep(0.1)
    raise TimeoutError(f"QEMU pid {pid} did not stop")


def parse_address(value: str) -> tuple[str, int]:
    host, separator, port = value.rpartition(":")
    if not separator or not host:
        raise argparse.ArgumentTypeError("address must be HOST:PORT")
    try:
        parsed_port = int(port)
    except ValueError as error:
        raise argparse.ArgumentTypeError("port must be an integer") from error
    if not 1 <= parsed_port <= 65535:
        raise argparse.ArgumentTypeError("port must be in 1..65535")
    return host, parsed_port


def mobiledevice_command(
    python: Path,
    client: Path,
    usboip: tuple[str, int],
    usbmuxd: tuple[str, int],
    arguments: list[str],
) -> list[str]:
    return [
        str(python),
        str(client),
        "--usboip",
        f"{usboip[0]}:{usboip[1]}",
        "--usbmuxd",
        f"{usbmuxd[0]}:{usbmuxd[1]}",
        *arguments,
    ]


def wait_for_mobiledevice(
    python: Path,
    client: Path,
    usboip: tuple[str, int],
    usbmuxd: tuple[str, int],
    timeout: float,
) -> None:
    command = mobiledevice_command(
        python,
        client,
        usboip,
        usbmuxd,
        ["usbmux", "list", "--simple"],
    )
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        try:
            result = subprocess.run(
                command,
                check=False,
                capture_output=True,
                text=True,
                timeout=min(remaining, 30),
            )
        except subprocess.TimeoutExpired:
            continue
        if result.returncode == 0:
            try:
                devices = json.loads(result.stdout)
            except json.JSONDecodeError:
                devices = None
            if isinstance(devices, list) and devices:
                return
        time.sleep(min(0.5, remaining))
    raise TimeoutError("the iOS guest did not connect to usbmuxd")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--python", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--usboip", type=parse_address, required=True)
    parser.add_argument("--usbmuxd", type=Path, required=True)
    parser.add_argument("--usbmuxd-address", type=parse_address, required=True)
    parser.add_argument("--usbmuxd-config", type=Path, required=True)
    parser.add_argument("--trace-log", type=Path, required=True)
    parser.add_argument("--monitor", type=Path, required=True)
    parser.add_argument("--pidfile", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=180)
    options = parser.parse_args()
    if options.timeout <= 0:
        parser.error("--timeout must be positive")
    if not options.monitor.is_socket():
        parser.error(f"QEMU monitor is not running at {options.monitor}")

    mux_address = options.usbmuxd_address
    mux_process: subprocess.Popen[bytes] | None = None
    try:
        if not tcp_server_is_ready(mux_address):
            options.usbmuxd_config.mkdir(parents=True, exist_ok=True)
            environment = os.environ.copy()
            environment["QEMU_IOSU_ADDRESS"] = (
                f"{options.usboip[0]}:{options.usboip[1]}"
            )
            mux_process = subprocess.Popen(
                [
                    str(options.usbmuxd),
                    "-f",
                    "-p",
                    "-S",
                    f"{mux_address[0]}:{mux_address[1]}",
                    "-P",
                    "NONE",
                    "-C",
                    str(options.usbmuxd_config),
                ],
                env=environment,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.STDOUT,
            )
            wait_for_tcp_server(
                mux_address,
                mux_process,
                min(options.timeout, 10),
            )

        wait_for_mobiledevice(
            options.python,
            options.client,
            options.usboip,
            mux_address,
            options.timeout,
        )

        enable_ftl_commit_trace(options.monitor)
        try:
            offset = options.trace_log.stat().st_size
            subprocess.run(
                mobiledevice_command(
                    options.python,
                    options.client,
                    options.usboip,
                    mux_address,
                    ["ios4-sleep"],
                ),
                check=True,
                timeout=options.timeout,
            )
            print("sleep submitted; waiting for a clean FTL root", flush=True)
            wait_for_clean_ftl_commit(
                options.trace_log,
                offset,
                options.timeout,
            )
            terminate_qemu(options.pidfile, options.qemu, options.monitor)
        except BaseException:
            disable_ftl_commit_trace(options.monitor)
            raise
        print("clean FTL root committed; QEMU stop requested before deep sleep")
    finally:
        if mux_process is not None:
            mux_process.terminate()
            try:
                mux_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                mux_process.kill()
                mux_process.wait()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
