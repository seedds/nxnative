#!/bin/bash
# Build one unsigned all-in-one IPA: the rewritten modules compiled into the executable, iOS shader libraries at DmIOS.app/msl, and the game files (RomFS) at DmIOS.app/romfs.
# In IPA mode Xcode skips the large resource copy; tools/pack_ipa.py packs the shader libraries, add-on content and RomFS straight from where they are (no Payload/ copy, nothing through the home disk).
# In --prepare-xcode mode the generated project copies those resources before Xcode signs it with the team the user selects. IPA mode signs only the executable ad-hoc to carry entitlements; this script never installs anything.
# Prerequisites: the rewriter (patched directory) and the iOS shader build (IOSOUT) have been run as in the README; Xcode and xcodegen are installed.
# Usage: dm/ios/tools/build_ipa.sh [output.ipa | --prepare-xcode]; --prepare-xcode generates the project without building, signing or packing.
#   Environment: GAME=game profile (dm/games/<base game title ID>, or one in DM_GAMES_DIR; required), W=the game's work folder (required: <NSP folder>/<title ID>-work, never the home folder), PATCHED / ROMFS / MSL_IOS can each be overridden
#   DM_COPY_ROMFS=NO leaves the game files (RomFS) out of the IPA, as it does for the Xcode build phase: the package then stays small (a large game's full package can exceed what iOS extracts), and the app takes the files from Documents/romfs.img (scripts/push_romfs.sh); default: they are packed
#   DM_ENTITLEMENTS=<plist> is the unsigned IPA's entitlements, signed ad-hoc into the executable (default: Increased Memory Limit; an empty string = none)
#   AOC=add-on content directory (default $W/data/aoc; when it exists it goes to DmIOS.app/aoc/<index>/; an empty string = none), GFXAPI=nvn / vulkan (default: by the shader libraries in MSL_IOS)
#   The app name is the game name in the base NSP's control data ($W/data/v0/control.nacp, else the one next to the RomFS), the app version is the display version in the packed game's control data (the one next to the RomFS:
#   nx_extract saves the update's there when an update is packed, else the base game's), the bundle ID is org.nxnative.<base game title ID in lowercase> (DM_DISPLAY_NAME / DM_APP_VERSION / DM_BUNDLE_ID override them);
#   CONTROL=<control.nacp> is the file whose languages the launcher's settings offer (the NSP script's languages_control; default: the packed game's control data as above);
#   DM_METAL_CAPTURE=1 makes a development build that can write GPU frame captures (MetalCaptureEnabled in Info.plist, for the host's DM_GPU_CAPTURE switch);
#   DM_SHOW_FPS=1 turns the frame-rate counter on (DMShowFPS in Info.plist; DM_SHOW_FPS in the app's Documents/env.txt overrides it on the phone);
#   DM_LANG=<language code> is the game language (DMLanguage in Info.plist, default en-US; DM_LANG in the app's Documents/env.txt overrides it on the phone);
#   both are also the launcher's defaults (dm/launcher.json from dm/games/settings.py; the player's choice there wins over them, Documents/env.txt over the choice);
#   the IPA name, code modules, RomFS marker and shader tables come from the game profile
#   PROGRAMS_TSV=<file> (the NSP script, a multi-program application: Super Mario 3D All-Stars): one app holding several programs, one row each, "<index>\t<profile>\t<patched>\t<RomFS>\t<iOS shaders>\t<graphics API>",
#   program 0's being GAME / PATCHED / ROMFS / MSL_IOS / GFXAPI (the app's name, version, icon and identity are its). Each program's modules go in sections tagged p<N> (gen_embed.py --tag),
#   its tables and launcher settings in dm/p<N>/, its RomFS and offline HTML pages in p<N>/, its shader libraries in msl/p<N>/, and the host is built over a table of their profiles
#   (game.py header_multi, the first program's profile lists them); main.m picks the program the last oe::ExecuteProgram recorded (Documents/program.bin). With --prepare-xcode the project's build phase copies them so (stage_resources.sh)
set -u
cd "$(dirname "$0")/.." || exit 1
IOS_SOURCE=$PWD; IOS=$PWD; DM=$(cd .. && pwd)
t() { echo "[$(date +%T)] $*"; }; die() { t "FAIL: $*"; exit 1; }
[ -n "${GAME:-}" ] || die "GAME is not set: GAME=<base game title ID> $0; profiles: $(python3 "$DM/games/game.py" list | awk -F'\t' '{printf "%s (%s)  ", $1, $3}')"
export GAME; GD=$(python3 "$DM/games/game.py" dir "$GAME") || die "no game profile $GAME"   # dm/games/<id>, or a profile in DM_GAMES_DIR (the NSP script's provisional one); the make that gen_build.py runs uses the same GAME
gp() { python3 "$DM/games/game.py" "$GAME" "$1" || die "game profile $GD: $1"; }   # read a key of the game profile
[ -f "$GD/game.tsv" ] || die "no game profile $GD/game.tsv"
[ -n "${W:-}" ] || die "W is not set: W=<NSP folder>/$GAME-work (the game's work folder) $0"
PKG=$(gp package)
if [ -z "${ROMFS:-}" ] && [ -f "$W/data/$PKG/RomFS.img" ]; then ROMFS=$W/data/$PKG/RomFS.img; fi
PATCHED=${PATCHED:-$W/build/patched_$PKG}; ROMFS=${ROMFS:-$W/data/$PKG/RomFS}; MSL_IOS=${MSL_IOS:-$W/build/ios/dmsh_b}; AOC=${AOC-$W/data/aoc}
PREPARE_XCODE=NO; [ "${1:-}" != --prepare-xcode ] || PREPARE_XCODE=YES
OUT_IPA=${1:-$(dirname "$W")/$(gp ipa)}; O=$W/build/ios/pkg; DD=$O/dd
XCODE_DD=${DM_XCODE_DERIVED_DATA:-$W/build/ios/xcode-dd}
MARKER=$(gp romfs_marker)
NACP=$W/data/v0/control.nacp; [ -f "$NACP" ] || NACP=$(dirname "$ROMFS")/control.nacp
[ -n "${DM_DISPLAY_NAME:-}" ] || DM_DISPLAY_NAME=$(python3 "$DM/tools/nacp.py" "$NACP") || die "no game name in $NACP (re-extract with nx_extract, or set DM_DISPLAY_NAME)"
VNACP=$(dirname "$ROMFS")/control.nacp; [ -f "$VNACP" ] || VNACP=$W/data/v0/control.nacp
[ -n "${DM_APP_VERSION:-}" ] || DM_APP_VERSION=$(python3 "$DM/tools/nacp.py" --version "$VNACP") || { t "no display version in $VNACP: the app version is 1.0"; DM_APP_VERSION=1.0; }
VNACP=${CONTROL:-$VNACP}   # from here on, the file that lists the game's languages for the launcher's settings
export LC_ALL=C DM_BUNDLE_ID=${DM_BUNDLE_ID:-org.nxnative.$(echo "$GAME" | tr 'A-Z' 'a-z')} DM_DISPLAY_NAME DM_APP_VERSION DM_APP_CATEGORY=$(gp app_category) DM_TEAM_ID=${DM_TEAM_ID:-0000000000}   # placeholder identity: unsigned, the install tool replaces it
if [ "${DM_SHOW_FPS:-0}" = 1 ]; then export DM_SHOW_FPS=YES; else export DM_SHOW_FPS=NO; fi
export DM_LANG=${DM_LANG:-en-US}
if [ "$PREPARE_XCODE" = YES ]; then
  IOS=${DM_XCODE_PROJECT_DIR:-$W/xcode}
  mkdir -p "$IOS" || die "mkdir $IOS"
  IOS=$(cd "$IOS" && pwd)
  [ "$IOS" != "$IOS_SOURCE" ] || die "the per-game Xcode project must be outside dm/ios"
  # Source links follow repository edits; generated files and build products belong to this game only. Linked again each time: a project prepared from another checkout (a removed worktree) follows this one
  for item in Sources tools; do ln -sfn "$IOS_SOURCE/$item" "$IOS/$item" || die "link $item"; done
  cp "$IOS_SOURCE/project.yml" "$IOS/project.yml" || die "copy project template"
  cd "$IOS" || die "cd $IOS"
  DM_SCHEME_NAME="$(echo "$DM_DISPLAY_NAME" | tr '/:' '__') ($GAME)"   # the scheme file is named after it

