#!/bin/sh
# The SPIR-V tools of the Vulkan shader chain, built from pinned source for Linux or Windows (x86-64): spirv-val and spirv-link (SPIRV-Tools v2025.1, with the SPIRV-Headers
# commit its DEPS file names), glslangValidator (glslang 15.1.0, with that SPIRV-Tools as its optimizer) and spirv-cross (SPIRV-Cross, tag vulkan-sdk-1.4.363.0: the commit
# dm/gfx/tools/spirv_cross_bin/build.sh builds for the Mac). dmsh_build_vk.py, dmsh_spv.py and gl_shaders.py run them, and dm/gfx/tools/dmsh_chain_vk.lock records the version
# line each prints, one row per tool and platform (spirv-tools_<platform>, glslang_<platform>, spirv-cross_<platform>), which dmsh_chain_vk.py --check-tools compares. One
# script for both platforms, so that both run the same source: the lock's linux-x64 and win-x64 rows hold the same lines (VERSIONS below).
# Who runs it:
#   - the port's bundles: its output is the --spirv input of scripts/package_port.py (bin/ of the bundle, which the packer puts first on PATH: build_ipa_from_nsp.py
#     bundled_spirv), so a player packs an NVN game with nothing installed (docs/development.md, "The port's bundles")
#   - the chain image (docker/Dockerfile.chain builds it in a stage of its own, with --all, and puts the output first on PATH), so the image's tools are the lock's. It lives in
#     docker/ because that folder is the image's build context
# Run it in a container, never on a player's PC; from the repository root:
#   linux:   docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-build sh docker/build_spirv_tools.sh linux build/port_spirv/linux-x64 --check
#   windows: docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src mstorsjo/llvm-mingw:20261020 sh docker/build_spirv_tools.sh windows build/port_spirv/win-x64 --check
# Linux: clang (Ubuntu 24.04's clang 18 in both images) with libstdc++ and libgcc linked in (-static-libstdc++ -static-libgcc): each program needs only libc and libm, as the
# bundle's other programs do (glibc's own symbols up to 2.38 at most; glibc is not linked statically, which would put its LGPL terms on the binaries). Windows: llvm-mingw's
# clang, linked with -static: Windows' own DLLs and the UCRT only, no libc++ or winpthread DLL.
# Every download is pinned below by URL and SHA-256 (GitHub's archives of the tags and commits; a mismatch stops with a [RED] line), into <work>/dl, kept for the next run.
# The version lines: SPIRV-Tools prints "unknown hash, <SOURCE_DATE_EPOCH>" built outside a git checkout, so SOURCE_DATE_EPOCH is the tag's commit time; glslang takes its version
# from its CHANGES.md; SPIRV-Cross asks git for `describe --tags` of its source (CMakeLists.txt) and takes the configure time (SOURCE_DATE_EPOCH: the commit's) as its timestamp,
# so its configure is given a stand-in that answers the pinned tag, the name git gives the commit whose archive the SHA-256 checked: the line is the one a clone of the tag
# prints (docker/Dockerfile.chain's earlier clone, the Mac's build.sh). Paths of the work folder are mapped out of the binaries (-ffile-prefix-map): the same image gives the same
# bytes in any folder.
# Output: <out>/spirv-val, spirv-link, glslangValidator, spirv-cross (.exe on Windows; --all adds spirv-as, spirv-dis and spirv-opt, which the chain image's tests use) and
# <out>/licenses/ (SPIRV-Tools.txt, SPIRV-Headers.txt, glslang.txt, SPIRV-Cross.txt with its LICENSES/ texts).
# --check: Linux: each program prints its row's line (VERSIONS) and needs no shared library beyond libc, libm and the dynamic loader; Windows (not run here): each is a PE32+
# x86-64 program importing no DLL of llvm-mingw's runtime (libc++, libunwind, libwinpthread), listed with llvm-readobj.
# Usage: sh docker/build_spirv_tools.sh [-j N] [--all] [--work <folder>] [--keep] <linux|windows> <out folder> [--check]
#   -j N: compile jobs (default 4); --work: downloads, sources and build trees (default <out>.work; the build trees and sources are removed at the end unless --keep).
# Time, measured on 2026-11-15 (16 vCPUs, another build beside it): 3.1 minutes for Linux and 2.4 for Windows at -j 6, 3.7 for the chain image's stage (--all, -j 4).
# The work folder keeps the downloads (9.4 MB) for the next run.
# Exit status: 0 when done, 1 on a failure (a download, a hash, a build step, a check), 2 on a usage error.
set -eu

