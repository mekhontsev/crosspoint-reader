# CrossPoint Reader — Plugins fork

Experimental [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)
fork with lazy SD-loaded native plugins for **Xteink X4 Pro**. The goal is a
small generic host, with plugin code and application protocols maintained and
updated independently.

> [!WARNING]
> This is an unofficial X4 Pro build, not an upstream CrossPoint release or a
> manufacturer-supported firmware. Do not install its binary on an X4, X3, or
> another board. On a USB-locked reader, keep a known-working X4 Pro rollback
> binary and use the existing SD firmware updater; do not change bootloader or
> partition settings.

## Start here

The single [installation, updates, controls, and recovery guide](https://github.com/mekhontsev/pagewire/blob/main/docs/user-guide.md)
covers the complete system. Read it before flashing. Release listings may contain
ordinary upstream builds; a version number alone does not identify a plugin-host
binary.

| Repository | Responsibility |
|---|---|
| This fork | Plugins menu entry, native loader, keyboard bridge, generic host services |
| [crosspoint-plugins](https://github.com/mekhontsev/crosspoint-plugins) | Independent manager/updater, Terminal, background Debugger, and plugin SDK |
| [PageWire](https://github.com/mekhontsev/pagewire) | Document protocol, Android companion, and Termux helper |

## What the fork adds

Selecting **Plugins** lazily loads `/plugins/manager.so`. The manager discovers
child modules from their embedded metadata; applications load on selection and
background services on request. No SD plugin runs during boot.

The current host contract is **ABI 5**:

- Existing activity/render/input services and a firmware-owned keyboard bridge.
- Authenticated raw BLE packets with backpressure and economical idle parameters.
- A second logical BLE channel for bounded, named background-service requests.
- Small opaque settings in internal NVS, without SD writes.
- Named plugin state providers, bounded read-only SD access, and recent-log snapshots.

The firmware does not know Terminal, Debugger commands, PageWire packets,
plugin-specific strings, fonts, layout, or settings formats. Other plugin
authors can use BLE and the remaining services for their own protocols. See the
[Plugin Development guide](https://github.com/mekhontsev/crosspoint-plugins/blob/main/PLUGIN_DEVELOPMENT.md)
and [plugin architecture](https://github.com/mekhontsev/crosspoint-plugins/blob/main/ARCHITECTURE.md).

See [Terminal running on X4 Pro](https://github.com/mekhontsev/crosspoint-plugins/blob/main/README.md#terminal-on-x4-pro)
for a photo of an independently loaded plugin using this host.

ABI compatibility is separate from the upstream firmware version, bundle version,
and PageWire wire version. There is no firmware build-ID match requirement.
A declared ABI mismatch is rejected before module execution.

## Upstream and safety boundary

The current source uses the upstream **1.6.0** version and incorporates
`upstream/develop`. This remains a fork: upstream reading features and fixes
are merged periodically, while plugin-specific code stays in the separate
plugin repository. Built-in upstream updates can replace the plugin-host fork,
so check the binary's origin before updating.

The plugin additions do not change bootloader, partition layout, early recovery,
or normal book-reading logic. Plugins are native code, not a security sandbox:
a bug can crash and restart the reader. Since plugins are not loaded at boot,
removing the faulty `.so` normally prevents another crash on selection.
This is not a guarantee against malicious native code or unsafe firmware flashing.

BLE runs only while an owning plugin activity is open. There are no periodic
application keepalives or unsolicited diagnostic streams. Radio-off reading
remains the power baseline; equal battery consumption has not been established.
See [idle-power behavior and validation](https://github.com/mekhontsev/pagewire/blob/main/docs/power-testing.md).

## Build and test

Use the [upstream development prerequisites](docs/contributing/getting-started.md),
then build this fork's explicit X4 Pro environment:

```sh
git clone --recurse-submodules https://github.com/mekhontsev/crosspoint-reader.git
cd crosspoint-reader
pio run -e x4pro
```

Output: `.pio/build/x4pro/firmware.bin`. This builds the application image only;
installation follows the shared guide. Plugins build independently in their
own repository.

Host-only regression checks:

```sh
cmake -S test -B build/tests
cmake --build build/tests --target PluginSettingsTest PluginServicesTest ConnectionModeRetryTest
ctest --test-dir build/tests -R 'PluginSettings|PluginServices|ConnectionMode' --output-on-failure
```

[Upstream reading controls](USER_GUIDE.md) and
[contributor documentation](docs/contributing/README.md) remain available for
ordinary CrossPoint features; they are not the installation guide for this fork.

## Development and license

The fork-specific work is **AI vibe-coded**, under maintainer direction,
review, builds, and hardware testing. This statement does not describe the
independent upstream CrossPoint project.

MIT licensed; see [LICENSE](LICENSE). Upstream authorship and bundled component
licenses are retained.
