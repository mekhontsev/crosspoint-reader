# REJECTED — X4 Terminal v0.1.0

> [!CAUTION]
> Do not publish this build. Hardware testing found an exit-time system reset
> after a completed BLE session. The corrected candidate is v0.1.1 or later.

Experimental prerelease for **Xteink X4 Pro only**. This is an unofficial fork
of CrossPoint Reader and is not supported by the upstream CrossPoint project or
Xteink.

The firmware identifies itself as
`1.5.0-x4pro-x4terminal-v0.1.0`, so it can be distinguished from an ordinary
CrossPoint X4 Pro build in diagnostics.

## What it adds

- `Terminal` home-menu activity, compiled only in the opt-in
  `x4pro-ble-terminal` build profile.
- Authenticated encrypted BLE service with passkey pairing and bond reuse.
- Ordered reset/append UTF-8 stream with write acknowledgements and replay.
- 32 KiB local transcript cache with Page Up/Page Down navigation.
- Hold Page Down to jump to the newest text.
- IBM Plex Mono sizes 8-24 with local `-` and `+` controls.
- Terminal-only English/system-language keyboard switch and literal command
  entry.
- BLE packet reception at acknowledged link speed, independently throttled
  e-ink rendering, and lower-power idle connection parameters.
- Windows Python smoke client and complete Android/Termux implementation
  contract.

## Hardware validation completed

- Boots on an Xteink X4 Pro with an existing CrossPoint installation.
- Enters and exits Terminal without a system crash after multi-page transfer.
- Authenticated pairing and bond reuse.
- English and Russian text.
- Multiple pages sent as ordinary short GATT writes without artificial packet
  delays.
- Periodic e-ink updates during a continuous transfer.
- Local paging and long-press jump to tail.
- Reader keyboard command observed by the desktop client.

## Not yet completed

- Android APK and Termux helper implementation.
- Hardware validation of the X4 Pro early-boot SD recovery route.
- Testing across multiple X4 Pro hardware revisions and Android vendors.

## Assets

- `x4-terminal-x4pro-v0.1.0.bin`
- `SHA256SUMS.txt`

Verify the checksum before flashing. Do not rename or use a binary intended for
another CrossPoint board.

## Installation and rollback

Read the full [X4 Terminal user guide](../x4-terminal-user-guide.md) before
installing. In summary:

1. Start from a working CrossPoint installation on an Xteink X4 Pro.
2. Keep an official upstream X4 Pro rollback binary on the SD card.
3. Install through **Settings > System > SD Card Firmware Update**.
4. Do not use Xteink Unlocker to install this experimental fork on a locked
   device.
5. Keep stable power and do not interrupt the update.

The built-in CrossPoint update checker still follows upstream releases.
Installing an upstream update removes this feature and returns to ordinary
CrossPoint.

## Source and protocol

- Branch: `feature/ble-terminal`
- Protocol: [CrossPoint Link Protocol](https://github.com/mekhontsev/crosspoint-link/blob/main/docs/crosspoint-link-protocol.md)
- Client architecture: [CrossPoint Link Architecture](https://github.com/mekhontsev/crosspoint-link/blob/main/docs/architecture.md)
- License: MIT; upstream copyright and license are retained.
