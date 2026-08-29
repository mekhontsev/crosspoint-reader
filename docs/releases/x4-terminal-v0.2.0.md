# X4 Terminal v0.2.0 — tail replacement candidate

> [!CAUTION]
> This candidate is not hardware-tested. Keep the v0.1.1 or another known-good
> X4 Pro image available before installation.

Protocol-v2 candidate for **Xteink X4 Pro only**. It keeps locally cached pages
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

## Hardware checks still required

1. Upgrade both firmware and APK; confirm the existing BLE bond reconnects.
2. Fill at least ten local pages, trigger repeated Codex status redraws, and
   confirm Page Up still reaches the older pages.
3. Confirm the APK sends truncate plus only the changed suffix, without repeated
   4 KiB resets.
4. Repeat Terminal exit/re-entry and verify the v0.1.1 exit-path correction.
5. Monitor free heap and confirm it remains above 50 KiB while receiving text.
