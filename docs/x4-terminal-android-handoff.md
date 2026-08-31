# X4 Terminal Android Bridge

The Android bridge and the Termux helper are implemented in
[`mekhontsev/x4-terminal-bridge`](https://github.com/mekhontsev/x4-terminal-bridge).
The BLE wire format is defined by
[`ble-terminal-protocol.md`](ble-terminal-protocol.md); that document is
normative for firmware and bridge implementations.

## Implemented boundary

```text
visible tmux viewport
  -> x4term helper (normalized snapshots over authenticated loopback IPC)
  -> Android Java foreground service (history, animation gate, BLE central)
  -> X4 Pro Terminal activity (atomic frame cache and e-ink rendering)
```

The helper uses Python's standard library and `tmux`; it does no Bluetooth
work. The APK uses Java and Android's platform Bluetooth APIs. The reader's
normal menus, book reader, storage, and update code are outside this feature.

The bridge sends bounded complete screen frames, not terminal output events or
scrollback. Android suppresses in-place animation replacements, retains the
newest snapshot for an explicit reader refresh, and keeps a bounded 64-frame
history. The reader keeps four fixed-size frames and requests missing adjacent
frames from Android.

## Safety properties

- IPC listens only on loopback and requires the random token created by
  `x4term init`.
- BLE uses authenticated LE Secure Connections and write-with-response.
- A frame becomes visible only after `FRAME_BEGIN`, all `FRAME_DATA`, and
  `FRAME_COMMIT` pass length, UTF-8, sequence, frame-ID, and CRC-32 checks.
- Android waits for the reader's `READY` status after e-ink rendering before
  sending another frame; pending automatic updates are latest-wins.
- Reader input reaches `tmux send-keys -l` through an argv API and never passes
  through a shell parser.
- Reconnect transfers one current frame, not accumulated output events.

Build, installation, controls, and troubleshooting are documented in
[`x4-terminal-user-guide.md`](x4-terminal-user-guide.md) and the bridge
repository's README.
