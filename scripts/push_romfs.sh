#!/bin/bash
# Copy a game's files to its iPhone app's Documents/romfs.img; resources.env also replaces Documents/aoc with the build's DLC set (see docs/development.md, Quick reinstalls).
# The files go as one RomFS image (dm/tools/romfs_image.py), packed next to the RomFS folder on first use: one file copies at full speed, where a folder of a few hundred
# thousand files ran the phone's file service out of open files (Tears of the Kingdom: 304,000). main.m only takes a complete image, so a copy cut off halfway is never used.
# Usage: push_romfs.sh <device UDID> <bundle ID> <game>_build/xcode/Gen/resources.env [--repack]
#    or: push_romfs.sh <device UDID> <bundle ID> <RomFS.img>   (the image build_ipa_from_nsp.py --romfs separate prints: <game>_build/extract/<version>/RomFS.img)
# A build made with DM_COPY_ROMFS=NO carries the identity of its image (romfs.id); the app uses only a Documents/romfs.img with that identity, so push again whenever the game or its version changed.
# Run it while the app (any build with that bundle ID) is installed. Rerun after a failure, before launching the game. A direct image push leaves DLC unchanged.
set -eu
[ "$#" = 3 ] || { [ "$#" = 4 ] && [ "$4" = --repack ]; } || { echo "usage: $0 <device UDID> <bundle ID> <resources.env> [--repack]" >&2; exit 1; }
dev=$1 id=$2 sync_aoc=0
case "$3" in
    *.env) sync_aoc=1; DM_RESOURCE_AOC=; source "$3";;
    *) DM_RESOURCE_ROMFS=$3; DM_RESOURCE_MARKER=;;
esac
if [ "$sync_aoc" = 1 ] && [ -n "$DM_RESOURCE_AOC" ] && [ ! -d "$DM_RESOURCE_AOC" ]; then
    echo "[RED] Add-on content unavailable: $DM_RESOURCE_AOC" >&2; exit 1
fi
if [ -f "$DM_RESOURCE_ROMFS" ]; then img=$DM_RESOURCE_ROMFS
else
    img=${DM_RESOURCE_ROMFS%/}.img
    repack=${4:-}; set -- "$DM_RESOURCE_ROMFS" "$img"
    [ "$repack" = --repack ] || set -- --reuse "$@"
    python3 -I "$(dirname "$0")/../dm/tools/romfs_image.py" "$@"
fi
python3 -I "$(dirname "$0")/../dm/tools/romfs.py" "$img" "${DM_RESOURCE_MARKER:-}"
echo "identity: $(python3 -I "$(dirname "$0")/../dm/tools/romfs_image.py" --identity "$img")"
xcrun devicectl device copy to --device "$dev" --domain-type appDataContainer --domain-identifier "$id" --source "$img" --destination Documents/romfs.img -q
# Add-on content (DLC): <index>.img files or legacy <index>/ trees. devicectl merges directories by default, retaining removed DLC that the host would still report owned.
# resources.env describes the complete DLC set. An empty set replaces old content with an empty directory, which also takes precedence over bundled DLC at startup.
if [ "$sync_aoc" = 1 ]; then
    aoc=$DM_RESOURCE_AOC
    if [ -z "$aoc" ]; then
        aoc=$(mktemp -d "${TMPDIR:-/tmp}/nxnative-aoc.XXXXXX")
        trap 'rmdir "$aoc"' EXIT
    fi
    xcrun devicectl device copy to --device "$dev" --domain-type appDataContainer --domain-identifier "$id" --source "$aoc" --destination Documents/aoc --remove-existing-content true -q
    echo "DLC on the phone: $id"
fi
echo "game files on the phone: $id"
