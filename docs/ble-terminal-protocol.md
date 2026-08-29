# X4 Terminal BLE Protocol

This is an experimental, X4 Pro-only protocol for displaying a slow-moving
plain-text view of an ordinary terminal session. The process in that terminal
may be a shell, Codex CLI, SSH, or any other line-oriented program. It is
intentionally not a full terminal emulator, remote shell, file transfer, or
pixel-frame protocol.

## Stream model

The Termux helper obtains a normalized plain-text view from `tmux`. Android
sends one `STREAM_RESET` when a connection starts, followed by ordered
`STREAM_APPEND` packets. For an append-only change it sends only the new suffix.
If cursor movement, line editing, screen clearing, or another redraw changes
older text, it sends a new reset followed by a bounded current snapshot.

The reader keeps the newest 32,768 UTF-8 bytes. When the buffer fills, it drops
old complete lines first. BLE reception and e-ink refresh are independent: the
firmware may coalesce several appends into one display refresh without losing
stream data.

The display scheduler never refreshes more often than once every 3 seconds.
During a continuous stream it shows the newest received state every 3 seconds;
after a shorter burst it waits 700 ms for more packets and draws the final state
when the minimum refresh interval permits. BLE reception continues independently
while the render task updates the panel. With no new data there are no display
refreshes. A valid packet keeps the normal auto-sleep timer reset for only 5
seconds; a quiet BLE connection does not keep the reader awake forever.

If a sequence gap is detected, the displayed text is preserved but further
appends are rejected. Android resynchronizes by sending a new `STREAM_RESET`
and then replaying its bounded latest snapshot as ordinary append packets.

## Buffer ownership and local navigation

`tmux` in Termux is the source of truth for the full terminal scrollback. The
Android bridge retains only the newest sanitized snapshot needed to reconnect
and replay. The reader owns a separate 32 KiB cache while the Terminal activity
is open.

The X4 Pro side keys page through that local cache. Paging and the nine local
IBM Plex Mono sizes (8 through 24 point in two-point steps) cause no BLE traffic
and do not wake the phone. The terminal font is compiled only into the dedicated
BLE Terminal profile; the regular CrossPoint profiles are unchanged. New
terminal output follows the tail while the reader is at the newest page. While
the user is looking at an older page, new appends are buffered but do not force
an e-ink refresh; returning to the newest page reveals them.

Leaving the Terminal activity deliberately stops BLE and releases the activity,
including its cache. On the next entry the reader advertises again. Android
reconnects, enables indications, sends `STREAM_RESET`, and replays its latest
snapshot. The BLE bond remains available, so a valid existing bond does not
normally require another passkey ceremony.

## Safety contract

- The feature is compiled only in the dedicated `x4pro-ble-terminal` environment.
- BLE initialization is forbidden during global construction and firmware
  startup. It may begin only after the user explicitly enters
  `BleTerminalActivity`.
- Leaving the activity must stop BLE and release every activity-owned resource.
- GATT traffic requires authenticated LE Secure Connections with a six-digit
  passkey displayed on the reader. Legacy pairing and unauthenticated writes
  are rejected.
- NimBLE may store up to three bonding records in its existing NVS area so a
  previously paired phone can reconnect securely. The feature does not write
  CrossPoint settings, SD data, OTA data, partition tables, or boot state.
- Incoming data is untrusted. Packet sizes, sequence numbers, UTF-8, and control
  characters are validated before display.
- Reader-to-client traffic is limited to allow-listed action IDs and a command
  line explicitly entered by the user on the reader's standard keyboard. A
  command is sent only over the authenticated encrypted indication channel.
  Firmware never executes commands itself.
- No code in this feature may change the early boot or SD recovery paths.

## Memory budget

The activity owns one 32,769-byte transcript buffer, including its trailing
NUL, plus one 256-byte reusable line-layout buffer. The X4 Pro build has 8 MB
PSRAM and routes allocations larger than 4 KiB through the external-memory-aware
allocator. Startup logs report the actual placement and the remaining heap so
it can be verified on hardware. This memory exists only while the activity is
open. Packet processing performs no allocation.

