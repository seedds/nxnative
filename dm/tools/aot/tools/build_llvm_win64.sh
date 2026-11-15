#!/bin/sh
# LLVM 18.1.3 and lld as static libraries for Windows x86-64 (x86_64-w64-mingw32), the LLVM that nxaot.exe links (dm/tools/aot/README.md, "Building nxaot for
# Windows"). There is no prebuilt LLVM for llvm-mingw, so it is cross-built here from the official source tarballs of the llvm-project release llvmorg-18.1.3,
# the version nxaot.lock pins. Run it inside the llvm-mingw image of docker/README.md, one container and no other image (the image has llvm-mingw's cross
# clang in /opt/llvm-mingw/bin, and gcc, CMake, Ninja, Python 3, curl and xz, which the native step and the download use). From the repository root:
#   docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src:ro" -v <work folder>:/work mstorsjo/llvm-mingw:20261020 \
#     sh /src/dm/tools/aot/tools/build_llvm_win64.sh [-j N] [--keep] /work
# The work folder holds the downloads (dl/), the sources (src/), the build trees (build-native/, build-cross/) and the prefix,
# <work>/llvm-18.1.3-x86_64-w64-mingw32, whose path is the last line printed. Steps, each skipped when its output is there:
#   1. the source tarballs of llvm, lld and cmake from the release into <work>/dl, each checked against its SHA-256 below (the release's GPG signatures
#      of those files, by the LLVM release key 474E22316ABF4785A88C6E8EA2C794A986419D8A, were checked when the hashes were written down)
#   2. what the build reads extracted into <work>/src/llvm-project/{llvm,lld,cmake}: not LLVM's tests, unit tests, benchmarks or documentation; one line of
#      lld's CMake changed (an install target for its static libraries, below)
#   3. llvm-tblgen and llvm-min-tblgen for the machine running the build (gcc) into <work>/build-native: the cross build runs them to generate its sources
#   4. LLVM with lld in its tree for x86_64-w64-mingw32 (llvm-mingw's clang through dm/host/cmake/x86_64-w64-mingw32.cmake, as the Windows host is built),
#      Release without assertions, the X86 target only, static libraries, no zlib, zstd, libxml2 or terminfo, RTTI on as in the Ubuntu LLVM the Linux nxaot
#      links; only the libraries nxaot links and what they need (LLVM_DISTRIBUTION_COMPONENTS below), no tool, test, example or benchmark, none of lld's
#      MachO, wasm or MinGW drivers; installed into the prefix with LLVM's and lld's CMake packages, lld's headers copied beside LLVM's
#   5. the build trees and the extracted sources removed (--keep keeps them); the downloads stay
# A finished prefix is reused when its stamp (.nxaot-prefix: the release, the components and the options) matches: the script then only prints it. Then
# configure nxaot with -DCMAKE_TOOLCHAIN_FILE=<repo>/dm/host/cmake/x86_64-w64-mingw32.cmake -DLLVM_DIR=<prefix>/lib/cmake/llvm -DLLD_DIR=<prefix>/lib/cmake/lld.
# Resources, measured on 2026-11-15 on 8 vCPUs at -j 4 with another -j 4 build running beside it: the work folder peaks at 0.85 GB (the cross build tree
# 394 MB, its objects and archives; the sources 160 MB; the prefix 197 MB; the downloads 61 MB; the native tablegens 40 MB) and ends at 0.26 GB (the
# downloads and the prefix); the machine's available memory stayed above 6.5 GB of 11.5; about 20 minutes of wall time (the native tablegens 2, the cross
# build's 1,767 steps and its install 17). -j N sets the compile jobs (default 4).
# Exit status: 0 with the prefix printed, 1 on a failure (a download, a hash, too little space, a build step), 2 on a usage error.
set -eu

