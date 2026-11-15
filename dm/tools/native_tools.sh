#!/bin/sh
# Builds the repository's two C# tools as native executables (NativeAOT), universal (arm64 and x86_64) as spirv-cross is, so running them needs no .NET (GitHub issue #1853);
# building them needs the .NET SDK 10:
#   dmsh_xlate → dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate   the shader translator (Ryujinx's shader library, dm/third_party/ryujinx), part of the translation chain
#   nx_extract → dm/tools/nx_extract_bin/nx_extract        NSP extraction (LibHac)
# Each embeds its own sources and prints their digest with --source-sha: tool_lock.py refuses a translator, and the NSP script an nx_extract, not built from the sources in the repository.
# The build is not byte-reproducible (the linker gives each build its own UUID): a rebuild of the same sources has another fingerprint. So rebuild only after changing a tool's sources, then:
#   dmsh_xlate: tool_lock.py --update-lock and dmsh_chain.py --header, commit the binary with the lock and the header (a chain change: rebuild the shaders), and check that the translator's
#               output is the same as before for the shaders of a game (or say why it changes);
#   nx_extract: commit the binary with the sources.
# Usage: native_tools.sh [dmsh_xlate] [nx_extract]   (default: both)   Environment: WORK=folder for the intermediate files (default: a temporary folder)
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${WORK:-$(mktemp -d -t native_tools)}
mkdir -p "$WORK"
command -v dotnet > /dev/null || { echo '[RED] building the native tools needs the .NET SDK 10 (brew install --cask dotnet-sdk); running them does not' >&2; exit 1; }

build() {   # <name> <csproj> <output executable>
    for rid in osx-arm64 osx-x64; do   # NativeAOT compiles one architecture per publish; the SDK cross-compiles the other
        dotnet publish "$2" -c Release -r "$rid" -nologo -v quiet --artifacts-path "$WORK/$1-artifacts" -o "$WORK/$1-$rid" > "$WORK/$1-$rid.log" 2>&1 \
            || { tail -20 "$WORK/$1-$rid.log" >&2; echo "[RED] $1 did not build for $rid (log: $WORK/$1-$rid.log)" >&2; exit 1; }
        # A weak import is a libSystem function newer than the executable's minimum macOS: it binds to 0 on a Mac without it, and the runtime calls it unchecked. A .NET whose native
        # libraries were configured against the current SDK (a source-built one, such as Homebrew's formula) gives these; Microsoft's SDK (the cask) does not (GitHub issue #2190: pipe2)
        weak=$(nm -m -u "$WORK/$1-$rid/$1" | grep -w weak || true)
        [ -z "$weak" ] || { echo "$weak" >&2; echo "[RED] $1 ($rid) imports functions its minimum macOS does not have, and would crash at address 0 there: build with Microsoft's .NET SDK (brew install --cask dotnet-sdk), not a source-built one" >&2; exit 1; }
    done
    lipo -create "$WORK/$1-osx-arm64/$1" "$WORK/$1-osx-x64/$1" -output "$3"
    for a in arm64 x86_64; do sha=$(arch -"$a" "$3" --source-sha); echo "$1 ($a): source $sha"; done
    echo "$1: sha256 $(shasum -a 256 "$3" | cut -c1-64), $(wc -c < "$3" | tr -d ' ') bytes"
}

for t in ${*:-dmsh_xlate nx_extract}; do
    case $t in
        dmsh_xlate) build dmsh_xlate "$REPO/dm/gfx/tools/dmsh_xlate/dmsh_xlate.csproj" "$REPO/dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate" ;;
        nx_extract) build nx_extract "$REPO/dm/tools/nx_extract/nx_extract.csproj" "$REPO/dm/tools/nx_extract_bin/nx_extract" ;;
        *) echo "usage: native_tools.sh [dmsh_xlate] [nx_extract]" >&2; exit 2 ;;
    esac
done