else DM_SCHEME_NAME=DmIOS; fi
export DM_SCHEME_NAME
# An NVN game's libraries are the folder b-$V or the one file b-$V.img beside its name (dm/tools/shader_lib.py), read in place: whether they hold a shader, one of their files, the path to what is there
lib_has() { python3 "$DM/tools/shader_lib.py" has "$1" 2> /dev/null; }
lib_cat() { python3 "$DM/tools/shader_lib.py" cat "$1" "$2" 2> /dev/null; }
lib_path() { python3 "$DM/tools/shader_lib.py" path "$1"; }
# The checks of one program's inputs (GAME, GD, PATCHED, ROMFS, MSL_IOS, GFXAPI, MODS, MARKER as set): every program of a game of several programs passes them
check_program() {
[ -f "$PATCHED/main.fixups.tsv" ] || die "no rewritten main in $PATCHED (run dm/tools/patch_nso.py first)"
MODS=$(python3 "$DM/games/game.py" modules "$GAME" "$PATCHED") || die "game profile $GD: modules"   # main's build row, as the host picks them: a version may lack a module another has
for f in $MODS; do [ -f "$PATCHED/$f.patched.bin" ] && [ -f "$PATCHED/$f.fixups.tsv" ] || die "no rewritten module $PATCHED/$f (run dm/tools/patch_nso.py first)"; done
python3 "$DM/tools/romfs.py" "$ROMFS" "$MARKER" || die "no RomFS at $ROMFS (missing $MARKER)"
V=${V:-$(sed -n 's/^#define DMSH_CHAIN "\(.*\)"/\1/p' "$DM/gfx/nvn/dmsh_chain.h")}   # the chain version the host is compiled with, one for every game
# A Vulkan game (Hades): the shaders are the game's SPIR-V, translated by dm/gfx/tools/vk_shaders.py into $MSL_IOS/vk; an OpenGL game (Digimon Story Cyber Sleuth): its GLSL, translated by
# dm/gfx/tools/gl_shaders.py into $MSL_IOS/gl; neither has NVN chain libraries. GFXAPI=nvn / vulkan / gl says which (the NSP script passes what main imports); without it, the shader
# libraries in $MSL_IOS tell: the chain's b-$V, else vk, else gl
if [ -z "${GFXAPI:-}" ]; then if lib_has "$MSL_IOS/b-$V"; then GFXAPI=nvn; elif [ -f "$MSL_IOS/vk/vk_shaders.metallib" ]; then GFXAPI=vulkan;
  elif [ -f "$MSL_IOS/gl/gl_shaders.metallib" ]; then GFXAPI=gl; else GFXAPI=nvn; fi; fi
if [ "$GFXAPI" = vulkan ]; then [ -f "$MSL_IOS/vk/vk_shaders.metallib" ] || die "no iOS Vulkan shader library $MSL_IOS/vk (dm/gfx/tools/vk_shaders.py \"$ROMFS\" <Mac dir> --ios \"$MSL_IOS\")"
elif [ "$GFXAPI" = gl ]; then [ -f "$MSL_IOS/gl/gl_shaders.metallib" ] || die "no iOS GL shader library $MSL_IOS/gl (dm/gfx/tools/gl_shaders.py \"$ROMFS\" <main> <Mac dir> --ios \"$MSL_IOS\")"
else [ -n "$V" ] && lib_has "$MSL_IOS/b-$V" || die "no iOS shader libraries $MSL_IOS/b-$V (IOSOUT=$MSL_IOS dmsh_build.py ...)"
  # One chain version serves every game and every state of a profile's tables: the libraries' stamp (b-$V/inputs.tsv, written by a full dmsh_build.py run) must name this game and the
  # digest of its profile's current inputs, and c1_full.tsv (not in the repository: the same build makes it, and cb_extent.tsv, from its Mac outputs) must name that digest, or the host ignores it
  IN=$(DM_GAME="$GAME" python3 "$DM/gfx/tools/dmsh_chain.py" --inputs) || die "dmsh_chain.py --inputs (game profile $GD)"
  STAMP=$(lib_cat "$MSL_IOS/b-$V" inputs.tsv)
  printf '%s\n' "$STAMP" | grep -qxF "$(printf 'game\t%s' "$GAME")" && printf '%s\n' "$STAMP" | grep -qxF "$(printf 'inputs\t%s' "$IN")" ||
    die "the iOS shader libraries $MSL_IOS/b-$V are not a full build from this profile's current tables (inputs $IN; their stamp: $([ -n "$STAMP" ] && printf '%s\n' "$STAMP" | grep -v '^#' | tr '\n\t' '  ' || echo none)): build them again (IOSOUT=$MSL_IOS dmsh_build.py ...)"
  head -1 "$GD/c1_full.tsv" 2> /dev/null | grep -q "^# chain b-$V inputs $IN " || die "$GD/c1_full.tsv is missing or not made from these libraries (chain $V, inputs $IN): a full dmsh_build.py run makes it and cb_extent.tsv"; fi
}
check_program
P0=$GAME; P0_PATCHED=$PATCHED; P0_ROMFS=$ROMFS; P0_MSL=$MSL_IOS; P0_GFX=$GFXAPI
prog_rows() { grep -v '^#' "$PROGRAMS_TSV" | grep -v '^$'; }   # index, profile, patched, RomFS, iOS shaders, graphics API
prog_set() {   # the globals of one row's program
  IFS=$'\t' read -r PN GAME PATCHED ROMFS MSL_IOS GFXAPI <<< "$1"; export GAME
  GD=$(python3 "$DM/games/game.py" dir "$GAME") || die "no game profile $GAME"; MODS=$(gp modules); MARKER=$(gp romfs_marker)
  if [ "$GFXAPI" = vulkan ]; then MSLDIR=vk; elif [ "$GFXAPI" = gl ]; then MSLDIR=gl; else MSLDIR=b-$V; fi
}
prog_back() { GAME=$P0; export GAME; GD=$(python3 "$DM/games/game.py" dir "$GAME"); PATCHED=$P0_PATCHED; ROMFS=$P0_ROMFS; MSL_IOS=$P0_MSL; GFXAPI=$P0_GFX; MODS=$(gp modules); MARKER=$(gp romfs_marker); }
if [ -n "${PROGRAMS_TSV:-}" ]; then
  [ -f "$PROGRAMS_TSV" ] || die "no PROGRAMS_TSV $PROGRAMS_TSV"
  while IFS= read -r row; do prog_set "$row"; t "program $PN: $GAME ($(gp name)), $GFXAPI"; check_program; done < <(prog_rows)
  prog_back
