# X4 Terminal Android Handoff

This document is the implementation handoff for the two Android-side components
that connect a Termux `tmux` pane to the X4 Terminal firmware. The BLE wire
format remains normative in [`ble-terminal-protocol.md`](ble-terminal-protocol.md).
If this document and the wire-format document disagree, the wire-format document
wins for bytes sent over BLE.

## Deliverables and scope

Build two separately installable/runnable components:

1. `x4term`, a small command-line helper installed inside Termux.
2. An Android APK containing a minimal status/configuration activity and a
   foreground connection service.

Use Python 3 and its standard library for the first Termux helper, plus the
external `tmux` executable. Use Java and the platform Android Bluetooth APIs
for the APK. Keep the protocol layers free of UI dependencies so either choice
can be replaced later without changing the wire or IPC contracts.

The result mirrors a named `tmux` pane as normalized plain text. It is not a
terminal emulator, screen-image transport, file transfer tool, or arbitrary
remote shell. The reader may send one explicitly entered command line or the
empty Enter action back to the selected pane.

The first useful version does not need Play Store packaging, multi-reader
support, a full settings UI, ANSI rendering, `mc`, `nvim`, or automatic creation
of the user's shell/Codex session.

## Component boundaries

| State or responsibility | Owner |
|---|---|
| `tmux` session, pane contents, and full scrollback | `tmux` |
| Capturing, normalizing, animation filtering, and limiting the current snapshot | Termux helper |
| Latest published snapshot received from Termux | APK service |
| BLE scan, bond, GATT connection, MTU, packet sequence, retries, and replay | APK service |
| Newest 32 KiB local paging cache while Terminal is open | X4 reader |
| E-ink refresh throttling | X4 reader |

The Termux helper sends complete *published snapshots* to the APK. The APK, not
the helper, compares a snapshot with the confirmed source and reader shadows and
chooses append, tail truncate plus append, or reset. This keeps every BLE state
transition in one process and makes reconnect replay deterministic.

## Runtime data flow

1. The user runs their shell, Codex, or SSH process in a named `tmux` pane.
2. `x4term serve --target <session:window.pane>` watches that pane.
3. The APK service connects to `x4term` over authenticated loopback IPC and
   receives the newest complete snapshot immediately.
4. While the X4 Terminal screen is closed, the APK retains only that newest
   snapshot and performs no GATT work.
5. Opening Terminal makes the reader advertise the service UUID. The APK finds
   it, securely connects, enables indications, sends `STREAM_RESET`, and replays
   the newest snapshot.
6. Later append-only snapshots send just their new suffix. A redraw truncates
   and replaces only the changed tail while preserving older reader pages. A
   reset is used only when the change predates the retained reader window.
7. Closing Terminal disconnects BLE. The APK returns to low-power filtered
   scanning; it does not stop the `tmux` session or discard the newest snapshot.

Only one BLE writer coroutine/state machine may exist. Snapshot updates arriving
during a transfer replace the pending snapshot with the newest one; they must
never start a second concurrent GATT write sequence.

## Termux command-line helper

### Commands

The minimum CLI is:

```text
x4term init
x4term serve --target <tmux-target>
x4term status
```

`init` creates a random 256-bit IPC token and a user-only configuration file,
for example `~/.config/x4-terminal/config.json`. It prints the loopback port and
token once so the user can enter them in the APK. `serve` must fail clearly if
the target pane does not exist. It must not create, resize, attach to, kill, or
otherwise reconfigure the user's `tmux` session.

The default target may be stored in the configuration file. Prefer a stable
pane ID such as `%12` after resolving a human-friendly target; tmux pane IDs are
not reused while that tmux server remains alive.

### Capture contract

Use `pipe-pane` only as a change signal; never forward its raw bytes. A change
signal schedules a debounced `capture-pane`. Polling at a modest interval is an
acceptable fallback when installing a pipe would replace an existing user pipe;
do not silently overwrite an existing `pipe-pane` configuration.

