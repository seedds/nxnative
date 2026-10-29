# nxaot: the ahead-of-time ARM64 to x86-64 recompiler

Phase 2 of the Windows and Linux port ([migration-status.md](../../../docs/design/migration-status.md)); the design is
[aot-recompiler.md](../../../docs/design/aot-recompiler.md). This folder holds its foundation and its floating point: the build, the lifter of the
integer, memory, branch, system-register, exclusive, floating-point, Advanced SIMD and cryptographic instruction classes (every IR opcode dynarmic's
A64 frontend emits), the differential vector harness against dynarmic, a benchmark of the NaN chains, and the census of phase 0; and phase 1: the
module loader (NROs and behaviour images with their manifests), function discovery over whole modules with jump tables recovered on the lifted IR,
exclusive windows and interprocedural summaries, a corpus of self-checking homebrew NROs with no game files, whole functions lifted with the calling
convention of design 2.3 (calls, returns checked against lr, real tail calls, non-local returns), the dispatcher of section 4 (jump tables as
switches, inline caches, the lookup table), the pack stage (`nxaot build`: one object and one shared object per module, linked by lld as a
library), and a minimal native run time (`nxaot_host`): the fourteen corpus NROs run natively on x86-64 and print what dynarmic prints. No game runs
through it yet, and nothing here touches the Apple builds.

## What exists

| Piece | Where | What it does |
|---|---|---|
| State block | [include/nxaot/guest_state.h](include/nxaot/guest_state.h) | `dm_guest_state` of design section 2.1, C, 64-byte aligned, every offset asserted; host-only fields `cnt` (the counter `mrs cntpct_el0` / `cntvct_el0` read in a region; a pack calls the host instead) and `exit_reason` / `exit_arg` |
| Pack descriptor | [include/nxaot/aot_module.h](include/nxaot/aot_module.h) | what a pack's library holds for a module (the descriptor, the lookup table, function map and line table, the inline-cache descriptors) and what it imports from the host (the SVC path, the miss path, traps, the counter), C |
| Decoder glue | [src/decode/translate.cpp](src/decode/translate.cpp) | dynarmic's A64 translation loop with nxaot's options (hints raise their exceptions, wall-clock counter) and the encodings dynarmic leaves to an interpreter that nxaot translates itself (`mrs xN, cntvct_el0`) |
| Lifter | [src/lift/lifter.cpp](src/lift/lifter.cpp), [src/lift/opcodes.cpp](src/lift/opcodes.cpp) | dynarmic IR blocks of a region to one LLVM function `void f(dm_guest_state *st)`: registers (x0..x30, SP, v0..v31) in allocas that SROA / mem2reg promote, NZCV as four `i1` compared directly against the operands of the `subs` / logical operation that set them, stored only when written; guest memory as `inttoptr` volatile accesses (alignment 1, 16 bytes for `ldr q`), `ldar` acquire, `stlr` sequentially consistent, exclusives as `cmpxchg` against `st->excl_*`, `dc zva` zeroing the aligned 64 bytes; `svc`, `brk`, `udf` and undefined encodings leave with `st->pc` and `st->exit_reason`. A region holding an IR opcode the lowering does not cover is refused whole |
| Floating point and SIMD | [src/lift/lower_fp.cpp](src/lift/lower_fp.cpp), [lower_vector.cpp](src/lift/lower_vector.cpp), [lower_crypto.cpp](src/lift/lower_crypto.cpp), [function_lifter.h](src/lift/function_lifter.h) | the lowering of section 7: FPCR as a compile-time specialisation, the NaN chains, Arm's NaN, min / max, rounding and conversion rules, saturation with FPSR.QC, the cumulative exception flags, AES-NI / SHA-NI / PCLMULQDQ with software forms ("Floating point and SIMD" below) |
| Run-time helpers | [src/lift/runtime.cpp](src/lift/runtime.cpp) | the operations whose exact Arm semantics are no x86 sequence (the estimates, half-precision arithmetic, FZ fused multiply-add fix-ups, some conversions, software AES): wrappers of dynarmic's own `common/fp` and `common/crypto`, which its backend calls for the same cases |
| ORC JIT | [src/lift/jit.cpp](src/lift/jit.cpp) | LLVM 18 ORC LLJIT, the default O2 pipeline, the host's CPU (`getHostCPUName` and its features) or a named level (`--cpu x86-64-v3`); the helpers as absolute symbols; the x86 assembly of a module (nxaot_bench) |
| Vector harness | [src/vec/](src/vec/) (`nxaot_vec`) | every vector on dynarmic's own A64 JIT (the reference) and as lifted code, from one initial state; compares x0..x30, SP, NZCV, FPCR, FPSR (QC and the cumulative flags), TPIDR_EL0, v0..v31, the exit (pc, reason, svc number) and the 64 KiB memory image byte for byte. It runs lifted code as a host gate would: MXCSR from FPCR before, its flags into FPSR after, and a continuation lifted for the new pc and FPCR after a normal exit inside the code (`msr fpcr`) |
| Vectors | [tests/vectors/](tests/vectors/), written by [tests/gen_vectors.py](tests/gen_vectors.py) and [tests/gen_fp_simd.py](tests/gen_fp_simd.py) | 179,296 vectors: 24,400 integer ones as `.tsv` (encodings written by hand from the Arm ARM), 154,896 floating-point and SIMD ones as `.tsv.gz` (every form of dynarmic's decoder table, fields swept, lanes of special values, 13 FPCR values: the variants of design section 12 and mixes) |
| Benchmark | [src/bench/main.cpp](src/bench/main.cpp), [calls.cpp](src/bench/calls.cpp) (`nxaot_bench`) | the cost of the NaN-chain checks, the x86 instructions lifted code compiles to, the instructions per guest memory access; the two calling conventions on synthetic call chains and on the corpus (time per call, spills), the dispatcher per `blr` hit and miss, a jump table, int_ops natively against dynarmic, the pack stage's throughput per tier ("Measurements") |
| Census | [src/analysis/census.cpp](src/analysis/census.cpp) (`nxaot census`) | phase 0's numbers over a raw code blob as `key<TAB>count` lines |
| Reach | [src/analysis/reach.cpp](src/analysis/reach.cpp) (`nxaot opcodes --reach`) | every entry of dynarmic's A64 decoder table translated with random fields: the opcodes the A64 frontend can emit; `--check` fails when one is not lowered |
| Module loader | [src/module/](src/module/) (`nxaot module`, `nxaot image`, `nxaot manifest`) | an NRO, or a behaviour image with its manifest (design 1.1, 1.2), as one image keyed by its SHA-256 (1.4): segments, MOD0, the dynamic section, symbols, init and fini arrays, RELA / JMPREL / RELR relocations, `.eh_frame_hdr`, FDEs, LSDAs, data blocks in text; the manifest parser and writer ("Module loader and discovery") |
| Discovery | [src/analysis/discover.cpp](src/analysis/discover.cpp), [jumptable.cpp](src/analysis/jumptable.cpp), [summary.cpp](src/analysis/summary.cpp) (`nxaot discover`) | design 1.3 over a whole module: function entries from every source with their provenance, a CFG per function, jump tables on the lifted IR, exclusive windows, the summaries of 2.3, the coverage of the text |
| Reference runner | [src/run/main.cpp](src/run/main.cpp) (`nxaot_run`) | a corpus NRO under dynarmic's A64 JIT with the corpus's two SVCs: the corpus's reference output on x86; `--set` writes a symbol before the run, `--fastmem` gives dynarmic direct memory access (the throughput comparison) |
| Corpus | [corpus/](corpus/) | seven self-checking C programs with a crt0 of their own, built with devkitA64 and with clang into fourteen committed NROs ("The corpus") |
| Function mode | [src/lift/lower_calls.cpp](src/lift/lower_calls.cpp), [lifter.h](src/lift/lifter.h) (`FunctionPlan`, `Convention`) | one guest function as one LLVM function of the convention of design 2.3, swifttailcc or ghccc: calls, returns checked against lr, tail calls, the inline caches, svc as a host call, the stores and reloads the summaries allow ("Calls and returns") |
| Pack stage | [src/build/](src/build/) (`nxaot build`) | the convention facts of a module (PASS, WRITES, LIVE_AFTER, RV, resume points: [plan.h](src/build/plan.h)), every function lifted into one module with the standard entries, the lookup table, the function map, the line table and the descriptor of [include/nxaot/aot_module.h](include/nxaot/aot_module.h); the tiers of design 9.3; an object, and a shared object by lld as a library |
| Native run time | [src/rt/](src/rt/) (`nxaot_host`) | the arena below 2^39, the image at its one-to-one address, the library by `dlopen` with its identity checked, the state block, the SVC path, the dispatcher's miss path, traps and faults through the line table, the counter ("The native run time") |
| Per-function differential test | [src/rt/funcdiff.cpp](src/rt/funcdiff.cpp) (`nxaot_funcdiff`) | every function of a module from one register and memory snapshot under the recompiled code and under dynarmic, final registers and written memory compared (design 12, item 2) |
| Runner | [tests/run_aot_tests.sh](tests/run_aot_tests.sh) | runs the tests and the benchmark and writes `logs/aot/` ([logs/README.md](../../../logs/README.md)) |
| CI | [.github/workflows/aot.yml](../../../.github/workflows/aot.yml) | the image, the build, CTest and the runner on every pull request and push to `main` and `multiplataforma` |

Vendored: [dm/third_party/dynarmic](../../third_party/dynarmic/VENDOR.md), the fork `azahar-emu/dynarmic` at `a46601580d55` (A64 frontend, IR, x86-64
backend and the externals its build needs; 19 MB).

## Building and testing in Docker

From the repository root. The image (about 1.9 GB) has Ubuntu 24.04 pinned by digest, LLVM 18.1.3 with its development packages, lld, CMake 3.28,
Ninja, Python 3.12 and the Boost headers:

```sh
docker build -t nxnative-aot -f docker/Dockerfile.aot docker

# configure, build, CTest (the vectors, the crypto vectors for x86-64-v3, the per-operation NaN mode, the reach check, the census, the vectors-current
# check, discovery over the corpus, the manifest test, the corpus reference run, the fp cases current, the corpus run natively, the per-function
# differential test); --tmpfs /src/build:exec,uid=$(id -u),gid=$(id -g) keeps the build tree in memory on a machine short of disk
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-aot sh -c \
  'cmake -S dm/tools/aot -B build/aot -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/aot && ctest --test-dir build/aot --output-on-failure'

# the same tests and the benchmark through the runner, which writes logs/aot/<test>.log and logs/aot/summary.txt
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-aot sh dm/tools/aot/tests/run_aot_tests.sh build/aot
```

From Git Bash on Windows, the same with `MSYS_NO_PATHCONV=1`, `$(pwd -W)` and no `--user`, as [docker/README.md](../../../docker/README.md) says for
the host. The image is built locally and on CI from [docker/Dockerfile.aot](../../../docker/Dockerfile.aot). A local build has no registry digest, and
its local image ID changes with every rebuild (the build attaches a new attestation manifest), so pinning the image itself by digest needs a push to a
registry first (`docker push`, then the digest it prints), as the llvm-mingw image of the host build is pinned. Its base image is pinned by digest
already, and its packages are the Ubuntu 24.04 versions of the day it is built: `nxaot.lock` and the configure check keep LLVM at 18.1.3.

Linking: `libLLVM-18.so` from the image (the Ubuntu package is built with `LLVM_LINK_LLVM_DYLIB`); `-DNXAOT_LLVM_STATIC=ON` links the component
libraries statically instead, as the distributed `nxaot` will be (design section 13). zlib (which LLVM itself links) reads the `.tsv.gz` vectors; lld's
ELF driver (`liblldELF.a`, the image's `liblld-18-dev`) links the packs. The build tree takes about 74 MB, and the tests' objects and shared objects
(corpus_native, corpus_funcdiff, the benchmark) about 56 MB more. On this repository's development machine (8 threads) the CTest run takes about 9
minutes: the vectors 4 of them, corpus_native and corpus_funcdiff 1.5 each.

## Running the harness and the census

```sh
build/aot/nxaot_vec dm/tools/aot/tests/vectors            # every .tsv and .tsv.gz of the folder; or name files; - reads standard input
build/aot/nxaot_vec --cpu x86-64-v3 dm/tools/aot/tests/vectors/crypto.tsv.gz   # a named target: no AES-NI, SHA or PCLMULQDQ, their software forms
build/aot/nxaot_vec --filter excl_seq --dump dm/tools/aot/tests/vectors/exclusive.tsv   # dynarmic IR and optimised LLVM IR of the matches
build/aot/nxaot_vec --state --filter fp.00612 dm/tools/aot/tests/vectors/fp.tsv.gz      # a mismatch with the initial registers
build/aot/nxaot_vec --nan-mode perop dm/tools/aot/tests/vectors/fp.tsv.gz       # every operation checks its own NaNs (chains: the default)
build/aot/nxaot census <raw ARM64 code file> --base <hex address of its first byte>
build/aot/nxaot opcodes                                    # every dynarmic IR opcode and whether the lifter lowers it
build/aot/nxaot opcodes --reach --check                    # the opcodes the A64 frontend emits, the decoder entries that emit them; all lowered?
build/aot/nxaot_bench                                      # the NaN-chain measurements, then phase 1's ("Measurements")
build/aot/nxaot_bench --sections calls,dispatch,spills,int_ops,pack --work /tmp/x   # phase 1's only
python3 dm/tools/aot/tests/gen_vectors.py                  # write the vectors again after changing a generator
```

`nxaot_vec` prints each mismatch with the vector's file and line, its words, its FPCR and every field that differs, then per class the vectors,
mismatches, refusals, FPSR differences of known dynarmic issues and normal exits, the IR opcodes each refused class needed, the vectors per known
issue, which lowered opcodes the passing vectors exercised, and last `== nxaot_vec: N vectors, M mismatches, U unsupported (listed), F FPSR
differences of known dynarmic issues ==`; it exits with 1 on a mismatch. Options: `--no-fpsr-model` lifts without modelling the cumulative flags
(and compares QC only), `--ignore-fpsr-flags` compares QC only, `--no-filters` compares the whole FPSR without the known-issue relaxations,
`--batch`, `--max-report`, `-v`. The vectors' format (8 tab-separated fields: id, class, text, code, seed, FPCR, register overrides, memory
overrides; `vk=` names the lane kind the seed fills v0..v31 with) and the memory layout (the image at `0x10000000`, code at `0x10008000`) are in
[src/vec/vectors.h](src/vec/vectors.h); the image is mapped at that address in the harness process with guard pages around it, since lifted code uses
guest addresses as host addresses.

**The big sweep.** Phase 2's acceptance asks for 10^8 vectors per instruction class; CI runs the committed set. Any number of random vectors streams
through the harness without files:

```sh
python3 dm/tools/aot/tests/gen_vectors.py --count 1000000 --seed 5 --classes simd_same --seeds-per-word 20 | build/aot/nxaot_vec --batch 1024 -
```

`--classes` takes the file names of the floating-point and SIMD generators (`fp`, `simd_same`, `simd_misc`, `simd_shift`, `simd_perm`, `crypto`,
`simd_ldst`, `fp_seq`) or the class names of their vectors; each round of the generator takes new seeds; `--seeds-per-word` runs each encoding with
that many value sets (and one FPCR), so that the lifter compiles it once. About 2,900 vectors a second per process on the development machine; the
numbers of a sweep are in "Measurements".

The census reads dynarmic's decoder (its matcher names), the Arm ARM's top-level encoding table, and Arm ARM masks for what dynarmic does not decode
(LSE atomics, `ldapr`, `msr` immediate, `hvc` / `smc`, `udf`). Exclusive windows and SP writes are followed on dynarmic's IR of each instruction: a
window is the straight line from an exclusive load to its store (conditional branches fall through, a second exclusive load or an unconditional
branch ends it unpaired), ABA-sensitive when a load inside reads through the loaded value (design 6.5); `sp.from_memory` counts SP set, within a basic
block, from a value a load produced. The `answer.*` keys answer phase 0's questions with 0 or 1.

## Module loader and discovery

```sh
build/aot/nxaot module dm/tools/aot/corpus/nro/switch-gcc.nro          # what the loader read: layout, MOD0, symbols, relocations, FDEs, LSDAs, data blocks
build/aot/nxaot image dm/tools/aot/corpus/nro/switch-gcc.nro /tmp/x    # the behaviour-image form: /tmp/x/switch-gcc.image.bin and switch-gcc.aot.tsv
build/aot/nxaot manifest /tmp/x/switch-gcc.aot.tsv                     # parse a manifest, print it back in canonical form
build/aot/nxaot discover dm/tools/aot/corpus/nro/switch-gcc.nro --tsv /tmp/x/switch.tsv            # discovery: a line per function, the summary
build/aot/nxaot discover /tmp/x/switch-gcc.aot.tsv --symbols-as-oracle                               # no symbol as a source; compared with them
```

**The loader** ([src/module/image.h](src/module/image.h)) reads an NRO as the host's loader does (`dm/host/loader.c`: the header's segments, MOD0 at
the offset in word +4, the dynamic section, `DT_SYMTAB` sized by `DT_HASH`, `DT_GNU_HASH` or the distance to `DT_STRTAB`, `DT_INIT` / `DT_FINI`,
the init, preinit and fini arrays read through their relocations, RELA, JMPREL and RELR), and `.eh_frame_hdr`'s search table with each FDE's CIE
augmentation (`zPLR`), LSDA pointer and the LSDA's call-site table and landing pads, as `dm/tools/nso_code.py` `fde_ranges` reads them. Data blocks
in text: an NRO's header after its entry branch (+4..+0x80: word +4 is MOD0's offset, then the header) and MOD0 with libnx's `LNY` records when it
lies in the text; for a behaviour image, the manifest's `data` rows. A manifest (`<name>.aot.tsv`, every row kind of design 1.2:
[src/module/manifest.h](src/module/manifest.h)) names its image `<name>.image.bin` beside it; the image's SHA-256 must be the one in its header.
`nxaot image` writes that form for an NRO (its bytes unchanged, so the key is the same SHA-256; a manifest with `layout` and `data` rows), so the
tool reads one input form whatever `patch_nso.py --aot` will add.