usage() { echo "usage: sh docker/build_spirv_tools.sh [-j N] [--all] [--work <folder>] [--keep] <linux|windows> <out folder> [--check]" >&2; exit 2; }
die() { echo "build_spirv_tools: [RED] $*" >&2; exit 1; }
say() { echo "build_spirv_tools: $*"; }

jobs=4 all=0 keep=0 check=0 work= platform= out=
while [ $# -gt 0 ]; do
    case $1 in
        -j) [ $# -ge 2 ] || usage; jobs=$2; shift 2 ;;
        -j*) jobs=${1#-j}; shift ;;
        --all) all=1; shift ;;
        --keep) keep=1; shift ;;
        --check) check=1; shift ;;
        --work) [ $# -ge 2 ] || usage; work=$2; shift 2 ;;
        -h|--help) usage ;;
        -*) echo "build_spirv_tools: unknown option $1" >&2; usage ;;
        *) if [ -z "$platform" ]; then platform=$1; elif [ -z "$out" ]; then out=$1; else usage; fi; shift ;;
    esac
done
[ -n "$platform" ] && [ -n "$out" ] || usage
case $jobs in ''|*[!0-9]*|0) echo "build_spirv_tools: -j takes a positive number, not '$jobs'" >&2; exit 2 ;; esac
case $platform in
    linux) exe= ;;
    windows) exe=.exe ;;
    *) usage ;;
esac

# The sources: name, archive URL, SHA-256, the archive's top folder, SOURCE_DATE_EPOCH (the commit's time, UTC). SPIRV-Headers is the commit SPIRV-Tools v2025.1's DEPS names
# (spirv_headers_revision); SPIRV-Cross the commit of the tag vulkan-sdk-1.4.363.0 (2026-09-16T11:27:19Z)
SOURCES="SPIRV-Headers https://github.com/KhronosGroup/SPIRV-Headers/archive/09913f088a1197aba4aefd300a876b2ebbaa3391.tar.gz 6d4aad5a86d7d5af144747aee1d616f624897d64d842f282aca7777f84244318 SPIRV-Headers-09913f088a1197aba4aefd300a876b2ebbaa3391 1738776763
SPIRV-Tools https://github.com/KhronosGroup/SPIRV-Tools/archive/refs/tags/v2025.1.tar.gz 6a9313fa68e061d463f616357cd24cdf1c3a27d906ea791d7ba67dd1b6666a40 SPIRV-Tools-2025.1 1740516879
glslang https://github.com/KhronosGroup/glslang/archive/refs/tags/15.1.0.tar.gz 4bdcd8cdb330313f0d4deed7be527b0ac1c115ff272e492853a6e98add61b4bc glslang-15.1.0 1734126321
SPIRV-Cross https://github.com/KhronosGroup/SPIRV-Cross/archive/f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7.tar.gz 92b0458889ed77eac9892b3be5a41e3cf716849fd90f39632e159e555abf3d03 SPIRV-Cross-f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7 1789558039"
SPIRV_CROSS_TAG=vulkan-sdk-1.4.363.0
# The first line each program prints (spirv-val --version, glslangValidator --version, spirv-cross --revision): dmsh_chain_vk.lock's rows of both platforms hold them
# (dm/tools/tests/test_package_port.py checks that they agree)
VERSIONS="spirv-tools	SPIRV-Tools v2025.1 unknown hash, 2025-02-25T20:54:39+00:00
glslang	Glslang Version: 11:15.1.0
spirv-cross	Git commit: vulkan-sdk-1.4.363.0 Timestamp: 2026-09-16T11:27:19"

