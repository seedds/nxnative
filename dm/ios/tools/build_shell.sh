#!/bin/bash
# Route C shell (docs/launcher-c-plan.md M2/M3): one iPhone and iPad app for every game the player added. It hosts the engine and
# embeds each game's framework (build_ipa.sh DM_FRAMEWORK_ONLY=<folder>: the game's modules, tables, profile, settings list, shader
# libraries, pipeline archive and icon). The game data stays out: the app takes it from Documents/games/<title ID>/ or a picked folder.
# Usage: dm/ios/tools/build_shell.sh --prepare-xcode | --build
#   --prepare-xcode  generates the project in SHELL_DIR and stops: Xcode (or xcodebuild with a team and a device, as the Mac launcher
#                    does) signs every framework and the app and installs it; nothing is signed or installed here
#   --build          also builds it for iOS, unsigned (CODE_SIGNING_ALLOWED=NO), to check it compiles and links (CI, off a device)
# Environment:
#   SHELL_DIR            the shell's project folder (required; outside dm/ios, on the drive with the games: Gen/, the project, derived data)
#   DM_SHELL_FRAMEWORKS  the games' framework folders, one per line (required, at least one; <folder>/DmGame<title ID>.framework)
#   DM_BUNDLE_ID         the shell's bundle ID (default org.nxnative.shell). Keep it for the life of the install: the same ID installed
#                        over the existing app keeps its Documents (game data, saves) when it is signed again after 7 days
#   DM_TEAM_ID           the signing team for the prepared project (default 0000000000, a placeholder)
#   DM_SHELL_NAME        the name under the icon (default NX2Apple); DM_APP_VERSION the app version (default 1.0)
#   DM_SHELL_PACKAGE     the frontend-ios Swift package (default: frontend-ios beside dm/ in this checkout)
#   DM_XCODE_DERIVED_DATA  derived data (default SHELL_DIR/dd); FONT_PY / STD_FONT as for build_ipa.sh
#   DM_GAMES_DIR         passed through, for a provisional game profile
#   DM_ENTITLEMENTS      empty: no Increased Memory Limit request in the prepared project (default: requested, as build_ipa.sh does)
# The host is compiled from dm/host once, with the first framework's game profile as its compiled defaults (gen_build.py:
# game_profile.h). Every host site reads the runtime profile (dm_profile(), hle_profile.h), which the shell points at the chosen game's
# profile.tsv before the host starts (DM_PROFILE), so one host runs every game of the shell.
set -u
cd "$(dirname "$0")/.." || exit 1
IOS_SOURCE=$PWD; DM=$(cd .. && pwd); REPO=$(cd "$DM/.." && pwd)
t() { echo "[$(date +%T)] $*"; }; die() { t "FAIL: $*"; exit 1; }
MODE=${1:-}; case "$MODE" in --prepare-xcode|--build) ;; *) die "usage: $0 --prepare-xcode | --build";; esac
[ -n "${SHELL_DIR:-}" ] || die "SHELL_DIR is not set: the shell's project folder"
[ -n "${DM_SHELL_FRAMEWORKS:-}" ] || die "DM_SHELL_FRAMEWORKS is not set: the games' framework folders, one per line"
mkdir -p "$SHELL_DIR" || die "mkdir $SHELL_DIR"; S=$(cd "$SHELL_DIR" && pwd)
[ "$S" != "$IOS_SOURCE" ] || die "the shell project must be outside dm/ios"
export DM_SHELL_PACKAGE=${DM_SHELL_PACKAGE:-$REPO/frontend-ios}
[ -f "$DM_SHELL_PACKAGE/Package.swift" ] || die "no Swift package at $DM_SHELL_PACKAGE (DM_SHELL_PACKAGE)"
export LC_ALL=C DM_BUNDLE_ID=${DM_BUNDLE_ID:-org.nxnative.shell} DM_TEAM_ID=${DM_TEAM_ID:-0000000000} DM_SHELL_NAME=${DM_SHELL_NAME:-NX2Apple} DM_APP_VERSION=${DM_APP_VERSION:-1.0}
export DM_FORK_PATCHES=$(paste -sd, "$DM/fork_patches.txt" 2>/dev/null || true)
DD=${DM_XCODE_DERIVED_DATA:-$S/dd}; mkdir -p "$DD" && DD=$(cd "$DD" && pwd)
V=$(sed -n 's/^#define DMSH_CHAIN "\(.*\)"/\1/p' "$DM/gfx/nvn/dmsh_chain.h"); [ -n "$V" ] || die "no DMSH_CHAIN in dm/gfx/nvn/dmsh_chain.h"