Capture recent history plus the visible pane as plain text. A suitable starting
point is `tmux capture-pane -p -J -S -2000 -t <target>`, followed by removal of
right-padding introduced by `-J`. Make the history-line limit configurable.
Never add `-e`, because color and attribute escape sequences are not display
data for the reader.

Normalize every capture before comparing it:

- decode as UTF-8 with replacement for undecodable input;
- normalize CRLF and bare CR to LF;
- remove NUL, ESC, DEL, C0/C1 controls other than LF and horizontal tab;
- remove trailing spaces from each row and trailing empty screen rows;
- preserve internal blank rows and tabs;
- keep at most 32,768 UTF-8 bytes, dropping old complete lines first and never
  splitting a UTF-8 code point.

### Change and animation filtering

Maintain `candidate` and `published` snapshots:

- coalesce pane-output signals for roughly 150-250 ms before capture;
- identical normalized captures do nothing;
- treat the newest twenty rows as volatile while they change;
- publish newly stable rows above that volatile tail without waiting for an
  animation to finish;
- publish the final volatile rows after the candidate has remained unchanged for
  1.5 seconds;
- send every published snapshot to the APK immediately, with no artificial
  three-second BLE delay.

This suppresses spinner/repaint churn while allowing real completed lines to
flow. The reader independently limits panel updates to once every three seconds;
one snapshot transfer is packetized as quickly as acknowledged BLE writes allow.

### Safe reader input

Never interpolate reader text into a shell command. Invoke `tmux` with an argv
API and a literal-text mode:

1. For `command`, send the text literally to the configured target.
2. Only after that call succeeds, send the `Enter` key in a separate call.
3. For `submit-input`, send only `Enter`.

The current reader firmware emits only `COMMAND` and `SUBMIT_INPUT`. Other
action IDs are reserved for future reader controls and should be validated and
logged, but not mapped to guessed keystrokes. In particular, do not assume that
"approve" means `y` for every program.

## APK architecture

Use a native Android foreground service as the single owner of both the BLE
state machine and the Termux IPC client. The activity only edits configuration,
requests permissions, starts/stops the service, and displays status. Blocking
socket, capture, or BLE work must not run on the Android main thread.

Suggested service states are:

```text
Stopped
WaitingForTermux
ScanningForReader
Bonding
Discovering
Subscribing
Replaying
Connected
Backoff
```

State transitions must be serialized. On a disconnect or GATT error, invalidate
the GATT object, outstanding write callback, connection sequence, and reader
shadow before scanning again.

The foreground notification should show at least: helper disconnected, scanning,
pairing, connected, and error. A user-visible Stop action must terminate scans,
GATT, IPC, and the service.

### Android permissions and service declaration

For Android 12 and newer, request `BLUETOOTH_SCAN` and `BLUETOOTH_CONNECT` at
runtime. The phone never advertises, so `BLUETOOTH_ADVERTISE` is unnecessary.
Declare legacy Bluetooth permissions with `maxSdkVersion="30"`. Because the app
does not infer location, use `neverForLocation` for scanning and limit legacy
location permission to API 30 where required.

Declare the foreground service as type `connectedDevice`. On Android 14 and
newer, also declare the type-specific foreground-service permission. Start the
service from a visible activity after the user enables it; do not assume that a
background or boot receiver may always start it.

Continuous automatic operation may require the user to exclude the APK and
Termux from device-specific battery restrictions. Do not request a wakelock by
default. BLE scanning must be filtered by the 128-bit service UUID and use a
low-power mode/backoff while the reader is absent.

### Optional Termux startup integration

The MVP may require the user to run `x4term serve` manually. A later opt-in
feature may start it through Termux `RunCommandService`. That integration must:

