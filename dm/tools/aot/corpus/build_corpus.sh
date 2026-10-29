#!/bin/sh
# Builds the corpus (dm/tools/aot/corpus, design section 12 item 3): every program with devkitA64's GCC and with upstream clang, as NROs with their
# symbol tables (nro/), and corpus.tsv. Everything runs in Docker; nothing is installed on the machine. From anywhere:
#   sh dm/tools/aot/corpus/build_corpus.sh            build and write dm/tools/aot/corpus/nro/ (the committed NROs)
#   sh dm/tools/aot/corpus/build_corpus.sh --check    build into build/corpus/check and compare byte for byte with the committed files
# GCC runs in the devkitpro/devkita64 image pinned below (pulled on first use, about 700 MB compressed, 3 GB unpacked), clang-18, ld.lld-18 and
# the NRO conversion in the nxnative-aot image (docker/Dockerfile.aot). With DKP_ROOT=<a devkitPro tree> GCC runs from that tree in the
# nxnative-aot image instead (fetch_devkita64.py extracts the same binaries from the pinned image's layer, for a machine short of disk).
# Every NRO is also compared with what devkitPro's elf2nro writes for its ELF (check_elf2nro.sh). Exit status: 0 on success, 1 on a build or
# comparison failure, 2 on a usage error.
set -eu
DKP_IMAGE=devkitpro/devkita64:20260219@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282
AOT_IMAGE=${AOT_IMAGE:-nxnative-aot}
check=0
case ${1:-} in
"") ;;
--check) check=1 ;;
*) echo "usage: sh dm/tools/aot/corpus/build_corpus.sh [--check]" >&2; exit 2 ;;
esac
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../../.." && pwd)
uid=$(id -u)
gid=$(id -g)
# Paths inside the containers: the repository is /src; compile.sh takes its output folder relative to the corpus folder
work=build/corpus
if [ $check -eq 1 ]; then out=$work/check; tsv=$work/check/corpus.tsv; else out=dm/tools/aot/corpus/nro; tsv=dm/tools/aot/corpus/corpus.tsv; fi

aot() { docker run --rm --user "$uid:$gid" -e HOME=/tmp -v "$repo:/src" -w /src "$AOT_IMAGE" "$@"; }
gcc_env() {
    if [ -n "${DKP_ROOT:-}" ]; then
        docker run --rm --user "$uid:$gid" -e HOME=/tmp -e DEVKITPRO=/opt/devkitpro -v "$DKP_ROOT:/opt/devkitpro:ro" -v "$repo:/src" -w /src "$AOT_IMAGE" "$@"
    else
        docker run --rm --user "$uid:$gid" -e HOME=/tmp -v "$repo:/src" -w /src "$DKP_IMAGE" "$@"
    fi
}

rm -rf "$repo/$work/gcc" "$repo/$work/clang" "$repo/$work/check"
gcc_env sh dm/tools/aot/corpus/compile.sh gcc "../../../../$work/gcc"
aot sh dm/tools/aot/corpus/compile.sh clang "../../../../$work/clang"
aot python3 dm/tools/aot/corpus/nros.py "$work/gcc" "$work/clang" "$out" "$tsv"
gcc_env sh dm/tools/aot/corpus/check_elf2nro.sh "$work/gcc" "$out" gcc "$work/clang" "$out" clang
if [ $check -eq 1 ]; then
    python3 "$here/nros.py" --compare "$repo/dm/tools/aot/corpus/nro" "$repo/dm/tools/aot/corpus/corpus.tsv" "$repo/$out" "$repo/$tsv"
fi