**Discovery** ([src/analysis/discover.h](src/analysis/discover.h)) takes the entries of design 1.3 in its order and records each entry's provenance
(`fn` rows: every source that named it; the first is its primary one): the module's entry, FDEs, `STT_FUNC` symbols (not with `--symbols-as-oracle`),
`DT_INIT`, `DT_FINI`, the arrays, relocation targets in text; then, from the walks, `bl` targets, `b` / `b.cond` / `cbz` / `tbz` targets outside
their function (tail calls), landing pads outside their function, jump-table targets that are other functions' entries, `adr` and `adrp` + `add`
text addresses that no function's code reaches and no function's FDE or symbol range holds (inside one they are a root of it: a table base, a resume
point), and last the gap sweep (the first word that is not padding of every stretch no function reached; inside a function's own range it is a
root of that function, not an entry). A walk follows a function's direct branches in `[entry, limit)`, the limit being the next entry or the FDE's
end, by dynarmic's decoder table and the Arm ARM's fields ([src/analysis/insn.h](src/analysis/insn.h)); `br x30` is a return (design 3.3), a call to
a function the summaries prove does not return does not fall through, and landing pads are reached from the calls their call-site range covers.
Coverage: every text word outside data blocks is in a function, padding (`nop` or zero) or reported as uncovered; undefined words inside functions
are counted.

