#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

APP_PATH="$ROOT_DIR/NxNative_Launcher.app"
if [ ! -d "$APP_PATH" ]; then
    APP_PATH="$SCRIPT_DIR/../NxNative_Launcher.app"
fi

if [ ! -d "$APP_PATH" ]; then
    echo "Error: NxNative_Launcher.app not found. Build it first with build_gui.sh."
    exit 1
fi

DMG_NAME="NxNative_Launcher.dmg"
OUTPUT_DMG="$ROOT_DIR/$DMG_NAME"
STAGING="/tmp/nxnative_dmg_staging"

echo "==> Preparing DMG staging for NxNative_Launcher..."
rm -rf "$STAGING"
mkdir -p "$STAGING"

# Copy App bundle
cp -R "$APP_PATH" "$STAGING/"

# Create Applications shortcut
ln -s /Applications "$STAGING/Applications"

# Create DMG
echo "==> Building compressed DMG: $OUTPUT_DMG"
rm -f "$OUTPUT_DMG"
hdiutil create -volname "NxNative_Launcher" -srcfolder "$STAGING" -ov -format UDZO "$OUTPUT_DMG"

# Clean staging
rm -rf "$STAGING"

echo "==> Successfully created DMG at: $OUTPUT_DMG"
