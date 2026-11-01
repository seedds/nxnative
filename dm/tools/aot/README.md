# nxaot: the ahead-of-time ARM64 to x86-64 recompiler

Phase 2 of the Windows and Linux port ([migration-status.md](../../../docs/design/migration-status.md)); the design is
[aot-recompiler.md](../../../docs/design/aot-recompiler.md). This folder holds its foundation and its floating point: the build, the lifter of the
integer, memory, branch, system-register, exclusive, floating-point, Advanced SIMD and cryptographic instruction classes (every IR opcode dynarmic's
A64 frontend emits), the differential vector harness against dynarmic, a benchmark of the NaN chains, and the census of phase 0; and phase 1: the
module loader (NROs and behaviour images with their manifests), function discovery over whole modules with jump tables recovered on the lifted IR,
exclusive windows and interprocedural summaries, a corpus of self-checking homebrew NROs with no game files, whole functions lifted with the calling
convention of design 2.3 (calls, returns checked against lr, real tail calls, non-local returns), the dispatcher of section 4 (jump tables as
switches, inline caches, the lookup table), the pack stage (`nxaot build`: one object and one shared object per module, linked by lld as a
library), and a minimal native run time (`nxaot_host`); and phase 3: the JIT fallback of design section 11 on the vendored dynarmic, unmodified
(what has no compiled entry runs in it and comes back to compiled code at the next compiled entry), non-local control flow through native frames
and through the JIT (C++ exceptions entering LSDA landing pads, fibers, a return past the caller), guest threads on host threads with Horizon's
thread and synchronization SVCs, and the safepoints of section 6.2. The eighteen corpus NROs run natively on x86-64, with everything in the JIT and
with no JIT, and print what dynarmic prints. No game runs through it yet, and nothing here touches the Apple builds.

## What exists

| Piece | Where | What it does |
|---|---|---|
| State block | [include/nxaot/guest_state.h](include/nxaot/guest_state.h) | `dm_guest_state` of design section 2.1, C, 64-byte aligned, every offset asserted; host-only fields `cnt` (the counter `mrs cntpct_el0` / `cntvct_el0` read in a region; a pack calls the host instead) and `exit_reason` / `exit_arg` |
| Pack descriptor | [include/nxaot/aot_module.h](include/nxaot/aot_module.h) | what a pack's library holds for a module (the descriptor of ABI 3, the lookup table, function map and line table, the gate sites, the standard entries' signature, the inline-cache descriptors) and what it imports from the host (the SVC path, the miss path, traps, the gates, the counter), C |
| Decoder glue | [src/decode/translate.cpp](src/decode/translate.cpp) | dynarmic's A64 translation loop with nxaot's options (hints raise their exceptions, wall-clock counter) and the encodings dynarmic leaves to an interpreter that nxaot translates itself (`mrs xN, cntvct_el0`) |
| Lifter | [src/lift/lifter.cpp](src/lift/lifter.cpp), [src/lift/opcodes.cpp](src/lift/opcodes.cpp) | dynarmic IR blocks of a region to one LLVM function `void f(dm_guest_state *st)`: registers (x0..x30, SP, v0..v31) in allocas that SROA / mem2reg promote, NZCV as four `i1` compared directly against the operands of the `subs` / logical operation that set them, stored only when written; guest memory as `inttoptr` volatile accesses (alignment 1, 16 bytes for `ldr q`), `ldar` acquire, `stlr` sequentially consistent, exclusives as `cmpxchg` against `st->excl_*`, `dc zva` zeroing the aligned 64 bytes; `svc`, `brk`, `udf` and undefined encodings leave with `st->pc` and `st->exit_reason`. A region holding an IR opcode the lowering does not cover is refused whole |
| Floating point and SIMD | [src/lift/lower_fp.cpp](src/lift/lower_fp.cpp), [lower_vector.cpp](src/lift/lower_vector.cpp), [lower_crypto.cpp](src/lift/lower_crypto.cpp), [function_lifter.h](src/lift/function_lifter.h) | the lowering of section 7: FPCR as a compile-time specialisation, the NaN chains, Arm's NaN, min / max, rounding and conversion rules, saturation with FPSR.QC, the cumulative exception flags, AES-NI / SHA-NI / PCLMULQDQ with software forms ("Floating point and SIMD" below) |
| Run-time helpers | [src/lift/runtime.cpp](src/lift/runtime.cpp) | the operations whose exact Arm semantics are no x86 sequence (the estimates, half-precision arithmetic, FZ fused multiply-add fix-ups, some conversions, software AES): wrappers of dynarmic's own `common/fp` and `common/crypto`, which its backend calls for the same cases |
| ORC JIT | [src/lift/jit.cpp](src/lift/jit.cpp) | LLVM 18 ORC LLJIT, the default O2 pipeline, the host's CPU (`getHostCPUName` and its features) or a named level (`--cpu x86-64-v3`); the helpers as absolute symbols; the x86 assembly of a module (nxaot_bench) |
| Vector harness | [src/vec/](src/vec/) (`nxaot_vec`) | every vector on dynarmic's own A64 JIT (the reference) and as lifted code, from one initial state; compares x0..x30, SP, NZCV, FPCR, FPSR (QC and the cumulative flags), TPIDR_EL0, v0..v31, the exit (pc, reason, svc number) and the 64 KiB memory image byte for byte. It runs lifted code as a host gate would: MXCSR from FPCR before, its flags into FPSR after, and a continuation lifted for the new pc and FPCR after a normal exit inside the code (`msr fpcr`) |
| Vectors | [tests/vectors/](tests/vectors/), written by [tests/gen_vectors.py](tests/gen_vectors.py) and [tests/gen_fp_simd.py](tests/gen_fp_simd.py) | 179,296 vectors: 24,400 integer ones as `.tsv` (encodings written by hand from the Arm ARM), 154,896 floating-point and SIMD ones as `.tsv.gz` (every form of dynarmic's decoder table, fields swept, lanes of special values, 13 FPCR values: the variants of design section 12 and mixes) |
| Benchmark | [src/bench/main.cpp](src/bench/main.cpp), [calls.cpp](src/bench/calls.cpp) (`nxaot_bench`) | the cost of the NaN-chain checks, the x86 instructions lifted code compiles to, the instructions per guest memory access; the two calling conventions on synthetic call chains and on the corpus (time per call, spills), the dispatcher per `blr` hit and miss, a jump table, int_ops natively against dynarmic, the pack stage's throughput per tier; the safepoint polls' cost, the JIT fallback's hand-over, round trip, cost per block and instance creation ("Measurements") |
| Census | [src/analysis/census.cpp](src/analysis/census.cpp) (`nxaot census`) | phase 0's numbers over a raw code blob as `key<TAB>count` lines |
| Reach | [src/analysis/reach.cpp](src/analysis/reach.cpp) (`nxaot opcodes --reach`) | every entry of dynarmic's A64 decoder table translated with random fields: the opcodes the A64 frontend can emit; `--check` fails when one is not lowered |
| Module loader | [src/module/](src/module/) (`nxaot module`, `nxaot image`, `nxaot manifest`) | an NRO, or a behaviour image with its manifest (design 1.1, 1.2), as one image keyed by its SHA-256 (1.4): segments, MOD0, the dynamic section, symbols, init and fini arrays, RELA / JMPREL / RELR relocations, `.eh_frame_hdr`, FDEs, LSDAs, data blocks in text; the manifest parser and writer ("Module loader and discovery") |
| Discovery | [src/analysis/discover.cpp](src/analysis/discover.cpp), [jumptable.cpp](src/analysis/jumptable.cpp), [summary.cpp](src/analysis/summary.cpp) (`nxaot discover`) | design 1.3 over a whole module: function entries from every source with their provenance, a CFG per function, jump tables on the lifted IR, exclusive windows, the summaries of 2.3, the coverage of the text |
| Reference runner | [src/run/main.cpp](src/run/main.cpp) (`nxaot_run`) | a corpus NRO under dynarmic's A64 JIT with the corpus's SVCs (output, exit, the thread and synchronization SVCs, its guest threads as contexts switched round robin on one JIT: a deterministic interleaving): the corpus's reference output on x86; `--set` writes a symbol before the run, `--fastmem` gives dynarmic direct memory access (the throughput comparison) |
| Corpus | [corpus/](corpus/) | nine self-checking programs (eight in C, one in C++) with a crt0 of their own, built with devkitA64 and with clang into eighteen committed NROs ("The corpus") |
| Function mode | [src/lift/lower_calls.cpp](src/lift/lower_calls.cpp), [lifter.h](src/lift/lifter.h) (`FunctionPlan`, `Convention`) | one guest function as one LLVM function of the convention of design 2.3, swifttailcc or ghccc: calls, returns checked against lr, tail calls, the inline caches, svc as a host call, the stores and reloads the summaries allow ("Calls and returns") |
| Pack stage | [src/build/](src/build/) (`nxaot build`) | the convention facts of a module (PASS, WRITES, LIVE_AFTER, RV, resume points: [plan.h](src/build/plan.h)); compile units of neighbouring functions (groups at tiers C and W, shards at H and H+; a function over the size limit split into parts joined through the state block), lifted, optimised and compiled on a pool of workers, one LLVM context each; the tables' object (the lookup table, the function map, the line table's bounds, the descriptor of [include/nxaot/aot_module.h](include/nxaot/aot_module.h), jit_entry and the slow-path wrappers); the tiers of design 9.3, by profile with `--profile`; a shared object (and a relocatable object) by lld as a library ("Pack throughput") |
| Caches | [src/cache/](src/cache/), [src/build/analysis_cache.cpp](src/build/analysis_cache.cpp) (`nxaot build --cache`) | design 13: each unit's object under the SHA-256 of everything its functions' code depends on, and the module's analysis (discovery's view and the plan) under the module's, its manifest's and the tool's; a warm repack neither analyses nor compiles ("Pack throughput") |
| Game-shaped benchmark | [tests/gen_bench.py](tests/gen_bench.py) | a generated program of thousands of functions of the kinds and sizes game code has, built as an NRO with the corpus's run time: the pack stage's throughput measured where the corpus is too small ("Pack throughput") |
| Native run time | [src/rt/](src/rt/) (`nxaot_host`) | the arena below 2^39, the image at its one-to-one address, the library by `dlopen` with its identity checked, the state blocks, the SVC path, the dispatcher's miss path, traps and faults through the line table, the counter ("The native run time") |
| JIT fallback | [src/rt/jit.h](src/rt/jit.h), [jit.cpp](src/rt/jit.cpp), the driver in [host.cpp](src/rt/host.cpp) | design 11: dynarmic's A64 JIT per guest thread behind `dm_jit_create` / `dm_jit_run` / `dm_jit_halt`, stopping at compiled entries and compiled call sites' returns with dynarmic unmodified, the hand-over of the register file both ways, the driver that takes compiled entries natively from it and propagates non-local transfers ("The JIT fallback") |
| Threads and safepoints | [src/rt/host.cpp](src/rt/host.cpp), the polls in [lower_calls.cpp](src/lift/lower_calls.cpp) | design 6.1 to 6.5: guest threads on host threads with a state block and a native stack each, Horizon's thread and synchronization SVCs, the shared exclusive monitor; the polls of 6.2 and the pause protocol of dm/host's `SetThreadActivity`, with a pause test (`nxaot_host --pause-test`) ("Threads and safepoints") |
| Per-function differential test | [src/rt/funcdiff.cpp](src/rt/funcdiff.cpp) (`nxaot_funcdiff`) | every function of a module from one register and memory snapshot under the recompiled code and under dynarmic, final registers and written memory compared (design 12, item 2) |
| Runner | [tests/run_aot_tests.sh](tests/run_aot_tests.sh) | runs the tests and the benchmark and writes `logs/aot/` ([logs/README.md](../../../logs/README.md)) |
| Host run time | [dm/host/aot.h](../../host/aot.h), `dm/host/aot_*.c`, CTest `t_aot_host` | what dm/host provides for packs: the arena below 2^39, packs loaded, the SVC path, the dispatcher, `dm_guest_call`, guest threads with safepoints on hle_thread.c, faults, the floating-point helpers in C; the corpus run through the host's loader, SVC dispatcher and thread layer ("Threads and safepoints") |
| Game modules | `patch_nso.py --aot`, dm/host/loader.c `load_modules`, CTest `t_aot_module`, `t_guest_map` | the recompiler's input written by the rewriter (a behaviour image and its manifest) and read by the host: the image checked and mapped, gates registered, imports bound into the GOT, the pack loaded; guest memory announced to the graphics backend ("A game module in the host") |
| CI | [.github/workflows/aot.yml](../../../.github/workflows/aot.yml) | the image, the build, CTest and the runner on every pull request and push to `main` and `multiplataforma`; then the corpus packed, `t_aot_host` in the host's image and with the JIT fallback here, and the floating-point differential test |

Vendored: [dm/third_party/dynarmic](../../third_party/dynarmic/VENDOR.md), the fork `azahar-emu/dynarmic` at `a46601580d55` (A64 frontend, IR, x86-64
backend and the externals its build needs; 19 MB).

## Building and testing in Docker

From the repository root. The image (about 1.9 GB) has Ubuntu 24.04 pinned by digest, LLVM 18.1.3 with its development packages, lld, CMake 3.28,
Ninja, Python 3.12 and the Boost headers:

