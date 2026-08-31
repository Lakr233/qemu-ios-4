#!/usr/bin/env python3
"""Create and prepare a disk-bound iPhone 3G QEMU ready-state checkpoint."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import socket
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Any


CHECKPOINT_VERSION = 1
MANIFEST_NAME = "manifest.json"
STATE_NAME = "vmstate"
WORKSPACE_MARKER = ".iphone3g-quick-workspace"
CHECKPOINT_FILES = (
    STATE_NAME,
    "nand.raw",
    "nor.raw",
    "gid-key.bin",
    "uid-key.bin",
)


class QMPClient:
    def __init__(self, path: Path, timeout: float) -> None:
        self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._socket.settimeout(timeout)
        self._socket.connect(str(path))
        self._reader = self._socket.makefile("rb")
        greeting = self._read_message()
        if "QMP" not in greeting:
            raise RuntimeError(f"invalid QMP greeting: {greeting!r}")
        self.execute("qmp_capabilities")

    def close(self) -> None:
        self._reader.close()
        self._socket.close()

    def _read_message(self) -> dict[str, Any]:
        while True:
            line = self._reader.readline()
            if not line:
                raise RuntimeError("QMP connection closed")
            message = json.loads(line)
            if "event" not in message:
                return message

    def execute(self, command: str, arguments: dict[str, Any] | None = None) -> Any:
        request: dict[str, Any] = {"execute": command}
        if arguments is not None:
            request["arguments"] = arguments
        self._socket.sendall(json.dumps(request).encode() + b"\r\n")
        response = self._read_message()
        if "error" in response:
            raise RuntimeError(f"QMP {command} failed: {response['error']!r}")
        if "return" not in response:
            raise RuntimeError(f"invalid QMP response: {response!r}")
        return response["return"]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb", buffering=0) as stream:
        while chunk := stream.read(16 * 1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def wait_for_migration(client: QMPClient, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        migration = client.execute("query-migrate")
        status = migration.get("status")
        if status == "completed":
            vm = client.execute("query-status")
            if vm.get("status") != "postmigrate" or vm.get("running"):
                raise RuntimeError(f"migration completed in unexpected VM state: {vm!r}")
            return
        if status in ("failed", "cancelled"):
            description = migration.get("error-desc", "no error description")
            raise RuntimeError(f"migration {status}: {description}")
        time.sleep(0.1)
    raise TimeoutError(f"QEMU migration did not complete within {timeout:g}s")


def pause_source(client: QMPClient) -> bool:
    state = client.execute("query-status")
    was_running = bool(state.get("running"))
    if was_running:
        client.execute("stop")
        state = client.execute("query-status")
    if state.get("running"):
        raise RuntimeError(f"checkpoint source did not pause: {state!r}")
    return was_running


def recover_source(
    client: QMPClient,
    resume: bool,
    timeout: float = 5.0,
) -> None:
    try:
        client.execute("migrate_cancel")
    except (OSError, RuntimeError):
        pass
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if client.execute("query-migrate").get("status") not in (
                "setup",
                "active",
                "pre-switchover",
                "device",
                "wait-unplug",
            ):
                break
        except (OSError, RuntimeError):
            return
        time.sleep(0.1)
    if resume:
        try:
            resume_source(client)
        except (OSError, RuntimeError):
            pass


def resume_source(client: QMPClient) -> None:
    state = client.execute("query-status")
    if not state.get("running"):
        client.execute("cont")
        state = client.execute("query-status")
    if not state.get("running"):
        raise RuntimeError(f"checkpoint source did not resume: {state!r}")


def resume_vm_when_ready(qmp: Path, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        client: QMPClient | None = None
        try:
            client = QMPClient(qmp, min(1.0, max(0.1, deadline - time.monotonic())))
            resume_source(client)
            return
        except (OSError, RuntimeError) as error:
            last_error = error
        finally:
            if client is not None:
                client.close()
        time.sleep(0.1)
    raise TimeoutError(
        f"resumed QEMU did not become runnable within {timeout:g}s"
    ) from last_error


def clone_file(source: Path, destination: Path) -> None:
    subprocess.run(["cp", "-c", str(source), str(destination)], check=True)


def checkpoint_manifest(directory: Path) -> dict[str, Any]:
    files: dict[str, dict[str, Any]] = {}
    for name in CHECKPOINT_FILES:
        path = directory / name
        files[name] = {"size": path.stat().st_size, "sha256": sha256_file(path)}
    return {"version": CHECKPOINT_VERSION, "files": files}


def validate_checkpoint(directory: Path) -> dict[str, Any]:
    manifest_path = directory / MANIFEST_NAME
    try:
        manifest = json.loads(manifest_path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"cannot read checkpoint manifest {manifest_path}") from error
    if manifest.get("version") != CHECKPOINT_VERSION:
        raise RuntimeError(f"unsupported checkpoint version in {manifest_path}")
    files = manifest.get("files")
    if not isinstance(files, dict) or set(files) != set(CHECKPOINT_FILES):
        raise RuntimeError(f"invalid checkpoint file set in {manifest_path}")
    for name in CHECKPOINT_FILES:
        expected = files[name]
        path = directory / name
        if (
            not isinstance(expected, dict)
            or not isinstance(expected.get("size"), int)
            or not isinstance(expected.get("sha256"), str)
        ):
            raise RuntimeError(f"invalid checkpoint record for {path}")
        if not path.is_file() or path.stat().st_size != expected["size"]:
            raise RuntimeError(f"checkpoint size mismatch for {path}")
        if sha256_file(path) != expected["sha256"]:
            raise RuntimeError(f"checkpoint hash mismatch for {path}")
    return manifest


def create_checkpoint(
    qmp: Path,
    checkpoint: Path,
    nand: Path,
    nor: Path,
    gid_key: Path,
    uid_key: Path,
    timeout: float,
) -> None:
    if checkpoint.exists():
        raise RuntimeError(f"checkpoint already exists: {checkpoint}")
    for path in (nand, nor, gid_key, uid_key):
        if not path.is_file():
            raise RuntimeError(f"checkpoint input does not exist: {path}")

    checkpoint.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(
        tempfile.mkdtemp(prefix=f".{checkpoint.name}.", dir=checkpoint.parent)
    )
    client = QMPClient(qmp, min(timeout, 10))
    source_was_running = False
    source_state_known = False
    published = False
    try:
        source_was_running = pause_source(client)
        source_state_known = True
        state = staging / STATE_NAME
        client.execute("migrate", {"uri": f"file:{state}"})
        wait_for_migration(client, timeout)

        for source, name in (
            (nand, "nand.raw"),
            (nor, "nor.raw"),
            (gid_key, "gid-key.bin"),
            (uid_key, "uid-key.bin"),
        ):
            clone_file(source, staging / name)
        manifest = checkpoint_manifest(staging)
        (staging / MANIFEST_NAME).write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n"
        )
        for name in (*CHECKPOINT_FILES, MANIFEST_NAME):
            (staging / name).chmod(0o444)
        staging.rename(checkpoint)
        published = True
        if source_was_running:
            try:
                resume_source(client)
            except (OSError, RuntimeError):
                recover_source(client, resume=True)
                raise
    finally:
        if source_state_known and not published:
            recover_source(
                client,
                resume=source_was_running,
            )
        client.close()
        if staging.exists():
            shutil.rmtree(staging)


def prepare_resume(checkpoint: Path, run_directory: Path) -> None:
    validate_checkpoint(checkpoint)
    if run_directory.exists():
        marker = run_directory / WORKSPACE_MARKER
        if not marker.is_file():
            raise RuntimeError(
                f"refusing to replace unowned resume directory {run_directory}"
            )
        pidfile = run_directory / "qemu.pid"
        if pidfile.is_file():
            try:
                pid = int(pidfile.read_text().strip())
                os.kill(pid, 0)
            except (OSError, ValueError):
                pass
            else:
                raise RuntimeError(f"resume QEMU is still running as pid {pid}")
        shutil.rmtree(run_directory)

    run_directory.mkdir(parents=True)
    (run_directory / WORKSPACE_MARKER).write_text("owned by checkpoint-iphone3g.py\n")
    for name in ("nand.raw", "nor.raw", "gid-key.bin", "uid-key.bin"):
        destination = run_directory / name
        clone_file(checkpoint / name, destination)
        destination.chmod(0o600)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    create = subparsers.add_parser("create")
    create.add_argument("--qmp", type=Path, required=True)
    create.add_argument("--checkpoint", type=Path, required=True)
    create.add_argument("--nand", type=Path, required=True)
    create.add_argument("--nor", type=Path, required=True)
    create.add_argument("--gid-key", type=Path, required=True)
    create.add_argument("--uid-key", type=Path, required=True)
    create.add_argument("--timeout", type=float, default=180)

    prepare = subparsers.add_parser("prepare-resume")
    prepare.add_argument("--checkpoint", type=Path, required=True)
    prepare.add_argument("--run-directory", type=Path, required=True)

    resume = subparsers.add_parser("resume-vm")
    resume.add_argument("--qmp", type=Path, required=True)
    resume.add_argument("--timeout", type=float, default=60)

    options = parser.parse_args()
    if options.command == "create":
        if options.timeout <= 0:
            parser.error("--timeout must be positive")
        create_checkpoint(
            options.qmp,
            options.checkpoint,
            options.nand,
            options.nor,
            options.gid_key,
            options.uid_key,
            options.timeout,
        )
        print(f"created ready-state checkpoint {options.checkpoint}")
    elif options.command == "prepare-resume":
        prepare_resume(options.checkpoint, options.run_directory)
        print(f"prepared checkpoint resume workspace {options.run_directory}")
    else:
        if options.timeout <= 0:
            parser.error("--timeout must be positive")
        resume_vm_when_ready(options.qmp, options.timeout)
        print(f"continued resumed VM through {options.qmp}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
