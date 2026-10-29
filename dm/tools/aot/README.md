# nxaot: the ahead-of-time ARM64 to x86-64 recompiler

Phase 2 of the Windows and Linux port ([migration-status.md](../../../docs/design/migration-status.md)); the design is
[aot-recompiler.md](../../../docs/design/aot-recompiler.md). This folder holds its foundation: the build, the lifter of the integer, memory, branch,
system-register and exclusive instruction classes, the differential vector harness against dynarmic, and the census of phase 0. No game runs through
it yet, and nothing here touches the Apple builds.

## What exists

| Piece | Where | What it does |
|---|---|---|
| State block | [include/nxaot/guest_state.h](include/nxaot/guest_state.h) | `dm_guest_state` of design section 2.1, C, 64-byte aligned, every offset asserted; host-only fields `cnt` (the counter `mrs cntpct_el0` / `cntvct_el0` read) and `exit_reason` / `exit_arg` |
| Decoder glue | [src/decode/translate.cpp](src/decode/translate.cpp) | dynarmic's A64 translation loop with nxaot's options (hints raise their exceptions, wall-clock counter) and the encodings dynarmic leaves to an interpreter that nxaot translates itself (`mrs xN, cntvct_el0`) |
| Lifter | [src/lift/lifter.cpp](src/lift/lifter.cpp), [src/lift/opcodes.cpp](src/lift/opcodes.cpp) | dynarmic IR blocks of a region to one LLVM function `void f(dm_guest_state *st)`: registers in allocas that SROA / mem2reg promote, NZCV as four `i1` compared directly against the operands of the `subs` / logical operation that set them, stored only when written; guest memory as `inttoptr` volatile accesses (alignment 1), `ldar` acquire, `stlr` sequentially consistent, exclusives as `cmpxchg` against `st->excl_*`, `dc zva` zeroing the aligned 64 bytes; `svc`, `brk`, `udf` and undefined encodings leave with `st->pc` and `st->exit_reason`. A region holding an IR opcode the lowering does not cover is refused whole |
| ORC JIT | [src/lift/jit.cpp](src/lift/jit.cpp) | LLVM 18 ORC LLJIT, the default O2 pipeline, the host's CPU (`getHostCPUName` and its features) or a named level (`--cpu x86-64-v3`) |
| Vector harness | [src/vec/](src/vec/) (`nxaot_vec`) | every vector on dynarmic's own A64 JIT (the reference) and as lifted code, from one initial state; compares x0..x30, SP, NZCV, FPCR, TPIDR_EL0, the exit (pc, reason, svc number) and the 64 KiB memory image byte for byte |
| Vectors | [tests/vectors/](tests/vectors/), written by [tests/gen_vectors.py](tests/gen_vectors.py) | 24,550 vectors, encodings written by hand from the Arm ARM, fields swept, operands from edge values and a fixed-seed stream |
| Census | [src/analysis/census.cpp](src/analysis/census.cpp) (`nxaot census`) | phase 0's numbers over a raw code blob as `key<TAB>count` lines |
| Runner | [tests/run_aot_tests.sh](tests/run_aot_tests.sh) | runs the tests and writes `logs/aot/` ([logs/README.md](../../../logs/README.md)) |
| CI | [.github/workflows/aot.yml](../../../.github/workflows/aot.yml) | the image, the build, CTest and the runner on every pull request and push to `main` and `multiplataforma` |

Vendored: [dm/third_party/dynarmic](../../third_party/dynarmic/VENDOR.md), the fork `azahar-emu/dynarmic` at `a46601580d55` (A64 frontend, IR, x86-64
backend and the externals its build needs; 19 MB).

## Building and testing in Docker

From the repository root. The image (about 1.9 GB) has Ubuntu 24.04 pinned by digest, LLVM 18.1.3 with its development packages, lld, CMake 3.28,
Ninja, Python 3.12 and the Boost headers:

```sh
docker build -t nxnative-aot -f docker/Dockerfile.aot docker

# configure, build, CTest (the vectors, the census, the vectors-current check)
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-aot sh -c \
  'cmake -S dm/tools/aot -B build/aot -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/aot && ctest --test-dir build/aot --output-on-failure'

# the same tests through the runner, which writes logs/aot/<test>.log and logs/aot/summary.txt
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$(pwd):/src" -w /src nxnative-aot sh dm/tools/aot/tests/run_aot_tests.sh build/aot
```

