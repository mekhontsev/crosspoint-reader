# X4 Terminal User Guide

## Read this first

X4 Terminal is an unofficial experimental CrossPoint fork for the **Xteink X4
Pro only**. It mirrors a slow-moving, normalized plain-text view of a terminal
over authenticated Bluetooth Low Energy. It is not supported by the upstream
CrossPoint project or Xteink.

Do not install this build on an original Xteink X4, X3, Paper Mono, or another
ESP32 board. A binary for the wrong board can make the device unbootable.

The atomic frame build was exercised on one X4 Pro with CrossPoint already
installed. Hardware checks covered boot, pairing and reconnect, English and
Russian frames, automatic scrolling, manual refresh, history paging, and return
to the home menu.

The companion Android APK and `x4term` Termux helper live in the separate
[`x4-terminal-bridge`](https://github.com/mekhontsev/x4-terminal-bridge)
repository. Firmware and APK protocol versions must match.

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

Install and configure the APK and helper as described in the bridge repository.
Run the shell, SSH client, or Codex session in a named `tmux` pane, then start:

```sh
x4term serve
```

Choose the tmux pane in the APK, enable the bridge service, and open
**Terminal** on the reader. On the first
connection the reader displays a random six-digit passkey; enter it in Android's
system pairing dialog. The bond is reused later. Re-entering Terminal makes
Android reconnect and send one current screen rather than replaying the session.

## Reader controls

- Keyboard icon: open or close command entry. Pressing Enter sends one literal
  command line and then Enter to the companion. Empty input sends Enter alone.
- `-` and `+`: choose one of nine local IBM Plex Mono sizes, 8 through 24.
- `EN` / system-language key inside the Terminal keyboard: switch between
  English and the supported layout matching the reader language.
- Refresh icon: request Android's exact current screen. Hold the icon for a
  full ghost-cleaning e-ink refresh. A small corner marker on this icon means
  the reader is displaying history instead of following the latest screen.
- Confirm: request Android's exact current screen. Use this after Codex or
  another full-screen program has redrawn existing rows. This remains the
  physical-button fallback for devices that provide Confirm.
- Hold Confirm for about 700 ms: request the current screen and use a full
  ghost-cleaning e-ink refresh.
- Page Up: show the previous cached screen or request it from Android.
- Page Down: show the next cached screen or request it from Android.
- Hold Page Down for about 700 ms: jump to the newest screen.
- Back: leave Terminal and stop its BLE stack.

Cached paging and font changes are local. Paging contacts Android only when the
adjacent screen is not in the four-frame reader cache.

## Data, refresh, and power behavior

The helper captures only the visible `tmux` viewport. Android stores up to 64
accepted text screens; the reader stores four 6 KiB screens. Every transferred
screen is applied atomically only after its length and CRC-32 pass.

Android automatically sends rows that were appended or cleanly scrolled. If an
already published row changes in place, Android treats the screen as animated:
it keeps the newest snapshot but sends nothing until Confirm is pressed or a
later capture demonstrates real upward progress. There is no periodic
three-second animation replay. While the panel is busy, automatic updates are
paused, or the configured minimum interval has not elapsed, Android keeps only
the newest pending screen. Manual Refresh and history navigation do not wait for
that interval.

The Android foreground service holds a partial wake lock while started. For
reliable screen-off operation, set both X4 Terminal Bridge and Termux to
unrestricted battery use on the phone; the helper also uses Termux:API's wake
lock command when it is installed.

Leaving Terminal clears the reader's local cache and stops BLE. Re-entering the
screen causes the companion to reconnect and replay its latest snapshot.

## Security model

- The reader requires authenticated LE Secure Connections with a random
  six-digit passkey and rejects unauthenticated writes.
- Terminal text is data only. The reader never executes it.
- Only an explicitly entered command line or allow-listed action can travel
  from reader to companion.
- The Termux helper passes command text to `tmux` as a literal argv
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
- Plain text only; no colors, cursor, images, mouse, or full terminal emulation.
- Reader history is four screens and exists only while Terminal is open; Android
  retains up to 64 accepted screens.
- Animation-only redraws deliberately remain hidden until Confirm or subsequent
  upward progress.
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
