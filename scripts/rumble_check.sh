#!/bin/bash
# Controller vibration check without a game: builds dm/host/rumble_check.m with the host's haptics code and plays a short sequence on the first
# connected controller (DualSense, Xbox, ...), wired or over Bluetooth. Keep its window in front while it plays. Exit status 1 when the build fails
# or no controller connects within 15 s.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT=${TMPDIR:-/tmp}/rumble_check
clang -O1 -fobjc-arc -Wall -Werror -I"$REPO/dm/host" -o "$OUT" "$REPO/dm/host/rumble_check.m" "$REPO/dm/host/haptics_apple.m" "$REPO/dm/host/hle_haptics.c" \
    -framework Cocoa -framework GameController -framework CoreHaptics -framework IOKit -framework AVFAudio
exec "$OUT"
