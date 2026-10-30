// LEGO Harry Potter Collection (010052A00B5D2000), Years 5-7 engine (nro0 = libharry2.nro): a memory access that faults on the null page reads
// zero (loads) or is dropped (stores), so the engine's own empty-structure branches decide — the emulator's recovery, natively.
//
// Why: the Years 5-7 frame is built by three pieces that this build orders differently from the Switch. The recorder (nro0+0xd2ff0) stores each
// pass's record at singleton+0x9d0+8*slot (`str x14,[x16,x15,lsl#3]`) and returns the slot id, or -1. The begin-frame routine (nro0+0x15f420)
// copies the 24 entries singleton[0x9d0+i] → singleton[0xa90+i] under the render lock (`bl 0xbae50` at 0x15f454, copy 0x15f4b8..0x15f578,
// release at 0x15f57c). The worker replays a pass while the main thread is still inside that critical section, so it reads structures the frame
// has not filled yet, and every one of them is dereferenced before the engine's null check:
//   nro0+0xd3a2c  ldr  x28, [x8, #0x18]   ; x8 = live pass entry (0): the engine's cmp/ccmp/b.eq at 0xd3a34 guards the SLOT, not the base
//   nro0+0xd3a60  ldr  x8,  [x28], #8     ; the pass's slot list, one entry per draw
//   nro0+0xd3a64  ldr  x0,  [x8, #8]      ; x8 = 0 (empty entry) — x0 = NULL would be handed to 0xd98a0, which walks [x0] as 0x10-byte records
//   nro0+0x156a60 ldr  x0,  [x8, #0x48]   ; x8 = published[tag] (0) on the first frame a pass records anything; the cbz is one instruction late
//   nro0+0x1bf53c ldaxr w9, [x8]          ; x8 = retained object + 0x58, the object pointer loaded as 0
//
// What the emulator did (the user's own Ryubing fix: Ryujinx.Cpu/NullPageCompat.cs TryHandleLoadOrStore, called from
// Ryujinx.HLE/HOS/Kernel/Process/KProcess.cs InvalidAccessHandler, title-gated to 010052a00b5d2000/…2800): it decoded the faulting access, wrote 0
// into a load's destination register and advanced ELR by 4, and dropped stores; its log shows the whole burst emulated one access at a time
// (va=0x18 at pc …4a2c, then 0x0 at …4a30, then 0x20, 0x4, 0x28, 0x30, 0x38 — the five pass blocks of nro0+0xd39c0). With x28 = 0 the engine's own
// cmp/ccmp take b.eq to 0xd3a7c — the branch it takes for an empty SLOT — and the game reached the Mac menu. That is a one-frame skip of the
// passes the frame has not built yet, not a replay of stale data: reading the live table at the published readers instead replays records that
// are deltas against the state the begin-frame reset installs, and an experiment with that on 2026-10-29 black-screened from t50.
//
// nxnative runs the guest code natively, so the access arrives as SIGSEGV at 0x18 / 0x8 / 0x58 and crash.c's page-fault hook chain is where the
// recovery runs: returning 1 with pc advanced resumes as if the access had produced zero (or, for a store, as if it had happened).
//
// Two refinements over the emulator's blanket rule, both measured:
//   · a load that faults here first WAITS, bounded, for the recorder to publish (returning 1 with pc unchanged re-executes it). Measured on
//     2026-10-29: every wait at every pass-table site timed out (23 "[RED] … still null after 4000 re-executions"), so the producer does not
//     publish while the worker holds the render lock; the wait is kept short because a later site may be merely late rather than blocked.
//   · an empty entry in the pass's slot list is SKIPPED (k_empty) rather than carried on as NULL: zeroing x0 at 0xd3a64 handed NULL to 0xd98a0,
//     which walked address 0 upward 0x10 bytes at a time to 0x1000 and crashed outside the null page. Skipping the entry is what the engine
//     itself does with an empty pass.
//
// Scope: only a host built for this game (DM_GAME_ID), only the module nro0 (Years 1-4 runs in nro1, untouched), only the build registered below,
// only addresses below 0x2000 (a null object plus a struct offset — a wild pointer such as 0x3f8000003f800640 takes the crash path with its full
// report), and a cap on the total recovered accesses (the emulator's spin guard, which capped at 500 000).
#include "dm.h"
#include <stdatomic.h>
#include <sys/ucontext.h>

#define LHP_NRO0_BUILD "7e19fa8490f09d9d2a71f74f5903211d"   // the libharry2.nro the crash was read from (patched_v131072/nro0.cuts.tsv)
#define WAIT_ROUNDS 400         // re-executions of the faulting load before reading zero (~1 ms: one signal round trip each)
#define GIVEUP_TIMEOUTS 3       // a site that times out this often stops waiting and reads zero at once (a real null dereference stays cheap)
#define NULLPASS_CAP 250000     // total recovered accesses