**Jump tables** ([src/analysis/jumptable.h](src/analysis/jumptable.h)), by data flow on the lifted IR as design 1.3 asks: for each `br`, the blocks
from one of the br block's dominators (the br block itself, then up the dominator tree) that reach the br are lifted by the lifter into one LLVM
function (`Region::follow`), SROA and EarlyCSE make it SSA, and the br's target (the operand of an `nxaot.indirect` marker the lifter emits,
`Region::mark_indirect`) must be a function of one guest load whose address is a function of one index. The index is bounded at the br by
LLVM's LazyValueInfo and known bits, through casts, constant arithmetic and phis (each lifted instruction reads its registers anew, so `cmp w13`
and the index `w13` are two `trunc x13`: an equal cast on each predecessor's straight line gives the edge's range); the entries are read from the
image (through the relocation of a slot, for tables of pointers in data) and every target must be a decoded instruction of the function or another
function's entry. A region is used only when it is closed: every block of it that reaches the br, but its first, is entered only from inside it.
Regions continue past a call by the callee's summary (`Region::calls`: what it may change is opaque, the rest keeps its value; GCC keeps a table
base in `x18` across a call to a function its interprocedural register allocation knows leaves `x18` alone) and through the function's known tables,
which become LLVM `switch`es (`Region::tables`, the lowering of design section 4). A switch in a loop, whose cases branch back to it, is first
bounded with the code no known edge reaches left out, then verified: with its cases walked, a closed region that includes them must give the same
targets, or the table is dropped. InstCombine is not run: it rewrites the index's `zext (trunc x)` as `and x, mask`, a value the compare does not
name. Shapes covered by the corpus: byte, halfword and word entries, signed (GCC) and unsigned (clang), `adr` bases and table-relative bases (clang's
uncompressed tables), bounds by `cmp` / `b.hi`, by `sub` then `cmp` (offset cases), bases set before calls, switches in loops. A br left unbounded is
a `br` row with its reason, for the dispatcher (design section 4): in the corpus only fnptr's `unops[i % 6](x)`, an indirect tail call whose index is
a remainder computed with a multiply and shifts that no LLVM analysis bounds.

**Exclusive windows** ([src/analysis/summary.cpp](src/analysis/summary.cpp)): from each `ld(a)x(r|p)` the straight line (conditional branches fall
through) to its `st(l)x(r|p)`, never past a second exclusive load (the rewriter's rules); `cas` when no other load is inside (compare-and-swap
semantics are equivalent, design 6.5), `aba` when a load reads through the loaded value, `load` for another load; the others are counted unpaired.
The rewriter's `oracle-W` rows are a subset (its x18 windows) and are not compared yet.

**Summaries** (design 2.3, the facts only): per function the registers it may read before writing them (backward liveness, a call's reads and its
must-writes), may change (everything written, callees included, minus what it restores), must write on every return path, and whether it is
**preserving**: x19..x29, SP and the low halves of v8..v15 hold their entry values at every return, which an abstract interpretation of each
instruction's dynarmic IR checks (registers as entry values plus offsets, SP-relative 8-byte slots, stores through other pointers taken not to reach
the save slots; a `ret` whose `x30` is not the entry's, as longjmp's `br x30`, is a non-local transfer and makes the function open). A fixed point over
the call graph: direct calls by their callee, `blr` and unbounded `br` by the union over the address-taken functions that return, `svc` as a call
that keeps x19 and up. Classes: `preserving`, `open`, `noreturn` (no return path). The address-taken set: entries from relocations, `adr` /
`adrp` pairs, the init and fini arrays and other functions' tables.

`nxaot discover` prints a line per function (`offset size provenance blocks calls indirect windows has_lsda class name`; `indirect` counts `blr` and
unbounded `br`) and `summary` lines; `--tsv` writes every row: `fn` (entry, size, limit, provenance, blocks, calls, unbounded brs, blrs, windows,
has_lsda, reads, writes, must, class, address-taken, exported, name), `jt` (br, function, first entry, width, signed, base, scale, first index,
count, region start, past calls, targets), `br`, `call`, `win`, `pad`, `gap` (uncovered words), `oracle` (with `--symbols-as-oracle`) and the
summary. `NXAOT_DUMP_REGION=<br pc>` prints the optimised IR of every region tried for that br; `NXAOT_DEBUG_REGION=1` the outcome of each.

What is not done: the census numbers of phase 0 over whole modules (FDE coverage, LSDA counts, setjmp imports, the address-taken set: all available
from `nxaot discover` now, not folded into `nxaot census`), `oracle-T` / `oracle-J` / `oracle-W` comparisons (the parser reads them), the
non-local control flow census beyond landing pads (setjmp by symbol, SP from memory), functions split for size (design 9.3), and the measured
throughput on game modules: every region is lifted afresh and the dominators and the fixed points are plain, which the corpus does not stress.

## The corpus

Design section 12, item 3: self-checking programs built as NROs with no game files and no keys, the same programs with devkitA64's GCC and with
upstream clang. [corpus/](corpus/):

| Program | What it checks |
|---|---|
| `int_ops` | integer arithmetic and bit manipulation, compiler-torture style: volatile inputs, each operation against a reference written another way, the corners C leaves undefined (division by zero, `INT_MIN / -1`, shifts by the width or more) in inline assembly, CRC32 / CRC32C, a TLS counter; built without unwind tables, so discovery works without FDEs |
| `switch` | switches lowered to jump tables of every entry size (byte, halfword, word: `switch_word.c`), offset cases, case ranges, fallthrough, a state machine, nested, sparse (a compare tree) and two clusters, checked against tables of function pointers and if chains |
| `fnptr` | vtables and dispatch tables in relocated data, callbacks built with `adrp` + `add`, direct and indirect tail calls, mutual recursion, constructors and destructors (the init and fini arrays, `DT_INIT`), cleanup landing pads (built with `-fexceptions`) |
| `fp` | the operations of design 7.2 over special values, single and double: 5,928 cases whose results [corpus/gen_fp.py](corpus/gen_fp.py) computes from the Arm ARM's pseudocode (`FPAdd`, `FPMulAdd`, `FPRecipEstimate`, `FPToFixed`, `FPRoundInt`, ...) with FPCR = 0 |
| `simd` | NEON intrinsics lane by lane against scalar references, the floating-point vector forms against the scalar instructions |
| `sjlj` | setjmp / longjmp of [corpus/rt/setjmp.S](corpus/rt/setjmp.S): second returns, deep recursion, a handler stack, callee-saved registers |
| `excl` | the C11 atomics of every width as `ldaxr` / `stlxr` loops, compare-and-swap, 128-bit pairs, `clrex`, a spin lock, an ABA-sensitive lock-free pop |

The run time ([corpus/rt/](corpus/rt/)): `crt0.S` lays out an NRO's start as libnx's does (a branch over the header, MOD0's offset at +4, MOD0 in the
text), sets SP to a stack of its own, relocates the module unless a loader did, points `tpidr_el0` at the TLS block, runs `DT_INIT` and the init
array, calls `main` and leaves through `svcExitProcess`; `rt.c` prints one line per `svcOutputDebugString` (`dm/host/hle_svc.c` answers it) and
ends with `PASS <checks>` or `FAIL <failed> of <checks>`; `mem.S` has `memcpy` and its kin, `nro.ld` the layout both linkers read.

```sh
sh dm/tools/aot/corpus/build_corpus.sh            # build both toolchains in Docker, write corpus/nro/ and corpus/corpus.tsv
sh dm/tools/aot/corpus/build_corpus.sh --check    # build into build/corpus/check, compare byte for byte with the committed files
python3 dm/tools/aot/corpus/fetch_devkita64.py /tmp/dkp && DKP_ROOT=/tmp/dkp/opt/devkitpro sh dm/tools/aot/corpus/build_corpus.sh --check
build/aot/nxaot_run dm/tools/aot/corpus/nro/fp-gcc.nro                  # one NRO under dynarmic: its lines
python3 dm/tools/aot/tests/test_corpus_run.py --nxaot-run build/aot/nxaot_run   # every NRO against corpus/expected/<program>.out
python3 dm/tools/aot/corpus/gen_fp.py             # after a change to the fp cases (CTest corpus_fp_current checks fp_cases.h is current)
```

GCC is devkitA64 15.2.0 from `devkitpro/devkita64:20260219` pinned by digest in `build_corpus.sh` (pulled on first use: about 700 MB compressed);
with `DKP_ROOT` the same binaries run in the `nxnative-aot` image from a devkitPro tree, which `fetch_devkita64.py` extracts from the pinned image's
layer (every digest checked, 190 MB) for a machine without the disk room for the image. clang is the image's clang-18 (`--target=aarch64-none-elf`).
Common flags: `-O2 -mcpu=cortex-a57 -mno-outline-atomics -ffreestanding -fPIE -fno-stack-protector -ffp-contract=off -fno-math-errno`, PIE links with
`--export-dynamic --hash-style=sysv --build-id=sha1` (GNU ld: RELA relocations; ld.lld: `-z pack-relative-relocs`, RELR); `corpus.tsv` lists
every NRO with its compiler, flags and SHA-256. [tests/elf2nro.py](tests/elf2nro.py) writes the NROs (and the symbol tables `test_discover.py` compares
with), byte for byte what devkitPro's `elf2nro` writes, which `build_corpus.sh` checks on every build. The builds are reproducible: no timestamps,
paths relative to the corpus folder, build IDs over the content; `--check` rebuilds byte-identically.

Run so far: under dynarmic (`nxaot_run`, CTest `corpus_ref`) and recompiled on x86-64 (`nxaot build` and `nxaot_host`, CTest `corpus_native`: both
conventions, the correctness mode and the max mode): all fourteen NROs print `PASS`, the GCC and clang builds of each program the same lines, recorded
in [corpus/expected/](corpus/expected/), and the native runs exactly what dynarmic prints. Not run: natively on ARM64 hardware (an Apple-silicon Mac;
nothing here can). `int_ops` reads an exported `int_ops_rounds` (1 in the committed output) that a runner may set before the run (`--set
int_ops_rounds=N`): its operation loop then runs N times, which the throughput measurement uses.