tools="spirv-val spirv-link glslangValidator spirv-cross"
[ "$all" -eq 0 ] || tools="$tools spirv-as spirv-dis spirv-opt"
mkdir -p "$out"
out=$(cd "$out" && pwd)
work=${work:-$out.work}
mkdir -p "$work/dl"
work=$(cd "$work" && pwd)
command -v python3 > /dev/null || die "no python3: SPIRV-Tools' and glslang's builds need it (both images have it)"
command -v cmake > /dev/null && command -v ninja > /dev/null || die "no cmake or ninja: run in nxnative-build or mstorsjo/llvm-mingw:20261020"
field() { echo "$SOURCES" | awk -v n="$1" -v f="$2" '$1 == n { print $f }'; }

# 1. Downloads, each checked against its SHA-256 (python3's urllib: the build image has no curl)
for name in SPIRV-Headers SPIRV-Tools glslang SPIRV-Cross; do
    url=$(field "$name" 2) sum=$(field "$name" 3)
    file=$work/dl/$name-$sum.tar.gz
    if [ ! -f "$file" ] || [ "$(sha256sum "$file" | cut -d' ' -f1)" != "$sum" ]; then
        say "download: $url"
        python3 -c 'import shutil, sys, urllib.request
with urllib.request.urlopen(sys.argv[1], timeout=120) as r, open(sys.argv[2], "wb") as f: shutil.copyfileobj(r, f)' "$url" "$file.part" || die "download of $url failed"
        mv "$file.part" "$file"
    fi
    got=$(sha256sum "$file" | cut -d' ' -f1)
    [ "$got" = "$sum" ] || die "$url: SHA-256 is $got, the pinned one $sum (a moved tag or a changed archive: check the source, then pin it again)"
done

# 2. Sources: glslang with SPIRV-Tools in its External/ folder and SPIRV-Headers in SPIRV-Tools' external/ (glslang's own layout: one build tree for both), SPIRV-Cross apart
src=$work/src
rm -rf "$src" "$work/build-glslang" "$work/build-cross" "$work/bin"
mkdir -p "$src"
unpack() {   # unpack <name> <folder>: the archive's top folder becomes <folder>
    tar -xzf "$work/dl/$1-$(field "$1" 3).tar.gz" -C "$src"
    mkdir -p "$(dirname "$2")"; mv "$src/$(field "$1" 4)" "$2"
}
unpack glslang "$src/glslang"
unpack SPIRV-Tools "$src/glslang/External/spirv-tools"
unpack SPIRV-Headers "$src/glslang/External/spirv-tools/external/spirv-headers"
unpack SPIRV-Cross "$src/SPIRV-Cross"

# 3. The compilers and link options of the platform
flags="-ffile-prefix-map=$work=."
if [ "$platform" = linux ]; then
    cc=clang cxx=clang++
    if command -v clang-18 > /dev/null && command -v clang++-18 > /dev/null; then cc=clang-18 cxx=clang++-18; fi
    command -v "$cxx" > /dev/null || die "no $cxx: run in nxnative-build (docker/Dockerfile.build) or the chain image's build stage"
    set -- -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc -s"
else
    command -v x86_64-w64-mingw32-clang++ > /dev/null || die "no x86_64-w64-mingw32-clang++: run in mstorsjo/llvm-mingw:20261020"
    set -- -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=x86_64 -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++ \
        -DCMAKE_EXE_LINKER_FLAGS="-static -s"
fi
export TZ=UTC GIT_CEILING_DIRECTORIES="$work"   # no git repository above the sources answers for them (SPIRV-Tools asks git first)

