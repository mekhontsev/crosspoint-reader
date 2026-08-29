# X4 Terminal User Guide

## Read this first

X4 Terminal is an unofficial experimental CrossPoint fork for the **Xteink X4
Pro only**. It mirrors a slow-moving, normalized plain-text view of a terminal
over authenticated Bluetooth Low Energy. It is not supported by the upstream
CrossPoint project or Xteink.

Do not install this build on an original Xteink X4, X3, Paper Mono, or another
ESP32 board. A binary for the wrong board can make the device unbootable.

The firmware has been built and exercised on one X4 Pro with CrossPoint already
installed. The tested path covers boot, Terminal entry, authenticated pairing,
English and Russian text, multiple pages of packetized data, local paging,
font controls, reader-to-client command entry, disconnect, and return to the
home menu without a crash.

The Android APK and Termux helper are not released yet. Until they exist, the
included Windows Python client is only a development smoke test; it does not
mirror a real Termux session or execute commands received from the reader.

## Safety prerequisites

Before installing:

1. Confirm that the device is an **Xteink X4 Pro** and currently boots a working
   CrossPoint installation.
2. Confirm that **Settings > System > SD Card Firmware Update** opens and can
   see `.bin` files on the SD card.
3. Charge the reader fully and keep external power stable during the update.
4. Download the official CrossPoint X4 Pro firmware from the upstream
   [CrossPoint releases](https://github.com/crosspoint-reader/crosspoint-reader/releases)
   and keep that `.bin` on the SD card as a rollback image.
5. Verify the SHA-256 checksum of every downloaded binary before selecting it
   in the firmware updater.

Do **not** use Xteink Unlocker to install this experimental fork on a USB-locked
device. The safe path tested for this work starts from an already working
CrossPoint installation and uses its SD-card updater.

CrossPoint contains an early SD recovery route for X4 Pro, but that route has
not yet been hardware-tested in this project. A locked device that cannot reach
normal boot may have no verified USB recovery path. Treat every installation as
experimental even though X4 Terminal does not modify the bootloader, partition
table, early boot path, or SD recovery implementation.

## Installation

1. Download the X4 Terminal X4 Pro `.bin` and `SHA256SUMS.txt` from this fork's
   GitHub pre-release.
2. Verify the checksum on a computer.
3. Copy both the X4 Terminal binary and an official X4 Pro rollback binary to
   the root of the reader's SD card. Give them distinct descriptive names.
4. On the reader, open **Settings > System > SD Card Firmware Update**.
5. Select the X4 Terminal binary, verify the filename again, and confirm.
6. Do not press Reset, remove the SD card, disconnect power, or operate the
   reader until the update completes and it restarts.
7. After boot, confirm that **Terminal** appears in the home menu.

The updater validates the ESP image, target chip/board metadata, image checksum,
size, and SHA-256 trailer before changing the active OTA image.

## Connecting

### Current Windows smoke test

Install Python and Bleak, then run from a checkout of this branch:

```powershell
py -3.10 -m pip install "bleak>=1,<4"
py -3.10 scripts/ble_terminal_client.py --self-test
py -3.10 scripts/ble_terminal_client.py
```

Open **Terminal** on the reader before starting the final command. The reader
shows `Waiting...` while advertising. On the first connection it displays a
random six-digit passkey; enter that code in the Windows pairing prompt. A valid
bond is reused on later connections.

The smoke client sends fixed demonstration text and prints reader commands. For
safety, it never executes those commands on the computer.

### Future Android connection

The companion APK will connect automatically while its foreground service is
enabled. A Termux helper will capture and normalize a selected `tmux` pane. When
the reader leaves Terminal, BLE stops; when Terminal is opened again, Android
reconnects and replays the newest snapshot without requiring a new passkey while
the bond remains valid.

The Android implementation contract is documented separately in
[`x4-terminal-android-handoff.md`](x4-terminal-android-handoff.md).

## Reader controls

- Keyboard icon: open or close command entry. Pressing Enter sends one literal
  command line and then Enter to the companion. Empty input sends Enter alone.
- `-` and `+`: choose one of nine local IBM Plex Mono sizes, 8 through 24.
- `EN` / system-language key inside the Terminal keyboard: switch between
  English and the supported layout matching the reader language.
- Page Up: move one local screen toward older text.
- Page Down: move one local screen toward newer text.
- Hold Page Down for about 700 ms: jump directly to the live tail.
- Back: leave Terminal and stop its BLE stack.

Paging and font changes are local. They do not contact or wake the phone.

## Data, refresh, and power behavior

The reader keeps the newest 32,768 bytes of valid UTF-8 while Terminal is open.
When the cache fills, it drops old complete lines first. If the entire cache is
one long line, it drops only the required prefix at a UTF-8 boundary. The full
terminal scrollback remains owned by `tmux`, not the reader.

BLE packets are accepted as quickly as acknowledged writes allow. E-ink drawing
is independent: during a continuous stream it refreshes no more than once every
three seconds, and after a short burst it coalesces the final state. With no new
data there are no display refreshes. A quiet connection returns to slower BLE
connection parameters.

Leaving Terminal clears the reader's local cache and stops BLE. Re-entering the
screen causes the companion to reconnect and replay its latest snapshot.

## Security model

- The reader requires authenticated LE Secure Connections with a random
  six-digit passkey and rejects unauthenticated writes.
- Terminal text is data only. The reader never executes it.
- Only an explicitly entered command line or allow-listed action can travel
  from reader to companion.
- The future Termux helper must pass command text to `tmux` as a literal argv
  value, never through shell interpolation.

BLE encryption protects the radio link after pairing. It does not protect a
phone that is already compromised, an unlocked Termux session, screenshots, or
terminal history stored by `tmux`.

## Rollback

If the reader still reaches its menus:

1. Leave Terminal.
2. Open **Settings > System > SD Card Firmware Update**.
3. Select the official X4 Pro rollback binary already stored on the SD card.
4. Confirm and wait for the restart.

The built-in **Check for updates** action follows the official upstream
CrossPoint repository, not this fork. Accepting an upstream update therefore
removes X4 Terminal and returns the reader to ordinary CrossPoint behavior.

If the device cannot reach its menus, stop experimenting rather than trying
unverified USB button sequences. The X4 Pro source contains an early SD recovery
route, but public recovery instructions should be added only after that exact
path has been tested on hardware.

## Known limitations

- X4 Pro only.
- Experimental firmware tested on one physical reader.
- Android companion not released yet.
- Plain text only; no colors, cursor, images, mouse, or full terminal emulation.
- Local reader history is limited to 32 KiB and exists only while Terminal is
  open.
- Side-key paging is local; reserved BLE page action IDs are not emitted.
- Current firmware UI emits only a literal command plus Enter, or Enter alone.
- OTA checks intentionally point to upstream CrossPoint rather than this fork.

## Reporting a problem

Include:

- exact release tag and binary SHA-256;
- device model shown at boot;
- whether the bootloader/USB flashing path is locked;
- the last action before the problem;
- the complete CrossPoint crash report and last logs, if one was produced;
- whether reboot, menu navigation, and SD firmware update still work.

Do not describe a recoverable activity crash as a brick. A brick means the
device no longer reaches a usable boot or verified recovery/update path.
