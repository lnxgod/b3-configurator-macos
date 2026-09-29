# B3 Configurator for macOS

A native macOS app to change the blafili B3 Bluetooth name and four-digit PIN over USB. The distributed `.app` bundles compiled Swift and C executables. It does not use Python, download dependencies, or compile anything at launch.

## Open the app

Download the packaged app from the [latest release](https://github.com/lnxgod/b3-configurator-macos/releases/latest): choose `B3-Configurator-macOS-arm64.zip` for Apple Silicon or `B3-Configurator-macOS-x86_64.zip` for Intel. Both builds target macOS 12 or later. Extract the ZIP to a local, non-synced folder (for example, your local Applications folder), then double-click **B3 Configurator.app**, connect exactly one B3 with a USB **data** cable, and choose **Check connection**.

Select **Change Bluetooth name**, **Set and enable PIN**, or both, then choose **Apply selected changes**. New names support 1–16 UTF-8 bytes. PINs require four ASCII digits; leading zeros are preserved. The PIN and confirmation fields start at `1234` and show their digits so you can check them directly; this is the app's suggested value, not a reading of the device's current PIN. Keep USB power connected until the app reports that the selected settings were verified after reboot. Reboots interrupt audio.

The name field shows a live byte counter and limits typing and pasted text to 16 bytes (16 basic ASCII characters), matching the reported working device-display name `GameChangersAIh1`. Emoji and accented characters can use more than one byte. Excess input is removed without splitting a composed character or deleting an existing suffix. Input-method composition is allowed to finish before the limit is applied. The field stays on one line at a fixed width. Both the save operation and the native image editor independently enforce the 16-byte new-name limit. Existing names up to the original 40-byte storage limit remain readable so they can be shortened.

The app does not communicate with USB automatically at launch. Checking the connection reads protocol, mode and schema information but does not open the name debug interface or change stored settings.

Name and PIN updates are sequential, not atomic. A successful rename remains in place if a later PIN update fails. The app reports this explicitly.

## Private backups and recovery

Session folders are saved under `~/Library/Application Support/B3 Configurator/Backups/`, with owner-only directory/file permissions. **Show backups** opens this folder. Images and PIN blocks may contain private device settings; keep them local.

- **Restore PIN…** accepts a session directory containing `before.bin` and `result.json`. It validates the schema and SHA-256 checksum and restores only PIN digits and the PIN-mode bit, preserving the other current settings. Original Python-tool PIN backups use the same format.
- To undo a rename, use the previous name recorded in that session's `name-result.json` if it fits the 16-byte limit; shorten an older, longer name to fit. Restore PIN does not restore the name or the whole EEPROM image.
- **Return to normal mode** recovers the AHI operating mode. If the B3 is already in normal mode, this does not force a reset or prove the debug interface is closed. Power-cycle the B3 if temporary name access is reported as still active.
- The app does not retry uncertain persistent writes. After an interrupted name write, retain the backup and investigate the device state before retrying; a failed write is not proof that nothing changed.

Only the tested B3 protocol/configuration layout is supported: AHI 0.4, schema `5819ff8235a6d18029d414ec778a7f5c`, chip `0x2049`, and the observed EEPROM filesystem v0x0101. Other layouts are rejected.

## Build and test

Build-machine requirement: Xcode or Apple Command Line Tools with Swift and clang. End users do not need these tools.

```sh
./native-macos/build.sh
```

The script compiles the app and four helpers, runs offline tests, signs the bundle ad hoc, and verifies that signature. It packages a ZIP under `dist/`, verifies an extracted copy, and prints the temporary local app path for preview. It builds for the machine's architecture; building on an Intel Mac produces an Intel build.

This workspace's file sync service adds Finder metadata to loose `.app` bundles, which causes strict signature verification to fail. Building outside the synced folder and delivering an archive preserves the verified app. No quarantine flags are removed by the build.

Hardware-free tests:

```sh
"/path/to/B3 Configurator.app/Contents/MacOS/B3Configurator" --self-test
"/path/to/B3 Configurator.app/Contents/Helpers/b3_image" --self-test
"/path/to/B3 Configurator.app/Contents/Helpers/debug_unlock" --self-test
```

These test PIN byte preservation, leading zeros, identity rejection, complete mocked rename/PIN lifecycles, backup restoration, post-reboot mismatches, no retry/reset after uncertain name writes, debug cleanup, private files, name-image parsing/patching, and the AES exchange. The image tests accept 16-byte names, reject 17-byte targets, cover multibyte UTF-8, and read and shorten a legacy 40-byte name while preserving unrelated settings. Before the native app's stricter new-name cap was added, its C image editor matched the original Python editor across 196 synthetic transformations, byte for byte; targets over 16 bytes are now intentionally rejected.

These are offline tests. During local use, the native app reported successful name and PIN writes and post-reboot readback verification on the connected B3. The user reported that the 16-byte ASCII name `GameChangersAIh1` displayed correctly, which sets the new-name cap. Unicode rendering has not been verified on hardware. The original package's pairing test did not establish that a Bluetooth peer is required to enter the stored PIN. Audio playback was not tested.

The downloadable build is ad hoc signed, not Developer ID signed or notarized. macOS may block the first launch; if you trust the release, follow [Apple's supported first-launch approval instructions](https://support.apple.com/en-us/102445). Passing the build's signature checks establishes bundle integrity, not Apple Developer ID trust.

## Source layout

- `App.swift`: AppKit interface, native process runner, USB workflow coordination, private journals/backups, and mocked workflow tests.
- `b3_image.c`: offline native name-image reader and editor, with self-tests.
- `build.sh`, `Info.plist`: native app packaging.
- `../chomp-b3-macos/`: native C USB helpers and write guards compiled into the app. The original Python command-line distribution is not required to build or run this native app.

## Android feasibility

The existing rename flow authenticates directly to the B3's class-9 internal hub, then uses a debug sibling device. Stock Android excludes hubs from application USB access. No supported alternate name command was established in the reviewed AHI implementation, so a standalone root-free Android rename app is not currently demonstrated. A native macOS app can use the existing transport.

References: [AOSP USB host access rules](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/services/usb/java/com/android/server/usb/UsbHostManager.java), [Qualcomm-authored AHI protocol source mirrored publicly](https://github.com/KunYi/adk63_sink/blob/master/apps/libs/ahi/ahi_protocol.h).