uint64_t now_ns(void);

static _Atomic int g_reads, g_stores, g_stopped;
static struct { uint64_t off; _Atomic int rounds, timeouts, seen; uint64_t last_ns; } g_site[32];
static _Atomic int g_nsite;

// An empty entry in a structure the frame has not filled yet: instead of zeroing the destination and carrying a NULL onward (which is how the
// pass walker 0xd98a0 ended up walking address 0 upward to 0x1000), resume at the instruction that skips the entry.
// nro0+0xd3a60  ldr x8, [x28], #8    ; the pass's slot list, one entry per draw
// nro0+0xd3a64  ldr x0, [x8, #8]     ; ← fault 0x8 when the entry is empty: x0 = NULL handed to 0xd98a0
// nro0+0xd3a70  bl  0xd98a0         ; which walks [x0] as a list of 0x10-byte records
// nro0+0xd3a74  subs x27, x27, #1   ; ← resume here: the entry is skipped, as the engine skips an empty pass at 0xd3a7c
static const struct { uint64_t fault, resume; } k_empty[] = {
    { 0xd3a64, 0xd3a74 },
};

// Zero a register of the interrupted context. Darwin's arm64 state has __x[29] only: 29 is __fp, 30 is __lr, 31 is xzr (discard).
static void set_reg(_STRUCT_ARM_THREAD_STATE64 *ss, int r, uint64_t v) {
    if (r >= 0 && r <= 28) ss->__x[r] = v;
    else if (r == 29) ss->__fp = v;
    else if (r == 30) ss->__lr = v;
}
static void zero_reg(_STRUCT_ARM_THREAD_STATE64 *ss, int r) { set_reg(ss, r, 0); }

// The command-list walker 0xd98a0 reads a record's type byte and treats anything above 6 as "unknown" and returns (`cmp w8,#6; b.hi 0xd9a68`).
// A byte load inside it that faults on the null page therefore reads 0xFF, not 0 — the emulator's stop-mode fill, which is what makes a walk over
// a null list unwind instead of stepping through address 0 forever (its log: "faulting byte loads return 0xFF: the game's tag dispatch treats
// values above 6 as unknown and returns").
#define WALKER_LO 0xd98a0ull
#define WALKER_HI 0xd9a80ull
static int byte_load(uint32_t in) {   // LDRB / LDRSB (size 00, opc 01/10/11) in the LDR/STR classes; LDRSB is filled the same way (all ones)
    if ((in & 0x3B000000) != 0x39000000 && (in & 0x3B200000) != 0x38000000 && (in & 0x3B200C00) != 0x38200800) return 0;
    unsigned opc = (in >> 22) & 3;
    return opc != 3 && opc != 0 && (in >> 30) == 0;
}

enum { EMU_NONE, EMU_LOAD, EMU_STORE };
// What the faulting access is: EMU_LOAD zeroes r1 (and r2 for a pair), EMU_STORE drops it and zeroes its status register rs (an exclusive store
// then reads as having succeeded). Anything not recognised — CAS/SWP, SIMD, system — answers EMU_NONE and takes the crash path with its report.
static int classify(uint32_t in, int *r1, int *r2, int *rs) {
    *r1 = (int)(in & 31); *r2 = -1; *rs = -1;
    if ((in & 0x3F000000) == 0x08000000) {                  // exclusive / acquire-release: LDXR, LDAXR, LDAR, STXR, STLXR, STLR (bit 22 = L)
        if (in & (1u << 22)) return EMU_LOAD;
        *rs = (int)((in >> 16) & 31);                      // Rs: xzr for STLR, whose value is not read
        return EMU_STORE;
    }
    unsigned opc;
    if ((in & 0x3B000000) == 0x39000000 || (in & 0x3B200000) == 0x38000000 || (in & 0x3B200C00) == 0x38200800) {   // LDR/STR: unsigned offset, imm9, register offset
        opc = (in >> 22) & 3;                              // 00 store, 01 load, 10 LDRSW, 11 PRFM
        return opc == 3 ? EMU_NONE : opc == 0 ? EMU_STORE : EMU_LOAD;
    }
    if ((in & 0x3A000000) == 0x28000000 && !(in & (1u << 26))) {   // LDP / STP (pair, not SIMD)
        if (!(in & (1u << 22))) return EMU_STORE;
        *r2 = (int)((in >> 10) & 31);
        return EMU_LOAD;
    }
    return EMU_NONE;
}

