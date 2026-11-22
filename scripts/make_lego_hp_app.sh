#!/bin/bash
# make_lego_hp_app.sh — LEGO® Harry Potter™ Collection (010052A00B5D2000): the Mac app the Years 5-7 GLYPHFIX ships as.
#
#   ./scripts/make_lego_hp_app.sh --from "/path/Lego HP NX.app" --host /path/to/dmhost \
#                                 --module /path/nro0.patched.bin --out "/path/Lego HP NX GLYPHFIX.app"
#
# The app is its source bundle with exactly three files changed:
#
#   Contents/MacOS/dmhost                   the host built from the pinned source (see dm/games/<id>/host/README.md);
#                                           sha256 1c78fd0d9f2c454afa556673bccc948aa3f9936122adb82507d2b5c661aa10a5
#   Contents/Resources/dm/nro0.patched.bin   libharry2.nro (Years 5-7) rewritten with this profile's two stubs — the
#                                           pass-table publish and the blit guard; sha256 6729d1b3…, 18,762,448 bytes
#                                           (./scripts/build_lego_hp.sh module builds it)
#   Contents/Resources/dm/env.txt            the present-path pins (this profile's env.txt), because the app's settings
#                                           window exports its own defaults over the profile compiled into the host
#
# Everything else — romfs.img, the other rewritten modules, the shader packages, launcher.json, the icon — is the source
# bundle's. Nothing here codesigns or re-signs: an unsigned host in a bundle whose signature no longer covers it is what
# runs, and re-signing it would change the binary the sha above names.
#
#   SRC=, HOST=, MODULE=, OUT=   the same four as environment variables
#   --dry-run                    check and report, write nothing
#   --force                      replace OUT when it exists
#
# The source bundle must be one this profile was built for: its nro0.patched.bin is the pre-guard module 56f3bf4f… or
# already the GLYPHFIX one. A bundle that is neither is told so, and left alone.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
ID=010052A00B5D2000
HOST_SHA=1c78fd0d9f2c454afa556673bccc948aa3f9936122adb82507d2b5c661aa10a5     # the host this revision was measured with (series + host-ringoff-y14.patch)
MODULE_SHA=6729d1b3c0857fbb93ed6d94ceb4f4a5105f4fc112f376dcab18527d7c1a2496   # nro0.patched.bin (Years 5-7, both stubs)
MODULE_BYTES=18762448
PREV_SHA=56f3bf4fdbb712f3c4f7d1c32d921efd38ec6abde304616ab29df82150ce9820     # the same module before the blit guard
ENV_SRC=$here/dm/games/$ID/env.txt

dry=0; force=0
while [ $# -gt 0 ]; do
  case $1 in
    --from) SRC=${2:?--from needs a path}; shift 2 ;;
    --host) HOST=${2:?--host needs a path}; shift 2 ;;
    --module) MODULE=${2:?--module needs a path}; shift 2 ;;
    --out)  OUT=${2:?--out needs a path}; shift 2 ;;
    --dry-run) dry=1; shift ;;
    --force) force=1; shift ;;
    -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1 (see --help)" >&2; exit 2 ;;
  esac
done
SRC=${SRC:?--from the app to change, e.g. --from "/Volumes/Workspace/Lego HP-NX/Lego HP NX.app"}
HOST=${HOST:?--host the host binary, e.g. --host /Volumes/Workspace/Lego HP-NX/nxnative/dm/host/dmhost}
MODULE=${MODULE:?--module the rewritten module, e.g. --module "/Volumes/Workspace/Lego HP-NX/nsp/010052A00B5D2000_build/repro/nro0.patched.bin"}
OUT=${OUT:?--out the app to write, e.g. --out "/Volumes/Workspace/Lego HP-NX/Lego HP NX GLYPHFIX.app"}

die() { echo "$*" >&2; exit 1; }
[ -d "$SRC/Contents" ] || die "no bundle at $SRC (expected $SRC/Contents)"
[ -f "$SRC/Contents/MacOS/dmhost" ] || die "$SRC has no Contents/MacOS/dmhost: not an nxnative app"
[ -f "$SRC/Contents/Info.plist" ] || die "$SRC has no Contents/Info.plist"
[ -f "$HOST" ] || die "no host at $HOST (make -C dm/host GAME=$ID)"
[ -f "$MODULE" ] || die "no module at $MODULE (./scripts/build_lego_hp.sh module)"
[ -f "$ENV_SRC" ] || die "no $ENV_SRC: this branch's env.txt is the third file of the delta"
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

