#!/bin/bash
# build_lego_hp.sh — LEGO® Harry Potter™ Collection (010052A00B5D2000), update 1.0.2 (v131072).
#
#   ./scripts/build_lego_hp.sh module    # rewrite libharry2.nro (Years 5-7) and verify the sha256 — the recipe
#   ./scripts/build_lego_hp.sh survey    # what the game is made of; builds nothing
#
# The module step is the one this branch exists for: the NSP's nro0 + dm/games/<id>/patches.py +
# dm/tools/patch_nso.py = nro0.patched.bin, byte for byte (13 s on an M1 Pro).
#
#   NRO=/path/to/libharry2.nro   a stock NRO (default: the one the NSP script extracted, under WORK_DIR)
#   OUT=/path                    where the rewritten module and its tables go
#   NSP=/path                    the folder holding the game's NSPs (survey)
#   KEYS=/path                   prod.keys (survey)
#   PYTHON=/path/python3.14      must have capstone 5.x (patch_nso.py rejects other majors)
#
# The full app build is not one of these steps: its shader split needs the derived profile's tables (the profile here
# carries only what the rewrite reads), so it stays the NSP script with DM_GAMES_DIR pointing there — see the note,
# "Shaders" and "Open".
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
ID=010052A00B5D2000
NSP=${NSP:-"/Volumes/T7 Storage1/Assets/Software/1. Switch Roms/Lego Harry Potter Collection"}
KEYS=${KEYS:-$HOME/.switch/prod.keys}
PY=${PYTHON:-/opt/homebrew/bin/python3.14}
WORK=${WORK_DIR:-"/Volumes/Workspace/Lego HP-NX/nsp/${ID}_build"}   # the build folder this game was brought up in
EXPECT=56f3bf4fdbb712f3c4f7d1c32d921efd38ec6abde304616ab29df82150ce9820   # nro0.patched.bin the app carries
case ${1:-module} in
  module)
    nro=${NRO:-$WORK/nro_v131072/nro0.nro}
    out=${OUT:-$WORK/repro}
    [ -f "$nro" ] || { echo "no $nro: set NRO= to the game's libharry2.nro (nro0)" >&2; exit 2; }
    mkdir -p "$out"
    "$PY" "$here/dm/tools/patch_nso.py" "$nro" "$out" nro0
    got=$(shasum -a 256 "$out/nro0.patched.bin" | cut -d' ' -f1)
    if [ "$got" = "$EXPECT" ]; then
      echo "nro0.patched.bin reproduces: $got"
    else
      echo "nro0.patched.bin differs: $got" >&2
      echo "  expected $EXPECT" >&2
      echo "  the rewriter, the patch files or the NRO are not the ones this module was built from" >&2
      exit 1
    fi ;;
  survey)
    exec "$PY" "$here/scripts/build_ipa_from_nsp.py" "$NSP" --survey-only ;;
  *) echo "usage: $0 [module|survey]" >&2; exit 2 ;;
esac
