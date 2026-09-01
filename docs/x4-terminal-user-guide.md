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

The SD-loaded plugin bundle lives in
[`mekhontsev/crosspoint-plugins`](https://github.com/mekhontsev/crosspoint-plugins).
The CrossPoint Link Android client, `x4term` Termux adapter, protocol
specification, and test vectors live in the separate
[`crosspoint-link`](https://github.com/mekhontsev/crosspoint-link) repository.
Firmware, plugins, and client must support the same plugin/wire ABIs.

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

1. Download the X4 Terminal X4 Pro `.bin`, `SHA256SUMS.txt`, and the
   `crosspoint-plugins.zip` bundle from their GitHub releases.
2. Verify the checksum on a computer.
3. Copy both the X4 Terminal binary and an official X4 Pro rollback binary to
   the root of the reader's SD card. Give them distinct descriptive names.
4. On the reader, open **Settings > System > SD Card Firmware Update**.
5. Select the X4 Terminal binary, verify the filename again, and confirm.
6. Do not press Reset, remove the SD card, disconnect power, or operate the
   reader until the update completes and it restarts.
7. Extract the plugin ZIP into the root of the SD card. It must create
   `/plugins/manager.so` and `/plugins/terminal.so`.
8. After boot, confirm that **Plugins** appears in the home menu and contains
   **Terminal**.

The updater validates the ESP image, target chip/board metadata, image checksum,
size, and SHA-256 trailer before changing the active OTA image.

## Updating child plugins

After the ABI3 firmware and manager are installed once, later child plugins can
be installed without flashing the reader. Select `crosspoint-plugins.zip` with
**Choose ZIP** in CrossPoint Link, then open **Plugins > Install via Bluetooth**
on the reader. The client validates `plugins/bundle.json`; the reader streams each
eligible child directly to `/plugins`, verifies it, and adds it to the dynamic
list. `manager.so` is intentionally excluded and must still be copied over Wi-Fi
or from the SD card. An interrupted child transfer is retried from the beginning.

## Connecting

Install and configure the APK and helper as described in the CrossPoint Link
repository.
Run the shell, SSH client, or Codex session in a named `tmux` pane, then start:

```sh
x4term serve
```

Choose the tmux pane in the APK, start CrossPoint Link, and open
**Plugins > Terminal** on the reader. On the first
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
- Page Up: show the previous tmux scrollback page. The first use lazily loads a
  bounded recent page set through Android.
- Page Down: show the next scrollback page. Past the newest history page it
  returns to Android's current snapshot.
- Hold Page Down for about 700 ms: jump to the newest screen.
- Back: leave Terminal, stop its BLE stack, and return directly to the main
  menu with **Plugins** selected.

Font changes are local. Paging contacts CrossPoint Link only when the adjacent
page is not in the four-frame reader cache. A jump to the newest screen
transfers only that screen; it neither preloads nor forcibly clears the other
three slots.

## Data, refresh, and power behavior

The reader reports its text rows and columns to Android. For an ordinary shell
pane, the helper may include only enough recent tmux scrollback to fill blank
rows at the bottom of the live reader view. Page Up lazily captures and splits
bounded recent tmux scrollback using the reader's rows and columns. Android
stores up to 64 of these text pages; the reader stores four 6 KiB pages.
Alternate-screen programs are captured strictly from their current viewport and
do not expose unrelated shell scrollback. Every transferred page is applied
atomically only after its length and CRC-32 pass.

Android automatically sends rows that were appended or cleanly scrolled. If an
already published row changes in place, Android treats the screen as animated:
it keeps the newest snapshot but sends nothing until Confirm is pressed or a
later capture demonstrates real upward progress. There is no periodic
three-second animation replay. While the panel is busy, automatic updates are
paused, or the configured minimum interval has not elapsed, Android keeps only
the newest pending screen. Manual Refresh and history navigation do not wait for
that interval.

The Android foreground service holds a partial wake lock while started. For
reliable screen-off operation, set both CrossPoint Link and Termux to
unrestricted battery use on the phone; the helper also uses Termux:API's wake
lock command when it is installed.

Leaving Terminal clears the reader's local cache, unloads `terminal.so`, and
stops BLE. Leaving Plugins also unloads `manager.so`. Re-entering causes the
companion to reconnect and replay its latest snapshot.

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
- The reader caches four scrollback pages at a time and lazily requests missing
  adjacent pages from CrossPoint Link, which retains up to 64 pages from the
  latest bounded capture. History older than that capture is unavailable.
- Animation-only redraws deliberately remain hidden until Confirm or subsequent
  upward progress.
- The loader accepts a module when plugin ABI 3 matches and its integrity check
  passes. An actually incompatible plugin can crash the activity and restart
  the reader. Removing its `.so` from the SD card disables it.
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