usage() {
    echo "usage: build_llvm_win64.sh [-j N] [--keep] <work folder>" >&2
    exit 2
}
jobs=4
keep=0
work=
while [ $# -gt 0 ]; do
    case $1 in
        -j) [ $# -ge 2 ] || usage; jobs=$2; shift 2 ;;
        -j*) jobs=${1#-j}; shift ;;
        --keep) keep=1; shift ;;
        -h|--help) usage ;;
        -*) echo "build_llvm_win64.sh: unknown option $1" >&2; usage ;;
        *) [ -z "$work" ] || usage; work=$1; shift ;;
    esac
done
[ -n "$work" ] || usage
case $jobs in ''|*[!0-9]*|0) echo "build_llvm_win64.sh: -j takes a positive number, not '$jobs'" >&2; exit 2 ;; esac

fail() {
    echo "build_llvm_win64.sh: $*" >&2
    exit 1
}

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../../.." && pwd)
toolchain=$repo/dm/host/cmake/x86_64-w64-mingw32.cmake
[ -f "$toolchain" ] || fail "$toolchain is missing"
mkdir -p "$work"
work=$(cd "$work" && pwd)

version=18.1.3
release=https://github.com/llvm/llvm-project/releases/download/llvmorg-$version
# The tarballs and their SHA-256 (checked after every download and before every extraction)
tarballs="llvm:fa6db8951f5ef576ac6bad43d5e1ed83962754538c998fbfa0397cd4521abc00
lld:ea4c325c272ef1022c6517335e0c55d6331cbb00c3b67634b9df1bce011d486e
cmake:acfecb615d41c5b1a0a31e15324994ca06f7a3f37d8958d719b20de0d217b71b"

# The libraries installed (66): what nxaot links (NXAOT_LLVM_COMPONENTS of dm/tools/aot/CMakeLists.txt with LLVM_NATIVE_ARCH X86: LLVMCore, LLVMSupport,
# LLVMOrcJIT, LLVMX86CodeGen / AsmParser / Desc / Disassembler / Info, LLVMAsmParser, LLVMAsmPrinter, LLVMPasses, LLVMAnalysis, LLVMMC, LLVMTransformUtils,
# LLVMMCDisassembler, LLVMObject; and lld's lldCommon, lldELF and lldCOFF) and every library those depend on, as CMake's file API (codemodel-v2) listed the
# dependencies between the static libraries of a configure of this tree with the options below. A library missing here stops the configure step (CMake:
# an exported target "requires target ... that is not in any export set"); then add it
components="LLVMAggressiveInstCombine LLVMAnalysis LLVMAsmParser LLVMAsmPrinter LLVMBinaryFormat LLVMBitReader LLVMBitWriter LLVMBitstreamReader
LLVMCFGuard LLVMCodeGen LLVMCodeGenTypes LLVMCore LLVMCoroutines LLVMDebugInfoBTF LLVMDebugInfoCodeView LLVMDebugInfoDWARF LLVMDebugInfoMSF
LLVMDebugInfoPDB LLVMDemangle LLVMExecutionEngine LLVMExtensions LLVMFrontendOffloading LLVMFrontendOpenMP LLVMGlobalISel LLVMHipStdPar LLVMIRPrinter
LLVMIRReader LLVMInstCombine LLVMInstrumentation LLVMJITLink LLVMLTO LLVMLibDriver LLVMLinker LLVMMC LLVMMCDisassembler LLVMMCParser LLVMObjCARCOpts
LLVMObject LLVMOption LLVMOrcJIT LLVMOrcShared LLVMOrcTargetProcess LLVMPasses LLVMProfileData LLVMRemarks LLVMRuntimeDyld LLVMScalarOpts
LLVMSelectionDAG LLVMSupport LLVMSymbolize LLVMTarget LLVMTargetParser LLVMTextAPI LLVMTransformUtils LLVMVectorize LLVMWindowsDriver
LLVMWindowsManifest LLVMX86AsmParser LLVMX86CodeGen LLVMX86Desc LLVMX86Disassembler LLVMX86Info LLVMipo lldCOFF lldCommon lldELF"
# Installed beside them: LLVM's headers and CMake package, lld's CMake package (its headers have no install rule in 18.1.3: copied below)
components="$components llvm-headers cmake-exports lld-cmake-exports"