# Every framework: a manifest of this chain, its executable, a title ID of its own. The first one's profile is the host's
FWLIST=$S/frameworks.txt
python3 -I - "$V" "$FWLIST" <<'PY' || die "the frameworks in DM_SHELL_FRAMEWORKS (above)"
import json, os, sys
chain, out = 'b-' + sys.argv[1], sys.argv[2]
paths = [p.strip() for p in os.environ['DM_SHELL_FRAMEWORKS'].splitlines() if p.strip()]
if not paths: sys.exit('[RED] no framework named')
seen, rows = {}, []
for p in paths:
    p = os.path.abspath(p)
    try: m = json.load(open(os.path.join(p, 'game.manifest.json'), encoding='utf-8'))
    except (OSError, ValueError) as e: sys.exit(f'[RED] {p}: no readable game.manifest.json ({e})')
    name, exe, tid = m.get('framework'), m.get('framework_executable'), m.get('title_id', '')
    if os.path.basename(p) != name: sys.exit(f'[RED] {p}: the manifest names {name}')
    if not os.path.isfile(os.path.join(p, exe or '-')): sys.exit(f'[RED] {p}: no executable {exe} (build_ipa.sh DM_FRAMEWORK_ONLY builds it)')
    if m.get('chain') != chain: sys.exit(f'[RED] {p}: built for shader chain {m.get("chain")}, this checkout is {chain}: build the game again')
    if tid in seen: sys.exit(f'[RED] {p}: title ID {tid} is already in the shell ({seen[tid]})')
    seen[tid] = p; rows.append((tid, p, m.get('name', '')))
with open(out, 'w', encoding='utf-8') as f:
    for tid, p, name in rows: f.write(f'{tid}\t{p}\t{name}\n')
PY
export GAME=$(head -1 "$FWLIST" | cut -f1)
t "shell: $DM_SHELL_NAME ($DM_BUNDLE_ID), compiled defaults of $GAME, games: $(cut -f3 "$FWLIST" | paste -sd, -)"

t "generate Gen/ (host sources, shared files, the project)"
for item in Sources Shell tools; do ln -sfn "$IOS_SOURCE/$item" "$S/$item" || die "link $item"; done
cd "$S" || die "cd $S"
rm -rf Gen && mkdir -p Gen/dm/font || die "mkdir Gen"
python3 "$IOS_SOURCE/tools/gen_build.py" "$DM/host" Gen > Gen/dm.xcconfig || die "gen_build"
echo 'OTHER_LDFLAGS = $(inherited) $(DM_FRAMEWORKS)' >> Gen/dm.xcconfig
cp "${STD_FONT:-/System/Library/Fonts/Supplemental/Arial Unicode.ttf}" Gen/dm/font/standard.ttf || die "copy standard font (STD_FONT=a TTF)"
"${FONT_PY:-python3}" "$DM/tools/make_ext_font.py" Gen/dm/font/nintendo_ext.ttf > /dev/null || die "make_ext_font.py (pip install fonttools, or FONT_PY=python with fontTools)"
mkdir -p Gen/Assets.xcassets && printf '{"info":{"author":"xcode","version":1}}\n' > Gen/Assets.xcassets/Contents.json || die "asset catalog"
# The shell asks for the Increased Memory Limit capability, as the one-game app does; a framework cannot hold entitlements
printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
  '<plist version="1.0"><dict><key>com.apple.developer.kernel.increased-memory-limit</key><true/></dict></plist>' > Gen/DmShell.entitlements || die "entitlements"