```sh
docker build -t nxnative-aot -f docker/Dockerfile.aot docker

# configure, build, CTest (the vectors, the crypto vectors for x86-64-v3, the per-operation NaN mode, the reach check, the census, the vectors-current
# check, discovery over the corpus, the manifest test, the corpus reference run, the fp cases current, the corpus run natively, the per-function
# differential test, the JIT fallback's three modes, the threaded programs, the safepoint pause test, the pack caches, the indirect calls'
# reloads and the standard entries' signature, the line tables, functions split for size, tiers by profile); --tmpfs
# /src/build:exec,uid=$(id -u),gid=$(id -g) keeps the build tree in memory on a machine short of disk
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
(corpus_native, corpus_funcdiff, corpus_jit, corpus_threads, safepoints, the benchmark) about 30 to 60 MB more. On this repository's development
machine (8 vCPUs) the CTest run takes about 11 minutes (639 s with ABI 3, 18 tests): the vectors 4 of them, corpus_native 1.4 and corpus_funcdiff
1.7, corpus_jit, corpus_threads and safepoints 33 s together, cache 20 s, plan 2 s and lines 25 s. Phase 5b's two (20 tests): split 43 s and
profile 7 s; with `ctest -j2` the whole run took 342 s.

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
tool reads one input form for NROs and game modules.

**Game modules: `patch_nso.py --aot`** ([dm/tools/patch_nso.py](../patch_nso.py), "The recompiler's input"; [rewriter.md](../../../docs/reference/rewriter.md#the-recompilers-input---aot)).
`patch_nso.py <NSO> <out dir> <name> --aot <aot dir>` rewrites the module as before, Apple's outputs byte for byte unchanged, and also writes
`<aot dir>/<name>.image.bin` and `<name>.aot.tsv`. The image is the NSO's text, read-only data and data as the rewriter decompressed them (the LZ4 of
the NSO is read there, as for Apple's `<name>.patched.bin`: neither the host nor nxaot decompresses anything), with the registered and matched cuts
and the frame step's branches applied, before any of Apple's rewrites. The manifest has every row kind of design 1.2 in the canonical order of
`WriteManifest`, so `nxaot manifest` prints it back unchanged. Where it, the design and [src/module/manifest.cpp](src/module/manifest.cpp) differ,
`manifest.cpp` is the format:

| Row | Design 1.2 | Written |
|---|---|---|
| the command | `patch_nso.py --aot <NSO> <out dir> <name>`, the image instead of `patched.bin` | `--aot <dir>` after the usual arguments, besides Apple's outputs: one rewrite gives both, and a test checks that Apple's outputs are unchanged |
| `layout` | `bss` | the zero bytes after `image_size`; 0 when the image holds them (a module with FRAME_STEP, below) |
| `aux` | the stubs "at the same offset" as in the patch area | the behaviour image has no patch area: the build's own FRAME_STEP function assembles its stubs again at the page after the state page (state page at `FPS_STATE_OFF`, as on Apple), and the image goes on to their end, bss and the key page's place as zeros. A module whose bss puts that past a `b`'s reach of the text (a FAR module) fails the rewrite at the `B()` assertion: the manifest's unsigned offsets cannot place the region before the base. nxaot lifts the region's code as part of the functions whose walks branch into it ("Calls and returns", The aux region) |
| `data` | `nso_code.data_blocks` | those, the MOD0 offset word at +4, and MOD0 with its `LNY` records when the text holds it, as nxaot reads an NRO (`NroDataBlocks`) |
| `obs` | `<number> <parameter>` | the OBS number's low 8 bits (decimal) and its bits 8 up (hex), as hle.c's gate table splits them |
| `oracle-J` | `<base> <width>` | `sites.tsv`'s J rows hold the landing pads' count (256, or the halfword table's entries), not the width: the rewriter records the entries' width (1 or 2) for the manifest |
| `oracle-W` | `<lo> <hi>` | `sites.tsv`'s W rows: `hi` is past the `cbz` / `cbnz` that follows the store when one does; nxaot's windows end past the store |

Checked on a synthetic module, with no game files ([dm/tools/tests/aot_synth.py](../tests/aot_synth.py): an NSO of hand-assembled AArch64, every
encoding checked with llvm-mc; its patch file has a cut, an observation gate and a frame step), by [test_aot_manifest.py](../tests/test_aot_manifest.py):
every row against the image's and the NSO's bytes; where capstone and lz4 are installed (tests.yml's tool tests; neither Docker image has them),
`patch_nso.py` with and without `--aot` (Apple's outputs identical) and the image and manifest equal to `aot_synth.py`'s; with `NXAOT=<nxaot>` (this
image, aot.yml) the manifest's round trip, nxaot's discovery against the oracle rows (design 12, item 5: the byte table's base and width, the offset
table's targets index by index, the window's start, its end four bytes short as said above, 0 words uncovered) and `nxaot build`. nxaot does not
compare the oracle rows itself at pack time yet.

**Discovery** ([src/analysis/discover.h](src/analysis/discover.h)) takes the entries of design 1.3 in its order and records each entry's provenance
(`fn` rows: every source that named it; the first is its primary one): the module's entry, FDEs, `STT_FUNC` symbols (not with `--symbols-as-oracle`),
`DT_INIT`, `DT_FINI`, the arrays, relocation targets in text; then, from the walks, `bl` targets, `b` / `b.cond` / `cbz` / `tbz` targets outside
their function (tail calls), landing pads outside their function, jump-table targets that are other functions' entries, `adr` and `adrp` + `add`
text addresses that no function's code reaches and no function's FDE or symbol range holds (inside one they are a root of it: a table base, a resume
point), and last the gap sweep (the first word that is not padding of every stretch no function reached; inside a function's own range it is a
root of that function, not an entry). A behaviour image's aux region (the frame step's stubs) is code of every function whose direct branches
reach it, its blocks part of that function's walk, never an entry of its own. A walk follows a function's direct branches in `[entry, limit)`, the limit being the next entry or the FDE's
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
The rewriter's `oracle-W` rows are a subset (its x18 windows), compared on the synthetic module by test_aot_manifest.py ("Game modules" above).

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
from `nxaot discover` now, not folded into `nxaot census`), the `oracle-T` / `oracle-J` / `oracle-W` comparisons at pack time (the parser reads
them; test_aot_manifest.py compares them on the synthetic module), the
non-local control flow census beyond landing pads (setjmp by symbol, SP from memory), functions split for size (design 9.3), and bounds of an
index computed from more than one value (a switch over `(a * b) >> 37 & 31`: the br goes to the dispatcher, and its cases run in the JIT fallback;
gen_bench.py's benchmark has 383 such brs). Discovery's cost on game-shaped modules is in "Pack throughput".

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
| `excl` | the C11 atomics of every width as `ldaxr` / `stlxr` loops, compare-and-swap, 128-bit pairs, `clrex`, a spin lock, an ABA-sensitive lock-free pop; then with threads ([corpus/rt/thread.h](corpus/rt/thread.h)): two workers released together (`svcWaitForAddress` / `svcSignalToAddress`) increment shared counters through an `ldaxr` / `stlxr` loop, a compare-and-swap loop, an `ldaxp` / `stlxp` pair and a libnx-style mutex (`svcArbitrateLock` / `svcArbitrateUnlock` when contended), 100,000 times each, with the exact totals checked; a third thread runs the register-invariant loop of [corpus/rt/pause.S](corpus/rt/pause.S) (`excl_pause_iterations`, exported, 20 million by default) that the pause test checks; the main thread sleeps (`svcSleepThread`), releases and joins them (`svcWaitSynchronization`) |
| `except` | C++ exceptions through the LSDA tables (design 3.4), [corpus/src/except.cpp](corpus/src/except.cpp) built as C++ with `-fexceptions` and no C++ library: objects whose destructors give every frame cleanup landing pads, thrown through deep recursion, a call through a pointer, a frame without cleanups, nested catchers and a rethrow, by the minimal personality-free unwinder of [corpus/rt/unwind.c](corpus/rt/unwind.c) (the FDE from `.eh_frame_hdr`, the CFA program, the LSDA's call-site table, the pad entered by a `ret` as libunwind and libgcc do it, `_Unwind_Resume`, a setjmp catcher); the destructors' count and order, callee-saved values across the unwind, the unwinder's frame and pad counts |
| `fiber` | stack switching by hand ([corpus/rt/fiber.S](corpus/rt/fiber.S), two stacks besides the main one): a generator fiber, a ring of two fibers that switch from inside recursions, callee-saved registers across switches, and a callee that returns to its caller's caller; every switch returns to a call site that is not the caller's |

The run time ([corpus/rt/](corpus/rt/)): `crt0.S` lays out an NRO's start as libnx's does (a branch over the header, MOD0's offset at +4, MOD0 in the
text), sets SP to a stack of its own, relocates the module unless a loader did, points `tpidr_el0` at the TLS block, runs `DT_INIT` and the init
array, calls `main` and leaves through `svcExitProcess`; `rt.c` prints one line per `svcOutputDebugString` (`dm/host/hle_svc.c` answers it) and
ends with `PASS <checks>` or `FAIL <failed> of <checks>`; `mem.S` has `memcpy` and its kin, `nro.ld` the layout both linkers read. For single
programs: `thread.S` (Horizon's thread and synchronization SVCs as libnx wraps them), `thread.c` (a thread with its stack and TLS block, a mutex),
`pause.S` (the register-invariant loop), `unwind.c` / `unwind.S` (the unwinder; built with `RT_UNWINDER`, rt.c then leaves `_Unwind_Resume` to it),
`fiber.S` (the context switch and the return past the caller).

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
layer (every digest checked, 228 MB with the C++ compiler the except program needs) for a machine without the disk room for the image. clang is
the image's clang-18 (`--target=aarch64-none-elf`). Common flags: `-O2 -mcpu=cortex-a57 -mno-outline-atomics -ffreestanding -fPIE
-fno-stack-protector -ffp-contract=off -fno-math-errno` (C++: `-std=gnu++17 -fno-rtti -fno-threadsafe-statics` instead of `-std=gnu11`), PIE links with
`--export-dynamic --hash-style=sysv --build-id=sha1` (GNU ld: RELA relocations; ld.lld: `-z pack-relative-relocs`, RELR); `corpus.tsv` lists
every NRO with its compiler, flags and SHA-256. [tests/elf2nro.py](tests/elf2nro.py) writes the NROs (and the symbol tables `test_discover.py` compares
with), byte for byte what devkitPro's `elf2nro` writes, which `build_corpus.sh` checks on every build. The builds are reproducible: no timestamps,
paths relative to the corpus folder, build IDs over the content; `--check` rebuilds byte-identically.

Run so far: under dynarmic (`nxaot_run`, CTest `corpus_ref`) and recompiled on x86-64 (`nxaot build` and `nxaot_host`, CTest `corpus_native`: both
conventions, the correctness mode and the max mode; CTest `corpus_jit`: with everything in the JIT and with no JIT): all eighteen NROs print `PASS`,
the GCC and clang builds of each program the same lines, recorded in [corpus/expected/](corpus/expected/), and the native runs exactly what dynarmic
prints. Not run: natively on ARM64 hardware (an Apple-silicon Mac; nothing here can). `int_ops` reads an exported `int_ops_rounds` (1 in the committed output) that a runner may set before the run (`--set
int_ops_rounds=N`): its operation loop then runs N times, which the throughput measurement uses.

## Calls and returns

```sh
build/aot/nxaot build dm/tools/aot/corpus/nro/fnptr-gcc.nro --shared /tmp/x/fnptr.so   # balanced (tier W), swifttailcc, a worker per CPU
build/aot/nxaot build ... --cc ghc --mode max --correctness --all-entries --cpu x86-64-v3 --spills --asm /tmp/x/fnptr.s
build/aot/nxaot build ... -o /tmp/x/fnptr.o --workers 2 --cache /tmp/x/aot-cache   # also one relocatable object; 2 workers; the cache
```

`nxaot build` ([src/build/pack.cpp](src/build/pack.cpp)) runs discovery, plans the convention ([src/build/plan.cpp](src/build/plan.cpp)) and lifts
every function in **function mode** ([src/lift/lower_calls.cpp](src/lift/lower_calls.cpp)): one LLVM function per guest function, named
`<module>+0x<offset>`, its blocks those its entry and its landing pads reach inside `[entry, limit)` (a block that runs to the limit continues in the
next function by a tail call). Design 2.3, 3.3 and 4, as implemented:

- **The convention's facts** (`FnFacts`): PASS = x0..x3 and the passable registers the function may read (its summary's `reads`); WRITES = the
  summary's `writes`, plus v8..v15 when the function writes them at all (a `ldr d8` restore zeroes the high half, which callers must not keep);
  LIVE_AFTER, a fixed point over every call site of the module (what is read before being written after the call; a tail call passes on its own
  function's; an indirect site counts for every standard entry; the host and the JIT read x0, x1, v0..v3 and SP); RV, the v0..v3 returned in host registers: the
  same over every chain of tail calls (a component of the tail-call graph; every standard entry is in one), the ones every returning member writes.
- **Registers** live in allocas as in a region; at entry the passed ones come from the arguments, the rest from the block (unused loads go). A
  forward data flow over the function's blocks gives the **dirty** registers (newer than their block copy) at every block end. A call stores
  dirty - PASS(callee) - {sp, lr}, then takes x0, x1 and RV from the return value and reloads WRITES(callee); through the dispatcher it also stores
  what it passes that the union of the possible callees may change (`CalleeView::exact`: the callee that runs stores only its own writes). The
  indirect view's WRITES is that union and AAPCS64's result registers, x0..x8 and v0..v3 (`analysis::ForeignWrites`), since the callee may be no
  function of the module: a host function through a GOT slot, another pack, code the JIT fallback runs (CTest `plan`,
  [tests/test_plan.py](tests/test_plan.py)). A return stores dirty & LIVE_AFTER & WRITES (not the returned registers), and the registers it restores whose block copy it changed before an inner call
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
  returns at once, and the host dispatches what reaches it as a new call (a target with no compiled entry, a continuation in the middle of a
  function, runs in the JIT: "The JIT fallback"). The sjlj program's longjmps (deep recursion, through a function pointer, nested handlers,
  callee-saved registers) and the except program's landing pads (entered by its unwinder's `ret`, past every frame between) all resume this way.
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
  cntpct_el0` is a `preserve_most` call of `dm_aot_cntpct` (8); `brk`, `udf` and undefined words call `nxaot_rt_trap` with a reason
  (`include/nxaot/aot_module.h`); branches into the middle of other functions go through `jit_entry`, and `msr fpcr` leaving the specialisation,
  untranslated encodings and code that runs into no entry hand the rest of the function to `nxaot_rt_jit` ("The JIT fallback"). A function the
  lifter refuses becomes a body that runs it in the JIT from its entry (none in the corpus). Safepoint polls: "Threads and safepoints".
- **Gates** (design 1.1; the manifest's `obs` and `spin` rows): before an observed instruction (the translation stops there, so a block starts
  at it) the code stores every register newer than the block, `pc` = the instruction, calls `nxaot_rt_gate(st, offset)` through a `preserve_all`
  wrapper and reloads x0..x29 (an observer may change them: `OBS_STDALLOC_INIT` writes x2); where a spin wait's back-edge is taken to its head, the
  same with `pc` = the head and no reload, then the edge's safepoint poll. The descriptor lists the sites (`gates`, sorted, kind and argument), the
  unit checks that its code calls every site its function's blocks hold (a pack that would not serve one fails), and dm/host checks them against
  the manifest's rows ("A game module in the host"). A module without such rows imports no `nxaot_rt_gate`.
- **The aux region** (design 1.1; the manifest's `aux` row, the frame step's stubs): code of every function whose walk branches into it, never a
  function of its own. Discovery follows a function's direct branches into the region and back (`Inside`), the lifter's region and reader take it
  (`Region::extra`), so a stub and the code around its site are one LLVM function, as on Apple the patch area runs between two instructions of
  the function: the stub's `b` back is a branch inside the function, wherever the site is (an entry, the middle of a loop), with no tail call that
  would hand a stale x30 to the next part. The unit's cache key holds the region's words its function reaches.