prefix=$work/llvm-$version-x86_64-w64-mingw32
src=$work/src/llvm-project
native=$work/build-native
cross=$work/build-cross

cross_args="-DCMAKE_BUILD_TYPE=Release
-DLLVM_ENABLE_PROJECTS=lld
-DLLVM_TARGETS_TO_BUILD=X86
-DLLVM_HOST_TRIPLE=x86_64-w64-windows-gnu
-DLLVM_ENABLE_ASSERTIONS=OFF
-DLLVM_ENABLE_RTTI=ON
-DBUILD_SHARED_LIBS=OFF
-DLLVM_BUILD_LLVM_DYLIB=OFF
-DLLVM_LINK_LLVM_DYLIB=OFF
-DLLVM_INCLUDE_TESTS=OFF
-DLLVM_INCLUDE_BENCHMARKS=OFF
-DLLVM_INCLUDE_EXAMPLES=OFF
-DLLVM_INCLUDE_DOCS=OFF
-DLLVM_INCLUDE_UTILS=OFF
-DLLVM_BUILD_TOOLS=OFF
-DLLVM_BUILD_UTILS=OFF
-DLLVM_ENABLE_BINDINGS=OFF
-DLLVM_ENABLE_ZLIB=OFF
-DLLVM_ENABLE_ZSTD=OFF
-DLLVM_ENABLE_LIBXML2=OFF
-DLLVM_ENABLE_TERMINFO=OFF
-DLLVM_ENABLE_LIBEDIT=OFF
-DLLVM_ENABLE_LIBPFM=OFF"
stamp=$(printf '%s\n%s\n%s\n' "$version" "$components" "$cross_args" | sha256sum | cut -d' ' -f1)

if [ -f "$prefix/.nxaot-prefix" ] && [ "$(cat "$prefix/.nxaot-prefix")" = "$stamp" ]; then
    echo "build_llvm_win64.sh: $prefix is built (stamp $stamp)" >&2
    echo "$prefix"
    exit 0
fi

# Space: the build needs about 0.85 GB in the work folder at its peak (measured), checked with a margin before the build trees are made
need_kib=$((1200 * 1024))
free_kib=$(df -Pk "$work" | awk 'NR == 2 { print $4 }')
if [ ! -d "$cross" ] && [ "$free_kib" -lt "$need_kib" ]; then
    fail "$work has $((free_kib / 1024)) MiB free, the build needs about $((need_kib / 1024)) MiB"
fi

