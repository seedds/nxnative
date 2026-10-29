#!/bin/sh
# Compares tests/elf2nro.py with devkitPro's elf2nro (switch-tools) on every corpus ELF, inside a container that has devkitPro (build_corpus.sh
# runs it):  sh check_elf2nro.sh <elf dir> <nro dir> <toolchain> [<elf dir> <nro dir> <toolchain> ...]
# Exit status 1 when an NRO differs from the one elf2nro writes.
set -eu
tool=${DEVKITPRO:-/opt/devkitpro}/tools/bin/elf2nro
tmp=$(mktemp -d)
status=0
n=0
while [ $# -ge 3 ]; do
    for elf in "$1"/*.elf; do
        p=$(basename "$elf" .elf)
        "$tool" "$elf" "$tmp/ref.nro" > /dev/null
        n=$((n + 1))
        if ! cmp -s "$tmp/ref.nro" "$2/$p-$3.nro"; then
            echo "check_elf2nro.sh: $2/$p-$3.nro differs from devkitPro's elf2nro output"
            status=1
        fi
    done
    shift 3
done
rm -rf "$tmp"
echo "check_elf2nro.sh: $n NROs compared with devkitPro's elf2nro, $([ $status -eq 0 ] && echo identical || echo 'differences found')"
exit $status
