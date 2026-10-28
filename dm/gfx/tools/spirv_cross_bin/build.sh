#!/bin/sh
# Builds the SPIR-V to MSL converter of the shader translation chain (and of the Vulkan and OpenGL layers' shader tools), from a pinned
# upstream commit, so every Mac runs the same build and its fingerprint is the one dmsh_chain.lock records (a Homebrew build is compiled from source on macOS versions without a
# bottle, and its fingerprint never matches even at the same version: GitHub issue #242).
# The build is reproducible: the same commit, options and Xcode give the same bytes in any folder (the embedded --revision timestamp is the commit's time in UTC, SOURCE_DATE_EPOCH).
# On macOS it builds a universal binary (arm64 and x86_64); on Linux it builds for the host architecture. SPIRV-Cross is Apache-2.0 (some headers MIT / Khronos): its LICENSE and LICENSES/
# are copied beside the binary.
# After changing the pin: run this, then dmsh_chain.py --update-lock and --header (a chain change: rebuild the shaders), and check that the MSL is the same as before for the shaders of
# a game (or say why it changes).
# Usage: build.sh [work folder] [output folder]   (default output: beside this script)
set -eu
TAG=vulkan-sdk-1.4.363.0                          # the newest SPIRV-Cross release on 2026-10-26, the version Homebrew ships (1.4.363.0)
COMMIT=f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7   # what TAG names; the build stops when it names another
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-$(mktemp -d -t spirv-cross)}
OUT=${2:-$HERE}
mkdir -p "$WORK"
mkdir -p "$OUT"
rm -rf "$WORK/src" "$WORK/build"
git clone --quiet --depth 1 --branch "$TAG" https://github.com/KhronosGroup/SPIRV-Cross.git "$WORK/src"
got=$(git -C "$WORK/src" rev-parse HEAD)
[ "$got" = "$COMMIT" ] || { echo "[RED] $TAG is commit $got, not the pinned $COMMIT" >&2; exit 1; }
export SOURCE_DATE_EPOCH="$(git -C "$WORK/src" log -1 --format=%ct)" TZ=UTC
common="-DCMAKE_BUILD_TYPE=Release -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_SKIP_INSTALL=ON"
if [ "$(uname -s)" = Darwin ]; then
    cmake -S "$WORK/src" -B "$WORK/build" $common -DCMAKE_OSX_ARCHITECTURES='arm64;x86_64' -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DCMAKE_EXE_LINKER_FLAGS='-Wl,-S,-x' > "$WORK/configure.log"
    jobs=$(sysctl -n hw.ncpu)
    platform="$(xcodebuild -version | head -1)"
else
    cmake -S "$WORK/src" -B "$WORK/build" -G Ninja $common -DCMAKE_EXE_LINKER_FLAGS='-Wl,-s' > "$WORK/configure.log"
    jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
    platform="$(uname -s)-$(uname -m)"
fi
cmake --build "$WORK/build" --target spirv-cross -j "$jobs" > "$WORK/build.log"
cp "$WORK/build/spirv-cross" "$OUT/spirv-cross"
cp "$WORK/src/LICENSE" "$OUT/LICENSE"
rm -rf "$OUT/LICENSES"; cp -R "$WORK/src/LICENSES" "$OUT/LICENSES"
if command -v sha256sum >/dev/null 2>&1; then hash=$(sha256sum "$OUT/spirv-cross" | cut -d' ' -f1); else hash=$(shasum -a 256 "$OUT/spirv-cross" | cut -d' ' -f1); fi
echo "spirv-cross $TAG ($COMMIT), $platform: sha256 $hash"
"$OUT/spirv-cross" --revision