From Git Bash on Windows, the same with `MSYS_NO_PATHCONV=1`, `$(pwd -W)` and no `--user`, as [docker/README.md](../../../docker/README.md) says for
the host. The image is built locally and on CI from [docker/Dockerfile.aot](../../../docker/Dockerfile.aot). A local build has no registry digest, and
its local image ID changes with every rebuild (the build attaches a new attestation manifest), so pinning the image itself by digest needs a push to a
registry first (`docker push`, then the digest it prints), as the llvm-mingw image of the host build is pinned. Its base image is pinned by digest
already, and its packages are the Ubuntu 24.04 versions of the day it is built: `nxaot.lock` and the configure check keep LLVM at 18.1.3.

Linking: `libLLVM-18.so` from the image (the Ubuntu package is built with `LLVM_LINK_LLVM_DYLIB`); `-DNXAOT_LLVM_STATIC=ON` links the component
libraries statically instead, as the distributed `nxaot` will be (design section 13). The build tree takes about 35 MB.

## Running the harness and the census

```sh
build/aot/nxaot_vec dm/tools/aot/tests/vectors            # every file of the folder; or name .tsv files
build/aot/nxaot_vec --cpu x86-64-v3 dm/tools/aot/tests/vectors
build/aot/nxaot_vec --filter excl_seq --dump dm/tools/aot/tests/vectors/exclusive.tsv   # dynarmic IR and optimised LLVM IR of the matches
build/aot/nxaot census <raw ARM64 code file> --base <hex address of its first byte>
build/aot/nxaot opcodes                                    # every dynarmic IR opcode and whether the lifter lowers it
python3 dm/tools/aot/tests/gen_vectors.py                  # write the vectors again after changing the generator
```

`nxaot_vec` prints each mismatch with the vector's file and line, its words and every field that differs, then per class the vectors, mismatches,
refusals and normal exits, the IR opcodes each refused class needed, and last
`== nxaot_vec: N vectors, M mismatches, U unsupported (listed) ==`; it exits with 1 on a mismatch. A refused vector (an FP or SIMD opcode) is not a
failure today. The vectors' format and the memory layout (the image at `0x10000000`, code at `0x10008000`) are in [src/vec/vectors.h](src/vec/vectors.h);
the image is mapped at that address in the harness process with guard pages around it, since lifted code uses guest addresses as host addresses.

The census reads dynarmic's decoder (its matcher names), the Arm ARM's top-level encoding table, and Arm ARM masks for what dynarmic does not decode
(LSE atomics, `ldapr`, `msr` immediate, `hvc` / `smc`, `udf`). Exclusive windows and SP writes are followed on dynarmic's IR of each instruction: a
window is the straight line from an exclusive load to its store (conditional branches fall through, a second exclusive load or an unconditional
branch ends it unpaired), ABA-sensitive when a load inside reads through the loaded value (design 6.5); `sp.from_memory` counts SP set, within a basic
block, from a value a load produced. The `answer.*` keys answer phase 0's questions with 0 or 1.

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

`nxaot opcodes` lists the 725 opcodes of dynarmic's IR: 131 lowered (`Void`, `Identity`, and every opcode the A64 translator emits for the integer,
memory, branch, system-register and exclusive classes, with the CRC32 opcodes and `VectorGetElement64`, which `ldxp` of two X registers needs),
492 floating point / SIMD / crypto (phase 2 of the plan; with them the 128-bit loads and stores and the integer max / min only the SIMD translators
emit), and 102 that never appear in an A64 block: 99 of the AArch32 frontend (its own, and the packed, `ssat` / `usat`, GE, carry and overflow opcodes
only it emits) and 3 dynarmic-internal ones (`Breakpoint`, `CallHostFunction`, `GetNZFromOp`). The vectors exercise 129 of the 130 lowered opcodes
other than `Void` (`nxaot_vec` prints the list after each run); the one left is `Identity`, which the identity-removal pass leaves unused before the
lifter looks. Apart from those two, an opcode is in the lowered set only when a passing vector reaches it. CRC32C is `crc32` (SSE4.2);
CRC32 (IEEE) is a 256-entry table a byte at a time (a PCLMULQDQ form is an optimisation for later). `dmb` of every kind is `fence seq_cst`.

## Known issues

Places where dynarmic, the reference, departs from the Arm ARM or from what the harness can compare. Each is handled as said; none is patched in the
vendored copy.

