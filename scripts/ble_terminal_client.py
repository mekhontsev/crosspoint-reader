#!/usr/bin/env python3
"""Small desktop smoke-test client for the X4 Terminal BLE protocol."""

from __future__ import annotations

import argparse
import asyncio
import re
import secrets
import struct
import sys
import zlib
from collections.abc import Iterable, Iterator
from dataclasses import dataclass


DEVICE_NAME = "X4 Terminal"
RX_CHARACTERISTIC_UUID = "6f2c8f10-7f7a-4f1e-a2a6-8e8b7b3f6d12"
TX_CHARACTERISTIC_UUID = "6f2c8f10-7f7a-4f1e-a2a6-8e8b7b3f6d13"

MAGIC = b"PW"
PROTOCOL_VERSION = 4
FRAME_BEGIN = 1
FRAME_DATA = 2
ACTION = 3
COMMAND = 4
FRAME_COMMIT = 5
FRAME_REQUEST = 6
FRAME_STATUS = 7
VIEWPORT = 8
PACKET_HEADER_BYTES = 10
MAX_PACKET_BYTES = 244
MAX_PAYLOAD_BYTES = MAX_PACKET_BYTES - PACKET_HEADER_BYTES
MAX_FRAME_BYTES = 6 * 1024

FRAME_FLAG_LATEST = 1
FRAME_FLAG_PRESENT = 2
FRAME_FLAG_RESET_CACHE = 4

ACTION_NAMES = {
    1: "interrupt",
    2: "approve",
    3: "reject",
    4: "submit-input",
}

FRAME_REQUEST_NAMES = {0: "current", 1: "previous", 2: "next"}
FRAME_STATUS_NAMES = {0: "ready", 1: "retry"}

# Covers CSI, OSC, DCS, SOS, PM, APC, and two-byte ESC sequences. This is a
# test bridge, not a terminal emulator; cursor-addressed output is intentionally
# reduced to plain text before it enters an atomic frame.
ANSI_ESCAPE = re.compile(
    r"\x1B(?:"
    r"\[[0-?]*[ -/]*[@-~]"
    r"|\][^\x1B\x07]*(?:\x07|\x1B\\)"
    r"|[PX^_][^\x1B]*(?:\x1B\\)"
    r"|[@-_]"
    r")"
)