## GATT service

The reader advertises as `X4 Terminal` every 500-750 ms while this activity is
open. It requests a 15-30 ms, zero-latency connection while packets are moving,
then returns to a 100-200 ms interval with peripheral latency 4 after 2 seconds
without stream data.

| Role | UUID | Properties |
|---|---|---|
| Service | `6f2c8f10-7f7a-4f1e-a2a6-8e8b7b3f6c01` | Primary |
| Android to reader | `6f2c8f10-7f7a-4f1e-a2a6-8e8b7b3f6c02` | Authenticated encrypted write with response |
| Reader to Android | `6f2c8f10-7f7a-4f1e-a2a6-8e8b7b3f6c03` | Indicate only after authenticated encryption |

Write with response supplies backpressure for the reader's fixed eight-packet
queue. Android sends the next packet immediately after the acknowledgement; it
must retry a failed write instead of advancing its sequence. This protocol does
not pace BLE traffic to the e-ink refresh rate.

## Packet envelope

All integers are little-endian. One application packet occupies one GATT
characteristic value and is at most 244 bytes, matching an ATT MTU of 247.
With a smaller negotiated MTU, Android sends a smaller `STREAM_APPEND` payload.
Android must not split a UTF-8 code point between append packets.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 | Magic `XT` |
| 2 | 1 | Protocol version, currently `1` |
| 3 | 1 | Packet type |
| 4 | 4 | Sequence number |
| 8 | 2 | Payload length |
| 10 | N | Payload |

Packet types:

- `STREAM_RESET` (`1`): empty payload. It clears the reader buffer, accepts any
  sequence number, and establishes the next expected sequence number.
- `STREAM_APPEND` (`2`): one non-empty, independently valid UTF-8 text chunk.
  It is appended as soon as the packet is accepted.
- `ACTION` (`3`): one allow-listed action ID: interrupt, approve, reject,
  enter, page-up, or page-down. This packet is used only from the reader to
  Android. Page-up and page-down IDs remain reserved for compatibility with
  early test firmware; current firmware consumes the physical side keys locally.
- `COMMAND` (`4`): one non-empty UTF-8 command line entered on the reader,
  without a line ending. This packet is used only from the reader to Android.
  The bridge injects the payload into its terminal session and then injects
  Enter. Pressing Enter on an empty reader input sends the existing `enter`
  action instead.

Display text permits UTF-8, newline, and horizontal tab. NUL, carriage return,
ESC, C0/C1 controls, overlong encodings, surrogates, and invalid Unicode are
rejected. ANSI stripping and line-ending normalization belong to the eventual
Android bridge.

Command text permits printable UTF-8 only. Embedded newline, tab, NUL, ESC,
other controls, invalid Unicode, and payloads above 234 bytes are rejected.
The keyboard limits input to the payload supported by the negotiated ATT MTU.
Only the Terminal command keyboard enables a language key. It starts with the
reader's UI language when FreeInkUI provides that layout (French, German,
Spanish, Russian, Ukrainian, Belarusian, or Kazakh) and switches between that
layout and English. Other CrossPoint keyboards keep their existing behavior.
The side Page Up and Page Down buttons move through the local transcript one
screen at a time. Holding Page Down for 700 ms jumps directly to the live tail.

## Android connection and replay contract

The reader is the BLE peripheral and Android is the central. The Android bridge
must serialize these steps for every connection:

The normative two-component APK/Termux architecture, authenticated loopback
IPC, lifecycle, and acceptance criteria are specified in
[`x4-terminal-android-handoff.md`](x4-terminal-android-handoff.md).

1. Scan by the service UUID, not only by the display name.
2. Connect and establish an authenticated LE Secure Connections bond. Reuse an
   existing valid bond when Android offers it.
3. Discover the service and request ATT MTU 247.
4. Enable indications on the reader-to-Android characteristic and wait for the
   subscription operation to complete.