fi

t "app: $DM_DISPLAY_NAME $DM_APP_VERSION ($DM_BUNDLE_ID), language $DM_LANG$([ $DM_SHOW_FPS = YES ] && echo ', frame-rate counter on')"
t "generate Gen/ (host sources, embedded modules, tables)"
rm -rf Gen && mkdir -p Gen/dm || die "mkdir Gen"
python3 tools/gen_build.py "$DM/host" Gen > Gen/dm.xcconfig || die "gen_build"
if [ -n "${PROGRAMS_TSV:-}" ]; then   # each program's modules under its tag (the host is built over a table of the programs' profiles: the first program's profile lists them, game.py header_multi)
  F=; : > Gen/embed.S
  while IFS= read -r row; do prog_set "$row"
    J=(); [ -f "$(dirname "$ROMFS")/jit/jit.img" ] && J=(--jit "$(dirname "$ROMFS")/jit/jit.img")   # the nn::jit image (dm/tools/jit_capture.py), beside the RomFS
    F="$F $(python3 tools/gen_embed.py --tag "p$PN" ${J[@]+"${J[@]}"} "$PATCHED" "Gen/embed_p$PN.S" $MODS)" || die "gen_embed (program $PN)"
    cat "Gen/embed_p$PN.S" >> Gen/embed.S && rm "Gen/embed_p$PN.S"
  done < <(prog_rows); prog_back