def make_packet(packet_type: int, sequence: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD_BYTES:
        raise ValueError("payload is too large")
    return struct.pack(
        "<2sBBIH", MAGIC, PROTOCOL_VERSION, packet_type, sequence & 0xFFFFFFFF, len(payload)
    ) + payload


def make_frame_begin(sequence: int, frame_id: int, frame: bytes, flags: int) -> bytes:
    if not 1 <= frame_id <= 0xFFFFFFFF or len(frame) > MAX_FRAME_BYTES:
        raise ValueError("invalid frame metadata")
    payload = struct.pack("<IHIB", frame_id, len(frame), zlib.crc32(frame), flags)
    return make_packet(FRAME_BEGIN, sequence, payload)


def make_frame_commit(sequence: int, frame_id: int) -> bytes:
    if not 1 <= frame_id <= 0xFFFFFFFF:
        raise ValueError("invalid frame id")
    return make_packet(FRAME_COMMIT, sequence, struct.pack("<I", frame_id))


def clean_display_text(text: str) -> str:
    text = ANSI_ESCAPE.sub("", text).replace("\r\n", "\n").replace("\r", "\n")
    return "".join(char for char in text if char in "\n\t" or ord(char) >= 0x20 and not 0x7F <= ord(char) <= 0x9F)


def utf8_chunks(text: str, byte_limit: int) -> Iterator[bytes]:
    if byte_limit < 4:
        raise ValueError("byte limit must fit every UTF-8 code point")

    chunk = bytearray()
    for char in text:
        encoded = char.encode("utf-8")
        if chunk and len(chunk) + len(encoded) > byte_limit:
            yield bytes(chunk)
            chunk.clear()
        chunk.extend(encoded)
    if chunk:
        yield bytes(chunk)


@dataclass(frozen=True)
class ReaderPacket:
    description: str
    packet_type: int
    frame_id: int = 0
    control: int = 0


def parse_reader_packet(data: bytearray) -> ReaderPacket | None:
    if len(data) < PACKET_HEADER_BYTES:
        return None
    magic, version, packet_type, sequence, payload_length = struct.unpack("<2sBBIH", data[:10])
    if magic != MAGIC or version != PROTOCOL_VERSION or payload_length != len(data) - PACKET_HEADER_BYTES:
        return None

    payload = bytes(data[PACKET_HEADER_BYTES:])
    if packet_type == ACTION:
        if payload_length != 1 or payload[0] not in ACTION_NAMES:
            return None
        action = ACTION_NAMES[payload[0]]
        return ReaderPacket(f"action={action}, sequence={sequence}", ACTION)

    if packet_type == COMMAND:
        if not payload:
            return None
        try:
            command = payload.decode("utf-8")
        except UnicodeDecodeError:
            return None
        if any(char in "\n\t" or ord(char) < 0x20 or 0x7F <= ord(char) <= 0x9F for char in command):
            return None
        return ReaderPacket(f"command={command!r} + Enter, sequence={sequence}", COMMAND)

    if packet_type == FRAME_REQUEST:
        if payload_length != 5 or payload[0] not in FRAME_REQUEST_NAMES:
            return None
        anchor = struct.unpack("<I", payload[1:])[0]
        name = FRAME_REQUEST_NAMES[payload[0]]
        return ReaderPacket(
            f"frame-request={name}, anchor={anchor}, sequence={sequence}",
            FRAME_REQUEST,
            anchor,
            payload[0],
        )

    if packet_type == FRAME_STATUS:
        if payload_length != 5 or payload[4] not in FRAME_STATUS_NAMES:
            return None
        frame_id = struct.unpack("<I", payload[:4])[0]
        if frame_id == 0:
            return None
        name = FRAME_STATUS_NAMES[payload[4]]
        return ReaderPacket(
            f"frame-status={name}, frame={frame_id}, sequence={sequence}",
            FRAME_STATUS,
            frame_id,
            payload[4],
        )

    if packet_type == VIEWPORT:
        if payload_length != 4:
            return None
        columns, rows = struct.unpack("<HH", payload)
        if columns == 0 or rows == 0:
            return None
        return ReaderPacket(f"viewport={columns}x{rows}, sequence={sequence}", VIEWPORT)

    return None


def demo_source() -> Iterable[str]:
    yield "X4 Terminal BLE test\n"
    yield "Complete UTF-8 screens are committed atomically.\n"
    yield "The sender waits while the reader refreshes its e-ink panel.\n"
    yield "Русский текст: соединение работает.\n"


def bound_frame(text: str) -> str:
    encoded = text.encode("utf-8")
    if len(encoded) <= MAX_FRAME_BYTES:
        return text
    encoded = encoded[-MAX_FRAME_BYTES:]
    while encoded and encoded[0] & 0xC0 == 0x80:
        encoded = encoded[1:]
    text = encoded.decode("utf-8")
    newline = text.find("\n")
    return text[newline + 1 :] if newline >= 0 else text


def frame_packets(text: str, frame_id: int, sequence: int, packet_limit: int, flags: int):
    encoded = text.encode("utf-8")
    yield make_frame_begin(sequence, frame_id, encoded, flags)
    sequence = (sequence + 1) & 0xFFFFFFFF
    for payload in utf8_chunks(text, packet_limit - PACKET_HEADER_BYTES):
        yield make_packet(FRAME_DATA, sequence, payload)
        sequence = (sequence + 1) & 0xFFFFFFFF
    yield make_frame_commit(sequence, frame_id)


def run_self_test() -> None:
    begin = make_frame_begin(7, 9, b"hi", FRAME_FLAG_LATEST | FRAME_FLAG_PRESENT)
    assert begin.hex() == "50570401070000000b00090000000200ac2a93d803"
    assert make_packet(FRAME_DATA, 8, b"hi").hex() == "505704020800000002006869"
    assert make_frame_commit(9, 9).hex() == "5057040509000000040009000000"
    assert clean_display_text("a\x1b[31mred\x1b[0m\r\nb\x00") == "ared\nb"
    chunks = list(utf8_chunks("AЖ🙂B", 5))
    assert b"".join(chunks).decode("utf-8") == "AЖ🙂B"
    assert all(len(chunk) <= 5 for chunk in chunks)
    action = make_packet(ACTION, 11, b"\x01")
    assert parse_reader_packet(bytearray(action)).description == "action=interrupt, sequence=11"
    command = make_packet(COMMAND, 14, "echo Привет".encode())
    assert parse_reader_packet(bytearray(command)).description == "command='echo Привет' + Enter, sequence=14"
    assert parse_reader_packet(bytearray(make_packet(COMMAND, 15, b"bad\ncommand"))) is None
    request = make_packet(FRAME_REQUEST, 16, struct.pack("<BI", 1, 9))
    assert parse_reader_packet(bytearray(request)).description == (
        "frame-request=previous, anchor=9, sequence=16"
    )
    status = make_packet(FRAME_STATUS, 17, struct.pack("<IB", 9, 0))
    assert parse_reader_packet(bytearray(status)).description == (
        "frame-status=ready, frame=9, sequence=17"
    )
    viewport = make_packet(VIEWPORT, 18, struct.pack("<HH", 42, 38))
    assert parse_reader_packet(bytearray(viewport)).description == "viewport=42x38, sequence=18"
    assert len(bound_frame("line\n" * 2000).encode("utf-8")) <= MAX_FRAME_BYTES
    print("BLE terminal client self-test passed")


async def text_source(path: str | None, demo_delay: float):
    if path is None:
        for text in demo_source():
            yield text
            await asyncio.sleep(demo_delay)
        return

    if path == "-":
        while True:
            line = await asyncio.to_thread(sys.stdin.readline)
            if line == "":
                return
            yield line
        return

    with open(path, "r", encoding="utf-8", errors="replace", newline="") as source:
        while block := source.read(4096):
            yield block


async def pair_with_reader_passkey(client) -> None:
    """Pair without Bleak's Windows-only Just Works fallback."""
    if sys.platform != "win32":
        await client.pair()
        return

    from winrt.windows.devices.enumeration import (
        DeviceInformation,
        DevicePairingKinds,
        DevicePairingProtectionLevel,
        DevicePairingResultStatus,
    )

    requester = getattr(client._backend, "_requester", None)
    if requester is None:
        raise SystemExit("Windows BLE requester is unavailable")

    device_information = await DeviceInformation.create_from_id_async(
        requester.device_information.id
    )
    if device_information.pairing.is_paired:
        level = device_information.pairing.protection_level
        if level != DevicePairingProtectionLevel.ENCRYPTION_AND_AUTHENTICATION:
            raise SystemExit(
                f"Existing Windows bond has insufficient protection ({level.name}); "
                "run once with --unpair and pair again."
            )
        print("Existing authenticated Windows bond reused")
        return

    custom_pairing = device_information.pairing.custom

    def on_pairing_requested(_sender, event_args) -> None:
        if event_args.pairing_kind != DevicePairingKinds.PROVIDE_PIN:
            print(f"Refusing unexpected pairing ceremony: {event_args.pairing_kind.name}")
            return

        while True:
            pin = input("Enter the 6-digit code shown on the X4 reader: ").strip()
            if len(pin) == 6 and pin.isascii() and pin.isdigit():
                event_args.accept_with_pin(pin)
                return
            print("The pairing code must contain exactly 6 digits.")

    token = custom_pairing.add_pairing_requested(on_pairing_requested)
    try:
        result = await custom_pairing.pair_with_protection_level_async(
            DevicePairingKinds.PROVIDE_PIN,
            DevicePairingProtectionLevel.ENCRYPTION_AND_AUTHENTICATION,
        )
    finally:
        custom_pairing.remove_pairing_requested(token)

    if result.status not in (
        DevicePairingResultStatus.PAIRED,
        DevicePairingResultStatus.ALREADY_PAIRED,
    ):
        raise SystemExit(f"Authenticated pairing failed: {result.status.name}")

    device_information = await DeviceInformation.create_from_id_async(
        requester.device_information.id
    )
    level = device_information.pairing.protection_level
    if level != DevicePairingProtectionLevel.ENCRYPTION_AND_AUTHENTICATION:
        raise SystemExit(f"Windows reports insufficient pairing protection: {level.name}")
    print("Authenticated encrypted pairing completed")


async def write_packet(client, characteristic: str, packet: bytes, retries: int) -> None:
    retries = max(0, retries)
    for attempt in range(retries + 1):
        try:
            await client.write_gatt_char(characteristic, packet, response=True)
            return
        except OSError:
            if attempt >= retries or not client.is_connected:
                raise
            delay = 0.5 * (attempt + 1)
            print(f"BLE write was interrupted; retrying the same packet in {delay:g}s ...")
            await asyncio.sleep(delay)


async def send(args: argparse.Namespace) -> None:
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError as error:
        raise SystemExit('Bleak is missing. Install it with: py -3.10 -m pip install "bleak>=1,<4"') from error

    if args.address:
        print(f"Connecting to {args.address} ...")
        device = args.address
    else:
        print(f'Scanning for "{DEVICE_NAME}" ...')
        device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=args.scan_timeout)
        if device is None:
            raise SystemExit(f'No "{DEVICE_NAME}" advertisement found. Open X4 Terminal on the reader first.')

    if args.unpair:
        client = BleakClient(device, timeout=args.connect_timeout)
        await client.unpair()
        print("Windows bond removed; reopen Terminal on the reader before reconnecting")
        return

    print("Connecting and requesting authenticated pairing ...")
    async with BleakClient(device, timeout=args.connect_timeout) as client:
        if not client.is_connected:
            raise SystemExit("BLE connection failed")

        await pair_with_reader_passkey(client)

        # An ATT Write Request can carry MTU - 3 bytes. Keep the protocol's
        # absolute 244-byte value limit as a ceiling.
        packet_limit = min(MAX_PACKET_BYTES, max(PACKET_HEADER_BYTES + 4, client.mtu_size - 3))
        print(f"Connected; ATT MTU={client.mtu_size}, frame-data payload={packet_limit - PACKET_HEADER_BYTES} bytes")

        initial_request = asyncio.Event()
        frame_finished = asyncio.Event()
        expected_status_frame = 0
        retry_requested = False

        def on_reader_packet(_characteristic, data: bytearray) -> None:
            nonlocal retry_requested
            description = parse_reader_packet(data)
            print(f"Reader -> PC: {description.description if description else ('invalid packet ' + data.hex())}")
            if description is None:
                return
            if description.packet_type == FRAME_REQUEST and description.control == 0:
                initial_request.set()
            elif description.packet_type == FRAME_STATUS and description.frame_id == expected_status_frame:
                retry_requested = description.control == 1
                frame_finished.set()

        await client.start_notify(TX_CHARACTERISTIC_UUID, on_reader_packet)

        try:
            await asyncio.wait_for(initial_request.wait(), timeout=15.0)
        except asyncio.TimeoutError as error:
            raise SystemExit("The reader did not request its current screen") from error

        sequence = secrets.randbits(32)
        frame_id = secrets.randbelow(0xFFFFFFFE) + 1
        current_text = ""
        first_frame = True

        async for raw_text in text_source(args.source, args.demo_delay):
            current_text = bound_frame(current_text + clean_display_text(raw_text))
            flags = FRAME_FLAG_LATEST | FRAME_FLAG_PRESENT
            if first_frame:
                flags |= FRAME_FLAG_RESET_CACHE
            first_frame = False

            while True:
                expected_status_frame = frame_id
                retry_requested = False
                frame_finished.clear()
                packets = list(frame_packets(current_text, frame_id, sequence, packet_limit, flags))
                await write_packet(
                    client, RX_CHARACTERISTIC_UUID, packets[0], args.write_retries
                )
                for packet in packets[1:]:
                    sequence = (sequence + 1) & 0xFFFFFFFF
                    await write_packet(client, RX_CHARACTERISTIC_UUID, packet, args.write_retries)
                    if args.packet_delay:
                        await asyncio.sleep(args.packet_delay)
                sequence = (sequence + 1) & 0xFFFFFFFF

                try:
                    await asyncio.wait_for(frame_finished.wait(), timeout=20.0)
                except asyncio.TimeoutError as error:
                    raise SystemExit(f"The reader did not finish frame {frame_id}") from error
                if not retry_requested:
                    break
                print(f"Reader requested retry for frame {frame_id}")

            frame_id = (frame_id + 1) & 0xFFFFFFFF or 1

        if args.wait_after:
            print(f"Transmission complete; waiting {args.wait_after:g} seconds for reader input ...")
            await asyncio.sleep(args.wait_after)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Send protocol-v4 atomic UTF-8 frames to the experimental X4 Terminal BLE screen."
    )
    parser.add_argument(
        "source",
        nargs="?",
        help='UTF-8 text file, or "-" for stdin. If omitted, send built-in test lines.',
    )
    parser.add_argument("--address", help="BLE address; omit to scan by advertised name")
    parser.add_argument("--scan-timeout", type=float, default=15.0)
    parser.add_argument("--connect-timeout", type=float, default=60.0)
    parser.add_argument(
        "--packet-delay",
        type=float,
        default=0.0,
        help="optional diagnostic pause after each acknowledged packet (default: none)",
    )
    parser.add_argument("--write-retries", type=int, default=3)
    parser.add_argument("--demo-delay", type=float, default=1.0)
    parser.add_argument("--wait-after", type=float, default=120.0)
    parser.add_argument("--unpair", action="store_true", help="remove the Windows bond and exit")
    parser.add_argument("--self-test", action="store_true", help="test packet code without Bluetooth")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.self_test:
        run_self_test()
        return
    try:
        asyncio.run(send(args))
    except KeyboardInterrupt:
        print("\nStopped")


if __name__ == "__main__":
    main()