# 1. Downloads
mkdir -p "$work/dl"
for row in $tarballs; do
    name=${row%%:*}
    sum=${row#*:}
    file=$work/dl/$name-$version.src.tar.xz
    if [ ! -f "$file" ] || ! echo "$sum  $file" | sha256sum -c - > /dev/null 2>&1; then
        echo "download: $name-$version.src.tar.xz" >&2
        curl -fsSL --retry 3 -o "$file.part" "$release/$name-$version.src.tar.xz" || fail "download of $name-$version.src.tar.xz failed"
        mv "$file.part" "$file"
    fi
    echo "$sum  $file" | sha256sum -c - > /dev/null 2>&1 || fail "$file: SHA-256 is $(sha256sum "$file" | cut -d' ' -f1), expected $sum"
done

# 2. Sources
if [ ! -f "$src/.extracted" ]; then
    rm -rf "$src"
    mkdir -p "$src"
    for name in llvm lld cmake; do
        tar -xJf "$work/dl/$name-$version.src.tar.xz" -C "$src" \
            --exclude="$name-$version.src/test" --exclude="$name-$version.src/unittests" \
            --exclude="$name-$version.src/benchmarks" --exclude="llvm-$version.src/docs" --exclude="llvm-$version.src/utils/gn"
        mv "$src/$name-$version.src" "$src/$name"
    done
    touch "$src/.extracted"
fi
# lld 18.1.3's add_lld_library gives a library its install-<name> target only when it is shared, and LLVM_DISTRIBUTION_COMPONENTS stops the configure step
# on a component without one ("Specified distribution component 'lldELF' doesn't have an install target"): the one change to the sources, the same target
# for the static libraries too (lld/cmake/modules/AddLLD.cmake; their install rule, COMPONENT <name>, is there already)
sed -i 's/^    if (\${ARG_SHARED} AND NOT CMAKE_CONFIGURATION_TYPES)$/    if (NOT CMAKE_CONFIGURATION_TYPES)/' "$src/lld/cmake/modules/AddLLD.cmake"
grep -q '^    if (NOT CMAKE_CONFIGURATION_TYPES)$' "$src/lld/cmake/modules/AddLLD.cmake" || fail "lld/cmake/modules/AddLLD.cmake: the install-target line was not found"

# 3. The native tablegens
if [ ! -x "$native/bin/llvm-tblgen" ] || [ ! -x "$native/bin/llvm-min-tblgen" ]; then
    echo "native llvm-tblgen (gcc)" >&2
    cmake -S "$src/llvm" -B "$native" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
        -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_DOCS=OFF \
        -DLLVM_INCLUDE_UTILS=OFF -DLLVM_INCLUDE_TOOLS=OFF -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF \
        -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBEDIT=OFF > "$work/build-native.log" 2>&1 \
        || { tail -n 30 "$work/build-native.log" >&2; fail "native configure failed ($work/build-native.log)"; }
    nice -n 10 cmake --build "$native" --target llvm-tblgen llvm-min-tblgen -j "$jobs" >> "$work/build-native.log" 2>&1 \
        || { tail -n 30 "$work/build-native.log" >&2; fail "native llvm-tblgen build failed ($work/build-native.log)"; }
fi

# 4. The cross build. LLVM_INCLUDE_TOOLS stays on: LLVM adds the projects of LLVM_ENABLE_PROJECTS (lld) from its tools folder; LLVM_BUILD_TOOLS=OFF and
# the distribution keep every tool out of the build
echo "cross build: $(date -u +%H:%M:%S) UTC, -j $jobs" >&2
# shellcheck disable=SC2086
cmake -S "$src/llvm" -B "$cross" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_INSTALL_PREFIX="$prefix" $cross_args \
    -DLLVM_TABLEGEN="$native/bin/llvm-tblgen" -DLLVM_NATIVE_TOOL_DIR="$native/bin" \
    "-DLLVM_DISTRIBUTION_COMPONENTS=$(echo $components | tr ' ' ';')" > "$work/build-cross.log" 2>&1 \
    || { tail -n 40 "$work/build-cross.log" >&2; fail "cross configure failed ($work/build-cross.log)"; }
rm -rf "$prefix"
nice -n 10 cmake --build "$cross" --target install-distribution -j "$jobs" >> "$work/build-cross.log" 2>&1 \
    || { tail -n 40 "$work/build-cross.log" >&2; fail "cross build failed ($work/build-cross.log)"; }
# lld's headers (lld/Common/Driver.h and the others nxaot's pack stage includes) and the Version.inc its configure wrote
mkdir -p "$prefix/include"
cp -R "$src/lld/include/lld" "$prefix/include/"
find "$prefix/include/lld" -name '*.in' -exec rm -f {} +
cp "$cross/tools/lld/include/lld/Common/Version.inc" "$prefix/include/lld/Common/"
cp "$src/llvm/LICENSE.TXT" "$prefix/LICENSE.TXT"
echo "$stamp" > "$prefix/.nxaot-prefix"
echo "cross build done: $(date -u +%H:%M:%S) UTC; prefix $(du -sh "$prefix" | cut -f1), build tree $(du -sh "$cross" | cut -f1)" >&2

# 5. Clean up
if [ "$keep" -eq 0 ]; then
    rm -rf "$cross" "$native" "$work/src"
fi
echo "$prefix"