# 4. SPIRV-Tools and glslang in one tree (SPIRV-Tools' version line takes SOURCE_DATE_EPOCH at build time)
targets="spirv-val spirv-link glslang-standalone"
[ "$all" -eq 0 ] || targets="$targets spirv-as spirv-dis spirv-opt"
say "SPIRV-Tools $(field SPIRV-Tools 4) and glslang $(field glslang 4) for $platform: $targets (-j $jobs)"
SOURCE_DATE_EPOCH=$(field SPIRV-Tools 5) cmake -S "$src/glslang" -B "$work/build-glslang" -G Ninja -DCMAKE_BUILD_TYPE=Release "$@" \
    -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" -DENABLE_OPT=ON -DBUILD_EXTERNAL=ON -DGLSLANG_TESTS=OFF -DGLSLANG_ENABLE_INSTALL=OFF -DENABLE_PCH=OFF \
    -DSPIRV_SKIP_TESTS=ON -DSPIRV_WERROR=OFF -DSPIRV_TOOLS_BUILD_STATIC=ON > "$work/glslang-configure.log" 2>&1 \
    || { tail -n 30 "$work/glslang-configure.log" >&2; die "configure of glslang and SPIRV-Tools failed ($work/glslang-configure.log)"; }
# shellcheck disable=SC2086
SOURCE_DATE_EPOCH=$(field SPIRV-Tools 5) nice -n 10 cmake --build "$work/build-glslang" --target $targets -j "$jobs" > "$work/glslang-build.log" 2>&1 \
    || { tail -n 40 "$work/glslang-build.log" >&2; die "build of glslang and SPIRV-Tools failed ($work/glslang-build.log)"; }

# 5. SPIRV-Cross: its configure asks git for the version (the stand-in answers the pinned tag) and the time (SOURCE_DATE_EPOCH)
mkdir -p "$work/bin"
printf '#!/bin/sh\n# Stands in for git in the configure of SPIRV-Cross (build_spirv_tools.sh): `describe` of the pinned commit is its tag\ncase "$1" in --version) echo "git version 2.43.0" ;; describe) echo "%s" ;; *) exit 1 ;; esac\n' \
    "$SPIRV_CROSS_TAG" > "$work/bin/describe-tag"
chmod 755 "$work/bin/describe-tag"
say "SPIRV-Cross $SPIRV_CROSS_TAG ($(field SPIRV-Cross 4)) for $platform"
SOURCE_DATE_EPOCH=$(field SPIRV-Cross 5) cmake -S "$src/SPIRV-Cross" -B "$work/build-cross" -G Ninja -DCMAKE_BUILD_TYPE=Release "$@" -DCMAKE_C_FLAGS="$flags" \
    -DCMAKE_CXX_FLAGS="$flags" -DGIT_EXECUTABLE="$work/bin/describe-tag" -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_SKIP_INSTALL=ON -DSPIRV_CROSS_SHARED=OFF \
    -DSPIRV_CROSS_STATIC=ON -DSPIRV_CROSS_CLI=ON > "$work/cross-configure.log" 2>&1 \
    || { tail -n 30 "$work/cross-configure.log" >&2; die "configure of SPIRV-Cross failed ($work/cross-configure.log)"; }
grep -q "Git hash: $SPIRV_CROSS_TAG" "$work/cross-configure.log" || die "SPIRV-Cross's configure did not take the tag from the stand-in ($work/cross-configure.log)"
nice -n 10 cmake --build "$work/build-cross" --target spirv-cross -j "$jobs" > "$work/cross-build.log" 2>&1 \
    || { tail -n 40 "$work/cross-build.log" >&2; die "build of SPIRV-Cross failed ($work/cross-build.log)"; }

# 6. The programs and their licences into <out>
found() {   # found <tree> <file name>: the one file of that name the build wrote
    f=$(find "$1" -type f -name "$2" ! -path '*/CMakeFiles/*' | head -n 1)
    [ -n "$f" ] || die "the build wrote no $2 in $1"
    echo "$f"
}
rm -rf "$out/licenses"; mkdir -p "$out/licenses"
for t in $tools; do
    case $t in
        glslangValidator) f=$(found "$work/build-glslang" "glslang$exe") ;;   # glslang's program (OUTPUT_NAME glslang); glslangValidator is the name it is run by
        spirv-cross) f=$(found "$work/build-cross" "spirv-cross$exe") ;;
        *) f=$(found "$work/build-glslang/External/spirv-tools/tools" "$t$exe") ;;
    esac
    cp "$f" "$out/$t$exe"; chmod 755 "$out/$t$exe"