static int site_of(uint64_t off) {   // Per-site state; a site is logged the first time it faults
    for (int i = 0, n = atomic_load(&g_nsite); i < n; i++) if (g_site[i].off == off) return i;
    int n = atomic_load(&g_nsite);
    if (n >= (int)(sizeof g_site / sizeof g_site[0])) return -1;
    g_site[n].off = off; g_site[n].last_ns = now_ns(); g_site[n].rounds = 0; g_site[n].timeouts = 0; g_site[n].seen = 0;
    if (atomic_compare_exchange_strong(&g_nsite, &n, n + 1)) return n;
    for (int i = 0, k = atomic_load(&g_nsite); i < k; i++) if (g_site[i].off == off) return i;
    return -1;
}

static int nullpass_fault(void *addr, int is_write, void *ucv) {
    uint64_t va = (uint64_t)addr, t = now_ns();
    if (va >= 0x2000 || atomic_load(&g_stopped)) return 0;
    ucontext_t *uc = ucv;
    _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    uint64_t pc = ss->__pc;
    module_t *m = module_of((void *)pc);
    if (!m || strcmp(m->name, "nro0") || strcmp(m->build_id, LHP_NRO0_BUILD)) return 0;
    int r1, r2, rs, kind = classify(*(const uint32_t *)pc, &r1, &r2, &rs);
    if (kind == EMU_NONE) return 0;                        // not an access this handler carries out: the crash report runs
    uint64_t off = pc - (uint64_t)m->base;
    for (int i = 0; i < (int)(sizeof k_empty / sizeof k_empty[0]); i++) {
        if (k_empty[i].fault != off) continue;
        ss->__pc = (uint64_t)m->base + k_empty[i].resume;
        static _Atomic int said;
        if (atomic_fetch_add(&said, 1) < 4)
            LOG("pass table: nro0+%#llx: empty entry in the pass's slot list; skipping it (resume nro0+%#llx)",
                (unsigned long long)off, (unsigned long long)k_empty[i].resume);
        return 1;
    }
    if (kind == EMU_STORE) {                               // dropped, as the emulator dropped stores
        ss->__pc = pc + 4;
        if (rs >= 0) zero_reg(ss, rs);
        int n = atomic_fetch_add(&g_stores, 1);
        if (n < 8) LOG("pass table: nro0+%#llx: store to %#llx dropped (nothing to write yet), instruction skipped",
                       (unsigned long long)off, (unsigned long long)va);
        return 1;
    }
    (void)is_write;
    int s = site_of(off);
    if (s >= 0 && t - g_site[s].last_ns > 50000000ull) g_site[s].rounds = 0;   // No fault here for 50 ms: a new occurrence, wait again
    if (s >= 0) g_site[s].last_ns = t;
    if (s >= 0 && atomic_load(&g_site[s].timeouts) < GIVEUP_TIMEOUTS && atomic_load(&g_site[s].rounds) < WAIT_ROUNDS) {
        if (atomic_fetch_add(&g_site[s].rounds, 1) == 0 && atomic_fetch_add(&g_site[s].seen, 1) < 8)
            LOG("pass table: nro0+%#llx faults at %#llx before this frame's record is published; waiting for it",
                (unsigned long long)off, (unsigned long long)va);
        return 1;   // pc unchanged: the faulting load is re-executed, so the guest resumes as soon as the recorder stores the entry
    }
    if (s >= 0 && atomic_load(&g_site[s].rounds) >= WAIT_ROUNDS) {
        atomic_store(&g_site[s].rounds, 0);
        if (atomic_fetch_add(&g_site[s].timeouts, 1) < 3)
            LOG("[RED] pass table: nro0+%#llx still null after %d re-executions: reading zero and skipping (the emulator's recovery)",
                (unsigned long long)off, WAIT_ROUNDS);
    }
    int n = atomic_fetch_add(&g_reads, 1);
    if (n >= NULLPASS_CAP) {
        atomic_store(&g_stopped, 1);
        LOG("[RED] pass table: %d reads recovered in nro0, stopping (a spin); the next one crashes with the full report", n);
        return 0;
    }
    if (off >= WALKER_LO && off < WALKER_HI && byte_load(*(const uint32_t *)pc)) {
        set_reg(ss, r1, ~0ull);                          // stop-mode: the walker's type byte is "unknown", so it returns
        ss->__pc = pc + 4;
        static _Atomic int said;
        if (atomic_fetch_add(&said, 1) < 4)
            LOG("pass table: nro0+%#llx: byte read at %#llx in the command-list walker reads 0xFF (unknown type, it returns)",
                (unsigned long long)off, (unsigned long long)va);
        return 1;
    }
    zero_reg(ss, r1);
    if (r2 >= 0) zero_reg(ss, r2);
    ss->__pc = pc + 4;
    return 1;
}

void dm_nullpass_install(void) {
    if (strcmp(DM_GAME_ID, "010052A00B5D2000")) return;
    dm_fault_hook_register(nullpass_fault);
    LOG("pass table: nro0's unfilled structures read as zero (LHP Years 5-7, build %s)", LHP_NRO0_BUILD);
}
