#!/bin/sh
# The licences of what llvm-mingw links statically into every Windows program it builds (-static: nxhost-run.exe, nxlauncher.exe, nxaot.exe, the SPIR-V tools), copied
# from the toolchain itself into <tree>/licenses/, where scripts/package_port.py takes a build tree's licences from (docs/development.md, "The port's bundles"):
#   llvm-mingw-LLVM.txt         the LLVM project's LICENSE.TXT (Apache 2.0 with LLVM exceptions): libc++, libc++abi, libunwind and compiler-rt's builtins
#   llvm-mingw-mingw-w64.txt    mingw-w64's COPYING.MinGW-w64-runtime.txt: the CRT and the other runtime libraries, "statically linked against the MinGW-w64 runtime"
#   llvm-mingw-winpthreads.txt  mingw-w64's COPYING.winpthreads.txt: winpthreads (the threads of libc++)
# Run in the llvm-mingw image the tree was built with (mstorsjo/llvm-mingw:20261020: LLVM 23.1.4, mingw-w64 15.0.0), after the build:
#   docker run --rm -u $(id -u):$(id -g) -v "$PWD":/src -w /src mstorsjo/llvm-mingw:20261020 sh dm/tools/mingw_licences.sh build/win64
# The toolchain is found from the x86_64-w64-mingw32-clang on PATH; a missing file stops it with its name (exit 1)
set -eu
[ $# -eq 1 ] || { echo "usage: mingw_licences.sh <build tree>" >&2; exit 2; }
cc=$(command -v x86_64-w64-mingw32-clang) || { echo "[RED] mingw_licences.sh: no x86_64-w64-mingw32-clang on PATH: run it in the llvm-mingw image" >&2; exit 1; }
prefix=$(cd "$(dirname "$(readlink -f "$cc")")/.." && pwd)
mkdir -p "$1/licenses"
for pair in "LICENSE.TXT llvm-mingw-LLVM.txt" "x86_64-w64-mingw32/share/mingw32/COPYING.MinGW-w64-runtime.txt llvm-mingw-mingw-w64.txt" \
            "x86_64-w64-mingw32/share/mingw32/COPYING.winpthreads.txt llvm-mingw-winpthreads.txt"; do
    set -- "$1" $pair
    [ -f "$prefix/$2" ] || { echo "[RED] mingw_licences.sh: $prefix/$2 is missing: not the llvm-mingw layout this script knows" >&2; exit 1; }
    cp "$prefix/$2" "$1/licenses/$3"
    echo "mingw_licences.sh: $1/licenses/$3 from $prefix/$2"
done