5. Pick any 32-bit initial sequence, send `STREAM_RESET`, then advance the
   sequence for every append packet.
6. Send the newest sanitized snapshot in independently valid UTF-8 chunks using
   write-with-response. Never split a code point. Do not advance the sequence
   after a failed write; retry the same packet after backpressure.
7. Confirm every incoming indication and dispatch `COMMAND` or `ACTION` to the
   Termux helper. Never execute reader text by constructing a shell command.

On disconnect, discard the connection sequence and all queued GATT work. Keep
the latest terminal snapshot, return to a low-power wait/scan state, and repeat
the complete sequence when the X4 service reappears. A reconnect always starts
with a reset; it never continues the previous packet sequence.

The Android service should remain idle when the reader is absent rather than
retrying a connection in a tight loop. The intended user-visible behavior is:
exit Terminal on the reader, use other reader screens, enter Terminal again,
and receive the latest Termux state automatically.

## Termux integration contract

The terminal to mirror runs inside a named `tmux` session. A small Termux helper
uses `pipe-pane` only as a change signal and obtains the rendered plain-text
state with `capture-pane`. It compares that state with the previous one:

- identical capture: send nothing;
- previous capture is a prefix: send only the suffix;
- any older text changed: replace the bounded snapshot with reset plus appends.

For reader input, the helper passes command text as a literal `tmux send-keys`
argument and then sends Enter separately. The empty-submit action sends Enter.
Current firmware emits no other action IDs; the remaining allow-listed IDs are
reserved and must not be mapped to guessed keystrokes. The helper must use an
argv-style process API rather than interpolate reader text into a shell command.

### Animation suppression

Raw pane output is never forwarded to BLE. Programs such as Codex frequently
redraw a spinner, progress indicator, or highlighted status text using terminal
control sequences. Even after ANSI removal those redraws may produce different
plain-text captures, so equality testing alone is insufficient.

The Termux helper maintains a candidate capture and a published capture:

- Pane output only marks the candidate dirty; it does not enqueue BLE data.
- Captures are debounced and normalized before comparison.
- The newest one or two rows are treated as a volatile tail while they keep
  changing. They are published only after remaining unchanged for 1.5 seconds.
- Stable rows above that tail may be published while the animation continues.
- A changed published snapshot is forwarded immediately after debounce. Its BLE
  packets are sent as fast as write acknowledgements allow; only the e-ink
  renderer, not BLE transport, has a three-second refresh limit.
- When the terminal becomes stable, the final tail and prompt are published.

This filtering belongs before BLE. Display-side coalescing alone would avoid
some panel refreshes but every packet would still wake the reader and extend its
short data-awake window.

## Desktop smoke test

The test client in `scripts/ble_terminal_client.py` can exercise the protocol
from Windows before the Android bridge exists. It sends only application data;
it cannot install or update firmware.

```powershell
py -3.10 -m pip install "bleak>=1,<4"
py -3.10 scripts/ble_terminal_client.py --self-test
py -3.10 scripts/ble_terminal_client.py
```

For the final command, first open **X4 Terminal** on the reader. On the first
connection, enter the six-digit code shown on the reader into the operating
system's pairing prompt. With no source argument the client sends four test
lines. Pass a UTF-8 file, or `-` to forward lines arriving on standard input.
The client strips common ANSI escape sequences and normalizes line endings, but
it deliberately does not emulate a cursor-addressed terminal.

Reader actions and commands are printed by this smoke-test client. For safety,
it does not execute or inject reader-entered commands into a local shell. By
default it keeps listening for reader input for two minutes after sending the
demo; use `--wait-after` to change that interval.

On Windows the client deliberately bypasses Bleak's built-in Just Works-only
pairing helper and registers a `PROVIDE_PIN` handler. It requests the operating
system's `ENCRYPTION_AND_AUTHENTICATION` protection level and aborts if Windows
reports anything weaker. Use `--unpair` to remove only the `X4 Terminal` bond
before repeating a first-pairing test.
