// The JIT fallback of docs/design/aot-recompiler.md section 11: dynarmic's A64 JIT (dm/third_party/dynarmic, unmodified) behind the C interface
// the design names, one instance per guest thread on that thread's state block (include/nxaot/guest_state.h), configured as section 11 says:
// fastmem with the one-to-one arena (guest addresses below 2^39 are host addresses), the shared exclusive monitor with inline exclusives (the
// same compare-and-swap on the same memory as compiled code, section 6.5), tpidr_el0 / tpidrro_el0 in the state block, cntfrq_el0 = 19.2 MHz,
// ctr_el0 = 0x40000, dczid_el0 = 4, the counter from the host clock, no cycle counting, svc into the host's SVC path, a 16 MB code cache, and
// without dynarmic's A64GetSetElimination pass (README.md, "Known issues", 1).
//
// Stopping at a compiled entry, with dynarmic unmodified: the host names the addresses the JIT must not run (dm_jit_host.stop_at: the compiled
// entries and the return addresses of compiled call sites), and the code reader hands dynarmic no instruction there, so the block that reaches
// one ends in dynarmic's NoExecuteFault exception, whose callback halts Run(). Translation caches that answer, so a stop costs nothing in the
// blocks that run; it only applies while the JIT's FPCR has the mode bits the compiled code was specialised for (section 7.1). To run a stop
// address itself (a continuation the host gives the JIT), dm_jit_run steps its one instruction first (dynarmic's single-step blocks are cached
// apart), then runs on.
//
// Gates (design 1.1 and 11): the host names the gate sites too (stop_at's DM_JIT_GATE_AT: an observation gate before its instruction, a spin wait's
// back-edge branch), and the code reader stops there in the same way, under any FPCR, unless compiled code takes the address itself (a compiled entry
// or return under the FPCR it was compiled for: its code calls the gate). The run returns DM_JIT_GATE with the gate's instruction not run and the
// whole register file in the block; the host serves the gate on it and goes on with dm_jit_run_past, which runs that one instruction as a
// single-step block (cached apart, so the stopping block and the step are each translated once) without stopping at it again, then runs on, or
// returns after it (one: a spin wait's back-edge, whose back-off runs only when the branch went back to the head). The lookup costs only at
// translation; code with no gate runs as before. dm_jit_run at a gate site stops there before anything runs, so the host serves it first.
//
// Faults (design 5.4): a fastmem access that faults takes dynarmic's fallback to the memory callbacks; with check_halt_on_memory_access the
// callback halts with MemoryAbort instead of touching the memory, so dynarmic leaves at the exact guest instruction, which dm_jit_run then single-
// steps with the access made for real: a fault there is reported at that instruction (dm_jit_access_pc, dm_jit_access_exact).
//
// The hand-over (2.5, 11): dm_jit_run copies the 31 general registers, SP, PC, the 32 vector registers, NZCV, FPCR and FPSR (with the
// cumulative flags compiled code left in MXCSR) into the JIT, and back into the block when it returns; the exclusive monitor is cleared both
// ways, as an exception return clears it. Plain C, for dm/host (the JIT glue in C++, design section 13).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "nxaot/guest_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dm_jit dm_jit;

// Why dm_jit_run returned (dm_jit_reason); the returned address is the guest pc where the guest continues (st->pc)
enum dm_jit_stop {
    DM_JIT_ENTRY = 1,     // a compiled entry (stop_at said so): continue natively through its standard entry
    DM_JIT_RETURN = 2,    // the return address of a call site of compiled code (stop_at said so): a native frame may wait for it
    DM_JIT_HALTED = 3,    // dm_jit_halt (a pause or stop request from another thread); pc is the next block's
    DM_JIT_NO_CODE = 4,   // no code at pc: the host-return address, or outside the code ranges
    DM_JIT_CALLBACK = 5,  // the host's svc callback asked to stop (an SVC that ends the thread or the run); pc is after the svc
    DM_JIT_TRAP = 6,      // brk, an undefined or unpredictable encoding, an encoding the JIT does not translate either (dm_jit_trap gives which)
    DM_JIT_GATE = 7,      // a gate site (stop_at's DM_JIT_GATE_AT): its instruction has not run; serve the gate, then dm_jit_run_past(pc)
    DM_JIT_STEPPED = 8,   // dm_jit_run_past with one: the one instruction ran; pc is the next
};
#define DM_JIT_GATE_AT 0x100   // or'ed into stop_at's answer: pc is a gate site (stopped at under any FPCR when compiled code does not take pc)