- request `com.termux.permission.RUN_COMMAND`;
- declare Termux package visibility when required by the target SDK;
- tell the user to set `allow-external-apps=true` in Termux;
- use the Termux constants/library rather than duplicating intent strings;
- run only the installed `x4term` executable with argv arguments;
- remain optional because this permission allows the APK to execute commands in
  the user's Termux environment.

Do not use one-shot Intent result data as the continuous terminal transport.
Loopback IPC remains the streaming channel.

## Authenticated loopback IPC

The helper listens only on `127.0.0.1`, default TCP port `47641` (configurable).
Other Android apps can reach loopback, so binding locally is not authentication.
Every connection must prove knowledge of the random 256-bit token from
`x4term init`. Reject authentication before accepting any snapshot or input.

Use UTF-8 JSON Lines: exactly one JSON object followed by LF. Newlines inside
snapshot text are JSON escapes, not framing bytes. Reject malformed messages,
unknown protocol versions, and encoded lines larger than 128 KiB.

APK to helper handshake:

```json
{"v":1,"type":"hello","token":"<base64url>","last_revision":17}
```

Helper replies with the latest snapshot even if the revision matches, so APK
process restarts need no special recovery:

```json
{"v":1,"type":"snapshot","revision":18,"text":"normalized UTF-8 text"}
```

Each later published change is another complete `snapshot` with a monotonically
increasing 64-bit revision. Revisions are local IPC metadata and are unrelated
to the 32-bit BLE packet sequence.

Reader input is forwarded APK to helper:

```json
{"v":1,"type":"command","id":"<uuid>","text":"literal command"}
{"v":1,"type":"action","id":"<uuid>","name":"submit-input"}
```

The helper responds:

```json
{"v":1,"type":"input-result","id":"<same uuid>","ok":true}
```

Cache a small bounded set of completed input IDs in the helper so an IPC retry
cannot type a command twice. Unknown message types or action names are errors,
not extension points to execute arbitrary commands. Optional `ping`/`pong`
messages may detect a half-open connection but must not wake BLE.

## BLE central state machine

Use the service and characteristic UUIDs from the wire-format document. The
device name is diagnostic only; discovery is filtered by service UUID.

For each connection, serialize these operations and wait for the corresponding
Android callback before starting the next:

1. Connect with LE transport.
2. Establish or reuse a bond. The reader displays a six-digit passkey and
   requires authenticated LE Secure Connections; let Android show its system
   PIN-entry UI.
3. Discover services and validate both characteristic properties.
4. Request ATT MTU 247. Continue with the negotiated MTU if Android grants less.
5. Enable local notifications and write `0x0002` to the CCC descriptor to enable
   indications; wait for descriptor-write success.
6. Start a new random 32-bit packet sequence.
7. Send `STREAM_RESET`, then replay the newest UTF-8-safe 4 KiB whole-line tail.

Every characteristic write uses write-with-response. Keep exactly one write in
flight and send the next packet immediately after its successful callback; add
no sleep between packets. The maximum append payload is:

```text
min(234, negotiatedAttMtu - 3 - 10)
```

Split only at UTF-8 code-point boundaries. Increment the sequence modulo
`2^32` after success. A retry uses the identical packet and sequence; duplicate
packets are deliberately idempotent on the reader. After bounded retries or a
write timeout, close GATT, reconnect, start a new sequence, reset, and replay.

While a transfer is running, keep only the newest pending IPC snapshot. After
the current write succeeds, compare that newest snapshot against the confirmed
source and reader shadows:

- equal: no write;
- source extends unchanged: append only the suffix;
- only the retained tail changed: send `STREAM_TRUNCATE`, then append its
  replacement;
- the change begins before the retained reader window: reset and replay the
  bounded 4 KiB tail.

Do not send the next snapshot's packets in the middle of an older write plan.
Reset, truncate, and append are ordinary sequenced packets; a confirmed plan
updates both shadows before the next snapshot is compared.

