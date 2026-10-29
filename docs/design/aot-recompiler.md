# Ahead-of-time ARM64 to x86-64 recompiler

Design for running the games' ARM64 code on x86-64 hosts (Windows 10 version 1803 and later, Linux, macOS on Intel) by recompiling it when the game is packed. Nothing here is implemented yet. Apple ARM64 hosts keep running the game's code natively after the [rewriter](../reference/rewriter.md); nothing in this document changes their outputs.

Marks used throughout:

- **[V]** verified: a line of this repository (`path:line`, at commit `8ed424a`), or an external source listed under [Sources](#sources).
- **[A]** an assumption or an estimate, with the check that settles it.

## Status and scope

- **Goal.** The highest run-time performance with bit-exact guest semantics (the results the Switch's Cortex-A57 gives), no stutter (nothing compiled while the game runs in the normal case), and a pack step whose cost is bounded and measured.
- **Already chosen by the user** (not decided again here): ahead-of-time recompilation written in C++ against the LLVM API; dynarmic as a JIT safety net only; native ARM64 execution on Apple unchanged; the Intel Mac (x86-64 macOS with the existing Metal backend) as the first target, so that recompiler bugs are separated from the Vulkan port.
- **Boundaries** (CLAUDE.md): this design adds a tool (`dm/tools/aot/`) and a run-time part of the host, changes the host modules that are ARM64- or Apple-specific (listed in sections 2, 5 and 14), and adds optional files to game profiles. It leaves the graphics backends (`dm/gfx/nvn`, `dm/gfx/vk`, `dm/gfx/gl`) and the shader chain alone. The one memory feature a backend needs on Windows and Linux, page aliasing for NVN's sparse pools (section 5), belongs to the host port, not to the recompiler.
- Today `scripts/build_ipa_from_nsp.py:2068` refuses `--platform mac` and `mac-app` on an Intel Mac; the x86 platforms are new.

## Decisions at a glance

| # | Decision | Section | Main evidence |
|---|---|---|---|
| 1 | Recompile the original NSO or NRO with the game-behaviour patches applied (cuts, function-matched cuts, observation gates, frame-step stubs, spin-wait regions), written by an AOT mode of `patch_nso.py`; the Apple-only rewrites are not used | 1 | `patch_nso.py:3-14`, `:133-136`, `:1093-1180` |
| 2 | The C++ tool decodes and analyses whole modules itself; the rewriter's x18-only analyses become test oracles | 1 | `patch_nso.py:382`, `:979`, `:995`; rewriter.md:232-233 |
| 3 | Two stacks: the guest stack in guest memory, a native stack for x86 `call`/`ret`; guest x30 always holds the original return address | 3 | `crash.c:80-94`, `hle_svc.c:283-295`, `patch_nso.py:297-304` |
| 4 | A return check at every guest `ret`; non-local transfers propagate through native frames; resume points only for LSDA landing pads and setjmp continuations | 3 | memory.md:262-267 |
| 5 | A state block per thread is the home of every guest register not passed in host registers; interprocedural summaries decide what is written back | 2 | `hle_thread.c:374-389`, threads.md:134-141 |
| 6 | One-to-one guest addresses (a guest address is the host address) in an arena below 2^39; Windows placeholders for sparse and aliased ranges | 5 | `loader.c:567-594`, `hle_svc.c:45`, `hle_nvn.c:2030-2034` |
| 7 | Every guest memory access is one LLVM `volatile` access (one x86 instruction); `stlr` → `xchg`, `dmb ish` → `mfence`, exclusive windows → `lock cmpxchg` or `atomicrmw` | 6 | LLVM Atomics guide; threads.md:99-105 |
| 8 | Floating point is specialised on FPCR (0 by default); exact per-operation lowering where x86 differs; NaN-chain checks at escape points with an exact recompute path; FMA3 for the fast build | 7 | `entry.S:70-82`, `hle_thread.c:345` |
| 9 | Instruction semantics: dynarmic's A64 decoder and translator retargeted to an LLVM IR emitter; results of real ARM hardware are the oracle | 9, 12 | dynarmic fork; the repository runs natively on Apple silicon |
| 10 | One native library per pack for all modules and NROs, with read-only, image-relative tables (function map, dispatch hash, resume points, line table) | 9 | services.md:663-669 |
| 11 | Two pack modes (balanced by default, max), sample-based PGO keyed by build ID, a per-function cache | 10, 13 | rewriter.md:207 |
| 12 | dynarmic (the maintained azahar-emu fork, 0BSD) only for discovery gaps, unrecorded non-local targets, foreign FPCR modes and diagnostics; misses are fed back into the next pack | 11 | `loader.c:502-506` |
| 13 | Safepoint polls at function entries and loop back-edges replace reading registers from a signal context when a thread is paused | 6 | `hle_thread.c:324-351` |

## 1. Inputs

### 1.1 What is recompiled

**Decision:** the recompiler's input is each module's *behaviour image*: the original NSO or NRO segments with every game-behaviour patch applied, plus a manifest. The rewriter's output (`<module>.patched.bin`) is not used.

Why not the rewriter's output [V]:

- It is built around Apple's kernel. The guest's x18 lives in a TSD slot reached through the key page and `tpidrro_el0` (`patch_nso.py:3-7`, `:137-153`, `:199-209`); SVCs and counter reads go through a gate found through the KGATE slot and identified by return address (`:332-348`, `loader.c:252-263`); `sub sp, sp, xN` is rounded for Apple's alignment check (`:222-230`). None of this exists on x86, and lifting it would compile Apple's workarounds rather than the game.
- Its stubs park x16 and x17 below SP (`site_br`, `:260-280`), a red zone Horizon's ABI does not have.
- Its patch area duplicates code (exclusive windows `:391-423`, retry blocks `:439-475`, spin-wait copies `:1419-1462`) at addresses that would appear as guest PCs and return addresses.

What carries over unchanged, because it is game behaviour and not platform [V]:

| Item | Today | In AOT mode |
|---|---|---|
| Registered and function-matched cuts (`CUTS`, `MATCH_CUTS`) | bytes replaced before the rewrite (`patch_nso.py:106-136`) | identical: applied to the behaviour image, then lifted like any code |
| SDK-wide function-matched cuts | `sdk_patches.py:231-251` | identical (see 5.5 and section 15 for the iPhone address-space cuts) |
| Observation gates (`OBS`, the region gates and StandardAllocator gates found by symbol) | a gate block in the patch area (`:1093-1145`) | a manifest row; before the instruction, the lifter calls the host observer with the whole register file in the state block, and reloads x0–x29 afterwards (a callback may write registers: `dm.h:100-101`, `hle_svc.c:343`) |
| Frame-step stubs (`FRAME_STEP`) | ARM64 words assembled into the patch area, branches written into text and listed as cuts (`:1147-1180`) | the same stubs in an auxiliary code region at the same offset, lifted as guest code; the state page stays at `FPS_STATE_OFF` (`loader.c:218-241`) |
| Spin-wait regions | found at `:1182-1418`, copied with a spin gate on their back-edges (`:1419-1462`) | the same analysis; the lifter puts the spin-gate call on the valid back-edges of the original code, without a copy. A guest poll takes a host core on x86 just as on Apple |
| Data blocks in text | `nso_code.data_blocks` (`nso_code.py:88-105`) | manifest rows; never decoded as code |

What is dropped, because only Apple needs it [V]: the x18 sites, the `tpidr_el0` and `tpidrro_el0` sites (fields of the state block on x86, section 2), the `sub sp, sp, xN` alignment, the KGATE gate path, the exclusive-window copies, the jump-table landing pads, the patch-area placement (`FAR`, `FRONT`, `BIG_TEXT`: `:37-61`) and the key page. `mrs xN, ctr_el0` is lifted as the constant the rewriter writes (`0x40000`, `:1070-1072`), so both platforms report the same cache geometry.

**Implementation:** `patch_nso.py --aot <NSO> <out dir> <name>` shares the code up to the cuts and the game-behaviour items, and writes `<name>.image.bin` and `<name>.aot.tsv` instead of `<name>.patched.bin`. Per-build knowledge stays in the patch tables it already loads (AGENTS.md:19). A test keeps the normal mode byte-identical, which is the rewriter's own rule ([Checking a rewriter change](../reference/rewriter.md#checking-a-rewriter-change)).

### 1.2 The manifest

TSV, like the rewriter's other tables (`patch_nso.py:1483-1504`), one row kind per line:

```
# nxaot-manifest 1	<module>	<build ID>	<SHA-256 of image.bin>
layout	text_end	<hex>	ro_off	<hex>	data_off	<hex>	bss	<hex>	image_size	<hex>
aux	<offset>	<length>	fps_state	<offset>        the frame-step stub region (absent without FRAME_STEP)
data	<start>	<end>                             a data block in text
obs	<offset>	<number>	<parameter>	<original instruction word>
spin	<head>	<back-edge>[,<back-edge>...]
cut	<offset>	<old hex>	<new hex>	<reason>        for the log only: already in the image
oracle-T	<br offset>	<target>[,<target>...]        the rewriter's offset tables (sites.tsv T rows)
oracle-J	<br offset>	<base>	<width>                 its byte and halfword tables (sites.tsv J rows)
oracle-W	<lo>	<hi>                                its exclusive windows (sites.tsv W rows)
```

The oracle rows come from the existing analysis (`patch_nso.py:1492-1498`); the C++ tool must reproduce them (section 12).

### 1.3 What the C++ tool analyses itself

The rewriter only analyses what touches x18: functions holding an x18 site (`patch_nso.py:979`), `br x18` jump tables (`:995-1020`) and windows containing x18 (`:382`). A recompiler needs every indirect branch and every window. The Python analysis also does not scale to whole modules: Monster Hunter Rise's `main` (125.3 MB of text, rewriter.md:207) takes 5.5 minutes to rewrite (`scripts/build_ipa_from_nsp.py:987`), and FINAL FANTASY RESONANCE's 82 MB without unwind entries needed analysis in pieces, 13 minutes at 2.7 GB (rewriter.md:232-233) [V]. So the C++ tool decodes and analyses whole modules natively:

- **Function discovery** from all of these: `.eh_frame_hdr` FDEs (as `nso_code.fde_ranges`, `nso_code.py:55-85`); exported `STT_FUNC` symbols with their sizes; `DT_INIT`, `DT_FINI` and `DT_INIT_ARRAY` (`loader.c:52-59`); relocation targets in text, RELA and RELR (`loader.c:268-338`): vtables and function-pointer tables; `adr` and `adrp` + `add` that build text addresses; `bl` and `b` targets (tail calls); LSDA landing pads (FDE augmentation `L`, read beside `R` in `nso_code.py:69-74`); jump-table targets; last, a sweep of the gaps that are left, which starts candidate functions after `ret`, `b` or `br` and skips data blocks. FDEs alone are not enough: FINAL FANTASY RESONANCE's `main` has 94 MB of text, 95 % of it without unwind entries (`dm/games/0100606026F12000/README.md:52`), and DRAGON QUEST XI S's first 70 MB have none (rewriter.md:50) [V].
- **Jump tables**, recovered on the lifted IR of each function by data flow rather than by shape: the `br` target must reduce to `base + extend(load(table + index × k))` (or an `adr` base plus a scaled entry), with the base a constant after constant propagation, the index bounded by a dominating unsigned compare or by its known bits (masks), the entries read from the image, and every target a decoded instruction of the function. This covers the special shapes the rewriter learnt one by one (copies, masks, far bases, complemented indices, forward bases: [Jump tables](../reference/rewriter.md#jump-tables)), because SSA removes the register copies they follow [A]. Checked against every `oracle-T` and `oracle-J` row: a disagreement fails the pack.
- **Exclusive windows**: every `ld(a)x(r|p)` … `st(l)x(r|p)` pair of a function, under the rewriter's rules (a window never spans a second exclusive load; retry blocks re-enter at the store: [Exclusive windows](../reference/rewriter.md#exclusive-windows)), classified as in 6.5.
- **Interprocedural summaries** for the calling convention (section 2): per function, the registers it may read before writing them, may write, and provably preserves; the call graph; the set of address-taken functions.
- **Non-local control flow**: LSDA call-site tables (`.gcc_except_table`), calls to `setjmp`, `_setjmp` and `sigsetjmp` by symbol, and writes of SP from memory (fiber switches, the unwinder's register restore) for the census (section 14).
- **Coverage**: every instruction of the text outside data blocks ends up in some compiled function (gap code in tier C, 9.3), so the whole module, NROs included, is compiled ahead of time; the JIT only stands in for entry points that discovery did not mark (section 11).

### 1.4 NROs

The NSP script collects NROs from the RomFS by SHA-256 and lists them in `nro.tsv` (services.md:656-661, `loader.c:376-401`); `ro::LoadModule` finds one by the image's hash (`hle_ro.c:52-77`) [V]. The recompiler keys each NRO's compiled code by the same SHA-256 (with the tool version, section 13), so packs of other versions of the game reuse it. All the NROs of a game go into the pack's one library (section 9): FINAL FANTASY X/X-2 registers 1,432 of them (services.md:663-669) [V], far too many for a library each.

## 2. Guest state and ABI

### 2.1 The state block

One per guest thread, allocated by the host when the thread is created (`hle_thread.c:287-303`), 64-byte aligned and outside the guest arena (5.2), so no guest pointer can point into it and the lifter may mark it `noalias`.

```c
typedef struct dm_guest_state {        // versioned with the chain (section 13); shared by the host runtime, the lifted code and the JIT glue
    uint64_t x[31];                    // x0..x30, in the order of the gate frame that svc_dispatch reads (hle_svc.c:399)
    uint64_t sp, pc;                   // pc is valid at escapes: SVCs, host calls, safepoints, fault reports
    uint32_t nzcv, fpcr, fpsr, pad;    // nzcv is materialised at escapes only
    _Alignas(16) uint8_t v[32][16];    // q0..q31
    uint64_t tpidr_el0, tpidrro_el0;   // replace the K18 / KTLS / KTLR thread slots (patch_nso.py:138, thread.c:12-27)
    uint64_t excl_addr, excl_val[2];   // monitor for exclusives that are not paired in one window (6.5)
    uint32_t excl_size, excl_valid;
    uint64_t pending;                  // target of a non-local transfer (3.3), 0 = none
    uint8_t  poll;                     // safepoint request (6.2)
    // host-only fields after this: thread object, native stack bounds, JIT instance, profiling counters
} dm_guest_state;
```

x18 is an ordinary register. The special cases for it in `GetThreadContext3` (`hle_thread.c:339-342`, `:382`) go away; `hz_ctx_t` (`hle_thread.c:374-389`) is filled from the block.

### 2.2 Where guest registers live

- Inside a recompiled function every guest register is an SSA value (lifted through allocas and `mem2reg`); no guest register is tied to a host register. Flags are lazy (9.2).
- Between functions, the **state block is the home** of every guest register except the ones passed in host registers for that call. The invariant at every call and return: each register that is not passed holds its current value in the block.

### 2.3 The calling convention between recompiled functions

The logical convention (decided):

- **Passed in host registers:** `st` (the state pointer), `sp`, `lr` (guest x30, the original return address, section 3) and x0–x3, plus x4–x7 and v0–v7 for callees whose summary says they read them. **Returned in host registers:** x0, x1 and v0–v3 (AAPCS64's integer pair and HFA returns).
- **Before a call** the caller stores to the block every register it changed since its entry or its previous call that the call does not pass. **After the call** it reloads, lazily at the next use, only what the callee may change: `WRITES(callee)` for a direct call; for an indirect call, the union of `WRITES` over the address-taken set.
- **At its return** a function stores the registers it changed that some caller may read afterwards: `LIVE_AFTER(f)`, the union over its call sites of the registers read before being written after the call. For an address-taken function this includes the union over every indirect call site of the program, so the rule is exact without assuming AAPCS64 at indirect calls.
- A callee is **preserving** when the analysis proves that every return path restores x19–x29, SP and the low halves of v8–v15 from the slots its prologue saved them to, with SP back at its entry value and no other store to those slots. Callers keep those registers in SSA across calls to it. The block invariant covers them too: a preserving function that stored its own x19 to the block before an inner call stores the restored value again before it returns, so a caller that kept x19 in SSA and a later callee that reads x19 from the block agree. Every other callee is **open**: callers reload whatever it may have changed. Code that does not follow AAPCS64 is thus handled by what it actually does: .NET NativeAOT's runtime helpers, IL2CPP thunks that move arguments through every scratch register ([Indirect tail calls through x18 in IL2CPP thunks](../reference/rewriter.md#indirect-tail-calls-through-x18-in-il2cpp-thunks)), libunwind's register restore.

Mapping onto LLVM (fixed by measurement in phase 1, section 14):

Chosen in phase 1: swifttailcc (`st` as swiftself), the default of `nxaot build`; measured against ghccc (LLVM 18.1 has no `preserve_nonecc`, ghccc is its stock predecessor with the same register property): 2.2 ns (5.4 TSC cycles) per call and return in both, ghccc 10 to 30 % more reloads on the corpus and one loop twice as slow [V: dm/tools/aot/README.md, "Phase 1: calls, the dispatcher, a jump table, throughput"].

- Guest tail calls (`b <function>`, `br xN` ending a function) must be real tail calls: guest tail-call chains can be unbounded (state machines). Candidates [V: LangRef]: `swifttailcc` (the callee pops the argument area, "so that mandatory tail calls are possible", with `st` as `swiftself`), and `preserve_none` with one uniform signature and `musttail` (all general registers carry arguments; none is callee-saved). The criterion is cycles per call and return and spills on call-heavy guest code from the corpus (section 12). If neither meets the target, a calling convention of its own is a patch to LLVM, which is the user's decision (section 15).
- Slow-path helpers (dispatch misses, NaN recomputation, safepoints, observers) use `preserve_most` or `preserve_all`, so their call sites force no spills [V: LangRef: on x86-64 the callee preserves every general register except R11 and the return registers; `preserve_all` also the XMM/YMM registers].
- Every address-taken function also gets a standard entry with one fixed signature, used by indirect calls and the JIT, which tail-calls its specialised body.

### 2.4 Calls to and from the host

- **SVCs.** `svc #n` writes back the whole register file and `pc`, calls `dm_aot_svc(st, n)` (C calling convention) and reloads x0–x8, the outputs (`entry.S:101-102`). The existing `svc_dispatch(int, uint64_t *regs, uint64_t orig)` (`hle_svc.c:399`) takes `st->x` as `regs` unchanged. A complete block at SVCs is required: `GetThreadContext3` reads the context of a thread paused in an SVC (`hle_thread.c:353-389`), the yield rule reads x19–x28 (threads.md:134-141), and `Break` walks x29 (`hle_svc.c:603-611`) [V].
- **Host functions the guest calls** (the `HLE` and `HLE_FORCE` entries, `hle.h:7-48`: the forced take-overs alone are more than 700, `loader.c:152`; NVN, Vulkan and GL entry points handed out by get-proc-address calls; the stubs of unimplemented imports, `entry.S:7-31`). On Apple a GOT slot simply holds the C function's address (`loader.c:144-148`, `:280-300`). On x86 each host function gets a guest-visible address in a host-function page inside the arena, which the dispatcher maps to a thunk. Thunks are generated at host build time from the macros' parameter lists: integer and pointer parameters from the next x register, `float` and `double` from the next v register (the host already avoids sub-32-bit and variadic parameters: `hle.h:2-3`); a parameter type the generator does not know stops the build. Host code that stores a host function's address in guest memory (the FMalloc vtable of `hle_uemalloc.c`, get-proc-address results) stores the guest-visible address (`dm_guest_fn(cname)`). Observers that look above the gate frame for the caller's x30 (`guest_caller`, `hle_svc.c:283-295`) read `st->x[30]` instead.
- The thunks run on the native stack, never on a guest stack, so `HLE_FORCE_BIG` and the big-stack entries (`hle.h:40-48`, `dm/gfx/nvn/nvn_entry.S:78-91`, memory.md:314-319) are not needed on x86 [V]. Host calls run with the guest thread's MXCSR, as they run with the guest's FPCR on Apple today.
- **Guest functions the host calls** are called as C function pointers today (`main.c:22-36`, `loader.c:635`, `hle_ro.c:75`, `:89`, `hle_guestthread.c:25-50`). On x86 they go through `dm_guest_call(st, addr, nargs, args…)`: x0–x7 from the arguments, x30 = a host-return sentinel in the host-function page, then the native entry or the JIT. The host's main thread runs guest code on its own stack today (`hle_svc.c:289`, `:531-536`); on x86 that stack is the native stack, so the main thread gets a guest stack of its own, which `QueryMemory` reports as it reports the host stack today.
- **TLS.** `mrs` / `msr tpidr_el0` and `mrs tpidrro_el0` read and write `st->tpidr_el0` and `st->tpidrro_el0`; `guest_thread_init` (`thread.c:12-27`) writes the initial values there.

### 2.5 The JIT boundary

The block holds the complete state at every transition between compiled code and the JIT, as at SVCs; section 11 describes the copy into dynarmic's register file.

## 3. Return addresses and unwinding

### 3.1 What the repository needs [V]

- Guest-visible return addresses must stay the original guest addresses. IL2CPP's C++ exceptions look up unwind tables by return address, which is why the rewriter keeps the original LR for `blr x18` (`patch_nso.py:297-304`); the SDK's unwinder finds the module through `svcQueryMemory` and MOD0 (memory.md:262-267, `hle_svc.c:511-523`); the host's backtraces (`crash.c:80-94`) and its attribution of address regions (`hle_svc.c:283-295`) walk x29 frame records.
- The x29 chain must hold the guest's own values (`patch_nso.py:332-336`).

### 3.2 Two stacks (decided)

- **The guest stack** is guest memory, and guest SP is a guest register (section 2). Frame records are written by the guest's own lifted stores, so they hold what they hold on hardware.
- **The native stack** is the host thread's stack: x86 return addresses, LLVM spills and saved host registers. Guest code never sees it.
- Why not one stack: an x86 `call` writes its return address at `[rsp-8]`. Whatever offset maps guest SP onto RSP, that slot lies in memory the callee's guest code owns as soon as it allocates its frame (`stp x29, x30, [sp, #-16]!`), so the guest would overwrite native return addresses. Emulating calls with jumps instead would give up the CPU's return-stack predictor and LLVM's native functions.

### 3.3 Calls and returns

- `bl target`: `lr = module_base + offset of the next instruction` (a load of the module's base, which LLVM hoists, plus a constant), then a native call. `blr xN`: the same, with the target from dispatch (section 4).
- `ret` (also `br x30` and `ret xN`): when the target equals the function's entry `lr`, a native `ret`; that is the normal case, and LLVM folds the compare away in leaf functions that never spill x30. Otherwise the function writes back the whole register file, sets `st->pending = target` and returns natively.
- After every call: one compare and branch on `st->pending` (macro-fused; the block's line is hot). When it fires:
  1. if `pending` is a resume point of this call site (a landing pad of the LSDA call-site entry covering it, or the continuation of a setjmp call) and `st->sp` equals this frame's SP, the frame clears `pending`, reloads every register from the block and branches there;
  2. otherwise it returns at once, writing nothing (propagation);
  3. the outermost native frame (a host-to-guest entry or the dispatcher) dispatches `pending` as a new call (section 4); a target in the middle of a function goes to the JIT (section 11).
- Cost on the normal path: one compare and branch per call site and per non-leaf return [A: measured in phase 1].

### 3.4 C++ exceptions, longjmp and fibers

- The guest's unwinder runs unchanged, as guest code. It walks guest frames with the guest's CFI and installs the landing pad through its register restore, which loads every register from a context and returns or branches to the pad. The pad is not the expected return target, so propagation pops native frames up to the frame that owns it. IL2CPP throws a C++ exception for every managed exception (memory.md:267), so every Unity game exercises this path; its cost is one compare and return per popped native frame, beside the guest unwinder's own work.
- A `longjmp` to a recorded setjmp continuation works the same way.
- A transfer to anything else (a fiber switch to another call site, a longjmp the analysis did not see) reaches the dispatcher, and the continuation runs in the JIT until it calls a compiled entry. Native fibers (one native stack per guest stack) come only if the census (section 14) finds an engine that switches stacks.
- Continuation entries are deliberately not made for every call site: each call would become a CFG merge point with every register loaded from memory.

### 3.5 Native unwinding, debugging and crash reports

- Native unwind information is emitted (`uwtable`: `.pdata` on Windows, `.eh_frame` for ELF and Mach-O), so debuggers and profilers walk recompiled frames. Guest semantics never use it.
- Crash reports (`crash.c`): host PC → module and guest offset through the line table (9.2); a guest backtrace from the native frames (each native return address maps to a guest call site, inlined frames from the inline table) and from the guest's x29 chain, starting at the last x29 written back.

## 4. Indirect branches

| Kind | Lowering | Cost |
|---|---|---|
| Jump table recovered in 1.3 | LLVM `switch`: a native jump table | as native code |
| PLT call (`adrp x16`; `ldr x17, [x16, …]`; `add x16`; `br x17`) | a call through a native GOT: one native pointer per guest GOT slot, filled by the loader whenever it binds the slot (`loader.c:318-338`, rebinds at `:409-424`); a compare of the guest slot with the value its native slot was filled for catches guest writes | one compare and an indirect call |
| `blr xN` (vtables, function pointers) | a per-site inline cache in data: a pointer to an immutable `{guest, native}` descriptor. Hit: compare and indirect call; miss: global lookup, then the descriptor pointer is replaced atomically | about two loads and a compare on a hit |
| Global lookup | an open-addressing hash table, guest address → descriptor, built at pack time as a read-only, image-relative section; an overflow table at run time for entries learnt then | about 5–20 ns when cold [A] |
| Unknown target | neither a function entry nor a resume point: the JIT (section 11), logged as `[RED] aot: …` and recorded per build for the next pack | rare by design |

- Size: Super Mario Bros. Wonder's `main` (28 MB) has about 138,000 functions (docs/agents/02-survey.md:69) [V]; Monster Hunter Rise's 125 MB `main` proportionally about 600,000 [A]. At 16 bytes a slot and half load, that is about 20 MB of demand-paged, read-only data.
- A `br` inside a function that the analysis cannot bound is not lowered as an `indirectbr` over every block (that would make every block a merge point); it leaves through the dispatcher, and the pack lists it.
- With a profile, a hot indirect site with a dominant target gets a guarded direct call (devirtualisation), which lets the max mode inline it (section 10).

## 5. Memory

### 5.1 One-to-one guest addresses (decided)

A guest address is the host address of the same byte. This is how the Apple host works today: modules are mapped at addresses the host chooses (`loader.c:567-594`), the guest block is one host reservation (`hle_svc.c:45`), and host code uses guest pointers as C pointers (for example `hle_svc.c:455`, `hle_ro.c:54-57`) [V]. Recompiled loads and stores use the guest address directly: no base register, no masking, no bounds check ("fastmem" with base 0). Keeping it also keeps unchanged the hundreds of host functions that take guest pointers.

### 5.2 An arena below 2^39 (decided)

- The Switch gives an application a 39-bit address space, and a guest program may rely on its pointers fitting in it (an engine can use the top bits as tags or counters) [A: whether any game does is unknown]. On Apple the host's mappings have stayed low enough for this never to come up; on Linux and Windows, `mmap` and the heaps hand out addresses near 2^47.
- So the host reserves an arena below 2^39 at startup and takes from it everything the guest can see: module images and NROs, the guest block (heap, alias and stack regions), NVN virtual pool ranges (`hle_nvn.c:1955-1964`), the host-function page, and host allocations handed to the guest. Some of those come from the host heap today: TLS blocks and thread-local regions (`thread.c:19-21`), the user exception context buffer (`hle_svc.c:643`), Unreal's heap on a host malloc zone (memory.md:242-243) [V]; they move to an arena allocator (`dm_guest_alloc`). State blocks and native stacks stay outside the arena.
- Linux: `mmap` with `MAP_FIXED_NOREPLACE` at chosen addresses; Windows: `VirtualAlloc2` with `MEM_ADDRESS_REQUIREMENTS` and placeholders (5.3); macOS on Intel: `mach_vm_allocate` at fixed addresses.
- Top-byte ignore: x86-64 has none, so a tagged pointer faults with #GP, which gives no fault address. The fault handler decodes the faulting x86 instruction (Zydis, already a dynarmic dependency) to report the address and the guest PC [A: the census cannot find tag use statically; phase 4 runs would show it].

### 5.3 Pages and the memory SVCs

- Horizon maps 4 KiB pages; the Apple host rounds to its 16 KiB pages (`hle_svc.c:450-453`, `:574-589`, memory.md:269-273) and keeps exposure bitmaps for the difference (`hle_svc.c:159-184`), written in terms of `getpagesize()` (`hle_svc.c:162`) [V]. x86 hosts have 4 KiB pages on all three systems, so the rounding becomes exact.
- `MapMemory` copies rather than aliasing (`hle_svc.c:442-460`, development.md:27-33) [V]: it needs no aliasing from the system.
- `MapPhysicalMemory` and `UnmapPhysicalMemory` (`hle_svc.c:548-602`) commit and decommit inside a reservation: `mprotect` and `madvise` on Linux and macOS, `VirtualAlloc(MEM_COMMIT)` and `VirtualFree(MEM_DECOMMIT)` on Windows. Windows' 64 KiB allocation granularity applies to reservation bases, not to commits inside a reservation.
- Aliasing (one page at two addresses) is needed for NVN's sparse virtual pools (`vm_remap` in `hle_nvn.c:2030-2034`) and for the write watch (`probe.c:288-291`). Linux: `memfd_create` and `mmap(MAP_SHARED | MAP_FIXED)`. Windows: the physical pool is a pagefile-backed section, the virtual range a placeholder (`VirtualAlloc2` with `MEM_RESERVE_PLACEHOLDER`, split with `VirtualFree` and `MEM_PRESERVE_PLACEHOLDER`), and views are mapped with `MapViewOfFile3` and `MEM_REPLACE_PLACEHOLDER`, for which "the 64k alignment requirements on Offset and BaseAddress do not apply" (Windows 10 version 1803 and later) [V]. This belongs to the host port, not to the recompiler.
- Guest text stays mapped readable (literal pools and tables in text) and is never executable on x86: a host jump into guest text faults at once.

### 5.4 Faults and GPU dirty tracking

- The GPU dirty tracker write-protects pages and handles the fault in a hook (`crash.c:95-115`, `dm.h:130-135`); `is_write` comes from ESR on Apple (`crash.c:136`) [V]. On x86 it comes from the page-fault error code: `uc_mcontext.gregs[REG_ERR] & 2` on Linux, `__es.__err & 2` on macOS, `ExceptionInformation[0]` in a vectored exception handler on Windows. The handler unprotects the page, marks it dirty and resumes; the faulting instruction runs again.
- Resuming is safe because every guest access is exactly one x86 memory instruction (6.4), so running it again repeats nothing else. The lifter's tests check that property on the generated code.
- `dm_guest_write_prepare` before the host writes guest memory (`crash.c:113-114`) is unchanged; 4 KiB pages invalidate less than Apple's 16 KiB.

### 5.5 The iPhone's address-space workarounds

The SDK-wide node-area and alignment cuts exist because "the Switch's alias region is 64 GB, where neither matters; the host's is about 4.4 GB" (`sdk_patches.py:208-218`), and the StandardAllocator gate is "a workaround for the iPhone's address space" (`hle_svc.c:322-329`) [V]. A 47-bit host can give the Switch's 64 GB alias region, which the Mac already uses for measurements (memory.md:166). Whether x86 packs drop these cuts is the user's decision (section 15): keeping them keeps the behaviour that was tested. The default proposed here keeps them and only enlarges the alias region.

## 6. Threads and synchronization

### 6.1 Threads

- Guest threads stay host threads, one to one (`hle_thread.c:2`). Each has a native stack (the system thread's stack, reserved generously, at least 8 MB [A: measure the ratio of native to guest stack depth]), its guest stack from the SDK as today, and its state block.
- `dm_thread_enter` (`entry.S:122-134`) becomes `dm_aot_enter(st, entry, arg, sp)`.

### 6.2 Pausing threads for garbage collectors: safepoints

- Today a thread running guest code is paused by SIGUSR2, and its ARM signal context is its guest context (`hle_thread.c:324-351`) [V]. On x86, at an arbitrary instruction the guest registers are spread over host registers and spills with no fixed mapping, so a signal handler cannot produce the guest context.
- **Decision:** the requester sets `st->poll` (and still signals, for code in the JIT); recompiled code polls at function entries and loop back-edges with one compare and branch; the cold path writes back every register, records `pc` and blocks under the same `G` and `pcv` logic as `on_pause_sig`. Threads in SVCs and in host code keep today's handling (`hle_thread.c:353-368`).
- Collector correctness: at a safepoint every guest register is in the block and every value the guest keeps in memory is in guest memory, as on hardware; native spills only hold copies. A conservative collector (IL2CPP's Boehm collector: [Pausing threads and reading their context for garbage collectors](../reference/threads.md#pausing-threads-and-reading-their-context-for-garbage-collectors)) therefore sees every root it would see on hardware.
- Diagnostics that sample other threads through Mach thread state (the hitch dump, `hle_thread.c:404-425`; the poll snapshot, `probe.c:305-329`) read the native PC instead (`x86_THREAD_STATE64` on macOS, `GetThreadContext` on Windows, a signal on Linux) and map it through the line table (9.2); they need no exact guest registers.

### 6.3 Memory ordering

x86-TSO only lets a load pass an earlier store to another address, which ARMv8 allows as well for plain accesses (and more besides) [A: the published models], so plain accesses need no fence. The ordered accesses:

| ARM | x86 | Note |
|---|---|---|
| `ldr`, `str`, `ldp`, `stp`, `ld1`, `st1` | `mov`, `movdqu` | |
| `ldar*` | `mov` (LLVM `load atomic acquire`) | acquire is free under TSO |
| `stlr*` | `xchg` (LLVM `store atomic seq_cst`) | ARM orders an `stlr` before a later `ldar` (RCsc); x86 needs the store-load barrier, which LLVM puts on the store side [V: Atomics guide] |
| `ld(a)xr` … `st(l)xr` window | `lock cmpxchg`; recognised read-modify-write idioms become `lock xadd` or `xchg` (LLVM `atomicrmw`) | a full barrier |
| `ldxp` … `stxp` | `lock cmpxchg16b` | CMPXCHG16B is part of x86-64-v2 |
| `dmb ish`, `dmb sy`, `dsb` | `mfence` (LLVM `fence seq_cst`) [V: Atomics guide] | |
| `dmb ishld`, `dmb ishst` | compiler barrier only | TSO keeps load→load, load→store and store→store [A] |
| `isb` | nothing | no self-modifying code; counter reads are host calls (section 8) |
| `wfe`, `wfi`, `yield` | `pause`; inside a detected spin region, the spin gate | |

FINAL FANTASY CRYSTAL CHRONICLES' Dekker handshake ([Unity's stream buffer handshake loses a wake-up on Apple cores](../reference/threads.md#unitys-stream-buffer-handshake-loses-a-wake-up-on-apple-cores)) is a game bug that ARM allows; x86-TSO allows it too (a store, then a load of another word), and its registered cut (plain `ldr` → `ldar`) fixes it on x86 as well, because `stlr` becomes `xchg` [V for the cut; A for the mapping].

### 6.4 Guest memory as LLVM sees it (decided)

- Every guest load and store is an LLVM `volatile` access with the alignment the lifter can prove (often 1). Unordered atomics would not do: "Unordered loads/stores can be moved out of a loop", and guest polling loops use plain `ldr` (threads.md:99-105; the spin-wait analysis at `patch_nso.py:1182-1199`). Monotonic atomics need a natural alignment the lifter cannot claim. Volatile accesses all happen, "in the stated order" [V: Atomics guide].
- LLVM does not promise that a non-atomic volatile access is atomic [V: Atomics guide]. The single-copy atomicity of aligned accesses up to 8 bytes, which ARM guarantees, therefore rests on the x86 backend emitting one instruction per access; the lifter's code-generation tests check it [A].
- The cost: LLVM cannot forward or merge guest memory accesses. The guest code was already optimised by its own compiler; the recompiler's gains are in registers, flags and calls. Plain accesses to frame slots proven private are a later, measured optimisation [A].
- Exclusive windows and `ldar` / `stlr` use real LLVM atomics with natural alignment: ARM faults on misaligned exclusive and ordered accesses, so the claim holds.

### 6.5 The exclusive monitor

- An `ldxr` remembers the value it loaded; the `stxr` is a compare-and-swap against it. dynarmic's interface has the same shape (`MemoryWriteExclusive*(vaddr, value, expected)`) [V], and its fastmem path for exclusives uses `lock cmpxchg` on the same memory, so compiled and JIT exclusives work together [A: check its emitter].
- The difference from hardware is ABA: when another thread changes the word and changes it back between the `ldxr` and the `stxr`, hardware fails the store and the compare-and-swap succeeds. A window whose stored value depends only on the loaded value and on registers the window does not change, with no other load inside, is equivalent: the success is a legal execution. A window that loads through the loaded value (a lock-free pop) is ABA-sensitive: the pack lists it (`[RED] aot: exclusive window main+0x… reads through the loaded value`) and the census counts such windows (section 14). yuzu ran Switch games on the same compare-and-swap semantics through dynarmic [A: evidence, not proof].
- A compare-and-swap never fails spuriously, where hardware may; failing less often is always legal.
- The Switch's CPU has no LSE atomics: the Tegra X1's Cortex-A57 reports `fp asimd aes pmull sha1 sha2 crc32` [V]. LSE encodings found by the census are lifted all the same (they are simple) and reported.

## 7. Floating point and SIMD

### 7.1 FPCR is a compile-time specialisation

- The host never sets FPCR; it only saves and restores it around gates and thread contexts (`entry.S:70-82`, `hle_thread.c:345`, `:384`) [V]. Guest threads therefore run with the FPCR Darwin gives a new thread unless the game writes it [A: 0; record it with a probe at `nnMain` on the Mac].
- **Decision:** each module is compiled for one FPCR value, 0 by default (round to nearest even, FZ = 0, DN = 0, AH = 0), which the host puts into MXCSR when a thread enters guest code: FPCR.RMode becomes MXCSR.RC (the four modes correspond), FZ = 1 becomes FTZ and DAZ together, and every exception stays masked. A profile key `aot_fpcr` selects another value for a game whose threads run with one. `msr fpcr, xN` calls a helper; when the new value differs from the specialisation, the continuation runs in the JIT, which keys its compiled blocks by FPCR, until FPCR comes back to it [A: dynarmic's coverage of FPCR modes].
- The Cortex-A57 has no half-precision arithmetic (its feature list in 6.5 has no `fphp` or `asimdhp` [V]), so FZ16 never matters; only conversions to and from half precision exist, lowered with F16C (x86-64-v3). FPCR.AHP is assumed 0, and the census checks it.

### 7.2 Where ARM and x86 differ, and the lowering

| # | ARM (the Arm ARM's pseudocode) | x86 SSE / AVX | Lowering |
|---|---|---|---|
| 1 | Default NaN `0x7FC00000` / `0x7FF8000000000000`, sign 0 [A] | the indefinite QNaN has its sign bit set [V] | NaN path (7.3) |
| 2 | Which NaN: the first signalling NaN in operand order, else the first quiet NaN [A] | the first source if it is a NaN, else the second [A: Intel SDM vol. 1, NaN rules] | NaN path |
| 3 | Fused multiply-add: the addend first in NaN order; a quiet-NaN addend with ∞ × 0 gives the default NaN [A] | its own operand order | NaN path |
| 4 | FMAX / FMIN: a NaN propagates; max(+0, −0) = +0, min(+0, −0) = −0 | MAXPS / MINPS return the second source when the values are equal and when either is a NaN [V] | per operation: AND / OR of the operands when equal, NaN blend |
| 5 | FMAXNM / FMINNM: a quiet NaN loses to a number, a signalling NaN propagates quieted | no equivalent | per operation |
| 6 | FMAXV / FMINV / FMAXNMV / FADDP: a fixed pairwise order | | an explicit tree in that order |
| 7 | FCVTZS / FCVTZU and the other rounding forms saturate; NaN → 0 | the integer indefinite `0x80…0` | `llvm.fptosi.sat` / `llvm.fptoui.sat` [A: their LangRef semantics match] or compare-and-fix; unsigned 64-bit by a sequence |
| 8 | FRINTA / FCVTAS: ties away from zero | ROUNDSS has no ties-away mode | an exact sequence |
| 9 | FRECPE / FRSQRTE / FRECPX / URECPE / URSQRTE: Arm's estimate algorithms | RCPPS / RSQRTPS give other values | Arm's algorithms, table-based |
| 10 | FRECPS / FRSQRTS are fused; 0 × ∞ gives 2.0 / 1.5 | | FMA3 and a blend |
| 11 | FMULX: 0 × ∞ gives ±2.0 | | a blend |
| 12 | FMADD / FMLA round once | FMA3 (x86-64-v3) | FMA3; the v2 build calls a correctly rounded software `fma` (slow) |
| 13 | With FZ = 1, flushing is decided before rounding [A] | FTZ decides after rounding [A] | only in FZ specialisations: results equal to ±(smallest normal) are checked again on a slow path |
| 14 | SDIV / UDIV by 0 give 0; INT_MIN / −1 gives INT_MIN | `div` / `idiv` raise #DE | explicit checks (LLVM's `sdiv` is undefined there) |
| 15 | Shifts by a register use the amount modulo the width | x86 masks the same way | the mask in IR (free after selection); LLVM's `shl` by the width or more is poison |
| 16 | CRC32 (IEEE polynomial) and CRC32C | only CRC32C (SSE4.2) | a PCLMULQDQ-based CRC32; `crc32` for CRC32C [A: bit order] |
| 17 | AESE / AESD / AESMC / AESIMC, PMULL, SHA1 / SHA256 | AES-NI (split differently), PCLMULQDQ, SHA-NI on some CPUs | compositions of AESENCLAST / AESDECLAST / AESENC; PCLMULQDQ; SHA in software or with SHA-NI. AES-NI and PCLMULQDQ are outside the x86-64 levels: a library that uses them records them as required features |
| 18 | Saturating operations set FPSR.QC | no flag | computed only where FPSR is observable (7.4) |
| 19 | Denormals at full speed on the A57 [A] | microcode assists on many Intel cores when FTZ / DAZ are off [A] | none (exactness first); measured (section 14) |
| 20 | Integer NEON (TBL / TBX, variable shifts, widening, narrowing, saturating) | | generic LLVM vector IR, exact by definition, which LLVM's x86 backend selects to SSE4.1 or AVX2 instructions (`pshufb`, `vpsllvd`, `pmovsx`, `packsswb`, …) for the target level |

Rules that hold for every lowering: no fast-math flags, no FP contraction (an `fmul` followed by an `fadd` stays two roundings), and LLVM intrinsics are used only where the LangRef fixes every bit of the result; `llvm.minimum`, `llvm.minnum` and the like leave NaN bits open and are not used without our own NaN handling [A].

### 7.3 The proven-safe fast path: NaN chains (decided)

- **P operations** are add, subtract, multiply, divide, square root, fused multiply-add with FMA3, negate, absolute value and conversions between precisions. Under the same rounding mode and with FZ = 0 they give bit-identical results on both ISAs whenever the result is not a NaN (IEEE 754 requires correctly rounded results), and a NaN operand always gives a NaN result [A: the instruction vectors confirm both].
- A connected group of P operations in a function is a *chain*. Its *escaping* values get one check each (`ucomiss` or `vcmpunordps`, a test and a branch): values that are stored, moved to integer registers or integer vector operations, passed or returned, live across a call or a safepoint, or used by an operation whose result carries a NaN's bits (FMAX, for example). Compares use chain values without a check: every NaN compares the same way.
- When a check fires, a cold path recomputes the chain from its inputs, which are still SSA values, with Arm's NaN rules implemented in integer operations, and continues with those bits.
- Why it is exact: an output that is not a NaN is bit-identical by the property above; a NaN anywhere in the chain reaches an escaping value or only compares. LLVM's default floating-point environment does not fix the payload or the quieting of NaNs [A: LangRef], so no NaN result of the fast path is ever used.
- The operations of rows 4 to 11 always get their own exact lowering; they end chains.

### 7.4 FPSR

The cumulative exception flags and QC are kept exactly only where FPSR is observable: when some loaded module contains `mrs xN, fpsr` (or a known `fetestexcept`), per the census, in the functions that can reach such a read. Otherwise they are not modelled; FPSR is still saved in thread contexts. The user confirms this policy (section 15).

## 8. Timing

- `mrs xN, cntpct_el0` and `cntvct_el0` are gates today (`patch_nso.py:536-537`, `:1067-1068`). They return the guest clock in 19.2 MHz ticks (`guest_ns() * 12 / 625`, as `svcGetSystemTick` does at `hle_svc.c:612`), wait while the app is in the background (services.md:608-613), and follow the deterministic clock under `DM_DETCLOCK` (`hle.h:24-27`) [V]. Recompiled: a direct `preserve_most` call to the same host function, about 20–40 ns through `clock_gettime` or `QueryPerformanceCounter` [A].
- `mrs xN, cntfrq_el0` becomes the constant 19,200,000. The rewriter does not rewrite it (`patch_nso.py:532-541` has no `cntfrq` kind), so on Apple a guest read returns the host's frequency while the ticks it divides are the guest's 19.2 MHz ticks [A: whether any module reads it; `insn_census.py:42` counts the reads].
- `isb` before a counter read: nothing.
- Frame pacing is host-side and unchanged; the FRAME_STEP stubs are lifted guest code that reads the same state page (`loader.c:218-241`). Precise sleeps on Windows need high-resolution waitable timers (why precision matters: threads.md:119-125); that is host-port work.

## 9. Code generation pipeline

```
NSO / NRO ─ patch_nso.py --aot ─▶ image.bin + aot.tsv          Python, the existing per-build tables
          ─ nxaot analyse ──────▶ an analysis database per module  C++: decode, discover, CFG, tables, summaries
          ─ nxaot lift ─────────▶ LLVM bitcode per shard       C++, in parallel
          ─ nxaot build ────────▶ objects (balanced) or ThinLTO (max), per tier
          ─ lld ────────────────▶ <pack>.aot.dll | .so | .dylib    one library per pack
the host: behaviour images in the arena, the library and its tables
```

### 9.1 Where the instruction semantics come from (decided)

- The decoder and the semantics of each instruction come from dynarmic's A64 frontend (its decoder tables and translator visitor, `src/dynarmic/frontend/A64` in the fork [V]), retargeted: the tool implements dynarmic's IR emitter interface as an LLVM IR builder, and maps dynarmic's IR operations (register reads and writes, flags, memory and exclusive accesses, FP and vector operations) onto the lowering of sections 6 and 7 [A: the fork translates single instructions, as its interpreter fallback needs].
- Reasons: the JIT fallback then shares the decoder and the semantics, so differential tests between the two isolate the backends; the frontend is the one yuzu ran Switch games with for years [A: a track record, not a proof]; its 0BSD licence fits the repository's MIT licence [V].
- Not chosen: semantics derived from Arm's ASL or Sail are authoritative, but they describe floating point as software algorithms, which makes them an oracle (section 12) rather than a lowering; hand-written semantics in the style of remill would be more work with no proven base.
- Encodings outside ARMv8.0 with CRC32 and the cryptographic extension are reported at pack time. The HINT-space instructions newer toolchains can emit (pointer authentication, BTI) are no-ops, as on the A57.

### 9.2 The shape of the IR

- One LLVM function per guest function, with the calling convention of 2.3, `nounwind`, `uwtable` and inline stack probes (`"probe-stack"="inline-asm"`, which Windows' guard-page stacks need).
- Guest registers: allocas promoted to SSA. Flags: the lifter keeps a descriptor of the last flag-setting operation (its kind, operands and result) and emits each condition as a direct compare of the operands (`subs` then `b.lt` becomes `icmp slt`; after `fcmp`, LT becomes `fcmp ult` and GE `fcmp oge`, matching Arm's NZCV for unordered operands). NZCV bits are materialised only for `mrs nzcv`, `adc` / `sbc` chains and escapes.
- Memory: `inttoptr` of the guest address, `volatile`, metadata that marks guest accesses; the state block through a `noalias` pointer.
- Calls as in sections 2 and 3; safepoint polls at entries and back-edges; host calls with a full write-back.
- System instructions: `dc zva` zeroes the aligned 64-byte block (`mrs dczid_el0` reads 4, as dynarmic's default [V]); the other data and instruction cache maintenance instructions do nothing (x86 caches are coherent, and the GPU sees guest memory through the dirty tracker, 5.4); `prfm` becomes a prefetch or nothing; `brk`, `udf` and words that do not decode call the host's crash path with the guest PC; HINT-space instructions are no-ops except `yield` and `wfe` (6.3).
- Debug information: the offset of each guest instruction is the line number in a synthetic file per module (`main.s`, `sdk.s`, `nro-<hash>.s`), and functions are named `main+0x1234`. The line tables then map native PCs to guest offsets for debuggers, profilers and crash reports; a compact copy goes into the library for `crash.c`.

### 9.3 Tiers and passes

| Tier | For | IR passes | Code generation |
|---|---|---|---|
| C (cold) | gap code, and functions the profile never sampled | SROA, instcombine, simplifycfg, dead-code elimination | FastISel, the fast register allocator |
| W (warm) | everything, when there is no profile | LLVM's O1 function pipeline | SelectionDAG, the greedy register allocator |
| H (hot) | functions the profile found hot | O2, inlining within the shard, the sample profile | O2 |
| H+ (max mode) | hot functions in the max mode | O3, ThinLTO import, devirtualisation, function ordering | O3 |

- Functions over a size limit (stretches without FDEs give functions of megabytes: rewriter.md:232-233) are split at safe points, as the rewriter splits its analysis (`patch_nso.py:943-978`), into LLVM functions joined by tail calls through the state block. LLVM's compile time grows faster than linearly with function size.
- Targets: the pack machine's own CPU (`llvm::sys::getHostCPUName()` and its features, the `-march=native` of the brief) by default, or x86-64-v3 or v2 for a portable library on request. The library records the features it needs; the host refuses a library the CPU cannot run and falls back to the portable one when it exists.

### 9.4 Output and loading

- Windows: COFF objects linked by `lld-link` into a DLL; Linux: an ELF shared object by `ld.lld`; macOS: a Mach-O dylib by `ld64.lld`. Code stays under 2 GB per library (the reach of RIP-relative addressing; a PE image is limited to 4 GB) [A: the largest pack is estimated at 0.5 to 1 GB of code].
- Read-only, image-relative tables, so loading applies no relocations to them: per module the function map (guest offset → native address), the dispatch hash, the resume points (pad address, call site, frame-SP rule), the native GOT slot map, the observation gate sites, the compact line and inline tables, and the identity of every input (module name, build ID, SHA-256 of `image.bin`, tool and chain version, FPCR, CPU features).
- The host (an x86 branch of `loader.c`) maps each `image.bin` into the arena (bss zero), relocates and binds as today (`loader.c:318-338`: relocations are data), loads the library once, checks every identity row against the loaded images (a mismatch stops with a red line), fills the native GOT, registers the tables with the dispatcher, and runs. It prefetches the part of the library the profile placed first (the hot code) with `madvise(MADV_WILLNEED)` or `PrefetchVirtualMemory`, so the first use of hot code does not wait for the disk. The iOS embedding (`loader.c:502-554`) and the patch area stay ARM-only.

### 9.5 Native replacements of hot SDK functions

The host already takes over SDK functions, but only service-layer ones; "the SDK's libc++ and nn::os internals do not go into this table" (`hle.h:32-34`) [V]. Replacing a guest function with native host code for speed is a new category, allowed only when it is bit-identical:

- **Candidates** are functions whose results are fully specified for every input: `memcpy`, `memmove`, `memset`, `memcmp`, `memchr`, `strlen`, `strcmp`, `strncmp`, found by symbol in the SDK's C library and chosen when the profile shows them hot. They hold for every game, as AGENTS.md:17 asks of an optimisation.
- **Conditions**: the same return registers and the same bytes written; no access outside the ranges the guest function itself would touch (a wider read could fault where the guest's does not); the registers it leaves changed are a subset of what callers may read (`LIVE_AFTER`, 2.3), or it writes them back as the guest code would.
- **Check**: a differential test against the recompiled guest function over random lengths, alignments and overlaps, in the census of each SDK build that uses the replacement.
- **Not candidates**: floating-point library functions (`sinf`, `expf`, …): the host's library rounds differently in the last bits; the guest's own implementation, recompiled, is the exact one.


## 10. Pack-time budget

Sizes found in the repository [V]:

| Module | Text | Source |
|---|---|---|
| Monster Hunter Rise `main` | 125.3 MB (about 31 million instructions) | rewriter.md:207 |
| FINAL FANTASY RESONANCE `main` | 94 MB, 95 % without FDEs | `dm/games/0100606026F12000/README.md:52` |
| Octopath Traveler 0 `main` | at least 71 MB without FDEs | rewriter.md:232 |
| DRAGON QUEST XI S `main` | at least 68 MB without FDEs | rewriter.md:232 |
| Animal Crossing: New Horizons `main` | 63 MB | rewriter.md:184 |
| Super Mario Bros. Wonder `main` | 28 MB, about 138,000 functions | docs/agents/02-survey.md:69 |
| Pokémon Sword `main` | 25 MB | `dm/games/0100ABF008968000/README.md:10` |
| Super Smash Bros. Ultimate NROs | 98 NROs, 157 MB of rewritten code | `dm/games/01006A800016E000/README.md:28` |
| FINAL FANTASY X/X-2 HD Remaster | 1,432 NROs | services.md:668 |

Today's Python rewrite takes 5.5 minutes for Monster Hunter Rise's `main` (`scripts/build_ipa_from_nsp.py:987`) and 13 minutes for FINAL FANTASY RESONANCE's (rewriter.md:233) [V].

Model: `T = T_analyse + Σ T_compile(function, tier) / workers + T_link`. The throughputs below are estimates that phase 1 replaces with measurements [A]:

| Step | Guest instructions per second per core |
|---|---|
| decoding, discovery, summaries | 38,000 (estimated at least 10 million) [V: measured on lavapipe-less Linux, Intel Core i9-13950HX] |
| lifting to IR | 150,000 (estimated 2 to 5 million) [V: measured on lavapipe-less Linux, Intel Core i9-13950HX] |
| tier C | 23,600 (estimated 150,000 to 300,000) [V: measured on lavapipe-less Linux, Intel Core i9-13950HX] |
| tier W | 6,700 (estimated 50,000 to 100,000) [V: measured on lavapipe-less Linux, Intel Core i9-13950HX] |
| tier H | 6,500 at O2 (estimated 15,000 to 30,000; H+ at O3: 6,200) [V: measured on lavapipe-less Linux, Intel Core i9-13950HX] |
| linking | 0.005 s per module of the corpus with lld as a library (estimated 10 to 30 s per pack) [V: measured on lavapipe-less Linux, Intel Core i9-13950HX] |

Measured in phase 1 over the corpus (14 NROs, 86,141 guest instructions, one thread, one tier per module; dm/tools/aot/README.md, "Measurements").

Estimates on the reference machine: 4 cores and 8 threads (Core i3-10100 class, x86-64-v3), 8 GB of RAM, a SATA SSD, about 4 compile workers of about 1 GB each [A]:

| Pack | Balanced, no profile (tier W) | Balanced with a profile (10 % H, the rest C) | Max (H / H+) | Repack with a warm cache |
|---|---|---|---|---|
| Mario `main` and `sdk` (about 8.5 million instructions) | about 1 min | about 1 min | about 2–3 min | under 30 s |
| Monster Hunter Rise `main` (about 31 million) | about 3 min | about 2–3 min | about 7–9 min | under 1 min |

Strategy: shards compiled in parallel (one LLVM context per worker thread), the number of workers bounded by free RAM; the per-function cache (section 13); tier H only with a profile; FDE-less gaps in tier C; huge functions split.

Measurable targets, checked in phase 5: on the reference machine, a balanced pack of at most 3 minutes for Mario and 10 minutes for Monster Hunter Rise's `main`; at most 30 minutes for any max pack; at most 1 minute for a repack with a warm cache; at most 2 seconds from host start to `nnMain`; and balanced packs reaching at least 90 % of the max pack's frame rate on the bench routes. Each pack logs `aot: <module> <instructions> <functions>`, the tier counts, the seconds per stage and the peak memory.

## 11. JIT fallback

When it runs:

1. an indirect transfer to an address with no compiled entry and no resume point (a discovery gap);
2. a non-local transfer to a continuation that has no native frame (fibers, a longjmp the analysis did not record);
3. FPCR changed away from the module's specialisation;
4. code the lifter rejected;
5. diagnostics: the probes write `brk` into code at run time (`probe.c:159-177`), which compiled code cannot take, so on x86 a probed function runs in the JIT with a hook [A].

Why it should be rare: every game in the games table runs on iOS, where code produced at run time cannot execute (`loader.c:502-506`; `dm/gfx/tools/gen_protos.py:47`, `dm/gfx/tools/dmsh_build.py:3`), so all the code they execute is in modules and NROs known when they are packed [V]. Switch games do not generate code: Super Mario Galaxy's recompiled PowerPC runs with `Jit false` (`dm/games/010099C022B96000/README.md:3`), Balatro's LuaJIT runs in its interpreter (`dm/games/0100CD801CE5E000/game.tsv:3`), and the host implements no code-memory SVC (unknown SVCs take the placeholder path, `hle_svc.c:648-650`) [V]. JIT use therefore measures how good discovery is. Each miss is logged with its guest address and appended to `<NSP folder>/<id>-work/aot_misses.tsv`, which the next pack compiles as entries; the target is zero on every smoke route.

Integration:

- dynarmic: the original repository's URL now returns 404; the maintained fork, `azahar-emu/dynarmic`, is 0BSD, was active in September 2026 and still has the A64 frontend [V]. It needs CMake, C++20 and Boost 1.57 or later, and bundles xbyak, Zydis, fmt, mcl and robin-map [V]. It is vendored under `dm/third_party/dynarmic` and built as a static library behind a C interface:

  ```c
  typedef struct dm_jit dm_jit;
  dm_jit  *dm_jit_create(dm_guest_state *st);
  uint64_t dm_jit_run(dm_jit *j, uint64_t pc);   // runs until a compiled entry is reached (returns its address) or it is halted
  void     dm_jit_halt(dm_jit *j);               // pause requests (6.2)
  ```

- Configuration [V for the fields]: `fastmem_pointer = 0` with the arena's address bits (one-to-one addresses), `fastmem_exclusive_access = true`, one `global_monitor` for every thread, `tpidr_el0` and `tpidrro_el0` pointing into the state block, `cntfrq_el0 = 19200000`, `ctr_el0 = 0x40000` (the rewriter's value), `dczid_el0 = 4`, `wall_clock_cntpct` with `GetCNTPCT` reading the host clock, `enable_cycle_counting = false`, and `CallSVC` calling `svc_dispatch` on the block. Its own handling of fastmem faults must run after the host's dirty-tracking hook [A].
- Leaving the JIT for compiled code needs a small patch: a hook in its block lookup that stops `Run()` when the next block's address is a compiled entry [A].
- Handing over state copies 31 general registers, SP, PC, 32 vector registers, NZCV, FPCR and FPSR between the block and the JIT, about 100 ns [A].
- One JIT instance per thread, made at its first use, with a small code cache (16 MB; dynarmic's default is 128 MB [V]).

## 12. Testing and verification

1. **Instruction vectors from ARM hardware.** The project's Apple-silicon Macs already run ARM64 code natively. A harness runs every ARMv8.0 encoding (with CRC32 and the cryptographic extension) over inputs that include special values (±0, ±∞, signalling and quiet NaNs with payloads, denormals, extreme normals, integer extremes) and records the general registers, the vector registers, NZCV and FPSR, with FPCR variants for the JIT checks. For these instructions the results are architecturally defined [A]; the implementation-defined registers (CTR, DCZID) are constants on both sides. The vectors are synthetic and live in `dm/tools/aot/tests/`. On x86 (Linux, Windows and macOS CI) every instruction is lifted alone and run, and dynarmic runs the same vectors. Unicorn, which the repository's tests already use (docs/agents/05-debugging.md:120, `dm/tools/tests/test_astral_fps.py:3`) [V], fuzzes further on x86 CI.
2. **Differential tests per function.** Each function of a module, or a sample, runs under the recompiled code and under dynarmic from the same snapshot (random and recorded register states, copy-on-write memory); the final registers and the pages written must match, and host calls are replayed from a recording. Game modules are tested locally only, and game files stay in the NSP folder (AGENTS.md:22).
3. **A corpus with no game files and no keys.** Self-checking programs built as NROs, both with devkitA64 (GCC, from devkitPro) and with upstream clang (`--target=aarch64-none-elf -mcpu=cortex-a57`, closer to Nintendo's clang-based toolchain [A]): compiler torture tests, libm and SIMD test suites, C++ exception and setjmp tests, threading and exclusive-window stress tests. They use a minimal crt0 and report through `svcOutputDebugString` (`hle_svc.c:613-616`), since the host answers SDK functions, not libnx's IPC services (docs/agents/02-survey.md:41) [V]. Native on Apple silicon and recompiled on x86, they must print the same output.
4. **Games on the Intel Mac**, with the Metal backend unchanged: the smoke routes of `scripts/regress.py` (`regress.py:16-30`), with the native Apple-silicon run as the baseline: the frame to reach, crashes, hangs, new red lines, JIT use. CPU exactness under `DM_DETCLOCK` and `DM_FIXED_TIME` (`hle.h:24-27`, `main.c:66-69`): hashes of what the CPU produces each frame (the NVN command memory, the audio renderer's output, saves) must equal the native run's. Screenshots differ between Apple and Intel or AMD GPUs and only inform [A: frame-by-frame determinism holds on routes dominated by one thread].
5. **Checks at pack time**: the recovered tables equal the rewriter's oracle rows; the census report; the library's identity rows.
6. **Performance**: the routes of `scripts/bench.py` on the reference machine, frame times at p50 and p99, and no frame over budget because of dispatch or the JIT.

Recording PGO profiles:

- By sampling, not instrumentation: a sampler in the host (a timer signal, about 1 kHz per thread) records the guest PC of the interrupted thread: on Apple silicon the native signal context's PC (module and offset), on x86 the native PC mapped through the line table. The x86 dispatcher also counts inline-cache misses per call site, with their targets.
- One file per game build: `dm/games/<id>/aot_profile/<build ID>.tsv` (function offset, samples, sampled instruction offsets, hot indirect targets). Guest offsets make it independent of the host ISA, so it can be recorded on Apple silicon with today's host; it holds offsets and counts, no game code. At pack time it becomes an LLVM sample profile (function `main+0x…`, line = guest offset) [A: the format's details].
- Routes: the profile's scripted `bench.tsv` routes.

## 13. Repository layout and build

```
dm/tools/aot/                     nxaot (C++20, CMake)
  CMakeLists.txt  nxaot.lock      the pinned LLVM version and the hashes of the prebuilt binaries
  src/decode/                     glue to dynarmic's frontend
  src/analysis/                   discovery, CFG, jump tables, windows, LSDA, summaries, census
  src/lift/                       the IR emitter: registers, flags, memory, FP, SIMD, calls, host calls
  src/build/                      tiers, ThinLTO, linking, tables
  src/cache/                      the per-function cache
  tests/                          instruction vectors, lifter tests, corpus sources
dm/host/aot_*.c                   run time: state blocks, dispatcher, safepoints, host-function page, generated thunks (C; the JIT glue in C++)
dm/host/gen/hle_thunks.c          generated from the HLE macros when the host is built
dm/third_party/dynarmic/          the vendored fork, with the exit-hook patch
dm/games/<id>/aot_profile/<build ID>.tsv   the sample profile of a build (optional)
dm/games/<id>/game.tsv            optional keys: aot_fpcr (default 0), aot_mode (balanced | max)
```

- **LLVM**: one pinned release, linked statically together with lld as a library (ELF, COFF and Mach-O), into one `nxaot` executable per host system [A: about 60–100 MB]. A chain version, `AOT_CHAIN` (as `DMSH_CHAIN` is for shaders, AGENTS.md:24), covers `nxaot`, the lifter's semantics and the layout of the state block; the host is built with it and loads only libraries of that version.
- **Distribution**: CI builds `nxaot` for Windows, Linux and macOS on x86-64 (and Apple silicon, to pack on a Mac) from a Docker image or pinned toolchains; `build_ipa_from_nsp.py` uses the binary whose hash `nxaot.lock` names, as the shader chain uses `dmsh_xlate_bin` against `dmsh_chain.lock` (AGENTS.md:29). Building from source stays possible for developers (CMake, Ninja, LLVM's CMake package).
- **`build_ipa_from_nsp.py`**: new platforms (`mac-x86` first; `linux` and `windows` with the Vulkan port), and after the rewrite stage (`rewrite()`, `:983-1028`) an `aot` stage with its own reuse key: the images' hashes, `nxaot.lock`, the target CPU or portable level, the mode, the profile file and the FPCR key.
- **The cache** lives in the work folder (`<NSP folder>/<id>-work/aot-cache/`, AGENTS.md:22), shared by every pack of that NSP folder, and is keyed per function by the SHA-256 of: the tool's chain version, the LLVM version, the CPU features, the tier and its options, FPCR, the function's instruction words with PC-relative fields replaced by reference numbers (as `nso_match.py` normalises relocations), the convention facts its code depends on (its callees' classes, its own write-back class) and its slice of the profile. Cached objects reach other code and data through symbols defined at the final link, so a function whose code did not change but whose addresses moved in a game update is reused (phase 5).
- **CMake** is new for first-party code; the Apple host keeps its Makefile (`dm/host/Makefile:19`, `:53-54`).

## 14. Plan and risks

### Phases

Each phase ends with its acceptance check.

| Phase | Content | Acceptance check |
|---|---|---|
| 0. Census | The C++ decoder (or `insn_census.py`) over every module on hand: the FP / SIMD mix, cryptographic and CRC instructions, LSE and later encodings, `msr` / `mrs fpcr` / `fpsr`, `cntfrq`, `dc zva`, `wfe`, exclusive windows (equivalent or ABA-sensitive), `br` recovery, FDE coverage, SP written from memory, setjmp imports, LSDA counts, the address-taken set | a report of numbers only per game, in its work folder, answering: LSE present? FPSR read? fibers? ABA-sensitive windows? `cntfrq` read? |
| 1. Skeleton | decoding, discovery and lifting of integer, memory, branch and call instructions; state block, dispatcher and the SVC path the corpus needs; the vector harness; benchmarks of the calling-convention candidates (2.3); throughput measurements | the corpus NROs print the same output natively and recompiled (Intel Mac and Linux); 100 % of the integer vectors pass; measured throughputs replace the [A] figures of section 10 |
| 2. FP and SIMD | every FP, NEON and cryptographic lowering; NaN chains | no mismatch over at least 10^8 vectors per instruction class; the cost of chain checks measured |
| 3. Control flow and JIT | resume points, `pending` propagation, safepoints, the dynarmic integration | the exception, longjmp and thread corpus passes; a run with everything in the JIT and a run with everything compiled print the same output; host pause tests in the manner of `t_preempt` pass with safepoints |
| 4. First games on the Intel Mac | x86 builds of the host parts they need; one small game first, then Super Mario Bros. Wonder | the smoke route reaches its frame with no new red line; the CPU stream hashes equal the native run's for N frames; no JIT entry after the feedback pack |
| 5. Performance | profiles, tiers, ThinLTO, inline caches, devirtualisation, function ordering, a cache that survives updates | the targets of section 10 on the reference machine; frame-time p99 within budget on the five games `regress.py impact` picks |
| 6. Breadth | Unity IL2CPP, Unreal Engine 4 and 5 (FDE-less), NRO-heavy games (Super Smash Bros. Ultimate, FINAL FANTASY X/X-2), .NET NativeAOT (Absolum), NERD's Hagi; native fibers if phase 0 found any; Linux and Windows with the Vulkan port | each game's smoke route as in phase 4 |

The Metal backend may use features that Intel Macs lack (MetalFX is linked, `dm/host/Makefile:54`; some profiles turn on Metal 4 pipeline archives) [A]; phase 4 starts with games whose profiles need neither, or with fallbacks in the backend.

### Risks, most serious first, and how each is retired

1. **Pack time on the largest modules.** Retired by the phase 1 throughputs and the phase 5 targets; mitigated by tiers, splitting and the cache.
2. **ABI exactness with code that does not follow AAPCS64.** The summaries and the open convention (2.3); retired by differential tests on Unity, NativeAOT and Unreal modules.
3. **FP exactness and its cost.** Retired by the phase 2 vectors and the per-chain overhead benchmarks.
4. **Discovery gaps in FDE-less code.** The census and the JIT feedback; retired when no JIT entry remains on Unreal routes.
5. **Non-local control flow** (fibers, unwinders of their own). The census; native fibers only if needed.
6. **The cost of the memory model** (`volatile` accesses, `stlr` as `xchg`) and ABA-sensitive windows. Benchmarks and the census.
7. **Pointer-width assumptions.** The arena below 2^39 keeps the Switch's guarantee; the fault handler reports tagged pointers.
8. **Denormal slowdowns on Intel**, where the SDK's audio renderer runs in-process (development.md:12). Measured with the FP-assist counters; there is no inexact mitigation.
9. **dynarmic's maintenance and Boost.** A vendored fork with minimal patches.
10. **LLVM churn and the tool's size.** A pinned version and prebuilt binaries.
11. **The scope of the host port.** ARM64 assembly (`entry.S`, `dm/gfx/nvn/nvn_entry.S`, `dm/gfx/nvn/nvn_thunks.S`, the inline stub of `hle.h:45-46`), Mach calls (`thread_get_state`, `vm_read_overwrite`, `vm_remap`), ARM signal contexts (`crash.c:136-156`, `hle_thread.c:334-345`, `probe.c`). An inventory first, then separate tasks.

## 15. Decisions that need the user

Each is a trade-off this document does not settle; the criteria say what would decide it.

1. **Boost**, which dynarmic requires: a vendored header-only subset, a patch that removes it from the fork, or a system Boost. Criteria: build complexity for contributors, repository size, divergence from the fork.
2. **How players get `nxaot`**: prebuilt binaries from CI (pinned by hash), or building LLVM themselves. Criteria: the wait before a first pack (building LLVM takes on the order of an hour [A]), download size (about 60–100 MB per system [A]), reproducibility, CI cost.
3. **CMake** as a new build system for first-party code. Criteria: LLVM's CMake package makes it the natural choice; the alternative is a Makefile around `llvm-config`.
4. **The iPhone's address-space cuts on x86**: keep them (the tested behaviour) or drop them for x86 packs (closer to hardware, but it needs a platform tag on cuts and new tests). Criteria: regression results on the games concerned ([memory.md](../reference/memory.md) lists them).
5. **FPSR flags**: modelled only where the census finds reads (proposed), or always. Criteria: the census; the cost of always modelling them.
6. **ABA-sensitive exclusive windows**: accept compare-and-swap semantics, or emulate them exactly for the listed windows (a global lock and instrumented stores to their addresses, much slower). Criteria: how many the census finds; gameplay tests.
7. **The pack-time budget and the default mode**: the acceptable wait for a first pack on the reference machine, and whether balanced stays the default.
8. **The CPU baseline**: require x86-64-v3 (FMA3, AVX2, F16C: Haswell, Zen and later), or also support v2 with a slow software FMA. Criteria: the share of target PCs without AVX2 and FMA; the cost of a second build.
9. **PGO profiles in the repository** (offsets and counts per build), or only in each player's work folder. Criteria: legal comfort; usefulness to every player.
10. **Patches to LLVM**, if no stock calling convention meets the phase 1 targets (2.3). Criteria: the measured gain against maintaining an LLVM fork.
11. **The JIT in release builds** on Windows and Linux (writable and executable memory, antivirus heuristics), or misses made fatal in release builds with feedback only. Criteria: the player's experience on a miss against the security posture.

## Sources

- LLVM Language Reference, calling conventions (`preserve_nonecc`, `preserve_mostcc`, `preserve_allcc`, `tailcc`, `swifttailcc`): <https://llvm.org/docs/LangRef.html#calling-conventions>
- The commit that added `preserve_none`: <https://llvm.googlesource.com/llvm-project/+/c166a43c6e6157b1309ea757324cc0a71c078e66>
- LLVM Atomic Instructions and Concurrency Guide (unordered, monotonic and volatile; x86 lowering of `seq_cst`): <https://llvm.org/docs/Atomics.html>
- `MapViewOfFile3` (placeholder replacement, alignment, Windows 10 version 1803): <https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile3>
- `VirtualAlloc2`: <https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc2>
- Raymond Chen, using placeholders to allocate contiguous address space: <https://devblogs.microsoft.com/oldnewthing/20240201-00/?p=109346>
- dynarmic, the maintained fork (licence 0BSD, last push 2026-09-26, a fork of NearlyTRex/CitraDynarmic, A32 and A64 frontends): <https://github.com/azahar-emu/dynarmic>; its A64 configuration (`UserCallbacks`, fastmem and exclusive fields, `ctr_el0`, `dczid_el0`, `code_cache_size`): <https://github.com/azahar-emu/dynarmic/blob/master/src/dynarmic/interface/A64/config.h>; its build (Boost 1.57 or later, C++20, `DYNARMIC_TESTS_USE_UNICORN`): <https://github.com/azahar-emu/dynarmic/blob/master/CMakeLists.txt>. The original repository, <https://github.com/merryhime/dynarmic>, returned 404 on 2026-10-28.
- Intel floating-point reference sheet (the real indefinite: a quiet NaN with the sign bit set): <https://kib.kiev.ua/x86docs/Intel/misc/FP_Ref_Sheet/floating-point-reference-sheet-v2-13.pdf>
- MINSD / MINPS (the second source for equal values and for NaNs): <https://www.felixcloutier.com/x86/minsd.html>
- Tegra X1 and the Cortex-A57's features (`fp asimd aes pmull sha1 sha2 crc32`): <https://forums.developer.nvidia.com/t/does-anyone-get-8-cpus-listed/40720>, <https://switchbrew.org/wiki/Tegra_X1>, <https://old.chipsandcheese.com/2023/12/12/cortex-a57-nintendo-switchs-cpu/>
