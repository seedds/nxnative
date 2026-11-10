#!/bin/bash
# Checks a Switch 2 controller's gyro and accelerometer on this Mac without a game: builds dm/host/sw2_gyro.m with the host's own Switch 2 driver (pad_switch2_apple.m)
# and prints the controller's motion ten times a second, as a game would get it (Switch frame: x right, y forward, z up; flat and still reads gravity 0 0 -1), with a window
# that shows a block turned as the controller is (blue: its face, orange: the triggers' edge; space or Recenter turns it back to face the screen; --no-window prints only).
# A healthy controller: flat and still, the gyro within a few deg/s of 0 and gravity about 1 G; turned, tens to hundreds of deg/s on the axis it turns around.
# Press a button on a controller already paired with this Mac (in a game with Switch 2 controllers on), or hold its Sync button to pair it. Ctrl-C quits.
# macOS asks once for Bluetooth access for the terminal that runs it. Usage: scripts/sw2_gyro.sh   (DM_SW2_VERBOSE=1 also logs the controller's raw reports)
set -euo pipefail
host="$(cd "$(dirname "$0")/../dm/host" && pwd)"
out="${TMPDIR:-/tmp}/nxnative-sw2-gyro"; mkdir -p "$out/inc"
: > "$out/inc/game_profile.h"   # dm.h includes a game profile; the driver uses none of it
cat > "$out/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>org.nxnative.sw2-gyro</string>
<key>CFBundleName</key><string>sw2_gyro</string>
<key>NSBluetoothAlwaysUsageDescription</key><string>Reads a Switch 2 controller's motion.</string>
</dict></plist>
EOF
bin="$out/sw2_gyro"
if [ ! -x "$bin" ] || [ -n "$(find "$host/sw2_gyro.m" "$host/pad_switch2_apple.m" "$host/pad_switch2_apple.h" "$host/switch2_proto.h" "$host/dm.h" "$host/dm_input.h" "$host/motion_fusion.h" "$0" -newer "$bin" 2>/dev/null)" ]; then
    echo "building $bin ..."
    clang -fobjc-arc -O1 -Wall -Werror -Wno-unused-function -I"$host" -I"$out/inc" -o "$bin" "$host/sw2_gyro.m" "$host/pad_switch2_apple.m" \
        -framework Cocoa -framework SceneKit -framework CoreBluetooth -framework IOBluetooth -sectcreate __TEXT __info_plist "$out/Info.plist"
fi
if pgrep -q dmhost; then echo "a game is running: quit it first (Cmd-Q), its Bluetooth connection to the controller would compete with this one" >&2; exit 1; fi
exec "$bin" "$@"
