#!/bin/bash
# Build a universal dmsh_xlate launcher so the shader translator runs on an Intel Mac.
#
# The translator is a .NET app (dmsh_xlate.dll, unchanged, covered by dmsh_chain.lock) behind a
# native apphost launcher. The committed launcher is arm64 only, so an Intel Mac fails with
# "Bad CPU type in executable". The lock covers the DLL and --source-sha, not the launcher, so a
# launcher built from the same sources is accepted: this script builds the x86_64 apphost with
# `dotnet build -r osx-x64` and joins it to the committed arm64 one with lipo.
#
# Usage: tools/intel-mac/build_dmsh_xlate.sh   (from anywhere; needs dotnet + lipo, i.e. Xcode CLT)
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
BIN="$REPO/dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate"
LOCK="$REPO/dm/gfx/tools/dmsh_chain.lock"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
die() { echo "FAIL: $*" >&2; exit 1; }

command -v dotnet >/dev/null || die "dotnet not found (6+ / SDK 8+ needed)"
command -v lipo >/dev/null || die "lipo not found (install Xcode command line tools)"
[ -f "$LOCK" ] || die "no $LOCK"

dotnet build "$REPO/dm/gfx/tools/dmsh_xlate" -c Release -r osx-x64 -o "$TMP/x64" >"$TMP/build.log" 2>&1 \
  || { tail -5 "$TMP/build.log" >&2; die "dotnet build failed"; }

git -C "$REPO" show HEAD:dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate > "$TMP/arm64" \
  || die "cannot read the committed arm64 launcher from git HEAD"
chmod +x "$TMP/arm64" "$TMP/x64/dmsh_xlate"

lipo -create "$TMP/arm64" "$TMP/x64/dmsh_xlate" -output "$TMP/x64/dmsh_xlate.universal" || die "lipo failed"

GOT=$("$TMP/x64/dmsh_xlate.universal" --source-sha | tail -1)   # beside its dll, so the apphost resolves it
WANT=$(awk '$1=="xlate_source"{print $2}' "$LOCK")
[ "$GOT" = "$WANT" ] || die "source sha $GOT does not match the lock ($WANT): the DLL differs from the locked build"

[ -f "$BIN.arm64.bak" ] || cp -p "$BIN" "$BIN.arm64.bak"   # keep the arm64-only launcher for reference
install -m 755 "$TMP/x64/dmsh_xlate.universal" "$BIN"
echo "ok: $BIN is universal ($(lipo -archs "$BIN")), xlate_source $GOT"
