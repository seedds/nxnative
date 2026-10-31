#!/bin/sh
# The packs of the corpus for dm/host's run time (CTest t_aot_host, dm/host/tests/t_aot_host.c): every NRO of corpus/corpus.tsv through
# `nxaot build` with the defaults (swifttailcc, the balanced mode, safepoint polls) into <out>/<name>.so, <name> the NRO's file name without
# .nro. Run in the nxnative-aot image (docker/Dockerfile.aot) after building nxaot:
#   sh dm/tools/aot/tests/build_packs.sh <nxaot build folder> <out folder>
# The objects nxaot writes beside each shared object are deleted; each NRO's build log is kept as <out>/<name>.log. Exit status 1 when a build
# fails (its log is printed), 2 on a usage error.
# Then the synthetic game module of CTest t_aot_module (dm/host/tests/t_aot_module.c) into <out>/module: dm/tools/tests/aot_synth.py writes its NSO,
# its patch file and, through patch_nso.py's own writer, the behaviour image and manifest patch_nso.py --aot writes for it (main.image.bin,
# main.aot.tsv), and `nxaot build` packs them into main.aot.so beside them, as a game's packs lie beside their manifests; its log is <out>/module.log
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
rm -rf "$out/module"
if ! { python3 "$here/../../tests/aot_synth.py" "$out/module" &&
       "$nxaot" build "$out/module/main.aot.tsv" -o "$out/module/main.o" --shared "$out/module/main.aot.so"; } > "$out/module.log" 2>&1; then
    cat "$out/module.log" >&2
    echo "build_packs.sh: the synthetic module's pack failed" >&2
    exit 1
fi
rm -f "$out/module/main.o"
echo "build_packs.sh: $n packs and the synthetic module's in $out ($(($(date +%s) - start)) s)"
