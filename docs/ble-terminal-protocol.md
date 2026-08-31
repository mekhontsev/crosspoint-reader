# X4 Terminal BLE Protocol

This experimental X4 Pro-only protocol displays atomic plain-text snapshots of
one `tmux` viewport. It is not a terminal emulator, remote shell, file-transfer
protocol, pixel-frame protocol, or transport for raw ANSI output.

## Frame model

The Termux helper captures and normalizes only the current visible pane. Android
keeps the newest snapshot, classifies its changes, and sends a complete text
frame as `FRAME_BEGIN`, zero or more `FRAME_DATA` packets, and `FRAME_COMMIT`.
The reader does not expose or draw staging data. It accepts a frame only when its
declared UTF-8 byte length and CRC-32 both match at commit.

Android automatically sends forward-only changes: complete rows added below the
published screen or upward scrolling inside optional fixed TUI chrome. A capture
that only changes already displayed rows freezes automatic publication. Android
continues to retain the newest snapshot, but it sends no animation-only frames.
A Current request from the reader force-publishes that newest snapshot.
Automatic sending resumes as soon as a later capture demonstrates real upward
progress.

Only one frame may be in flight. Snapshots received while the reader is receiving
or refreshing replace one pending automatic snapshot; intermediate snapshots are
not queued. The reader sends `FRAME_STATUS(READY)` after an accepted frame has
either finished its requested e-ink refresh or was cached without a refresh.
Android does not begin another automatic frame before that status arrives.
An optional Android-side minimum interval further limits automatic frames with
the same latest-wins behavior. Current, history navigation, initial sync, and a
tmux source change bypass that interval.

## Reader cache and navigation

The reader owns four fixed 6 KiB frame slots while Terminal is open. Android
keeps up to 64 accepted frames. Side-key navigation uses an adjacent cached frame
without BLE; if that frame is absent, the reader asks Android for the previous or
next frame relative to the displayed frame ID.

New live frames are cached while the user reads an older frame, but they do not
move or refresh the displayed page. Current returns to the newest snapshot.
Holding Page Down also jumps to the newest cached frame and asks Android for its
current snapshot.

Leaving Terminal stops BLE and frees the activity and cache. Re-entering Terminal
starts a new connection. Its first frame carries `RESET_CACHE`, so stale frame IDs
from an earlier Android process cannot be mixed with the new history.
Selecting another tmux pane also sends its first frame with `RESET_CACHE` while
leaving the BLE connection intact.

## Reader controls

- Confirm: request and display Android's current snapshot.
- Hold Confirm for about 700 ms: request the current snapshot and use a full
  ghost-cleaning e-ink refresh when it arrives.
- Page Up / Page Down: previous or next frame.
- Hold Page Down for about 700 ms: return to the current live frame.
- Keyboard icon: send one literal command followed by Enter. Empty input sends
  Enter only.
- Refresh icon: touch equivalent of Confirm. A long touch requests Current with
  a full refresh; its corner marker indicates history mode.
- `-` / `+`: change the local terminal font.
- Back: leave Terminal and stop the BLE stack.

## Safety and memory

- The feature is compiled only in `x4pro-ble-terminal` and starts BLE only after
  the user enters `BleTerminalActivity`.
- GATT traffic requires authenticated LE Secure Connections with a random
  six-digit passkey. Unauthenticated writes are rejected.
- Incoming lengths, sequences, flags, UTF-8, control characters, frame IDs, and
  CRC-32 values are validated before a frame enters the cache.
- Reader text is data only. Firmware never executes it.
- Reader commands are allow-listed packet types and travel only over the
  authenticated indication characteristic.
- The feature does not write CrossPoint settings, SD data, OTA data, partition
  tables, boot state, or recovery state.

The activity uses four 6,145-byte committed-frame slots, one 6,145-byte staging
slot, and one 256-byte reusable layout buffer. These fixed members total about
30 KiB, slightly less than the previous 32 KiB stream buffer. Packet processing
does not allocate heap memory.

## GATT service

The reader advertises as `X4 Terminal` every 500-750 ms while Terminal is open.
It requests a 15-30 ms zero-latency connection while packets move, then returns
to a 100-200 ms interval with peripheral latency 4 after two quiet seconds.

