#!/usr/bin/env python3
"""Send deterministic iPhone 3G button or touch input through QEMU QMP."""

from __future__ import annotations

import argparse
import json
import socket
import time
from pathlib import Path
from typing import Any


ABS_MAX = 0x7FFF
DEFAULT_GESTURE_STEPS = 12


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

    def pointer(self, x: int, y: int, down: bool | None = None) -> None:
        events: list[dict[str, Any]] = [
            {"type": "abs", "data": {"axis": "x", "value": x}},
            {"type": "abs", "data": {"axis": "y", "value": y}},
        ]
        if down is not None:
            events.append(
                {"type": "btn", "data": {"button": "left", "down": down}}
            )
        self.execute("input-send-event", {"events": events})

    def key(self, qcode: str, down: bool) -> None:
        self.execute(
            "input-send-event",
            {
                "events": [
                    {
                        "type": "key",
                        "data": {
                            "down": down,
                            "key": {"type": "qcode", "data": qcode},
                        },
                    }
                ]
            },
        )


def scale(value: float, extent: int) -> int:
    if extent <= 1 or value < 0 or value > extent - 1:
        raise ValueError(f"coordinate {value} lies outside extent {extent}")
    return round(value * ABS_MAX / (extent - 1))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qmp", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--duration", type=float, default=0.8)
    parser.add_argument("--steps", type=int, default=DEFAULT_GESTURE_STEPS)
    parser.add_argument("--from-x", type=float, default=55)
    parser.add_argument("--to-x", type=float, default=310)
    parser.add_argument("--y", type=float, default=430)
    parser.add_argument("--key", choices=("home", "power"))
    parser.add_argument("--key-duration", type=float, default=0.2)
    options = parser.parse_args()

    if not options.key and options.steps < 2:
        parser.error("--steps must be at least 2")
    if options.duration < 0:
        parser.error("--duration cannot be negative")
    if options.key_duration < 0:
        parser.error("--key-duration cannot be negative")

    client = QMPClient(options.qmp, options.timeout)
    if options.key:
        try:
            client.key(options.key, True)
            if options.key_duration:
                time.sleep(options.key_duration)
            client.key(options.key, False)
        finally:
            client.close()
        print(f"sent iPhone 3G {options.key} button")
        return 0

    y = scale(options.y, 480)
    points = [
        scale(
            options.from_x
            + (options.to_x - options.from_x) * step / (options.steps - 1),
            320,
        )
        for step in range(options.steps)
    ]
    delay = options.duration / (options.steps - 1)

    try:
        client.pointer(points[0], y, True)
        for x in points[1:]:
            if delay:
                time.sleep(delay)
            client.pointer(x, y)
        client.pointer(points[-1], y, False)
    finally:
        client.close()

    print(
        f"sent Zephyr2 swipe ({options.from_x:g},{options.y:g}) -> "
        f"({options.to_x:g},{options.y:g}) in {options.steps} frames"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
