#!/bin/bash
# build_spirv_windows.sh <work folder>: spirv-cross.exe for Windows, cross-compiled on a Mac from the commit dm/gfx/tools/spirv_cross_bin/build.sh pins. It needs only Windows' standard C runtime.
set -eu
TAG=vulkan-sdk-1.4.363.0; COMMIT=f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7; W=$1; mkdir -p "$W/gen"
python3 -m pip install --quiet --target "$W/py" ziglang
rm -rf "$W/src"; git clone --quiet --depth 1 --branch "$TAG" https://github.com/KhronosGroup/SPIRV-Cross.git "$W/src"
[ "$(git -C "$W/src" rev-parse HEAD)" = "$COMMIT" ] || { echo "[RED] $TAG is not the pinned $COMMIT" >&2; exit 1; }
printf '#ifndef SPIRV_CROSS_GIT_VERSION_H_\n#define SPIRV_CROSS_GIT_VERSION_H_\n#define SPIRV_CROSS_GIT_REVISION "Git commit: f11ba9f (Windows build)"\n#endif\n' > "$W/gen/gitversion.h"
cd "$W/src"; PYTHONPATH="$W/py" python3 -m ziglang c++ -target x86_64-windows-gnu -O2 -DNDEBUG -std=c++14 -I"$W/gen" -I. -static -s -o "$W/spirv-cross.exe" main.cpp spirv_*.cpp
echo "$W/spirv-cross.exe"
