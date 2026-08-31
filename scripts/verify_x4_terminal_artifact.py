#!/usr/bin/env python3
"""Reject an X4 Pro artifact that was built without the Terminal feature."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


ELF_MARKERS = (
    b"BleTerminalActivity",
    b"BleTerminalTransport",
    b"x4terminal-v",
)
BIN_MARKERS = (
    b"X4 Terminal",
    b"x4terminal-v",
)


def require_markers(path: Path, markers: tuple[bytes, ...]) -> bytes:
    try:
        contents = path.read_bytes()
    except OSError as error:
        raise SystemExit(f"x4-terminal verification failed: {error}") from error

    missing = [marker.decode("ascii") for marker in markers if marker not in contents]
    if missing:
        joined = ", ".join(missing)
        raise SystemExit(
            f"x4-terminal verification failed: {path} is missing {joined}; "
            "build the x4pro-ble-terminal environment"
        )
    return contents


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "build_dir",
        nargs="?",
        type=Path,
        default=Path(".pio/build/x4pro-ble-terminal"),
    )
    arguments = parser.parse_args()

    elf = arguments.build_dir / "firmware.elf"
    binary = arguments.build_dir / "firmware.bin"
    require_markers(elf, ELF_MARKERS)
    binary_contents = require_markers(binary, BIN_MARKERS)
    digest = hashlib.sha256(binary_contents).hexdigest()
    print(f"verified X4 Terminal firmware: {binary} ({len(binary_contents)} bytes)")
    print(f"sha256: {digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