// The guest code the JIT may translate: [lo, hi), a module's text or its aux region (the frame step's stubs)
typedef struct dm_jit_range {
    uint64_t lo, hi;
} dm_jit_range;

// What the host provides, once per process before the first dm_jit_create
typedef struct dm_jit_host {
    const dm_jit_range *code;    // the guest code the JIT may translate (copied): no instruction anywhere else (DM_JIT_NO_CODE)
    unsigned code_count;
    uint32_t fpcr;               // the FPCR mode bits compiled code is specialised for: DM_JIT_ENTRY and DM_JIT_RETURN apply only under them
    unsigned max_threads;        // the shared exclusive monitor's processors: at most this many instances at a time
    size_t code_cache;           // bytes of code cache per instance (16 MB)
    void *ctx;                   // passed to the callbacks
    // DM_JIT_ENTRY, DM_JIT_RETURN or 0 (run on) for a guest pc, with DM_JIT_GATE_AT when it is a gate site; must give the same answer for the
    // whole life of an instance (it is asked when a block is translated, once)
    int (*stop_at)(void *ctx, uint64_t pc);
    // svc #number with st holding the whole register file and st->pc after the svc: 0 to go on (st's registers are taken back), else stop
    int (*svc)(void *ctx, dm_guest_state *st, uint32_t number);
    uint64_t (*cntpct)(void *ctx, dm_guest_state *st);  // mrs cntpct_el0 / cntvct_el0: 19.2 MHz guest ticks
} dm_jit_host;

void dm_jit_init(const dm_jit_host *host);
// The code ranges again, with modules loaded since (only ever more code: a range once given stays code); every instance drops its translations
// before its next run, as a block translated before may end where there was no code then
void dm_jit_set_code(const dm_jit_range *code, unsigned count);

// One instance per guest thread, on its state block; one_block_per_run (measurements only): cycle counting with a budget of one block, so that
// Run() returns after every block and the host checks the next pc against stop_at (the alternative to the code-reader stops, design 11)
dm_jit *dm_jit_create(dm_guest_state *st);
dm_jit *dm_jit_create_stepping(dm_guest_state *st, int one_block_per_run);
void dm_jit_destroy(dm_jit *j);

// st -> the JIT, run from pc (pc itself is run even when stop_at names it DM_JIT_ENTRY or DM_JIT_RETURN; at a gate site nothing runs and the run
// returns DM_JIT_GATE) until a stop, the JIT -> st; returns st->pc
uint64_t dm_jit_run(dm_jit *j, uint64_t pc);
// After the host served the gate at pc (DM_JIT_GATE): pc's instruction runs without stopping there, then the run goes on as dm_jit_run's, or with
// one, returns after that instruction (DM_JIT_STEPPED, unless it stopped otherwise)
uint64_t dm_jit_run_past(dm_jit *j, uint64_t pc, int one);
int dm_jit_reason(const dm_jit *j);   // enum dm_jit_stop of the last dm_jit_run
uint32_t dm_jit_trap(const dm_jit *j);  // DM_JIT_TRAP: an enum dm_exit_reason (DM_EXIT_BRK, DM_EXIT_UNDEFINED, ...)
void dm_jit_halt(dm_jit *j);          // from any thread: the running dm_jit_run returns DM_JIT_HALTED at the next block boundary

// Counters: dm_jit_run and dm_jit_run_past calls, Run() calls (one per block in the one-block mode), single steps of stop addresses and gate
// sites, gate stops, memory aborts (a callback access stepped again at its exact instruction)
typedef struct dm_jit_counts {
    uint64_t runs, run_calls, steps, gates, aborts;
} dm_jit_counts;
dm_jit_counts dm_jit_get_counts(const dm_jit *j);

// The hand-over alone (st into the JIT and back), n times: seconds per round trip (nxaot_bench)
double dm_jit_measure_handover(dm_jit *j, unsigned n);

// The guest access a memory callback of the JIT is making on this thread (0 when none): a fault there is a guest fault at that guest pc, the
// exact instruction when dm_jit_access_exact says so (a data access; dc zva's zeroing gives the pc of its block's start)
uint64_t dm_jit_access_pc(void);
uint64_t dm_jit_access_address(void);
int dm_jit_access_exact(void);

#ifdef __cplusplus
}
#endif
