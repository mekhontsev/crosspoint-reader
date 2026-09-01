# X4 Terminal BLE Protocol

This experimental X4 Pro-only protocol displays atomic plain-text snapshots of
one `tmux` pane. The Terminal mode is not a terminal emulator, remote shell,
pixel-frame protocol, or transport for raw ANSI output. Protocol v4 also
reserves an authenticated package-transfer mode used only by the Plugins
manager; that mode never runs while Terminal is active.

## Frame model

The reader reports its text geometry after connection and after a font change.
For a normal shell pane, the Termux helper may prepend only enough recent tmux
history to fill otherwise blank reader rows. Alternate-screen programs are
always captured from their current viewport, so unrelated shell history cannot
appear inside a TUI. Android keeps the newest normalized snapshot, classifies
its changes, and sends a complete text
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
keeps up to 64 pages of the current tmux scrollback. These are text pages, not a
timeline of previously published screen snapshots. Side-key navigation uses an
adjacent page in the reader cache without BLE; if that page is absent, the reader
asks Android for it relative to the displayed frame ID.

The page set is created lazily on the first Previous request. The helper captures
bounded recent scrollback, wraps it to the reader's reported columns, and splits
it into pages aligned from the newest row. Android sends only the requested page
over BLE. Automatic live frames are paused while history is displayed. Current,
or Next past the newest captured page, returns to the newest live snapshot and
transfers only that page: it neither preloads nor forcibly erases the reader's
other three slots. Each later automatic live frame replaces that same latest
frame ID, so Page Up cannot accidentally navigate old animation or output
snapshots.

Leaving Terminal stops BLE and frees the activity and cache. Re-entering Terminal
starts a new connection. Its first frame carries `RESET_CACHE`, so stale frame IDs
from an earlier Android process cannot be mixed with the new history.
Selecting another tmux pane also sends its first frame with `RESET_CACHE` while
leaving the BLE connection intact.

## Reader controls

- Confirm: request and display Android's current snapshot.
- Hold Confirm for about 700 ms: request the current snapshot and use a full
  ghost-cleaning e-ink refresh when it arrives.
- Page Up / Page Down: previous or next tmux scrollback page.
- Page Down past the newest history page: return to Current.
- Hold Page Down for about 700 ms: return to the current live frame.
- Keyboard icon: send one literal command followed by Enter. Empty input sends
  Enter only.
- Refresh icon: touch equivalent of Confirm. A long touch requests Current with
  a full refresh; its corner marker indicates history mode.
- `-` / `+`: change the local terminal font.
- Back: leave Terminal, stop the BLE stack, and return to the main menu.

## Safety and memory

- The generic loader and BLE host are compiled in the X4 Pro firmware. The
  Terminal activity and fonts live in `terminal.so` on SD and start BLE only
  after the user enters **Plugins > Terminal**.
- GATT traffic requires authenticated LE Secure Connections with a random
  six-digit passkey. Unauthenticated writes are rejected.
- Incoming lengths, sequences, flags, UTF-8, control characters, frame IDs, and
  CRC-32 values are validated before a frame enters the cache.
- Reader text is data only. Firmware never executes it.
- Reader commands are allow-listed packet types and travel only over the
  authenticated indication characteristic.
- Terminal does not write SD data. The Plugins installer may overwrite one
  validated child module under `/plugins`; it cannot target `manager.so`, OTA
  data, partition tables, boot state, or recovery state.

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

Protocol v4 retains the atomic-frame UUIDs introduced by v3; both are separate
from the incompatible stream-based v2 protocol. The BLE bond is independent of
service UUIDs and can normally be reused after both components are upgraded.

## Packet envelope

All integers are little-endian. One packet occupies one GATT characteristic
value and is at most 244 bytes, matching ATT MTU 247. With a smaller MTU Android
reduces `FRAME_DATA` payloads. It never splits a UTF-8 code point between data
packets.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 | Magic `XT` |
| 2 | 1 | Protocol version `4` |
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

