# B3 Configurator for macOS

A native Mac app for setting the **blafili B3 Bluetooth name and four-digit PIN** over USB. The download includes the app and its native helpers; no Python, compiler, Windows driver, or vendor tool installation is needed.

## Download

Open the [latest release](https://github.com/lnxgod/b3-configurator-macos/releases/latest) and choose the ZIP for your Mac:

| Mac | Download |
| --- | --- |
| Apple Silicon (M-series) | `B3-Configurator-macOS-arm64.zip` |
| Intel | `B3-Configurator-macOS-x86_64.zip` |

Requires **macOS 12 or later**. In Apple menu → About This Mac, look for an Apple M-series chip or Intel processor. The GitHub-generated “Source code” archives are for developers; download one of the named app ZIPs above to run the app.

## Use the app

1. Extract the ZIP to a local, non-synced folder, such as your local Applications folder.
2. Open **B3 Configurator.app** and connect exactly one blafili B3 with a USB data cable.
3. Choose **Check connection**, select the name and/or PIN changes, then choose **Apply selected changes**.
4. Keep USB connected until post-reboot verification finishes. The B3 restarts and audio pauses.

New names support **1–16 UTF-8 bytes**. PINs are four digits, including leading zeros. The displayed `1234` is a suggested value, not a reading of your current PIN. Nothing is written automatically at launch.

These builds are **ad hoc signed, not Developer ID signed or notarized**. macOS may block the first launch. If you trust this release, follow [Apple’s instructions for opening an app from an unidentified developer](https://support.apple.com/en-us/102445). No security setting is changed by the build or app.

See the [full usage and recovery guide](native-macos/README.md) for private backups, PIN restoration and limitations. A successful name change remains saved if a later PIN change fails. Backup files can contain private device settings and stay on your Mac.

## Build from source

Install Xcode or Apple Command Line Tools, then run from the repository root:

```sh
./native-macos/build.sh
```

The script builds for the host architecture, runs offline mocked tests, signs the app ad hoc, verifies the signature, packages a ZIP, and verifies an extracted copy. It creates the ZIP and its `.sha256` file in `dist/`. No USB operation is performed during the build or its tests.

To verify a downloaded ZIP, put its matching `.sha256` file beside it and run:

```sh
shasum -a 256 -c B3-Configurator-macOS-arm64.zip.sha256
```

Use the `x86_64` filename for an Intel download.

## Automated builds and releases

[GitHub Actions](https://github.com/lnxgod/b3-configurator-macos/actions/workflows/macos.yml) builds on both Apple Silicon and Intel runners for main-branch pushes, pull requests and manual runs. Pushing a version tag such as `v1.0.0` builds both architectures and publishes their verified ZIPs and checksums to a GitHub Release. The tag must match the app version in `native-macos/Info.plist`.

Cloud builds verify compilation, mocked workflows and packaging. They do not test USB hardware. Prior local macOS use verified name/PIN writes and post-reboot readback on the tested B3 layout; Intel hardware behavior and every Bluetooth peer’s PIN behavior are not established by those tests.

## Source layout

- `native-macos/`: AppKit/Swift app, native image editor, build script and guide.
- `native-common/`: shared name-image parser.
- `chomp-b3-macos/`: native USB helpers and guarded name/PIN transport.

This repository contains the native Mac app build inputs. Compatibility checks reject unsupported device layouts. This is an independent utility, not an official Blafili or Qualcomm application.
