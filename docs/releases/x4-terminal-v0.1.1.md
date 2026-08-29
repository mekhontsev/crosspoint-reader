# X4 Terminal v0.1.1 — hardware-tested candidate

> [!CAUTION]
> Early-boot X4 Pro SD recovery remains unverified. Keep a known-good firmware
> image available and do not publish a binary release without that warning.

Corrective hardware-test candidate for **Xteink X4 Pro only**. It supersedes
the rejected v0.1.0 candidate, which reset while returning Home after a BLE
session.

The firmware identifies itself as
`1.5.0-x4pro-x4terminal-v0.1.1`.

## Exit-path correction

- Stop NimBLE before allocating the Home activity and before ActivityManager
  takes its render lock.
- Follow NimBLE's single host-stop procedure instead of racing separate GAP
  advertising/disconnect requests against it.
- Keep the transport object alive independently of the Terminal activity while
  the NimBLE FreeRTOS task completes its asynchronous epilogue.
- Allocate the Home activity with the project's no-throw helper so memory
  pressure is logged rather than converted into an abort.
- Add teardown checkpoints to crash logs.

## Hardware check

Passed on an Xteink X4 Pro on 2026-08-29:

1. Booted with the v0.1.1 version string.
2. Received text over an existing authenticated Windows bond.
3. Returned Home after the client disconnected.
4. Returned Home while the BLE connection was still active.
5. Re-entered Terminal, reconnected, received text, and returned Home in
   repeated cycles without a crash report.

Early-boot X4 Pro SD recovery remains unverified on hardware. The Android APK
and Termux helper are not included yet.
