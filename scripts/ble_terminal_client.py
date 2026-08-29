#!/usr/bin/env python3
"""Small desktop smoke-test client for the X4 Terminal BLE protocol."""

from __future__ import annotations

import argparse
import asyncio
import re
import secrets
import struct
import sys
from collections.abc import Iterable, Iterator


DEVICE_NAME = "X4 Terminal"
RX_CHARACTERISTIC_UUID = "6f2c8f10-7f7a-4f1e-a2a6-8e8b7b3f6c02"
TX_CHARACTERISTIC_UUID = "6f2c8f10-7f7a-4f1e-a2a6-8e8b7b3f6c03"

MAGIC = b"XT"
PROTOCOL_VERSION = 1
STREAM_RESET = 1
STREAM_APPEND = 2
ACTION = 3
COMMAND = 4
PACKET_HEADER_BYTES = 10
MAX_PACKET_BYTES = 244
MAX_APPEND_BYTES = MAX_PACKET_BYTES - PACKET_HEADER_BYTES

ACTION_NAMES = {
    1: "interrupt",
    2: "approve",
    3: "reject",
    4: "submit-input",
    5: "page-up",
    6: "page-down",
}

# Covers CSI, OSC, DCS, SOS, PM, APC, and two-byte ESC sequences. This is a
# test bridge, not a terminal emulator; cursor-addressed output is intentionally
# reduced to a plain append-only stream.
ANSI_ESCAPE = re.compile(
    r"\x1B(?:"
    r"\[[0-?]*[ -/]*[@-~]"
    r"|\][^\x1B\x07]*(?:\x07|\x1B\\)"
    r"|[PX^_][^\x1B]*(?:\x1B\\)"
    r"|[@-_]"
    r")"
)


def make_packet(packet_type: int, sequence: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_APPEND_BYTES:
        raise ValueError("payload is too large")
    return struct.pack(
        "<2sBBIH", MAGIC, PROTOCOL_VERSION, packet_type, sequence & 0xFFFFFFFF, len(payload)
    ) + payload


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


def parse_reader_packet(data: bytearray) -> str | None:
    if len(data) < PACKET_HEADER_BYTES:
        return None
    magic, version, packet_type, sequence, payload_length = struct.unpack("<2sBBIH", data[:10])
    if magic != MAGIC or version != PROTOCOL_VERSION or payload_length != len(data) - PACKET_HEADER_BYTES:
        return None

    payload = bytes(data[PACKET_HEADER_BYTES:])
    if packet_type == ACTION:
        if payload_length != 1:
            return None
        action = ACTION_NAMES.get(payload[0], f"unknown-{payload[0]}")
        return f"action={action}, sequence={sequence}"

    if packet_type == COMMAND:
        if not payload:
            return None
        try:
            command = payload.decode("utf-8")
        except UnicodeDecodeError:
            return None
        if any(char in "\n\t" or ord(char) < 0x20 or 0x7F <= ord(char) <= 0x9F for char in command):
            return None
        return f"command={command!r} + Enter, sequence={sequence}"

    return None


def demo_source() -> Iterable[str]:
    yield "X4 Terminal BLE test\n"
    yield "Only new UTF-8 text is being transferred.\n"
    yield "The reader should coalesce these packets into slow e-ink refreshes.\n"
    yield "Русский текст: соединение работает.\n"


def run_self_test() -> None:
    reset = make_packet(STREAM_RESET, 7)
    assert reset == b"XT\x01\x01\x07\x00\x00\x00\x00\x00"
    assert clean_display_text("a\x1b[31mred\x1b[0m\r\nb\x00") == "ared\nb"
    chunks = list(utf8_chunks("AЖ🙂B", 5))
    assert b"".join(chunks).decode("utf-8") == "AЖ🙂B"
    assert all(len(chunk) <= 5 for chunk in chunks)
    action = make_packet(ACTION, 11, b"\x01")
    assert parse_reader_packet(bytearray(action)) == "action=interrupt, sequence=11"
    page_up = make_packet(ACTION, 12, b"\x05")
    page_down = make_packet(ACTION, 13, b"\x06")
    assert parse_reader_packet(bytearray(page_up)) == "action=page-up, sequence=12"
    assert parse_reader_packet(bytearray(page_down)) == "action=page-down, sequence=13"
    command = make_packet(COMMAND, 14, "echo Привет".encode())
    assert parse_reader_packet(bytearray(command)) == "command='echo Привет' + Enter, sequence=14"
    assert parse_reader_packet(bytearray(make_packet(COMMAND, 15, b"bad\ncommand"))) is None
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

        # An ATT Write Request can carry MTU - 3 bytes. Ten bytes belong to our
        # envelope, so the remaining bytes are safe for independently valid
        # UTF-8. Keep the protocol's absolute 244-byte value limit as a ceiling.
        packet_limit = min(MAX_PACKET_BYTES, max(PACKET_HEADER_BYTES + 4, client.mtu_size - 3))
        append_limit = packet_limit - PACKET_HEADER_BYTES
        print(f"Connected; ATT MTU={client.mtu_size}, append payload={append_limit} bytes")

        def on_reader_packet(_characteristic, data: bytearray) -> None:
            description = parse_reader_packet(data)
            print(f"Reader -> PC: {description or ('invalid packet ' + data.hex())}")

        await client.start_notify(TX_CHARACTERISTIC_UUID, on_reader_packet)

        sequence = secrets.randbits(32)
        await write_packet(
            client,
            RX_CHARACTERISTIC_UUID,
            make_packet(STREAM_RESET, sequence),
            args.write_retries,
        )
        sequence = (sequence + 1) & 0xFFFFFFFF

        async for raw_text in text_source(args.source, args.demo_delay):
            clean_text = clean_display_text(raw_text)
            for payload in utf8_chunks(clean_text, append_limit):
                await write_packet(
                    client,
                    RX_CHARACTERISTIC_UUID,
                    make_packet(STREAM_APPEND, sequence, payload),
                    args.write_retries,
                )
                sequence = (sequence + 1) & 0xFFFFFFFF
                if args.packet_delay:
                    await asyncio.sleep(args.packet_delay)

        if args.wait_after:
            print(f"Transmission complete; waiting {args.wait_after:g} seconds for reader input ...")
            await asyncio.sleep(args.wait_after)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Send append-only UTF-8 text to the experimental X4 Terminal BLE screen."
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
