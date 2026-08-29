# DRAFT — X4 Terminal v0.1.1

> [!CAUTION]
> Do not publish this release until its BLE-session exit path has passed
> hardware testing and the notes retain the unverified-recovery warning.

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

## Required hardware check

1. Boot and confirm the v0.1.1 version string.
2. Enter Terminal and receive text over an existing authenticated bond.
3. Exit immediately after transfer and again after the client disconnects.
4. Repeat enter, reconnect, receive, and exit at least three times.
5. Confirm every exit returns Home without a crash report.

Early-boot X4 Pro SD recovery remains unverified on hardware. The Android APK
and Termux helper are not included yet.