# The module: the one this branch's patches.py produces. Both hashes and the byte count, before copying 13 GB.
mod_sha=$(shasum -a 256 "$MODULE" | cut -d' ' -f1)
[ "$mod_sha" = "$MODULE_SHA" ] || die "module is $mod_sha, not the GLYPHFIX $MODULE_SHA
  ./scripts/build_lego_hp.sh module rebuilds it from this profile's patches.py"
[ "$(stat -f %z "$MODULE")" = "$MODULE_BYTES" ] || die "module is $(stat -f %z "$MODULE") bytes, not $MODULE_BYTES"
echo "module: $mod_sha ($MODULE_BYTES bytes, both stubs)"

# The source bundle must be one this profile rewrites: the pre-guard module or the guarded one.
srcmod=$SRC/Contents/Resources/dm/nro0.patched.bin
[ -f "$srcmod" ] || die "no $srcmod: this bundle is not for this profile"
src_sha=$(shasum -a 256 "$srcmod" | cut -d' ' -f1)
case $src_sha in
  "$PREV_SHA") echo "source module: $src_sha (before the blit guard; replaced)" ;;
  "$MODULE_SHA") echo "source module: $src_sha (already the GLYPHFIX module; replaced byte for byte)" ;;
  *) die "source bundle carries $src_sha, which is neither $PREV_SHA nor $MODULE_SHA: not this profile's bundle" ;;
esac

if [ "$dry" = 1 ]; then
  echo "would copy $SRC -> $OUT (clone), install the host and the module, write Contents/Resources/dm/env.txt:"
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
files_before=$(find "$SRC/Contents" -type f | wc -l | tr -d ' ')
env_new=0; [ -f "$SRC/Contents/Resources/dm/env.txt" ] || env_new=1   # the original app has no env.txt: this delta adds it

# 2. The host.
install -m 755 "$HOST" "$OUT/Contents/MacOS/dmhost"

# 3. The module — the guest patch, the only file the glyph fix is in.
install -m 644 "$MODULE" "$OUT/Contents/Resources/dm/nro0.patched.bin"

# 4. The env pins, with the reason they are here (the file says it).
mkdir -p "$OUT/Contents/Resources/dm"
install -m 644 "$ENV_SRC" "$OUT/Contents/Resources/dm/env.txt"

# 5. What the bundle now is. The three files are the delta; the rest is the source bundle's, byte for byte.
got_host=$(shasum -a 256 "$OUT/Contents/MacOS/dmhost" | cut -d' ' -f1)
[ "$got_host" = "$( [ "$host_sha" = "$HOST_SHA" ] && echo "$HOST_SHA" || echo "$host_sha" )" ] \
  || die "the installed host is $got_host, not what was copied ($host_sha)"
got_mod=$(shasum -a 256 "$OUT/Contents/Resources/dm/nro0.patched.bin" | cut -d' ' -f1)
[ "$got_mod" = "$MODULE_SHA" ] || die "the installed module is $got_mod, not $MODULE_SHA"
cmp -s "$OUT/Contents/Resources/dm/env.txt" "$ENV_SRC" || die "env.txt did not land as this branch's copy"
[ "$((files_before + env_new))" = "$(find "$OUT/Contents" -type f | wc -l | tr -d ' ')" ] \
  || die "the file count changed: this delta touches three files and adds none but env.txt"
if [ -f "$SRC/Contents/Resources/dm/env.txt" ]; then
  cmp -s "$SRC/Contents/Resources/dm/env.txt" "$ENV_SRC" || echo "note: the source bundle had an env.txt; it is replaced" >&2
fi
echo "wrote $OUT"
echo "  Contents/MacOS/dmhost                   $got_host"
echo "  Contents/Resources/dm/nro0.patched.bin  $got_mod ($(stat -f %z "$OUT/Contents/Resources/dm/nro0.patched.bin") bytes)"
echo "  Contents/Resources/dm/env.txt           $(wc -c < "$OUT/Contents/Resources/dm/env.txt" | tr -d ' ') bytes, $(grep -c '^DM_' "$OUT/Contents/Resources/dm/env.txt") pins"
if [ "$env_new" = 1 ]; then
  echo "three files changed (env.txt added); $files_before files in the source Contents, otherwise the source bundle's"
else
  echo "three files changed; $files_before files in Contents, unchanged in count and otherwise the source bundle's"
fi
echo "unsigned host inside a signed bundle: expected, and not re-signed (nothing here codesigns)"
