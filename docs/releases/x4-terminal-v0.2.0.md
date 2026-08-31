# X4 Terminal v0.2.0 — atomic screen frames

Xteink X4 Pro-only terminal profile. It mirrors one plain-text `tmux` viewport
without replaying the session or forwarding Codex animation frames.

The firmware identifies itself as `1.5.0-x4pro-x4terminal-v0.2.0`.

## Protocol

- Replace the mutable transcript stream with protocol-v4 atomic frames:
  `FRAME_BEGIN`, UTF-8 data chunks, and `FRAME_COMMIT`.
- Validate the complete frame length and CRC-32 before changing the reader cache.
- Retain the atomic-frame GATT UUID set, separate from incompatible v2 clients.
- Report reader rows and columns so normal shell history can fill blank rows
  without mixing scrollback into alternate-screen programs.
- Let the reader request Current, Previous, or Next and report Ready only after
  the requested e-ink update completes.
- Keep only one pending Android frame; reconnect transfers one current screen.

## Animation and navigation

- Android automatically publishes append/scroll progress but freezes when an
  already displayed row is replaced.
- Confirm force-publishes the newest Android snapshot. Holding Confirm also uses
  a full ghost-cleaning panel refresh.
- Page Down at the newest frame is an additional physical refresh action.
- Four fixed reader slots provide immediate nearby navigation; Android retains
  up to 64 accepted screens for cache misses.
- New output does not move or refresh the page while older history is displayed.

## Memory

Four committed 6 KiB slots plus one staging slot use about 30 KiB of fixed
activity-owned memory, slightly less than the former 32 KiB transcript buffer.
Packet processing remains allocation-free.

## SD plugins

- Firmware contains only the **Plugins** entry, validated ELF loader, and BLE
  host services.
- `manager.so`, `terminal.so`, Terminal sources, and generated IBM Plex Mono
  fonts live and build in the separate `crosspoint-plugins` repository.
- Modules require plugin ABI 1 and a valid ELF length and SHA-256 trailer.

## Verification

- JVM tests cover frame encoding, CRC metadata, request/status parsing,
  animation suppression, scroll recovery, latest-wins history, and UTF-8 chunks.
- Termux tests cover viewport-only capture, normalization, authenticated IPC,
  command deduplication, and literal command submission.
- Firmware host tests cover atomic commit, CRC and length rejection, sequence
  recovery, duplicate packets, UTF-8, and reader control encoding.
- The exact `x4pro-ble-terminal` firmware profile and Android APK must build
  successfully before publishing artifacts.
- Build firmware with `./bin/build-x4-terminal`; the wrapper rejects an
  ordinary X4 Pro release image that does not contain the plugin loader,
  transport host, and version markers. Build SD modules independently with
  `crosspoint-plugins/bin/build`.
- A physical X4 Pro hardware pass covered boot, BLE reconnect, English and
  Russian frames, automatic scrolling, manual refresh, and history paging.