done
licence() {   # licence <name> <what> <archive URL> <licence file>: the text, under a line naming what it covers
    { echo "$2 ($3), linked into the SPIR-V tools of docker/build_spirv_tools.sh"; echo; cat "$4"; } > "$out/licenses/$1.txt"
}
licence SPIRV-Tools "SPIRV-Tools v2025.1" "$(field SPIRV-Tools 2)" "$src/glslang/External/spirv-tools/LICENSE"
licence SPIRV-Headers "SPIRV-Headers $(field SPIRV-Headers 4 | sed 's/^SPIRV-Headers-//')" "$(field SPIRV-Headers 2)" "$src/glslang/External/spirv-tools/external/spirv-headers/LICENSE"
licence glslang "glslang 15.1.0" "$(field glslang 2)" "$src/glslang/LICENSE.txt"
licence SPIRV-Cross "SPIRV-Cross $SPIRV_CROSS_TAG" "$(field SPIRV-Cross 2)" "$src/SPIRV-Cross/LICENSE"
for l in "$src"/SPIRV-Cross/LICENSES/*.txt; do { echo; echo "---- SPIRV-Cross LICENSES/$(basename "$l") (the licences of some of its files) ----"; echo; cat "$l"; } >> "$out/licenses/SPIRV-Cross.txt"; done
for t in $tools; do say "$out/$t$exe: $(wc -c < "$out/$t$exe") bytes, sha256 $(sha256sum < "$out/$t$exe" | cut -c1-16)"; done

# 7. The check
if [ "$check" -eq 1 ]; then
    if [ "$platform" = linux ]; then
        echo "$VERSIONS" | while IFS='	' read -r row want; do
            case $row in spirv-tools) cmd="spirv-val --version" ;; glslang) cmd="glslangValidator --version" ;; spirv-cross) cmd="spirv-cross --revision" ;; esac
            # shellcheck disable=SC2086
            got=$("$out"/$cmd 2>&1 | head -n 1) || true
            [ "$got" = "$want" ] || die "$cmd prints '$got', not the line of dmsh_chain_vk.lock's ${row}_linux-x64 row, '$want'"
            say "check: $cmd: $got"
        done
        readelf=$(command -v readelf || command -v llvm-readelf-18 || command -v llvm-readelf) || die "no readelf to list what the programs need"
        for t in $tools; do
            needed=$(LC_ALL=C "$readelf" -d "$out/$t" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | tr '\n' ' ')
            for n in $needed; do case $n in libc.so.6|libm.so.6|ld-linux-x86-64.so.2) ;; *) die "$t needs $n: only libc and libm may be (rebuild with -static-libstdc++ -static-libgcc)" ;; esac; done
            say "check: $t needs ${needed:-no shared library}"
        done
    else
        for t in $tools; do
            imports=$(llvm-readobj --coff-imports "$out/$t.exe" | sed -n 's/^ *Name: \(.*\.[dD][lL][lL]\)$/\1/p' | sort -u | tr '\n' ' ')
            [ -n "$imports" ] || die "$t.exe: llvm-readobj lists no imports (not a PE program?)"
            for d in $imports; do case $(echo "$d" | tr 'A-Z' 'a-z') in libc++*|libunwind*|libwinpthread*|libgcc*|libstdc++*) die "$t.exe imports $d, a DLL of the toolchain's runtime, not one of Windows" ;; esac; done
            say "check: $t.exe imports $imports(not run here: a Windows program)"
        done
    fi
fi
if [ "$keep" -eq 0 ]; then rm -rf "$src" "$work/build-glslang" "$work/build-cross" "$work/bin"; fi
say "done: $out ($(du -sh "$out" | cut -f1))"
