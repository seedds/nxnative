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
# The image goes over AFC (house_arrest, the channel of Finder's file sharing; libimobiledevice's afcclient), which streams it. devicectl's copy has the Mac's CoreDeviceService read
# the whole file into its own memory: a 26 GB image filled a 16 GB Mac's swap, macOS asked to quit applications and killed the service ("The connection was interrupted";
# docs/reference/packing.md). AFC writes Documents/romfs.img.part, which main.m never takes, and renames it once the device holds all of it: afcclient exits 0 even when a
# command fails, so the size on the device decides. ROMFS_PUSH_VIA=afc / devicectl chooses (default: afc when afcclient is installed).
size=$(stat -f %z "$img")
via=${ROMFS_PUSH_VIA:-auto}
if [ "$via" = auto ]; then if command -v afcclient > /dev/null; then via=afc; else via=devicectl; fi; fi
if [ "$via" = afc ]; then
    afc() { afcclient -u "$dev" --container "$id" -- "$@"; }
    remote_size() { afc info "$1" 2> /dev/null | sed -n 's/^ *"st_size": *\([0-9][0-9]*\).*/\1/p'; }
    remote_type() { afc info "$1" 2> /dev/null | sed -n 's/^ *"st_ifmt": *"\([A-Z_]*\)".*/\1/p'; }
    # afcclient finds a device only by its UDID, over USB (devicectl also takes its own identifier or the device's name, and Wi-Fi): check that it opens the app's
    # Documents before copying, so a wrong ID or a Wi-Fi-only device is named as such instead of ending as an incomplete copy.
    [ "$(remote_type /Documents)" = S_IFDIR ] || { echo "[RED] afcclient cannot open Documents of $id on device $dev: it needs the device's UDID (idevice_id -l lists the connected ones; devicectl's identifier and the device's name do not work), a USB connection, and the app installed. ROMFS_PUSH_VIA=devicectl copies through devicectl instead" >&2; exit 1; }
    echo "copying $((size >> 20)) MB over AFC (about 30 MB/s over USB)"
    afc put -f "$img" /Documents/romfs.img.part
    got=$(remote_size /Documents/romfs.img.part)
    [ "$got" = "$size" ] || { echo "[RED] the copy is incomplete: Documents/romfs.img.part holds ${got:-nothing} of $size bytes (a full device, or the cable); Documents/romfs.img is unchanged. Run this again" >&2; exit 1; }
    afc rm /Documents/romfs.img > /dev/null 2>&1 || true
    afc mv /Documents/romfs.img.part /Documents/romfs.img
    got=$(remote_size /Documents/romfs.img)
    [ "$got" = "$size" ] || { echo "[RED] Documents/romfs.img.part was not renamed to romfs.img (romfs.img holds ${got:-nothing} of $size bytes). Run this again" >&2; exit 1; }
elif [ "$via" = devicectl ]; then
    mem=$(sysctl -n hw.memsize)
    if [ -z "${ROMFS_PUSH_VIA:-}" ] && [ "$size" -gt $((mem / 2)) ]; then
        echo "[RED] the image ($((size >> 20)) MB) is larger than half this Mac's memory ($((mem >> 20)) MB): devicectl copies it through memory and can fill the swap. Install afcclient (brew install libimobiledevice) and run this again, or set ROMFS_PUSH_VIA=devicectl to copy it anyway" >&2
        exit 1
    fi
    xcrun devicectl device copy to --device "$dev" --domain-type appDataContainer --domain-identifier "$id" --source "$img" --destination Documents/romfs.img -q
else echo "ROMFS_PUSH_VIA is afc or devicectl, not $via" >&2; exit 1; fi
# Add-on content (DLC): <index>.img files or legacy <index>/ trees. resources.env describes the complete DLC set, so Documents/aoc is replaced, not merged (a merge would keep
# removed DLC that the host still reports owned). An empty set leaves an empty Documents/aoc, which also takes precedence over bundled DLC at startup. Over AFC only: devicectl's
# --remove-existing-content true, given Documents/aoc, empties the app's whole data container, saves included, and then fails with "Failed to retrieve the file node for
# Documents/aoc" (CoreDevice 651.32; docs/reference/packing.md), so devicectl never syncs DLC here.
if [ "$sync_aoc" = 1 ]; then
    if [ "$via" = afc ]; then
        if [ -n "$(remote_type /Documents/aoc)" ]; then afc rm -rf /Documents/aoc < /dev/null > /dev/null; fi
        [ -z "$(remote_type /Documents/aoc)" ] || { echo "[RED] the old Documents/aoc could not be removed; the DLC on the phone is unchanged. Run this again" >&2; exit 1; }
        afc mkdir /Documents/aoc
        missing=
        if [ -n "$DM_RESOURCE_AOC" ]; then
            for f in "$DM_RESOURCE_AOC"/*; do
                [ -e "$f" ] || continue
                n=$(basename "$f"); afc put -r -f "$f" "/Documents/aoc/$n" > /dev/null
                [ -n "$(remote_type "/Documents/aoc/$n")" ] || missing="$missing $n"
            done
        fi
        [ "$(remote_type /Documents/aoc)" = S_IFDIR ] && [ -z "$missing" ] || { echo "[RED] the DLC did not all reach Documents/aoc (missing:${missing:- the folder}). Run this again" >&2; exit 1; }
        echo "DLC on the phone: $id"
    elif [ -n "$DM_RESOURCE_AOC" ]; then
        echo "[RED] the DLC was not copied: devicectl's --remove-existing-content empties the whole app container, saves included, so it needs afcclient (brew install libimobiledevice). The game files were copied" >&2
        exit 1
    else echo "no DLC in this build; Documents/aoc left as it is (afcclient would empty it)"; fi
fi
echo "game files on the phone: $id"