else J=(); [ -f "$(dirname "$ROMFS")/jit/jit.img" ] && J=(--jit "$(dirname "$ROMFS")/jit/jit.img")
  F=$(python3 tools/gen_embed.py ${J[@]+"${J[@]}"} "$PATCHED" Gen/embed.S $MODS) || die "gen_embed"; fi
echo "OTHER_LDFLAGS = \$(inherited) \$(DM_FRAMEWORKS) $F" >> Gen/dm.xcconfig
# One program's tables into TD: the rewriter's, the shader tables and the launcher's settings
program_tables() {
cp "$PATCHED"/*.fixups.tsv "$PATCHED"/*.gates.tsv "$PATCHED"/*.cuts.tsv "$PATCHED"/*.obs.tsv $TD/ || die "copy rewriter tables"
if [ -f "$PATCHED/nro.tsv" ]; then cp "$PATCHED/nro.tsv" $TD/ || die "copy the NRO table"; fi   # the NRO modules the game loads with nn::ro (hle_ro.c), embedded by gen_embed.py
if [ "$GFXAPI" = vulkan ] || [ "$GFXAPI" = gl ]; then cp "$GD/shader_map.tsv" "$GD/pso_manifest.tsv" $TD/ || die "copy shader tables"   # empty NVN tables: the NVN layer draws nothing
else cp "$GD/shader_map.tsv" "$GD/pso_manifest.tsv" "$GD/c1_full.tsv" $TD/ || die "copy shader tables"
  if [ -f "$GD/cb_extent.tsv" ]; then cp "$GD/cb_extent.tsv" $TD/ || die "copy cb_extent.tsv"; else t "no $GD/cb_extent.tsv: the host copies whole uniform bindings"; fi
  # the shader pairs, beside the shader map where the pipeline prebuild reads them (nvn_mtl.m pso_prebuild): without them the app makes no pulling bases, and a hybrid draw has no stand-in
  for f in shader_pairs.tsv pso_pairs.tsv; do if [ -f "$GD/$f" ]; then cp "$GD/$f" $TD/ || die "copy $f"; fi; done; fi   # optional
# The launcher's settings for this game (Launcher.swift): dm/games/settings.tsv filtered by the profile, main's rewriter table and the game's languages (dm/games/settings.py)
L=$(GFXAPI="$GFXAPI" python3 "$DM/games/settings.py" "$GAME" "$PATCHED" "$VNACP" $TD/launcher.json) || die "launcher settings (dm/games/settings.py)"; t "launcher: $L"
}
if [ -n "${PROGRAMS_TSV:-}" ]; then
  : > Gen/dm/programs.txt
  while IFS= read -r row; do prog_set "$row"; TD=Gen/dm/p$PN; mkdir -p "$TD" || die "mkdir $TD"; VNACP=$(dirname "$ROMFS")/control.nacp; [ "$PN" != 0 ] || VNACP=${CONTROL:-$VNACP}; program_tables; echo "$PN" >> Gen/dm/programs.txt; done < <(prog_rows)
  prog_back
  LJ=Gen/dm/p0/launcher.json
else TD=Gen/dm; program_tables; LJ=Gen/dm/launcher.json; fi
# The pipeline archive made offline (dm/gfx/tools/pso_offline.py): the base of every shader pair, compiled here by metal-tt for each GPU architecture in DM_PSO_GPUS (comma-separated:
# g18p = A19 / A19 Pro; another family's device has its own architecture, applegpu_g19p = the family after the A19 (its archive compiles with the current toolchain); none = no archive), so the device
# does not build them on its first start (Breath of the Wild: 15,767 bases, 7-8 minutes on an iPhone 17 Pro Max).
# Only the Metal 4 path reads it, which iOS turns on for a game whose profile sets pso_mtl4 on: the default is what the profile's pso_gpu key names (whitespace-separated architectures, joined for
# pso_offline.py's --gpu), else g18p, and none for the games that do not set pso_mtl4 on (their app would carry it unread). It goes into the app as pso/ (main.m hands it to the host as
# DM_PSO_BUNDLE), with the pulling vertex libraries the bases are made of. Each architecture adds about 650 MB to the app (Breath of the Wild; 220 MB in the IPA) and 4 minutes to the first build;
# made again only when its inputs change (the libraries, the pairs, the profile's depth_mode, metal-tt)
PSO=; P4=$(python3 "$DM/games/game.py" "$GAME" pso_mtl4 off) || die "game profile $GD: pso_mtl4"; PG=$(python3 "$DM/games/game.py" "$GAME" pso_gpu '') || die "game profile $GD: pso_gpu"
PGK=g18p; N=0; for w in $PG; do case $w in none) N=9; break ;; esac; case ${w#applegpu_} in '' | *[!A-Za-z0-9]*) N=9; break ;; esac; N=$((N+1)); done
if [ "$P4" = on ] && [ "$PG" ]; then case $N in 1 | 2 | 3 | 4) PGK=$(echo "$PG" | tr -s ' \t' ',') ;; *) t "profile $GD: pso_gpu '$PG' is not 1-4 whitespace-separated GPU architectures (each one such as g18p or applegpu_g19p, with or without the applegpu_ prefix; there is no none: without the key the archive defaults follow pso_mtl4) - taking g18p" ;; esac; fi
if [ -n "${DM_PSO_GPUS+set}" ]; then GPUS=$DM_PSO_GPUS; elif [ "$P4" = on ]; then GPUS=$PGK; else GPUS=none; fi
if [ "$GFXAPI" = nvn ] && [ -n "$GPUS" ] && [ "$GPUS" != none ] && [ -f "$GD/shader_pairs.tsv" ]; then
  PSO=$W/build/ios/pso/bundle
  t "pipeline archive for $GPUS (metal-tt; minutes when its inputs changed)"
  python3 "$DM/gfx/tools/pso_offline.py" --game "$GAME" --msl "$MSL_IOS/b-$V" --out "$PSO" --work "$W/build/ios/pso/work" --gpu "$GPUS" ${DM_PSO_IOS_MIN:+--os-min "$DM_PSO_IOS_MIN"} \
    | while IFS= read -r ln; do t "$ln"; done
  if [ "${PIPESTATUS[0]}" != 0 ]; then   # an optimization: the device builds the bases on its first start without it
    t "[RED] pipeline archive: pso_offline.py failed (its output is above); the app is built without one and the device builds its pipeline bases on its first start (DM_PSO_GPUS=none skips this step)"
    rm -rf "$PSO"; PSO=
  fi
else [ "$GFXAPI" != nvn ] || t "no pipeline archive ($([ -n "${DM_PSO_GPUS+set}" ] && echo "DM_PSO_GPUS=${GPUS:-empty}" || echo "the profile does not set pso_mtl4 on; DM_PSO_GPUS=g18p builds one")$([ -f "$GD/shader_pairs.tsv" ] || echo ', no shader_pairs.tsv'))"; fi
# The packed language (DMLanguage) is the Game language row's default, which settings.py makes the game's first language when it lacks DM_LANG: the screen and DM_LAUNCHER=0 then agree
LD=$(python3 -c 'import json, sys; print(next((s["default"] for s in json.load(open(sys.argv[1]))["settings"] if s["env"] == "DM_LANG"), ""))' $LJ) || die "launcher settings ($LJ)"
if [ -n "$LD" ] && [ "$LD" != "$DM_LANG" ]; then t "game language: the game has no $DM_LANG; packing $LD, its first"; export DM_LANG=$LD; fi
# Diagnostic build: DM_ENV_FILE=file → dm/env.txt in the bundle (main.m sets one environment variable per line at boot)
if [ -n "${DM_ENV_FILE:-}" ]; then cp "$DM_ENV_FILE" Gen/dm/env.txt || die "copy DM_ENV_FILE"; t "env: $(grep -v '^#' "$DM_ENV_FILE" | tr '\n' ' ')"; fi
# The shader libraries go into the archive as DmIOS.app/msl/<folder>, or an NVN game's as the file DmIOS.app/msl/<folder>.img (main.m sets DM_MSL_DIR to msl)
if [ "$GFXAPI" = vulkan ]; then MSLDIR=vk; elif [ "$GFXAPI" = gl ]; then MSLDIR=gl; else MSLDIR=b-$V; fi
# Shared-font stand-ins: standard = macOS's Arial Unicode (absent on iOS), Nintendo extension = button icons generated by make_ext_font.py (needs fontTools; FONT_PY names a python that has it)
mkdir -p Gen/dm/font && cp "${STD_FONT:-/System/Library/Fonts/Supplemental/Arial Unicode.ttf}" Gen/dm/font/standard.ttf || die "copy standard font (STD_FONT=a TTF)"
"${FONT_PY:-python3}" "$DM/tools/make_ext_font.py" Gen/dm/font/nintendo_ext.ttf > /dev/null || die "make_ext_font.py (pip install fonttools, or FONT_PY=python with fontTools)"
# App icon: the game's own icon (icon.jpg saved from the control data by nx_extract, the English 256x256 one). DM_ICON can point at another image; by default the icon.jpg next to the RomFS is used.
# It is scaled to a 1024x1024 PNG in the asset catalog Gen/Assets.xcassets (a single-size app icon; actool generates the other sizes); with neither, no icon is set and the home-screen icon is blank.
# The catalog is always created (the project always lists it, see project.yml); without an icon it stays empty and DM_APPICON is not set
ICON=${DM_ICON:-$(dirname "$ROMFS")/icon.jpg}
mkdir -p Gen/Assets.xcassets && printf '{"info":{"author":"xcode","version":1}}\n' > Gen/Assets.xcassets/Contents.json || die "asset catalog"
if [ -f "$ICON" ]; then
  AI=Gen/Assets.xcassets/AppIcon.appiconset; mkdir -p "$AI" || die "mkdir $AI"
  sips -s format png -z 1024 1024 "$ICON" --out "$AI/icon.png" > /dev/null || die "sips: cannot convert the icon $ICON"
  printf '{"images":[{"filename":"icon.png","idiom":"universal","platform":"ios","size":"1024x1024"}],"info":{"author":"xcode","version":1}}\n' > "$AI/Contents.json"
  echo "DM_APPICON = AppIcon" >> Gen/dm.xcconfig
  t "app icon: $ICON"
else t "no app icon ($ICON not found): the home-screen icon stays blank"; fi
# A prepared Xcode project is signed in Xcode, so it asks for the Increased Memory Limit capability the bigger games need (see the README); DM_ENTITLEMENTS=another plist, or empty for none.
# The unsigned IPA carries the same request in its executable's ad-hoc signature below; the install tool replaces that signature.
if [ "$PREPARE_XCODE" = YES ] && [ -n "${DM_ENTITLEMENTS-default}" ]; then
  if [ -n "${DM_ENTITLEMENTS:-}" ]; then cp "$DM_ENTITLEMENTS" Gen/DmIOS.entitlements || die "copy DM_ENTITLEMENTS"
  else printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
    '<plist version="1.0"><dict><key>com.apple.developer.kernel.increased-memory-limit</key><true/></dict></plist>' > Gen/DmIOS.entitlements || die "entitlements"; fi
  echo "CODE_SIGN_ENTITLEMENTS = Gen/DmIOS.entitlements" >> Gen/dm.xcconfig
  t "entitlements: $(plutil -convert json -o - Gen/DmIOS.entitlements)"
fi
# DM_COPY_ROMFS=NO also for the prepared project: its Copy game resources phase (stage_resources.sh) then leaves the game files out and writes the image's identity; Gen/dm.xcconfig is made again by every run
[ "${DM_COPY_ROMFS:-YES}" != NO ] || [ "$PREPARE_XCODE" != YES ] || echo "DM_COPY_ROMFS = NO" >> Gen/dm.xcconfig
xcodegen generate -q > /dev/null || die "xcodegen"
# DMCommit / DMChain: the checkout that built the app (+dirty with uncommitted changes to tracked files) and the shader chain it carries, which the host logs at start (main.c)
COMMIT=$(git -C "$DM" rev-parse --short=12 HEAD 2> /dev/null) && { [ -z "$(git -C "$DM" status --porcelain --untracked-files=no 2> /dev/null)" ] || COMMIT=$COMMIT+dirty
  /usr/libexec/PlistBuddy -c "Add :DMCommit string $COMMIT" Gen/Info.plist > /dev/null || die "DMCommit"; t "source: $COMMIT"; }
case "$MSLDIR" in b-*) /usr/libexec/PlistBuddy -c "Add :DMChain string ${MSLDIR#b-}" Gen/Info.plist > /dev/null || die "DMChain";; esac
# DM_METAL_CAPTURE=1: a development build that may write GPU frame captures (MetalCaptureEnabled; the host's DM_GPU_CAPTURE switch writes them to Documents/gpu_captures)
if [ "${DM_METAL_CAPTURE:-0}" = 1 ]; then /usr/libexec/PlistBuddy -c "Add :MetalCaptureEnabled bool true" Gen/Info.plist > /dev/null || die "MetalCaptureEnabled"; t "Metal frame capture: enabled (DM_METAL_CAPTURE=1)"; fi
RP=; if [ -n "${PROGRAMS_TSV:-}" ]; then   # a game of several programs: each program's files, which the Xcode build phase copies under p<index> as the IPA has them (stage_resources.sh)
  RP=Gen/resources_programs.tsv; printf '# program\tRomFS\tmarker\tiOS shaders\tshader folder (build_ipa.sh)\n' > "$RP"
  M0=$MSLDIR; while IFS= read -r row; do prog_set "$row"; printf '%s\t%s\t%s\t%s\t%s\n' "$PN" "$ROMFS" "$MARKER" "$(lib_path "$MSL_IOS/$MSLDIR")" "$MSLDIR" >> "$RP"; done < <(prog_rows); prog_back; MSLDIR=$M0; fi
DM_RESOURCE_BUILD=${PKG:-} GAME=$GAME python3 tools/gen_resources.py "$IOS" "$ROMFS" "$MARKER" "$(lib_path "$MSL_IOS/$MSLDIR")" "$MSLDIR" "$AOC" "$XCODE_DD" "$DM/gfx/nvn/dmsh_chain.h" "$PSO" $RP || die "generate Xcode resource paths"
if [ "$PREPARE_XCODE" = YES ]; then
  t "Xcode project: $IOS/DmIOS.xcodeproj"
  t "Xcode derived data: $XCODE_DD"
  t "scheme: $DM_SCHEME_NAME"
  t "ready: select your Team in Signing & Capabilities, select a physical device, and Run (Release)"
  exit 0
fi

t "xcodebuild (Release, unsigned, iPhone + iPad)"
mkdir -p "$O"
xcodebuild -project DmIOS.xcodeproj -scheme DmIOS -configuration Release -destination 'generic/platform=iOS' -derivedDataPath "$DD" \
  CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY= DM_COPY_GAME_RESOURCES=NO \
  DM_XCODE_OUTPUT_ROOT="$DD" SYMROOT="$DD/Build/Products" OBJROOT="$DD/Build/Intermediates.noindex" build > "$O/build.log" 2>&1 \
  || { grep -E 'error:' "$O/build.log" | head -20; die "xcodebuild, log $O/build.log"; }
# SYMROOT and OBJROOT above keep the unsigned build separate from the interactive Xcode app.
APP=$DD/Build/Products/Release-iphoneos/DmIOS.app
[ -n "$APP" ] && [ -d "$APP" ] || die "no built app"; t "app: $APP"

# Straight from the sources: the app without its signature, and without the folders an app built before pack_ipa.py still has (DerivedData keeps resources that left the project;
# msl/ is also deleted there, it took 1.5 GB of the home disk), then the shader libraries, the add-on content (the RomFS extracted from the player's DLC NSPs, one folder per index:
# the app sets DM_AOC_DIR at launch, the host reports them as owned and mounts them from it) and the RomFS
rm -rf "$APP/msl" "$O/Payload"   # Payload/: what the copy-and-zip packing left behind, as was out.ipa (and its index) unless it is the IPA packed here, which a repeat pack reuses
[ "$O/out.ipa" -ef "$OUT_IPA" ] || rm -f "$O/out.ipa"; [ "$O/out.ipa.idx" -ef "$OUT_IPA.idx" ] || rm -f "$O/out.ipa.idx"
# The entitlements of an unsigned IPA: an unsigned build has no signature for a sideloader to read them from, so the Increased Memory Limit request would be lost (AltStore and others carry over
# the entitlements of the signature they replace; the Xcode project gets them through CODE_SIGN_ENTITLEMENTS, above). The executable gets an ad-hoc signature that carries them: the sideloader
# replaces the signature and keeps the entitlements. Only the executable is signed: the bundle's resources are packed from elsewhere and the signature is replaced anyway
if [ -n "${DM_ENTITLEMENTS-default}" ]; then
  ENT=$O/DmIOS.entitlements
  if [ -n "${DM_ENTITLEMENTS:-}" ]; then cp "$DM_ENTITLEMENTS" "$ENT" || die "copy DM_ENTITLEMENTS"
  else printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
    '<plist version="1.0"><dict><key>com.apple.developer.kernel.increased-memory-limit</key><true/></dict></plist>' > "$ENT" || die "entitlements"; fi
  EXE=$APP/DmIOS; [ ! -f "$APP/Info.plist" ] || EXE=$APP/$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$APP/Info.plist") || die "CFBundleExecutable of $APP"
  xattr -cr "$EXE" 2> /dev/null   # codesign refuses a file with a resource fork, Finder information or quarantine attributes
  codesign --force --sign - --entitlements "$ENT" "$EXE" || die "ad-hoc signature with the entitlements"
  codesign -d --entitlements - "$EXE" > /dev/null 2>&1 || die "the executable's signature has no entitlements"
  t "entitlements in the executable's ad-hoc signature (the install tool re-signs and keeps them)"
fi
P=Payload/DmIOS.app
SKIP=(--skip $P/_CodeSignature --skip $P/embedded.mobileprovision --skip $P/msl --skip $P/aoc --skip $P/romfs --skip $P/romfs.img --skip $P/romfs.id --skip $P/html --skip $P/jit)
if [ -n "${PROGRAMS_TSV:-}" ]; then   # each program's shader libraries, RomFS, offline HTML pages and nn::jit image under its own folder
  [ "${DM_COPY_ROMFS:-YES}" != NO ] || die "DM_COPY_ROMFS=NO: a game of several programs keeps its programs' files in the app"
  MAPS=("$P=$APP")
  if [ -n "$AOC" ] && [ -d "$AOC" ]; then MAPS+=("$P/aoc=$AOC"); fi
  while IFS= read -r row; do prog_set "$row"; SKIP+=(--skip "$P/p$PN")
    if [ -f "$ROMFS" ]; then MAPS+=("$P/p$PN/romfs.img=$ROMFS"); else MAPS+=("$P/p$PN/romfs=$ROMFS"); fi
    MAPS+=("$P/msl/p$PN/$MSLDIR=$MSL_IOS/$MSLDIR")
    if [ -d "$(dirname "$ROMFS")/html" ]; then MAPS+=("$P/p$PN/html=$(dirname "$ROMFS")/html"); fi
    if [ -d "$(dirname "$ROMFS")/jit" ]; then MAPS+=("$P/p$PN/jit=$(dirname "$ROMFS")/jit"); t "program $PN: nn::jit image $(dirname "$ROMFS")/jit"; fi   # beside its RomFS (loader.c dm_jit_serve_dir)
  done < <(prog_rows); prog_back
else
MAPS=()
if [ "${DM_COPY_ROMFS:-YES}" = NO ]; then
  t "no game files in the IPA (DM_COPY_ROMFS=NO): the app takes Documents/romfs.img (scripts/push_romfs.sh)"
  # The image's identity (dm/host/romfs_identity.h) goes into the app as romfs.id: main.m then uses only a Documents/romfs.img with the same size and index hash, and says so when it is another version or missing.
  # A RomFS folder has no image to name yet (push_romfs.sh packs it on the Mac), so such a build carries no identity and accepts any complete image
  if [ -f "$ROMFS" ]; then
    mkdir -p "$O" && python3 -I "$DM/tools/romfs_image.py" --identity "$ROMFS" "$GAME" "$DM_APP_VERSION" "${PKG:-}" > "$O/romfs.id" || die "identity of $ROMFS"
    MAPS=("$P/romfs.id=$O/romfs.id"); t "game files identity: $(cat "$O/romfs.id")"
  fi
elif [ -f "$ROMFS" ]; then MAPS=("$P/romfs.img=$ROMFS"); else MAPS=("$P/romfs=$ROMFS"); fi
if [ -n "$AOC" ] && [ -d "$AOC" ]; then MAPS+=("$P/aoc=$AOC"); t "add-on content: $(ls "$AOC" | tr '\n' ' ')"; fi
if [ -n "${PSO:-}" ]; then MAPS+=("$P/pso=$PSO"); fi
MAPS+=("$P/msl/$MSLDIR=$MSL_IOS/$MSLDIR" "$P=$APP")
  if [ -d "$(dirname "$ROMFS")/html" ]; then MAPS+=("$P/html=$(dirname "$ROMFS")/html"); t "offline HTML pages: $(dirname "$ROMFS")/html"; fi   # beside its RomFS, where the host finds them (hle_web.c)
  if [ -d "$(dirname "$ROMFS")/jit" ]; then MAPS+=("$P/jit=$(dirname "$ROMFS")/jit"); t "nn::jit image: $(dirname "$ROMFS")/jit"; fi
fi
t "pack IPA (already-compressed game files stored, everything else deflated)"
# The pulling vertex libraries (<key>_bp, dm/gfx/tools/dmsh_pull.py) serve the hybrid pipelines. Default to the same pso_mtl4 setting compiled by gen_build.py; IOS_PULL=0 / 1 overrides it.
# The pipeline archive's bases are made of them, so an IPA with an archive (pso/) always carries them, whatever IOS_PULL says (an Xcode build copies them always).
if [ -n "${PROGRAMS_TSV:-}" ]; then PULL=0   # no program of such a game sets pso_mtl4 so far (the macro is a table's there, not a number)
else PULL=$(sed -n 's/^#define DM_GAME_PSO_MTL4 //p' "$DM/host/gen/$GAME/game_profile.h") || die "read game profile header"
case "$PULL" in 0|1) ;; *) die "no DM_GAME_PSO_MTL4 in generated game profile";; esac; fi
# Keep this array nonempty: macOS Bash 3.2 treats an empty array as unset under set -u.
ZS=$(gp zip_store); [ -z "${PROGRAMS_TSV:-}" ] || ZS=$(for g in $(prog_rows | cut -f2); do python3 "$DM/games/game.py" "$g" zip_store; done | tr ':' '\n' | sort -u | paste -sd: -)   # every program's
PACK_OPTS=(--store "$ZS"); [ -n "${PSO:-}" ] || [ "${IOS_PULL:-$PULL}" = 1 ] || PACK_OPTS+=(--skip-suffix _bp.metallib)
python3 tools/pack_ipa.py "$OUT_IPA" "${PACK_OPTS[@]}" "${SKIP[@]}" "${MAPS[@]}" || die "pack_ipa"
t "done: $OUT_IPA ($(du -h "$OUT_IPA" | cut -f1))"