## Calls and returns

```sh
build/aot/nxaot build dm/tools/aot/corpus/nro/fnptr-gcc.nro -o /tmp/x/fnptr.o --shared /tmp/x/fnptr.so   # balanced (tier W), swifttailcc
build/aot/nxaot build ... --cc ghc --mode max --correctness --all-entries --cpu x86-64-v3 --spills --asm /tmp/x/fnptr.s
```

`nxaot build` ([src/build/pack.cpp](src/build/pack.cpp)) runs discovery, plans the convention ([src/build/plan.cpp](src/build/plan.cpp)) and lifts
every function in **function mode** ([src/lift/lower_calls.cpp](src/lift/lower_calls.cpp)): one LLVM function per guest function, named
`<module>+0x<offset>`, its blocks those its entry and its landing pads reach inside `[entry, limit)` (a block that runs to the limit continues in the
next function by a tail call). Design 2.3, 3.3 and 4, as implemented:

- **The convention's facts** (`FnFacts`): PASS = x0..x3 and the passable registers the function may read (its summary's `reads`); WRITES = the
  summary's `writes`, plus v8..v15 when the function writes them at all (a `ldr d8` restore zeroes the high half, which callers must not keep);
  LIVE_AFTER, a fixed point over every call site of the module (what is read before being written after the call; a tail call passes on its own
  function's; an indirect site counts for every standard entry; the host reads x0, x1, v0..v3); RV, the v0..v3 returned in host registers: the
  same over every chain of tail calls (a component of the tail-call graph; every standard entry is in one), the ones every returning member writes.
- **Registers** live in allocas as in a region; at entry the passed ones come from the arguments, the rest from the block (unused loads go). A
  forward data flow over the function's blocks gives the **dirty** registers (newer than their block copy) at every block end. A call stores
  dirty - PASS(callee) - {sp, lr}, then takes x0, x1 and RV from the return value and reloads WRITES(callee); through the dispatcher it also stores
  what it passes that the union of the possible callees may change (`CalleeView::exact`: the callee that runs stores only its own writes). A return
  stores dirty & LIVE_AFTER & WRITES (not the returned registers), and the registers it restores whose block copy it changed before an inner call
  (design 2.3: "stores the restored value again"); a tail call stores dirty - PASS(callee) and what its callers reload that the callee passes back
  unchanged; svc, traps and non-local returns store every dirty register, so the block then holds the whole register file. Preserving callees keep
  x19..x29 and SP in SSA across the call. **The correctness mode** (`--correctness`) stores every register at every call, tail call and return,
  reloads every register after every call and takes nothing from arguments or return values: the reference the tests compare with.
- **Two conventions** (`--cc`): **swifttailcc** (the default) with `st` as `swiftself` (R13), sp, lr, x0..x3 in RDI, RSI, RDX, RCX, R8, R9, x4..x7
  on the stack and v0..v7 in XMM0..7, each function's signature carrying only what it reads (the callee pops its argument area, so `musttail`
  between different prototypes is allowed); **ghccc**, the stand-in for preserve_none: LLVM 18.1, which `nxaot.lock` pins, has no `preserve_nonecc`
  (it came with LLVM 19), and ghccc is the convention preserve_none generalises (every general register carries arguments, none is callee-saved): one
  uniform signature, st, sp, lr, x0..x6 in R13, RBP, R12, RBX, R14, RSI, RDI, R8, R9, R15, v0..v5 in XMM1..6 (x7, v6, v7 through the block), frame
  pointers off. Both return `{x0, x1, v0..v3}` in RAX, RDX, XMM0..3. swifttailcc is the default from the numbers ("Measurements"): calls and returns
  cost the same in both (2.2 ns, 5.4 TSC cycles, for a leaf), ghccc reloads 10 to 30 % more on the corpus (no callee-saved host registers across
  calls), and one loop was twice as slow under it; ghccc is 14 % faster on a chain of frame-building calls and on an inline-cache hit.
