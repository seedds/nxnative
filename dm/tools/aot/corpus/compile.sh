#!/bin/sh
# Compiles and links the corpus programs with one toolchain, inside a container (build_corpus.sh runs it; the repository is the working directory's
# ../../../.. and paths stay relative to this folder, so that nothing machine-specific reaches the ELF files or their build IDs):
#   sh compile.sh gcc <out dir>     devkitA64's GCC and GNU ld, from $DEVKITPRO (/opt/devkitpro in the devkitpro/devkita64 image)
#   sh compile.sh clang <out dir>   clang-18 and ld.lld-18 of the nxnative-aot image
# Writes <out dir>/<program>.elf for every program of PROGRAMS. The flags of each program are in the case below and in corpus.tsv.
set -eu
[ $# -eq 2 ] || { echo "usage: sh compile.sh gcc|clang <out dir>" >&2; exit 2; }
tc=$1
out=$2
cd "$(dirname "$0")"
mkdir -p "$out"

PROGRAMS=${CORPUS_PROGRAMS:-"int_ops switch fnptr fp simd sjlj excl"}
COMMON="-O2 -mcpu=cortex-a57 -mno-outline-atomics -std=gnu11 -ffreestanding -fPIE -fno-stack-protector -ffp-contract=off -fno-math-errno -Wall -Wextra -Werror -Irt"
LINK="-pie --no-dynamic-linker --eh-frame-hdr --export-dynamic --hash-style=sysv --build-id=sha1 -z text -z max-page-size=0x1000 -T rt/nro.ld"
case $tc in
gcc)
    bin=${DEVKITPRO:-/opt/devkitpro}/devkitA64/bin
    CC="$bin/aarch64-none-elf-gcc"
    LD="$bin/aarch64-none-elf-ld"
    LDFLAGS="$LINK"                                   # RELA relocations
    ;;
clang)
    CC="clang-18 --target=aarch64-none-elf"
    LD="ld.lld-18"
    LDFLAGS="$LINK -z pack-relative-relocs"           # RELR relocations (DT_RELR), as SDK 20 modules have them
    ;;
*)
    echo "unknown toolchain $tc" >&2; exit 2 ;;
esac

# flags <program>: what the program adds to COMMON
flags() {
    case $1 in
    int_ops) echo "-fno-asynchronous-unwind-tables -fno-unwind-tables" ;;  # no FDEs: discovery without unwind entries
    fnptr) echo "-fasynchronous-unwind-tables -fexceptions" ;;              # cleanup landing pads: LSDA call-site tables
    *) echo "-fasynchronous-unwind-tables" ;;
    esac
}

# sources <program>: its C and assembly files besides the run time
sources() {
    case $1 in
    switch) echo "src/switch.c src/switch_word.c" ;;
    sjlj) echo "src/sjlj.c rt/setjmp.S" ;;
    *) echo "src/$1.c" ;;
    esac
}

version=$($CC --version | head -n 1)
printf '# program\ttoolchain\tcompiler\tflags\tlink\n' > "$out/flags.tsv"
for p in $PROGRAMS; do
    objs=""
    pf=$(flags "$p")
    wide=""
    if [ "$tc" = clang ] && [ "$p" = switch ]; then wide="; src/switch_word.c: -mllvm -aarch64-enable-compress-jump-tables=false"; fi
    printf '%s\t%s\t%s\t%s\t%s\n' "$p" "$tc" "$version" "$COMMON $pf$wide" "$LDFLAGS" >> "$out/flags.tsv"
    for s in rt/crt0.S rt/mem.S rt/rt.c $(sources "$p"); do
        o="$out/$p.$(basename "$s" | tr . _).o"
        extra=""
        # clang's compressed jump tables (byte and halfword entries) are its default; the word-table source is built without them, so that
        # the corpus has clang's table-relative word tables too (GCC picks word entries there by the distance of its cases)
        if [ "$tc" = clang ] && [ "$s" = src/switch_word.c ]; then extra="-mllvm -aarch64-enable-compress-jump-tables=false"; fi
        # shellcheck disable=SC2086
        $CC $COMMON $pf $extra -c "$s" -o "$o"
        objs="$objs $o"
    done
    # shellcheck disable=SC2086
    $LD $LDFLAGS -o "$out/$p.elf" $objs
done
echo "compile.sh $tc: $(echo $PROGRAMS | wc -w) programs in $out"