- **The standard entries' one signature** (design 2.3, ABI 3): x0..x3 and the convention's passable registers in host registers (swifttailcc:
  x4..x7 and v0..v7; ghccc: x4..x6 and v0..v5), x0 and x1 returned in host registers and v0..v3 through the block (rv 0: the standard entries'
  tail-call component returns no v register in host registers, its members store what they write of v0..v3 at their returns, where every
  standard entry's LIVE_AFTER has them), whatever the module's address-taken functions read: the same in every pack of a convention and mode, so
  the dispatcher binds an inline cache of one pack to another pack's standard entry (a call into an NRO, an NRO calling its loader back) and the
  next call goes there directly. The descriptor records it (`std_x`, `std_v`, `rv`).
- **Tables in the tables' object** ([include/nxaot/aot_module.h](include/nxaot/aot_module.h)): `.nxaot.lookup` (open addressing, module offset ->
  standard entry, self-relative), `.nxaot.fnmap` (every function's body and size), `.nxaot.gates` and the exported descriptor `nxaot_module`;
  **the line table** (3.5) is `lines.o`'s, made once every unit is compiled: `.nxaot.fnlines` (a run per function, parallel to the function map)
  and `.nxaot.lines` ({native address - the body's, guest offset}, each run sorted). Each unit marks its guest memory accesses with
  `!pcsections` into the section `.nxaot_pcs` of its object; LLVM writes, per metadata node, the addresses of every machine instruction that
  carries it and then the guest offset once (`AsmPrinter::emitPCSections`), so the section is no array of pairs (ABI 2's readers took it for one:
  246 of fnptr-gcc's 415 entries and 8,812 of switch-gcc's 24,293 were misread). `ReadLineMarks` (src/build/pack.cpp) tells the words apart by
  the unit object's relocations (only addresses have one), files each address under the body symbol that holds it, and sets `SHF_EXCLUDE` on the
  section, which lld then drops: the pack holds nxaot's table only, the same whatever order LLVM wrote the groups in. Call sites and the other
  native addresses map through the DWARF line table (line = guest offset in a synthetic `<module>.s`; `--no-debug-info` drops it). The selection
  DAG gives an access's mark to every machine instruction it lowers it to (a `cmpxchg`'s `lock cmpxchg` and the `setcc` of its success), all of
  them the right guest instruction; in 8 of the 58,843 accesses of the 24 packs test_lines.py builds LLVM folded a load into its user and unfolded it again,
  and the mark ended on a neighbouring instruction of the same lowering: a fault at that load is reported at the nearest entry before it, which
  the report says. CTest `lines` (`nxaot lines <pack> <module>`) checks every entry of every corpus pack against the code and the module.
- **Tiers** (9.3, `--tier`): C (SROA, instcombine, simplifycfg, DCE; FastISel), W (LLVM's O1 pipeline, codegen Less), H (O2), H+ (O3, codegen
  Aggressive). Without a profile the pack modes are one tier for every function: `balanced` W, `max` H+. One tier per module for now. At C and W
  every function is a compile unit of its own, at H and H+ a shard of neighbouring functions ("Pack throughput"): H and H+ inline within a shard
  (design 9.3), not across shards (ThinLTO's import is phase 5b's).

Found by the tests and fixed in the lowering: a guest call site that two lifted blocks hold (the block that runs into a loop's head holds the
loop's first instructions, and the head's block holds them again) had an inline cache per copy, so the second copy missed again what the first had
learnt (t_aot_hostfn's cross-pack loop: one dispatcher miss after the warm-up); a site now has one slot per function (`FnDispatch`). And: a preserving callee that stored its own x29 before an inner call left that copy in the block, which a
later callee saved in its frame record (nxaot_funcdiff, summary mode: the missing "store the restored value again" rule); an indirect call reloaded
x3 from the block after the call because some address-taken function writes x3, while the leaf that ran stored nothing (nxaot_bench's blr loop never
ended: the `exact` rule above); an indirect call reloaded only what the module's own functions may write, so in a module where none writes v0 the
call site kept its own v0 after a host function returned a float there (dm/host's t_aot_hostfn: `sqrtf(6.25f)` through the GOT gave back 6.25,
0x40c80000 for 0x40200000, with aot_synth.py's v0-writing `hf_one` made a nop; the summaries' indirect callee and the plan's indirect view now
include AAPCS64's result registers, and that run gives 2.5).

## The native run time

```sh
build/aot/nxaot_host /tmp/x/fnptr.so dm/tools/aot/corpus/nro/fnptr-gcc.nro       # the lines, as nxaot_run prints them; exit status of the run
build/aot/nxaot_host /tmp/x/int_ops.so dm/tools/aot/corpus/nro/int_ops-gcc.nro --set int_ops_rounds=200 --quiet --repeat 5
build/aot/nxaot_host /tmp/x/fnptr.so dm/tools/aot/corpus/nro/fnptr-gcc.nro --relocate --set hidden_ptr=@0x1844   # a target with no entry: the JIT
build/aot/nxaot_host /tmp/x/fnptr.so dm/tools/aot/corpus/nro/fnptr-gcc.nro --jit-all     # everything in the JIT fallback; --jit-none: no JIT
build/aot/nxaot_host /tmp/x/excl.so dm/tools/aot/corpus/nro/excl-gcc.nro --pause-test 50 --set excl_pause_iterations=200000000   # section 6.2
build/aot/nxaot_host /tmp/x/fnptr.so dm/tools/aot/corpus/nro/fnptr-gcc.nro --lookup     # the module offsets the lookup table holds
build/aot/nxaot build dm/tools/aot/corpus/nro/fnptr-gcc.nro -o /tmp/x/all.o --shared /tmp/x/all.so --all-entries --correctness
build/aot/nxaot_funcdiff /tmp/x/all.so dm/tools/aot/corpus/nro/fnptr-gcc.nro --seeds 3   # every function against dynarmic
python3 dm/tools/aot/tests/test_corpus_native.py --nxaot build/aot/nxaot --nxaot-host build/aot/nxaot_host --nxaot-run build/aot/nxaot_run --work /tmp/x
python3 dm/tools/aot/tests/test_funcdiff.py --nxaot build/aot/nxaot --nxaot-funcdiff build/aot/nxaot_funcdiff --work /tmp/x [--cc ghc]
python3 dm/tools/aot/tests/test_corpus_jit.py --nxaot build/aot/nxaot --nxaot-host build/aot/nxaot_host --work /tmp/x     # mixed, jit-all, jit-none
python3 dm/tools/aot/tests/test_threads.py --nxaot build/aot/nxaot --nxaot-host build/aot/nxaot_host --work /tmp/x [--runs N]
python3 dm/tools/aot/tests/test_safepoints.py --nxaot build/aot/nxaot --nxaot-host build/aot/nxaot_host --work /tmp/x [--pauses N]
```

[src/rt/host.h](src/rt/host.h), the minimal form of what the host will do (design 2.4, 3.3, 4, 5, 6, 8, 9.4, 11), as a library (`nxaot_rt`) that
nxaot_host, nxaot_funcdiff and nxaot_bench share. Decision against 9.4: the pack is a shared object linked by lld as a library inside `nxaot build`,
and the host loads it with `dlopen`, as the design's host loads the pack's library (no LLVM in the host; the library's imports resolve against the
executable, which exports them).

- **The arena** (5.2): 256 GiB reserved at 2^36 (64 GiB to 320 GiB, below 2^39) with `MAP_FIXED_NOREPLACE`, nothing accessible until mapped: the host-return
  address `arena + 0x10` (no code), the main thread's guest stack (1 MiB at +16 MiB), its TLS block, a scratch area for nxaot_funcdiff, and the
  module's image at +256 MiB, its one-to-one guest address (5.1): text and read-only data readable and never executable (5.3: a jump into guest
  text faults), data and bss writable. State blocks and native stacks are outside it.
- **Loading** (9.4): the library's descriptor is checked against the image (ABI, SHA-256, sizes; a mismatch stops with a red line and status 2),
  `*base` is written, the line table and the function map are read. `--relocate` applies the module's relocations as dm/host's loader does (the
  corpus's crt0 then skips its own); `--set` writes a symbol.
- **A run**: a state block (x0 = 0, x1 = -1 as nxaot_run, x30 = the host-return address, SP and `tpidr_el0` the host's stack and TLS block, the
  monitor clear: 6.5, `st->excl_*` the thread's local monitor), MXCSR of the module's FPCR (7.1), then `enter(st, standard entry of _start)`; a
  non-local transfer that reaches the thread's outermost level is dispatched as a new call, or run in the JIT when it has no compiled entry (3.3,
  "The JIT fallback"). The run ends when a thread calls svcExitProcess or fails: every other thread stops at its next poll, SVC, JIT block
  boundary or wait, and is joined ("Threads and safepoints").
- **What the library imports**: `dm_aot_svc` (the SVCs of "Threads and safepoints"; others stop with status 5), `nxaot_rt_dispatch` (the miss
  path: the lookup table, a descriptor per target published with a release store; no entry: `jit_entry` with the target in `st->pc`, so the JIT
  runs it; with `--jit-none`, `[RED] aot: the indirect branch at module+0x... to 0x... (module+0x...) has no compiled entry, and the JIT fallback
  is off (--jit-none)` and status 3), `nxaot_rt_trap` (brk, undefined words and an untranslatable encoding status 1, a no-return call that
  returned status 6), `nxaot_rt_jit` (the JIT fallback, "The JIT fallback"), `nxaot_rt_safepoint` (a poll, "Threads and safepoints"),
  `nxaot_rt_gate` (a gate of design 1.1, which only behaviour images with obs or spin rows have: here a pause point like a safepoint, no
  observer; dm/host's runs them), `dm_aot_cntpct` (CLOCK_MONOTONIC as 19.2 MHz ticks, `guest_ns * 12 / 625` as `svcGetSystemTick`), and the floating-point helpers of
  [src/lift/runtime.cpp](src/lift/runtime.cpp).
- **Faults** (5.4 is the host's job): SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGTRAP in guest code, on an alternate stack, are reported with the
  faulting address and the guest instruction the line table gives (`[RED] aot: SIGSEGV at address ... in guest code (host pc ..., in the function at
  module+0x..., guest instruction module+0x...)`), status 4. Dirty tracking (write-protected pages resumed after the fault) is dm/host's.
- **Exit statuses** (`rt::Status`): 0 svcExitProcess, 1 trap, 2 usage or a library that does not match, 3 the JIT fallback is needed and refused
  (`--jit-none`, nxaot_funcdiff), 4 fault, 5 an SVC the corpus run time does not answer, 6 a return where none may be (the guest returned to the
  host from _start or a thread's entry, a no-return callee came back), 7 timeout, 8 stopped at an svc (nxaot_funcdiff), 9 the pause test found a
  state block inconsistent or made fewer pauses than asked.

**The per-function differential test** (`nxaot_funcdiff`, CTest `corpus_funcdiff`, design 12 item 2 in its simple form): a pack built with
`--all-entries`; after the host relocated the image, every function runs three times from one snapshot under the recompiled code and under dynarmic
(whose memory callbacks allow exactly what the native mapping allows), with registers drawn from a seed (pointers into data, the scratch area and
the stack, small integers, and non-canonical values that fault at once in both). Runs that end the same way are compared: in the correctness mode
every register (x0..x30, SP, NZCV, v0..v31) and every written page, in the summary mode what the convention gives a caller (x0, x1, v0..v3 at a
return, x0..x7 at an svc) and every written page. Runs that would leave for the JIT (a `blr` to a random value; it runs with `--jit-none`), fault
in both, or time out are counted apart.

What dm/host provides for the integration (dm/host/aot_*.c) is listed, item by item, at the end of "Threads and safepoints".

## The JIT fallback

Design section 11, on the vendored dynarmic with nothing changed in it ([src/rt/jit.h](src/rt/jit.h), [jit.cpp](src/rt/jit.cpp); the driver in
[src/rt/host.cpp](src/rt/host.cpp), `Host::JitLoop`):

```c
dm_jit  *dm_jit_create(dm_guest_state *st);   // one instance per guest thread, made at its first use, on that thread's state block
uint64_t dm_jit_run(dm_jit *j, uint64_t pc);  // st -> the JIT, run from pc until a stop, the JIT -> st; returns where the guest continues
void     dm_jit_halt(dm_jit *j);              // from any thread: dm_jit_run returns at the next block boundary (pauses and stops)
```

(`dm_jit_reason` says which stop: a compiled entry, a compiled call site's return, a halt, no code, an SVC that ends the thread, a trap, a gate
site; `dm_jit_init` gives once per process what the host knows: the code ranges, the module's FPCR, the stop addresses and gate sites, the SVC
path, the counter. `dm_jit_run_past` goes on after a gate the host served, `dm_jit_set_code` gives the code ranges again when modules are loaded.)

- **Configuration** (design 11): fastmem with pointer 0 and 39 address bits (the one-to-one arena; an access at or above 2^39 or a fault takes
  the memory callbacks), `fastmem_exclusive_access` with one `ExclusiveMonitor` for every instance (64 processors), `tpidr_el0` and `tpidrro_el0`
  pointing into the state block, `cntfrq_el0` = 19.2 MHz, `ctr_el0` = 0x40000, `dczid_el0` = 4 (with `hook_data_cache_operations`, so that `dc
  zva` zeroes the aligned block: "Known issues", 2), `wall_clock_cntpct` with `GetCNTPCT` on the host clock, no cycle counting, `CallSVC` into the
  host's SVC path on the whole register file, a 16 MB code cache, and `A64GetSetElimination` off ("Known issues", 1).
- **Stopping at a compiled entry, unmodified.** The JIT must not run what has a compiled entry, and must stop where a native frame may wait for a
  callee (the return address after every `bl` / `blr` of compiled code: the descriptor's `returns` table). dynarmic asks `MemoryReadCode` for
  every instruction it translates, so the reader hands over no instruction at those addresses: the block that reaches one ends in dynarmic's own
  NoExecuteFault exception, whose callback (`ExceptionRaised`) records the address and halts `Run()`. Translation caches the answer, so it costs
  nothing in the blocks that run; it applies only while the JIT's FPCR has the mode bits compiled code was specialised for (the reader asks the
  instance's FPCR, which dynarmic's block key also holds). To run such an address itself, `dm_jit_run` single-steps its instruction first
  (dynarmic caches single-step blocks apart), then runs on. The alternatives the design names were measured against it: `HaltExecution` from a hook
  is what the stop uses; running one block per `Run()` with cycle counting and checking the next pc in the host costs 35.6 ns more per block
  (40.2 ns against the JIT's own 4.7 ns, "Measurements") and sees block boundaries only (a block that runs into the next function's entry without a
  branch passes it). So no patch: [VENDOR.md](../../third_party/dynarmic/VENDOR.md) still describes an unchanged copy.
- **Gates** (design 1.1 and 11). The JIT runs the guest's original code, so it stops where compiled code calls `nxaot_rt_gate`: the host's
  `stop_at` marks every manifest's gate rows (`DM_JIT_GATE_AT`; `load_modules` hands them to `aot_run.c`), and the code reader hands over no instruction there either,
  under any FPCR, unless compiled code takes the address itself (an entry or a return under its FPCR, whose code calls the gate). The run returns
  `DM_JIT_GATE` with the gate's instruction not run and the whole register file in the block; the host serves the gate on the block (the
  observer, which may change x0..x29, or the back-off as an SVC frame) and calls `dm_jit_run_past`, which runs that one instruction as a single-step
  block without stopping there again, then goes on. A spin wait's back-edge is a branch: `dm_jit_run_past(..., 1)` returns after it, and the host
  runs the back-off only when it went back to the head, as compiled code calls it on the taken edge only. `dm_jit_run` at a gate site returns at
  once, before anything is copied, so a host call or a continuation that starts at an observed instruction serves its gate first. Both the block
  that stops before the site and the step are translated once and cached (the step apart, as dynarmic keys single-step blocks), so the lookup costs
  only at translation and code with no gate runs as before. The host's side is `dm/host/aot_run.c` (`jit_stop_at`, `run_jit`). A manifest row no
  pack serves (no compiled code holds it: a `[RED] ` line at load) is a stop too, so its gate is served when the JIT reaches that code (t_aot_module's
  hidden function). An encoding the lifter does not translate at an observed instruction hands that instruction over after its block called the gate
  (`lifter.cpp`, the block's start; `lower_calls.cpp` `FnExit`): `nxaot_rt_jit` then runs it with `dm_jit_run_past`, so the gate is served once.
  nxaot's own runner has no gates.
- **Code ranges.** The JIT translates only inside the ranges the host gives (`dm_jit_range`): `nxaot_host` the module's text and its aux region
  when the image has one; the host every loaded module's text and aux region (the frame-step stubs, design 1.1: the manifest's aux row, which
  `module_t` keeps), and again with each NRO loaded
  later (every instance drops its translations before its next run, as a block may end where there was no code before).
- **The hand-over** (2.5, 11): `dm_jit_run` copies x0..x30, SP, PC, v0..v31, NZCV, FPCR and FPSR (with the cumulative flags compiled code left in
  MXCSR, which it clears) into the instance, and back into the block when it stops: 102 to 109 ns both ways. The exclusive monitor is cleared both ways,
  as an exception return clears it. At an SVC the JIT copies its state to the block, calls the host's SVC path, and takes the general registers back.
- **When it runs** (design 11, cases 1 to 4), and how compiled code hands over:
  1. an indirect branch to an address with no compiled entry: the dispatcher's miss path writes the target to `st->pc` and returns `jit_entry`
     (a function of the pack with the standard entries' signature) instead of a standard entry; `jit_entry` stores what it is passed and calls
     `nxaot_rt_jit(st, lr, sp)`; the inline cache keeps its descriptor, so the next call misses again. A direct `bl` or `b` to such an address
     (never made by discovery: every target is an entry) calls or tail-calls `jit_entry` the same way;
  2. a non-local transfer to a continuation with no native frame (a fiber switch, a return past the caller, a longjmp nobody recorded): the frames
     propagate it (`st->pending`, 3.3) to the thread's outermost level, which runs it in the JIT when it has no compiled entry;
  3. `msr fpcr` leaving the specialisation, an encoding the lifter leaves untranslated, code that runs into no entry: the function stores every
     dirty register and pc, calls `nxaot_rt_jit(st, its entry's lr, its entry's SP)` and returns natively what the JIT left in the block;
  4. a function the lifter refused: its body stores what it is passed and does the same from its entry.
- **The driver** (`JitLoop`): runs the JIT until the guest returns to the lr it was given (lr alone decides, as a compiled function's own
  return does: design 3.3), taking every compiled entry natively on the way (`enter` with the block's registers; its normal return continues in the
  JIT at the lr it was entered with, x30 is that lr, and SP is the call's unless the function may change it: the lookup slot's
  `NXAOT_LOOKUP_SP_KEPT`). A return address of compiled code reached with an SP above the one the JIT was entered with may be a native frame's
  further out: nested, it is propagated to them (`st->pending`); at the outermost level nothing can own it and the JIT runs it. Back where the native
  frame waits but under another FPCR than it was compiled for, the return is propagated too (the caller cannot go on under it). A halt is a
  safepoint ("Threads and safepoints"); an SVC that ends the thread or the run leaves through the thread's base, outside dynarmic's `Run()`.
- **Logging and counting**: every JIT entry is a discovery gap or a continuation without a native frame, logged once per address on standard error
  as `[RED] aot: <why> at 0x... (module+0x...) runs in the JIT fallback (design section 11)` and counted (`nxaot_host`'s second summary line: runs,
  distinct addresses, `Run()` calls, stepped stops, native calls, seconds). `NXAOT_JIT_TRACE=1` prints every JIT run and native call.
- **Modes** (`nxaot_host`; in the host `DM_AOT_JIT_ALL=1` is `--jit-all`): mixed (the default), `--jit-all` (everything in the JIT: no stop at compiled entries, the dispatcher and the entries
  never used), `--jit-none` (every case above stops the run with a [RED] line and status 3: phase 1's behaviour; nxaot_funcdiff runs so),
  `--jit-one-block` (the measurement of the alternative above).
- **Faults**: the host's handler is installed before dynarmic's (which comes with the first instance and hands every fault outside its code cache
  to the previous handler): a fastmem fault in JIT code takes dynarmic's fallback to the memory callbacks. With `check_halt_on_memory_access` a
  callback does not touch memory but halts with `MemoryAbort`, so dynarmic leaves at the exact instruction (it checks that halt only after a
  callback: fastmem accesses cost nothing more); `dm_jit_run` steps that instruction again with its callback accesses made, and a fault there is
  reported as a guest fault at that instruction (`nxaot_host`: "in a memory access of the JIT fallback near guest 0x...", now exact; the host:
  "a memory access of the JIT fallback, guest instruction main+0x..."; `dc zva`, whose hook has no such check, keeps "near").

The corpus in the three modes (CTest `corpus_jit`, [tests/test_corpus_jit.py](tests/test_corpus_jit.py)): every NRO prints its expected lines
mixed, with `--jit-all` and, where the mixed run needed no JIT, with `--jit-none`; fnptr's `hidden_ptr` pointed at the second instruction of
`hidden_target` (a `blr` to an address with no entry) enters the JIT once, comes back to compiled code and prints what `--jit-all` prints. JIT
entries in the mixed runs, all expected: `fiber` (its continuations after every switch, 29 and 54 addresses), `fp` (the FPCR checks: the rest
of `div_rz` after its `msr fpcr`, case 3, back to compiled code at its return; and the rest of `fpcr_set`, which returns to its caller under RM:
the JIT stops nowhere under another FPCR, so it runs `main` on under RM, then, FPCR restored, until `main` returns, taking compiled entries
natively again; 8 and 9 addresses), `except-gcc` (5 targets of the opcode switch of rt/unwind.c's
`run_program`: GCC's halfword table, whose base register is set in the function's prologue outside the loop that dispatches on it, is left
unbounded by discovery, "the entry address depends on more than one value": a discovery gap of the kind the JIT exists for, which phase 5's
feedback compiles).

Found by these tests and fixed: after the JIT called a compiled entry natively, the block's SP and x30 were the copies some inner frame stored
(they travel in host registers at every call, design 2.3), so the JIT continued with SP 32 bytes low and the next native call overwrote its
caller's frame record (fiber's last check looped through `rt_finish`); the driver now takes x30 as the return address and SP as the call's when the
function keeps SP, and `kHostReads` (what the host and the JIT read after a call) includes SP, so that a function that may change it stores it. The
return check compared SP too, which a `br` through `jit_entry` from the middle of a function (an unbounded br, SP of the body) never matched at its
return (SP of the entry); lr alone decides now.

## Threads and safepoints

**Threads** (design 6.1, 6.5; [src/rt/host.cpp](src/rt/host.cpp)): each guest thread is a host thread (pthreads, an 8 MB native stack, its own
alternate signal stack), with a state block of its own (64-byte aligned, outside the arena) and, at its first use, a JIT instance. The SVCs, with
the semantics and result codes of Horizon's kernel, mapped onto one lock and one condition variable (correctness first, as dm/host's
`hle_thread.c`): CreateThread (0x08: x1 entry, x2 argument, x3 stack top; w1 the handle), StartThread (0x09), ExitThread (0x0A), SleepThread
(0x0B), CloseHandle (0x16), WaitSynchronization on thread handles (0x18: signalled when the thread exited), ArbitrateLock / ArbitrateUnlock
(0x1A, 0x1B: the waiters bit 0x40000000, the lock handed to the first waiter), WaitForAddress (0x34: types 0 to 2) and SignalToAddress (0x35:
types 0 and 1); output lines are serialised. A thread that returns from its entry ends the run (Horizon requires svcExitThread); svcExitProcess on
any thread, or a failure, stops every other thread at its next poll, SVC, JIT block boundary or wait (the poll byte, `dm_jit_halt`, the condition
variable), and the run joins them. nxaot_run gives the reference the same SVCs with its threads as contexts on one dynarmic instance, switched round
robin at a blocking SVC, at svcSleepThread and every 20,000 instructions: a deterministic interleaving.

**The exclusive monitor across threads** (6.5): compiled code keeps the monitor in the thread's block (`st->excl_*`) and stores with a compare-and-
swap against the value its `ldxr` read (`lock cmpxchg`, `cmpxchg16b` for pairs); the JIT's inline exclusives check dynarmic's monitor for the
processor and then do `lock cmpxchg` against the value it read on the same memory
(`dm/third_party/dynarmic/src/dynarmic/backend/x64/emit_x64_memory.cpp.inc:411-500`), so compiled code and the JIT interoperate through the memory
itself (design 6.5's [A], checked in the emitter); a JIT store also clears other processors' reservations of the address, a compiled one does not,
which a compare-and-swap does not need.

**Memory ordering** (6.3), what the lifter emits ([src/lift/lifter.cpp](src/lift/lifter.cpp), `LowerMemory`, `LowerContext`) and LLVM 18 makes of
it on x86-64 (checked with `llc-18` and in the excl pack's assembly):

| ARM | LLVM | x86 |
|---|---|---|
| `ldr`, `str` and the other plain accesses | `load volatile`, `store volatile` | `mov` |
| `ldar*` | `load atomic acquire` (natural alignment) | `mov` |
| `stlr*` | `store atomic seq_cst` | `xchg` |
| `ldxr` / `stxr`, `ldaxr` / `stlxr` | `load atomic` monotonic / acquire; `cmpxchg` monotonic / seq_cst | `mov`; `lock cmpxchg` (a full barrier either way) |
| `ldxp` / `stxp` and the acquire / release pair | `load atomic i128`; `cmpxchg i128` | `lock cmpxchg16b` |
| `dmb` and `dsb` of every kind | `fence seq_cst` (dynarmic's IR does not keep the option, so `dmb ishld` / `ishst` too) | `mfence` |

Why it is sufficient: x86-TSO reorders only a later load before an earlier store to another address; acquire loads and release stores are
free under TSO, the one ARM ordering TSO lacks (an `stlr` before a later `ldar`, RCsc) is the store-load one, which `xchg` gives, and `mfence` and
`lock`-prefixed instructions order everything. The excl program's threaded part runs 25 times natively and 5 times in the JIT per compiler in
CTest `corpus_threads` ([tests/test_threads.py](tests/test_threads.py)), with real host threads contending on every counter, and 200 times
natively and 10 times in the JIT per compiler in a longer run (`--runs 200 --jit-runs 10`: 420 runs): every run gives the exact totals.

**Safepoints** (6.2): recompiled code polls `st->poll` (a monotonic atomic byte: `cmpb $0, 856(%r13)` and a branch weighted cold) at the entry of
every function that calls or tail-calls (every cycle of the call graph passes through one), at every branch to a block at or before its own start
(every cycle of a function has such an edge: addresses cannot only grow around it) and before a jump-table switch that can branch back. A function
that neither calls nor loops runs a bounded time and has no poll. The cold path stores the dirty registers (so the block holds the whole register
file), NZCV only where some path reads it before writing it (a dead NZCV would keep every compare's flags materialised in the loop: the cost went
from 17.9 % to 4.4 % on int_ops with it), and pc (the block the thread continues at), and calls `nxaot_rt_safepoint` through a preserve_all
wrapper. `nxaot build --no-safepoints` leaves the polls out (measurements only). The pause protocol is dm/host's `SetThreadActivity`
(`hle_thread.c`): the requester sets `pause_req` and the poll byte, halts the thread's JIT instance (the JIT polls nothing: `dm_jit_halt` stops it
at the next block boundary, and dm_jit_run then copies its state to the block), and counts a thread inside an SVC (bit 0 of `svc_state`) as stopped
at once, the block having been complete when the SVC was entered; such a thread waits at the SVC's exit until the resume. A stopped thread's block
holds x0..x30, SP, pc, NZCV, v0..v31, FPCR, FPSR and the TLS registers; x0..x8 of a thread stopped inside an SVC may still change as the SVC writes
its results. The resume clears the requests and the poll bytes and wakes every waiter.

**The pause test** (CTest `safepoints`, [tests/test_safepoints.py](tests/test_safepoints.py); `nxaot_host --pause-test N`, in the manner of
dm/host's `tests/t_preempt.c`): a host thread beside the guest's stops every guest thread N times at random intervals while more than one runs,
reads every state block while the world is stopped, checks it (pc a guest instruction, SP in the arena and 16-byte aligned at a poll, and at the
head of [corpus/rt/pause.S](corpus/rt/pause.S)'s loop the register invariant x20 = 2 × x19, x21 = ~x19, which a block written back incompletely or
from another moment than its pc breaks), and resumes them. excl runs so with 50 pauses, 6 seeds natively and 2 in the JIT, per compiler: 800
pauses, about 760 invariant checks, no inconsistent block, the output unchanged.

**What dm/host provides** for the integration (`dm/host/aot_*.c`, [dm/host/aot.h](../../host/aot.h); built by dm/host/CMakeLists.txt only, where
`DM_NATIVE_EXEC` is 0), item by item against the signatures this run time defines (include/nxaot/aot_module.h, src/rt/jit.h); CTest `t_aot_host`
(dm/host/tests/t_aot_host.c) runs the corpus through it, packed by [tests/build_packs.sh](tests/build_packs.sh):

- **done**: the arena allocator (5.2; aot_arena.c `dm_guest_reserve`, `dm_guest_alloc`, `dm_guest_free`: 256 GiB at 2^36 through the platform
  layer's new `plat_vm_reserve_at`, the host-function page first, then the stub page) and the module loader's mapping into it (loader.c:
  `loader_add_image` for an image as it is, an NRO, and `load_modules` for a game's modules from their manifests: "A game module in the host" below;
  text readable and never executable);
  `*desc->base` written per module (`dm_aot_pack_load`, after the identity checks of 9.4). hle_svc.c's guest block (heap, alias and stack
  regions), thread.c's TLS blocks and thread-local regions (into the block's `tpidr_el0` / `tpidrro_el0`), GetInfo's user exception context and
  the main thread's guest stack live in the arena. **Open**: NVN's virtual pools (`hle_nvn.c`) and Unreal's heap (`hle_uemalloc.c`), still host
  memory;
- **done**: `dm_aot_svc` (aot_run.c): `svc_dispatch(n, st->x, ...)` between `thr_svc_enter` / `thr_svc_leave` on the block; svcExitProcess
  stops the process (every guest thread leaves at its next poll, SVC or kernel wait, hle_thread.c `thr_stop_all`);
- **done**: `nxaot_rt_dispatch`: the lookup table of the pack whose image holds the target, bound in the call site's inline cache when that pack
  is the caller's or records the same standard-entry signature, convention and mode (ABI 3: a cross-pack call then goes to the other pack's
  standard entry directly from the next call on, counted in `dm_aot_stats_t.cross_pack`), the host-function page through `jit_entry` (the
  target in `st->pc`, and `nxaot_rt_jit` runs the slot's thunk, `dm_aot_hostcall`), a pack of another signature through `jit_entry` and that
  pack's `enter` (`cross_pack_handovers`: none with nxaot's packs), `jit_entry` with `NXAOT_TRAP_JIT_MISS` for the rest. **Open**: the packs are
  scanned in order (FINAL FANTASY X/X-2 has 1,432 NROs: a sorted index then);
- **done**: `nxaot_rt_gate` (aot_run.c): the site's gate of the calling pack's descriptor (the return address names the pack), `dm_aot_gate` at
  its guest address (hle.c `gate_dispatch`: the observer on the block's registers, or the spin wait's back-off as an SVC frame, where a pause
  stops the thread with its whole register file in the block), then a process stop leaves; counted (`obs_gates`, `spin_gates`);
- **done**: `nxaot_rt_trap`, `nxaot_rt_jit` (`Host::JitLoop` in C), `nxaot_rt_safepoint` (hle_thread.c `thr_safepoint`: SetThreadActivity's
  pause wait), `dm_aot_cntpct` (`guest_ns() * 12 / 625` after `guest_clock_read_wait`, as svcGetSystemTick and the counter gate);
- **done**: `dm_guest_call` on top of `enter`, with the outermost level of `Host::RunGuest`; the outermost call on a thread is the base it leaves
  through (ExitThread, a process stop, a trap, a fault). loader.c's `run_module_inits` calls DT_INIT through it. **Open**: the other places the
  host calls guest code as a C function (main.c's start sequence, hle_ro.c's init and fini, hle_guestthread.c's host-created threads, whose
  entry is a host function and needs a slot);
- **done**: thread creation (hle_thread.c's x86-64 form: an 8 MB native stack, the state block made when the thread starts, its alternate signal
  stack, a JIT instance at first use); SetThreadActivity (0x32) setting the poll byte and halting the JIT instance; GetThreadContext3 (0x33)
  filling `hz_ctx_t` from the block. The host's pause test found that SetThreadActivity's resume does not clear `paused`: a thread paused inside
  an SVC and resumed before the SVC returns leaves through `thr_svc_leave`'s fast path with it set, and its next pause, while it runs guest code,
  returns at once with the thread still running (before the fix none of 150 pauses stopped a thread at a poll; after it, 130 to 200 stops at
  polls). Fixed for x86-64 (the resume clears it, under `!DM_NATIVE_EXEC`); Apple's code has the same path (there GetThreadContext3 then returns a
  zeroed context), and its one-statement fix waits for a Mac test;
- **done**: the dirty-tracking fault hook (5.4) ahead of dynarmic's fastmem handler (aot_fault.c, the portable host's fault handling where crash.c
  is Apple's: installed at start, installed again in front of dynarmic's after its first instance, chaining to it for faults the hooks leave); a
  fault in compiled code or in the JIT's memory callbacks is reported at its guest instruction (the line table, 3.5) and stops the process
  (status 4). Windows: a vectored exception handler that redirects a guest fault to an ordinary frame, written and not run;
- **done**: MXCSR at every entry into compiled code (the module's FPCR). **Open**: `aot_fpcr` per profile (the host takes the first pack's FPCR, and
  a pack specialised for another gets a [RED] line), the cumulative FPSR flags compiled code keeps in MXCSR across a JIT hand-over (cleared there,
  as in nxaot_host);
- **done**: the floating-point helpers of src/lift/runtime.h in C (dm/host/aot_fp.c: the functions of dynarmic's common/fp and common/crypto that
  runtime.cpp wraps, ported, since the host's build image has no C++ library or Boost), checked against runtime.cpp by dm/host/tests/aot_fp_diff.sh
  (in this image; CI runs it): 8,391,100 cases (every half-precision operand of the unary operations, round to integral and the conversions from
  half under ten FPCR values; 300,000 random cases per operation), 0 mismatches; two deliberate mutations of the port gave 47,233 and 95,648.

The corpus in the host (CTest `t_aot_host`, each run a child process whose lines are hle_svc.c's `[guest]` lines of svcOutputDebugString;
docker/README.md, "The recompiled corpus in the host"): built without the JIT fallback (the nxnative-build image), 13 of the 18 NROs print their
expected lines and exit 0, and fp-gcc, fp-clang, fiber-gcc, fiber-clang and except-gcc stop with status 3 and a [RED] line naming the address
(fp at its `msr fpcr`, fiber at its first continuation, except-gcc at the opcode switch of rt/unwind.c); with it (`NXHOST_JIT`, this image) all 18
do, entering the JIT at as many addresses as nxaot_host does ("The JIT fallback": fp 8 and 9, fiber 29 and 54, except-gcc 5). excl's threads: 6 runs
per compiler, exact totals, 4 guest threads on hle_thread.c (56 to about 25,000 SVCs a run: the mutex handoff's convoy of "Measurements"). The pause
test: 3 seeds of 50 pauses per compiler, 130 to 200 stops at polls and 130 to 140 register-invariant checks per compiler, 0 inconsistent blocks. A
host call: fnptr's `hidden_ptr` set to a host function's slot, the program's check passes through it. Dirty pages (protected again every 0.5 ms):
int_ops 1 write fault a run, excl from 4 to 634, all stores of compiled code; with the JIT, fiber's pages also take stores of JIT code (0 to 5 a run
outside compiled code, none inside the JIT's memory callbacks). `t_aot_host` takes 3 to 5 s.

## A game module in the host

What a game's module needs from dm/host on x86-64 beside the run time above, written and checked without a game (phase 4's first half; CTest
`t_aot_module`, `t_guest_map`):

- **The input.** The modules' folder the host is given holds, per module, the manifest and behaviour image `patch_nso.py --aot` wrote ("Game modules"
  above) and the pack nxaot built from them: `<name>.aot.tsv`, `<name>.image.bin`, `<name>.aot.so` (`.aot.dll` on Windows; aot.h
  `DM_AOT_PACK_SUFFIX`). For a game that folder is `<NSP folder>/<id>_build/aot_<version>/`, beside Apple's `patched_<version>/`; the per-function cache
  of design 13 stays in the work folder (`<id>-work/aot-cache/`). The image is decompressed already: the rewriter reads the NSO's LZ4, as it does for
  Apple's `patched.bin`, so the host has no LZ4 and the pack and the manifest name the same bytes.
- **The loader** (dm/host/loader.c, "The game-module path"; x86-64's `load_modules`, under `!DM_NATIVE_EXEC`: Apple's is unchanged): every row of the
  manifest checked as nxaot's reader checks it; the image's size, layout and SHA-256 against the manifest (a mismatch stops the load with a [RED]
  line naming both hashes); the image mapped one to one in the arena as `loader_add_image` maps one (text readable and never executable, the rest
  readable and writable, bss zero); the build ID into `module_t.build_id`, as fixups.tsv's on Apple, so `dm_main_build` and per-build behaviour
  work; the aux row's state page as the frame step's (`dm_fps_state`); every module mapped and parsed before any is relocated, as on Apple; then
  each pack loaded with the identity of its image (`dm_aot_load_module_pack`: a pack of another image is refused with both hashes). NROs listed in
  `nro.tsv` are not mapped on this path yet (a [RED] line; `ro::LoadModule` then finds none).
- **Imports in the GOT** (design 2.4; aot_arena.c `dm_aot_bind`, called at the end of loader.c's `bind_sym`, so `relocate` and the NRO rebinds
  take it): for a module in the arena, a host function (an HLE, HLE_FORCE or NVN entry) binds to its slot of the host-function page, and an import
  nothing implements to the slot of its named stub in a stub page after it (4096 slots, as hle.c's `MAX_STUB`; `unimpl_stubs` is only their
  identities here). A compiled `blr` through the GOT misses the inline cache and goes through `nxaot_rt_dispatch` and `jit_entry` to the slot's
  thunk, or to hle.c's `unimpl_c` (the `UNIMPL` line once, 0 in x0, as Apple's stub). A host function with no thunk stops the load with a [RED] line
  naming the import: hle_table.c's libc passthrough has none yet (it binds only where no module defines the symbol). Modules outside the arena
  (the unit tests' hand-made images) keep the host addresses Apple binds.
- **Observation gates and spin waits** (the manifest's `obs` and `spin` rows): registered in hle.c's gate table at the guest address of the observed
  instruction and of each valid back-edge, with the number and parameter or the head Apple's `gates.tsv` gives, and dispatched by `dm_aot_gate(st,
  pc)` (aot_run.c: `gate_dispatch` over the block's registers, which an observer may change), which the pack's code reaches through
  `nxaot_rt_gate` ("Calls and returns", Gates). Once the pack is loaded, load_modules checks the manifest's rows against the gate sites its
  descriptor lists (aot_arena.c `dm_aot_pack_check_gates`): a site no row names refuses the pack (one of another manifest), a row the pack does not
  serve (no discovered function holds it; nxaot build says so too) is a [RED] line each, its observer or back-off not running; then
  `aot: <module>: N gates (... observation gates, ... spin wait back-edges) called by its pack, M not`.
- **The frame step's aux region**: the stubs run as compiled code of the functions whose sites branch to them ("Calls and returns", The aux
  region), reading and writing the state page at `FPS_STATE_OFF` as on Apple.
- **Guest memory and the graphics backend** (hle_svc.c, docs/design/vulkan-backend.md 4.2): SetHeapSize growing, MapMemory's destination and
  MapPhysicalMemory call `be_guest_map` with the host range they opened, after opening it; SetHeapSize shrinking and UnmapPhysicalMemory call
  `be_guest_unmap` with the range they are about to close, while it is still accessible, so an import of it goes first. UnmapMemory leaves its pages
  accessible and calls neither; a request that changes nothing calls neither. Apple's `nvn_be.h` has them as empty inline functions and the SVCs
  do not call them there. `t_guest_map` (28 checks) records every call through a stub backend on t_svc_memory's small layout.

`t_aot_module` ([dm/host/tests/t_aot_module.c](../../host/tests/t_aot_module.c)) runs the synthetic module of [aot_synth.py](../tests/aot_synth.py)
through this path, packed by [tests/build_packs.sh](tests/build_packs.sh) into `<packs>/module`: 30 checks in the run (the GOT's two slots, the
relocated pointer, the image's permissions, the build ID, the state page, both gates through `dm_aot_gate`, the pack's gate sites, then `dm_aot_run`
from the entry: the host function through the GOT and the dispatcher, the stub, SetHeapSize reaching `be_guest_map` with the heap's range, both x18
jump tables, the exclusive window, the spin wait, the observed function twice, the cut function, the frame step's function through its stub, a
relocated pointer, OutputDebugString and ExitProcess, with `results[]` as expected), and the pack's own gate calls: the observer runs before the
observed instruction with x0 (41, then 1), x19 and x30 as the guest has them there and the row's parameter, and the x0 + 1000 it writes is what the
instruction reads (results 1042 and 1002); the spin wait's flag is 0 for the run, so the loop spins, its back-edge reaching the host's back-off
(hundreds to thousands of times) until a host thread sets the flag after three; the frame step's stub ran once (it counts at state +0x30). Then 7
checks in the parent: the loader's gate line, the fault probe (its pointer set to the arena's first page: the report must say `guest instruction
main+0xc8`, the probe's load, exactly), and two refusals (an image that is not its manifest's, the pack of another image), each with a [RED] line
naming both SHA-256s. It shows the input path, the binding and the gates on a module made for the purpose: not a real NSO, not a game's size, imports,
observers or spin waits.

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
caching of the analyses (design 10, 13) are phase 5's; the numbers are the measured starting point. Phase 5a's are in "Pack throughput".

### Phase 3: the JIT fallback, safepoints, threads

`nxaot_bench --sections polls,jit` on the same machine (best of 5; timings vary by about 10 % from run to run):

| What | Measured |
|---|---|
| Safepoint polls on int_ops-gcc (`int_ops_rounds` = 200): with them / without (`--no-safepoints`) | 0.0680 s / 0.0657 s: +3.5 % (+4.4 % and +8.4 % in two other runs, one beside another container's build); text 62,829 / 35,629 bytes (+76 %: the cold paths) |
| The same before NZCV was stored only where live, and with polls at every function entry | +17.9 % (the flags of every compare kept materialised in the polling loops) |
| Hand-over: x0..x30, SP, PC, v0..v31, NZCV, FPCR, FPSR into the JIT and back (`dm_jit_measure_handover`) | 102 to 109 ns (four runs) |
| A `dm_jit_run` that stops at once (hand-over, `Run()`, a cached block with no code, its callback, the halt) | 133 to 137 ns |
| Per block in the JIT, int_ops-gcc with `--jit-all` (`int_ops_rounds` = 20, 5,733,411 blocks; time inside `dm_jit_run`, translation included) | 4.7 to 4.9 ns |
| Per block with one block per `Run()` and the next pc checked in the host (`--jit-one-block`, the alternative to the code-reader stops) | 40.2 ns (+35.6 ns; 45.0 ns, +40.0 ns, in the runner's run) |
| Compiled code against the JIT on the same run | 4.0 times as fast (phase 1: 4.3 against dynarmic with fastmem in `nxaot_run`) |
| A new JIT instance (16 MB code cache; dynarmic's emitter builds its fastmem and exclusive fallback thunks) | 27 to 32 ms; the first instance of a process and its first translations about 100 ms more (dynarmic's static decoder tables): the time of every `--jit-all` run of the small NROs and of the pause test's first pause in the JIT |

The corpus with the JIT (CTest `corpus_jit`): ms of the run natively (mixed) and with `--jit-all`, the mixed run's JIT runs and the addresses they
started at; the native times of the NROs that enter the JIT include an instance's creation and the first translations:

| NRO | mixed ms | JIT runs / addresses | jit-all ms | | NRO | mixed ms | JIT runs / addresses | jit-all ms |
|---|---:|---:|---:|---|---|---:|---:|---:|
| int_ops-gcc | 0.506 | 0 | 104.5 | | int_ops-clang | 0.353 | 0 | 111.4 |
| switch-gcc | 0.273 | 0 | 350.8 | | switch-clang | 0.147 | 0 | 127.3 |
| fnptr-gcc | 0.036 | 0 | 106.2 | | fnptr-clang | 0.036 | 0 | 110.2 |
| fp-gcc | 120.6 | 8 / 8 | 109.0 | | fp-clang | 106.5 | 9 / 9 | 122.6 |
| simd-gcc | 0.098 | 0 | 117.5 | | simd-clang | 0.087 | 0 | 122.5 |
| sjlj-gcc | 0.039 | 0 | 173.3 | | sjlj-clang | 0.040 | 0 | 107.5 |
| excl-gcc | 586.5 | 0 | 337.8 | | excl-clang | 229.2 | 0 | 285.8 |
| except-gcc | 100.7 | 1,618 / 5 | 130.6 | | except-clang | 0.690 | 0 | 117.6 |
| fiber-gcc | 100.5 | 442 / 29 | 114.1 | | fiber-clang | 105.5 | 421 / 54 | 111.3 |

excl's native time is its threaded part, and it varies from 7 ms to about 0.9 s from run to run (420 runs of `test_threads.py --runs 200`):
the libnx-style mutex hands the lock to the first waiter in the kernel (ArbitrateUnlock), and with two workers contending a handoff waits for the
waiter's wake-up through a condition variable, so a convoy forms on some runs; the totals are exact on every run. The pause test (CTest
`safepoints`): 50 pauses with more than one thread running in each run; a thread at a poll stops within about 100 microseconds of the request (its
wake-up from the previous resume), the longest stop natively 0.2 to 10 ms, in the JIT about 100 ms (a worker's first instance and translations).
CTest's new tests take about 33 s (`corpus_jit`), 11 s (`corpus_threads`) and 7 s (`safepoints`).

## Pack throughput

Phase 5 (design sections 9.3, 10 and 13): where the pack stage's time went, the causes fixed, the units compiled on worker threads and the
per-function cache (phase 5a, the first sections below), then the compile stage's rate, functions split for size, units of several functions,
the analysis cache and tiers by profile (phase 5b, "Phase 5b" below). No game module is on the development machine, so the corpus and a
game-shaped benchmark are measured and the rest is extrapolated per instruction.

```sh
build/aot/nxaot build <module> --shared <lib.so> [-o <object.o>] [--workers N] [--cache <dir>] [--profile <file>] [--time-passes] [--ir <file.ll>]
python3 dm/tools/aot/tests/gen_bench.py --functions 4000 /tmp/bench          # the game-shaped benchmark, in the nxnative-aot image
NXAOT_ARGS='--cache /tmp/aot-cache' sh dm/tools/aot/tests/build_packs.sh build/aot /tmp/packs   # the corpus packs, cached
```

`nxaot build` logs, besides the module's numbers and sites: `aot: tiers C <n> W <n> H <n> H+ <n>`; `aot: seconds discover .. plan .. lift ..
verify .. optimise .. codegen .. link ..; compile wall .. with <n> workers; total ..; peak memory .. MiB`, where lifting, verification,
optimisation and code generation are the units' seconds summed and the others wall-clock seconds; the instructions per second of each
(per core) and of the compile and the whole pack (wall); `aot: discover seconds: ...`, discovery's steps with their counts (jump-table tries and
regions, summary runs, rounds, computed and reused summaries, the distinct words translated); `aot: units <n> (...), workers <n>`; `aot: split
functions <n> into <p> parts, <t> transfers between parts, joined in <s> s`; with `--profile`, `aot: profile <file>: <s> samples at <o> offsets,
<x> outside every function`; with `--cache`, `aot: cache <dir>: <h> hits, <m> misses (units; functions <fh> hit, <fm> missed), <r> bytes read,
<w> bytes written, keys <s> s` and `aot: analysis cache: hit | miss (<why>), <s> s`. `--time-passes` prints LLVM's pass timers (one worker);
`--ir` writes the lifted IR before the tier's passes, `--ir-opt` after them (what code generation is given). `--no-groups` (one function per
unit at tiers C and W, as phase 5a), `--no-split` and `--split-above N` are for measurements and tests. `NXAOT_CHECK_PROGRAMS=1` runs every
cached abstract program of the summaries beside the interpretation of dynarmic's IR it replaces and stops at the first difference;
`NXAOT_TRACE=1` prints each unit before it is compiled.

**The benchmark** ([tests/gen_bench.py](tests/gen_bench.py)): a C program generated from a seed, built with the corpus's run time and clang
flags, with unwind tables and no exported symbols but main's: thousands of functions, most of one to three statements (accessors, arithmetic),
some of four to twelve, one in twenty of twenty to sixty; statements of the kinds game code has (arithmetic, loads and stores, loops, switches
over a field with a range check (jump tables), floating point, NEON vectors, struct copies, byte loops, direct calls, calls through a table of
pointers); main calls every function through drivers with a bounded depth and prints one checksum, so a pack of it runs and is compared with
dynarmic (`nxaot_run`). `--unbounded-switches` switches over a remainder instead, which discovery cannot bound (a stress case). With 4,000
functions (seed 1): 533,160 instructions in 4,088 functions; with 16,000 (seed 2): 2,163,759 in 16,276. Both packs print what dynarmic prints
(`bench 9bd31f1d51c59eda` for seed 1), with 1 and with 8 workers, balanced and max; 383 of its brs compute their table's index from more than one
value, which discovery does not bound ("Module loader and discovery"), so their cases run in the JIT fallback at run time (7,818 JIT runs, the same
before and after these changes).

### Before and after

On the development machine (Intel Core i9-13950HX, 8 vCPUs and 11 GiB in a virtual machine, Docker image `nxnative-aot`, tier W, swifttailcc),
best of 3 per stage; the benchmark's runs each waited for the one-minute load to fall under 1.5, but another agent's builds ran on the machine
at times, so single runs vary by up to 20 %. Guest instructions per second; discovery includes the plan; optimisation and code generation are
the units' seconds summed; the last three columns are the whole `nxaot build` in seconds (the baseline's sum of its stages: it logged no total):

| NRO | Instructions | Functions | Discovery and plan, per s | Lifting, per s | Optimisation and code generation, per s | Pack before | Pack after, 1 worker | Pack after, 8 workers |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| except-clang | 2,663 | 46 | 9,900 → 32,877 | 71,973 → 98,630 | 2,553 → 2,704 | 1.36 s | 1.13 s | 0.31 s |
| except-gcc | 2,672 | 48 | 3,790 → 28,731 | 66,800 → 92,138 | 2,627 → 2,914 | 1.77 s | 1.09 s | 0.24 s |
| excl-clang | 1,962 | 50 | 15,092 → 28,853 | 72,667 → 85,304 | 2,752 → 2,835 | 0.88 s | 0.82 s | 0.25 s |
| excl-gcc | 1,910 | 51 | 27,681 → 28,088 | 136,429 → 76,400 | 3,304 → 2,550 | 0.67 s | 0.88 s | 0.19 s |
| fiber-clang | 1,801 | 37 | 16,373 → 27,708 | 85,762 → 100,056 | 3,538 → 3,463 | 0.65 s | 0.64 s | 0.21 s |
| fiber-gcc | 1,635 | 38 | 15,139 → 24,403 | 65,400 → 86,053 | 2,620 → 2,659 | 0.77 s | 0.74 s | 0.22 s |
| fnptr-clang | 1,606 | 59 | 15,442 → 23,970 | 69,826 → 76,476 | 2,920 → 2,788 | 0.68 s | 0.70 s | 0.22 s |
| fnptr-gcc | 1,603 | 61 | 14,441 → 22,900 | 59,370 → 76,333 | 2,532 → 2,524 | 0.78 s | 0.77 s | 0.23 s |
| fp-clang | 1,724 | 31 | 13,903 → 24,629 | 74,957 → 101,412 | 2,907 → 2,737 | 0.79 s | 0.77 s | 0.21 s |
| fp-gcc | 1,809 | 32 | 15,462 → 25,843 | 64,607 → 86,143 | 2,242 → 2,428 | 0.96 s | 0.87 s | 0.20 s |
| int_ops-clang | 2,453 | 68 | 12,776 → 33,603 | 79,129 → 90,852 | 3,479 → 3,383 | 0.94 s | 0.87 s | 0.31 s |
| int_ops-gcc | 2,312 | 69 | 12,989 → 32,563 | 68,000 → 82,571 | 2,901 → 2,837 | 1.02 s | 0.96 s | 0.34 s |
| simd-clang | 6,632 | 48 | 20,406 → 80,878 | 92,111 → 105,270 | 4,265 → 5,043 | 1.97 s | 1.53 s | 1.06 s |
| simd-gcc | 3,548 | 49 | 19,711 → 46,684 | 68,231 → 77,130 | 2,187 → 2,310 | 1.86 s | 1.71 s | 1.10 s |
| sjlj-clang | 1,593 | 35 | 21,240 → 23,087 | 122,538 → 79,650 | 3,231 → 2,714 | 0.59 s | 0.71 s | 0.23 s |
| sjlj-gcc | 1,592 | 36 | 13,157 → 21,808 | 54,897 → 79,600 | 2,217 → 2,291 | 0.88 s | 0.84 s | 0.26 s |
| switch-clang | 7,259 | 61 | 21,102 → 64,812 | 115,222 → 134,426 | 7,673 → 7,986 | 1.37 s | 1.14 s | 0.42 s |
| switch-gcc | 51,097 | 63 | 46,116 → 364,979 | 104,067 → 109,650 | 9,015 → 9,469 | 7.28 s | 6.19 s | 5.15 s |
| synthetic module | 106 | 9 | 1,606 → 1,606 | 106,000 → 53,000 | 2,865 → 1,656 | 0.11 s | 0.15 s | 0.11 s |
| all 19 modules | 95,977 | | 21,636 → 64,806 | 91,320 → 101,348 | 4,895 → 5,023 | 25.3 s | 22.5 s | 11.3 s |

Discovery is 7.9 times as fast on switch-gcc and 7.6 on except-gcc, 3 times over the corpus; the small NROs' lifting and compile rates move by a
few milliseconds either way (noise at this size). Peak memory: 65 to 209 MiB before (one module), 64 to 187 MiB with one worker, 73 to 239 MiB
with eight. The game-shaped benchmark (533,160 instructions, 4,088 functions; seconds, the units' seconds summed for lifting to code generation):

| Stage (seconds) | Baseline | Analysis and lifting fixed, one module, one thread | Units, 1 worker | Units, 8 workers |
|---|---:|---:|---:|---:|
| discovery | 243.4 | 6.13 | 5.97 | 3.63 |
| census (FPSR) | 0.58 | 0.000 | 0.000 | 0.000 |
| plan | - | 0.10 | 0.09 | 0.10 |
| lifting | stops (`type == Type::Opaque`) | 4.17 | 3.65 | 3.06 |
| verification | - | 0.92 | 1.25 | 0.94 |
| optimisation | - | 39.15 | 34.44 | 30.33 |
| code generation | - | 41.89 | 42.50 | 37.93 |
| link (lld) | - | 0.035 | 0.125 | 0.118 |
| compile, wall | - | 86.1 | 82.86 | 9.13 |
| whole pack, wall | - | 92.4 | 89.19 | 13.09 |
| peak memory | - | 1,188 MiB | 126 MiB | 226 MiB |

The baseline cannot pack the benchmark (cause 6 below); its discovery is `nxaot discover`'s and its census the pack stage's.

### The causes, and what was changed

Measured first with timers around discovery's steps, the plan, the lifting, the verifier, the passes and code generation (the log lines above),
LLVM's pass timers (`--time-passes`) and `getrusage` (the image has no perf or valgrind). The fixes keep every result: the discovery tables
(`nxaot discover --tsv`) of the eighteen NROs, the synthetic module and both benchmarks are byte for byte the baseline's, but for the count of
regions lifted (work, not a result); the tests below pass.

1. **The summaries** ([src/analysis/summary.cpp](src/analysis/summary.cpp), [discover.cpp](src/analysis/discover.cpp)): 87 % of switch-gcc's
   discovery (0.87 of 1.0 s) and 79 % of except-gcc's (0.57 of 0.73 s: 13 runs of the fixed point, 65 rounds, 3,120 functions summarised for 48
   functions). Every run started afresh after each discovery round that reached the summaries; each run computed every function in every round;
   the preservation analysis translated every instruction with dynarmic at every visit and interpreted every block in every iteration; the
   register effects were cached per pc and per run only; the union over the address-taken functions was recomputed for every blr of every
   function at every step. Now: `InsnCache` translates each distinct word once (twice more to find the immediates that follow the pc or its page:
   adr, bl's return address, literal addresses, adrp) into the register effects and a small program of the operations the analysis reads, which
   runs as the interpretation did (`NXAOT_CHECK_PROGRAMS=1` runs both on every instruction: no difference on the corpus); a block's instructions'
   effects are composed once per computation; a block is interpreted again only when its entry state changed (the same states, the same order);
   within a run a function is computed again only when a summary it reads changed since its last computation (the same rounds); across runs a
   function's summary for inputs seen before (its walk's state, its callees' summaries, the address-taken union) is taken from `SummaryMemo`; a
   run is skipped when nothing it reads changed.
2. **The jump tables** ([discover.cpp](src/analysis/discover.cpp), [jumptable.cpp](src/analysis/jumptable.cpp)): a br that no region bounds was
   tried again after every walk of its function and, past calls, after every run of the summaries, each try lifting up to 13 regions in a new
   LLVM context: on the benchmark 91,700 regions, about 70 s. Now a try is skipped when the function's walk state (a SHA-256 of what a region
   reads of it) and every call effect and target validity the last try read are unchanged; a region's analysis is kept by everything its lifting
   and analysis read (`RegionMemo`); and the functions' brs are tried on the workers, their results applied in function order. 3,376 regions on
   the benchmark.
3. **The census in the pack stage** ([src/build/pack.cpp](src/build/pack.cpp)): the whole census of the text ran to answer whether the code reads
   FPSR; `ReadsFpsr` ([census.cpp](src/analysis/census.cpp)) answers that alone, as the census does.
4. **The plan** ([src/build/plan.cpp](src/build/plan.cpp)): its own register effects per pc, now discovery's `InsnCache`; LIVE_AFTER's fixed point
   passed every function in every round, now only those whose LIVE_AFTER changed (it only grows by unions: the same result).
5. **Lifting, a fixed cost per function** ([src/lift/lower_calls.cpp](src/lift/lower_calls.cpp), `FnInitRegisters`): the prologue stored all 31
   x registers, 32 v registers, SP and the flags into their allocas in every function, about 250 IR instructions that SROA and dead-code
   elimination removed again: fnptr-gcc's lifted IR was 57,717 lines, 47,015 now. Only the registers some part of the function reads are
   initialised (the optimised code is the same up to register assignment: 8 of fnptr-gcc's 125 functions allocate other registers).
6. **A crash the benchmark found** ([src/lift/lower_fp.cpp](src/lift/lower_fp.cpp), `Exact`): the exact recompute of a NaN chain asked dynarmic for
   the instruction of an operand before checking whether it was an immediate (`fmov s1, #3.0` forwarded into an `fadd`), and dynarmic's assertion
   (`type == Type::Opaque`) stopped the pack: the baseline cannot pack the benchmark.
7. **One module for everything** (pack.cpp): every function was lifted into one LLVM module, optimised and compiled on one thread, with the
   module's memory (990 to 1,188 MiB on the benchmark). Units on workers, next.

Looked at and not the cause: the verifier (1 to 2 %), one LLVM context per function (there was one per module), DWARF (about 9 % of compile time
and a third of the units' object bytes on the benchmark; line tables only would save 6.5 % of the bytes: left as it is), dynarmic's decoder table
(60 to 70 ms per process, built once). The compile stage's own rate (LLVM's O1 pipeline and code generation on the lifted IR: InstCombine is 52 %
of optimisation, instruction selection 39 % and the assembly printer 19 % of code generation on the benchmark) is unchanged per core: it is the IR's
size per guest instruction, phase 5b's (below).

### Compile units on workers

[src/build/pack.cpp](src/build/pack.cpp) (design 9.3, 10): after the analysis (discovery and the plan, on the calling thread; discovery's jump
tables on the workers), each function is a compile unit (as phase 5a built them; since phase 5b a unit at tiers C and W is a group of
neighbouring functions, and a function over the split limit is a unit per part: "Phase 5b" below): its body and its standard entry lifted,
verified, optimised by the tier's pipeline and
compiled into an object of its own, in an LLVM context and module of its own, by one of `--workers` threads, each with its own target machine.
At H and H+, whose pipelines inline, a unit is a shard of neighbouring functions instead (16,000 to 64,000 instructions in address order, ended
after a function whose code's SHA-256 starts with a multiple of 8, so that a change moves the shards' ends near it only), and they inline within
the shard. One more object holds what every unit shares: the module's base, the empty inline-cache descriptor, the slow-path wrappers,
`jit_entry`, `enter`, the lookup table, the function map, the returns, the line table's bounds and the descriptor. Bodies, standard entries and
the shared code are hidden symbols, which units declare and lld resolves; lld links the tables' object and the units in entry order into the
shared object (`--shared`), or into one relocatable object (`-o`, `ld.lld -r`). Workers take the largest units first. A unit's object depends on
the unit only, so the number of workers changes nothing in the pack; the objects that are not the cache's go to `<output>.parts/`, removed after
the link. `--workers 0` (the default) takes one worker per CPU this process may run on (`sched_getaffinity`), at most one per 512 MiB of
`MemAvailable` (the whole pack stays under 240 MiB with 8 workers at tier W on the benchmark, under 600 MiB in the max mode: below).

Packs are deterministic: LLVM 18 writes the `!pcsections` groups of a function in the order of a `DenseMap` keyed by metadata pointers
(`AsmPrinter::emitPCSections`), so two compilations of the same unit can order its marks' section differently, but that section stays out of the
link (`SHF_EXCLUDE`) and the pack's line table is nxaot's, sorted ("Calls and returns"): two cold packs of switch-gcc are the same but for the
soname (checked after ABI 3; before it they differed in `nxaot_lines`). A warm repack reuses the cached objects and is byte for byte the cold pack.

The speedup (best of 3 per worker count by the whole pack's time; discovery's jump tables run on as many threads as there are workers):

| Workers | Benchmark: discovery | compile, wall | whole pack | speedup, compile / pack | peak memory | switch-gcc: whole pack | speedup | peak memory | synthetic module: whole pack |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 5.97 s | 82.86 s | 89.19 s | 1.00 / 1.00 | 127 MiB | 5.96 s | 1.00 | 185 MiB | 0.16 s |
| 2 | 4.62 s | 36.47 s | 41.40 s | 2.27 / 2.15 | 146 MiB | 4.75 s | 1.26 | 204 MiB | 0.12 s |
| 4 | 3.72 s | 17.89 s | 21.94 s | 4.63 / 4.07 | 171 MiB | 4.73 s | 1.26 | 226 MiB | 0.11 s |
| 8 | 3.63 s | 9.13 s | 13.09 s | 9.08 / 6.82 | 227 MiB | 4.72 s | 1.26 | 239 MiB | 0.11 s |

The benchmark scales with the workers: its 4,088 units are small and many, and the largest-first order leaves no long tail. Its units' summed
seconds are about 13 % higher with one worker than with two to eight (82 against 71 to 72), so the compile stage's speedup at 8 is 9.1. That is
not the main thread: pinned to one vCPU (`taskset -c 3`), one worker and two take the same time (86.0 and 84.2 s); the vCPUs of this virtual
machine run on the host's performance and efficiency cores, a possible cause (not isolated). switch-gcc stops at 1.3 times: its
`sw_word` (44,059 instructions) is one unit of about 4.5 s (functions split for size, phase 5b). The synthetic module is too small to gain (a
process's fixed costs). The 16,000-function benchmark (2,163,759 instructions, 16,276 functions) packs in 51.7 s with 8 workers (discovery 12.7
s, compile 37.5 s, link 0.43 s, peak memory 438 MiB): 41,800 instructions per second for the whole pack, as the 4,000-function one's 40,700.
In the max mode the benchmark is 31 shards of neighbouring functions, 15.3 s with 8 workers and a peak of 599 MiB.

### The per-function cache

[src/cache/](src/cache/) (design 13), `--cache <dir>`; for a game `<NSP folder>/<id>-work/aot-cache/`. An entry is `<dir>/<2 hex>/<key>.o`, the
unit's object, and `<key>.tsv`, its counts and notes, both written to a temporary name and renamed (the object first), so an interrupted pack
leaves no entry that looks whole and two packs may share a directory. The key ([pack.cpp](src/build/pack.cpp), `UnitKey`) is the SHA-256 of:
the key's format; nxaot.lock's LLVM and dynarmic, LLVM's version and this executable's SHA-256 (the chain version until `AOT_CHAIN` exists); the
target triple, CPU and features; the tier, the convention, the correctness mode, the debug information, line table and safepoint options,
FPCR and whether FPSR is modelled; the module's symbol prefix; the standard entries' signature and the indirect view (the union over
address-taken functions); then the function's entry and limit, its convention facts (PASS, WRITES, RV, returns, LIVE_AFTER, a standard entry,
setjmp continuations, landing pads, jump tables, roots), its instruction words from entry to limit with the pc-relative fields (b, bl,
b.cond, cbz, tbz, adr, adrp, literal loads) cleared and a reference number in their place, as `nso_match.py` normalises relocations, the
referenced addresses in order, and the convention facts of every function it references. A shard's key is the SHA-256 of its functions' keys.
`--asm`, `--spills` and `--ir` compile every unit (the cache holds objects only).

Limits: the lowering writes module offsets into the code (the body's symbol, return addresses, pcs, the line table), so the entry and the
referenced addresses are module offsets in the key, and a function that moved in a game update misses (its words, normalised, are the same:
reusing it needs the lowering to reach other code through symbols only, phase 5b). A change to a function's convention facts (a callee's WRITES,
say) misses the functions that read them, as it must.

CTest `cache` ([tests/test_cache.py](tests/test_cache.py)): every corpus NRO packed cold into a new cache (every function a miss) and warm with
another number of workers (every function a hit, the shared object byte for byte the cold one's); a copy of fnptr-gcc with one `movz` immediate
changed, in a function without a jump table, whose discovery tables are otherwise the original's: one miss, the other functions hits; fnptr-gcc
at tier C: every function a miss.

Measured with 8 workers (the default here): the corpus's packs (`build_packs.sh` with `NXAOT_ARGS='--cache <dir>'`: the eighteen NROs and the
synthetic module) cold in 12.7 s, 891 misses, 5,792,296 bytes written (891 objects, 5.9 MB with the facts files); warm in 3.3 s, 891 hits, every
shared object byte for byte the cold one's. The benchmark cold in 13.69 s, 4,088 misses, 24,821,360 bytes written; warm in 4.32 s, 4,088 hits,
the same shared object: 3.89 s of it are discovery, about 0.1 s the keys and 0.2 s reading the objects and linking. A warm repack is the
analysis's time.

### Extrapolation to the games after phase 5a, against section 10's targets

No game module is on this machine, so the benchmark's measured rates are carried over per instruction. That is an assumption: the benchmark's
discovery is linear in its size (533,160 instructions in 3.7 s and 2,163,759 in 13.3 s, the tables on 8 threads), but a game's call graph,
function sizes and jump tables may not be the benchmark's. Mario is the design's `main` and `sdk`, 8.5 million instructions and about 160,000
functions; Monster Hunter Rise's `main` 31 million and about 600,000 functions (the function counts scaled from Super Mario Bros. Wonder's
138,000 in 28 MB: assumed). Per second on this machine: the analysis (discovery and the plan) 87,879 instructions on one thread and 143,092 with
the jump tables on 8; compilation 7,377 per worker with 8 workers at tier W (6,509 with one), 15,650 at tier C and 6,648 in the max mode (H+
shards), so 8 workers compile 58,403, 122,933 and 47,502; lld links about 29 µs per unit. The reference machine (4 cores and 8 threads, Core
i3-10100 class) is taken as r = 0.5 to 0.7 of one of this machine's vCPUs per core, and its 8 threads as 5 of them (both assumed, not
measured):

| Pack | Mode | This machine, 8 workers | Reference machine (r = 0.7 .. 0.5) | Target on the reference machine |
|---|---|---:|---:|---:|
| Mario `main` and `sdk` | balanced, no profile (tier W) | 3.5 min | 7.1 min .. 9.9 min | at most 3 min |
| Mario `main` and `sdk` | every function at tier C | 2.2 min | 4.2 min .. 5.8 min | - |
| Mario `main` and `sdk` | max (H / H+) | 4.0 min | 8.3 min .. 11.7 min | at most 30 min |
| Mario `main` and `sdk` | repack, warm cache (the analysis and the link) | 1.1 min | 1.5 min .. 2.1 min | at most 1 min |
| Monster Hunter Rise `main` | balanced, no profile (tier W) | 12.7 min | 25.8 min .. 36.1 min | at most 10 min |
| Monster Hunter Rise `main` | every function at tier C | 8.1 min | 15.2 min .. 21.2 min | - |
| Monster Hunter Rise `main` | max (H / H+) | 14.8 min | 30.4 min .. 42.6 min | at most 30 min |
| Monster Hunter Rise `main` | repack, warm cache (the analysis and the link) | 3.9 min | 5.6 min .. 7.8 min | at most 1 min |

On the reference machine, of section 10's targets only Mario's max pack is met (8 to 12 minutes against at most 30); Monster Hunter Rise's max
pack is at its limit or over it (30 to 43 minutes); the balanced packs take 2.4 to 3.6 times their targets, and even every function at tier C
(the floor of the balanced mode with a profile) misses Mario's 3 minutes. A warm repack is bounded by the analysis, which the cache does not keep:
the benchmark's warm repack is 4.32 s, 3.89 s of it discovery, and the games' would be 1.5 to 8 minutes against 1. The compile stage's rate
per core and the analysis's are the gap; phase 5b's items below close it.

### What phase 5a left, and what was found on the way

Phase 5b's list as phase 5a wrote it; "Phase 5b" below says what became of each item.

- **The compile stage's rate per core**, now most of a pack: LLVM's O1 pipeline and code generation on the lifted IR run at 6,509 to 7,377
  guest instructions per second per core at tier W, as phase 1 measured. The IR is the cost (fnptr-gcc's lifted IR is 47,015 lines for 1,603
  instructions before the passes; the NaN-chain cold blocks, the polls, the stores at calls and returns; InstCombine is half of the
  optimisation, instruction selection two fifths of code generation): tiers by profile (tier C is 2.1 times as fast per worker on the
  benchmark, for code the profile never samples), a leaner lowering, and functions split for size (switch-gcc's `sw_word`, 44,059
  instructions, is one unit and bounds its pack's wall time).
- **A unit's fixed cost**: about 0.5 to 1.5 ms of pass-pipeline and object set-up per unit (fnptr-gcc's 62 units: 0.758 s of compile CPU against
  0.725 s in one module), some 5 to 15 CPU-minutes over Monster Hunter Rise's estimated 600,000 functions, and about 2.7 KiB of object per unit
  (DWARF headers, sections, symbols: the benchmark's units are 25.2 MB against 14.1 MB as one object, its shared object 10.2 against 9.4 MB).
  Pipelines kept per worker, or units of a few functions where the cache keys them per function, are the options.
- **Inlining across units**: H and H+ inline within a shard of neighbouring functions; design 9.3's ThinLTO import, devirtualisation and
  function ordering are phase 5b's.
- **The cache across game updates**: the lowering writes module offsets into the code, so a function that moved misses (above).
- **The analysis, cold and warm**: the summaries' fixed point and the walks run on one thread (by design: the summaries' rounds are an order
  on which the results depend; the tables run on the workers), 143,092 instructions per second here; and the cache keeps objects only, so a warm
  repack runs the whole analysis again (the benchmark's: 3.89 of 4.32 s). Keeping the analysis's results (per module, keyed by its SHA-256
  and the tool's, or per function as the objects are) is what a warm repack within a minute needs.
- **One process per module**: dynarmic's decoder table costs 60 to 70 ms per process; FINAL FANTASY X/X-2's 1,432 NROs packed one process each
  would pay it 1,432 times (one pack for every module, phase 4's list, removes it).
- **Found here, fixed since (ABI 3)**: (1) the line table's entries were not {native, offset} pairs whenever several machine instructions share
  one guest access's metadata node (a guest `ldp`, dc zva's stores, a loop rotated or duplicated by LLVM): LLVM writes such a group as all its
  addresses and then the offset once (`AsmPrinter::emitPCSections`), and both readers took 8-byte pairs: 246 of fnptr-gcc's 415 entries and 8,812
  of switch-gcc's 24,293 named an address outside the library's code or an offset outside the module's text. nxaot now reads the groups back from
  each unit's object by their relocations and writes a table of its own ("Calls and returns"); CTest `lines` checks every entry (fnptr-gcc now
  487 entries, switch-gcc 24,378, all at instructions of their function's code and at loads and stores of its guest code). (2) LLVM 18 orders
  those groups by pointer hash: the marks' section is now kept out of the link, and the packs are deterministic. **Open**: (3) discovery does
  not bound a table index computed from several values (above).

### Phase 5b

The compile stage's rate per core, functions split for size, units of several functions, the analysis kept for a warm repack, and tiers by
profile, in that order; ThinLTO import was left (below). Measured as phase 5a was, on the same machine and benchmark (best of 3 unless said;
the machine was shared with other agents' builds at times, and the runs below waited for its one-minute load to fall under 1.5), and checked
by the same tests: CTest 20 of 20 (18 before, `split` and `profile` new), the vectors at 0 mismatches (179,296; the x86-64-v3 crypto set 9,600;
the per-operation NaN mode 20,856), the per-function differential test at 0 failures of 3,724 runs, every corpus NRO printing its expected
output natively in every configuration, CTest `lines` at 0 bad entries.

#### 1. A leaner lowering

Where the IR went, measured with `--ir` and `--ir-opt` (new: the IR after the tier's passes, what code generation is given) and LLVM's `opt`
and `llc` with `-time-passes` over the dumped units of a 400-function benchmark (seed 3, 57,708 instructions): of the 470,477 IR instructions
the units held before the passes, SROA left 281,407; of those, the NaN chains' cold blocks were 35,909 (12.8 %), the non-local return paths
24,168 and the safepoints' cold blocks 12,100; after the O1 pipeline the NaN chains' cold blocks were still 21,259 of 151,851 instructions
(14 % of what code generation compiled), most of it the integer form of Arm's NaN rules (FPProcessNaNs: some thirty operations per escape).
InstCombine was 40 % of the optimisation: the O1 *module* pipeline runs it eight times, its function pipeline four. Before SROA a function
carried 71 allocas (every register, flag and FPSR accumulator, 4,331 of fnptr-gcc's 42,240 instructions), an FPSR flush of some ten
instructions at every call, return, poll and exit whether or not the function raised a flag, and dynarmic's spelling of every `mov`, address
and carry-less add (`add x, 0`, `shl x, 0`, `select true`, `or 0, x`). The changes, each keeping the code what InstCombine would make of it:

- **The NaN rules out of line** ([src/lift/lower_fp.cpp](src/lift/lower_fp.cpp), `NaNCall`): a chain's cold path (`Escape`) calls
  `nxaot.nan<1|2|3>.<type>`, the same rules made by `NaN1` / `NaN2` / `NaN3` once per unit, `linkonce_odr` in a comdat (the link keeps one),
  `noinline`, `cold`, `preserve_all`, no memory access. The hot path is unchanged; the cold blocks shrank from 21,259 to 6,026 instructions
  after the passes.
- **Tier W's pipeline** ([src/build/pack.cpp](src/build/pack.cpp), `Optimise`): design 9.3's "O1 function pipeline"
  (`buildFunctionSimplificationPipeline(O1)` per function) instead of phase 5a's whole O1 module pipeline, whose interprocedural passes have
  nothing to do in a unit of separately compiled functions and whose optimisation pipeline ran InstCombine four more times: on the 400-function
  benchmark the passes' time fell by a third, for 1.4 % more instructions after them.
- **Folded as it is built** ([function_lifter.h](src/lift/function_lifter.h)): the builder is `IRBuilder<InstSimplifyFolder>`, so what
  InstSimplify folds (the forms above) is never made; InstCombine runs InstSimplify on every instruction, so the code is the same.
- **What the passes only removed** ([lifter.cpp](src/lift/lifter.cpp), `FnCleanup`): a function whose lowering raised no FPSR bit and no QC has
  its flushes removed (they were `st->fpsr |= 0`), then every alloca nothing loads, with its stores.

| fnptr-gcc (1,603 instructions) | phase 5a | phase 5b |
|---|---:|---:|
| `--ir` lines | 52,133 (32.5 per instruction) | 39,088 (24.4) |
| IR instructions before the passes | 42,240 (26.4 per instruction) | 29,195 (18.2) |
| IR instructions after them | 11,594 | 12,042 |
| 400-function benchmark (57,708 instructions): `--ir` lines | 721,511 (12.5 per instruction) | 565,909 (9.8) |
| IR instructions before the passes / after them | 605,688 / 151,822 | 447,850 / 142,913 |

Looked at and not changed, with the measurement that decided it: the polls (`--no-safepoints` saves 6 % of the compile on the 400-function
benchmark, 0.35 of 6.0 CPU seconds; design 6.2's rule stays), debug information and the line marks (`--no-debug-info`, `--no-line-table`: no
difference outside the noise), NZCV stored at calls (141 stores of NZCV in the 142,913 instructions after the passes), the field addresses of
the state block computed once per function (at tier C, whose code generation has no CodeGenPrepare, they would stay live in registers across
the function), and flags computed only when read (some 2,000 flag-setting instructions in the 400-function benchmark at about eight IR
instructions each, which InstCombine removes when unused: about 3 % of its input, not done). Code generation is now three quarters of the
compile at tier W and is where the time is: instruction selection about a quarter of it, the assembly printer a ninth; its cost follows the
IR left after the passes, whose cold paths (safepoints 6 %, non-local returns 4 %, NaN chains 4 %) are what the design asks for.

#### 2. Functions split for size

[src/build/pack.cpp](src/build/pack.cpp) (`SplitParts`), [src/lift/lower_calls.cpp](src/lift/lower_calls.cpp) (`FnSplit`, `FunctionPlan::
part_entries`): a function of more than `--split-above` instructions (8,192 by default) becomes parts of a quarter to a half of that, cut in
address order where the fewest of its edges cross, at instruction boundaries outside exclusive windows (block starts, and every 256th
instruction inside a long straight run: `sw_word` is 19 blocks of up to thousands of instructions); a function reaching the aux region is
kept whole (its gate sites are checked per function). Each part is an LLVM function `<body>.part<offset>` and a compile unit of its own,
entered with the whole register file in the state block, the function's entry SP and lr, and the module offset of the block it starts at in
x0's argument (a switch over its entries). The function's body is a stub that stores what it was passed (and sp, lr), polls, and tail-calls
the part holding the entry; a transfer to another part's block stores the whole register file, polls when it goes backwards (a loop through
parts) and tail-calls that part (musttail: swifttailcc allows another prototype, ghccc's is uniform); a landing pad or setjmp continuation in
another part is reached the same way from the pending path. A return stores every x and v register outside WRITES again (the whole
function's restore set, which no part sees). The parts' objects are joined by `ld.lld -r` into one object whose code is one section, so the
line table's offsets from the body hold for every part and the pack keeps one function map row per guest function (ABI 3 unchanged); the
fault report names the function for an address in any part.

Deviations from design 9.3: the cut points are chosen on discovery's CFG (the rewriter cuts its analysis after `ret` / `b` / `br` with no
branch crossing); a transfer stores the whole register file rather than the registers newer than the block. The second was found by the new
test: storing only the dirty registers left fnptr-gcc's `main` resuming in its last part with the SP an inner frame had left in the block
(SP and lr travel in host registers, so the block's copies are the last ones some inner frame stored: include/nxaot/aot_module.h,
`NXAOT_LOOKUP_SP_KEPT`), and a callee's safepoint poll, gate or JIT continuation stores registers it restores before returning, which only its
inner calls and svcs make it store again; the function itself holds the right values in SSA, the next part reads the block. Found while
reading those failures, though not their cause, and fixed for every function: after a call the lowering kept the block's dirty set of before
the call, so a transfer that followed the call in the same block (a part's, or a tail call into the next function after a call that returns
there) would not store x0, x1 and the returned v registers when they had not been dirty before it (`FnCall` now updates it as the data flow
does).

CTest `split` ([tests/test_split.py](tests/test_split.py)): every corpus NRO packed with `--split-above 64` (414 functions in 14,925 parts,
across calls, loops, jump tables, landing pads, setjmp continuations, fibers and threads), in both conventions and the correctness mode, runs
natively printing its expected output, and its line table passes `nxaot lines`; switch-gcc at the default limit splits `sw_word` into 21
parts and prints its output. With `--split-above 24` and 150 (ghccc) and 24 in the max mode every NRO runs too (checked by hand, not a test).

| switch-gcc (51,097 instructions; `sw_word` 44,059) | whole pack, 8 workers | compile CPU (optimise and codegen) | peak memory | text |
|---|---:|---:|---:|---:|
| phase 5a | 4.99 s | 5.47 s | 240 MiB | 238,584 bytes |
| lowering (1), `sw_word` whole (`--no-split`) | 4.61 s | 4.81 s | 229 MiB | 240,490 bytes |
| `sw_word` in 21 parts | 0.67 s | 2.30 s | 136 MiB | 367,886 bytes |
| and groups (3) | 0.74 s | 2.53 s | 138 MiB | 368,287 bytes |

The parts compile faster in sum (LLVM's superlinear cost) as well as in parallel; their code is half again as large (the stores and loads of
the register file at 33 transfers, and each part's prologue).

#### 3. The unit's fixed cost: groups of functions

Measured first (timers around each unit's set-up, since removed): the O1 pipeline's construction cost 25 µs per unit and the LLVM context and
kit 37 µs; the code generation pass manager's construction and destruction 0.54 ms per unit at tier W and 0.19 ms at tier C (some 3 to 4 % of
code generation on the benchmark), plus each object's own ELF and DWARF headers, sections and symbols, its cache entry and its link input.
Keeping the code generation pass manager across modules would mean re-running LLVM's legacy pass manager, its MachineModuleInfo and its
streamer on another module, which LLVM does not do itself; units of several functions remove the same cost and the per-object ones with it.
So at tiers C and W a unit is now a group of neighbouring functions of one tier (`Units`): a group ends after a function whose code's SHA-256
starts with a multiple of 4 once it holds a quarter of the group limit, and at the limit, 1,024 instructions (in a module of less than 32
limits' worth a thirty-second of it, at least 64, so that a small NRO keeps units for its workers; above that the limits are fixed, so a
game module's groups do not move with its size); the functions are still optimised one at a time (the function pipeline does not inline). The cache stays keyed per function: a group's key is the SHA-256 of its functions' keys,
as a shard's is, so a change misses its group only (`aot: cache` counts the units and their functions). `--no-groups` restores one function
per unit.

| | units | compile CPU | object bytes | per unit | shared object |
|---|---:|---:|---:|---:|---:|
| fnptr-gcc, one function per unit (`--no-groups`), 1 worker | 61 | 0.56 s | 308,784 | 5,062 | 111,840 |
| fnptr-gcc, groups of up to 1,024 instructions, 1 worker | 4 | 0.51 s | 151,072 | 37,768 | 96,032 |
| fnptr-gcc, groups of up to 64 (its size's limit), 1 worker | 17 | 0.51 s | 187,984 | 11,058 | 99,720 |
| benchmark (4,000), one function per unit, tier W, 8 workers | 4,088 | 52.84 s | 27,137,392 | 6,638 | 9,765,536 |
| benchmark, groups | 688 | 50.06 s | 16,982,416 | 24,684 | 8,816,280 |
| benchmark, one function per unit, tier C | 4,088 | 25.91 s | 32,983,184 | 8,068 | 15,747,896 |
| benchmark, groups, tier C | 688 | 22.32 s | 22,838,960 | 33,196 | 14,797,920 |

(Phase 5a's fnptr-gcc: 61 units and the tables, 0.65 s of compile CPU with one worker in these runs, 0.758 s in its own table.) fnptr-gcc as 4
units of up to 1,024 instructions took 0.35 s with 8 workers against phase 5a's 0.20 s as 61 units: hence the smaller limit for small modules;
as 17 units, 0.19 s.

#### 4. The analysis cache

[src/build/analysis_cache.h](src/build/analysis_cache.h): with `--cache`, what the pack stage takes from discovery and the plan is kept per
module in `<cache>/analysis/<key>.bin`, under the SHA-256 of the format, the tool's identity (nxaot.lock, LLVM, this executable's SHA-256:
the chain version until `AOT_CHAIN` exists, as the units' keys have it), the module's name and image SHA-256, a behaviour image's manifest
file, the convention, `--all-entries` and the split limit. It holds per function the entry, limit, block bounds (the gate sites and the
returns' table read them) and call sites, for a function the pack may split also the blocks' successors and the exclusive windows; the census's
FPSR answer; discovery's notes; the whole plan. A warm repack reads it instead of running discovery and the plan. The file is written through a
temporary name and renamed; its payload is followed by its own SHA-256, and an entry that is short, fails its checksum, names another key or
does not parse is ignored (`aot: analysis cache: miss (<why>)`) and written again: never trusted. A changed tier or profile reuses it (the
analysis does not depend on them). CTest `cache` ([tests/test_cache.py](tests/test_cache.py)) now checks the cold pack misses and the warm one
hits it for every corpus NRO, a changed instruction word misses it (another image), a changed tier hits it, and an entry with a byte changed,
cut short, unparsable with a valid checksum or of another key is ignored, the pack byte for byte the cold one's, and the entry rewritten.
The 16,000-function benchmark's warm repack fell from 15.42 s (13.4 of them discovery) to 0.81 s, its entry 1.6 MB (about 22 MB at Monster
Hunter Rise's size); extrapolated, 27 to 37 s for Monster Hunter Rise's `main` on the reference machine (below), against the design's minute.

#### 5. Tiers by profile

`nxaot build --profile <file>` (design 9.3: "tier H only with a profile"): in the balanced mode a function holding a sampled offset is compiled
at tier H and the others at tier C; in the max mode at H+ and H. Without a profile nothing changes (W, or H+ in the max mode); `--tier` and
`--profile` exclude each other. The format, one sample per line:

```
# comment (from '#' to the end of a line); blank lines are ignored
<guest module offset, hexadecimal, 0x optional> <count, decimal>
0x1a2c40 118
1a2c58	3
```

Separated by spaces or a tab; an offset may repeat (the counts add up); a count of 0 does not make its function hot; an offset in no function
is counted (`aot: profile <file>: <s> samples at <o> offsets, <x> outside every function`) and ignored; a malformed line stops the build with
its file and line number. The host does not write this file yet (design 12's sampler, `dm/games/<id>/aot_profile/<build ID>.tsv` with
function offsets, samples and instruction offsets, is to come; this format is its instruction-offset column). The counts only choose the tier:
LLVM's sample profile (the design's "the sample profile" of tier H) is not fed from them yet. A function's tier is part of its cache key;
units are of one tier each. CTest `profile` ([tests/test_profile.py](tests/test_profile.py)): a profile written by hand for fnptr-gcc,
fp-clang, except-gcc and switch-gcc (three functions sampled at their entries, last instructions and a second offset, a sample outside every
function, a count of 0): the logged tiers are C n-3 and H 3 in the balanced mode, H n-3 and H+ 3 in the max mode (fnptr-gcc: `aot: tiers C 58
W 0 H 3 H+ 0`), the profile line counts what the file holds, and every pack prints its expected output; a malformed profile stops the build
naming its line.

#### 6. ThinLTO import: not done

Left open, as the brief allowed: items 1 to 5 took the phase. What it would take: H and H+ units are shards compiled in parallel, so importing
a callee's body into another shard needs ThinLTO's per-module summaries, an import list per shard, and the imported functions' cache keys in
the importing shard's key (a change in a callee then misses every shard that imported it). The bodies are hidden external functions, so an
imported copy would be `available_externally`; the lifter's IR has no other obstacle. Devirtualisation and function ordering need the profile's
indirect-call targets and hot functions, which the host does not record yet.

#### The packs after phase 5b

Best of 3 (the one-worker benchmark runs and the cold runs with a cache: best of 2 and 1), the same machine as phase 5a's tables, phase 5a's
binary measured again beside phase 5b's in the same sessions; seconds; "compile CPU" is optimisation and code generation summed over the
units; the rate is guest instructions per second per core of that CPU time:

| Pack | phase 5a: whole pack | compile CPU | rate per core | phase 5b: whole pack | compile wall | compile CPU | rate per core | peak memory |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| fnptr-gcc, tier W, 8 workers | 0.20 | 0.82 | 1,950 | 0.19 | 0.10 | 0.64 | 2,497 | 90 MiB |
| switch-gcc, tier W, 8 workers | 4.99 | 5.47 | 9,336 | 0.74 | 0.55 | 2.53 | 20,165 | 138 MiB |
| benchmark (4,000), tier W, 1 worker | 91.92 | 78.77 | 6,768 | 70.81 | 63.73 | 57.64 | 9,249 | 125 MiB |
| benchmark (4,000), tier W, 8 workers | 13.68 | 71.33 | 7,475 | 10.95 | 6.86 | 50.06 | 10,650 | 232 MiB |
| benchmark (4,000), tier C, 8 workers | 8.38 | 29.89 | 17,838 | 7.36 | 3.38 | 22.32 | 23,889 | 213 MiB |
| benchmark (4,000), a profile of 10 % (H 409, C 3,679), 8 workers | - | - | - | 7.92 | 4.03 | 27.26 | 19,561 | 225 MiB |
| benchmark (4,000), the max mode (H+), 8 workers | 15.3 | - | - | 14.01 | 10.10 | 67.35 | 7,916 | 486 MiB |
| benchmark (16,000), tier W, 8 workers | 52.69 | 283.75 | 7,626 | 44.16 | 28.39 | 207.66 | 10,420 | 450 MiB |
| benchmark (16,000), a profile of 10 %, 8 workers (machine loaded) | - | - | - | 33.49 | 18.83 | 129.01 | 16,772 | 436 MiB |
| the corpus's packs (`build_packs.sh`), cold, with a cache | 13.39 | | | 8.26 | | | | |
| the same, warm | 3.60 | | | 3.17 | | | | |
| benchmark (4,000), repack with a warm cache | 4.10 | | | 0.27 | 0.13 | | | 93 MiB |
| benchmark (16,000), repack with a warm cache | 15.42 | | | 0.81 | 0.40 | | | 189 MiB |

The rate per core at tier W rose by 37 % with one worker (6,768 to 9,249) and 43 % with eight; at tier C by 34 %. Of the benchmark's 11 s at tier W
with 8 workers, discovery and the plan are now 3.9 s: the analysis is the next bound of a cold pack (below). The warm repack reads the analysis
(0.06 s for the 16,000-function benchmark's 1.6 MB entry), computes the units' keys on the workers (1.4 s of CPU), takes 2,693 objects from
the cache and links them (0.16 s); the corpus's warm packs are 21 processes, each paying its fixed start (the decoder table, the executable's
SHA-256 for the keys).

#### Extrapolation to the games after phase 5b

The same model and assumptions as phase 5a's (above: the benchmark's rates per instruction, Mario 8.5 and Monster Hunter Rise 31 million
instructions, the reference machine at r = 0.5 to 0.7 of one of this machine's vCPUs per core and its 8 threads as 5 of them; the analysis
divided by r, the compile stage and the warm repack by 5 r / 8). Rates on this machine with 8 workers, from the 4,000-function benchmark: the
analysis 135,700 instructions per second; compilation 77,700 at tier W, 157,700 at tier C, 132,300 with a profile of 10 % (H) and 90 % (C),
52,800 in the max mode; a warm repack 2.67 million (the 16,000-function benchmark: 0.81 s).

| Pack | Mode | This machine, 8 workers | Reference machine (r = 0.7 .. 0.5) | after phase 5a | Target |
|---|---|---:|---:|---:|---:|
| Mario `main` and `sdk` | balanced, no profile (tier W) | 2.9 min | 5.7 .. 7.9 min | 7.1 .. 9.9 min | at most 3 min |
| Mario `main` and `sdk` | balanced with a profile (10 % H, the rest C) | 2.1 min | 3.9 .. 5.5 min | - | at most 3 min |
| Mario `main` and `sdk` | every function at tier C | 1.9 min | 3.5 .. 5.0 min | 4.2 .. 5.8 min | - |
| Mario `main` and `sdk` | max (H+) | 3.7 min | 7.6 .. 10.7 min | 8.3 .. 11.7 min | at most 30 min |
| Mario `main` and `sdk` | repack, warm cache | 3.2 s | 7 .. 10 s | 1.5 .. 2.1 min | at most 1 min |
| Monster Hunter Rise `main` | balanced, no profile (tier W) | 10.5 min | 20.6 .. 28.9 min | 25.8 .. 36.1 min | at most 10 min |
| Monster Hunter Rise `main` | balanced with a profile (10 % H, the rest C) | 7.7 min | 14.4 .. 20.1 min | - | at most 10 min |
| Monster Hunter Rise `main` | every function at tier C | 7.1 min | 12.9 .. 18.1 min | 15.2 .. 21.2 min | - |
| Monster Hunter Rise `main` | max (H+) | 13.6 min | 27.8 .. 38.9 min | 30.4 .. 42.6 min | at most 30 min |
| Monster Hunter Rise `main` | repack, warm cache | 11.6 s | 27 .. 37 s | 5.6 .. 7.8 min | at most 1 min |

Met on the reference machine: both warm repacks (well under a minute) and Mario's max pack; Monster Hunter Rise's max pack is at its limit
(28 to 39 minutes against 30). Not met: the balanced packs, 1.3 to 1.8 times Mario's 3 minutes with a profile and 1.4 to 2 times Monster Hunter
Rise's 10. In both, the analysis is now about two fifths of the time (Mario with a profile: 89 to 125 s of 236 to 331), so the next gains are
the analysis's rate on a cold pack (its summaries and walks on one thread) and the compile stage's at tier C (code generation, three quarters
of it).

#### What is left

- **The analysis is now a third of a cold pack** (3.6 to 3.8 s of the 4,000-function benchmark's 7.4 to 11 s with 8 workers): discovery runs at
  about 140,000 instructions per second here, its summaries' fixed point and walks on one thread. The analysis cache removes it from a warm
  repack only.
- **Code generation** is three quarters of the compile at tier W; what is left of the lowering's fat is small (above), so tier C for code the
  profile never samples is the lever, and with it the host's sampler that writes the profiles.
- **The cache across game updates**: unchanged (the lowering writes module offsets into the code).
- **One process per module**: unchanged.
- **ThinLTO import, devirtualisation, function ordering, LLVM's sample profile at tier H**: open (5 and 6 above).

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

dm/host's JIT fallback (`NXHOST_JIT`, the same dynarmic) needs the same headers, which the host's image (docker/Dockerfile.build) does not have: there
the host builds without it, and CI builds it in this image. Adding `libboost-dev` to Dockerfile.build in a layer of its own is the follow-up (not done:
the development machine's disk).

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
17. **Single to half with DN on AVX-512 hosts.** The Arm ARM's `FPConvert` raises Invalid Operation for a signalling NaN input whatever FPCR.DN
    says (DN only chooses the default NaN as the result). dynarmic's F16C path for `FPSingleToHalf` (`fcvt h, s`, and `fcvtn` / `fcvtn2` of
    single lanes, which its A64 frontend converts lane by lane) forces the default NaN before `vcvtps2ph` (`ForceToDefaultNaN`,
    `backend/x64/emit_x64_floating_point.cpp:142`): on a host with AVX-512 F, VL and DQ through `vfpclassss` and a masked blend, which raise
    nothing, so the signalling NaN never reaches an instruction that signals; on other hosts through `vcmpunordss`, which raises Invalid
    Operation for it. So the reference agrees with the Arm ARM here and not on AVX-512 runners: found by the aot workflow on GitHub (an
    icelake-server CPU), `4e216a04` (`fcvtn2 v4.8h, v16.4s`, FPCR 0x2000000, v16.s[0] = 0x7fbfffff) gave FPSR 0x14 from the reference and 0x15
    lifted, and `0e2169b8` (`fcvtn v24.4h, v13.4s`, FPCR 0x3400000, v13.s[3] = 0xffa00000) 0x10 and 0x11 (simd_misc.17828 and .17877; the lanes are
    the seed's, recomputed from `src/vec/vectors.cpp`). The lowering (`vcvtps2ph` on the operand, which signals) is right; the filter relaxes
    lifted-only IOC for regions with a single-to-half conversion under DN only when the reference host has those AVX-512 features
    (`KnownIssue::host`), so that on other hosts the bit is still compared exactly. The JIT fallback inherits the issue on such hosts. Lifted-only
    IOC (AVX-512 hosts).

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
| 1. Skeleton | decoding, discovery, lifting of integer / memory / branch / call code, state block, dispatcher, SVC path, vector harness, calling-convention benchmarks | **done** except what follows: the lifter (0 mismatches over 24,400 integer vectors), the module loader, discovery, the summaries; function mode with the convention of 2.3 in two LLVM mappings (swifttailcc, the default by the measurements, and ghccc for preserve_none, which LLVM 18.1 lacks), calls, returns checked against lr, tail calls, `pending` propagation with resume points at setjmp continuations and landing pads, the correctness mode; jump tables as switches, inline caches, standard entries, the lookup table, the miss path; `nxaot build` (tiers C, W, H, H+; the balanced and max modes; an object with the lookup table, function map and line table, a shared object by lld); `nxaot_host` (the arena below 2^39, the SVC path, the counter, faults through the line table); the corpus natively in both conventions and the correctness mode, equal to dynarmic and to the expected output; nxaot_funcdiff on every corpus function; the measurements ("Measurements"). Not done: the corpus natively on ARM64 hardware (an Apple-silicon Mac), one tier per function and profiles (phase 5), ThinLTO, functions split for size, the oracle-T / J / W comparisons, the pack throughput targets of section 10 (not met: "Pack throughput") |
| 2. FP and SIMD | every FP, NEON and crypto lowering, NaN chains | every opcode the A64 frontend emits lowered; 0 mismatches over the committed vectors and 7,000,000 random floating-point and SIMD ones (10^6 per class for seven of the eight classes, `fp_seq` not completed; not the 10^8 of the acceptance); the chain checks measured. Not done: the row-13 FZ slow path for the basic operations, inline estimates, combined escape checks, the run-time helpers in the host |
| 3. Control flow and JIT | resume points, `pending`, safepoints, dynarmic integration | **done** except what follows: the JIT fallback on dynarmic unmodified (`dm_jit_create` / `dm_jit_run` / `dm_jit_halt`, design 11's configuration, the stops at compiled entries and call sites' returns through the code reader, the hand-over measured), every case of section 11 handed to it (the dispatcher's misses, branches to no entry, FPCR away from the specialisation, untranslated encodings, code that runs into no entry, refused functions, continuations with no native frame), the driver that calls compiled entries natively and propagates non-local transfers; C++ exceptions through LSDA landing pads with a personality-free unwinder, fibers and a return past the caller in the corpus; guest threads with the thread and synchronization SVCs, per-thread state blocks, native stacks and JIT instances, the shared monitor; safepoint polls and the pause protocol with the pause test; the three JIT modes, the threads and the pause test in CTest. The acceptance: the exception, longjmp and thread corpus passes natively, in the JIT and with no JIT where none is needed; `--jit-all` and the compiled runs print the same lines; the pause test passes (CTest `corpus_jit`, `corpus_threads`, `safepoints`). dm/host's side done for the corpus (`dm/host/aot_*.c`, CTest `t_aot_host`; its open items are in the list at the end of "Threads and safepoints"). Not done: the exactness of a JIT target's callee-saved registers is assumed (a compiled caller keeps what the union of address-taken functions preserves across an indirect call); the corpus has no untranslated encoding, no refused function and no run-off into a non-entry (those hand-overs are built and not run); priority and core of threads (FIFO order, no affinity), Horizon's other synchronization objects in this run time (events, condition variables: SignalProcessWideKey / WaitProcessWideKeyAtomic; dm/host's thread layer has them); the discovery gap except-gcc shows (a halfword jump table whose base is set before its loop) |
| 4. to 6. | games on the Intel Mac, performance, breadth | phase 5's first half, pack throughput, done ("Pack throughput": the stages measured and logged, the analysis's causes fixed, compile units on workers, the per-function cache; section 10's targets extrapolated and not met; what is left is phase 5b's); the rest not started. Prepared for phase 4: the host's run time of packs (dm/host/aot_*.c; Linux checked by `t_aot_host`, Windows written and not run, the Intel Mac not built: the aot files are CMake's, and aot_fault.c reads Linux's signal context). Phase 4's first half, without a game ("A game module in the host"): `patch_nso.py --aot` with its manifest checked against nxaot's reader and discovery, the loader's game-module path (manifest, image, gates, packs), NSO decompression on that path (the rewriter's, as for Apple: nothing in the host), host functions and unimplemented imports bound into a module's GOT, `be_guest_map` / `be_guest_unmap` from the memory SVCs; CTest `t_aot_module` and `t_guest_map`. Done since, in nxaot (ABI 3): the lifter's calls of the observation gates and spin waits (design 1.1; the descriptor's gate sites, checked by the host against the manifest's rows), the frame step's aux region lifted as code of the functions that branch into it, the standard entries' common signature (calls between packs bound in inline caches, no JIT hand-over), the line table nxaot's own and read correctly by both readers (CTest `lines`). Still to come for a first game module: NROs on the game-module path, thunks for hle_table.c's libc passthrough, main.c's start sequence through `dm_guest_call`, a host program for x86-64, Boost in the host's image for the JIT fallback (some of these are the host's and done since there: docs/design/migration-status.md) |