1. **`msr nzcv` then `mrs nzcv` in one block.** dynarmic's `A64GetSetElimination` pass forwards the whole written register to the read
   (`ir/opt/a64_get_set_elimination_pass.cpp:67`), where NZCV bits 27:0 read as 0. Encoding: `d51b4200 d53b4201` (`msr nzcv, x0; mrs x1, nzcv`):
   with the pass, 56 of the 300 `msr_mrs` vectors differed (x1 = 0xfe947a5a from the reference, 0xf0000000 lifted, for x0 = 0xfe947a5a).
   The reference runs without that pass (`src/vec/reference.cpp`); the JIT fallback (design section 11) must too, or the pass be fixed upstream.
2. **`dc zva` at an unaligned address.** With `hook_data_cache_operations` off, dynarmic's `A64CallbackConfigPass` zeroes 64 bytes from the address
   itself (`ir/opt/a64_callback_config_pass.cpp:32`), not from the aligned block that holds it (read in the code, not run). Encoding: `d50b7420`
   with an unaligned `x0`. The reference takes the hook and zeroes the aligned block, as the Arm ARM's `dc zva` does.
3. **FPSR after a block lookup.** dynarmic's dispatcher compiles a block it misses with the guest's MXCSR loaded (`backend/x64/block_of_code.cpp:370`,
   `:449`), so its own floating point (hash-map load factors) can leave IXC in the guest's cumulative flags: the first vector, `94000001` (`bl +4`),
   ended with FPSR 0x10. The harness does not compare the final FPSR (an `mrs fpsr` result is compared in its register). The JIT fallback inherits this.
4. **FPCR bits.** `msr fpcr` keeps the bits dynarmic keeps (`FPCR_MASK = 0x07C89F00`, `backend/x64/a64_jitstate.cpp:51`), which include FZ16 and the
   trap enables a Cortex-A57 does not implement [A: the A57 reads them as 0]. The lifter uses the same mask (`kFpcrMask`), so both differ from the A57
   alike; FPSR keeps QC and the cumulative flags (0x0800009F) on both.
5. **Branch targets.** dynarmic's location descriptors keep 56-bit PCs, sign-extended from bit 55 (`frontend/A64/a64_location_descriptor.h:28`), so a
   `br` to a non-canonical address leaves the reference at another pc than the lifted code: `d61f0020` (`br x1`) with x1 = 0x123456789abcdef0 ends at
   0x003456789abcdef0 in the reference and at x1 in the lifted code. That is the Arm ARM's result when top-byte-ignore is on for EL0 (BranchAddr), not
   when it is off. The vectors branch to canonical addresses; what the recompiler does with tagged branch targets is an open question of the design.
6. **Hints.** dynarmic's A64 JIT passes only two translation options (`backend/x64/a64_interface.cpp:274`), so `yield`, `wfe`, `wfi`, `sev` and
   `sevl` always raise `ExceptionRaised`, whatever `hook_hint_instructions` says. The reference treats them as hints and goes on; the lifter emits
   `pause` for the first three.
7. **What dynarmic leaves to an interpreter.** `mrs xN, cntvct_el0` (dynarmic translates `cntpct_el0` only): the decoder glue translates it and the
   reference's interpreter callback does the same. LSE atomics, `ldapr`, `hvc` / `smc` and `msr` immediate end a block as untranslated on both sides.

## What is not done yet

The phases of design section 14, and where this folder stands:

| Phase | Content | Here |
|---|---|---|
| 0. Census | the numbers per module | `nxaot census` over raw code; NSO / NRO parsing, FDE coverage, setjmp imports, LSDA counts and the address-taken set are not done |
| 1. Skeleton | decoding, discovery, lifting of integer / memory / branch / call code, state block, dispatcher, SVC path, vector harness, calling-convention benchmarks | the state block, the lifter of those classes (one region of blocks per function), the harness (0 mismatches over 24,400 vectors). Not done: discovery, function summaries and the calling convention (2.3), calls and returns (3), the dispatcher (4), the host's SVC path (`svc` only leaves the function), safepoints, ThinLTO, the cache, the corpus NROs, the throughput measurements |
| 2. FP and SIMD | every FP, NEON and crypto lowering, NaN chains | not started: such blocks are refused |
| 3. Control flow and JIT | resume points, `pending`, safepoints, dynarmic integration | not started |
| 4. to 6. | games on the Intel Mac, performance, breadth | not started |
