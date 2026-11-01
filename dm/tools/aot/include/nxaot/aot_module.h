// What a pack's library holds for one module, and what it asks of the host (docs/design/aot-recompiler.md sections 1.1, 2.3, 3.5, 4, 9.4): `nxaot
// build` (src/build) writes it, the host run time (src/rt, dm/host/aot_*.c) reads it. Plain C, like the state block it is shared with.
//
// The library exports one descriptor per module, `nxaot_module` (struct nxaot_module_desc). Its tables are read-only and image-relative: guest
// addresses are module offsets, native addresses are 32-bit offsets from the field that holds them (the line table's: from a function's body), so
// loading applies no relocation to them:
//   lookup    the dispatcher's open-addressing hash, module offset -> the standard entry of a function (section 4: every address-taken function)
//   fns       every compiled function, by module offset: its body and size, for crash reports and profilers
//   fn_lines  parallel to fns: each function's run of the line table
//   lines     the line table (section 3.5): for every guest memory access, the native address as bytes from the start of the body of the function
//             that holds it, with the module offset of its guest instruction (what a fault report needs, 5.4); each function's run sorted by native
//             address. nxaot writes it from each compile unit's code after code generation (src/build/pack.cpp, "The line table": LLVM's
//             !pcsections marks the accesses in a section of the unit's object, which nxaot reads back with its relocations and keeps out of the
//             link), so its format is nxaot's own. Every other native address maps through the DWARF line table of the library (line = guest
//             offset, section 9.2) and the function map
//   returns   the module offset after every call site (bl, blr) of compiled code, sorted: the places where a native frame may wait for its
//             callee to come back, which the JIT fallback stops at (section 11; src/rt/jit.cpp)
//   gates     the observation gates and spin-wait back-edges the code calls the host at (design 1.1: the manifest's obs and spin rows), sorted by
//             module offset: the host checks them against the manifest's rows (dm/host/loader.c) and maps a call's site to its gate
// The host writes the module's base address to *base before guest code runs (section 3.3: return addresses are base + offset) and calls guest
// functions through `enter`, a C-convention trampoline that passes st's registers to a standard entry. Standard entries (section 2.3) have one
// signature in every pack of a convention and mode: st, sp, lr and x0..x3 and the convention's passable registers (swifttailcc: x4..x7 and v0..v7;
// ghccc: x4..x6 and v0..v5) in host registers; x0 and x1 come back in host registers and v0..v3 through the block (rv 0). std_x and std_v record
// the registers, so the dispatcher binds an inline cache of one pack to a standard entry of another directly when both record the same signature,
// convention and mode (a cross-pack call: an NRO's function called from main, main's called back from the NRO). `jit_entry` has the standard
// entries' signature: the dispatcher returns it for a target with no compiled entry (after writing the target to st->pc), and compiled code calls
// it for a direct branch to such a target; it stores what it is passed to st, runs the JIT fallback (nxaot_rt_jit) and returns st's x0, x1 and rv.
//
// The library imports these from the host (the C convention; the library's preserve_all / preserve_most wrappers keep the call sites free of
// spills, section 2.3):
//   void  dm_aot_svc(dm_guest_state *st, uint32_t number)                      svc #number: st holds the whole register file, st->pc the next pc
//   void *nxaot_rt_dispatch(dm_guest_state *st, uint64_t target, const nxaot_ic_desc **slot, uint64_t site)
//                                                                              an inline-cache miss at module offset site: the native standard
//                                                                              entry of guest address target (this pack's, or another pack's of
//                                                                              the same signature), with *slot updated; jit_entry for a target
//                                                                              with no such entry (st->pc = target); or no return
//   void  nxaot_rt_trap(dm_guest_state *st, uint64_t offset, uint32_t reason) a trap (reason: NXAOT_TRAP_*); does not return
//   void  nxaot_rt_jit(dm_guest_state *st, uint64_t lr, uint64_t sp)          the JIT fallback (section 11): st holds the whole register file,
//                                                                              st->pc where the guest continues; returns when the guest
//                                                                              returns to lr (st then holds the whole register file; sp, the
//                                                                              caller's SP at its entry, is for the logs), or with st->pending
//                                                                              set (a non-local transfer, section 3.3)
//   void  nxaot_rt_safepoint(dm_guest_state *st)                               a poll found st->poll set (section 6.2): st holds the whole
//                                                                              register file and st->pc; returns when the thread may go on
//   void  nxaot_rt_gate(dm_guest_state *st, uint64_t site)                     the gate at module offset site, an entry of gates (design 1.1):
//                                                                              st holds the whole register file and st->pc (an observation gate:
//                                                                              the observed instruction, which runs after it; a spin wait's
//                                                                              back-edge: the head the loop goes on at). An observer may change
//                                                                              x0..x29, which the code reloads; a spin wait's back-off may pause
//                                                                              the thread (a safepoint) and changes no register. Imported only by
//                                                                              packs of modules with gate sites
//   uint64_t dm_aot_cntpct(dm_guest_state *st)                                 mrs cntpct_el0 / cntvct_el0: the guest counter, 19.2 MHz ticks
//   the floating-point helpers of src/lift/runtime.h (nxaot_rt_fp_*, nxaot_rt_aes)
#ifndef NXAOT_AOT_MODULE_H
#define NXAOT_AOT_MODULE_H