| Role | UUID | Properties |
|---|---|---|
| Service | `6f2c8f10-7f7a-4f1e-a2a6-8e8b7b3f6d11` | Primary |
| Android to reader | `6f2c8f10-7f7a-4f1e-a2a6-8e8b7b3f6d12` | Authenticated encrypted write with response |
| Reader to Android | `6f2c8f10-7f7a-4f1e-a2a6-8e8b7b3f6d13` | Authenticated encrypted indication |

Protocol v3 has separate UUIDs from the incompatible stream-based v2 protocol.
The BLE bond is independent of service UUIDs and can normally be reused after
both components are upgraded.

## Packet envelope

All integers are little-endian. One packet occupies one GATT characteristic
value and is at most 244 bytes, matching ATT MTU 247. With a smaller MTU Android
reduces `FRAME_DATA` payloads. It never splits a UTF-8 code point between data
packets.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 | Magic `XT` |
| 2 | 1 | Protocol version `3` |
| 3 | 1 | Packet type |
| 4 | 4 | Connection-local sequence number |
| 8 | 2 | Payload length |
| 10 | N | Payload |

Android chooses any initial sequence for a connection. `FRAME_BEGIN` accepts any
sequence and establishes the next expected sequence, allowing recovery from an
abandoned transfer. Data and commit packets must then be consecutive. Retrying
the immediately preceding identical write is harmless.

## Android-to-reader packets

### `FRAME_BEGIN` (`1`)

| Payload offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Nonzero frame ID |
| 4 | 2 | Complete frame length, 0-6144 UTF-8 bytes |
| 6 | 4 | IEEE CRC-32 of the complete frame |
| 10 | 1 | Frame flags |

Flags:

- `LATEST` (`0x01`): newest accepted Android frame;
- `PRESENT` (`0x02`): explicit reader request; display even when reading history;
- `RESET_CACHE` (`0x04`): clear connection-stale reader frames before commit.

Unknown flag bits reject the frame.

### `FRAME_DATA` (`2`)

One non-empty independently valid UTF-8 chunk. The accumulated bytes may not
exceed the length declared by `FRAME_BEGIN`. A zero-length frame has no data
packets.

### `FRAME_COMMIT` (`5`)

Four-byte frame ID. It must match `FRAME_BEGIN`. Length or CRC mismatch rejects
the frame and leaves the last displayed e-ink image unchanged.

## Reader-to-Android packets

### `ACTION` (`3`)

One allow-listed byte: interrupt (`1`), approve (`2`), reject (`3`), or
submit-input (`4`). Current firmware emits submit-input only; other IDs remain
reserved and must not be mapped to guessed keystrokes.

### `COMMAND` (`4`)

One non-empty printable UTF-8 command line without a newline or tab, at most the
payload permitted by the negotiated MTU. The helper injects it with literal
`tmux send-keys -l`, waits briefly, and sends Enter separately.

### `FRAME_REQUEST` (`6`)

| Payload offset | Size | Field |
|---:|---:|---|
| 0 | 1 | Current (`0`), previous (`1`), or next (`2`) |
| 1 | 4 | Anchor frame ID, or zero before the first frame |

Current force-publishes the newest Android snapshot. Previous and next select a
history neighbor. If no neighbor exists, Android may return the anchor again.

### `FRAME_STATUS` (`7`)

| Payload offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Nonzero frame ID |
| 4 | 1 | Ready (`0`) or retry (`1`) |

Ready is sent after presentation finishes or after a non-presented history cache
insert. Retry asks Android to resend the same complete frame from a new begin.

## Text validation

Display text permits valid Unicode UTF-8, newline, and horizontal tab. NUL,
carriage return, ESC, DEL, other C0/C1 controls, overlong encodings, surrogates,
and invalid Unicode are rejected. The helper normalizes line endings, removes
forbidden controls and trailing screen padding, and limits a viewport to 6 KiB.

## Connection lifecycle

1. Android scans by the v3 service UUID and establishes authenticated LE Secure
   Connections.
2. Android discovers GATT, requests MTU 247, and enables indications.
3. The reader requests Current.
4. Android sends one atomic `PRESENT | LATEST | RESET_CACHE` frame.
5. The reader commits, displays, and sends Ready.
6. Later safe forward snapshots are sent only while the reader is ready.
7. An animation-only redraw stays on Android until Current is requested or a
   later capture demonstrates upward progress.
8. On disconnect both peers discard connection-local sequences and write plans.

Write-with-response supplies packet backpressure. Frame status supplies display
backpressure. Neither peer replays a queue of intermediate snapshots after a
reconnect.
