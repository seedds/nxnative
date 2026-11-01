#!/bin/sh
# The packs of the corpus for dm/host's run time (CTest t_aot_host, dm/host/tests/t_aot_host.c): every NRO of corpus/corpus.tsv through
# `nxaot build` with the defaults (swifttailcc, the balanced mode, safepoint polls) into <out>/<name>.so, <name> the NRO's file name without
# .nro. Run in the nxnative-aot image (docker/Dockerfile.aot) after building nxaot:
#   sh dm/tools/aot/tests/build_packs.sh <nxaot build folder> <out folder>
# Each NRO's build log is kept as <out>/<name>.log. Exit status 1 when a build fails (its log is printed), 2 on a usage error. NXAOT_ARGS adds
# options to every `nxaot build` (--workers N, --cache <dir>: README.md, "Pack throughput").
# Then the synthetic game module of CTest t_aot_module (dm/host/tests/t_aot_module.c) into <out>/module: dm/tools/tests/aot_synth.py writes its NSO,
# its patch file and, through patch_nso.py's own writer, the behaviour image and manifest patch_nso.py --aot writes for it (main.image.bin,
# main.aot.tsv), and `nxaot build` packs them into main.aot.so beside them, as a game's packs lie beside their manifests; its log is <out>/module.log.
# Last, all of them again as Windows packs (DLLs) into <out>/win64 (below)
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
    if ! "$nxaot" build "$corpus/nro/$nro" --shared "$out/$name.so" ${NXAOT_ARGS:-} > "$out/$name.log" 2>&1; then
        cat "$out/$name.log" >&2
        echo "build_packs.sh: nxaot build failed for $nro" >&2
        exit 1
    fi
    n=$((n + 1))
done
rm -rf "$out/module"
if ! { python3 "$here/../../tests/aot_synth.py" "$out/module" &&
       "$nxaot" build "$out/module/main.aot.tsv" --shared "$out/module/main.aot.so" ${NXAOT_ARGS:-}; } > "$out/module.log" 2>&1; then
    cat "$out/module.log" >&2
    echo "build_packs.sh: the synthetic module's pack failed" >&2
    exit 1
fi
echo "build_packs.sh: $n packs and the synthetic module's in $out ($(($(date +%s) - start)) s)"
# The host-function module of CTest t_aot_hostfn (dm/host/tests/t_aot_hostfn.c) into <out>/hostfn: aot_synth.py --hostfn writes its image and manifest (main) and the NRO it
# loads at run time (sub.nro, with nro.tsv); nxaot packs main from its manifest and sub from the NRO file, as a game's NROs are packed beside its modules; its log is
# <out>/hostfn.log
rm -rf "$out/hostfn"
if ! { python3 "$here/../../tests/aot_synth.py" --hostfn "$out/hostfn" &&
       "$nxaot" build "$out/hostfn/main.aot.tsv" --shared "$out/hostfn/main.aot.so" ${NXAOT_ARGS:-} &&
       "$nxaot" build "$out/hostfn/sub.nro" --shared "$out/hostfn/sub.aot.so" ${NXAOT_ARGS:-}; } > "$out/hostfn.log" 2>&1; then
    cat "$out/hostfn.log" >&2
    echo "build_packs.sh: the host-function module's packs failed" >&2
    exit 1
fi
echo "build_packs.sh: the host-function module's two packs in $out/hostfn"
# The same packs for the Windows host (nxaot build --target windows: DLLs, README.md "Windows packs") into <out>/win64, the folder NXAOT_PACKS names
# on the Windows PC (dm/host/tests/run_win64_tests.ps1 looks for it as build\packs\win64; docker/README.md): <name>.dll per corpus NRO (t_aot_host's
# name for it on Windows), and module/ and hostfn/ with the synthetic modules' inputs written again by aot_synth.py and main.aot.dll, sub.aot.dll
# (DM_AOT_PACK_SUFFIX); the logs beside them as above
win=$out/win64
mkdir -p "$win"
start=$(date +%s)
for nro in $(grep -v '^#' "$corpus/corpus.tsv" | cut -f1); do
    name=${nro%.nro}
    if ! "$nxaot" build "$corpus/nro/$nro" --target windows --shared "$win/$name.dll" ${NXAOT_ARGS:-} > "$win/$name.log" 2>&1; then
        cat "$win/$name.log" >&2
        echo "build_packs.sh: nxaot build --target windows failed for $nro" >&2
        exit 1
    fi
done
rm -rf "$win/module" "$win/hostfn"
if ! { python3 "$here/../../tests/aot_synth.py" "$win/module" &&
       "$nxaot" build "$win/module/main.aot.tsv" --target windows --shared "$win/module/main.aot.dll" ${NXAOT_ARGS:-} &&
       python3 "$here/../../tests/aot_synth.py" --hostfn "$win/hostfn" &&
       "$nxaot" build "$win/hostfn/main.aot.tsv" --target windows --shared "$win/hostfn/main.aot.dll" ${NXAOT_ARGS:-} &&
       "$nxaot" build "$win/hostfn/sub.nro" --target windows --shared "$win/hostfn/sub.aot.dll" ${NXAOT_ARGS:-}; } > "$win/synth.log" 2>&1; then
    cat "$win/synth.log" >&2
    echo "build_packs.sh: the synthetic modules' Windows packs failed" >&2
    exit 1
fi
echo "build_packs.sh: $n Windows packs and the synthetic modules' in $win ($(($(date +%s) - start)) s)"