#include <stdint.h>

#include "nxaot/guest_state.h"

#ifdef __cplusplus
extern "C" {
#endif

// 3: the standard entries' common signature (std_x, std_v; rv 0), the gate sites, the line table per function (fn_lines, lines)
#define NXAOT_MODULE_ABI 3u
#define NXAOT_LOOKUP_EMPTY UINT64_MAX  // the module offset of an empty lookup slot

// Why compiled code stopped (nxaot_rt_trap), and why it handed a continuation to the JIT fallback (st->exit_reason before nxaot_rt_jit): the
// exits of guest_state.h, and the cases of section 11
enum nxaot_trap {
    NXAOT_TRAP_BRK = DM_EXIT_BRK,                    // brk at offset (a trap)
    NXAOT_TRAP_UNDEFINED = DM_EXIT_UNDEFINED,        // an unallocated or reserved encoding (udf among them) (a trap)
    NXAOT_TRAP_UNPREDICTABLE = DM_EXIT_UNPREDICTABLE,  // (a trap)
    NXAOT_TRAP_NO_CODE = DM_EXIT_NO_CODE,            // execution ran out of the function into what is no function's entry: the JIT
    NXAOT_TRAP_UNTRANSLATED = DM_EXIT_UNTRANSLATED,  // an encoding dynarmic's frontend does not translate (LSE atomics, hvc, ...): the JIT
    NXAOT_TRAP_JIT_TARGET = 64,                      // a branch or call to an address that is no compiled function's entry: the JIT
    NXAOT_TRAP_JIT_FPCR = 65,                        // msr fpcr left the FPCR the module was compiled for (section 7.1): the JIT
    NXAOT_TRAP_NORETURN = 66,                        // a call returned that the analysis said does not return (a trap)
    NXAOT_TRAP_JIT_REFUSED = 67,                     // a function the lifter refused (section 11, case 4): the JIT
    NXAOT_TRAP_JIT_MISS = 68,                        // an indirect branch to an address with no compiled entry (the dispatcher): the JIT
};

// Pack modes, conventions and options the code was built with (nxaot_module_desc.flags)
#define NXAOT_FLAG_GHC 1u            // the ghccc convention (else swifttailcc)
#define NXAOT_FLAG_CORRECTNESS 2u    // every register stored and reloaded at every call (the reference mode of the differential tests)
#define NXAOT_FLAG_ALL_ENTRIES 4u    // every function has a standard entry and a lookup slot (nxaot_funcdiff)
#define NXAOT_FLAG_FPSR 8u           // the cumulative FPSR flags are modelled (section 7.4)
#define NXAOT_FLAG_SAFEPOINTS 16u    // safepoint polls at function entries and backward branches (section 6.2)
// What a cross-pack call needs both packs to share besides std_x, std_v and rv: the convention, and whether calls store every register
#define NXAOT_FLAG_ABI_MASK (NXAOT_FLAG_GHC | NXAOT_FLAG_CORRECTNESS)

// An inline cache's descriptor (section 4): immutable once published; a cache slot points at one, and a miss replaces the pointer atomically
typedef struct nxaot_ic_desc {
    uint64_t guest;   // the guest address it was made for (UINT64_MAX: none)
    void *native;     // the standard entry to call
} nxaot_ic_desc;

typedef struct nxaot_lookup_entry {
    uint64_t offset;  // module offset of the function, NXAOT_LOOKUP_EMPTY for an empty slot
    int32_t entry;    // its standard entry, relative to this field
    uint32_t flags;   // NXAOT_LOOKUP_*
} nxaot_lookup_entry;

// SP and lr travel in host registers at every call (section 2.3), so after a normal return the block's copies are the last ones some inner
// frame stored. A caller outside compiled code (the JIT, the host) takes lr as the return address (a normal return is a ret to it) and, for a
// function that keeps SP (it is not in its WRITES), SP as it was at the call; a function that may change SP stores it at its return
#define NXAOT_LOOKUP_SP_KEPT 1u

typedef struct nxaot_fn_entry {
    uint64_t offset;  // module offset of the guest function
    int32_t body;     // its compiled body, relative to this field
    uint32_t size;    // bytes of guest code
} nxaot_fn_entry;

typedef struct nxaot_fn_lines {
    uint32_t first, count;  // the function's entries of the line table: lines[first] .. lines[first + count - 1], sorted by native
} nxaot_fn_lines;

typedef struct nxaot_line_entry {
    uint32_t native;  // the native address, as bytes from the start of the function's compiled body (nxaot_fn_entry.body of its run)
    uint32_t offset;  // the module offset of the guest instruction
} nxaot_line_entry;

// The gates of design 1.1 the code calls nxaot_rt_gate at (the manifest's obs and spin rows)
enum nxaot_gate_kind {
    NXAOT_GATE_OBS = 1,   // an observation gate before the instruction at offset; arg: the number in bits 0..7 and its parameter from bit 8, as the
                          // manifest's two columns and hle.c's gate table keep them
    NXAOT_GATE_SPIN = 2,  // a spin wait's back-edge: the branch at offset, when it is taken back to the head; arg: the head's module offset
};
typedef struct nxaot_gate_site {
    uint32_t offset;  // module offset of the observed instruction, or of the back-edge branch
    uint32_t kind;    // enum nxaot_gate_kind
    uint64_t arg;
} nxaot_gate_site;

typedef struct nxaot_module_desc {
    uint32_t abi;               // NXAOT_MODULE_ABI
    uint32_t flags;             // NXAOT_FLAG_*
    char name[32];              // the module's name ("main", "nro-<hash>", the NRO's file name), NUL-terminated
    uint8_t sha256[32];         // identity: the SHA-256 of the image the code was compiled from (section 1.4)
    uint64_t image_size;        // bytes of the image (bss excluded) and of its text, as the code assumes them
    uint64_t text_end;
    uint64_t bss_size;
    uint64_t entry;             // module offset of the module's entry (_start)
    uint32_t fpcr;              // the FPCR the code is specialised for (section 7.1)
    uint32_t lookup_mask;       // lookup slots - 1 (a power of two minus one)
    uint32_t rv;                // which of v0..v3 the standard entries return in registers (bits 0..3; 0 since ABI 3); `enter` stores them to st->v
    uint32_t pad;
    uint64_t *base;             // the host writes the module's base address here
    const nxaot_lookup_entry *lookup;
    const nxaot_fn_entry *fns;
    uint64_t fn_count;
    const nxaot_fn_lines *fn_lines;   // the line table (section 3.5): fn_count runs, parallel to fns; null when the pack has none (--no-line-table)
    const nxaot_line_entry *lines;
    void (*enter)(dm_guest_state *st, const void *standard_entry);  // calls a standard entry with st's registers; stores x0, x1 and rv back
    const char *tool;           // the toolchain and options it was built with, for the logs
    const uint32_t *returns;    // the module offset after every call site of compiled code, sorted (the JIT's stop points besides entries)
    uint64_t return_count;
    const void *jit_entry;      // the JIT fallback with the standard entries' signature (see above)
    uint32_t std_x, std_v;      // the standard entries' signature: the x and v registers (bit i: x_i, v_i) they take in host registers besides st,
                                // sp and lr (rv: the v registers they return in host registers besides x0 and x1)
    const nxaot_gate_site *gates;   // sorted by offset; null when gate_count is 0
    uint64_t gate_count;
} nxaot_module_desc;

// The lookup hash of a module offset: lookup_mask + 1 slots, linear probing (src/build/pack.cpp builds it, the dispatcher probes it)
static inline uint32_t nxaot_lookup_hash(uint64_t offset, uint32_t mask) {
    return (uint32_t)(((offset >> 2) * 0x9E3779B97F4A7C15ull) >> 32) & mask;
}

// Whether compiled code of pack a may call a standard entry of pack b directly (a cross-pack call): the same signature, convention and mode, and code
// specialised for the same FPCR mode bits (section 7.1: both run under the MXCSR of the caller's)
static inline int nxaot_same_std_abi(const nxaot_module_desc *a, const nxaot_module_desc *b) {
    return (a->flags & NXAOT_FLAG_ABI_MASK) == (b->flags & NXAOT_FLAG_ABI_MASK) && a->std_x == b->std_x && a->std_v == b->std_v && a->rv == b->rv &&
           (a->fpcr & 0x07C80000u) == (b->fpcr & 0x07C80000u);
}

// The gate site at module offset site of a pack (binary search of the sorted table), NULL when none
static inline const nxaot_gate_site *nxaot_gate_at(const nxaot_module_desc *d, uint64_t site) {
    uint64_t lo = 0, hi = d->gate_count;
    while (lo < hi) {
        const uint64_t mid = lo + (hi - lo) / 2;
        if (d->gates[mid].offset == site) return &d->gates[mid];
        if (d->gates[mid].offset < site) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif
