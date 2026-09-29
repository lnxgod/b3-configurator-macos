#!/bin/bash
# Build a self-contained native application. No helper is invoked against USB.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SOURCE_DIR="$PROJECT_DIR/chomp-b3-macos"
DIST_DIR="$PROJECT_DIR/dist"
APP_NAME="B3 Configurator.app"
ARCH="$(uname -m)"
MIN_MACOS="12.0"
HELPERS_ONLY=0
ARCHIVE_NAME="B3-Configurator-macOS-$ARCH.zip"

usage() {
    cat <<'USAGE'
Usage: ./native-macos/build.sh [--helpers-only]

Builds dist/B3-Configurator-macOS-<architecture>.zip (macOS 12+).
Requires Apple's Xcode or Command Line Tools only on the build machine.
The finished app includes its native helpers and needs no Python or compiler.
The app is signed and verified in /private/tmp to avoid Finder metadata added
by synced Documents folders. A successful build keeps that app for local
preview and prints its path. Extract the ZIP into a non-synced Applications
folder before use; the archive is the durable distributable artifact.

--helpers-only  Compile and self-test helpers without requiring App.swift.
                Outputs native-macos/.build/<architecture>/Helpers/.
USAGE
}

case "${1:-}" in
    "") ;;
    --helpers-only) HELPERS_ONLY=1 ;;
    --help|-h) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
esac
if [[ $# -gt 1 ]]; then usage >&2; exit 2; fi
case "$ARCH" in
    arm64|x86_64) ;;
    *) printf 'Unsupported build architecture: %s\n' "$ARCH" >&2; exit 1 ;;
esac

for source in probe.c debug_unlock.c usb_descriptors.c pin_guard.h name_transport.h; do
    if [[ ! -f "$SOURCE_DIR/$source" ]]; then
        printf 'Missing source: %s\n' "$SOURCE_DIR/$source" >&2
        exit 1
    fi
done
if [[ "$HELPERS_ONLY" -eq 0 ]]; then
    for source in App.swift b3_image.c Info.plist; do
        if [[ ! -f "$SCRIPT_DIR/$source" ]]; then
            printf 'Missing native app source: %s\n' "$SCRIPT_DIR/$source" >&2
            exit 1
        fi
    done
fi

CLANG="$(xcrun --find clang)"
SDK_PATH="$(xcrun --sdk macosx --show-sdk-path)"
C_FLAGS=(-std=c11 -O2 -Wall -Wextra -Werror -arch "$ARCH"
    -mmacosx-version-min="$MIN_MACOS" -isysroot "$SDK_PATH" -I "$SOURCE_DIR")

mkdir -p "$DIST_DIR"
STAGE_DIR="$(mktemp -d /private/tmp/b3-native-build.XXXXXX)"
VERIFY_DIR=""
KEEP_STAGE=0
cleanup() {
    local result=$?
    if [[ -n "$VERIFY_DIR" ]]; then rm -rf "$VERIFY_DIR"; fi
    if [[ "$KEEP_STAGE" -eq 1 ]]; then
        if [[ "$result" -ne 0 ]]; then
            printf 'Build diagnostics preserved at %s\n' "$STAGE_DIR" >&2
            xattr -r "$STAGED_APP" >&2 || true
        fi
    else
        rm -rf "$STAGE_DIR"
    fi
    exit "$result"
}
trap cleanup EXIT
STAGED_APP="$STAGE_DIR/$APP_NAME"
HELPERS_DIR="$STAGED_APP/Contents/Helpers"
mkdir -p "$HELPERS_DIR"

printf 'Compiling native USB helpers for %s…\n' "$ARCH"
for helper in probe debug_unlock usb_descriptors; do
    "$CLANG" "${C_FLAGS[@]}" "$SOURCE_DIR/$helper.c" \
        -framework IOKit -framework CoreFoundation -o "$HELPERS_DIR/$helper"
done
# --self-test returns before any USB discovery; only mocked transfers and AES run.
"$HELPERS_DIR/debug_unlock" --self-test

if [[ -f "$SCRIPT_DIR/b3_image.c" ]]; then
    "$CLANG" "${C_FLAGS[@]}" "$SCRIPT_DIR/b3_image.c" -o "$HELPERS_DIR/b3_image"
    # This helper handles files only, and its self-test uses in-memory fixtures.
    "$HELPERS_DIR/b3_image" --self-test
elif [[ "$HELPERS_ONLY" -eq 1 ]]; then
    printf 'b3_image.c is not present yet; built the three original helpers.\n'
fi

if [[ "$HELPERS_ONLY" -eq 1 ]]; then
    PREVIEW_DIR="$SCRIPT_DIR/.build/$ARCH/Helpers"
    mkdir -p "$PREVIEW_DIR"
    for helper in "$HELPERS_DIR"/*; do cp "$helper" "$PREVIEW_DIR/"; done
    printf 'Compiled helpers: %s\n' "$PREVIEW_DIR"
    exit 0
fi

SWIFTC="$(xcrun --find swiftc)"
mkdir -p "$STAGED_APP/Contents/MacOS" "$STAGE_DIR/ModuleCache"
cp -X "$SCRIPT_DIR/Info.plist" "$STAGED_APP/Contents/Info.plist"
plutil -lint "$STAGED_APP/Contents/Info.plist"
printf 'Compiling the native application…\n'
"$SWIFTC" -O -swift-version 5 -sdk "$SDK_PATH" \
    -target "$ARCH-apple-macosx$MIN_MACOS" \
    -module-cache-path "$STAGE_DIR/ModuleCache" \
    -framework AppKit -framework Foundation -framework CryptoKit \
    "$SCRIPT_DIR/App.swift" -o "$STAGED_APP/Contents/MacOS/B3Configurator"
# The app's --self-test path uses mocked device operations and never opens USB.
"$STAGED_APP/Contents/MacOS/B3Configurator" --self-test

if command -v codesign >/dev/null 2>&1; then
    # Keep a failed signature artifact for diagnosis without touching sources.
    KEEP_STAGE=1
    # Finder may attach metadata to newly created .app directories. Remove only
    # the two attributes codesign rejects, and only from this staged build.
    # Preserve quarantine, provenance and every other extended attribute.
    while IFS= read -r -d '' item; do
        for attribute in com.apple.FinderInfo com.apple.ResourceFork; do
            if xattr -p "$attribute" "$item" >/dev/null 2>&1; then
                xattr -d "$attribute" "$item"
            fi
        done
    done < <(/usr/bin/find "$STAGED_APP" -print0)
    for helper in "$HELPERS_DIR"/*; do
        codesign --force --sign - --timestamp=none "$helper"
    done
    codesign --force --sign - --timestamp=none "$STAGED_APP"
    codesign --verify --deep --strict "$STAGED_APP"
    printf 'Ad hoc signature verified. This local build is not notarized.\n'
else
    printf 'codesign is unavailable; this build is unsigned and not notarized.\n'
fi

# Do not move a loose .app into this synced workspace: FileProvider can attach
# FinderInfo there immediately, invalidating its strict signature verification.
# The archive contains only our newly authored bundle, not source/download data.
ARCHIVE_TEMP="$STAGE_DIR/$ARCHIVE_NAME"
ditto --norsrc --noextattr -c -k --keepParent "$STAGED_APP" "$ARCHIVE_TEMP"
VERIFY_DIR="$(mktemp -d /private/tmp/b3-archive-check.XXXXXX)"
ditto -x -k "$ARCHIVE_TEMP" "$VERIFY_DIR"
EXTRACTED_APP="$VERIFY_DIR/$APP_NAME"
if command -v codesign >/dev/null 2>&1; then
    codesign --verify --deep --strict "$EXTRACTED_APP"
fi
cmp "$STAGED_APP/Contents/MacOS/B3Configurator" "$EXTRACTED_APP/Contents/MacOS/B3Configurator"
for helper in probe debug_unlock usb_descriptors b3_image; do
    cmp "$HELPERS_DIR/$helper" "$EXTRACTED_APP/Contents/Helpers/$helper"
done
cmp "$STAGED_APP/Contents/Info.plist" "$EXTRACTED_APP/Contents/Info.plist"
rm -rf "$VERIFY_DIR"
VERIFY_DIR=""
# Preserve any previous package in this successful build's temporary directory.
# Every other item in dist remains unchanged.
ARCHIVE_PATH="$DIST_DIR/$ARCHIVE_NAME"
if [[ -e "$ARCHIVE_PATH" || -L "$ARCHIVE_PATH" ]]; then
    if [[ ! -f "$ARCHIVE_PATH" || -L "$ARCHIVE_PATH" ]]; then
        printf 'Refusing to replace a non-file archive destination: %s\n' "$ARCHIVE_PATH" >&2
        exit 1
    fi
    cp -X "$ARCHIVE_PATH" "$STAGE_DIR/previous-$ARCHIVE_NAME"
fi
mv "$ARCHIVE_TEMP" "$ARCHIVE_PATH"
( cd "$DIST_DIR" && shasum -a 256 "$ARCHIVE_NAME" > "$ARCHIVE_NAME.sha256" )
rm -rf "$STAGE_DIR/ModuleCache"
KEEP_STAGE=1
printf 'Verified archive: %s\nLocal preview: %s\nOpen preview: open "%s"\n' \
    "$ARCHIVE_PATH" "$STAGED_APP" "$STAGED_APP"
