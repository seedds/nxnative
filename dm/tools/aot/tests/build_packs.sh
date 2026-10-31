#!/bin/sh
# The packs of the corpus for dm/host's run time (CTest t_aot_host, dm/host/tests/t_aot_host.c): every NRO of corpus/corpus.tsv through
# `nxaot build` with the defaults (swifttailcc, the balanced mode, safepoint polls) into <out>/<name>.so, <name> the NRO's file name without
# .nro. Run in the nxnative-aot image (docker/Dockerfile.aot) after building nxaot:
#   sh dm/tools/aot/tests/build_packs.sh <nxaot build folder> <out folder>
# The objects nxaot writes beside each shared object are deleted; each NRO's build log is kept as <out>/<name>.log. Exit status 1 when a build
# fails (its log is printed), 2 on a usage error.
set -eu

if [ $# -ne 2 ]; then
    echo "usage: build_packs.sh <nxaot build folder> <out folder>" >&2
    exit 2
fi
build=$1
out=$2
here=$(cd "$(dirname "$0")" && pwd)
corpus=$here/../corpus
nxaot=$build/nxaot
if [ ! -x "$nxaot" ]; then
    echo "build_packs.sh: $nxaot is not built (cmake --build $build --target nxaot)" >&2
    exit 2
fi
mkdir -p "$out"

n=0
start=$(date +%s)
for nro in $(grep -v '^#' "$corpus/corpus.tsv" | cut -f1); do
    name=${nro%.nro}
    if ! "$nxaot" build "$corpus/nro/$nro" -o "$out/$name.o" --shared "$out/$name.so" > "$out/$name.log" 2>&1; then
        cat "$out/$name.log" >&2
        echo "build_packs.sh: nxaot build failed for $nro" >&2
        exit 1
    fi
    rm -f "$out/$name.o"
    n=$((n + 1))
done
echo "build_packs.sh: $n packs in $out ($(($(date +%s) - start)) s)"
