#!/bin/bash
# One-time setup for packing the iOS app in WSL (Linux on Windows) instead of on a Mac; docs/development.md "Packing on Windows (WSL)" lists what it needs installed first.
# It makes, in DM_WSL_CACHE (default ~/.cache/nxnative-wsl), where the NSP script finds them in WSL:
#   spirv-cross/spirv-cross   the shader chain's SPIRV-Cross, built for Linux from the tag and commit dm/gfx/tools/spirv_cross_bin/build.sh pins (that build is a Mac binary)
#   xlate/dmsh_xlate          the shader translator: the in-repo assemblies (dmsh_xlate_bin, linked, so their fingerprints are the lock's) started through dotnet, as the in-repo
#                             launcher is a macOS executable
# then checks them, the Metal compiler and the iOS SDK that dm/ios/tools/wsl/xcrun reaches. Run it again after a pull that changes the pinned SPIRV-Cross or the translator's files.
# Usage: scripts/setup_wsl.sh
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd); WSL_DIR=$REPO/dm/ios/tools/wsl
CACHE=${DM_WSL_CACHE:-$HOME/.cache/nxnative-wsl}
[ -n "${WSL_DISTRO_NAME:-}" ] || { echo "[RED] not running in WSL: the Metal compiler is Apple's Metal Developer Tools for Windows, reached through WSL's interop" >&2; exit 1; }
for t in git cmake ninja c++ dotnet python3; do command -v "$t" > /dev/null || { echo "[RED] $t not found" >&2; exit 1; }; done
mkdir -p "$CACHE"

B=$REPO/dm/gfx/tools/spirv_cross_bin/build.sh
TAG=$(sed -n 's/^TAG=\([^ ]*\).*/\1/p' "$B"); COMMIT=$(sed -n 's/^COMMIT=\([^ ]*\).*/\1/p' "$B")
SC=$CACHE/spirv-cross
if ! "$SC/spirv-cross" --revision 2>&1 | grep -q "Git commit: $TAG "; then   # --revision prints on stderr
  echo "building spirv-cross $TAG ($COMMIT) for Linux"
  W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
  git clone --quiet --depth 1 --branch "$TAG" -c advice.detachedHead=false https://github.com/KhronosGroup/SPIRV-Cross.git "$W/src"
  got=$(git -C "$W/src" rev-parse HEAD)
  [ "$got" = "$COMMIT" ] || { echo "[RED] $TAG is commit $got, not the pinned $COMMIT" >&2; exit 1; }
  export SOURCE_DATE_EPOCH="$(git -C "$W/src" log -1 --format=%ct)" TZ=UTC   # as build.sh: the embedded --revision timestamp is the commit's
  cmake -S "$W/src" -B "$W/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_SKIP_INSTALL=ON -DCMAKE_EXE_LINKER_FLAGS=-s > "$W/configure.log"
  cmake --build "$W/build" --target spirv-cross > "$W/build.log"
  mkdir -p "$SC"; cp "$W/build/spirv-cross" "$W/src/LICENSE" "$SC/"
fi

X=$CACHE/xlate
rm -rf "$X"; mkdir -p "$X"
for f in "$REPO"/dm/gfx/tools/dmsh_xlate_bin/*; do [ "$(basename "$f")" = dmsh_xlate ] || ln -s "$f" "$X/"; done
printf '%s\n' '#!/bin/sh' '# The shader translator through dotnet (made by scripts/setup_wsl.sh)' 'exec dotnet "$(dirname "$0")/dmsh_xlate.dll" "$@"' > "$X/dmsh_xlate"
chmod +x "$X/dmsh_xlate"

echo "spirv-cross: $("$SC/spirv-cross" --revision 2>&1 | head -1)"
echo "translator: source $("$X/dmsh_xlate" --source-sha) (in the repository: $(cd "$REPO/dm/gfx/tools" && python3 -c 'import dmsh_chain; print(dmsh_chain.xlate_source_sha())'))"
echo "Metal compiler: $("$WSL_DIR/xcrun" -sdk iphoneos metal --version | head -1)"
echo "iOS SDK: $("$WSL_DIR/xcrun" -sdk iphoneos --show-sdk-version) ($("$WSL_DIR/xcrun" -sdk iphoneos --show-sdk-path))"
echo "ready: python3 scripts/build_ipa_from_nsp.py <NSP folder> -k <prod.keys> (and --allow-other-metal while the Windows Metal compiler is not the version dmsh_chain.lock records)"