### `PLUGIN_UPDATE_BEGIN` (`10`)

Used only while **Plugins → Install via Bluetooth** is open.

| Payload offset | Size | Field |
|---:|---:|---|
| 0 | 1 | Module-name length N, 1-32 |
| 1 | N | Lowercase module name without `.so` |
| 1+N | 4 | Complete `.so` byte length |
| 5+N | 32 | SHA-256 of the complete `.so` |

The reader rejects invalid names, `manager`, a loaded child module, oversized
files, and overlapping transfers. Success is reported with
`PLUGIN_UPDATE_STATUS(READY)` before any data is sent.

### `PLUGIN_UPDATE_DATA` (`11`)

Four-byte file offset followed by one non-empty binary chunk. Offsets must be
strictly sequential. Write-with-response and the reader's bounded GATT queue
provide backpressure.

### `PLUGIN_UPDATE_END` (`12`)

Four-byte complete file length. The reader requires the declared byte count and
full-file SHA-256 to match, then validates the embedded plugin trailer, ABI,
ELF structure, ELF digest, and descriptor export before reporting Complete.

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

Current force-publishes the newest Android snapshot and leaves history mode.
The first Previous request lazily builds the bounded page set; later Previous
and Next requests select its neighbors. At the oldest known page, Android leaves
the displayed page in place without another transfer. Next past the newest page,
or an unknown anchor, falls back to the newest live snapshot.

### `FRAME_STATUS` (`7`)

| Payload offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Nonzero frame ID |
| 4 | 1 | Ready (`0`) or retry (`1`) |

Ready is sent after presentation finishes or after a non-presented history cache
insert. Retry asks Android to resend the same complete frame from a new begin.

### `VIEWPORT` (`8`)

| Payload offset | Size | Field |
|---:|---:|---|
| 0 | 2 | Nonzero terminal columns |
| 2 | 2 | Nonzero terminal rows |

The reader sends this after connecting and whenever its local font size changes.
Android forwards it to `x4term`. Rows and columns determine lazy scrollback page
boundaries; they never resize the user's tmux pane.

### `PLUGIN_UPDATE_HELLO` (`9`)

Sent by the Plugins manager after connection. Its six-byte payload contains the
four-byte plugin ABI followed by the maximum two-byte update data chunk. A
client must reject a package whose ABI differs.

### `PLUGIN_UPDATE_STATUS` (`13`)

One status byte followed by a four-byte value: Ready (`1`, value zero), Complete
(`2`, installed byte count), or Error (`3`, implementation-defined error code).
On disconnect an incomplete direct write remains an invalid module and a later
connection restarts that module from offset zero.

## Text validation

Display text permits valid Unicode UTF-8, newline, and horizontal tab. NUL,
carriage return, ESC, DEL, other C0/C1 controls, overlong encodings, surrogates,
and invalid Unicode are rejected. The helper normalizes line endings, removes
forbidden controls and trailing screen padding, and limits every transferred
page to 6 KiB.

## Connection lifecycle

1. Android scans by the atomic-frame service UUID and establishes authenticated LE Secure
   Connections.
2. Android discovers GATT, requests MTU 247, and enables indications.
3. The reader sends Viewport and requests Current.
4. Android forwards Viewport to the helper.
5. Android sends one atomic `PRESENT | LATEST | RESET_CACHE` frame.
6. The reader commits, displays, and sends Ready.
7. Later safe forward snapshots are sent only while the reader is ready.
8. An animation-only redraw stays on Android until Current is requested or a
   later capture demonstrates upward progress.
9. On the first Previous request, the helper supplies a bounded page set and
   Android transfers its penultimate page. Further cache misses transfer one
   adjacent page at a time.
10. On disconnect both peers discard connection-local sequences and write plans.

Write-with-response supplies packet backpressure. Frame status supplies display
backpressure. Neither peer replays a queue of intermediate snapshots after a
reconnect.