# DM_ENTITLEMENTS= (empty) leaves the request out, for a team whose profiles cannot carry the capability yet (the game may then run
# out of memory on the device)
if [ "$MODE" = --prepare-xcode ] && [ -n "${DM_ENTITLEMENTS-default}" ]; then echo "CODE_SIGN_ENTITLEMENTS = Gen/DmShell.entitlements" >> Gen/dm.xcconfig
elif [ "$MODE" = --prepare-xcode ]; then t "entitlements: none (DM_ENTITLEMENTS is empty): no Increased Memory Limit"; fi
# The template with one embedded, unlinked framework per game where it says @FRAMEWORKS@
python3 -I - "$IOS_SOURCE/project-shell.yml" "$FWLIST" project.yml <<'PY' || die "project.yml"
import sys
template, fwlist, out = sys.argv[1:]
lines, done = [], False
for line in open(template, encoding='utf-8').read().splitlines():
    if line.lstrip().startswith('# @FRAMEWORKS@'):
        pad = line[:len(line) - len(line.lstrip())]
        for row in open(fwlist, encoding='utf-8').read().splitlines():
            path = row.split('\t')[1]
            lines += [f'{pad}- framework: "{path}"', f'{pad}  embed: true', f'{pad}  link: false', f'{pad}  codeSign: true']
        done = True
        continue
    lines.append(line)
if not done: sys.exit('[RED] project-shell.yml has no @FRAMEWORKS@ line')
open(out, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
PY
xcodegen generate -q > /dev/null || die "xcodegen"
COMMIT=$(git -C "$DM" rev-parse --short=12 HEAD 2> /dev/null) && { [ -z "$(git -C "$DM" status --porcelain --untracked-files=no 2> /dev/null)" ] || COMMIT=$COMMIT+dirty
  /usr/libexec/PlistBuddy -c "Add :DMCommit string $COMMIT" Gen/Info.plist > /dev/null || die "DMCommit"; t "source: $COMMIT"; }
/usr/libexec/PlistBuddy -c "Add :DMChain string $V" Gen/Info.plist > /dev/null || die "DMChain"
/usr/libexec/PlistBuddy -c "Add :DMForkPatches string $DM_FORK_PATCHES" Gen/Info.plist > /dev/null || die "DMForkPatches"
if [ "$MODE" = --prepare-xcode ]; then
  t "Xcode project: $S/DmShell.xcodeproj (scheme DmShell)"
  t "Xcode derived data: $DD"
  t "ready: select your Team in Signing & Capabilities and a physical device, then Run (Release); or xcodebuild -scheme DmShell -destination id=<device> DEVELOPMENT_TEAM=<team> -allowProvisioningUpdates"
  exit 0
fi

t "xcodebuild (Release, unsigned, iPhone + iPad)"
xcodebuild -project DmShell.xcodeproj -scheme DmShell -configuration Release -destination 'generic/platform=iOS' -derivedDataPath "$DD" \
  CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY= build > "$S/build.log" 2>&1 \
  || { grep -E 'error:' "$S/build.log" | head -20; die "xcodebuild, log $S/build.log"; }
APP=$DD/Build/Products/Release-iphoneos/DmShell.app
[ -x "$APP/DmShell" ] || die "no built app at $APP"
while IFS=$'\t' read -r tid path name; do
  fw=$APP/Frameworks/$(basename "$path"); [ -d "$fw" ] || die "the app does not embed $(basename "$path")"
done < "$FWLIST"
t "done: $APP ($(du -sh "$APP" | cut -f1)), unsigned"
