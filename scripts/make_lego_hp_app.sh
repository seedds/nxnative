#!/bin/bash
# make_lego_hp_app.sh — LEGO® Harry Potter™ Collection (010052A00B5D2000): the Mac app the Years 5-7 fix ships as.
#
#   ./scripts/make_lego_hp_app.sh --from "/path/Lego HP NX.app" --host /path/to/dmhost \
#                                 --out "/path/Lego HP NX (fixed).app"
#
# The app is its source bundle with exactly two files changed:
#
#   Contents/MacOS/dmhost              the host built from the pinned source (see dm/games/<id>/host/README.md);
#                                      sha256 99100d734bea3c12a3d2e68c62699e3e190e4d4b2d8ba815d1cc924f1798d723
#   Contents/Resources/dm/env.txt      the present-path pins (this profile's env.txt), because the app's settings
#                                      window exports its own defaults over the profile compiled into the host
#
# Everything else — romfs.img, the rewritten modules, the shader packages, launcher.json, the icon — is the source
# bundle's. Nothing here codesigns or re-signs: an unsigned host in a bundle whose signature no longer covers it is
# what runs, and re-signing it would change the binary the sha above names.
#
#   SRC=, HOST=, OUT=   the same three as environment variables
#   --dry-run           check and report, write nothing
#   --force             replace OUT when it exists
#
# The source bundle must already carry this profile's module: nro0.patched.bin sha256 56f3bf4f… (what
# `./scripts/build_lego_hp.sh module` produces). A bundle that does not is told so, and left alone.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
ID=010052A00B5D2000
HOST_SHA=99100d734bea3c12a3d2e68c62699e3e190e4d4b2d8ba815d1cc924f1798d723     # the host the fix was measured with
MODULE_SHA=56f3bf4fdbb712f3c4f7d1c32d921efd38ec6abde304616ab29df82150ce9820   # nro0.patched.bin (Years 5-7)
ENV_SRC=$here/dm/games/$ID/env.txt

dry=0; force=0
while [ $# -gt 0 ]; do
  case $1 in
    --from) SRC=${2:?--from needs a path}; shift 2 ;;
    --host) HOST=${2:?--host needs a path}; shift 2 ;;
    --out)  OUT=${2:?--out needs a path}; shift 2 ;;
    --dry-run) dry=1; shift ;;
    --force) force=1; shift ;;
    -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1 (see --help)" >&2; exit 2 ;;
  esac
done
SRC=${SRC:?--from the app to change, e.g. --from "/Volumes/Workspace/Lego HP-NX/Lego HP NX.app"}
HOST=${HOST:?--host the host binary, e.g. --host /Volumes/Workspace/Lego HP-NX/nxnative/dm/host/dmhost}
OUT=${OUT:?--out the app to write, e.g. --out "/Volumes/Workspace/Lego HP-NX/Lego HP NX (fixed).app"}

die() { echo "$*" >&2; exit 1; }
[ -d "$SRC/Contents" ] || die "no bundle at $SRC (expected $SRC/Contents)"
[ -f "$SRC/Contents/MacOS/dmhost" ] || die "$SRC has no Contents/MacOS/dmhost: not an nxnative app"
[ -f "$SRC/Contents/Info.plist" ] || die "$SRC has no Contents/Info.plist"
[ -f "$HOST" ] || die "no host at $HOST (make -C dm/host GAME=$ID)"
[ -f "$ENV_SRC" ] || die "no $ENV_SRC: this branch's env.txt is the second half of the delta"
[ ! "$OUT" -ef "$SRC" ] || die "OUT is SRC: this script never writes the source bundle"
[ ! -e "$OUT" ] || { [ "$force" = 1 ] || die "$OUT exists (--force replaces it)"; }

# The host: the pinned build, or say which one this is. A host built from another commit is not the measured one,
# and the numbers in the note belong to the pinned binary.
host_sha=$(shasum -a 256 "$HOST" | cut -d' ' -f1)
if [ "$host_sha" = "$HOST_SHA" ]; then
  echo "host: $host_sha (the pinned build)"