Validate reader indications with the same header, length, version, UTF-8, and
allow-list rules as the firmware. Android confirms GATT indications through its
Bluetooth stack. Convert each accepted `COMMAND` or `ACTION` into one IPC request
with a fresh UUID. Log and ignore the currently reserved action IDs.

## Reconnect and power behavior

- With the foreground service disabled: no IPC, scan, or BLE activity.
- Service enabled, helper absent: retry loopback connection with capped backoff.
- Helper present, reader absent: retain newest snapshot and perform filtered,
  low-power BLE discovery without a tight connection loop.
- Reader appears: connect and replay automatically.
- Reader disappears: close GATT once, discard connection-only state, and scan.
- Helper reconnects: request and replace the complete newest snapshot.
- Android Bluetooth turns off: stop GATT/scan and wait for the adapter to return.
- Bond/authentication failure: stop retrying rapidly and expose a user action to
  forget/re-pair the reader.

No Android-side timer controls the e-ink panel. The APK transports every
published snapshot at BLE speed; the reader owns its three-second redraw limit.

## Security rules

- BLE payloads are accepted only through the authenticated encrypted GATT
  characteristics enforced by the reader.
- IPC binds to loopback and requires the random token before any other command.
- Store the helper token in a mode-0600 Termux file and private APK storage; do
  not write it to shared storage or ordinary logs.
- Treat snapshots and reader input as untrusted length-bounded UTF-8.
- Use argv process APIs for every `tmux` invocation. Never use `shell=true`,
  `sh -c`, string interpolation, or `eval` with reader data.
- The APK must not offer a generic "execute shell command" IPC message.

## Suggested project layout

The Android work should live in its own repository, not in the firmware tree:

```text
x4-terminal-android/
  app/                         Kotlin APK
    ble/                       GATT protocol and connection state machine
    ipc/                       loopback client and message validation
    service/                   foreground service
    ui/                        status, permissions, configuration
  termux/
    x4term/                    CLI package
    tests/                     capture, normalization, filtering, IPC tests
  protocol-tests/
    vectors/                   shared packet and JSON test vectors
  README.md
```

Keep BLE packet encoding independent of Android framework classes so it can be
unit-tested on the JVM. Keep normalization/filtering independent of subprocess
and socket code so it can be tested directly in Termux/Python.

## Minimum acceptance tests

1. Packet vectors match `BleTerminalProtocol.h` and the desktop smoke client.
2. Russian and English UTF-8 cross small and large negotiated-MTU boundaries
   without splitting code points.
3. A 40 KiB capture keeps the newest 32 KiB and valid UTF-8.
4. Spinner-only changes never publish; completed lines do; the final prompt
   appears after 1.5 seconds of stability.
5. An append-only capture sends only its suffix; changed older text sends reset
   plus the latest snapshot.
6. A failed write retries the identical sequence; duplicate delivery is safe.
7. Exit/re-enter Terminal reconnects and replays without user action or a new
   passkey when the bond is still valid.
8. A command containing spaces, quotes, `$`, semicolons, and Unicode reaches
   `tmux` literally and cannot invoke a shell parser.
9. Retrying the same IPC input ID does not type it twice.
10. With Terminal closed and no pane changes, neither component loops or emits
    continuous traffic.

For a hardware smoke test, the reader firmware and Windows client already prove
the protocol path. Use `scripts/ble_terminal_client.py` as executable reference
code for packet layout, UTF-8 chunking, write ordering, and indication parsing.

## Upstream platform references

- Android Bluetooth permissions:
  <https://developer.android.com/develop/connectivity/bluetooth/bt-permissions>
- Android foreground-service type `connectedDevice`:
  <https://developer.android.com/develop/background-work/services/fgs/service-types#connected-device>
- Termux `RUN_COMMAND` intent:
  <https://github.com/termux/termux-app/wiki/RUN_COMMAND-Intent>
- tmux capture, pipe, pane IDs, and literal `send-keys`:
  <https://github.com/tmux/tmux/wiki/Advanced-Use>
