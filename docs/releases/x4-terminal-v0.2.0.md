# X4 Terminal v0.2.0 — tail replacement

Protocol-v2 release for **Xteink X4 Pro only**. It keeps locally cached pages
when Codex or another terminal program redraws its current status area.

The firmware identifies itself as
`1.5.0-x4pro-x4terminal-v0.2.0`.

## Protocol change

- Add ordered `STREAM_TRUNCATE` packets that remove a validated UTF-8 byte count
  from the transcript tail before replacement text is appended.
- Preserve older pages instead of clearing the full 32 KiB reader cache during
  a terminal redraw.
- Use protocol version 2 and separate service/characteristic UUIDs, preventing a
  v2 APK from silently connecting to incompatible v1 firmware.
- Keep packet processing allocation-free; the existing activity-owned 32 KiB
  buffer and memory budget are unchanged.

## Verification completed

- Firmware protocol host tests cover tail replacement, UTF-8 boundaries,
  invalid ranges, sequence gaps, and duplicate truncates.
- Android protocol tests cover byte vectors, separate source/reader shadows,
  short reconnect replay, and preservation of older local history.
- The full `x4pro-ble-terminal` ESP32-S3 firmware build succeeds with 28.7%
  static RAM and 88.6% of the application flash partition used.
- The Windows smoke client uses the protocol-v2 UUIDs and exercises reset,
  multi-page append, tail truncate, and replacement append; its packet
  self-test passes.

## Hardware verification

Installed on a physical Xteink X4 Pro on 2026-08-29. The reader booted the
`1.5.0-x4pro-x4terminal-v0.2.0` image and remained operational.