else
  [ "${ALLOW_OTHER_HOST:-0}" = 1 ] || die "host is $host_sha, not the pinned $HOST_SHA
  build it from the pinned source (dm/games/$ID/host/README.md), or set ALLOW_OTHER_HOST=1 to ship this one"
  echo "host: $host_sha (NOT the pinned build; ALLOW_OTHER_HOST=1)" >&2
fi
[ "$(head -c 4 "$HOST" | xxd -p)" = "cffaedfe" ] || die "$HOST is not a Mach-O arm64 binary"

# The source bundle's module: the delta assumes the fix's module is already in it. Report both before copying 13 GB.
mod=$SRC/Contents/Resources/dm/nro0.patched.bin
[ -f "$mod" ] || die "no $mod: this bundle is not for this profile (build the module first)"
mod_sha=$(shasum -a 256 "$mod" | cut -d' ' -f1)
if [ "$mod_sha" = "$MODULE_SHA" ]; then
  echo "module: $mod_sha (nro0.patched.bin, the profile's cut)"
else
  echo "module: $mod_sha is NOT $MODULE_SHA — the bundle carries a different Years 5-7 module; the module is" >&2
  echo "        not part of this delta, so it is left as it is (./scripts/build_lego_hp.sh module rebuilds it)" >&2
fi

if [ "$dry" = 1 ]; then
  echo "would copy $SRC -> $OUT (clone), install the host and write Contents/Resources/dm/env.txt:"
  sed 's/^/    /' "$ENV_SRC" | grep -v '^    #' || true
  exit 0
fi

# 1. The bundle. ditto keeps resource forks, xattrs and the signature files as they are; --clone is an APFS
#    clonefile, so 13 GB of RomFS costs nothing on the same volume.
rm -rf "$OUT" 2>/dev/null || true
if ! ditto --clone "$SRC" "$OUT" 2>/dev/null; then
  echo "clone unsupported here: copying (this reads and writes the whole bundle)" >&2
  ditto "$SRC" "$OUT"
fi
[ -d "$OUT/Contents" ] || die "the copy did not produce a bundle at $OUT"

# 2. The host.
install -m 755 "$HOST" "$OUT/Contents/MacOS/dmhost"

# 3. The env pins, with the reason they are here (the file says it).
mkdir -p "$OUT/Contents/Resources/dm"
install -m 644 "$ENV_SRC" "$OUT/Contents/Resources/dm/env.txt"

# 4. What the bundle now is. Both files are the delta; the rest is the source bundle's, byte for byte.
got_host=$(shasum -a 256 "$OUT/Contents/MacOS/dmhost" | cut -d' ' -f1)
[ "$got_host" = "$( [ "$host_sha" = "$HOST_SHA" ] && echo "$HOST_SHA" || echo "$host_sha" )" ] \
  || die "the installed host is $got_host, not what was copied ($host_sha)"
cmp -s "$OUT/Contents/Resources/dm/env.txt" "$ENV_SRC" || die "env.txt did not land as this branch's copy"
cmp -s "$OUT/Contents/Resources/dm/nro0.patched.bin" "$mod" || die "the module changed: this delta touches two files"
if [ -f "$SRC/Contents/Resources/dm/env.txt" ]; then
  cmp -s "$SRC/Contents/Resources/dm/env.txt" "$ENV_SRC" || echo "note: the source bundle had an env.txt; it is replaced" >&2
fi
echo "wrote $OUT"
echo "  Contents/MacOS/dmhost            $got_host"
echo "  Contents/Resources/dm/env.txt    $(wc -c < "$OUT/Contents/Resources/dm/env.txt" | tr -d ' ') bytes, $(grep -c '^DM_' "$OUT/Contents/Resources/dm/env.txt") pins"
echo "  Contents/Resources/dm/nro0.patched.bin  $mod_sha (unchanged)"
echo "unsigned host inside a signed bundle: expected, and not re-signed (nothing here codesigns)"