- **Returns** (3.3): `ret` (and `br x30`, `ret xN`) compares its target with the lr the function was entered with; equal, a native `ret` (the
  compare folds away where x30 is never written); else the whole register file to the block, the target in `st->pending` and a native return.
  After every call one compare and branch on `st->pending`: a resume point of the call site (the LSDA landing pad covering it; every setjmp
  continuation of the function, setjmp found by symbol) entered with this frame's SP takes it, reloading every register; otherwise the frame
  returns at once, and the host dispatches what reaches it as a new call (a target with no compiled entry stops with the JIT-needed status: the
  resume of a continuation in the middle of a function is the JIT's, phase 3). The sjlj program's longjmps (deep recursion, through a function
  pointer, nested handlers, callee-saved registers) all resume this way.
- **Calls and tail calls**: `bl` is a call of the callee's body with `lr = base + pc + 4`; `b`, `b.cond`, `cbz`, `tbz` to another function and a
  fall into the next one are `musttail` calls; a call the summaries say does not return traps if it returns.
- **Indirect branches** (4): a recovered jump table is an LLVM `switch` over module offsets (the target minus the base), a native jump table, its
  other values going to the dispatcher; `blr` and unbounded `br` (fnptr's `unops[i % 6](x)` among them) go through a per-site **inline cache**:
  the slot points at an immutable `{guest, native}` descriptor; a hit is the slot's load, the guest address's load and a compare (and the native
  entry's load beside it), a miss calls the host through a `preserve_all` wrapper (`nxaot.dispatch`), which looks the target up in the module's
  **lookup table** and publishes a descriptor in the slot. Every host-callable function (address-taken, the module's entry, exported; every
  function with `--all-entries`) has a **standard entry** with the module's one signature (x0..x3 and what some address-taken function reads), which
  stores what its body does not take and tail-calls the body.
- **Addresses built from the pc** (adr, adrp, literal loads, return addresses, the pcs of svc and exceptions) are base-relative: the module runs
  wherever the host maps it (`*desc->base`, one invariant load per function). Each block is translated a second time 2^44 higher, and every 64-bit
  immediate that moved by exactly that much is such an address, lowered as `base + offset`.
- **Host calls**: `svc` stores the dirty registers and `st->pc`, calls `dm_aot_svc(st, n)` (the C convention) and reloads x0..x8 (2.4); `mrs
  cntpct_el0` is a `preserve_most` call of `dm_aot_cntpct` (8); `brk`, `udf`, undefined words, branches into the middle of other functions and `msr
  fpcr` leaving the specialisation call `nxaot_rt_trap` with a reason (`include/nxaot/aot_module.h`). A function the lifter refuses becomes a body
  that traps to the JIT (none in the corpus).
- **Tables in the object** ([include/nxaot/aot_module.h](include/nxaot/aot_module.h)): `.nxaot.lookup` (open addressing, module offset ->
  standard entry, self-relative), `.nxaot.fnmap` (every function's body and size), `nxaot_lines` (the line table, 3.5: every guest memory access's
  native address and guest offset, written by LLVM's `!pcsections`; LLVM 18 keeps those on memory instructions only, so call sites map through the
  DWARF line table, line = guest offset in a synthetic `<module>.s`, `--no-debug-info` drops it) and the exported descriptor `nxaot_module`.
- **Tiers** (9.3, `--tier`): C (SROA, instcombine, simplifycfg, DCE; FastISel), W (LLVM's O1 pipeline, codegen Less), H (O2), H+ (O3, codegen
  Aggressive). Without a profile the pack modes are one tier for every function: `balanced` W, `max` H+. One tier per module for now.

Found by the tests and fixed in the lowering: a preserving callee that stored its own x29 before an inner call left that copy in the block, which a
later callee saved in its frame record (nxaot_funcdiff, summary mode: the missing "store the restored value again" rule); an indirect call reloaded
x3 from the block after the call because some address-taken function writes x3, while the leaf that ran stored nothing (nxaot_bench's blr loop never
ended: the `exact` rule above).

## The native run time

```sh
build/aot/nxaot_host /tmp/x/fnptr.so dm/tools/aot/corpus/nro/fnptr-gcc.nro       # the lines, as nxaot_run prints them; exit status of the run
build/aot/nxaot_host /tmp/x/int_ops.so dm/tools/aot/corpus/nro/int_ops-gcc.nro --set int_ops_rounds=200 --quiet --repeat 5
build/aot/nxaot_host /tmp/x/fnptr.so dm/tools/aot/corpus/nro/fnptr-gcc.nro --relocate --set hidden_ptr=@0x1844   # a target with no entry: exit 3
build/aot/nxaot_host /tmp/x/fnptr.so dm/tools/aot/corpus/nro/fnptr-gcc.nro --lookup     # the module offsets the lookup table holds
build/aot/nxaot build dm/tools/aot/corpus/nro/fnptr-gcc.nro -o /tmp/x/all.o --shared /tmp/x/all.so --all-entries --correctness
build/aot/nxaot_funcdiff /tmp/x/all.so dm/tools/aot/corpus/nro/fnptr-gcc.nro --seeds 3   # every function against dynarmic
python3 dm/tools/aot/tests/test_corpus_native.py --nxaot build/aot/nxaot --nxaot-host build/aot/nxaot_host --nxaot-run build/aot/nxaot_run --work /tmp/x
python3 dm/tools/aot/tests/test_funcdiff.py --nxaot build/aot/nxaot --nxaot-funcdiff build/aot/nxaot_funcdiff --work /tmp/x [--cc ghc]
```

[src/rt/host.h](src/rt/host.h), the minimal form of what the host will do (design 2.4, 3.3, 4, 5, 6.5, 8, 9.4), as a library (`nxaot_rt`) that
nxaot_host, nxaot_funcdiff and nxaot_bench share. Decision against 9.4: the pack is a shared object linked by lld as a library inside `nxaot build`,
and the host loads it with `dlopen`, as the design's host loads the pack's library (no LLVM in the host; the library's imports resolve against the
executable, which exports them).

- **The arena** (5.2): 8 GiB reserved at 2^36 (64 GiB, below 2^39) with `MAP_FIXED_NOREPLACE`, nothing accessible until mapped: the host-return
  address `arena + 0x10` (no code), the main thread's guest stack (1 MiB at +16 MiB), its TLS block, a scratch area for nxaot_funcdiff, and the
  module's image at +256 MiB, its one-to-one guest address (5.1): text and read-only data readable and never executable (5.3: a jump into guest
  text faults), data and bss writable. State blocks and native stacks are outside it.
- **Loading** (9.4): the library's descriptor is checked against the image (ABI, SHA-256, sizes; a mismatch stops with a red line and status 2),
  `*base` is written, the line table and the function map are read. `--relocate` applies the module's relocations as dm/host's loader does (the
  corpus's crt0 then skips its own); `--set` writes a symbol.
- **A run**: a state block (x0 = 0, x1 = -1 as nxaot_run, x30 = the host-return address, SP and `tpidr_el0` the host's stack and TLS block, the
  monitor clear: 6.5, single-threaded, `st->excl_*` the local monitor), MXCSR of the module's FPCR (7.1), then `enter(st, standard entry of
  _start)`; a non-local transfer that reaches the host is dispatched as a new call.
- **What the library imports**: `dm_aot_svc` (svcOutputDebugString prints a line, svcExitProcess ends the run; other SVCs stop with status 5),
  `nxaot_rt_dispatch` (the miss path: the lookup table, a descriptor per target published with a release store; no entry: `[RED] aot: the indirect
  branch at module+0x... to 0x... (module+0x...) has no compiled entry: the JIT fallback (design section 11) is phase 3` and status 3),
  `nxaot_rt_trap` (a red line per reason; brk and undefined words status 1, the JIT's cases status 3), `dm_aot_cntpct` (CLOCK_MONOTONIC as 19.2 MHz
  ticks, `guest_ns * 12 / 625` as `svcGetSystemTick`), and the floating-point helpers of [src/lift/runtime.cpp](src/lift/runtime.cpp).
- **Faults** (5.4 is the host's job): SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGTRAP in guest code, on an alternate stack, are reported with the
  faulting address and the guest instruction the line table gives (`[RED] aot: SIGSEGV at address ... in guest code (host pc ..., in the function at
  module+0x..., guest instruction module+0x...)`), status 4. Dirty tracking (write-protected pages resumed after the fault) is dm/host's.
- **Exit statuses** (`rt::Status`): 0 svcExitProcess, 1 trap, 2 usage or a library that does not match, 3 the JIT fallback is needed, 4 fault, 5
  an SVC the corpus run time does not answer, 6 a return where none may be (the guest returned to the host from _start, a no-return callee came
  back), 7 timeout and 8 stopped at an svc (nxaot_funcdiff).

**The per-function differential test** (`nxaot_funcdiff`, CTest `corpus_funcdiff`, design 12 item 2 in its simple form): a pack built with
`--all-entries`; after the host relocated the image, every function runs three times from one snapshot under the recompiled code and under dynarmic
(whose memory callbacks allow exactly what the native mapping allows), with registers drawn from a seed (pointers into data, the scratch area and
the stack, small integers, and non-canonical values that fault at once in both). Runs that end the same way are compared: in the correctness mode
every register (x0..x30, SP, NZCV, v0..v31) and every written page, in the summary mode what the convention gives a caller (x0, x1, v0..v3 at a
return, x0..x7 at an svc) and every written page. Runs that leave for the JIT (a `blr` to a random value), fault in both, or time out are counted
apart.

What dm/host must provide for the integration: the arena allocator (`dm_guest_alloc`, 5.2) and the module loader's mapping into it, `dm_guest_call`
on top of `enter` (2.4), `svc_dispatch` behind `dm_aot_svc` (the block's `x` is its register array), the host-function page's addresses in the
lookup path (`dm_aot_hostcall` for targets inside the page), thread creation with a state block and a native stack per thread, the counter and the
dirty-tracking fault hook, and the JIT fallback of section 11 behind `nxaot_rt_dispatch`'s and `nxaot_rt_trap`'s JIT cases.

## Floating point and SIMD

Design sections 7.1 to 7.4, as lowered here.

- **FPCR is a compile-time specialisation (7.1).** `LiftOptions::fpcr` names the FPCR a region is compiled for; only its mode bits count (AHP, DN,
  FZ, RMode, FZ16: `kFpcrModeMask`, what dynarmic's location descriptor keeps), and the blocks are translated with it. Guest code runs with the
  MXCSR of that FPCR (`lift::GuestMxcsr`: RMode as the rounding control, FZ as FTZ and DAZ, every exception masked), which the host sets once when a
  thread enters guest code and nxaot_vec sets around each run; lifted functions never load MXCSR themselves. LLVM is told the rounding mode through
  constrained intrinsics with that static mode, so that nothing is folded in another mode, and FZ through `denormal-fp-math`. Chosen over a
  `ldmxcsr` / `stmxcsr` pair at every function entry and exit: the pair costs tens of cycles per call and serialises, and every function of a module
  shares one FPCR anyway; with round to nearest and the flags unmodelled the operations are plain LLVM ones and optimise freely. `msr fpcr` writes
  `st->fpcr` and the new mode bits into MXCSR, and its block leaves the function when the mode bits differ from the specialisation, for the
  dispatcher to continue in code compiled for the new value (the JIT of section 11 later; nxaot_vec lifts a continuation).
- **FPSR (7.4).** `LiftOptions::fpsr_flags` models the cumulative exception flags exactly: the function is `strictfp` and every floating-point
  operation is a constrained intrinsic with strict exception behaviour (none removed, speculated or folded). Native operations raise their flags in
  MXCSR, which `mrs fpsr` and the host's gate read (`lift::FpsrFromMxcsr`); operations lowered in integers raise none of their own and add the Arm
  bits explicitly (conversions to integers: IOC, IXC; FZ: IDC per flushed operand); the helpers OR theirs into `st->fpsr`. QC is a flag of the
  function stored at exit, from every saturating operation, scalar or vector, as dynarmic ORs `fpsr_qc`. A pack sets `fpsr_flags` for modules where
  the census finds an FPSR read (`answer.fpsr_read`), as decision 5 says; nxaot_vec always sets it.
- **NaN chains (7.3).** The P operations (add, subtract, multiply, divide, square root, fused multiply-add with FMA3 and FZ = 0, negate, absolute
  value, single / double conversions, pairwise add; scalar and vector) are chain nodes (`ChainNode`): the native result, and how to rebuild the Arm
  result from the operands' exact values. A use outside the chain (a register write that survives the block, a store, an integer or another
  floating-point operation that keeps NaN bits) takes the node's escaped value: one NaN check (`ucomiss` / `vcmpunordps` and a branch weighted
  cold); when it fires, a cold block recomputes the chain from its inputs with Arm's rules (`FPProcessNaNs`, the fused multiply-add's default NaN
  and IOC for a quiet NaN addend with infinity times zero, FPCR.DN) in integer operations. With FPCR.DN an arithmetic node escapes branch-free (the
  default NaN where the fast value is a NaN), except a chain that holds a fused multiply-add when the flags are modelled: x86 raises nothing for
  that invalid case (known issue 16), so it takes the check and the cold block. Compares and conversions to integer read fast values directly. Chains span instructions because the lifter
  forwards vector registers within a block (`ForwardVectorRegisters`, as dynarmic's get/set elimination does) and drops a write the block
  overwrites. `NaNMode::PerOp` checks every operation instead (dynarmic's way, the reference of the measurements; CTest `vectors_perop` runs the
  scalar and sequence vectors with it); `NaNMode::Unchecked` is inexact and exists only for measuring.
- **The operations of rows 4 to 11 have their own exact lowering**, with quiet compares only so that quiet NaNs raise nothing: FMAX / FMIN
  (AND / OR of equal operands, NaN rules), FMAXNM / FMINNM (a single quiet NaN loses), FMULX (0 times infinity is 2.0), FRECPS / FRSQRTS (FMA3,
  and dynarmic's software step when a lane is a NaN or near infinity), rounding to integral (roundss without Inexact, ties away from zero by an exact
  trunc-and-compare sequence, FRINTX by `rint`), conversions to integer and fixed point (in double precision, saturation and NaN by compares and
  selects, IOC / IXC computed; ties away included), from integer (one rounding in the current mode, unsigned 64-bit values halved with a sticky bit).
  The estimates (FRECPE, FRSQRTE, FRECPX), half-precision arithmetic, FZ fused multiply-add results of +-(smallest normal) and conversions in a mode
  other than FPCR's or with AHP / FZ16 call the run-time helpers. F16C converts halves.
- **Integer SIMD** is generic LLVM vector IR (shuffles, `smax` / `sadd.sat` and friends, widening and narrowing, `pshufb` per register for TBL /
  TBX, table lookups for URECPE / URSQRTE); polynomial multiplication is PCLMULQDQ for PMULL 1Q or a shift-and-XOR sequence.
- **Cryptography:** AESE / AESD as AESENCLAST / AESDECLAST with a zero key, AESMC as AESDECLAST then AESENC, AESIMC; SHA256H / H2 / SU0 / SU1 with
  SHA-NI in dynarmic's register arrangement; SHA-1 is generic IR (dynarmic's frontend expands it); SM4's S-box is a table. Without AES-NI the
  helpers, without SHA the lifter runs dynarmic's SHA-256 polyfill on the block first, without PCLMULQDQ the generic sequence. `Target` holds what the
  target CPU has (`Jit::TargetFeatures`); x86-64-v3 is the baseline (FMA3, F16C, AVX2), and AES-NI, SHA and PCLMULQDQ are outside it.
- **64-bit vector forms (2S):** when an operation's only uses keep the low 64 bits, a division divides 1.0 by 1.0 in the upper lanes and the
  estimates skip them, so that the zero upper lanes raise nothing (`LowHalfOnly`; known issue 15).

What is not done: the slow path of design 7.2 row 13 for add / subtract / multiply / divide / square root under FZ (they follow dynarmic, see the
end of "Known issues"); the half-precision estimates and steps are scalar helper calls per lane; FRECPE / FRSQRTE are helper calls (a table-based
inline form is an optimisation for later); one combined check for all the escapes of a block (four checks of a 4x4 matrix multiply cost about 1.4
ns of its 5 per iteration).

## Measurements

`nxaot_bench` on the development machine (Intel Core i9-13950HX, LLVM's `alderlake`, 2,000,000 iterations, best of 5, Docker): ns per iteration of
each kernel's loop, and the x86 instructions of one iteration lifted alone, all of them and the hot ones (the blocks a run takes when no value is a
NaN). Timings vary by about 10 % from run to run on this machine.

| Kernel | Mode | ns / iteration | x86 instructions | hot |
|---|---|---:|---:|---:|
| 4x4 matrix multiply (`ld1` x 2, 4 x (`fmul` + 3 `fmla` by element), `st1`) | chains | 5.2 | 1,118 | 91 |
| | per operation | 9.5 | 2,651 | 149 |
| | unchecked | 3.7 | 63 | 63 |
| 8 dependent `fmla v0.4s` | chains | 13.8 | 267 | 23 |
| | per operation | 14.4 | 1,288 | 56 |
| | unchecked | 14.0 | 16 | 16 |
| 4 `fmadd s` + `fadd` + `fmul` + `str s` | chains | 7.9 | 581 | 41 |
| | per operation | 7.6 | 879 | 45 |
| | unchecked | 6.6 | 22 | 22 |

The chain checks cost 40 % of the matrix multiply (four escaping 4-lane results, one `vcmpunordps` / `vmovmskps` / `test` / `jne` each, and the
spills the cold recomputation causes), nothing measurable in a long dependent `fmla` chain (one check per eight operations), and 19 % in the scalar
loop; checking every operation instead costs 2.6 times the unchecked matrix multiply. The cold blocks are most of the code size. With the flags
modelled (strict floating point) the times are within the noise of these. dynarmic's JIT runs the same loops at 70 to 90 ns per iteration, but its
memory goes through callbacks here (no fastmem), so that number is no comparison of the backends.

Section 6.4 (each guest memory access one x86 instruction): the kernel `ldr q8, [x0, #16]; str q8, [x1, #32]; ldp q9, q10, [x0, #32]; ld1 {v12.4s,
v13.4s}, [x1]; st1 {v12.s}[2], [x2]; ldr d14, [x0, #24]; str s14, [x1, #20]` (`3dc00408 3d800828 ad412809 4c40a82c 4d00804c fd400c0e bd00142e`) makes
nine guest accesses (the pair and the two-register `ld1` two each), and its lifted function nine x86 memory instructions: `vmovups 16(%rax)`,
`vmovups %xmm0, 32(%rcx)`, `vmovups 32(%rax)`, `vmovups 48(%rax)`, `vmovups (%rcx)`, `vmovups 16(%rcx)`, `vextractps $2, %xmm3, (%rdx)`,
`movq 24(%rax)`, `movl %eax, 20(%rcx)` (`nxaot_bench --asm`; the other instructions are the state block's loads and stores).

The big sweep (`gen_vectors.py --count 1000000 --seed S --classes C --seeds-per-word 20 | nxaot_vec --batch 1024 -`, once per class, four or
five processes at a time), vectors, mismatches, FPSR differences of known issues (by issue number), seconds: `fp` 1,000,000, 0, 65,727 (8: 26,903,
9: 26,338, 10: 1,380, 11: 620, 12: 10,486), 255; `simd_same` 1,000,000, 0, 17,311 (11: 36, 12: 12,990, 13: 2,961, 15: 1,324), 265; `simd_misc`
1,000,000, 0, 70,847 (8: 21,313, 9: 28,873, 10: 2,172, 12: 8,310, 14: 7,334, 15: 2,845), 250; `simd_shift` 1,000,000, 0, 18,055 (9: 17,133,
12: 922), 226; `simd_perm` 1,000,000, 0, 7,866 (11: 453, 12: 7,413), 262; `crypto` 1,000,000, 0, 0, 289; `simd_ldst` 1,000,000, 0, 0, 897.
Between 21 % and 38 % of the vectors of the data-processing classes are undefined encodings (the field sweep includes the reserved values),
which both executors must agree on. `fp_seq` did not complete: its first run found the FMA bug of known issue 16 (vector 28,302 of seed 1008)
and was stopped for the fix; the second ended without a result (its container was removed from outside the run). The first run of
`simd_ldst` found that the generator left `sp`-based addresses at the seed's random `sp`, which can lie within 256 bytes of the image's ends
(both executors faulted on the same address); the generator now sets `sp` as it sets other base registers. `simd_misc`, `simd_shift` and
`crypto` ran before the FMA fix, which does not reach them.

### Phase 1: calls, the dispatcher, a jump table, throughput

`nxaot_bench --sections calls,dispatch,spills,int_ops,pack` on the same machine (Intel Core i9-13950HX, LLVM's `alderlake`, Docker, Linux without a
GPU), every module built by `nxaot build` into a shared object and run by the native run time; synthetic programs of x3 = 20,000,000 iterations,
best of 5. The TSC runs at 2.419 GHz (the cores turbo well above it: a TSC cycle is about two core cycles here). In the W rows every guest function
is kept out of line (`noinline`), so that a call is a call; the H+ rows are built as the max mode builds (LLVM inlines the leaf and the tail chain:
0). ns per operation, minus the baseline's time per iteration (the loop alone, 0.03 ns: LLVM unrolls its doubling), divided by the operations per
iteration; blr_hit against the direct call, blr_miss against blr_hit.

| Program | swifttail W | ghc W | swifttail H+ | ghc H+ | What one operation is |
|---|---:|---:|---:|---:|---|
| leaf | 2.22 ns, 5.4 cycles | 2.23 ns, 5.4 | 0 (inlined) | 0 | `bl` to `add x0, x0, x0; ret`: a call and a return |
| chain8 | 2.58 ns, 6.2 | 2.23 ns, 5.4 | 1.34 ns, 3.2 | 1.20 ns, 2.9 | one of 8 nested calls, each function with a frame record (`stp` / `ldp x29, x30`) |
| saved | 3.55 ns, 8.6 | 3.75 ns, 9.1 | 1.95 ns, 4.7 | 1.96 ns, 4.8 | an iteration: a call to a function that saves and uses x19, x20, with x19..x22 live across it |
| tail4 | 2.20 ns, 5.3 | 2.22 ns, 5.4 | 0 | 0 | a call into 3 tail calls (`b`) and a return |
| blr_hit | +0.91 ns, 2.2 | +0.70 ns, 1.7 | 3.07 ns, 7.4 | 2.61 ns, 6.3 | `blr` to the same leaf: the inline cache hits (W: over a direct call) |
| blr_miss | +22.1 ns, 54 | +22.9 ns, 55 | +19.7 ns, 48 | +22.0 ns, 53 | `blr` alternating between two targets: every call misses (the dispatcher's lookup and the slot's update) |
| switch8 | 2.53 ns, 6.1 | 5.42 ns, 13.1 | 2.95 ns, 7.1 | 2.89 ns, 7.0 | an iteration through a switch of 8 cases (a byte table, `br`), a native jump table |

Spills and reloads of the register allocator (tier W), the text, and the run time of the program natively (best of 5, warm):

| NRO | Convention | Spills | Reloads | Text bytes | Call and tail-call sites | Run ms |
|---|---|---:|---:|---:|---:|---:|
| fnptr-gcc | swifttail | 169 | 309 | 31,437 | 93 | 0.005 |
| | ghc | 161 | 382 | 29,002 | | 0.005 |
| fnptr-clang | swifttail | 210 | 266 | 27,581 | 74 | 0.005 |
| | ghc | 208 | 346 | 25,498 | | 0.005 |
| switch-gcc | swifttail | 314 | 646 | 205,373 | 102 | 0.144 |
| | ghc | 328 | 741 | 202,506 | | 0.138 |
| switch-clang | swifttail | 361 | 629 | 57,245 | 73 | 0.100 |
| | ghc | 377 | 697 | 54,490 | | 0.109 |

The conventions cost the same per call and return; ghccc has no callee-saved host registers, so what lives across a call is spilled to the native
stack (10 to 30 % more reloads on the corpus), and its fixed argument registers took one loop (switch8) to twice the time; it is faster on a deep
chain of frame-building calls (no callee-saved registers to push in each) and on an inline-cache hit. swifttailcc is the default. preserve_none
(LLVM 19) is the convention to measure again when nxaot.lock moves.

Integer throughput, `int_ops-gcc.nro` with `int_ops_rounds = 200` (398,275,757 guest instructions as dynarmic counts them; best of 5; the native
and the dynarmic runs print the same lines):

| Executor | Seconds | Guest instructions per second |
|---|---:|---:|
| native, swifttailcc, balanced | 0.0611 | 6,514 M |
| native, ghccc, balanced | 0.0612 | 6,504 M |
| dynarmic's JIT, memory callbacks (`nxaot_run`) | 0.4256 | 936 M |
| dynarmic's JIT, fastmem (`nxaot_run --fastmem`) | 0.2631 | 1,514 M |

The recompiled code runs int_ops 4.3 times as fast as dynarmic with fastmem, 7 times as fast as with memory callbacks.

The pack stage over the whole corpus (14 NROs, 86,141 guest instructions, one thread, swifttailcc; guest instructions per second of each stage;
`switch-gcc`, 51,097 of them, dominates):

| Tier | Analysis (discovery, census, plan) | Lifting | Optimisation and code generation | Link (lld) per NRO | Text | Peak memory |
|---|---:|---:|---:|---:|---:|---:|
| C | 37,988 | 156,921 | 23,570 | 0.005 s | 1,465 KiB | 232 MiB |
| W (balanced) | 38,005 | 148,691 | 6,700 | 0.006 s | 742 KiB | 232 MiB |
| H | 37,823 | 150,710 | 6,535 | 0.006 s | 733 KiB | 232 MiB |
| H+ (max) | 37,525 | 157,530 | 6,199 | 0.005 s | 775 KiB | 232 MiB |

These are far below design section 10's estimates (at least 10 million for analysis, 2 to 5 million for lifting, 150,000 to 300,000 for tier C,
50,000 to 100,000 for tier W): at these rates Monster Hunter Rise's 31 million instructions would take about 95 minutes on one core in the balanced
mode (77 of them optimisation and code generation), against the target of 10 minutes on 4 cores. Where the time goes,
per stage of `switch-gcc` at tier W: discovery 1.0 s (jump-table regions lifted afresh per br, summaries that translate each instruction again per
pass), lifting 0.5 s, optimisation 1.4 s, code generation 3.7 s (DWARF line info costs 5 to 10 % of it). Code generation dominates; the functions
carry an alloca per register, the pending check after every call and the NaN-chain cold blocks. Shards in parallel, the per-function cache and the
caching of the analyses (design 10, 13) are phase 5's; the numbers are the measured starting point.

## The lock file

[nxaot.lock](nxaot.lock) names the LLVM release (`18.1.3`) and the dynarmic commit. The configure step stops when the LLVM package differs, or when
[VENDOR.md](../../third_party/dynarmic/VENDOR.md) names another commit; the lifter checks `LLVM_VERSION_STRING` against it at compile time. The hashes
of the prebuilt `nxaot` binaries go there later (design section 13).

## Boost

dynarmic includes `boost/icl/interval_map.hpp`, `interval_set.hpp`, `boost/variant.hpp` and `variant/get.hpp` (headers only). The decision was a vendored
header-only subset made with `bcp`. **Temporary deviation, for the lead to record:** the subset `bcp` makes of those four headers from Boost 1.83
(Ubuntu 24.04's `libboost-tools-dev`) is 21 MB, over the 15 MB allowed (preprocessor 9.6 MB, mpl 4.7 MB, type_traits 1.3 MB, intrusive 0.96 MB,
icl 0.8 MB, the rest under 0.7 MB each; `bcp` follows every `#include` of every configuration), so the image installs the distribution's Boost headers
(`libboost-dev`, in a layer of its own) and there is no `dm/third_party/boost/` yet. An option measured for the decision: the headers this build actually
includes (`clang -M` over every compile command) are 795 files, 4.2 MB (mpl 224, type_traits 157, preprocessor 110, icl 97, variant 31, range 31);
such a list is specific to the compilers and configurations it was taken with (MSVC or llvm-mingw may need more of `boost/config` and mpl's
preprocessed headers).

## Lifter coverage

`nxaot opcodes` lists the 725 opcodes of dynarmic's IR: 589 lowered and 136 that never appear in an A64 block: 133 of the AArch32 frontend (its own;
the packed, `ssat` / `usat`, GE, carry and overflow opcodes only it emits; and 34 floating-point and SIMD ones: the widening multiply pseudo-operations
`VectorSignedMultiply` / `VectorUnsignedMultiply` with `GetUpperFromOp` / `GetLowerFromOp`, the 64-bit lane integer max / min / multiply / reduce /
sign extension, `VectorEqual128`, VTBL's `VectorTableLookup64`, the 16-bit fixed-point and the vector half-precision conversions of VCVT, and
`SignedSaturatedAddWithFlag32` / `SubWithFlag32`) and 3 dynarmic-internal ones (`Breakpoint`, `CallHostFunction`, `GetNZFromOp`). `nxaot opcodes
--reach --check` (CTest `opcodes_reach`) proves the split: it translates 655,956 words filled from every entry of dynarmic's A64 decoder table
under four FPCR values, finds the 576 opcodes the A64 frontend emits, and fails when one is not lowered. The vectors exercise 587 of the 588 lowered
opcodes other than `Void` (`nxaot_vec` prints the list after each run); the one left is `Identity`, which the identity-removal pass leaves unused before
the lifter looks. Apart from those two, an opcode is in the lowered set only when a passing vector reaches it. CRC32C is `crc32` (SSE4.2);
CRC32 (IEEE) is a 256-entry table a byte at a time (a PCLMULQDQ form is an optimisation for later). `dmb` of every kind is `fence seq_cst`.

Vectors per file of the committed set (all pass; `nxaot_vec` prints them per class): `fp.tsv.gz` 14,856 (with `fma_invalid` 456),
`simd_same.tsv.gz` 42,000, `simd_misc.tsv.gz` 39,000, `simd_shift.tsv.gz` 15,600, `simd_perm.tsv.gz` 13,800, `crypto.tsv.gz` 9,600,
`simd_ldst.tsv.gz` 14,040, `fp_seq.tsv.gz` 6,000; the integer `.tsv` files 24,400 (`dp_imm` 4,200, `dp_reg` 7,400, `branch` 2,200, `loadstore`
4,900, `exclusive` 1,800, `system` 2,100, `sequence` 1,400, `undefined` 400).

## Known issues

Places where dynarmic, the reference, departs from the Arm ARM or from what the harness can compare. Each is handled as said; none is patched in the
vendored copy.

1. **`msr nzcv` then `mrs nzcv` in one block.** dynarmic's `A64GetSetElimination` pass forwards the whole written register to the read
   (`ir/opt/a64_get_set_elimination_pass.cpp:67`), where NZCV bits 27:0 read as 0. Encoding: `d51b4200 d53b4201` (`msr nzcv, x0; mrs x1, nzcv`):
   with the pass, 56 of the 300 `msr_mrs` vectors differed (x1 = 0xfe947a5a from the reference, 0xf0000000 lifted, for x0 = 0xfe947a5a).
   The reference runs without that pass (`src/vec/reference.cpp`); the JIT fallback (design section 11) must too, or the pass be fixed upstream.
   The lifter forwards vector registers only, in a pass of its own (`ForwardVectorRegisters`).
2. **`dc zva` at an unaligned address.** With `hook_data_cache_operations` off, dynarmic's `A64CallbackConfigPass` zeroes 64 bytes from the address
   itself (`ir/opt/a64_callback_config_pass.cpp:32`), not from the aligned block that holds it (read in the code, not run). Encoding: `d50b7420`
   with an unaligned `x0`. The reference takes the hook and zeroes the aligned block, as the Arm ARM's `dc zva` does.
3. **FPSR after a block lookup.** dynarmic's dispatcher compiles a block it misses with the guest's MXCSR loaded (`backend/x64/block_of_code.cpp:370`,
   `:449`), so its own floating point (hash-map load factors) can leave IXC in the guest's cumulative flags: the first vector, `94000001` (`bl +4`),
   ended with FPSR 0x10, and `d65f03c0` (`ret x30`) after another `ret` to a different target did again. The harness now runs every vector twice
   on the reference and compares the second run, which finds every block of its path compiled (`src/vec/reference.cpp`, `Reference::Run`), and
   compares the whole final FPSR; `--ignore-fpsr-flags` compares QC alone, should the issue come back. The JIT fallback inherits the issue.
4. **FPCR bits.** `msr fpcr` keeps the bits dynarmic keeps (`FPCR_MASK = 0x07C89F00`, `backend/x64/a64_jitstate.cpp:51`), which include FZ16 and the
   trap enables a Cortex-A57 does not implement [A: the A57 reads them as 0]. The lifter uses the same mask (`kFpcrMask`), so both differ from the A57
   alike; FPSR keeps QC and the cumulative flags (0x0800009F) on both. The trap enables reach neither executor's floating point: code is compiled
   for the mode bits only, and dynarmic's software operations, which assert on an enabled trap, get the same (the helpers here too).
5. **Branch targets.** dynarmic's location descriptors keep 56-bit PCs, sign-extended from bit 55 (`frontend/A64/a64_location_descriptor.h:28`), so a
   `br` to a non-canonical address leaves the reference at another pc than the lifted code: `d61f0020` (`br x1`) with x1 = 0x123456789abcdef0 ends at
   0x003456789abcdef0 in the reference and at x1 in the lifted code. That is the Arm ARM's result when top-byte-ignore is on for EL0 (BranchAddr), not
   when it is off. The vectors branch to canonical addresses; what the recompiler does with tagged branch targets is an open question of the design.
6. **Hints.** dynarmic's A64 JIT passes only two translation options (`backend/x64/a64_interface.cpp:274`), so `yield`, `wfe`, `wfi`, `sev` and
   `sevl` always raise `ExceptionRaised`, whatever `hook_hint_instructions` says. The reference treats them as hints and goes on; the lifter emits
   `pause` for the first three.
7. **What dynarmic leaves to an interpreter.** `mrs xN, cntvct_el0` (dynarmic translates `cntpct_el0` only): the decoder glue translates it and the
   reference's interpreter callback does the same. LSE atomics, `ldapr`, `hvc` / `smc` and `msr` immediate end a block as untranslated on both sides.

The issues below are FPSR only: dynarmic's x86-64 backend raises other cumulative exception bits than the Arm ARM's pseudocode for some
operations, and the lowering follows the Arm ARM. The lifter notes the issues a region meets in the forms dynarmic takes them
(`lift::DynarmicIssue`, `Lifted::dynarmic_issues`), and nxaot_vec relaxes only those FPSR bits, only in the direction listed, for such a vector,
and only when the FPSR is all that differs (`src/vec/main.cpp`, `KnownIssues`; it prints the vectors per issue; `--no-filters` turns it off).
Values, QC and the other registers are compared exactly in every case.

8. **FRINTN / FRINTP / FRINTM / FRINTZ / FRINTI raise Inexact.** dynarmic's native path is `roundss` / `roundps` with the rounding immediate but
   without the precision-suppression bit (`backend/x64/constants.h:162`), so an inexact result raises PE, IXC. The Arm ARM's `FPRoundInt` with
   `exact` FALSE raises no Inexact; only FRINTX does. Example: `1e644020` (`frintn d0, d1`) with d1 = 1.5: reference FPSR 0x10, Arm and lifted 0.
   Reference-only IXC.
9. **Conversions to integer and fixed point, dynarmic's native forms** (FCVTNS / NU / PS / PU / MS / MU / ZS / ZU, scalar and vector, and the
   fixed-point FCVTZS / FCVTZU; `backend/x64/emit_x64_floating_point.cpp:1623`, `emit_x64_vector_floating_point.cpp:2012`). They
   clamp with `minsd` / `maxsd` or compare-and-select before `cvttsd2si`, so a saturated result raises no Invalid Operation (positive overflow of
   the signed forms, negative values to the unsigned ones, a quiet NaN zeroed by a quiet compare); unsigned 64-bit conversions convert the operand
   with the signed `cvttsd2si` too, which raises Invalid Operation for values in [2^63, 2^64) that convert exactly; negative values to unsigned
   are clamped to 0 before rounding, so Inexact is lost or IXC appears where Arm raises IOC; and the fixed-point forms scale by 2^fbits in the
   source precision, which overflows (OFC, IXC) for large operands where Arm scales exactly. Examples: `9e581ccd` (`fcvtzs x13, d6, #57`) with
   d6 = 0x43efffffffffffff saturates: Arm IOC, reference nothing; `9e390100` (`fcvtzu x0, s8`) with s8 = 0x5f7ffffe (1.8e19, exact):
   reference IOC, Arm nothing. Reference-only IOC / IXC / OFC, lifted-only IOC / IXC.
10. **Conversions dynarmic does in software** (FCVTAS / FCVTAU, the half-precision ones): its `FPToFixed` (`common/fp/op/FPToFixed.cpp:41`)
    raises Invalid Operation for any negative nonzero operand of an unsigned conversion, also one that rounds to 0, where the Arm ARM's
    `FPToFixed` rounds first (0, no overflow) and raises Inexact. Example: `9e250196` (`fcvtau x22, s12`) with s12 = 0x9be5733f (-3.8e-22):
    reference IOC, Arm IXC. Reference-only IOC, lifted-only IXC.
11. **FMADD / FMSUB / FNMADD / FNMSUB with DN = 0.** When the native result is a NaN, dynarmic's NaN path multiplies the two factors
    (`vmuls`, `emit_x64_floating_point.cpp:745`) to test for infinity times zero; the product's Inexact, Overflow and Underflow reach the flags,
    which the Arm ARM's `FPMulAdd` (a NaN addend is the result) does not raise. Example: `1f020c20` (`fmadd s0, s1, s2, s3`) with s1 = s2 =
    1.1 and s3 a quiet NaN: reference IXC. Reference-only IXC / OFC / UFC.
12. **No Input Denormal under FZ.** dynarmic's native operations flush denormal operands through MXCSR.DAZ, which raises nothing; the Arm ARM's
    `FPUnpack` raises Input Denormal (IDC) for each operand it flushes. The lowering raises IDC as Arm does (`FunctionLifter::InputDenormal`),
    as dynarmic's own software paths also do. Example: `1e222820` (`fadd s0, s1, s2`) with s1 = 0x00000001 under FZ: Arm IDC, reference nothing.
    Lifted-only IDC.
13. **FMAX / FMIN / FMAXNM / FMINNM of vectors raise Invalid Operation for quiet NaNs.** dynarmic computes `maxps` / `minps` on every lane,
    which signal for any NaN operand (`emit_x64_vector_floating_point.cpp:950`); the Arm ARM signals for signalling NaNs only (the scalar
    forms are right: they test with `ucomiss` first). Example: `4e22f420` (`fmax v0.4s, v1.4s, v2.4s`) with a quiet NaN lane in v1: reference IOC.
    Reference-only IOC.
14. **FRSQRTE (single, double).** dynarmic's native estimate truncates the operand and computes `1 / sqrt` with `sqrtss` / `divss`
    (`emit_x64_floating_point.cpp:1182`), which raises Inexact; its truncation turns +infinity into a signalling NaN that its `ucomiss` test then
    signals (Invalid Operation); and its `rsqrtss` path for zero, negative, denormal (FZ) and NaN operands raises nothing where the Arm ARM raises
    Divide by Zero, Invalid Operation or Input Denormal. Values agree. Examples: `7ea1d9f6` (`frsqrte s22, s15`) with s15 = +inf: reference IOC,
    Arm nothing; `7ea1d817` with s0 = 0x0000000b under FZ: Arm IDC and DZC, reference nothing. Reference-only IXC / IOC, lifted-only IOC / DZC /
    IDC.
15. **FDIV / FRECPE / FRSQRTE of 64-bit vectors (2S).** dynarmic computes all four lanes, and the zero upper lanes the frontend's `GetD` supplies
    give 0 / 0 (Invalid Operation) and estimates of 0 (Divide by Zero). The Arm ARM computes two lanes; the lowering computes the upper lanes
    on 1.0 (division) or not at all (estimates) when the result's only uses keep the low 64 bits (`FunctionLifter::LowHalfOnly`). Example:
    `2e22fc20` (`fdiv v0.2s, v1.2s, v2.2s`): reference IOC always. Reference-only IOC / DZC.
16. **A quiet NaN addend with infinity times zero.** The Arm ARM's `FPMulAdd` returns the default NaN and raises Invalid Operation; x86's FMA returns
    the quiet NaN and raises nothing. dynarmic's vector NaN correction sets the default NaN without the flag
    (`emit_x64_vector_floating_point.cpp:1299`), and with DN = 1 its scalar and vector forms are `vfmadd` alone (`emit_x64_floating_point.cpp:638`,
    `emit_x64_vector_floating_point.cpp:1326`); its scalar forms with DN = 0 raise it through the multiply that tests for infinity times zero
    (`vmuls`, `emit_x64_floating_point.cpp:745`, issue 11). The lowering raises IOC in every form (`FunctionLifter::MulAddInvalid`; with DN a chain
    that holds an FMA takes the cold block instead of the default-NaN select when the flags are modelled). Examples:
    `4ea5cc81` (`fmls v1.4s, v4.4s, v5.4s`) with quiet NaN addends, v4 = 0, v5 = +inf: Arm IOC, reference nothing; `1f450881`
    (`fmadd d1, d4, d5, d2`) with d2 a quiet NaN, d4 = 0, d5 = +inf under DN: the same. Found by the big sweep (`fp_seq`, `fmadd` with DN = 0
    under FPCR.RZ: the lifted run raised nothing and the reference IOC, a lowering bug, now fixed); the committed class `fma_invalid` (in
    `fp.tsv.gz`) covers every form. Lifted-only IOC.

Shared with dynarmic, not filtered, and open: x86 detects tininess after rounding and the Arm ARM before, so a result that rounds up to the
smallest normal raises UFC on Arm and not here (FZ = 0); with FZ = 1, x86 flushes after rounding and Arm before, so add, subtract, multiply,
divide and square root of such a result give the smallest normal here and zero on Arm (design 7.2, row 13). The brief of phase 2 asks the
FZ handling to follow dynarmic's: fused multiply-add gets the software slow path dynarmic has (`FPMulAdd` for a result of +-smallest normal), the
other operations take x86's FTZ as dynarmic does. Random vectors do not hit these boundaries; a slow path for them is a later, measured change.

## What is not done yet

The phases of design section 14, and where this folder stands:

| Phase | Content | Here |
|---|---|---|
| 0. Census | the numbers per module | `nxaot census` over raw code; NROs and behaviour images are read (`nxaot module`) and FDE coverage, LSDA counts and the address-taken set come out of `nxaot discover`, not yet as census keys; NSO decompression (LZ4) and setjmp imports are not done |
| 1. Skeleton | decoding, discovery, lifting of integer / memory / branch / call code, state block, dispatcher, SVC path, vector harness, calling-convention benchmarks | **done** except what follows: the lifter (0 mismatches over 24,400 integer vectors), the module loader, discovery, the summaries; function mode with the convention of 2.3 in two LLVM mappings (swifttailcc, the default by the measurements, and ghccc for preserve_none, which LLVM 18.1 lacks), calls, returns checked against lr, tail calls, `pending` propagation with resume points at setjmp continuations and landing pads, the correctness mode; jump tables as switches, inline caches, standard entries, the lookup table, the miss path; `nxaot build` (tiers C, W, H, H+; the balanced and max modes; an object with the lookup table, function map and line table, a shared object by lld); `nxaot_host` (the arena below 2^39, the SVC path, the counter, faults through the line table); the corpus natively in both conventions and the correctness mode, equal to dynarmic and to the expected output; nxaot_funcdiff on every corpus function; the measurements ("Measurements"). Not done: the corpus natively on ARM64 hardware (an Apple-silicon Mac), safepoints (6.2: phase 3), the resume of a continuation that has no native frame and every other JIT case (phase 3: the run stops with status 3), one tier per function and profiles (phase 5), ThinLTO and the cache, functions split for size, the oracle-T / J / W comparisons, threads (one state block, one thread), the pack throughput targets of section 10 (measured far below them) |
| 2. FP and SIMD | every FP, NEON and crypto lowering, NaN chains | every opcode the A64 frontend emits lowered; 0 mismatches over the committed vectors and 7,000,000 random floating-point and SIMD ones (10^6 per class for seven of the eight classes, `fp_seq` not completed; not the 10^8 of the acceptance); the chain checks measured. Not done: the row-13 FZ slow path for the basic operations, inline estimates, combined escape checks, the run-time helpers in the host |
| 3. Control flow and JIT | resume points, `pending`, safepoints, dynarmic integration | next. In place from phase 1: `pending` propagation, resume points of setjmp continuations and landing pads, the host's dispatch of what reaches it; the JIT's cases stop the run with a [RED] line and status 3 (`nxaot_rt_trap`, `nxaot_rt_dispatch`) where `dm_jit_run` will continue |
| 4. to 6. | games on the Intel Mac, performance, breadth | not started |
