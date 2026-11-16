// The x86-64 execution engine inside the host (docs/design/aot-recompiler.md 2.4 to 11; dm/host/aot_*.c), on the corpus of dm/tools/aot: self-checking NROs
// with no game files, recompiled by `nxaot build` into packs (dm/tools/aot/tests/build_packs.sh), run through the host's own parts. The host's loader maps
// each NRO one to one in the arena below 2^39 and relocates it (loader.c loader_add_image), the pack is loaded with its identity checked (aot_arena.c),
// dm_aot_run enters _start on the main guest thread, svcOutputDebugString is answered by hle_svc.c's svc_dispatch (its "[guest] " log lines are the
// program's output), and the threads of the excl program are hle_thread.c's guest threads (svcCreateThread, ArbitrateLock, WaitForAddress,
// WaitSynchronization, ...) on host threads with a state block each.
// Each run is a child process (this program with --run), as a game is a process; the parent compares its lines with corpus/expected/<program>.out and its
// exit status, and prints a table. A row whose lines differ names the first line that differs and any "[guest] " line found inside another line (a log line
// another thread wrote in two pieces around it: a flake of the pause rows under CPU load, dm.h LOG). The runs:
//   the log lines first (--log-race): one thread writes LOG lines while another prints svcOutputDebugString lines through svc_dispatch, every line whole
//     (dm.h LOG writes its line in one call; written in two, a guest line landed inside a log line and the capture below lost it)
//   every NRO once: its expected lines and exit 0. A host built without the JIT fallback (CMake NXHOST_JIT off) cannot run what has no compiled entry: the
//     programs listed in JIT_NEEDED below then stop with status 3 (DM_AOT_JIT_NEEDED) and a [RED] line naming the address, after printing the start of
//     their expected lines; any other NRO stopping so fails
//   fp again with the JIT built in (jit-start): the JIT's first instance is made ahead on a helper thread from the pack's load (aot_run.c dm_aot_jit_prepare): run once it
//     is ready (--jit-ready, as a game's boot gives it time), the first use takes it and makes nothing (no instance, no decoder table: the 35 + 181..197 ms it cost
//     the thread before; a generous 20 ms bound on the time it took); and in the plain run, whose miss comes at once, the first use waited for it rather than made one
//   excl again, EXCL_RUNS times per compiler: the exact totals of its threaded counters, with real host threads contending
//   excl with the pause test (design 6.2, in the manner of dm/tools/aot/tests/test_safepoints.py): a guest thread whose entry is a host function pauses
//     every other guest thread PAUSES times through SetThreadActivity while more than one runs, reads each through GetThreadContext3 and checks it (pc a
//     guest instruction, SP in the arena and 16-byte aligned, at the head of corpus/rt/pause.S's loop x20 = 2 * x19 and x21 = ~x19), and resumes them
//   fnptr with hidden_ptr set to the guest-visible address of a host function (t_aot_host_entries.c): the guest's blr goes through the dispatcher into the
//     host-function page and its thunk (2.4); the program's check of it passes only if the host answered
//   int_ops and excl (with the JIT built in, fiber too) with their data and bss write-protected again every 0.5 ms and a page-fault hook that unprotects a page at
//     its first write (5.4: the hook first, then the guest store runs again; excl's threads fault on their own stacks; fiber's JIT code must reach the hook before
//     dynarmic's fastmem handler, so no fault may happen inside the JIT's memory callbacks)
//   int_ops with the sampler (DM_AOT_PROFILE, aot_profile.c; design 12's sampled instruction offsets) and int_ops_rounds raised to about a second of CPU time: the child
//     maps every byte of the pack's compiled code through the pack's positions (aot_arena.c dm_aot_offset_of_code; include/nxaot/aot_module.h nxaot_position_at) and
//     counts the bytes whose instruction lies outside the function holding them (none may), writes the profile, checks that every function the sampler counted has a
//     row inside it (its tier: nxaot makes a function with a sample anywhere in it hot) and one function line `# function 0x<entry> <samples> <rows>` (what the packer and the
//     launcher count functions by): one line per sampled function, its samples the sampler's count for it (its rows' sum plus its samples at no instruction), then as many
//     rows as it says, each inside that function, no row before the first line; and writes the same samples in the form the sampler wrote before positions
//     (each function's at its entry). The parent checks the profile: every row `0x<offset> <samples>` under the module's header, the hottest function ref_crc (by
//     corpus/nro/<name>.syms.tsv), most of its samples inside its loops (the range of its backward branches, read from the NRO's words) and away from its entry, at
//     several instructions. With nxaot (--nxaot or the environment variable NXAOT; the aot image), the profile round-trips: `nxaot build --profile` weighs functions
//     (its `aot: profile weights:` line), NXAOT_PROFILE_WEIGHTS=0 gives another pack, the same bytes as the entry form's (the tiers alone)
//   lrtail, with the JIT built in and nxaot (it packs it): a module this test writes word by word whose function calls, then jumps through a br discovery cannot
//     bound into code with no compiled entry, 63 times: the JIT runs each case and the rest of the function, and execution comes back to the caller's compiled code
//     when the function returns (exact JIT runs, addresses and misses, none nonlocal: dm/tools/aot/README.md, "Tail calls and the native frame's return")
//   fpret, with the JIT built in and nxaot (it packs it): a module this test writes word by word whose function sets FPCR and jumps to a host function that returns to
//     the caller under it: the frames propagate the return, which is a miss `fpcr`, not `nonlocal` (aot_run.c REASON_FPCR_RETURN)
//   fpresume, with the JIT built in and nxaot (it packs it, with a misses file the test writes): fpret's shape with the caller's call site a resume point: the
//     return under RZ reaches the caller's frame as a pending transfer at its resume point, which must not take it under another FPCR (lower_calls.cpp FnCall)
//   pollkeep, with nxaot (it packs it): a module this test writes word by word whose leaf keeps x19 but stores its own x19 in a loop's poll (the poll byte set by a host
//     function first): the leaf stores the restored x19 again before it returns, and its caller's caller, which reloads x19 after an indirect call, finds its own
//     (lower_calls.cpp FnDataflow: a poll's and a gate's stores count in restore_block)
//   spkeep, with nxaot (it packs it): a module this test writes word by word whose function calls through a PLT stub's shape (WRITES the union, SP in it) a function
//     that keeps SP, moves it and calls a leaf with an svc: the caller reloads SP from the block and must find its own (the right sum, no JIT run: dm/tools/aot/README.md,
//     "A callee's SP left in the block")
//   guestbt, with nxaot (it packs it): a module this test writes word by word whose third function, two calls deep, aborts through a host function, through svcBreak,
//     or faults on an address no page backs: the host's lines name the call site and the guest's frames exactly (aot.h dm_aot_guest_backtrace, hle_svc.c Break,
//     aot_fault.c dm_aot_fault_backtrace), through compiled code and with the JIT built in through the JIT fallback alone (DM_AOT_JIT_ALL), and with frames in both
//     engines: a function the JIT runs called by compiled code, a compiled one called by the JIT, a compiled one called by a compiled function's rest the JIT runs
//   polls, with nxaot (it packs it, at tier H, with the polls of loops bounded by a constant left out and with every poll): a module this test writes word by
//     word whose thread runs, by turns, loops left without a poll and loops that look bounded and must keep theirs; a tester pauses it at random intervals: every
//     loop stopped in, the worst CPU time from request to stop bounded, nxaot's counts exact (polls_rows below; dm/tools/aot/README.md, "Threads and safepoints")
//   cutcall and noret, with nxaot (it packs them): a module this test writes word by word whose function is cut by the host's misses at two cases of a jump table,
//     the first ending with a call whose return address is the second's entry: the call's continuation is the next function (the right sum); and a call of a
//     function the summaries say does not return, which returns: the JIT goes on from its return address with every register (cutcall_rows below)
//   t_aot_host [--packs <dir>] [--corpus <dir>] [--nro <name>]... [--nxaot <nxaot>]
//     the packs: --packs, else the environment variable NXAOT_PACKS, else the CMake option NXAOT_PACKS; with none, a note and exit 0 (CTest stays green where
//     no packs exist: the Windows runner, a build without the aot image)
//   t_aot_host --run <pack> <nro> [--set <symbol>=<value>]... [--hidden-host] [--dirty] [--pause N --seed S] [--positions] [--jit-ready] [--bt <mode>]      one run (the child)
//   t_aot_host --log-race N      N log lines and N guest lines from two threads (the child of the log-lines row)
#include "../aot.h"
#include "../aot_hostfn.h"
#if NXHOST_JIT
#include "rt/jit.h"   // dm_jit_access_pc: a fault inside the JIT's memory callbacks
#endif
#include <stdarg.h>
#include <stdatomic.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/wait.h>
#include <ucontext.h>
#endif

#ifndef NXHOST_JIT
#define NXHOST_JIT 0
#endif
#ifndef T_AOT_CORPUS
#define T_AOT_CORPUS ""
#endif
#ifndef T_AOT_PACKS
#define T_AOT_PACKS ""
#endif
#if defined(_WIN32)
#define PACK_EXT ".dll"
#define popen _popen
#define pclose _pclose
#else
#define PACK_EXT ".so"
#endif

// The programs whose runs enter the JIT fallback (dm/tools/aot/README.md, "The JIT fallback": fiber's continuations after a stack switch, fp's code after
// msr fpcr; except-gcc left the list when discovery bounded its unwinder's opcode switch)
static const char *const JIT_NEEDED[] = {"fp-gcc", "fp-clang", "fiber-gcc", "fiber-clang"};
enum { EXCL_RUNS = 5, PAUSES = 50, PAUSE_SEEDS = 3, LOG_RACE_LINES = 20000 };
// The guestbt NRO's layout and its host abort's argument (guestbt_nro, below)
enum { GBT_TEXT = 0x1000, GBT_RO = 0x1000, GBT_DATA = 0x2000, GBT_SIZE = 0x3000, GBT_BSS = 0x1000, GBT_PASS = GBT_RO + 0x100 };
#define T_BT_ABORT 0xAB0127u

void guest_thread_init(void);          // thread.c
uint32_t svc_thread_main_init(void);   // hle_thread.c
uint32_t thr_cur_handle(void);
uint64_t svc_dispatch(int num, uint64_t *regs, uint64_t orig_addr);   // hle_svc.c
uint64_t t_aot_hidden(uint64_t x);   // t_aot_host_entries.c
void t_aot_pause_tester(uint64_t arg);
uint64_t t_aot_poll_self(uint64_t x);
// t_aot_poll_self's request: the calling thread's poll byte (guest_state.h), as a pause request sets it
void t_aot_poll_request(void) {
    dm_guest_state *st = dm_aot_self_state();
    if (st) __atomic_store_n(&st->poll, (uint8_t)1, __ATOMIC_SEQ_CST);
}
extern _Atomic uint64_t g_t_aot_hidden_calls;

// ---------- The child: one run ----------
static struct {
    unsigned pauses, seed, threaded, done, invariant, inconsistent, exited, expired;
    uint32_t self;
    uint64_t head, text_lo, text_hi;
    volatile uint32_t *stop;   // excl_pause_stop: the pause loop ends when the tester has its threaded pauses (or at the limit)
    char bad[200];
} g_pt;
static uint64_t g_dirty_lo, g_dirty_hi;
static _Atomic uint64_t g_dirty_faults, g_dirty_compiled, g_dirty_callback;   // write faults the hook resumed: all, a pack's code's, the JIT callbacks'
static _Atomic int g_dirty_stop;

// Horizon's svc::ThreadContext (arm64, 0x320 bytes), as GetThreadContext3 writes it (hle_thread.c hz_ctx_t)
typedef struct { uint64_t r[29], fp, lr, sp, pc; uint32_t pstate, pad; __uint128_t v[32]; uint32_t fpcr, fpsr; uint64_t tpidr; } hz_ctx;
_Static_assert(sizeof(hz_ctx) == 0x320, "ThreadContext");

static uint64_t svc(int n, uint64_t a0, uint64_t a1) {   // an SVC from host code on a guest thread (the pause tester): its own registers, not its block
    uint64_t r[32] = {a0, a1};
    svc_dispatch(n, r, 0);
    return r[0];
}
static uint64_t rng(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }

static void bt_host_abort(void);
// The guestbt NRO (guestbt_nro, below): module offsets of its calls, svcs, faulting loads and functions
typedef struct { uint64_t start_call, f1_call, f2_host, f2_svc, f2_fault, f2, f2_blr, h, h_host, h_svc, h_fault, k, k_call, j, j_call; } gbt_sites;
static size_t guestbt_nro(uint8_t *img, size_t cap, gbt_sites *s);
// The polls row (polls_rows, below): its child's options, its tester, its start
static int lat_on(void);
static void lat_option(unsigned min_per_shape);
static void lat_body(void);
static int lat_start(const module_t *m);
void t_aot_pause_body(uint64_t arg) {
    if (arg == T_BT_ABORT) bt_host_abort();   // the guestbt row's host function (its mode 1), not a pause tester
    if (lat_on()) lat_body();   // the polls row's tester (--latency, below); it does not return
    g_pt.self = thr_cur_handle();
    uint64_t seed = 0x9E3779B97F4A7C15ull * (g_pt.seed + 1);
    uint32_t hs[64];
    const uint64_t limit = plat_now_ns() + 30000000000ull;   // 30 s, as the polls row's: past it the row fails, naming the threaded pauses that landed
    while (dm_aot_stop_status() < 0 && g_pt.threaded < g_pt.pauses && !(g_pt.expired = plat_now_ns() > limit)) {
        plat_sleep_ns((10 + rng(&seed) % 91) * 1000);   // 10 to 100 us, as the reference's default interval
        int n = thr_handles(hs, 64, g_pt.self);
        if (n < 1) continue;
        int paused[64] = {0};
        for (int i = 0; i < n; i++) paused[i] = svc(0x32, hs[i], 1) == 0;   // SetThreadActivity(paused): returns once the thread stopped
        for (int i = 0; i < n; i++) {
            if (!paused[i]) continue;
            hz_ctx c;
            memset(&c, 0, sizeof c);
            if (svc(0x33, (uint64_t)(uintptr_t)&c, hs[i])) continue;   // GetThreadContext3
            if (!c.pc && !c.sp) { g_pt.exited++; continue; }          // it exited meanwhile: no state block any more
            const char *why = NULL;
            if (c.pc != DM_AOT_HOST_RETURN && (c.pc < g_pt.text_lo || c.pc >= g_pt.text_hi || (c.pc & 3))) why = "pc is no guest instruction";
            else if (!dm_aot_in_arena(c.sp, 1) || (c.sp & 15)) why = "sp is no 16-byte aligned guest stack address";
            else if (g_pt.head && c.pc == g_pt.head) {
                g_pt.invariant++;
                if (c.r[20] != 2 * c.r[19] || c.r[21] != ~c.r[19]) why = "x19 / x20 / x21 break x20 = 2 x19, x21 = ~x19 at the loop head";
            }
            if (why) {
                if (!g_pt.inconsistent++)
                    snprintf(g_pt.bad, sizeof g_pt.bad, "thread %#x: %s (pc %#llx sp %#llx)", hs[i], why, (unsigned long long)c.pc, (unsigned long long)c.sp);
            }
        }
        for (int i = 0; i < n; i++) if (paused[i]) svc(0x32, hs[i], 0);   // resume
        g_pt.done++;
        if (n > 1) g_pt.threaded++;
    }
    // The window ends by count: the pause loop's stop word (excl_pause_stop), which its thread reads once per iteration. Before, the loop ran a fixed
    // 200,000,000 iterations and the row asked whether 50 threaded pauses fitted in the time the CPU took for them: a quiet machine missed it now and then
    if (g_pt.stop) *g_pt.stop = 1;
    dm_aot_svc(dm_aot_self_state(), 0x0A);   // svcExitThread: the thread leaves here
}

static int dirty_hook(void *addr, int is_write, void *uc) {   // a dirty-page tracker's hook (5.4): signal context, mprotect only
    uint64_t a = (uint64_t)(uintptr_t)addr;
    if (!is_write || a < g_dirty_lo || a >= g_dirty_hi) return 0;
    const uint64_t pg = plat_page_size();
    if (plat_vm_protect((void *)(uintptr_t)(a & ~(pg - 1)), pg, PLAT_PROT_READ | PLAT_PROT_WRITE)) return 0;
    atomic_fetch_add(&g_dirty_faults, 1);
#if defined(_WIN32)
    const uint64_t pc = (uint64_t)((const CONTEXT *)uc)->Rip;
#else
    const uint64_t pc = (uint64_t)((const ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
#endif
    if (dm_aot_pack_of_code(pc)) atomic_fetch_add(&g_dirty_compiled, 1);
#if NXHOST_JIT
    if (dm_jit_access_pc()) atomic_fetch_add(&g_dirty_callback, 1);   // dynarmic's fastmem handler ran first and fell back to its callbacks (5.4's order broken)
#endif
    return 1;   // the faulting store runs again
}

// The range write-protected again every 0.5 ms while the run lasts, as a GPU dirty tracker protects the pages it watches again after each frame: stores made
// later (by the JIT fallback too, whose fastmem handler comes after the hook: 5.4) fault again
static void *dirty_rearm(void *p) {
    (void)p;
    while (!atomic_load(&g_dirty_stop)) {
        plat_sleep_ns(500000);
        plat_vm_protect((void *)(uintptr_t)g_dirty_lo, g_dirty_hi - g_dirty_lo, PLAT_PROT_READ);
    }
    return NULL;
}

typedef struct {
    const char *pack, *nro; int nsets; const char *sets[8]; int hidden, dirty, positions; unsigned pause, seed; int status; double secs;
    const dm_aot_pack *loaded; const module_t *m;   // the pack and module the run loaded (the positions' check)
    int jit_ready, ready_ahead;                     // --jit-ready: the run starts once the JIT's instance made ahead is ready; it was
    int bt;                                         // --bt <mode>: guestbt's mode, with the host function and the address no page backs it reads (guestbt_nro)
    long hostfn;                                    // --hostfn <module offset>: the guest-visible address of t_aot_hidden written there (fpret_nro), 0: none
    long pollfn;                                    // --pollfn <module offset>: the guest-visible address of t_aot_poll_self written there (pollkeep_nro), 0: none
} run_args;

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(len > 0 ? (size_t)len : 1);
    if (!p || fread(p, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(p); return NULL; }
    fclose(f); *n = (size_t)len;
    return p;
}
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static void *guest_main(void *p) {
    run_args *a = p;
    a->status = DM_AOT_USAGE;
    guest_thread_init();                    // thread.c: the TLS block and thread-local region in the arena, for this thread's state block
    const uint32_t hmain = svc_thread_main_init();   // hle_thread.c: this thread is the main guest thread (handle 0x1001)
    size_t len = 0;
    uint8_t *bytes = slurp(a->nro, &len);
    if (!bytes || len < 0x80 || memcmp(bytes + 0x10, "NRO0", 4)) { printf("[RED] t_aot_host: %s is no NRO\n", a->nro); return NULL; }
    // The NRO header: segments (offset, size) of text, read-only data and data at +0x20, bss at +0x38; the image is the file (dm/tools/aot module/image.cpp)
    const uint64_t text_end = rd32(bytes + 0x24), ro_off = rd32(bytes + 0x28), data_off = rd32(bytes + 0x30), bss = rd32(bytes + 0x38);
    if (rd32(bytes + 0x20) || rd32(bytes + 0x18) > len || data_off + rd32(bytes + 0x34) > len) { printf("[RED] t_aot_host: %s: bad NRO header\n", a->nro); return NULL; }
    char name[16];
    const char *base = a->nro;
    for (const char *c = a->nro; *c; c++) if (*c == '/' || *c == '\\') base = c + 1;
    snprintf(name, sizeof name, "%s", base);
    if (strstr(name, ".nro")) *strstr(name, ".nro") = 0;
    dm_aot_identity id;
    plat_sha256(bytes, len, id.sha256);
    id.image_size = len; id.text_end = text_end; id.bss_size = bss;
    module_t *m = loader_add_image(name, bytes, len, (size_t)bss, text_end, ro_off, data_off);
    free(bytes);
    char err[600];
    dm_aot_pack *pack = dm_aot_pack_load(a->pack, m, &id, err, sizeof err);
    if (!pack) { printf("[RED] t_aot_host: %s\n", err); return NULL; }
    a->loaded = pack; a->m = m;
    for (int i = 0; i < a->nsets; i++) {   // symbol=value: a 64-bit value written over a variable of the program before it runs (after relocation)
        char sym[64]; const char *eq = strchr(a->sets[i], '=');
        snprintf(sym, sizeof sym, "%.*s", eq ? (int)(eq - a->sets[i]) : 0, a->sets[i]);
        void *where = eq ? module_sym(m, sym) : NULL;
        if (!where) { printf("[RED] t_aot_host: --set %s: no such symbol\n", a->sets[i]); return NULL; }
        uint64_t v = strtoull(eq + 1, NULL, 0);
        memcpy(where, &v, 8);
    }
    if (a->hostfn) {   // a module written here that branches to a host function through a pointer in its data (fpret_nro)
        const uint64_t fn = dm_guest_fn((const void *)t_aot_hidden);
        memcpy((uint8_t *)m->base + a->hostfn, &fn, 8);
    }
    if (a->pollfn) {   // pollkeep_nro's request through a pointer in its data
        const uint64_t fn = dm_guest_fn((const void *)t_aot_poll_self);
        memcpy((uint8_t *)m->base + a->pollfn, &fn, 8);
    }
    if (a->hidden) {   // fnptr's hidden_ptr: the guest-visible address of a host function (2.4)
        uint64_t *hp = module_sym(m, "hidden_ptr");
        if (!hp) { printf("[RED] t_aot_host: no hidden_ptr in %s\n", name); return NULL; }
        *hp = dm_guest_fn((const void *)t_aot_hidden);
    }
    if (a->bt) {   // guestbt (its row's header): the mode, the host function's guest-visible address, the address of its fault, the mixed rows' target and h's action
        // --bt 1..3: f2 aborts, breaks or faults itself; 4..6: f2 calls h + 4 (h in the JIT fallback), which does; 7..9: f2 calls k + 4 (k in the JIT), which calls h
        // (compiled), which does; 10..12: f2 hands the rest of itself to the JIT at j, which calls h (compiled), which does
        static uint8_t img[GBT_SIZE];
        gbt_sites s;
        guestbt_nro(img, sizeof img, &s);
        const uint64_t base = (uint64_t)(uintptr_t)m->base;
        const uint64_t target = a->bt >= 10 ? base + s.j : a->bt >= 7 ? base + s.k + 4 : a->bt >= 4 ? base + s.h + 4 : 0;
        const uint64_t act = a->bt >= 4 ? 1 + (uint64_t)(a->bt - 4) % 3 : 0;
        const uint64_t words[5] = {(uint64_t)a->bt, dm_guest_fn((const void *)t_aot_pause_tester), DM_AOT_ARENA_BASE + 0x800, target, act};
        memcpy((uint8_t *)m->base + GBT_DATA, words, sizeof words);
    }
    if (a->dirty) {   // data and bss read-only, each page opened by the hook at its first write
        g_dirty_lo = (uint64_t)(uintptr_t)m->base + (data_off & ~(uint64_t)(plat_page_size() - 1));
        g_dirty_hi = (uint64_t)(uintptr_t)m->base + m->size;
        dm_fault_hook_register(dirty_hook);
        if (plat_vm_protect((void *)(uintptr_t)g_dirty_lo, g_dirty_hi - g_dirty_lo, PLAT_PROT_READ)) { printf("[RED] t_aot_host: write-protect failed\n"); return NULL; }
        pthread_t th;
        if (!pthread_create(&th, NULL, dirty_rearm, NULL)) pthread_detach(th);
    }
    if (a->pause) {   // the pause tester: a guest thread whose entry is a host function, created and started through the host's thread SVCs
        g_pt.pauses = a->pause; g_pt.seed = a->seed;
        uint64_t *head = module_sym(m, "nxaot_pause_loop_head");
        g_pt.head = head ? (uint64_t)(uintptr_t)head : 0;
        g_pt.stop = (volatile uint32_t *)module_sym(m, "excl_pause_stop");
        g_pt.text_lo = (uint64_t)(uintptr_t)m->base; g_pt.text_hi = g_pt.text_lo + text_end;
        const uint64_t stack = (uint64_t)(uintptr_t)dm_guest_alloc(0x10000);
        uint64_t r[32] = {0, dm_guest_fn((const void *)t_aot_pause_tester), 0, stack + 0x10000, 44, (uint64_t)-2};
        svc_dispatch(0x08, r, 0);
        if (r[0]) { printf("[RED] t_aot_host: svcCreateThread for the pause tester: %#llx\n", (unsigned long long)r[0]); return NULL; }
        uint64_t s[32] = {r[1]};
        svc_dispatch(0x09, s, 0);
    }
    if (lat_on() && !lat_start(m)) return NULL;   // the polls row's tester (--latency)
#if NXHOST_JIT
    if (a->jit_ready) {   // as a game's boot gives the helper time before its first miss: the run starts once the JIT's instance made ahead is ready (10 s at most)
        for (int i = 0; i < 10000 && !dm_jit_get_pool_stats().spare; i++) plat_sleep_ns(1000000);
        a->ready_ahead = dm_jit_get_pool_stats().spare;
    }
#endif
    a->status = dm_aot_run((uint64_t)(uintptr_t)m->base + dm_aot_pack_desc(pack)->entry, 0, hmain, &a->secs);
    atomic_store(&g_dirty_stop, 1);
    return NULL;
}

static void timeout_exit(void) {
    fprintf(stderr, "[RED] t_aot_host: the run did not end within 120 s\n");
    fflush(NULL);
    _exit(DM_AOT_STUCK);
}

typedef struct { uint64_t off, n; } prow;
static int prow_cmp(const void *a, const void *b) { const prow *x = a, *y = b; return x->off < y->off ? -1 : x->off > y->off; }

// The sampler's run (--positions, with DM_AOT_PROFILE set by the parent): the pack's positions over every byte of its functions' code, through the host's reader and through
// nxaot_position_at directly (an instruction outside the function holding the byte would be counted at another function: none may be), then the profile written, read back and
// checked against the per-function counts (every sampled function keeps a row inside it), and the same samples written per function at the entries (<dir>/<module>.entries.tsv)
static void positions_check(const run_args *a) {
    const dm_aot_pack *p = a->loaded;
    const char *dir = getenv("DM_AOT_PROFILE");
    if (!p || !dir || !dm_aot_profile_on()) { printf("[RED] t_aot_host: --positions: no pack, or the sampler is off (DM_AOT_PROFILE)\n"); return; }
    const nxaot_module_desc *d = dm_aot_pack_desc(p);
    const nxaot_positions *t = dm_aot_pack_pos(p);
    uint64_t lo = UINT64_MAX, bytes = 0, placed = 0, none = 0, crossed = 0, mismatched = 0;
    for (uint64_t i = 0; i < d->fn_count; i++) {
        const uint64_t body = (uint64_t)(uintptr_t)&d->fns[i].body + (uint64_t)(int64_t)d->fns[i].body;
        if (body < lo) lo = body;
    }
    // Every byte from the first body to the end of the pack's code: the function the map gives, the instruction the positions give
    for (uint64_t pc = lo; t && lo != UINT64_MAX && dm_aot_pack_of_code(pc) == p; pc++) {
        int pk = -1;
        const long fn = dm_aot_fn_of_code(pc, &pk);
        if (fn < 0) continue;
        bytes++;
        const nxaot_fn_entry *f = &d->fns[fn];
        const uint64_t body = (uint64_t)(uintptr_t)&f->body + (uint64_t)(int64_t)f->body;
        const uint32_t raw = nxaot_position_at(t, (uint64_t)fn, f->offset, pc - body);
        const int64_t off = dm_aot_offset_of_code(pc, pk, fn);
        int64_t want;   // what the host's reader must give
        if (raw == NXAOT_POS_NONE) { none++; want = -1; }
        else if (raw < f->offset || raw - f->offset >= f->size) { crossed++; want = -2; }
        else { placed++; want = (int64_t)raw; }
        mismatched += off != want;
    }
    dm_aot_profile_write();   // _exit runs no atexit writer
    char name[64], path[1200];
    snprintf(name, sizeof name, "%s", a->m->name);
    for (char *s = name; *s; s++) if (*s == '/' || *s == '\\') *s = '_';
    snprintf(path, sizeof path, "%s/%s.tsv", dir, name);
    size_t len = 0;
    char *text = (char *)slurp(path, &len);
    if (text && !(text = realloc(text, len + 1))) abort();
    if (text) text[len] = 0;
    size_t nrows = 0, cap = 0, sampled = 0, kept = 0;
    prow *rows = NULL;
    // The function lines (aot_profile.c): `# function 0x<entry> <samples> <rows>` before each sampled function's rows. Each must name a function of the pack's map with the
    // sampler's count for it as its samples (all of them: its rows' sum plus its samples at no instruction, its host calls and SVCs and its code with no position; for a function
    // sampled at none of its instructions its one row, at its entry, holds them all) and the number of rows after it, every one inside that function
    _Atomic uint64_t *c = dm_aot_pack_samples(p);
    size_t flines = 0, fl_good = 0, rows_in = 0;
    long cur = -2;   // the function of the last function line (its index), -1 when that line names none, -2 before the first
    unsigned long long cur_n = 0, cur_decl = 0, cur_seen = 0, cur_sum = 0;
    for (char *line = text, *next; ; line = next) {
        const int end = !line || line >= text + len || !*line;
        unsigned long long off, cnt, decl;
        const int fline = !end && !strncmp(line, "# function 0x", 13) && sscanf(line, "# function %llx %llu %llu", &off, &cnt, &decl) == 3;
        if ((end || fline) && cur != -2)   // the function line before closes
            fl_good += cur >= 0 && c && cur_n == atomic_load(&c[cur]) && cur_seen == cur_decl && cur_sum <= cur_n;
        if (end) break;
        next = memchr(line, '\n', (size_t)(text + len - line));
        if (next) *next++ = 0;
        else next = text + len;
        if (fline) {
            flines++;
            size_t lo = 0, hi = dm_aot_pack_fn_count(p);   // the function whose entry it names
            while (lo < hi) { const size_t mid = lo + (hi - lo) / 2; if (dm_aot_pack_fn(p, mid) < off) lo = mid + 1; else hi = mid; }
            cur = lo < dm_aot_pack_fn_count(p) && dm_aot_pack_fn(p, lo) == off ? (long)lo : -1;
            cur_n = cnt; cur_decl = decl; cur_seen = cur_sum = 0;
            continue;
        }
        if (line[0] == '#' || sscanf(line, "%llx %llu", &off, &cnt) != 2) continue;
        if (nrows == cap && !(rows = realloc(rows, (cap = cap ? 2 * cap : 256) * sizeof *rows))) abort();
        rows[nrows++] = (prow){off, cnt};
        if (cur >= 0) { cur_seen++; cur_sum += cnt; rows_in += off >= dm_aot_pack_fn(p, (size_t)cur) && off < dm_aot_pack_fn_end(p, (size_t)cur); }
    }
    qsort(rows, nrows, sizeof *rows, prow_cmp);
    snprintf(path, sizeof path, "%s/%s.entries.tsv", dir, name);
    FILE *f = fopen(path, "wb");
    if (f) fprintf(f, "# the samples of %s.tsv per function at its entry, the form the sampler wrote before positions (t_aot_host --positions)\n", name);
    for (size_t j = 0; c && j < dm_aot_pack_fn_count(p); j++) {
        const uint64_t v = atomic_load(&c[j]), e = dm_aot_pack_fn(p, j), end = dm_aot_pack_fn_end(p, j);
        if (!v) continue;
        sampled++;
        size_t k = 0, hi = nrows;   // the first row at or after the entry
        while (k < hi) { const size_t mid = k + (hi - k) / 2; if (rows[mid].off < e) k = mid + 1; else hi = mid; }
        kept += k < nrows && rows[k].off < end;
        if (f) fprintf(f, "0x%llx %llu\n", (unsigned long long)e, (unsigned long long)v);
    }
    if (f) fclose(f);
    printf("t_aot_host: positions: %llu code bytes of %llu functions: %llu at an instruction, %llu at none, %llu at one outside their function, %llu read otherwise by the host; "
           "profile: %zu rows, %zu sampled functions, %zu with a row inside them; %s; module %s, profile %s/%s.tsv\n", (unsigned long long)bytes, (unsigned long long)d->fn_count,
           (unsigned long long)placed, (unsigned long long)none, (unsigned long long)crossed, (unsigned long long)mismatched, nrows, sampled, kept,
           t ? "the pack has positions" : "the pack has no positions", a->m->name, dir, name);
    printf("t_aot_host: function lines: %zu for %zu sampled functions, %zu of %zu rows after one and inside its function, %zu with the sampler's samples and their rows' count\n",
           flines, sampled, rows_in, nrows, fl_good);
    free(rows);
    free(text);
}

static int child(int argc, char **argv) {
    run_args a;
    memset(&a, 0, sizeof a);
    a.pack = argv[2]; a.nro = argv[3];
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--set") && i + 1 < argc && a.nsets < 8) a.sets[a.nsets++] = argv[++i];
        else if (!strcmp(argv[i], "--hidden-host")) a.hidden = 1;
        else if (!strcmp(argv[i], "--hostfn") && i + 1 < argc) a.hostfn = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--pollfn") && i + 1 < argc) a.pollfn = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--dirty")) a.dirty = 1;
        else if (!strcmp(argv[i], "--positions")) a.positions = 1;
        else if (!strcmp(argv[i], "--jit-ready")) a.jit_ready = 1;
        else if (!strcmp(argv[i], "--bt") && i + 1 < argc) a.bt = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pause") && i + 1 < argc) a.pause = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) a.seed = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--latency") && i + 1 < argc) lat_option((unsigned)atoi(argv[++i]));
        else { fprintf(stderr, "t_aot_host: unknown option %s\n", argv[i]); return DM_AOT_USAGE; }
    }
    g_flag.dbg = 1;   // hle_svc.c prints every svcOutputDebugString as a "[guest] " line
    crash_install();
    dm_aot_arena_init();
    plat_after_ms(120000, timeout_exit);
    // The main guest thread: on a thread with an 8 MB native stack, as hle_thread.c gives the guest threads it creates (design 6.1)
    pthread_attr_t attr; pthread_t th;
    pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 8u << 20);
    if (pthread_create(&th, &attr, guest_main, &a)) { fprintf(stderr, "t_aot_host: pthread_create failed\n"); return DM_AOT_USAGE; }
    pthread_join(th, NULL);
    if (a.positions) positions_check(&a);
    dm_aot_stats_t s;
    dm_aot_stats(&s);
    printf("t_aot_host: status %d after %.6f s: %llu threads, %llu SVCs, %llu host calls, %llu dispatcher misses, %llu calls into another pack, "
           "%llu pauses at polls, %llu JIT addresses, hidden %llu, dirty faults %llu (%llu by compiled code, %llu in the JIT's memory callbacks)\n", a.status, a.secs, (unsigned long long)s.threads,
           (unsigned long long)s.svcs, (unsigned long long)s.host_calls, (unsigned long long)s.misses, (unsigned long long)s.cross_pack,
           (unsigned long long)s.safepoints, (unsigned long long)s.jit_addresses, (unsigned long long)atomic_load(&g_t_aot_hidden_calls),
           (unsigned long long)atomic_load(&g_dirty_faults), (unsigned long long)atomic_load(&g_dirty_compiled), (unsigned long long)atomic_load(&g_dirty_callback));
    {   // The JIT fallback's runs and misses (aot_run.c note_jit): the (address, reason) pairs of entries the next pack could serve, the nonlocal ones among them
        size_t nonlocal = 0;
        const size_t n = dm_aot_jit_misses(NULL, 0, NULL);
        dm_aot_miss *ms = calloc(n ? n : 1, sizeof *ms);
        if (ms) { dm_aot_jit_misses(ms, n, NULL); for (size_t i = 0; i < n; i++) { const char *w = dm_aot_miss_word(ms[i].reason); nonlocal += w && !strcmp(w, "nonlocal"); } free(ms); }
        printf("t_aot_host: jit: %llu runs, %llu addresses, %zu misses (%zu nonlocal)\n", (unsigned long long)s.jit_runs, (unsigned long long)s.jit_addresses, n, nonlocal);
        printf("t_aot_host: jit instances: %llu made ahead, %llu taken, %llu made on first use, %llu waits (%.3f ms), longest first use %.3f ms; ready before the run %d\n",
               (unsigned long long)s.jit_ahead, (unsigned long long)s.jit_taken, (unsigned long long)s.jit_made_on_use, (unsigned long long)s.jit_waits,
               (double)s.jit_wait_ns * 1e-6, (double)s.jit_first_use_ns * 1e-6, a.ready_ahead);
    }
    if (a.pause)
        printf("t_aot_host: pause test: %u pauses (%u with more than one thread running), %u register-invariant checks, %u inconsistent, %u exited%s%s%s\n",
               g_pt.done, g_pt.threaded, g_pt.invariant, g_pt.inconsistent, g_pt.exited, g_pt.expired ? "; the 30 s limit came first" : "",
               g_pt.inconsistent ? "; first: " : "", g_pt.bad);
    if (dm_aot_profile_on()) dm_aot_profile_write();   // DM_AOT_PROFILE: the profile, misses and bindings files, as a game's exit writes them (_exit runs no atexit writer; once)
    fflush(NULL);
    _exit(a.status);   // other threads may still be parked (a pause tester, a stuck thread): the process ends here, as a game's
}

// The log-lines child: a host thread writes n LOG lines while this one prints n lines through svcOutputDebugString (hle_svc.c's "[guest] " lines), both on
// stderr, which the parent reads together with stdout. The pause test's tester logs its first SetThreadActivity calls while the program prints
static unsigned g_race_n;
static void *race_logger(void *p) {
    (void)p;
    for (unsigned i = 0; i < g_race_n; i++) LOG("t_aot_host: log race %u", i);
    return NULL;
}
static int log_race(unsigned n) {
    g_flag.dbg = 1;
    crash_install();
    dm_aot_arena_init();
    g_race_n = n;
    pthread_t th;
    if (pthread_create(&th, NULL, race_logger, NULL)) { fprintf(stderr, "t_aot_host: pthread_create failed\n"); return DM_AOT_USAGE; }
    char s[64];
    for (unsigned i = 0; i < n; i++) {
        const int len = snprintf(s, sizeof s, "log race %u", i);
        uint64_t r[32] = {(uint64_t)(uintptr_t)s, (uint64_t)len};
        svc_dispatch(0x27, r, 0);   // OutputDebugString
    }
    pthread_join(th, NULL);
    fflush(NULL);
    return 0;
}

// ---------- The parent ----------
typedef struct {
    int status; char *guest; size_t nguest; char red[400]; double secs; unsigned long long stat[12]; int have_stat; unsigned pt[5]; int have_pt;
    unsigned long long jit[4]; int have_jit;   // the child's JIT line: runs, addresses, misses, nonlocal misses
    unsigned long long ji[4]; double ji_wait_ms, ji_first_ms; int ji_ready, have_ji;   // its JIT instances line: made ahead, taken, made on first use, waits
    unsigned ndm, spliced; char splice[300];   // "[dm] " lines; lines holding a "[guest] " line after their start (the first of them)
    char pos[700], prof[900], fl[300];         // the positions' check of the child (--positions), the sampler's summary line (aot_profile.c), the child's function lines' check
} result;

static void append(char **buf, size_t *n, size_t *cap, const char *s, size_t len) {
    if (*n + len + 1 > *cap) { *cap = (*n + len + 1) * 2; *buf = realloc(*buf, *cap); if (!*buf) abort(); }
    memcpy(*buf + *n, s, len); *n += len; (*buf)[*n] = 0;
}

// One child: its stdout and stderr in one pipe, as a game's log is one stream. A guest line is a line that starts with "[guest] "; one found later in a line
// (another thread's line written in two pieces around it) is counted as spliced and not taken
static int run_cmd(const char *cmd, result *r) {
    memset(r, 0, sizeof *r);
    FILE *f = popen(cmd, "r");
    if (!f) { snprintf(r->red, sizeof r->red, "popen failed"); r->status = -1; return -1; }
    char line[8192]; size_t cap = 0;
    while (fgets(line, sizeof line, f)) {
        const char *gp = strstr(line, "[guest] ");
        if (gp && gp != line && !r->spliced++) { snprintf(r->splice, sizeof r->splice, "%s", line); r->splice[strcspn(r->splice, "\n")] = 0; }
        r->ndm += !strncmp(line, "[dm] ", 5);
        if (gp == line) append(&r->guest, &r->nguest, &cap, line + 8, strlen(line + 8));
        else if (strstr(line, "[RED]") && !r->red[0]) { snprintf(r->red, sizeof r->red, "%s", strstr(line, "[RED]")); r->red[strcspn(r->red, "\n")] = 0; }
        if (!strncmp(line, "t_aot_host: status ", 19)) {
            r->have_stat = sscanf(line, "t_aot_host: status %*d after %lf s: %llu threads, %llu SVCs, %llu host calls, %llu dispatcher misses, %llu calls into another pack, "
                                        "%llu pauses at polls, %llu JIT addresses, hidden %llu, dirty faults %llu (%llu by compiled code, %llu in the JIT's memory callbacks)", &r->secs, &r->stat[0],
                                  &r->stat[1], &r->stat[2], &r->stat[3], &r->stat[4], &r->stat[5], &r->stat[6], &r->stat[7], &r->stat[8], &r->stat[9], &r->stat[10]) == 12;
        }
        if (!strncmp(line, "t_aot_host: jit: ", 17))
            r->have_jit = sscanf(line, "t_aot_host: jit: %llu runs, %llu addresses, %llu misses (%llu nonlocal)", &r->jit[0], &r->jit[1], &r->jit[2], &r->jit[3]) == 4;
        if (!strncmp(line, "t_aot_host: jit instances: ", 27))
            r->have_ji = sscanf(line, "t_aot_host: jit instances: %llu made ahead, %llu taken, %llu made on first use, %llu waits (%lf ms), longest first use %lf ms; ready before the run %d",
                                &r->ji[0], &r->ji[1], &r->ji[2], &r->ji[3], &r->ji_wait_ms, &r->ji_first_ms, &r->ji_ready) == 7;
        if (!strncmp(line, "t_aot_host: pause test: ", 24))
            r->have_pt = sscanf(line, "t_aot_host: pause test: %u pauses (%u with more than one thread running), %u register-invariant checks, %u inconsistent, %u exited",
                                &r->pt[0], &r->pt[1], &r->pt[2], &r->pt[3], &r->pt[4]) == 5;
        const char *pl = strstr(line, "t_aot_host: positions: "), *sl = strstr(line, "aot: profile: ");
        if (pl && !r->pos[0]) { snprintf(r->pos, sizeof r->pos, "%s", pl); r->pos[strcspn(r->pos, "\n")] = 0; }
        const char *fll = strstr(line, "t_aot_host: function lines: ");
        if (fll && !r->fl[0]) { snprintf(r->fl, sizeof r->fl, "%s", fll); r->fl[strcspn(r->fl, "\n")] = 0; }
        if (sl && strstr(sl, " Hz of each guest thread's CPU time") && !r->prof[0]) { snprintf(r->prof, sizeof r->prof, "%s", sl); r->prof[strcspn(r->prof, "\n")] = 0; }
    }
    int st = pclose(f);
#if defined(_WIN32)
    r->status = st;
#else
    r->status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
#endif
    if (!r->guest) { r->guest = calloc(1, 1); r->nguest = 0; }
    return r->status;
}

static int run_child(const char *self, const char *pack, const char *nro, const char *extra, result *r) {
    char cmd[4096];
#if defined(_WIN32)
    snprintf(cmd, sizeof cmd, "\"\"%s\" --run \"%s\" \"%s\" %s 2>&1\"", self, pack, nro, extra);
#else
    snprintf(cmd, sizeof cmd, "'%s' --run '%s' '%s' %s 2>&1", self, pack, nro, extra);
#endif
    return run_cmd(cmd, r);
}

// A failing row's note: where the run's guest lines leave the expected ones (the first differing line, counted from 1), and a spliced line if one was seen
static void diff_note(const char *want, const result *r, char *out, size_t n) {
    const char *w = want, *g = r->guest;
    size_t line = 1;
    for (; *w && *g; line++) {
        const size_t wl = strcspn(w, "\n"), gl = strcspn(g, "\n");
        if (wl != gl || memcmp(w, g, wl)) break;
        w += wl + (w[wl] == '\n'); g += gl + (g[gl] == '\n');
    }
    size_t o = 0;
    out[0] = 0;
    if (*w || *g) {
        char ws[160] = "no more lines", gs[160] = "no more lines";
        if (*w) snprintf(ws, sizeof ws, "\"%.*s\"", (int)strcspn(w, "\n"), w);
        if (*g) snprintf(gs, sizeof gs, "\"%.*s\"", (int)strcspn(g, "\n"), g);
        o = (size_t)snprintf(out, n, "; line %zu: want %s, got %s", line, ws, gs);
    }
    if (r->spliced && o < n) snprintf(out + o, n - o, "; %u guest line(s) inside other lines, first: %s", r->spliced, r->splice);
}

static char *read_text(const char *path) {
    size_t n = 0;
    uint8_t *p = slurp(path, &n);
    if (!p) return NULL;
    char *s = realloc(p, n + 1);
    s[n] = 0;
    return s;
}

static int is_jit_needed(const char *name) {
    for (size_t i = 0; i < sizeof JIT_NEEDED / sizeof *JIT_NEEDED; i++) if (!strcmp(JIT_NEEDED[i], name)) return 1;
    return 0;
}

static int checks, fails;
static size_t count_lines(const char *s) { size_t n = 0; for (; s && *s; s++) n += *s == '\n'; return n; }
static void row(const char *name, const char *mode, int ok, const result *r, const char *note) {
    checks++; fails += !ok;
    printf("%-14s %-9s %-4s %3d %9.3f %6zu  %s\n", name, mode, ok ? "ok" : "FAIL", r->status, r->secs * 1e3, count_lines(r->guest), note);
}

// ---------- The sampler's positions (int_ops; the header's list) ----------
#define POS_ROUNDS "2000"   // int_ops_rounds: about a second of CPU time
static void set_env(const char *k, const char *v) {   // for the children popen starts
#if defined(_WIN32)
    char b[1100]; snprintf(b, sizeof b, "%s=%s", k, v ? v : ""); _putenv(b);
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}
static char *without_digests(const char *s) {   // int_ops with int_ops_rounds raised: its digests cover the rounds, its checks and the rest stay as expected
    char *out = calloc(strlen(s) + 1, 1);
    if (!out) abort();
    for (size_t o = 0; *s;) {
        const size_t n = strcspn(s, "\n") + (s[strcspn(s, "\n")] == '\n');
        int digest = 0;
        for (size_t i = 0; i + 8 <= n && !digest; i++) digest = !memcmp(s + i, ".digest ", 8);
        if (!digest) { memcpy(out + o, s, n); o += n; }
        s += n;
    }
    return out;
}
typedef struct { uint64_t lo, hi; char name[64]; } sym_fn;
// The functions of corpus/nro/<name>.syms.tsv (`offset size type binding name`, hexadecimal): their number, into fns[0 .. max)
static int corpus_functions(const char *corpus, const char *name, sym_fn *fns, int max) {
    char path[1100];
    snprintf(path, sizeof path, "%s/nro/%s.syms.tsv", corpus, name);
    char *text = read_text(path);
    int n = 0;
    for (char *line = text, *next; line && *line && n < max; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        unsigned long long off, size; char type[16], bind[16], sym[64];
        if (sscanf(line, "%llx\t%llx\t%15s\t%15s\t%63s", &off, &size, type, bind, sym) == 5 && !strcmp(type, "func") && size) {
            fns[n].lo = off; fns[n].hi = off + size;
            snprintf(fns[n].name, sizeof fns[n].name, "%s", sym);
            n++;
        }
    }
    free(text);
    return n;
}
// The loops of the code [lo, hi) of an NRO (module offsets are file offsets): from the lowest target of a backward branch inside it to the highest such branch; 0 when none
static int loop_range(const uint8_t *img, size_t len, uint64_t lo, uint64_t hi, uint64_t *llo, uint64_t *lhi) {
    *llo = UINT64_MAX; *lhi = 0;
    for (uint64_t pc = lo; pc + 4 <= hi && pc + 4 <= len; pc += 4) {
        const uint32_t w = rd32(img + pc);
        int64_t d = 0;
        if ((w & 0xFF000010u) == 0x54000000u || (w & 0x7E000000u) == 0x34000000u) d = (int64_t)((int32_t)(w << 8) >> 13) * 4;   // b.cond, cbz / cbnz: imm19
        else if ((w & 0x7E000000u) == 0x36000000u) d = (int64_t)((int32_t)(w << 13) >> 18) * 4;                                  // tbz / tbnz: imm14
        else if ((w & 0xFC000000u) == 0x14000000u) d = (int64_t)((int32_t)(w << 6) >> 6) * 4;                                    // b: imm26
        if (d >= 0 || pc + (uint64_t)d < lo) continue;
        if (pc + (uint64_t)d < *llo) *llo = pc + (uint64_t)d;
        if (pc > *lhi) *lhi = pc;
    }
    return *lhi != 0;
}
// nxaot build <nro> --shared <out> --profile <profile> (NXAOT_PROFILE_WEIGHTS=0 when off): its exit status, its `aot: profile weights:` line into line
static int nxaot_build(const char *nxaot, const char *nro, const char *out, const char *profile, int off, char *line, size_t n) {
    char cmd[4096], buf[4096];
#if defined(_WIN32)
    snprintf(cmd, sizeof cmd, "\"\"%s\" build \"%s\" --shared \"%s\" --profile \"%s\" 2>&1\"", nxaot, nro, out, profile);
#else
    snprintf(cmd, sizeof cmd, "'%s' build '%s' --shared '%s' --profile '%s' 2>&1", nxaot, nro, out, profile);
#endif
    set_env("NXAOT_PROFILE_WEIGHTS", off ? "0" : NULL);
    line[0] = 0;
    FILE *f = popen(cmd, "r");
    if (!f) { set_env("NXAOT_PROFILE_WEIGHTS", NULL); return -1; }
    while (fgets(buf, sizeof buf, f)) if (!strncmp(buf, "aot: profile weights:", 21)) { snprintf(line, n, "%s", buf); line[strcspn(line, "\n")] = 0; }
    const int st = pclose(f);
    set_env("NXAOT_PROFILE_WEIGHTS", NULL);
    return st;
}
static int same_file(const char *a, const char *b) {
    size_t na = 0, nb = 0;
    uint8_t *x = slurp(a, &na), *y = slurp(b, &nb);
    const int same = x && y && na == nb && !memcmp(x, y, na);
    free(x); free(y);
    return same;
}

// ---------- A br that is no tail call (dm/tools/aot/README.md, "Tail calls and the native frame's return") ----------
// The NRO lrtail, written here word by word (no compiler in the host's image; nxaot packs it): start calls F LRTAIL_CALLS times with i = 0, 1, ... and sums
// the results. F(i) with i above 0 calls G (x30 then holds the return address inside F) and jumps through br to case (i & 3) of four, at x30 + 16 + 8 *
// (i & 3): no table load, so discovery leaves the br to the dispatcher, and no case is a block of F; the cases join, call G again and return through F's
// epilogue. F(0) branches to the join at once, which makes the join a block of F: its call of G is a compiled call site. G(x) = x + 16, so F(0) = 16 and
// F(i) = i + 33 + (i & 3). start prints "lrtail PASS" or "lrtail FAIL" (svcOutputDebugString) and ends the process (svcExitProcess). Text at 0 (the entry
// branch, the NRO header to 0x80, the code), MOD0 and an empty dynamic section and the two strings in read-only data at 0x1000, a page of data at 0x2000,
// a page of bss
enum { LRTAIL_CALLS = 64 };
static uint32_t a_b(int64_t off) { return 0x14000000u | (uint32_t)((off >> 2) & 0x3FFFFFF); }
static uint32_t a_bl(int64_t off) { return 0x94000000u | (uint32_t)((off >> 2) & 0x3FFFFFF); }
static uint32_t a_bcond(unsigned cond, int64_t off) { return 0x54000000u | (uint32_t)((off >> 2) & 0x7FFFF) << 5 | cond; }
static uint32_t a_adr(unsigned rd, int64_t off) { return 0x10000000u | (uint32_t)(off & 3) << 29 | (uint32_t)((off >> 2) & 0x7FFFF) << 5 | rd; }
static uint32_t a_movz(unsigned rd, unsigned imm) { return 0xD2800000u | imm << 5 | rd; }
static uint32_t a_addi(unsigned rd, unsigned rn, unsigned imm) { return 0x91000000u | imm << 10 | rn << 5 | rd; }
static size_t lrtail_nro(uint8_t *img, size_t cap, uint64_t *want) {
    enum { TEXT = 0x1000, RO = 0x1000, DATA = 0x2000, SIZE = 0x3000, BSS = 0x1000, PASS_AT = RO + 0x100, FAIL_AT = RO + 0x120 };
    if (cap < SIZE) return 0;
    memset(img, 0, SIZE);
    uint32_t w[256] = {0};
    size_t n = 0x80 / 4;   // words before 0x80: the entry branch, MOD0's offset, the header (written below)
    const size_t start = n;
    // start: x19 = i, x20 = the sum
    w[n++] = 0xA9BE7BFDu;               // stp x29, x30, [sp, #-32]!
    w[n++] = 0x910003FDu;               // mov x29, sp
    w[n++] = 0xA90153F3u;               // stp x19, x20, [sp, #16]
    w[n++] = a_movz(19, 0);
    w[n++] = a_movz(20, 0);
    const size_t loop = n;
    w[n++] = 0xAA1303E0u;               // mov x0, x19
    const size_t call_f = n++;          // bl F
    w[n++] = 0x8B000294u;               // add x20, x20, x0
    w[n++] = a_addi(19, 19, 1);
    w[n++] = 0xF100027Fu | LRTAIL_CALLS << 10;   // cmp x19, #LRTAIL_CALLS
    w[n] = a_bcond(3, (int64_t)(loop - n) * 4); n++;   // b.lo loop
    uint64_t sum = 16;
    for (unsigned i = 1; i < LRTAIL_CALLS; i++) sum += i + 33 + (i & 3);
    *want = sum;
    w[n++] = a_movz(9, (unsigned)sum);  // the sum fits 16 bits
    w[n++] = 0xEB09029Fu;               // cmp x20, x9
    const size_t to_fail = n++;         // b.ne fail
    w[n] = a_adr(0, (int64_t)PASS_AT - (int64_t)n * 4); n++;
    w[n++] = a_movz(1, 11);             // "lrtail PASS"
    w[n++] = 0xD40004E1u;               // svc #0x27 (OutputDebugString)
    const size_t to_exit = n++;         // b exit
    w[to_fail] = a_bcond(1, (int64_t)(n - to_fail) * 4);
    w[n] = a_adr(0, (int64_t)FAIL_AT - (int64_t)n * 4); n++;
    w[n++] = a_movz(1, 11);             // "lrtail FAIL"
    w[n++] = 0xD40004E1u;               // svc #0x27
    w[to_exit] = a_b((int64_t)(n - to_exit) * 4);
    w[n++] = 0xD40000E1u;               // svc #0x7 (ExitProcess)
    w[n++] = 0xD4200000u;               // brk #0
    // F
    const size_t f = n;
    w[call_f] = a_bl((int64_t)(f - call_f) * 4);
    w[n++] = 0xA9BE7BFDu;               // stp x29, x30, [sp, #-32]!
    w[n++] = 0x910003FDu;               // mov x29, sp
    w[n++] = 0xF9000BF3u;               // str x19, [sp, #16]
    w[n++] = 0xAA0003F3u;               // mov x19, x0
    const size_t to_join0 = n++;        // cbz x19, join
    const size_t call_g1 = n++;         // bl G
    w[n++] = 0x92400669u;               // and x9, x19, #3       (at x30)
    w[n++] = a_addi(10, 30, 16);        // add x10, x30, #16     (the first case)
    w[n++] = 0x8B090D4Au;               // add x10, x10, x9, lsl #3
    w[n++] = 0xD61F0140u;               // br x10
    if ((n - (call_g1 + 1)) * 4 != 16) return 0;
    size_t to_join[4];
    for (unsigned k = 0; k < 4; k++) { w[n++] = a_addi(0, 0, k + 1); to_join[k] = n++; }
    const size_t join = n;
    for (unsigned k = 0; k < 4; k++) w[to_join[k]] = a_b((int64_t)(join - to_join[k]) * 4);
    w[to_join0] = 0xB4000000u | (uint32_t)(((join - to_join0) & 0x7FFFF) << 5) | 19;   // cbz x19
    const size_t call_g2 = n++;         // bl G
    w[n++] = 0xF9400BF3u;               // ldr x19, [sp, #16]
    w[n++] = 0xA8C27BFDu;               // ldp x29, x30, [sp], #32
    w[n++] = 0xD65F03C0u;               // ret
    // G
    const size_t g = n;
    w[call_g1] = a_bl((int64_t)(g - call_g1) * 4);
    w[call_g2] = a_bl((int64_t)(g - call_g2) * 4);
    w[n++] = a_addi(0, 0, 16);
    w[n++] = 0xD65F03C0u;               // ret
    w[0] = a_b((int64_t)start * 4);
    w[1] = RO;                          // MOD0's offset
    memcpy(img, w, n * 4);
    // The NRO header: magic, version, size, flags; text, read-only data and data (offset, size); bss
    memcpy(img + 0x10, "NRO0", 4);
    const uint32_t hdr[] = {0, SIZE, 0, 0, TEXT, RO, DATA - RO, DATA, SIZE - DATA, BSS};
    memcpy(img + 0x14, hdr, sizeof hdr);
    // MOD0 (offsets from it): the dynamic section (DT_NULL alone), bss start and end, an empty .eh_frame_hdr, the module object (bss)
    memcpy(img + RO, "MOD0", 4);
    const int32_t mod0[] = {0x20, SIZE - RO, SIZE + BSS - RO, 0x40, 0x40, SIZE - RO};
    memcpy(img + RO + 4, mod0, sizeof mod0);
    memcpy(img + PASS_AT, "lrtail PASS", 11);
    memcpy(img + FAIL_AT, "lrtail FAIL", 11);
    return SIZE;
}
// What every pack below adds to its nxaot build: " --no-guess" while a row checks the misses' path of a br left to the dispatcher (casecall: its br's cases
// come from the host's misses, which a first pack's guarded cases from the function's own code would serve first: dm/tools/aot/README.md, "Static cases of
// a br left to the dispatcher"), else nothing
static const char *pack_extra = "";
// nxaot build <nro> --shared <out>: its exit status, its last line into line
// misses: a misses file for `nxaot build --misses` (NULL: none)
static int nxaot_pack_misses(const char *nxaot, const char *nro, const char *out, const char *misses, char *line, size_t n) {
    char cmd[4096], buf[4096], extra[600] = "";
#if defined(_WIN32)
    if (misses) snprintf(extra, sizeof extra, " --misses \"%s\"", misses);
    snprintf(cmd, sizeof cmd, "\"\"%s\" build \"%s\" --shared \"%s\"%s%s 2>&1\"", nxaot, nro, out, extra, pack_extra);
#else
    if (misses) snprintf(extra, sizeof extra, " --misses '%s'", misses);
    snprintf(cmd, sizeof cmd, "'%s' build '%s' --shared '%s'%s%s 2>&1", nxaot, nro, out, extra, pack_extra);
#endif
    line[0] = 0;
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    while (fgets(buf, sizeof buf, f)) { snprintf(line, n, "%s", buf); line[strcspn(line, "\n")] = 0; }
    return pclose(f);
}
static int nxaot_pack(const char *nxaot, const char *nro, const char *out, char *line, size_t n) { return nxaot_pack_misses(nxaot, nro, out, NULL, line, n); }
// option file: one more option of nxaot build and its file (--bind <file>)
static int nxaot_pack_with(const char *nxaot, const char *nro, const char *out, const char *option, const char *file, char *line, size_t n) {
    char cmd[4096], buf[4096];
#if defined(_WIN32)
    snprintf(cmd, sizeof cmd, "\"\"%s\" build \"%s\" --shared \"%s\" %s \"%s\"%s 2>&1\"", nxaot, nro, out, option, file, pack_extra);
#else
    snprintf(cmd, sizeof cmd, "'%s' build '%s' --shared '%s' %s '%s'%s 2>&1", nxaot, nro, out, option, file, pack_extra);
#endif
    line[0] = 0;
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    while (fgets(buf, sizeof buf, f)) { snprintf(line, n, "%s", buf); line[strcspn(line, "\n")] = 0; }
    return pclose(f);
}
// The row: lrtail packed by nxaot and run. Each call of F but the first reaches the JIT fallback once, at its case (the dispatcher has no entry there), and
// comes back to start's compiled code when F returns: 2 JIT runs per call (the case to the return of the join's call of G, which the JIT itself made, and from
// there to F's return), 5 addresses (the four cases, that return), 4 misses (the cases, indirect), none nonlocal. Before, the br was a native tail call of the
// JIT with x30 as its stop, F's return to start a non-local transfer no frame took, and the thread's outermost level ran the rest of start's loop, every F and
// G in it, in the JIT; and every return of a call the JIT made was a nonlocal miss
static void lrtail_row(const char *self, const char *nxaot) {
    if (!NXHOST_JIT) { printf("%-14s %-9s note: no JIT fallback (NXHOST_JIT): not run\n", "lrtail", "jit"); return; }
    if (!nxaot || !*nxaot) { printf("%-14s %-9s note: no nxaot (--nxaot, NXAOT): lrtail is not packed here\n", "lrtail", "jit"); return; }
    static uint8_t img[0x3000];
    uint64_t want = 0;
    const size_t len = lrtail_nro(img, sizeof img, &want);
    char nro[200], pack[200], line[600] = "", note[1400];
    mkdir("t_aot_host.tmp", 0755);
    mkdir("t_aot_host.tmp/lrtail", 0755);
    snprintf(nro, sizeof nro, "t_aot_host.tmp/lrtail/lrtail.nro");
    snprintf(pack, sizeof pack, "t_aot_host.tmp/lrtail/lrtail%s", PACK_EXT);
    FILE *f = fopen(nro, "wb");
    const int wrote = f && fwrite(img, 1, len, f) == len;
    if (f) fclose(f);
    result r;
    memset(&r, 0, sizeof r);
    static char no_lines[1];
    r.guest = no_lines;
    const int built = wrote && nxaot_pack(nxaot, nro, pack, line, sizeof line) == 0;
    if (!built) {
        snprintf(note, sizeof note, "%s: %s", wrote ? "nxaot build failed" : "cannot write the NRO", line);
        row("lrtail", "jit", 0, &r, note);
        return;
    }
    run_child(self, pack, nro, "", &r);
    const unsigned runs = 2 * (LRTAIL_CALLS - 1);
    const int ok = r.status == 0 && !strcmp(r.guest, "lrtail PASS\n") && r.have_jit && r.jit[0] == runs && r.jit[1] == 5 && r.jit[2] == 4 && r.jit[3] == 0;
    snprintf(note, sizeof note, "%u calls of F, %u through a br after a call: %llu JIT runs (want %u), %llu JIT addresses (want 5), %llu misses (want 4), %llu nonlocal (want 0)%s%s",
             LRTAIL_CALLS, LRTAIL_CALLS - 1, r.jit[0], runs, r.jit[1], r.jit[2], r.jit[3], r.red[0] ? "; " : "", r.red);
    row("lrtail", "jit", ok, &r, note);
    if (r.guest != no_lines) free(r.guest);
}

// ---------- A callee's SP left in the block (dm/tools/aot/README.md, "A callee's SP left in the block") ----------
// The NRO spkeep, written here word by word as lrtail is, in the shape of the SDK's nn::os start (Mario vs. Donkey Kong's nnosInitialize): start calls outer(i)
// SPKEEP_CALLS times and sums the results. outer keeps i on its stack and calls stub, a PLT stub's shape (a br through a pointer in data, which start wrote: no
// relocation needed), whose WRITES are the union over the address-taken functions; spfn (address-taken, never called) moves SP, so that union holds SP and outer
// reloads SP from the block after the call. stub reaches target, which keeps SP but moves it (a frame of 32 bytes, its own value 0x777 where outer's i is in
// outer's frame) and calls leaf, which makes an svc with its own frame (svcGetSystemTick): the svc stores leaf's SP, leaf's return stores its restored SP, the SP
// target passed it. target(x) = x + 105, outer(i) = target(i) + 2i. start prints "spkeep PASS" when the sum is right, "spkeep FAIL" otherwise, and ends the process
enum { SPKEEP_CALLS = 16 };
static size_t spkeep_nro(uint8_t *img, size_t cap, uint64_t *want) {
    enum { TEXT = 0x1000, RO = 0x1000, DATA = 0x2000, SIZE = 0x3000, BSS = 0x1000, PASS_AT = RO + 0x100, FAIL_AT = RO + 0x120 };
    if (cap < SIZE) return 0;
    memset(img, 0, SIZE);
    uint32_t w[256] = {0};
    size_t n = 0x80 / 4;
    const size_t start = n;
    // start: the stub's pointer (DATA) and spfn's address (DATA + 8, which makes it address-taken), then x19 = i, x20 = the sum
    w[n++] = 0xA9BE7BFDu;               // stp x29, x30, [sp, #-32]!
    w[n++] = 0x910003FDu;               // mov x29, sp
    w[n++] = 0xA90153F3u;               // stp x19, x20, [sp, #16]
    const size_t adr_target = n++;      // adr x9, target
    w[n] = a_adr(10, (int64_t)DATA - (int64_t)n * 4); n++;   // adr x10, DATA
    w[n++] = 0xF9000149u;               // str x9, [x10]
    const size_t adr_spfn = n++;        // adr x11, spfn
    w[n++] = 0xF900054Bu;               // str x11, [x10, #8]
    w[n++] = a_movz(19, 0);
    w[n++] = a_movz(20, 0);
    const size_t loop = n;
    w[n++] = 0xAA1303E0u;               // mov x0, x19
    const size_t call_outer = n++;      // bl outer
    w[n++] = 0x8B000294u;               // add x20, x20, x0
    w[n++] = a_addi(19, 19, 1);
    w[n++] = 0xF100027Fu | SPKEEP_CALLS << 10;   // cmp x19, #SPKEEP_CALLS
    w[n] = a_bcond(3, (int64_t)(loop - n) * 4); n++;   // b.lo loop
    uint64_t sum = 0;
    for (unsigned i = 0; i < SPKEEP_CALLS; i++) sum += 105 + 3 * i;
    *want = sum;
    w[n++] = a_movz(9, (unsigned)sum);  // the sum fits 16 bits
    w[n++] = 0xEB09029Fu;               // cmp x20, x9
    const size_t to_fail = n++;         // b.ne fail
    w[n] = a_adr(0, (int64_t)PASS_AT - (int64_t)n * 4); n++;
    w[n++] = a_movz(1, 11);             // "spkeep PASS"
    w[n++] = 0xD40004E1u;               // svc #0x27 (OutputDebugString)
    const size_t to_exit = n++;         // b exit
    w[to_fail] = a_bcond(1, (int64_t)(n - to_fail) * 4);
    w[n] = a_adr(0, (int64_t)FAIL_AT - (int64_t)n * 4); n++;
    w[n++] = a_movz(1, 11);             // "spkeep FAIL"
    w[n++] = 0xD40004E1u;               // svc #0x27
    w[to_exit] = a_b((int64_t)(n - to_exit) * 4);
    w[n++] = 0xD40000E1u;               // svc #0x7 (ExitProcess)
    w[n++] = 0xD4200000u;               // brk #0
    // outer(i): i at [sp, #24] of its frame, read back after the call
    const size_t outer = n;
    w[call_outer] = a_bl((int64_t)(outer - call_outer) * 4);
    w[n++] = 0xA9BE7BFDu;               // stp x29, x30, [sp, #-32]!
    w[n++] = 0x910003FDu;               // mov x29, sp
    w[n++] = 0xF9000BF3u;               // str x19, [sp, #16]
    w[n++] = 0xF9000FE0u;               // str x0, [sp, #24]
    w[n++] = 0xAA0003F3u;               // mov x19, x0
    const size_t call_stub = n++;       // bl stub
    w[n++] = 0xF9400FE1u;               // ldr x1, [sp, #24]
    w[n++] = 0x8B010000u;               // add x0, x0, x1
    w[n++] = 0x8B130000u;               // add x0, x0, x19
    w[n++] = 0xF9400BF3u;               // ldr x19, [sp, #16]
    w[n++] = 0xA8C27BFDu;               // ldp x29, x30, [sp], #32
    w[n++] = 0xD65F03C0u;               // ret
    // stub: br through the pointer at DATA, as a PLT entry through its GOT slot
    const size_t stub = n;
    w[call_stub] = a_bl((int64_t)(stub - call_stub) * 4);
    w[n] = a_adr(16, (int64_t)DATA - (int64_t)n * 4); n++;   // adr x16, DATA
    w[n++] = 0xF9400211u;               // ldr x17, [x16]
    w[n++] = 0xD61F0220u;               // br x17
    // target(x): keeps SP; its own frame holds 0x777 where outer's holds i
    const size_t target = n;
    w[adr_target] = a_adr(9, (int64_t)(target - adr_target) * 4);
    w[n++] = 0xA9BE7BFDu;               // stp x29, x30, [sp, #-32]!
    w[n++] = 0x910003FDu;               // mov x29, sp
    w[n++] = 0xF9000BF3u;               // str x19, [sp, #16]
    w[n++] = a_addi(19, 0, 100);        // add x19, x0, #100
    w[n++] = a_movz(9, 0x777);
    w[n++] = 0xF9000FE9u;               // str x9, [sp, #24]
    const size_t call_leaf = n++;       // bl leaf
    w[n++] = 0x8B130000u;               // add x0, x0, x19
    w[n++] = 0xF9400BF3u;               // ldr x19, [sp, #16]
    w[n++] = 0xA8C27BFDu;               // ldp x29, x30, [sp], #32
    w[n++] = 0xD65F03C0u;               // ret
    // leaf: an svc inside a frame of its own, 5
    const size_t leaf = n;
    w[call_leaf] = a_bl((int64_t)(leaf - call_leaf) * 4);
    w[n++] = 0xF81F0FE0u;               // str x0, [sp, #-16]!
    w[n++] = 0xD40003C1u;               // svc #0x1E (GetSystemTick)
    w[n++] = 0x910043FFu;               // add sp, sp, #16
    w[n++] = a_movz(0, 5);
    w[n++] = 0xD65F03C0u;               // ret
    // spfn: returns with SP 16 bytes up (never called: its address is taken)
    const size_t spfn = n;
    w[adr_spfn] = a_adr(11, (int64_t)(spfn - adr_spfn) * 4);
    w[n++] = 0x910043FFu;               // add sp, sp, #16
    w[n++] = 0xD65F03C0u;               // ret
    w[0] = a_b((int64_t)start * 4);
    w[1] = RO;                          // MOD0's offset
    memcpy(img, w, n * 4);
    memcpy(img + 0x10, "NRO0", 4);
    const uint32_t hdr[] = {0, SIZE, 0, 0, TEXT, RO, DATA - RO, DATA, SIZE - DATA, BSS};
    memcpy(img + 0x14, hdr, sizeof hdr);
    memcpy(img + RO, "MOD0", 4);
    const int32_t mod0[] = {0x20, SIZE - RO, SIZE + BSS - RO, 0x40, 0x40, SIZE - RO};
    memcpy(img + RO + 4, mod0, sizeof mod0);
    memcpy(img + PASS_AT, "spkeep PASS", 11);
    memcpy(img + FAIL_AT, "spkeep FAIL", 11);
    return SIZE;
}
// The row: spkeep packed by nxaot and run, with or without the JIT built in: the right sum and no JIT run at all. Before the fix (lower_calls.cpp FnDataflow: SP
// counts as stored at every call), target returned without storing SP, outer went on with target's SP (leaf's return had stored it): its i read as 0x777 and its
// return through target's frame record (to outer's own call site) a non-local transfer, which the thread's outermost level ran in the JIT ("spkeep FAIL", 1
// nonlocal miss; without the JIT, status 3)
static void spkeep_row(const char *self, const char *nxaot) {
    if (!nxaot || !*nxaot) { printf("%-14s %-9s note: no nxaot (--nxaot, NXAOT): spkeep is not packed here\n", "spkeep", "sp"); return; }
    static uint8_t img[0x3000];
    uint64_t want = 0;
    const size_t len = spkeep_nro(img, sizeof img, &want);
    char nro[200], pack[200], line[600] = "", note[1400];
    mkdir("t_aot_host.tmp", 0755);
    mkdir("t_aot_host.tmp/spkeep", 0755);
    snprintf(nro, sizeof nro, "t_aot_host.tmp/spkeep/spkeep.nro");
    snprintf(pack, sizeof pack, "t_aot_host.tmp/spkeep/spkeep%s", PACK_EXT);
    FILE *f = fopen(nro, "wb");
    const int wrote = len && f && fwrite(img, 1, len, f) == len;
    if (f) fclose(f);
    result r;
    memset(&r, 0, sizeof r);
    static char no_lines[1];
    r.guest = no_lines;
    if (!wrote || nxaot_pack(nxaot, nro, pack, line, sizeof line) != 0) {
        snprintf(note, sizeof note, "%s: %s", wrote ? "nxaot build failed" : "cannot write the NRO", line);
        row("spkeep", "sp", 0, &r, note);
        return;
    }
    run_child(self, pack, nro, "", &r);
    const int ok = r.status == 0 && !strcmp(r.guest, "spkeep PASS\n") && r.have_jit && r.jit[0] == 0 && r.jit[2] == 0;
    snprintf(note, sizeof note, "%u calls through a PLT stub's shape into a function that keeps SP and calls (sum %llu): %llu JIT runs (want 0), %llu misses (want 0), %llu nonlocal%s%s",
             SPKEEP_CALLS, (unsigned long long)want, r.jit[0], r.jit[2], r.jit[3], r.red[0] ? "; " : "", r.red);
    row("spkeep", "sp", ok, &r, note);
    if (r.guest != no_lines) free(r.guest);
}

// ---------- A return under another FPCR is no non-local transfer (aot_run.c REASON_FPCR_RETURN) ----------
// The NRO fpret, written here word by word: start calls f with x0 = 0; f sets FPCR to round towards zero (msr fpcr: the JIT fallback runs the rest of f) and jumps through a
// pointer in its data (FPRET_HOSTFN, which the child's --hostfn fills) to the host function t_aot_hidden, which returns to start's call site under round towards zero. start's
// native frame cannot go on under that FPCR: the frames propagate the return (jit_loop: the return its native frame waits for, under another FPCR), and the thread's outermost
// level runs start's rest in the JIT, which reads FPCR (still RZ), clears it and checks t_aot_hidden's result. That return is no non-local transfer: a `nonlocal` miss there
// would give the next pack a resume point at start's call site, where start's compiled code would go on under the wrong FPCR. Its miss is `fpcr`
enum { FPRET_HOSTFN = 0x2000 };
static size_t fpret_nro(uint8_t *img, size_t cap, uint64_t *cont) {
    enum { TEXT = 0x1000, RO = 0x1000, DATA = FPRET_HOSTFN, SIZE = 0x3000, BSS = 0x1000, PASS_AT = RO + 0x100, FAIL_AT = RO + 0x120 };
    if (cap < SIZE) return 0;
    memset(img, 0, SIZE);
    uint32_t w[128] = {0};
    size_t n = 0x80 / 4;
    const size_t start = n;
    w[n++] = 0xA9BF7BFDu;               // stp x29, x30, [sp, #-16]!
    w[n++] = 0x910003FDu;               // mov x29, sp
    w[n++] = a_movz(0, 0);
    const size_t call_f = n++;          // bl f
    *cont = n * 4;
    w[n++] = 0xD53B440Bu;               // mrs x11, fpcr        (round towards zero: the return came back under it)
    w[n++] = 0xD51B441Fu;               // msr fpcr, xzr        (the packs' FPCR again)
    w[n++] = 0xD28B4B49u;               // movz x9, #0x5a5a
    w[n++] = 0xF2AB4B49u;               // movk x9, #0x5a5a, lsl #16
    w[n++] = 0xF2CB4B49u;               // movk x9, #0x5a5a, lsl #32
    w[n++] = 0xF2EB4B49u;               // movk x9, #0x5a5a, lsl #48   (t_aot_hidden(0) = 0 ^ 0x5a5a5a5a5a5a5a5a)
    w[n++] = 0xEB09001Fu;               // cmp x0, x9
    const size_t to_fail1 = n++;        // b.ne fail
    w[n++] = 0xD2A0180Cu;               // movz x12, #0xc0, lsl #16   (FPCR.RMode = 3, RZ)
    w[n++] = 0xEB0C017Fu;               // cmp x11, x12
    const size_t to_fail2 = n++;        // b.ne fail
    w[n] = a_adr(0, (int64_t)PASS_AT - (int64_t)n * 4); n++;
    w[n++] = a_movz(1, 10);             // "fpret PASS"
    w[n++] = 0xD40004E1u;               // svc #0x27 (OutputDebugString)
    const size_t to_exit = n++;         // b exit
    w[to_fail1] = a_bcond(1, (int64_t)(n - to_fail1) * 4);
    w[to_fail2] = a_bcond(1, (int64_t)(n - to_fail2) * 4);
    w[n] = a_adr(0, (int64_t)FAIL_AT - (int64_t)n * 4); n++;
    w[n++] = a_movz(1, 10);             // "fpret FAIL"
    w[n++] = 0xD40004E1u;               // svc #0x27
    w[to_exit] = a_b((int64_t)(n - to_exit) * 4);
    w[n++] = 0xD40000E1u;               // svc #0x7 (ExitProcess)
    w[n++] = 0xD4200000u;               // brk #0
    // f: FPCR to RZ, then a jump to the host function (x30 is still start's call site: the host function returns there)
    const size_t f = n;
    w[call_f] = a_bl((int64_t)(f - call_f) * 4);
    w[n] = a_adr(9, (int64_t)DATA - (int64_t)n * 4); n++;   // adr x9, FPRET_HOSTFN
    w[n++] = 0xF9400130u;               // ldr x16, [x9]
    w[n++] = 0xD2A01802u;               // movz x2, #0xc0, lsl #16
    w[n++] = 0xD51B4402u;               // msr fpcr, x2
    w[n++] = 0xD61F0200u;               // br x16
    w[0] = a_b((int64_t)start * 4);
    w[1] = RO;
    memcpy(img, w, n * 4);
    memcpy(img + 0x10, "NRO0", 4);
    const uint32_t hdr[] = {0, SIZE, 0, 0, TEXT, RO, DATA - RO, DATA, SIZE - DATA, BSS};
    memcpy(img + 0x14, hdr, sizeof hdr);
    memcpy(img + RO, "MOD0", 4);
    const int32_t mod0[] = {0x20, SIZE - RO, SIZE + BSS - RO, 0x40, 0x40, SIZE - RO};
    memcpy(img + RO + 4, mod0, sizeof mod0);
    memcpy(img + PASS_AT, "fpret PASS", 10);
    memcpy(img + FAIL_AT, "fpret FAIL", 10);
    return SIZE;
}
// The row (the JIT built in): the right result under the right FPCR, two misses (the msr fpcr and the return), neither `nonlocal`, and no non-local transfer line. Before
// REASON_FPCR_RETURN the return was a `nonlocal` miss at start's call site, with "a non-local transfer to a continuation with no native frame"
static void fpret_row(const char *self, const char *nxaot) {
    if (!NXHOST_JIT) { printf("%-14s %-9s note: no JIT fallback (NXHOST_JIT): not run\n", "fpret", "fpcr"); return; }
    if (!nxaot || !*nxaot) { printf("%-14s %-9s note: no nxaot (--nxaot, NXAOT): fpret is not packed here\n", "fpret", "fpcr"); return; }
    static uint8_t img[0x3000];
    uint64_t cont = 0;
    const size_t len = fpret_nro(img, sizeof img, &cont);
    char nro[200], pack[200], line[600] = "", note[1400], extra[64];
    mkdir("t_aot_host.tmp", 0755);
    mkdir("t_aot_host.tmp/fpret", 0755);
    snprintf(nro, sizeof nro, "t_aot_host.tmp/fpret/fpret.nro");
    snprintf(pack, sizeof pack, "t_aot_host.tmp/fpret/fpret%s", PACK_EXT);
    FILE *f = fopen(nro, "wb");
    const int wrote = len && f && fwrite(img, 1, len, f) == len;
    if (f) fclose(f);
    result r;
    memset(&r, 0, sizeof r);
    static char no_lines[1];
    r.guest = no_lines;
    if (!wrote || nxaot_pack(nxaot, nro, pack, line, sizeof line) != 0) {
        snprintf(note, sizeof note, "%s: %s", wrote ? "nxaot build failed" : "cannot write the NRO", line);
        row("fpret", "fpcr", 0, &r, note);
        return;
    }
    snprintf(extra, sizeof extra, "--hostfn %#x", FPRET_HOSTFN);
    run_child(self, pack, nro, extra, &r);
    const int ok = r.status == 0 && !strcmp(r.guest, "fpret PASS\n") && r.have_jit && r.jit[2] == 2 && r.jit[3] == 0 && r.have_stat && r.stat[7] == 1;
    snprintf(note, sizeof note, "a return to start+%#llx under RZ: %llu misses (want 2: the msr fpcr and the return, both `fpcr`), %llu nonlocal (want 0), host function called %llu times%s%s",
             (unsigned long long)cont, r.jit[2], r.jit[3], r.stat[7], r.red[0] ? "; first red line: " : "", r.red);
    row("fpret", "fpcr", ok, &r, note);
    if (r.guest != no_lines) free(r.guest);
}

// ---------- A poll's stores count in restore_block (lower_calls.cpp FnDataflow) ----------
// The NRO pollkeep, written here word by word: start sets x19 to 0x1234 and calls q through a register (blr: the indirect view, whose WRITES are the union over the
// standard entries, so start reloads x19 from the block after it). q calls the host function t_aot_poll_self through a pointer in its data (POLLKEEP_FN, which the
// child's --pollfn fills): it sets the thread's poll byte. Then q calls p, a leaf that saves x19 and x20, counts x19 up in a loop of 8 and restores both: the poll of
// the loop's back-edge takes its cold path once and stores p's x19 of the moment into the block. p keeps x19 (outside its WRITES), so q and start keep their copy
// in SSA across it; but start's reload after the blr reads the block. Before the fix p returned without storing the restored x19 again (its restore_block counted the
// stores of calls and svcs, not those of polls and gates) and start read 1 ("pollkeep FAIL")
enum { POLLKEEP_FN = 0x2000 };
static size_t pollkeep_nro(uint8_t *img, size_t cap) {
    enum { TEXT = 0x1000, RO = 0x1000, DATA = POLLKEEP_FN, SIZE = 0x3000, BSS = 0x1000, PASS_AT = RO + 0x100, FAIL_AT = RO + 0x120 };
    if (cap < SIZE) return 0;
    memset(img, 0, SIZE);
    uint32_t w[128] = {0};
    size_t n = 0x80 / 4;
    const size_t start = n;
    w[n++] = 0xA9BE7BFDu;               // stp x29, x30, [sp, #-32]!
    w[n++] = 0x910003FDu;               // mov x29, sp
    w[n++] = 0xF9000BF3u;               // str x19, [sp, #16]
    w[n++] = a_movz(19, 0x1234);
    const size_t adr_r = n++;           // adr x11, r         (r's address taken: the union of the indirect view writes x19)
    const size_t adr_q = n++;           // adr x9, q
    w[n++] = 0xD63F0120u;               // blr x9
    w[n++] = a_movz(10, 0x1234);
    w[n++] = 0xEB0A027Fu;               // cmp x19, x10
    const size_t to_fail = n++;         // b.ne fail
    w[n] = a_adr(0, (int64_t)PASS_AT - (int64_t)n * 4); n++;
    w[n++] = a_movz(1, 13);             // "pollkeep PASS"
    w[n++] = 0xD40004E1u;               // svc #0x27 (OutputDebugString)
    const size_t to_exit = n++;         // b exit
    w[to_fail] = a_bcond(1, (int64_t)(n - to_fail) * 4);
    w[n] = a_adr(0, (int64_t)FAIL_AT - (int64_t)n * 4); n++;
    w[n++] = a_movz(1, 13);             // "pollkeep FAIL"
    w[n++] = 0xD40004E1u;               // svc #0x27
    w[to_exit] = a_b((int64_t)(n - to_exit) * 4);
    w[n++] = 0xD40000E1u;               // svc #0x7 (ExitProcess)
    w[n++] = 0xD4200000u;               // brk #0
    // q: the poll request through the host function, then p
    const size_t q = n;
    w[adr_q] = a_adr(9, (int64_t)(q - adr_q) * 4);
    w[n++] = 0xA9BF7BFDu;               // stp x29, x30, [sp, #-16]!
    w[n++] = 0x910003FDu;               // mov x29, sp
    w[n] = a_adr(9, (int64_t)DATA - (int64_t)n * 4); n++;   // adr x9, POLLKEEP_FN
    w[n++] = 0xF9400130u;               // ldr x16, [x9]
    w[n++] = 0xD63F0200u;               // blr x16            (t_aot_poll_self: the poll byte set)
    const size_t call_p = n++;          // bl p
    w[n++] = 0xA8C17BFDu;               // ldp x29, x30, [sp], #16
    w[n++] = 0xD65F03C0u;               // ret
    // p: a leaf that keeps x19 and x20 and loops with them
    const size_t p = n;
    w[call_p] = a_bl((int64_t)(p - call_p) * 4);
    w[n++] = 0xA9BF53F3u;               // stp x19, x20, [sp, #-16]!
    w[n++] = a_movz(19, 0);
    w[n++] = a_movz(20, 8);
    const size_t loop = n;
    w[n++] = a_addi(19, 19, 1);         // add x19, x19, #1
    w[n++] = 0xF1000694u;               // subs x20, x20, #1
    w[n] = a_bcond(1, (int64_t)loop * 4 - (int64_t)n * 4); n++;   // b.ne loop (its poll)
    w[n++] = 0xA8C153F3u;               // ldp x19, x20, [sp], #16
    w[n++] = 0xD65F03C0u;               // ret
    // r: never called; its address is taken, and it changes x19 and returns, so the indirect view's WRITES (the union) hold x19
    const size_t r = n;
    w[adr_r] = a_adr(11, (int64_t)(r - adr_r) * 4);
    w[n++] = a_movz(19, 0);
    w[n++] = 0xD65F03C0u;               // ret
    w[0] = a_b((int64_t)start * 4);
    w[1] = RO;
    memcpy(img, w, n * 4);
    memcpy(img + 0x10, "NRO0", 4);
    const uint32_t hdr[] = {0, SIZE, 0, 0, TEXT, RO, DATA - RO, DATA, SIZE - DATA, BSS};
    memcpy(img + 0x14, hdr, sizeof hdr);
    memcpy(img + RO, "MOD0", 4);
    const int32_t mod0[] = {0x20, SIZE - RO, SIZE + BSS - RO, 0x40, 0x40, SIZE - RO};
    memcpy(img + RO + 4, mod0, sizeof mod0);
    memcpy(img + PASS_AT, "pollkeep PASS", 13);
    memcpy(img + FAIL_AT, "pollkeep FAIL", 13);
    return SIZE;
}
// The row (nxaot; the JIT not needed): start's x19 back after the call, no JIT run, the host function called once
static void pollkeep_row(const char *self, const char *nxaot) {
    if (!nxaot || !*nxaot) { printf("%-14s %-9s note: no nxaot (--nxaot, NXAOT): pollkeep is not packed here\n", "pollkeep", "poll"); return; }
    static uint8_t img[0x3000];
    const size_t len = pollkeep_nro(img, sizeof img);
    char nro[200], pack[200], line[600] = "", note[1400], extra[64];
    mkdir("t_aot_host.tmp", 0755);
    mkdir("t_aot_host.tmp/pollkeep", 0755);
    snprintf(nro, sizeof nro, "t_aot_host.tmp/pollkeep/pollkeep.nro");
    snprintf(pack, sizeof pack, "t_aot_host.tmp/pollkeep/pollkeep%s", PACK_EXT);
    FILE *f = fopen(nro, "wb");
    const int wrote = len && f && fwrite(img, 1, len, f) == len;
    if (f) fclose(f);
    result r;
    memset(&r, 0, sizeof r);
    static char no_lines[1];
    r.guest = no_lines;
    if (!wrote || nxaot_pack(nxaot, nro, pack, line, sizeof line) != 0) {
        snprintf(note, sizeof note, "%s: %s", wrote ? "nxaot build failed" : "cannot write the NRO", line);
        row("pollkeep", "poll", 0, &r, note);
        return;
    }
    snprintf(extra, sizeof extra, "--pollfn %#x", POLLKEEP_FN);
    run_child(self, pack, nro, extra, &r);
    const int ok = r.status == 0 && !strcmp(r.guest, "pollkeep PASS\n") && r.have_jit && r.jit[0] == 0 && r.have_stat && r.stat[2] == 1;
    snprintf(note, sizeof note, "a caller's x19 reloaded after a callee's poll stored a restored x19 of the moment: %llu JIT runs (want 0), %llu host calls (want 1)%s%s",
             r.jit[0], r.stat[2], r.red[0] ? "; first red line: " : "", r.red);
    row("pollkeep", "poll", ok, &r, note);
    if (r.guest != no_lines) free(r.guest);
}

// ---------- PLT stubs bound to their own module (aot.h, "The loader's bindings"; dm/tools/aot/README.md "PLT stubs bound to their own module") ----------
// The NRO bindstub, written here word by word with a dynamic section: one symbol, t_bind_fn (global, default visibility, defined: f), and one JUMP_SLOT relocation
// of it at BIND_SLOT. start calls the PLT stub (adrp x16; ldr x17, [x16, #lo]; add x16, x16, #lo; br x17) 64 times with x0 = i; f returns 3 * x0 + 1; start sums
// the results. The loader binds the slot to f itself (nothing else defines the name, the host does not take it over), so the host writes the binding with
// DM_AOT_PROFILE, and a pack built with it (nxaot build --bind) runs the stub as b to f: no dispatcher miss. A bindings file that names another target makes a
// pack the host refuses when it loads it
enum { BIND_SLOT = 0x2000, BIND_CALLS = 64 };
static size_t bindstub_nro(uint8_t *img, size_t cap, uint64_t *f_off, uint64_t *want) {
    enum { TEXT = 0x1000, RO = 0x1000, DATA = BIND_SLOT, SIZE = 0x3000, BSS = 0x1000, PASS_AT = RO + 0x400, FAIL_AT = RO + 0x420,
           DYN = RO + 0x20, SYMTAB = RO + 0x100, STRTAB = SYMTAB + 2 * 24, JMPREL = RO + 0x200 };
    if (cap < SIZE) return 0;
    memset(img, 0, SIZE);
    uint32_t w[128] = {0};
    size_t n = 0x80 / 4;
    const size_t start = n;
    w[n++] = 0xA9BE7BFDu;               // stp x29, x30, [sp, #-32]!
    w[n++] = 0x910003FDu;               // mov x29, sp
    w[n++] = 0xA90157F3u;               // stp x19, x21, [sp, #16]
    w[n++] = a_movz(19, 0);             // i
    w[n++] = a_movz(21, 0);             // the sum
    const size_t loop = n;
    w[n++] = 0xAA1303E0u;               // mov x0, x19
    const size_t call_stub = n++;       // bl stub
    w[n++] = 0x8B0002B5u;               // add x21, x21, x0
    w[n++] = a_addi(19, 19, 1);
    w[n++] = 0xF101027Fu;               // cmp x19, #64
    w[n] = a_bcond(1, (int64_t)loop * 4 - (int64_t)n * 4); n++;   // b.ne loop
    uint64_t sum = 0;
    for (unsigned i = 0; i < BIND_CALLS; i++) sum += 3 * i + 1;
    *want = sum;
    w[n++] = a_movz(9, (unsigned)sum);  // the sum fits 16 bits
    w[n++] = 0xEB0902BFu;               // cmp x21, x9
    const size_t to_fail = n++;         // b.ne fail
    w[n] = a_adr(0, (int64_t)PASS_AT - (int64_t)n * 4); n++;
    w[n++] = a_movz(1, 13);             // "bindstub PASS"
    w[n++] = 0xD40004E1u;               // svc #0x27 (OutputDebugString)
    const size_t to_exit = n++;         // b exit
    w[to_fail] = a_bcond(1, (int64_t)(n - to_fail) * 4);
    w[n] = a_adr(0, (int64_t)FAIL_AT - (int64_t)n * 4); n++;
    w[n++] = a_movz(1, 13);             // "bindstub FAIL"
    w[n++] = 0xD40004E1u;               // svc #0x27
    w[to_exit] = a_b((int64_t)(n - to_exit) * 4);
    w[n++] = 0xD40000E1u;               // svc #0x7 (ExitProcess)
    w[n++] = 0xD4200000u;               // brk #0
    // f: 3 * x0 + 1
    const size_t f = n;
    *f_off = f * 4;
    w[n++] = 0x8B000409u;               // add x9, x0, x0, lsl #1
    w[n++] = 0x91000520u;               // add x0, x9, #1
    w[n++] = 0xD65F03C0u;               // ret
    // The PLT stub: the slot's page and offset (both in the first 4 GiB: the adrp's page delta is (BIND_SLOT >> 12) - (stub >> 12))
    const size_t stub = n;
    w[call_stub] = a_bl((int64_t)(stub - call_stub) * 4);
    const int64_t pages = (int64_t)(BIND_SLOT >> 12) - (int64_t)((stub * 4) >> 12);
    w[n++] = 0x90000010u | (uint32_t)(pages & 3) << 29 | (uint32_t)((pages >> 2) & 0x7FFFF) << 5;   // adrp x16, BIND_SLOT page
    w[n++] = 0xF9400211u | (uint32_t)((BIND_SLOT & 0xFFF) / 8) << 10;                               // ldr x17, [x16, #lo]
    w[n++] = 0x91000210u | (uint32_t)(BIND_SLOT & 0xFFF) << 10;                                     // add x16, x16, #lo
    w[n++] = 0xD61F0220u;                                                                          // br x17
    w[0] = a_b((int64_t)start * 4);
    w[1] = RO;
    memcpy(img, w, n * 4);
    memcpy(img + 0x10, "NRO0", 4);
    const uint32_t hdr[] = {0, SIZE, 0, 0, TEXT, RO, DATA - RO, DATA, SIZE - DATA, BSS};
    memcpy(img + 0x14, hdr, sizeof hdr);
    memcpy(img + RO, "MOD0", 4);
    const int32_t mod0[] = {DYN - RO, SIZE - RO, SIZE + BSS - RO, 0x40, 0x40, SIZE - RO};
    memcpy(img + RO + 4, mod0, sizeof mod0);
    // The dynamic section: the symbols, their strings and the one JUMP_SLOT relocation
    const uint64_t dyn[][2] = {{6, SYMTAB}, {5, STRTAB}, {10, 16}, {11, 24}, {23, JMPREL}, {2, 24}, {20, 7}, {0, 0}};
    memcpy(img + DYN, dyn, sizeof dyn);
    const char strs[] = "\0t_bind_fn";
    memcpy(img + STRTAB, strs, sizeof strs);
    uint8_t sym[24] = {0};
    const uint32_t name = 1;
    memcpy(sym, &name, 4);
    sym[4] = 0x12;                       // STB_GLOBAL, STT_FUNC
    sym[5] = 0;                          // STV_DEFAULT
    const uint16_t shndx = 1;
    memcpy(sym + 6, &shndx, 2);
    const uint64_t value = f * 4, size = 12;
    memcpy(sym + 8, &value, 8);
    memcpy(sym + 16, &size, 8);
    memcpy(img + SYMTAB + 24, sym, 24);
    const uint64_t rela[3] = {BIND_SLOT, (1ull << 32) | 1026u, 0};
    memcpy(img + JMPREL, rela, sizeof rela);
    memcpy(img + PASS_AT, "bindstub PASS", 13);
    memcpy(img + FAIL_AT, "bindstub FAIL", 13);
    return SIZE;
}
// The rows (nxaot): the host's bindings file of a run (DM_AOT_PROFILE) holds the slot and f; the pack built with it runs with no dispatcher miss (the pack built
// without it: one, at the stub's br) and the right sum; a pack built with a file that binds the slot to f + 4 is refused at load, naming the slot
static void bindstub_rows(const char *self, const char *nxaot) {
    if (!nxaot || !*nxaot) { printf("%-14s %-9s note: no nxaot (--nxaot, NXAOT): bindstub is not packed here\n", "bindstub", "bind"); return; }
    static uint8_t img[0x3000];
    uint64_t f_off = 0, want = 0;
    const size_t len = bindstub_nro(img, sizeof img, &f_off, &want);
    char nro[200], pack[200], pack_b[200], pack_x[200], dir[200], bind[260], bad[260], line[600] = "", note[1400];
    mkdir("t_aot_host.tmp", 0755);
    mkdir("t_aot_host.tmp/bindstub", 0755);
    snprintf(dir, sizeof dir, "t_aot_host.tmp/bindstub/profile");
    mkdir(dir, 0755);
    snprintf(nro, sizeof nro, "t_aot_host.tmp/bindstub/bindstub.nro");
    snprintf(pack, sizeof pack, "t_aot_host.tmp/bindstub/bindstub%s", PACK_EXT);
    snprintf(pack_b, sizeof pack_b, "t_aot_host.tmp/bindstub/bound/bindstub%s", PACK_EXT);
    snprintf(pack_x, sizeof pack_x, "t_aot_host.tmp/bindstub/wrong/bindstub%s", PACK_EXT);
    mkdir("t_aot_host.tmp/bindstub/bound", 0755);
    mkdir("t_aot_host.tmp/bindstub/wrong", 0755);
    snprintf(bind, sizeof bind, "%s/bindstub.bind.tsv", dir);
    snprintf(bad, sizeof bad, "t_aot_host.tmp/bindstub/wrong.bind.tsv");
    remove(bind);
    FILE *f = fopen(nro, "wb");
    const int wrote = len && f && fwrite(img, 1, len, f) == len;
    if (f) fclose(f);
    result r;
    memset(&r, 0, sizeof r);
    static char no_lines[1];
    r.guest = no_lines;
    if (!wrote || nxaot_pack(nxaot, nro, pack, line, sizeof line) != 0) {
        snprintf(note, sizeof note, "%s: %s", wrote ? "nxaot build failed" : "cannot write the NRO", line);
        row("bindstub", "bind", 0, &r, note);
        return;
    }
    // 1. A run of the pack without bindings, under DM_AOT_PROFILE: the right sum, the stub's br through the dispatcher, the bindings file written
    set_env("DM_AOT_PROFILE", dir);
    run_child(self, pack, nro, "", &r);
    set_env("DM_AOT_PROFILE", NULL);
    char *text = read_text(bind);
    char want_row[96];
    snprintf(want_row, sizeof want_row, "0x%x 0x%llx t_bind_fn\n", BIND_SLOT, (unsigned long long)f_off);
    const int file_ok = text && strstr(text, want_row) && strstr(text, "image SHA-256 ");
    int ok = r.status == 0 && !strcmp(r.guest, "bindstub PASS\n") && r.have_stat && r.stat[3] >= 1 && file_ok;
    snprintf(note, sizeof note, "without --bind: %llu dispatcher misses (the stub's br), bindings file %s%s%s", r.stat[3], file_ok ? "with the slot's row" : "missing or without the row",
             r.red[0] ? "; " : "", r.red);
    row("bindstub", "profile", ok, &r, note);
    if (r.guest != no_lines) free(r.guest);
    // 2. The pack built with the file: no dispatcher miss
    memset(&r, 0, sizeof r);
    r.guest = no_lines;
    char cmd_line[600] = "";
    if (!file_ok || nxaot_pack_with(nxaot, nro, pack_b, "--bind", bind, cmd_line, sizeof cmd_line) != 0) {
        snprintf(note, sizeof note, "nxaot build --bind failed or no bindings file: %s", cmd_line);
        row("bindstub", "bound", 0, &r, note);
        free(text);
        return;
    }
    run_child(self, pack_b, nro, "", &r);
    ok = r.status == 0 && !strcmp(r.guest, "bindstub PASS\n") && r.have_stat && r.stat[3] == 0 && r.have_jit && r.jit[0] == 0;
    snprintf(note, sizeof note, "with --bind: %u calls through the stub, sum %llu, %llu dispatcher misses (want 0), %llu JIT runs%s%s", BIND_CALLS, (unsigned long long)want, r.stat[3],
             r.jit[0], r.red[0] ? "; " : "", r.red);
    row("bindstub", "bound", ok, &r, note);
    if (r.guest != no_lines) free(r.guest);
    // 3. A file that binds the slot elsewhere (f + 4): the host refuses that pack
    memset(&r, 0, sizeof r);
    r.guest = no_lines;
    FILE *b = fopen(bad, "w");
    const char *head = text ? strstr(text, "# module ") : NULL;
    const size_t head_len = head ? strcspn(head, "\n") + 1 : 0;
    const int bad_ok = b && head && fprintf(b, "%.*s0x%x 0x%llx t_bind_fn\n", (int)head_len, head, BIND_SLOT, (unsigned long long)(f_off + 4)) > 0;
    if (b) fclose(b);
    free(text);
    if (!bad_ok || nxaot_pack_with(nxaot, nro, pack_x, "--bind", bad, cmd_line, sizeof cmd_line) != 0) {
        snprintf(note, sizeof note, "nxaot build --bind (another target) failed: %s", cmd_line);
        row("bindstub", "wrong", 0, &r, note);
        return;
    }
    run_child(self, pack_x, nro, "", &r);
    ok = r.status != 0 && strstr(r.red, "it assumes the GOT slot") != NULL;
    snprintf(note, sizeof note, "a bindings file naming f + 4: status %d (want a refused pack), %s", r.status, r.red[0] ? r.red : "no red line");
    row("bindstub", "wrong", ok, &r, note);
    if (r.guest != no_lines) free(r.guest);
}

// ---------- A resume point under another FPCR (lower_calls.cpp FnCall: the resume check reads st->fpcr) ----------
// The NRO fpresume, fpret's shape with start's call site a resume point (the test writes the misses file that makes it one: `<offset> nonlocal 1` under the image's
// SHA-256, as a game's run writes it): f sets FPCR to round towards zero and jumps to the host function t_aot_hidden, which returns to start's call site under it. The
// frames propagate that return (jit_loop: the return its native frame waits for, under another FPCR) with st->pending at the call site, which is now start's resume
// point with start's SP. start's rest rounds 2.75 to an integer in the current mode (frinti: 2 under RZ, 3 under the packs' round to nearest), reads FPCR, clears
// it and checks the three. Resumed natively, start's compiled code runs the checks as it is specialised ("fpresume FAIL"); the transfer must go on past start's frame and
// the thread's outermost level run the rest in the JIT, under RZ
static size_t fpresume_nro(uint8_t *img, size_t cap, uint64_t *cont) {
    enum { TEXT = 0x1000, RO = 0x1000, DATA = FPRET_HOSTFN, SIZE = 0x3000, BSS = 0x1000, PASS_AT = RO + 0x100, FAIL_AT = RO + 0x120 };
    if (cap < SIZE) return 0;
    memset(img, 0, SIZE);
    uint32_t w[128] = {0};
    size_t n = 0x80 / 4;
    const size_t start = n;
    w[n++] = 0xA9BF7BFDu;               // stp x29, x30, [sp, #-16]!
    w[n++] = 0x910003FDu;               // mov x29, sp
    w[n++] = a_movz(0, 0);
    const size_t call_f = n++;          // bl f
    *cont = n * 4;                      // the resume point: the misses file names it
    w[n++] = 0x1E60D001u;               // fmov d1, #2.75
    w[n++] = 0x1E67C020u;               // frinti d0, d1       (the current rounding mode: 2 under RZ)
    w[n++] = 0x9E78000Du;               // fcvtzs x13, d0
    w[n++] = 0xD53B440Bu;               // mrs x11, fpcr
    w[n++] = 0xD51B441Fu;               // msr fpcr, xzr        (the packs' FPCR again)
    w[n++] = 0xD28B4B49u;               // movz x9, #0x5a5a
    w[n++] = 0xF2AB4B49u;               // movk x9, #0x5a5a, lsl #16
    w[n++] = 0xF2CB4B49u;               // movk x9, #0x5a5a, lsl #32
    w[n++] = 0xF2EB4B49u;               // movk x9, #0x5a5a, lsl #48   (t_aot_hidden(0))
    w[n++] = 0xEB09001Fu;               // cmp x0, x9
    const size_t to_fail1 = n++;        // b.ne fail
    w[n++] = 0xD2A0180Cu;               // movz x12, #0xc0, lsl #16   (FPCR.RMode = 3, RZ)
    w[n++] = 0xEB0C017Fu;               // cmp x11, x12
    const size_t to_fail2 = n++;        // b.ne fail
    w[n++] = 0xF10009BFu;               // cmp x13, #2
    const size_t to_fail3 = n++;        // b.ne fail
    w[n] = a_adr(0, (int64_t)PASS_AT - (int64_t)n * 4); n++;
    w[n++] = a_movz(1, 13);             // "fpresume PASS"
    w[n++] = 0xD40004E1u;               // svc #0x27 (OutputDebugString)
    const size_t to_exit = n++;         // b exit
    w[to_fail1] = a_bcond(1, (int64_t)(n - to_fail1) * 4);
    w[to_fail2] = a_bcond(1, (int64_t)(n - to_fail2) * 4);
    w[to_fail3] = a_bcond(1, (int64_t)(n - to_fail3) * 4);
    w[n] = a_adr(0, (int64_t)FAIL_AT - (int64_t)n * 4); n++;
    w[n++] = a_movz(1, 13);             // "fpresume FAIL"
    w[n++] = 0xD40004E1u;               // svc #0x27
    w[to_exit] = a_b((int64_t)(n - to_exit) * 4);
    w[n++] = 0xD40000E1u;               // svc #0x7 (ExitProcess)
    w[n++] = 0xD4200000u;               // brk #0
    // f: FPCR to RZ, then a jump to the host function (x30 is still start's call site: the host function returns there)
    const size_t f = n;
    w[call_f] = a_bl((int64_t)(f - call_f) * 4);
    w[n] = a_adr(9, (int64_t)DATA - (int64_t)n * 4); n++;   // adr x9, FPRET_HOSTFN
    w[n++] = 0xF9400130u;               // ldr x16, [x9]
    w[n++] = 0xD2A01802u;               // movz x2, #0xc0, lsl #16
    w[n++] = 0xD51B4402u;               // msr fpcr, x2
    w[n++] = 0xD61F0200u;               // br x16
    w[0] = a_b((int64_t)start * 4);
    w[1] = RO;
    memcpy(img, w, n * 4);
    memcpy(img + 0x10, "NRO0", 4);
    const uint32_t hdr[] = {0, SIZE, 0, 0, TEXT, RO, DATA - RO, DATA, SIZE - DATA, BSS};
    memcpy(img + 0x14, hdr, sizeof hdr);
    memcpy(img + RO, "MOD0", 4);
    const int32_t mod0[] = {0x20, SIZE - RO, SIZE + BSS - RO, 0x40, 0x40, SIZE - RO};
    memcpy(img + RO + 4, mod0, sizeof mod0);
    memcpy(img + PASS_AT, "fpresume PASS", 13);
    memcpy(img + FAIL_AT, "fpresume FAIL", 13);
    return SIZE;
}
// The row (the JIT built in, nxaot): packed with the misses file, run: the right result under RZ, 2 misses (the msr fpcr and the return the frames propagated, both
// `fpcr`), none `nonlocal`, the host function called once. Before the FPCR check, start's frame took the transfer at its resume point and its compiled code failed the checks ("fpresume FAIL")
static void fpresume_row(const char *self, const char *nxaot) {
    if (!NXHOST_JIT) { printf("%-14s %-9s note: no JIT fallback (NXHOST_JIT): not run\n", "fpresume", "fpcr"); return; }
    if (!nxaot || !*nxaot) { printf("%-14s %-9s note: no nxaot (--nxaot, NXAOT): fpresume is not packed here\n", "fpresume", "fpcr"); return; }
    static uint8_t img[0x3000];
    uint64_t cont = 0;
    const size_t len = fpresume_nro(img, sizeof img, &cont);
    char nro[200], pack[200], misses[200], line[600] = "", note[1400], extra[64];
    mkdir("t_aot_host.tmp", 0755);
    mkdir("t_aot_host.tmp/fpresume", 0755);
    snprintf(nro, sizeof nro, "t_aot_host.tmp/fpresume/fpresume.nro");
    snprintf(pack, sizeof pack, "t_aot_host.tmp/fpresume/fpresume%s", PACK_EXT);
    snprintf(misses, sizeof misses, "t_aot_host.tmp/fpresume/fpresume.misses.tsv");
    FILE *f = fopen(nro, "wb");
    int wrote = len && f && fwrite(img, 1, len, f) == len;
    if (f) fclose(f);
    // The misses file a run with a nonlocal transfer to start's call site writes (aot_profile.c): the header with the image's SHA-256, then the row
    uint8_t sha[32];
    plat_sha256(img, len, sha);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", sha[i]);
    f = fopen(misses, "w");
    wrote = wrote && f && fprintf(f, "# module fpresume, build ID 0, image SHA-256 %s\n%#llx nonlocal 1\n", hex, (unsigned long long)cont) > 0;
    if (f) fclose(f);
    result r;
    memset(&r, 0, sizeof r);
    static char no_lines[1];
    r.guest = no_lines;
    if (!wrote || nxaot_pack_misses(nxaot, nro, pack, misses, line, sizeof line) != 0) {
        snprintf(note, sizeof note, "%s: %s", wrote ? "nxaot build failed" : "cannot write the NRO or its misses", line);
        row("fpresume", "fpcr", 0, &r, note);
        return;
    }
    snprintf(extra, sizeof extra, "--hostfn %#x", FPRET_HOSTFN);
    run_child(self, pack, nro, extra, &r);
    const int ok = r.status == 0 && !strcmp(r.guest, "fpresume PASS\n") && r.have_jit && r.jit[2] == 2 && r.jit[3] == 0 && r.have_stat && r.stat[7] == 1;
    snprintf(note, sizeof note, "a return to start's resume point +%#llx under RZ: %llu misses (want 2, both `fpcr`), %llu nonlocal (want 0), host function called %llu times%s%s",
             (unsigned long long)cont, r.jit[2], r.jit[3], r.stat[7], r.red[0] ? "; first red line: " : "", r.red);
    row("fpresume", "fpcr", ok, &r, note);
    if (r.guest != no_lines) free(r.guest);
}

// ---------- The guest's call site and backtrace in the host's diagnostics (aot.h, "The calling guest's context, and backtraces") ----------
// The NRO guestbt, written here word by word as lrtail is: start calls f1, f1 calls f2, each with a frame record (stp x29, x30; mov x29, sp). f2 reads the word at
// GBT_DATA (the child's --bt <mode> writes it, with the words after it) and, by mode: 1 calls the host function at GBT_DATA + 8 with x0 = T_BT_ABORT (t_aot_pause_tester,
// whose body then makes the calls hle_sdk16.c's diag_abort makes on this engine: the abort path of nn::diag); 2 makes svcBreak (hle_svc.c: the SDK's own abort, as a
// game whose nn::diag is not taken over reaches it); 3 loads from the address at GBT_DATA + 16, which no page backs (a guest fault); 0 returns, and start prints
// "guestbt PASS". The rows check the lines exactly: the call site, and the backtrace's return addresses, f2's call then f1's then start's, nothing after them.
// The mixed rows, frames in both engines: from mode 4 on, h aborts, breaks or faults as f2 does in modes 1 to 3, by the action at GBT_DATA + 32 (1 to 3). Modes 4 to 9
// make f2 call the address at GBT_DATA + 24 through blr (f2_blr); k only calls h (bl, with a frame record). Both start with an instruction a blr past it skips, and
// the child points the blr at h + 4 or k + 4, where the pack has no entry (discovery makes h and k entries of their own: f2 takes their addresses with adr, an
// address no walk reaches): the dispatcher misses and the JIT fallback runs from there (dm/tools/aot/README.md, "The JIT fallback", case 1). So h + 4 runs h in the
// JIT called by compiled f2, and k + 4 runs k in the JIT, whose bl h stops at h's compiled entry and calls it natively: compiled h called by JIT code. Modes 10 to 12
// make f2 call z (a ret) and then br to the address at GBT_DATA + 24, which the child points at j, code of f2's own after the br that no walk reaches: a br with x30
// other than f2's entry lr is no tail call, so compiled f2 hands the rest of itself over (README.md, "Tail calls and the native frame's return"), and the JIT runs j,
// whose bl h calls compiled h: f2's frame is then the JIT's. Everything before f2's mode-3 branch is as it was (the offsets the rows above name); the code of the
// other modes follows f2's ret
static size_t guestbt_nro(uint8_t *img, size_t cap, gbt_sites *s) {
    if (cap < GBT_SIZE) return 0;
    memset(img, 0, GBT_SIZE);
    uint32_t w[256] = {0};
    size_t n = 0x80 / 4;
    const size_t start = n;
    w[n++] = 0xA9BF7BFDu;               // stp x29, x30, [sp, #-16]!
    w[n++] = 0x910003FDu;               // mov x29, sp
    const size_t call_f1 = n++;         // bl f1
    w[n] = a_adr(0, (int64_t)GBT_PASS - (int64_t)n * 4); n++;
    w[n++] = a_movz(1, 12);             // "guestbt PASS"
    w[n++] = 0xD40004E1u;               // svc #0x27 (OutputDebugString)
    w[n++] = 0xD40000E1u;               // svc #0x7 (ExitProcess)
    w[n++] = 0xD4200000u;               // brk #0
    const size_t f1 = n;
    w[call_f1] = a_bl((int64_t)(f1 - call_f1) * 4);
    w[n++] = 0xA9BF7BFDu;
    w[n++] = 0x910003FDu;
    const size_t call_f2 = n++;         // bl f2
    w[n++] = 0xA8C17BFDu;               // ldp x29, x30, [sp], #16
    w[n++] = 0xD65F03C0u;               // ret
    const size_t f2 = n;
    w[call_f2] = a_bl((int64_t)(f2 - call_f2) * 4);
    w[n++] = 0xA9BF7BFDu;
    w[n++] = 0x910003FDu;
    w[n] = a_adr(9, (int64_t)GBT_DATA - (int64_t)n * 4); n++;   // x9 = the mode's word
    w[n++] = 0xF940012Au;               // ldr x10, [x9]
    w[n++] = 0xF100015Fu | 1u << 10;    // cmp x10, #1
    const size_t to_not1 = n++;         // b.ne not1
    w[n++] = 0xF9400530u;               // ldr x16, [x9, #8]
    w[n++] = a_movz(0, 0x127);          // x0 = T_BT_ABORT: 0xAB0127 in two halves
    w[n++] = 0xF2A01560u;               // movk x0, #0xab, lsl #16
    const size_t call_host = n++;
    w[call_host] = 0xD63F0200u;         // blr x16
    const size_t to_out1 = n++;         // b out
    w[to_not1] = a_bcond(1, (int64_t)(n - to_not1) * 4);
    w[n++] = 0xF100015Fu | 2u << 10;    // cmp x10, #2
    const size_t to_not2 = n++;
    w[n++] = a_movz(0, 0);
    w[n++] = a_movz(1, 0);
    w[n++] = a_movz(2, 0);
    const size_t svc_break = n;
    w[n++] = 0xD40004C1u;               // svc #0x26 (Break)
    const size_t to_out2 = n++;
    w[to_not2] = a_bcond(1, (int64_t)(n - to_not2) * 4);
    w[n++] = 0xF100015Fu | 3u << 10;    // cmp x10, #3
    const size_t to_out3 = n++;
    w[n++] = 0xF940092Bu;               // ldr x11, [x9, #16]
    const size_t fault = n;
    w[n++] = 0xF9400160u;               // ldr x0, [x11]: no page there
    const size_t out = n;
    w[to_out1] = a_b((int64_t)(out - to_out1) * 4);
    w[to_out2] = a_b((int64_t)(out - to_out2) * 4);
    w[n++] = 0xA8C17BFDu;               // ldp x29, x30, [sp], #16
    w[n++] = 0xD65F03C0u;               // ret
    // more (any mode from 4 on; one with no target returns): modes 10 and up to more2, the others the blr to the target at GBT_DATA + 24
    const size_t more = n;
    w[to_out3] = a_bcond(1, (int64_t)(more - to_out3) * 4);   // b.ne more
    w[n++] = 0xF100015Fu | 10u << 10;   // cmp x10, #10
    const size_t to_more2 = n++;        // b.hs more2
    w[n++] = 0xF9400D30u;               // ldr x16, [x9, #24]
    const size_t to_out4 = n++;         // cbz x16, out
    const size_t adr_h = n++, adr_k = n++;   // adr x17, h; adr x17, k
    const size_t f2_blr = n;
    w[n++] = 0xD63F0200u;               // blr x16
    w[n] = a_b((int64_t)out * 4 - (int64_t)n * 4); n++;   // b out
    w[to_out4] = 0xB4000000u | (uint32_t)((((int64_t)out - (int64_t)to_out4) & 0x7FFFF) << 5) | 16;
    // more2: a call (x30 is then no longer f2's entry lr), then the br to j
    const size_t more2 = n;
    w[to_more2] = a_bcond(2, (int64_t)(more2 - to_more2) * 4);
    const size_t call_z = n++;          // bl z
    w[n++] = 0xF9400D30u;               // ldr x16, [x9, #24]
    w[n++] = 0xD61F0200u;               // br x16
    // j: reached only through that br
    const size_t j = n;
    const size_t j_call = n++;          // bl h
    w[n] = a_b((int64_t)out * 4 - (int64_t)n * 4); n++;   // b out
    // h: the action at GBT_DATA + 32, as f2's modes 1 to 3
    const size_t h = n;
    w[adr_h] = a_adr(17, (int64_t)(h - adr_h) * 4);
    w[j_call] = a_bl((int64_t)(h - j_call) * 4);
    w[n++] = 0xAA1F03F1u;               // mov x17, xzr: the instruction a blr to h + 4 skips
    w[n++] = 0xA9BF7BFDu;
    w[n++] = 0x910003FDu;
    w[n] = a_adr(9, (int64_t)GBT_DATA - (int64_t)n * 4); n++;
    w[n++] = 0xF940112Au;               // ldr x10, [x9, #32]
    w[n++] = 0xF100015Fu | 1u << 10;    // cmp x10, #1
    const size_t h_not1 = n++;
    w[n++] = 0xF9400530u;               // ldr x16, [x9, #8]
    w[n++] = a_movz(0, 0x127);
    w[n++] = 0xF2A01560u;               // movk x0, #0xab, lsl #16
    const size_t h_host = n;
    w[n++] = 0xD63F0200u;               // blr x16
    const size_t h_out1 = n++;
    w[h_not1] = a_bcond(1, (int64_t)(n - h_not1) * 4);
    w[n++] = 0xF100015Fu | 2u << 10;    // cmp x10, #2
    const size_t h_not2 = n++;
    w[n++] = a_movz(0, 0);
    w[n++] = a_movz(1, 0);
    w[n++] = a_movz(2, 0);
    const size_t h_svc = n;
    w[n++] = 0xD40004C1u;               // svc #0x26 (Break)
    const size_t h_out2 = n++;
    w[h_not2] = a_bcond(1, (int64_t)(n - h_not2) * 4);
    w[n++] = 0xF100015Fu | 3u << 10;    // cmp x10, #3
    const size_t h_out3 = n++;
    w[n++] = 0xF940092Bu;               // ldr x11, [x9, #16]
    const size_t h_fault = n;
    w[n++] = 0xF9400160u;               // ldr x0, [x11]: no page there
    const size_t h_out = n;
    w[h_out1] = a_b((int64_t)(h_out - h_out1) * 4);
    w[h_out2] = a_b((int64_t)(h_out - h_out2) * 4);
    w[h_out3] = a_bcond(1, (int64_t)(h_out - h_out3) * 4);
    w[n++] = 0xA8C17BFDu;               // ldp x29, x30, [sp], #16
    w[n++] = 0xD65F03C0u;               // ret
    // k: calls h
    const size_t k = n;
    w[adr_k] = a_adr(17, (int64_t)(k - adr_k) * 4);
    w[n++] = 0xAA1F03F1u;               // mov x17, xzr: the instruction a blr to k + 4 skips
    w[n++] = 0xA9BF7BFDu;
    w[n++] = 0x910003FDu;
    const size_t k_call = n;
    w[n] = a_bl((int64_t)(h - k_call) * 4); n++;   // bl h
    w[n++] = 0xA8C17BFDu;
    w[n++] = 0xD65F03C0u;
    // z: returns
    const size_t z = n;
    w[call_z] = a_bl((int64_t)(z - call_z) * 4);
    w[n++] = 0xD65F03C0u;               // ret
    if (n * 4 > GBT_RO) return 0;
    w[0] = a_b((int64_t)start * 4);
    w[1] = GBT_RO;                      // MOD0's offset
    memcpy(img, w, n * 4);
    memcpy(img + 0x10, "NRO0", 4);
    const uint32_t hdr[] = {0, GBT_SIZE, 0, 0, GBT_TEXT, GBT_RO, GBT_DATA - GBT_RO, GBT_DATA, GBT_SIZE - GBT_DATA, GBT_BSS};
    memcpy(img + 0x14, hdr, sizeof hdr);
    memcpy(img + GBT_RO, "MOD0", 4);
    const int32_t mod0[] = {0x20, GBT_SIZE - GBT_RO, GBT_SIZE + GBT_BSS - GBT_RO, 0x40, 0x40, GBT_SIZE - GBT_RO};
    memcpy(img + GBT_RO + 4, mod0, sizeof mod0);
    memcpy(img + GBT_PASS, "guestbt PASS", 12);
    *s = (gbt_sites){4 * call_f1, 4 * call_f2, 4 * call_host, 4 * svc_break, 4 * fault, 4 * f2, 4 * f2_blr, 4 * h, 4 * h_host, 4 * h_svc, 4 * h_fault, 4 * k, 4 * k_call,
                     4 * j, 4 * j_call};
    return GBT_SIZE;
}
// The abort a host function makes for the guest (the guestbt row's mode 1): the lines hle_sdk16.c's diag_abort prints on this engine, by the same calls (it is not linked
// into this test), then the end of the process without abort()'s signal
static void bt_host_abort(void) {
    const uint64_t at = dm_aot_guest_lr();
    const module_t *m = module_of((const void *)(uintptr_t)at);
    LOG("game called nn::diag abort: Result %#x, call site %s+%#llx", 0u, m ? m->name : "?", m ? (unsigned long long)(at - (uint64_t)(uintptr_t)m->base) : (unsigned long long)at);
    dm_aot_guest_backtrace();
    fflush(NULL);
    _exit(DM_AOT_TRAP);
}
// The whole output of one child (stdout and stderr), and its status
static int run_all(const char *self, const char *pack, const char *nro, const char *extra, char **out) {
    char cmd[4096], line[8192];
#if defined(_WIN32)
    snprintf(cmd, sizeof cmd, "\"\"%s\" --run \"%s\" \"%s\" %s 2>&1\"", self, pack, nro, extra);
#else
    snprintf(cmd, sizeof cmd, "'%s' --run '%s' '%s' %s 2>&1", self, pack, nro, extra);
#endif
    size_t n = 0, cap = 0;
    *out = calloc(1, 1);
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) append(out, &n, &cap, line, strlen(line));
    const int st = pclose(f);
#if defined(_WIN32)
    return st;
#else
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
#endif
}
// The line of text that starts with prefix (its first), without its newline, into out; 0 when none
static int line_with(const char *text, const char *prefix, char *out, size_t n) {
    for (const char *l = text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        const char *at = strstr(l, prefix);
        const char *eol = strchr(l, '\n');
        if (at && (!eol || at < eol)) { snprintf(out, n, "%.*s", (int)(eol ? eol - at : (long)strlen(at)), at); return 1; }
    }
    out[0] = 0;
    return 0;
}
// The rows: guestbt packed by nxaot and run in each mode, through compiled code and (with the JIT built in) with every instruction in the JIT fallback (DM_AOT_JIT_ALL)
static void guestbt_rows(const char *self, const char *nxaot) {
    if (!nxaot || !*nxaot) { printf("%-14s %-9s note: no nxaot (--nxaot, NXAOT): guestbt is not packed here\n", "guestbt", "bt"); return; }
    static uint8_t img[GBT_SIZE];
    gbt_sites s;
    const size_t len = guestbt_nro(img, sizeof img, &s);
    char nro[200], pack[200], line[600] = "", note[1600];
    mkdir("t_aot_host.tmp", 0755);
    mkdir("t_aot_host.tmp/guestbt", 0755);
    snprintf(nro, sizeof nro, "t_aot_host.tmp/guestbt/guestbt.nro");
    snprintf(pack, sizeof pack, "t_aot_host.tmp/guestbt/guestbt%s", PACK_EXT);
    FILE *f = fopen(nro, "wb");
    const int wrote = len && f && fwrite(img, 1, len, f) == len;
    if (f) fclose(f);
    result r;
    memset(&r, 0, sizeof r);
    static char no_lines[1];
    r.guest = no_lines;
    if (!wrote || nxaot_pack(nxaot, nro, pack, line, sizeof line) != 0) {
        snprintf(note, sizeof note, "%s: %s", wrote ? "nxaot build failed" : "cannot write the NRO", line);
        row("guestbt", "bt", 0, &r, note);
        return;
    }
    // The return addresses: each call's offset + 4
    char ret_f1[48], ret_start[48], ret_blr[48], ret_kcall[48];
    snprintf(ret_f1, sizeof ret_f1, "guestbt+%#llx", (unsigned long long)(s.f1_call + 4));
    snprintf(ret_start, sizeof ret_start, "guestbt+%#llx", (unsigned long long)(s.start_call + 4));
    snprintf(ret_blr, sizeof ret_blr, "guestbt+%#llx", (unsigned long long)(s.f2_blr + 4));
    snprintf(ret_kcall, sizeof ret_kcall, "guestbt+%#llx", (unsigned long long)(s.k_call + 4));
    // Modes 1 to 3 through compiled code and (with the JIT built in) with every instruction in the JIT fallback; with the JIT, modes 4 to 12 (the engines mixed, compiled
    // code entered): 4 to 6 in h, run by the JIT and called by compiled f2 ("c>j"), 7 to 9 in compiled h, called by k in the JIT ("j>c"), 10 to 12 in compiled h, called
    // by the rest of f2, which compiled f2 handed to the JIT at a br ("br")
    char ret_jcall[48];
    snprintf(ret_jcall, sizeof ret_jcall, "guestbt+%#llx", (unsigned long long)(s.j_call + 4));
    for (int pass = 0; pass <= 2 * NXHOST_JIT; pass++) {
        const int jit = pass == 1, mixed = pass == 2;
        set_env("DM_AOT_JIT_ALL", jit ? "1" : NULL);
        for (int mode = mixed ? 4 : 1; mode <= (mixed ? 12 : 3); mode++) {
            char extra[64], want_head[300], want_bt[300], got_head[300], got_bt[300], ret_host[48], above[200], miss[200], tail[160] = "", *out = NULL;
            const int act = mode <= 3 ? mode : (mode - 4) % 3 + 1, shape = mode <= 3 ? 0 : mode <= 6 ? 1 : mode <= 9 ? 2 : 3;
            const uint64_t fn = shape ? s.h : s.f2, host = shape ? s.h_host : s.f2_host, svc_at = shape ? s.h_svc : s.f2_svc, fault_at = shape ? s.h_fault : s.f2_fault;
            const int jit_fault = jit || shape == 1;   // the fault is in a memory access of the JIT fallback (else in compiled code)
            snprintf(ret_host, sizeof ret_host, "guestbt+%#llx", (unsigned long long)(host + 4));
            // The frames above the function that aborts: in c>j f2's call through blr, in j>c k's call of h and f2's call of k, in br j's call of h (in f2's frame); then
            // f1's call of f2 and start's call of f1
            if (shape == 0) snprintf(above, sizeof above, "%s %s", ret_f1, ret_start);
            else if (shape == 1) snprintf(above, sizeof above, "%s %s %s", ret_blr, ret_f1, ret_start);
            else if (shape == 2) snprintf(above, sizeof above, "%s %s %s %s", ret_kcall, ret_blr, ret_f1, ret_start);
            else snprintf(above, sizeof above, "%s %s %s", ret_jcall, ret_f1, ret_start);
            // A mixed mode runs from h + 4, k + 4 or j in the JIT: its miss line names that address (else discovery gave it an entry, and the row would not mix the engines)
            miss[0] = 0;
            if (shape) snprintf(miss, sizeof miss, "at guestbt+%#llx runs in the JIT fallback", (unsigned long long)(shape == 1 ? s.h + 4 : shape == 2 ? s.k + 4 : s.j));
            snprintf(extra, sizeof extra, "--bt %d", mode);
            const int st = run_all(self, pack, nro, extra, &out);
            int want_st;
            const char *head;
            if (act == 1) {
                head = "game called nn::diag abort: ";
                want_st = DM_AOT_TRAP;
                snprintf(want_head, sizeof want_head, "game called nn::diag abort: Result 0, call site %s", ret_host);
                snprintf(want_bt, sizeof want_bt, "[dm] bt: %s %s", ret_host, above);
            } else if (act == 2) {
                head = "Break#";
                want_st = 42;
                snprintf(want_head, sizeof want_head, "Break#1 reason=0 Result=0 svc at guestbt+%#llx", (unsigned long long)svc_at);
                snprintf(want_bt, sizeof want_bt, "[dm] bt: %s", above);
            } else {
                head = "[RED] aot: SIGSEGV";
                want_st = DM_AOT_FAULT;
                if (jit_fault) {
                    // The function from the pack's function map (the guest range that holds the instruction), the instruction exact (the JIT steps it again with
                    // its access made: jit.cpp Retry), and the frames from the JIT's own registers at the access, the same chain as compiled code's native frames
                    snprintf(want_head, sizeof want_head, "[RED] aot: SIGSEGV at address %#llx in guest code (a memory access of the JIT fallback, in the function at guestbt+%#llx, "
                             "guest instruction guestbt+%#llx)", (unsigned long long)(DM_AOT_ARENA_BASE + 0x800), (unsigned long long)fn, (unsigned long long)fault_at);
                } else {
                    // The compiled fault's line names the function and the instruction after the host pc
                    snprintf(want_head, sizeof want_head, "[RED] aot: SIGSEGV at address %#llx in guest code (host pc ", (unsigned long long)(DM_AOT_ARENA_BASE + 0x800));
                    snprintf(tail, sizeof tail, ", in the function at guestbt+%#llx, guest instruction guestbt+%#llx)", (unsigned long long)fn, (unsigned long long)fault_at);
                }
                snprintf(want_bt, sizeof want_bt, "[dm] bt: %s", above);
            }
            line_with(out, head, got_head, sizeof got_head);
            line_with(out, "[dm] bt:", got_bt, sizeof got_bt);
            const int head_ok = tail[0] ? !strncmp(got_head, want_head, strlen(want_head)) && strstr(got_head, tail) != NULL : !strcmp(got_head, want_head);
            const int miss_ok = !miss[0] || strstr(out, miss) != NULL;
            const int ok = st == want_st && head_ok && !strcmp(got_bt, want_bt) && miss_ok;
            snprintf(note, sizeof note, "%s: \"%s\" / \"%s\"%s%s%s%s", act == 1 ? "host-call abort" : act == 2 ? "svcBreak" : "fault", got_head, got_bt,
                     ok ? "" : "; want \"", ok ? "" : want_head, ok ? "" : tail, ok ? "" : "\"");
            if (!ok) snprintf(note + strlen(note), sizeof note - strlen(note), " / \"%s\" with status %d%s%s", want_bt, want_st, miss_ok ? "" : "; no JIT miss line: ", miss_ok ? "" : miss);
            r.status = st;
            static const char *const names[4][2][3] = {{{"abort", "break", "fault"}, {"abort-jit", "break-jit", "fault-jit"}},
                                                       {{"abort-c>j", "break-c>j", "fault-c>j"}}, {{"abort-j>c", "break-j>c", "fault-j>c"}},
                                                       {{"abort-br", "break-br", "fault-br"}}};
            row("guestbt", names[shape][jit][act - 1], ok, &r, note);
            free(out);
        }
    }
    set_env("DM_AOT_JIT_ALL", NULL);
}

// ---------- Safepoint polls left out of loops bounded by a constant (dm/tools/aot/README.md, "Threads and safepoints"; design 6.2) ----------
// The NRO polls, written here word by word: start runs five shapes in turn, forever; nothing in it ends the process. The two whose polls nxaot leaves out
// (at tier H, where it leaves out the polls of loops bounded by a constant): (0) a loop of 2^17 passes around an inner loop of 100 passes (the inner loop's
// poll left out, the outer's kept), (1) a loop of 2^18 calls of a leaf whose own loop of 50 passes has no poll (the calling loop keeps its own, and the leaf,
// which calls nothing, has none at its entry). Three that look bounded and are not, which must keep theirs, each with a store to guest memory in every pass
// (volatile, so that no pass of LLVM's computes the loop away once its poll is gone) and some 2^28 passes, a quarter of a second or more: (2) a loop bounded by a
// register (loaded from data: 2^28), (3) a loop whose counter goes up on one path through its body and back down on the other (up once every 65,536 passes, to
// 4,095), (4) a loop with two back edges, one of which leaves its counter as it was (up once every 2^20 passes, to 256). The child (--latency N): a guest thread
// whose entry is a host function (the pause tester's) pauses the main thread at random intervals of 10 to 100 us until every shape has been stopped in N times
// (or 20,000 pauses, or 30 s), and for each pause measures the main thread's CPU time from the request to its stop (the code it ran before it reached a
// safepoint: what a missing poll would make long, and what the machine's load changes little: a vCPU the hypervisor takes away still counts, a few
// milliseconds at worst here) and the wall time; the stop's pc (where it polled) names the shape (the leaf's code counts for shape 1); then svcExitProcess,
// which the main thread obeys at its next poll. A loop that lost its poll is never stopped in (its pauses wait for its end, and stop in the next shape after up
// to a quarter of a second). The parent packs the NRO at tier H twice, with the polls left out and with NXAOT_POLL_ELISION=0 (every poll, the old pack), checks
// nxaot's poll counts (exactly the two loops above left out), and runs each: every shape stopped in LAT_MIN times, no inconsistent context, the worst CPU time
// from request to stop under LAT_BOUND_US, in both packs (the median is printed)
enum { LAT_SHAPES = 5, LAT_MIN = 100, LAT_MAX_PAUSES = 20000, LAT_BOUND_US = 50000 };
static const char *const LAT_NAMES[LAT_SHAPES] = {"bounded-inner", "calls", "register-bound", "counter-in-body", "two-back-edges"};
static struct {
    int on; unsigned min;
    uint64_t lo[LAT_SHAPES + 3];   // shape i's code: guest addresses [lo[i], lo[i + 1]); the leaf's [lo[LAT_SHAPES + 1], lo[LAT_SHAPES + 2]), shape 1's
    unsigned n[LAT_SHAPES], pauses, elsewhere, inconsistent;
    uint64_t cpu_max[LAT_SHAPES], wall_max[LAT_SHAPES];
    uint32_t cpu_us[LAT_SHAPES][LAT_MAX_PAUSES];   // each stop's CPU time from request to stop, in microseconds (the medians printed)
    char bad[200];
#if !defined(_WIN32)
    clockid_t clk; int have_clk;   // the main guest thread's CPU clock
#endif
} g_lat;
static int lat_on(void) { return g_lat.on; }
static void lat_option(unsigned min_per_shape) { g_lat.on = 1; g_lat.min = min_per_shape ? min_per_shape : 1; }

static uint32_t a_movzw(unsigned rd, unsigned imm) { return 0x52800000u | imm << 5 | rd; }
static uint32_t a_movz16(unsigned rd, unsigned imm) { return 0xD2A00000u | imm << 5 | rd; }                          // movz xd, #imm, lsl #16
static uint32_t a_addwi(unsigned rd, unsigned rn, unsigned imm) { return 0x11000000u | imm << 10 | rn << 5 | rd; }
static uint32_t a_subwi(unsigned rd, unsigned rn, unsigned imm) { return 0x51000000u | imm << 10 | rn << 5 | rd; }
static uint32_t a_addr(unsigned rd, unsigned rn, unsigned rm, unsigned lsl) { return 0x8B000000u | rm << 16 | lsl << 10 | rn << 5 | rd; }
static uint32_t a_eor(unsigned rd, unsigned rn, unsigned rm) { return 0xCA000000u | rm << 16 | rn << 5 | rd; }
static uint32_t a_cmpwi(unsigned rn, unsigned imm) { return 0x7100001Fu | imm << 10 | rn << 5; }
static uint32_t a_cmpi(unsigned rn, unsigned imm) { return 0xF100001Fu | imm << 10 | rn << 5; }
static uint32_t a_cmp(unsigned rn, unsigned rm) { return 0xEB00001Fu | rm << 16 | rn << 5; }
static uint32_t a_tst_low(unsigned rn, unsigned ones) { return 0xF240001Fu | (ones - 1) << 10 | rn << 5; }          // tst xn, #(2^ones - 1)
static uint32_t a_mov(unsigned rd, unsigned rm) { return 0xAA0003E0u | rm << 16 | rd; }
static const uint32_t A_STR_X10_X28_8 = 0xF900078Au;   // str x10, [x28, #8]
enum { A_EQ = 0, A_NE = 1, A_LO = 3 };

// The NRO (layout as lrtail_nro's: text from 0, read-only data at 0x1000, data at 0x2000 holding shape 2's bound and the word shapes 2 to 4 store to, a page
// of bss); lo[] gets the shapes' module offsets, lo[LAT_SHAPES] the end of the last, then the leaf's start and end
static size_t polls_nro(uint8_t *img, size_t cap, uint64_t lo[LAT_SHAPES + 3]) {
    enum { RO = 0x1000, DATA = 0x2000, SIZE = 0x3000, BSS = 0x1000 };
    if (cap < SIZE) return 0;
    memset(img, 0, SIZE);
    uint32_t w[256] = {0};
    size_t n = 0x80 / 4;
    const size_t start = n;
    w[n] = a_adr(28, (int64_t)DATA - (int64_t)n * 4); n++;
    w[n++] = 0xF9400382u;               // ldr x2, [x28]: shape 2's bound
    const size_t outer = n;
    // 0: an outer loop of 2^17 passes around an inner loop of 100
    lo[0] = n * 4;
    w[n++] = a_movz(12, 0);
    const size_t a_outer = n;
    w[n++] = a_movzw(9, 0);
    const size_t a_inner = n;
    w[n++] = a_eor(10, 10, 9);
    w[n++] = a_addr(10, 10, 10, 1);     // x10 = 3 * x10
    w[n++] = a_addwi(9, 9, 1);
    w[n++] = a_cmpwi(9, 100);
    w[n] = a_bcond(A_LO, (int64_t)(a_inner - n) * 4); n++;
    w[n++] = a_addi(12, 12, 1);
    w[n++] = a_movz16(13, 2);           // 2^17
    w[n++] = a_cmp(12, 13);
    w[n] = a_bcond(A_LO, (int64_t)(a_outer - n) * 4); n++;
    // 1: a loop of 2^18 calls of leaf
    lo[1] = n * 4;
    w[n++] = a_movz(12, 0);
    const size_t b_loop = n;
    w[n++] = a_mov(0, 10);
    const size_t call_leaf = n++;       // bl leaf
    w[n++] = a_addr(10, 10, 0, 0);
    w[n++] = a_addi(12, 12, 1);
    w[n++] = a_movz16(13, 4);           // 2^18
    w[n++] = a_cmp(12, 13);
    w[n] = a_bcond(A_LO, (int64_t)(b_loop - n) * 4); n++;
    // 2: bounded by a register
    lo[2] = n * 4;
    w[n++] = a_movz(9, 0);
    const size_t c1 = n;
    w[n++] = a_addr(10, 10, 9, 0);
    w[n++] = A_STR_X10_X28_8;
    w[n++] = a_addi(9, 9, 1);
    w[n++] = a_cmp(9, 2);
    w[n] = a_bcond(A_LO, (int64_t)(c1 - n) * 4); n++;
    // 3: the counter goes up, and on most passes back down
    lo[3] = n * 4;
    w[n++] = a_movzw(9, 0);
    w[n++] = a_movz(11, 0);
    const size_t c2 = n;
    w[n++] = a_addi(11, 11, 1);
    w[n++] = a_addwi(9, 9, 1);
    w[n++] = A_STR_X10_X28_8;
    w[n++] = a_tst_low(11, 16);
    const size_t to_keep = n++;         // b.eq keep
    w[n++] = a_subwi(9, 9, 1);
    w[to_keep] = a_bcond(A_EQ, (int64_t)(n - to_keep) * 4);
    w[n++] = a_cmpwi(9, 4095);
    w[n] = a_bcond(A_LO, (int64_t)(c2 - n) * 4); n++;
    // 4: two back edges, the second leaving the counter as it was
    lo[4] = n * 4;
    w[n++] = a_movz(9, 0);
    w[n++] = a_movz(11, 0);
    const size_t c3 = n;
    w[n++] = a_addi(11, 11, 1);
    w[n++] = A_STR_X10_X28_8;
    w[n++] = a_tst_low(11, 20);
    const size_t to_down = n++;         // b.ne down
    w[n++] = a_addi(9, 9, 1);
    w[n++] = a_cmpi(9, 256);
    w[n] = a_bcond(A_LO, (int64_t)(c3 - n) * 4); n++;   // the first back edge
    const size_t to_done = n++;         // b done
    w[to_down] = a_bcond(A_NE, (int64_t)(n - to_down) * 4);
    w[n++] = a_eor(10, 10, 11);
    w[n] = a_b((int64_t)(c3 - n) * 4); n++;             // the second
    w[to_done] = a_b((int64_t)(n - to_done) * 4);
    lo[5] = n * 4;
    w[n] = a_b((int64_t)(outer - n) * 4); n++;
    // leaf(x0): 50 passes, no call
    const size_t leaf = n;
    lo[6] = n * 4;
    w[call_leaf] = a_bl((int64_t)(leaf - call_leaf) * 4);
    w[n++] = a_movzw(9, 0);
    w[n++] = a_movz(1, 0);
    const size_t l_loop = n;
    w[n++] = a_addr(1, 1, 0, 0);
    w[n++] = a_eor(1, 1, 9);
    w[n++] = a_addwi(9, 9, 1);
    w[n++] = a_cmpwi(9, 50);
    w[n] = a_bcond(A_LO, (int64_t)(l_loop - n) * 4); n++;
    w[n++] = a_mov(0, 1);
    w[n++] = 0xD65F03C0u;               // ret
    lo[7] = n * 4;
    w[0] = a_b((int64_t)start * 4);
    w[1] = RO;                          // MOD0's offset
    memcpy(img, w, n * 4);
    memcpy(img + 0x10, "NRO0", 4);
    const uint32_t hdr[] = {0, SIZE, 0, 0, 0x1000, RO, DATA - RO, DATA, SIZE - DATA, BSS};
    memcpy(img + 0x14, hdr, sizeof hdr);
    memcpy(img + RO, "MOD0", 4);
    const int32_t mod0[] = {0x20, SIZE - RO, SIZE + BSS - RO, 0x40, 0x40, SIZE - RO};
    memcpy(img + RO + 4, mod0, sizeof mod0);
    const uint64_t bound = 1ull << 28;
    memcpy(img + DATA, &bound, 8);
    return SIZE;
}

// The child's side: the shapes' guest addresses, the main thread's CPU clock (this runs on it), then the tester as the pause test starts its own
static int lat_start(const module_t *m) {
    static uint8_t img[0x3000];
    uint64_t lo[LAT_SHAPES + 3];
    if (!polls_nro(img, sizeof img, lo)) return 0;
    for (int i = 0; i < LAT_SHAPES + 3; i++) g_lat.lo[i] = (uint64_t)(uintptr_t)m->base + lo[i];
#if !defined(_WIN32)
    g_lat.have_clk = pthread_getcpuclockid(pthread_self(), &g_lat.clk) == 0;
#endif
    g_pt.text_lo = (uint64_t)(uintptr_t)m->base; g_pt.text_hi = g_pt.text_lo + 0x1000;
    const uint64_t stack = (uint64_t)(uintptr_t)dm_guest_alloc(0x10000);
    uint64_t r[32] = {0, dm_guest_fn((const void *)t_aot_pause_tester), 0, stack + 0x10000, 44, (uint64_t)-2};
    svc_dispatch(0x08, r, 0);
    if (r[0]) { printf("[RED] t_aot_host: svcCreateThread for the polls tester: %#llx\n", (unsigned long long)r[0]); return 0; }
    uint64_t s[32] = {r[1]};
    svc_dispatch(0x09, s, 0);
    return 1;
}
static uint64_t lat_cpu_ns(void) {
#if !defined(_WIN32)
    struct timespec ts;
    if (g_lat.have_clk && !clock_gettime(g_lat.clk, &ts)) return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
    return plat_now_ns();   // no thread CPU clock: the wall clock (an upper bound)
}
static int lat_u32_cmp(const void *a, const void *b) { const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
static unsigned lat_median(int shape) {
    const unsigned n = g_lat.n[shape];
    if (!n) return 0;
    qsort(g_lat.cpu_us[shape], n, sizeof g_lat.cpu_us[shape][0], lat_u32_cmp);
    return g_lat.cpu_us[shape][n / 2];
}
static void lat_body(void) {
    const uint32_t self = thr_cur_handle();
    uint64_t seed = 0x2545F4914F6CDD1Dull;
    const uint64_t give_up = plat_now_ns() + 30000000000ull;
    for (;;) {
        int done = g_lat.pauses >= LAT_MAX_PAUSES || plat_now_ns() > give_up || dm_aot_stop_status() >= 0;
        int all = 1;
        for (int i = 0; i < LAT_SHAPES; i++) all &= g_lat.n[i] >= g_lat.min;
        if (done || all) break;
        plat_sleep_ns((10 + rng(&seed) % 91) * 1000);
        uint32_t hs[8];
        if (thr_handles(hs, 8, self) < 1) continue;
        const uint64_t c0 = lat_cpu_ns(), w0 = plat_now_ns();
        const int paused = svc(0x32, hs[0], 1) == 0;   // returns once the thread stopped
        const uint64_t w1 = plat_now_ns(), c1 = lat_cpu_ns();
        if (!paused) continue;
        g_lat.pauses++;
        hz_ctx c;
        memset(&c, 0, sizeof c);
        const char *why = NULL;
        if (svc(0x33, (uint64_t)(uintptr_t)&c, hs[0])) why = "GetThreadContext3 failed";
        else if (c.pc < g_pt.text_lo || c.pc >= g_pt.text_hi || (c.pc & 3)) why = "pc is no guest instruction";
        else if (!dm_aot_in_arena(c.sp, 1) || (c.sp & 15)) why = "sp is no 16-byte aligned guest stack address";
        if (why) {
            if (!g_lat.inconsistent++) snprintf(g_lat.bad, sizeof g_lat.bad, "%s (pc %#llx sp %#llx)", why, (unsigned long long)c.pc, (unsigned long long)c.sp);
        } else {
            int shape = -1;
            for (int i = 0; i < LAT_SHAPES; i++) if (c.pc >= g_lat.lo[i] && c.pc < g_lat.lo[i + 1]) shape = i;
            if (c.pc >= g_lat.lo[LAT_SHAPES + 1] && c.pc < g_lat.lo[LAT_SHAPES + 2]) shape = 1;   // the leaf's loop (with every poll)
            if (shape < 0) g_lat.elsewhere++;
            else {
                g_lat.n[shape]++;
                if (c1 - c0 > g_lat.cpu_max[shape]) g_lat.cpu_max[shape] = c1 - c0;
                const uint64_t us = (c1 - c0) / 1000;
                g_lat.cpu_us[shape][g_lat.n[shape] - 1] = us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
                if (w1 - w0 > g_lat.wall_max[shape]) g_lat.wall_max[shape] = w1 - w0;
            }
        }
        svc(0x32, hs[0], 0);   // resume
    }
    for (int i = 0; i < LAT_SHAPES; i++)
        printf("t_aot_host: latency %s: %u stops, median %u us, worst %.1f us of its CPU time, %.1f us wall\n", LAT_NAMES[i], g_lat.n[i], lat_median(i),
               (double)g_lat.cpu_max[i] * 1e-3,
               (double)g_lat.wall_max[i] * 1e-3);
    printf("t_aot_host: latency: %u pauses, %u elsewhere, %u inconsistent%s%s\n", g_lat.pauses, g_lat.elsewhere, g_lat.inconsistent, g_lat.inconsistent ? "; first: " : "",
           g_lat.bad);
    fflush(stdout);
    dm_aot_svc(dm_aot_self_state(), 0x07);   // svcExitProcess: the main thread leaves at its next poll
    dm_aot_svc(dm_aot_self_state(), 0x0A);   // and this thread here
}

// nxaot build <nro> --shared <out> --tier H, with NXAOT_POLL_ELISION=0 when every poll is wanted: its exit status, its `aot: safepoint polls:` line into line
static int nxaot_polls_build(const char *nxaot, const char *nro, const char *out, int every, char *line, size_t n) {
    char cmd[4096], buf[4096];
#if defined(_WIN32)
    snprintf(cmd, sizeof cmd, "\"\"%s\" build \"%s\" --shared \"%s\" --tier H 2>&1\"", nxaot, nro, out);
#else
    snprintf(cmd, sizeof cmd, "'%s' build '%s' --shared '%s' --tier H 2>&1", nxaot, nro, out);
#endif
    set_env("NXAOT_POLL_ELISION", every ? "0" : NULL);
    line[0] = 0;
    FILE *f = popen(cmd, "r");
    if (!f) { set_env("NXAOT_POLL_ELISION", NULL); return -1; }
    while (fgets(buf, sizeof buf, f)) if (!strncmp(buf, "aot: safepoint polls:", 21)) { snprintf(line, n, "%s", buf); line[strcspn(line, "\n")] = 0; }
    const int st = pclose(f);
    set_env("NXAOT_POLL_ELISION", NULL);
    return st;
}

// The rows: the NRO packed with the polls left out and with every poll, the counts, and each pack's run under the tester
static void polls_rows(const char *self, const char *nxaot) {
    if (!nxaot || !*nxaot) { printf("%-14s %-9s note: no nxaot (--nxaot, NXAOT): polls is not packed here\n", "polls", "latency"); return; }
    static uint8_t img[0x3000];
    uint64_t lo[LAT_SHAPES + 3];
    const size_t len = polls_nro(img, sizeof img, lo);
    char nro[200];
    mkdir("t_aot_host.tmp", 0755);
    mkdir("t_aot_host.tmp/polls", 0755);
    snprintf(nro, sizeof nro, "t_aot_host.tmp/polls/polls.nro");
    FILE *f = fopen(nro, "wb");
    const int wrote = len && f && fwrite(img, 1, len, f) == len;
    if (f) fclose(f);
    unsigned long long total[2] = {0}, left_bounded[2] = {0};
    for (int every = 0; every < 2; every++) {
        // One file name in two folders: the name is the pack's soname, part of its bytes
        char dir[200], pack[300], line[1000] = "", note[2400], buf[1000];
        snprintf(dir, sizeof dir, "t_aot_host.tmp/polls/%s", every ? "every" : "elided");
        mkdir(dir, 0755);
        snprintf(pack, sizeof pack, "%s/polls%s", dir, PACK_EXT);
        result r;
        memset(&r, 0, sizeof r);
        static char no_lines[1];
        r.guest = no_lines;
        const int built = wrote && nxaot_polls_build(nxaot, nro, pack, every, line, sizeof line) == 0;
        unsigned long long entries = 0, kept = 0;
        const int counted = built && sscanf(line, "aot: safepoint polls: %llu at function entries, %llu at backward branches of %llu (%llu left out closing",
                                            &entries, &kept, &total[every], &left_bounded[every]) == 4;
        // Exactly the two bounded loops' polls left out (the other three shapes' and the calling loops' kept); every poll in the old pack
        const int counts_ok = counted && left_bounded[every] == (every ? 0u : 2u) && kept + left_bounded[every] == total[every] && (!every || total[1] == total[0]);
        if (!counts_ok) {
            snprintf(note, sizeof note, "%s: %s", built ? "the poll counts are not the expected ones (want 2 left out in loops bounded by a constant; 0 with every poll)"
                                                        : wrote ? "nxaot build failed" : "cannot write the NRO", line[0] ? line : "no safepoint polls line");
            row("polls", every ? "every" : "elided", 0, &r, note);
            continue;
        }
        // The run: the child's own lines (its latency lines, its status line, a [RED] line)
        char cmd[1200];
#if defined(_WIN32)
        snprintf(cmd, sizeof cmd, "\"\"%s\" --run \"%s\" \"%s\" --latency %d 2>&1\"", self, pack, nro, LAT_MIN);
#else
        snprintf(cmd, sizeof cmd, "'%s' --run '%s' '%s' --latency %d 2>&1", self, pack, nro, LAT_MIN);
#endif
        unsigned n[LAT_SHAPES] = {0}, pauses = 0, elsewhere = 0, inconsistent = 1;
        double cpu[LAT_SHAPES] = {0}, wall[LAT_SHAPES] = {0};
        unsigned median[LAT_SHAPES] = {0};
        int have_sum = 0, have[LAT_SHAPES] = {0};
        FILE *p = popen(cmd, "r");
        while (p && fgets(buf, sizeof buf, p)) {
            for (int i = 0; i < LAT_SHAPES; i++) {
                char head[80];
                snprintf(head, sizeof head, "t_aot_host: latency %s: ", LAT_NAMES[i]);
                if (!strncmp(buf, head, strlen(head)))
                    have[i] = sscanf(buf + strlen(head), "%u stops, median %u us, worst %lf us of its CPU time, %lf us wall", &n[i], &median[i], &cpu[i], &wall[i]) == 4;
            }
            if (!strncmp(buf, "t_aot_host: latency: ", 21)) have_sum = sscanf(buf, "t_aot_host: latency: %u pauses, %u elsewhere, %u inconsistent", &pauses, &elsewhere, &inconsistent) == 3;
            if (!strncmp(buf, "t_aot_host: status ", 19)) sscanf(buf, "t_aot_host: status %*d after %lf s", &r.secs);
            if (strstr(buf, "[RED]") && !r.red[0]) { snprintf(r.red, sizeof r.red, "%s", strstr(buf, "[RED]")); r.red[strcspn(r.red, "\n")] = 0; }
        }
        const int st = p ? pclose(p) : -1;
#if defined(_WIN32)
        r.status = st;
#else
        r.status = st >= 0 && WIFEXITED(st) ? WEXITSTATUS(st) : 128;
#endif
        int ok = r.status == 0 && have_sum && inconsistent == 0;
        double worst_cpu = 0, worst_wall = 0;
        size_t o = (size_t)snprintf(note, sizeof note, "%s; %u pauses", line + 5, pauses);
        for (int i = 0; i < LAT_SHAPES; i++) {
            ok = ok && have[i] && n[i] >= LAT_MIN && cpu[i] < LAT_BOUND_US;
            if (cpu[i] > worst_cpu) worst_cpu = cpu[i];
            if (wall[i] > worst_wall) worst_wall = wall[i];
            if (o < sizeof note) o += (size_t)snprintf(note + o, sizeof note - o, "; %s: %u stops, median %u us CPU, worst %.1f us CPU, %.1f us wall", LAT_NAMES[i], n[i], median[i], cpu[i], wall[i]);
        }
        if (o < sizeof note)
            snprintf(note + o, sizeof note - o, "; worst %.1f us of the thread's CPU time from request to stop (bound %d us), %.1f us wall; %u inconsistent, %u elsewhere%s%s",
                     worst_cpu, LAT_BOUND_US, worst_wall, inconsistent, elsewhere, r.red[0] ? "; " : "", r.red);
        row("polls", every ? "every" : "elided", ok, &r, note);
    }
}

// int_ops under the sampler: the child's run and checks (--positions), the profile's rows of the hot function, and with nxaot the round trip (the header's list)
static void positions_rows(const char *self, const char *name, const char *pack, const char *nrop, const char *corpus, const char *expected, const char *nxaot) {
    char dir[300], prof[400] = "", entries[400] = "", module[64] = "", note[2000];
    mkdir("t_aot_host.tmp", 0755);
    snprintf(dir, sizeof dir, "t_aot_host.tmp/%s", name);
    mkdir(dir, 0755);
    {   // an earlier run's files go first (the module's name as guest_main makes it)
        char cut[16], old[400];
        snprintf(cut, sizeof cut, "%s.nro", name);
        if (strstr(cut, ".nro")) *strstr(cut, ".nro") = 0;
        snprintf(old, sizeof old, "%s/%s.tsv", dir, cut); remove(old);
        snprintf(old, sizeof old, "%s/%s.entries.tsv", dir, cut); remove(old);
    }
    set_env("DM_AOT_PROFILE", dir);
    result r;
    run_child(self, pack, nrop, "--positions --set int_ops_rounds=" POS_ROUNDS, &r);
    set_env("DM_AOT_PROFILE", NULL);
    // The profile's file and module as the child named them (the module's name is the NRO's file name, cut to 15 characters)
    const char *pm = strstr(r.pos, "; module ");
    if (pm) sscanf(pm, "; module %63[^,], profile %399s", module, prof);
    if (prof[0] && strlen(prof) > 4) snprintf(entries, sizeof entries, "%.*s.entries.tsv", (int)(strlen(prof) - 4), prof);
    char *want = without_digests(expected), *got = without_digests(r.guest);
    const int run_ok = r.status == 0 && !strcmp(want, got);
    free(want); free(got);
    // The child's walk of the code and its check of the profile against its counts
    unsigned long long bytes = 0, nfns = 0, placed = 0, none = 0, crossed = 1, mism = 1;
    size_t nrows = 0, sampled = 0, kept = 0;
    const int walked = r.pos[0] && sscanf(r.pos, "t_aot_host: positions: %llu code bytes of %llu functions: %llu at an instruction, %llu at none, %llu at one outside their function, "
                                                 "%llu read otherwise by the host; profile: %zu rows, %zu sampled functions, %zu with a row inside them", &bytes, &nfns, &placed, &none,
                                          &crossed, &mism, &nrows, &sampled, &kept) == 9;
    // The function lines: one per sampled function, each with the sampler's samples and its rows' count, every row after one and inside its function
    size_t flines = 0, fl_sampled = 1, rows_in = 0, fl_rows = 1, fl_good = 0;
    const int lined = r.fl[0] && sscanf(r.fl, "t_aot_host: function lines: %zu for %zu sampled functions, %zu of %zu rows after one and inside its function, %zu with the sampler's "
                                              "samples and their rows' count", &flines, &fl_sampled, &rows_in, &fl_rows, &fl_good) == 5;
    const int lines_ok = lined && flines > 0 && flines == fl_sampled && fl_good == flines && rows_in == fl_rows && fl_rows == nrows;
    unsigned long long all = 0, code = 0, at_ins = 0, outside = 1;
    const char *am = strstr(r.prof, "all modules: ");
    const int summed = am && sscanf(am, "all modules: %llu samples, %llu in compiled code (%llu at a guest instruction, %llu at one outside their function", &all, &code, &at_ins,
                                    &outside) == 4;
    // The profile: its header, its rows; the samples per function of the corpus's symbols, ref_crc's inside its loops and at its entry
    char *text = prof[0] ? read_text(prof) : NULL;
    char head[96];
    snprintf(head, sizeof head, "# module %s, build ID ", module);
    int header = 0, bad_lines = 0;
    sym_fn fns[256];
    const int nf = corpus_functions(corpus, name, fns, 256);
    int crc = -1;
    for (int i = 0; i < nf; i++) if (!strcmp(fns[i].name, "ref_crc")) crc = i;
    unsigned long long per[256] = {0}, total = 0, in_loop = 0, at_entry = 0;
    uint64_t llo = 0, lhi = 0;
    size_t img_len = 0;
    uint8_t *img = slurp(nrop, &img_len);
    const int loops = crc >= 0 && img && loop_range(img, img_len, fns[crc].lo, fns[crc].hi, &llo, &lhi);
    free(img);
    int offsets_in = 0;
    struct { unsigned long long off, n; } top[6] = {{0, 0}};
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (line[0] == '#') { header += !strncmp(line, head, strlen(head)); continue; }
        unsigned long long off = 0, n = 0; char extra = 0;
        if (sscanf(line, "%llx %llu %c", &off, &n, &extra) != 2 || strncmp(line, "0x", 2)) { bad_lines++; continue; }
        total += n;
        for (int i = 0; i < nf; i++) if (off >= fns[i].lo && off < fns[i].hi) per[i] += n;
        if (crc >= 0 && off >= fns[crc].lo && off < fns[crc].hi) {
            offsets_in++;
            at_entry += off == fns[crc].lo ? n : 0;
            in_loop += loops && off >= llo && off <= lhi ? n : 0;
            for (int k = 0; k < 6; k++) if (n > top[k].n) { memmove(&top[k + 1], &top[k], (size_t)(5 - k) * sizeof top[0]); top[k].off = off; top[k].n = n; break; }
        }
    }
    free(text);
    int hottest = -1;
    for (int i = 0; i < nf; i++) if (hottest < 0 || per[i] > per[hottest]) hottest = i;
    const unsigned long long in_fn = crc >= 0 ? per[crc] : 0;
    const int ok = run_ok && walked && summed && crossed == 0 && mism == 0 && placed > 0 && outside == 0 && at_ins > 0 && sampled > 0 && kept == sampled && header == 1 && !bad_lines &&
                   nrows > 0 && crc >= 0 && hottest == crc && loops && in_loop * 2 > in_fn && (in_fn - at_entry) * 2 > in_fn && offsets_in >= 3 && lines_ok;
    snprintf(note, sizeof note, "%zu rows, %llu samples (%llu of %llu in compiled code at an instruction); ref_crc [%#llx, %#llx) %llu: %llu in its loops [%#llx, %#llx], %llu at its entry, "
             "%d offsets, the hottest of %d functions%s; %llu code bytes of %llu functions: %llu at an instruction, %llu at none, %llu outside their function (%llu read otherwise "
             "by the host); %zu of %zu sampled functions with a row inside them; function lines: %zu for %zu sampled functions, %zu of %zu rows inside the function of the line "
             "before them, %zu lines with the sampler's samples and their rows' count%s%s", nrows, total, at_ins, code, crc >= 0 ? (unsigned long long)fns[crc].lo : 0ull,
             crc >= 0 ? (unsigned long long)fns[crc].hi : 0ull, in_fn, in_loop, (unsigned long long)llo, (unsigned long long)lhi, at_entry, offsets_in, nf,
             hottest == crc ? "" : " (not the hottest)", bytes, nfns, placed, none, crossed, mism, kept, sampled, flines, fl_sampled, rows_in, fl_rows, fl_good,
             run_ok ? "" : "; the run's lines or status differ", r.red[0] ? "; " : "");
    if (r.red[0]) strncat(note, r.red, sizeof note - strlen(note) - 1);
    row(name, "positions", ok, &r, note);
    printf("%-14s %-9s      ref_crc's rows with the most samples:", "", "");
    for (int k = 0; k < 6 && top[k].n; k++) printf(" 0x%llx %llu;", top[k].off, top[k].n);
    printf("\n");
    free(r.guest);
    // The round trip through nxaot (the aot image): weighed functions, and the weights off giving the entry form's pack (the tiers alone)
    if (!nxaot || !*nxaot) {
        printf("%-14s %-9s note: no nxaot (--nxaot, NXAOT): the profile's round trip through nxaot build --profile is not checked here\n", name, "roundtrip");
        return;
    }
    char w_so[400], off_so[400], e_so[400], wl[400], ol[400], el[400];   // one file name in three folders: the name is the pack's soname, part of its bytes
    snprintf(w_so, sizeof w_so, "%s/weighted", dir); mkdir(w_so, 0755); snprintf(w_so, sizeof w_so, "%s/weighted/x%s", dir, PACK_EXT);
    snprintf(off_so, sizeof off_so, "%s/off", dir); mkdir(off_so, 0755); snprintf(off_so, sizeof off_so, "%s/off/x%s", dir, PACK_EXT);
    snprintf(e_so, sizeof e_so, "%s/entries", dir); mkdir(e_so, 0755); snprintf(e_so, sizeof e_so, "%s/entries/x%s", dir, PACK_EXT);
    const int s1 = nxaot_build(nxaot, nrop, w_so, prof, 0, wl, sizeof wl), s2 = nxaot_build(nxaot, nrop, off_so, prof, 1, ol, sizeof ol),
              s3 = nxaot_build(nxaot, nrop, e_so, entries, 0, el, sizeof el);
    unsigned long long weighed = 0, entry_only = 0;
    sscanf(wl, "aot: profile weights: %llu functions", &weighed);
    const char *eo = strstr(el, "; ");
    if (eo) sscanf(eo, "; %llu functions sampled at their entry only", &entry_only);
    static char no_lines[1];
    result r2;
    memset(&r2, 0, sizeof r2);
    r2.guest = no_lines;
    r2.status = s1 | s2 | s3;
    const int same_off = same_file(off_so, e_so), differs = !same_file(w_so, off_so);
    snprintf(note, sizeof note, "%s; NXAOT_PROFILE_WEIGHTS=0: %s, the entry form's pack %s; the weighted pack %s it (%s)", wl[0] ? wl : "no profile weights line", ol[0] ? "off" : "no line",
             same_off ? "the same bytes" : "differs", differs ? "differs from" : "is the same as", el[0] ? el : "no line for the entry form");
    row(name, "roundtrip", !s1 && !s2 && !s3 && weighed > 0 && strstr(ol, "off (NXAOT_PROFILE_WEIGHTS=0)") && same_off && differs && entry_only > 0, &r2, note);
}

// ---------- A call whose return address starts another function (dm/tools/aot/README.md, "A call into the next function") ----------
// The NRO cutcall, written here word by word: start adds F(0) and F(1). F(s) keeps a counter x19 = s .. CUT_N - 1 and a sum in x20 across a loop whose case comes
// from a jump table discovery cannot bound (a byte of a table in RO, indexed by x19 and added to an adr: no compare bounds it). Case 0 calls g, and its return address
// is case 1's label: case 0 adds i + 7, case 1 adds i (x0 is 0). g keeps x19 in guest terms but is open (it calls through a PLT stub's shape whose union moves SP:
// spkeep's shape) and its svc stores its own x19, 0xBAD, in the block. Case 0 is gap code discovery compiles as a function (no walk reaches it past the br); the
// host's misses file of a run of the pack without misses names case 1 and the loop's head (indirect), whose entries cut F and that function: case 0's function
// ends with the call of g, whose return is case 1's entry. Without the continuation of that
// call (discover.cpp Finish, Function::out) the fragment had no return path (summarised noreturn) and its call site gave g's LIVE_AFTER nothing: g returned without
// storing the x19 it restored, the fragment reloaded g's 0xBAD from the block and the JIT ran case 1 from there with it (the sum wrong). Mario vs. Donkey Kong's
// second pack: main+0x2ee9b4's call of main+0x24397c, whose return is main+0x2ee9c4's entry (x1 = 0x100000000 two calls later)
enum { CUT_N = 8 };
static const uint8_t cut_idx[CUT_N] = {0, 1, 0, 1, 0, 1, 0, 1};
static size_t cutcall_nro(uint8_t *img, size_t cap, uint64_t *want, uint64_t *c0, uint64_t *c1) {
    enum { TEXT = 0x1000, RO = 0x1000, DATA = 0x2000, SIZE = 0x3000, BSS = 0x1000, PASS_AT = RO + 0x100, FAIL_AT = RO + 0x120, IDX_AT = RO + 0x140 };
    if (cap < SIZE) return 0;
    memset(img, 0, SIZE);
    uint32_t w[256] = {0};
    size_t n = 0x80 / 4;
    const size_t start = n;
    w[n++] = 0xA9BE7BFDu;               // stp x29, x30, [sp, #-32]!
    w[n++] = 0x910003FDu;               // mov x29, sp
    w[n++] = 0xA90153F3u;               // stp x19, x20, [sp, #16]
    w[n] = a_adr(10, (int64_t)DATA - (int64_t)n * 4); n++;   // adr x10, DATA
    const size_t adr_leaf = n++;        // adr x9, leaf (the stub's pointer)
    w[n++] = 0xF9000149u;               // str x9, [x10]
    const size_t adr_spfn = n++;        // adr x11, spfn (address-taken: the union moves SP)
    w[n++] = 0xF900054Bu;               // str x11, [x10, #8]
    w[n++] = a_movz(0, 0);
    const size_t call_f0 = n++;         // bl F
    w[n++] = a_mov(19, 0);
    w[n++] = a_movz(0, 1);
    const size_t call_f1 = n++;         // bl F
    w[n++] = a_addr(20, 19, 0, 0);      // add x20, x19, x0
    uint64_t sum = 0;
    for (unsigned s0 = 0; s0 < 2; s0++)
        for (unsigned i = s0; i < CUT_N; i++) sum += i + (cut_idx[i] == 0 ? 7 : 0);
    *want = sum;
    w[n++] = a_movz(9, (unsigned)sum);
    w[n++] = a_cmp(20, 9);
    const size_t to_fail = n++;         // b.ne fail
    w[n] = a_adr(0, (int64_t)PASS_AT - (int64_t)n * 4); n++;
    w[n++] = a_movz(1, 12);             // "cutcall PASS"
    w[n++] = 0xD40004E1u;               // svc #0x27 (OutputDebugString)
    const size_t to_exit = n++;         // b exit
    w[to_fail] = a_bcond(1, (int64_t)(n - to_fail) * 4);
    w[n] = a_adr(0, (int64_t)FAIL_AT - (int64_t)n * 4); n++;
    w[n++] = a_movz(1, 12);             // "cutcall FAIL"
    w[n++] = 0xD40004E1u;
    w[to_exit] = a_b((int64_t)(n - to_exit) * 4);
    w[n++] = 0xD40000E1u;               // svc #0x7 (ExitProcess)
    w[n++] = 0xD4200000u;               // brk #0
    // F(s)
    const size_t F = n;
    w[call_f0] = a_bl((int64_t)(F - call_f0) * 4);
    w[call_f1] = a_bl((int64_t)(F - call_f1) * 4);
    w[n++] = 0xA9BD7BFDu;               // stp x29, x30, [sp, #-48]!
    w[n++] = 0x910003FDu;               // mov x29, sp
    w[n++] = 0xA90153F3u;               // stp x19, x20, [sp, #16]
    w[n++] = 0xF90013FCu;               // str x28, [sp, #32]
    w[n] = a_adr(28, (int64_t)IDX_AT - (int64_t)n * 4); n++;   // adr x28, the case table
    w[n++] = a_mov(19, 0);
    w[n++] = a_movz(20, 0);
    const size_t loop = n;
    w[n++] = a_movz(0, 0);
    w[n++] = 0x38736B8Au;               // ldrb w10, [x28, x19]
    const size_t adr_base = n++;        // adr x9, case 0
    w[n++] = a_addr(9, 9, 10, 2);       // add x9, x9, x10, lsl #2
    w[n++] = 0xD61F0120u;               // br x9
    const size_t case0 = n;
    w[adr_base] = a_adr(9, (int64_t)(case0 - adr_base) * 4);
    const size_t call_g = n++;          // case 0: bl g, whose return address is case 1
    const size_t case1 = n;
    w[n++] = a_addr(20, 20, 19, 0);     // case 1: add x20, x20, x19
    w[n++] = a_addr(20, 20, 0, 0);      // add x20, x20, x0
    w[n++] = a_addi(19, 19, 1);
    w[n++] = a_cmpi(19, CUT_N);
    w[n] = a_bcond(3, (int64_t)(loop - n) * 4); n++;   // b.lo loop
    w[n++] = a_mov(0, 20);
    w[n++] = 0xF94013FCu;               // ldr x28, [sp, #32]
    w[n++] = 0xA94153F3u;               // ldp x19, x20, [sp, #16]
    w[n++] = 0xA8C37BFDu;               // ldp x29, x30, [sp], #48
    w[n++] = 0xD65F03C0u;               // ret
    // g: its own x19 (0xBAD) in the block at an svc, a call through the stub, x19 restored, 7
    const size_t g = n;
    w[call_g] = a_bl((int64_t)(g - call_g) * 4);
    w[n++] = 0xA9BE7BFDu;               // stp x29, x30, [sp, #-32]!
    w[n++] = 0x910003FDu;               // mov x29, sp
    w[n++] = 0xF9000BF3u;               // str x19, [sp, #16]
    w[n++] = a_movz(19, 0xBAD);
    w[n++] = 0xD40003C1u;               // svc #0x1E (GetSystemTick): the block gets x19 = 0xBAD
    const size_t call_stub = n++;       // bl stub
    w[n++] = 0xF9400BF3u;               // ldr x19, [sp, #16]
    w[n++] = 0xA8C27BFDu;               // ldp x29, x30, [sp], #32
    w[n++] = a_movz(0, 7);
    w[n++] = 0xD65F03C0u;               // ret
    const size_t stub = n;              // br through the pointer at DATA, as a PLT entry through its GOT slot
    w[call_stub] = a_bl((int64_t)(stub - call_stub) * 4);
    w[n] = a_adr(16, (int64_t)DATA - (int64_t)n * 4); n++;   // adr x16, DATA
    w[n++] = 0xF9400211u;               // ldr x17, [x16]
    w[n++] = 0xD61F0220u;               // br x17
    const size_t leaf = n;
    w[adr_leaf] = a_adr(9, (int64_t)(leaf - adr_leaf) * 4);
    w[n++] = 0xD65F03C0u;               // ret
    const size_t spfn = n;              // returns with SP 16 bytes up (never called: its address is taken)
    w[adr_spfn] = a_adr(11, (int64_t)(spfn - adr_spfn) * 4);
    w[n++] = 0x910043FFu;               // add sp, sp, #16
    w[n++] = 0xD65F03C0u;               // ret
    w[0] = a_b((int64_t)start * 4);
    w[1] = RO;                          // MOD0's offset
    memcpy(img, w, n * 4);
    memcpy(img + 0x10, "NRO0", 4);
    const uint32_t hdr[] = {0, SIZE, 0, 0, TEXT, RO, DATA - RO, DATA, SIZE - DATA, BSS};
    memcpy(img + 0x14, hdr, sizeof hdr);
    memcpy(img + RO, "MOD0", 4);
    const int32_t mod0[] = {0x20, SIZE - RO, SIZE + BSS - RO, 0x40, 0x40, SIZE - RO};
    memcpy(img + RO + 4, mod0, sizeof mod0);
    memcpy(img + PASS_AT, "cutcall PASS", 12);
    memcpy(img + FAIL_AT, "cutcall FAIL", 12);
    memcpy(img + IDX_AT, cut_idx, CUT_N);
    *c0 = case0 * 4;
    *c1 = case1 * 4;
    return SIZE;
}

// The NRO noret: start keeps 5 in x19 across a call of k, whose only return is br x16 (x16 = the lr it was given): discovery cannot bound that br, and the only
// address-taken function (dead, a brk) does not return, so the summaries say k does not return. start adds k's 37 to x19 and prints PASS for 42. A call to a
// function the summaries call noreturn leaves every register the caller holds in the block, and its return goes on in the JIT from the return address
// (lower_calls.cpp FnCallDirect; aot.h NXAOT_TRAP_JIT_NORETURN, the miss "noreturn", which nxaot leaves unserved). It was a trap: status 6 (DM_AOT_RETURNED)
static size_t noret_nro(uint8_t *img, size_t cap, uint64_t *ret_at) {
    enum { TEXT = 0x1000, RO = 0x1000, DATA = 0x2000, SIZE = 0x3000, BSS = 0x1000, PASS_AT = RO + 0x100, FAIL_AT = RO + 0x120 };
    if (cap < SIZE) return 0;
    memset(img, 0, SIZE);
    uint32_t w[128] = {0};
    size_t n = 0x80 / 4;
    const size_t start = n;
    w[n++] = 0xA9BE7BFDu;               // stp x29, x30, [sp, #-32]!
    w[n++] = 0x910003FDu;               // mov x29, sp
    w[n++] = 0xA90153F3u;               // stp x19, x20, [sp, #16]
    w[n] = a_adr(10, (int64_t)DATA - (int64_t)n * 4); n++;   // adr x10, DATA
    const size_t adr_dead = n++;        // adr x9, dead
    w[n++] = 0xF9000149u;               // str x9, [x10]
    w[n++] = a_movz(19, 5);
    const size_t call_k = n++;          // bl k
    *ret_at = n * 4;
    w[n++] = a_addr(0, 0, 19, 0);       // add x0, x0, x19
    w[n++] = a_movz(9, 42);
    w[n++] = a_cmp(0, 9);
    const size_t to_fail = n++;         // b.ne fail
    w[n] = a_adr(0, (int64_t)PASS_AT - (int64_t)n * 4); n++;
    w[n++] = a_movz(1, 10);             // "noret PASS"
    w[n++] = 0xD40004E1u;               // svc #0x27
    const size_t to_exit = n++;
    w[to_fail] = a_bcond(1, (int64_t)(n - to_fail) * 4);
    w[n] = a_adr(0, (int64_t)FAIL_AT - (int64_t)n * 4); n++;
    w[n++] = a_movz(1, 10);             // "noret FAIL"
    w[n++] = 0xD40004E1u;
    w[to_exit] = a_b((int64_t)(n - to_exit) * 4);
    w[n++] = 0xD40000E1u;               // svc #0x7 (ExitProcess)
    w[n++] = 0xD4200000u;               // brk #0
    const size_t k = n;
    w[call_k] = a_bl((int64_t)(k - call_k) * 4);
    w[n++] = a_mov(16, 30);             // mov x16, x30
    w[n++] = a_movz(0, 37);
    w[n++] = 0xD61F0200u;               // br x16
    const size_t dead = n;
    w[adr_dead] = a_adr(9, (int64_t)(dead - adr_dead) * 4);
    w[n++] = 0xD4200000u;               // brk #0
    w[0] = a_b((int64_t)start * 4);
    w[1] = RO;
    memcpy(img, w, n * 4);
    memcpy(img + 0x10, "NRO0", 4);
    const uint32_t hdr[] = {0, SIZE, 0, 0, TEXT, RO, DATA - RO, DATA, SIZE - DATA, BSS};
    memcpy(img + 0x14, hdr, sizeof hdr);
    memcpy(img + RO, "MOD0", 4);
    const int32_t mod0[] = {0x20, SIZE - RO, SIZE + BSS - RO, 0x40, 0x40, SIZE - RO};
    memcpy(img + RO + 4, mod0, sizeof mod0);
    memcpy(img + PASS_AT, "noret PASS", 10);
    memcpy(img + FAIL_AT, "noret FAIL", 10);
    return SIZE;
}

static int write_file(const char *path, const void *p, size_t len) {
    FILE *f = fopen(path, "wb");
    const int ok = f && fwrite(p, 1, len, f) == len;
    if (f) fclose(f);
    return ok;
}

// The rows: cutcall packed without misses and run under DM_AOT_PROFILE (the cases in the JIT fallback, the host's misses file naming both), then packed with that file
// and run: the right sum and no fault; noret packed and run: the right sum, a JIT continuation at the call's return address and its miss row
static void cutcall_rows(const char *self, const char *nxaot) {
    if (!nxaot || !*nxaot) { printf("%-14s %-9s note: no nxaot (--nxaot, NXAOT): cutcall and noret are not packed here\n", "cutcall", "cut"); return; }
    if (!NXHOST_JIT) {   // the first run's cases, and noret's continuation, run in the JIT fallback
        printf("%-14s %-9s note: a host built without the JIT fallback (CMake NXHOST_JIT off): cutcall and noret need it\n", "cutcall", "cut");
        return;
    }
    static uint8_t img[0x3000];
    uint64_t want = 0, c0 = 0, c1 = 0;
    const size_t len = cutcall_nro(img, sizeof img, &want, &c0, &c1);
    char dir[200], nro[200], pack[200], pack_m[200], misses[260], line[600] = "", note[1600];
    mkdir("t_aot_host.tmp", 0755);
    mkdir("t_aot_host.tmp/cutcall", 0755);
    snprintf(dir, sizeof dir, "t_aot_host.tmp/cutcall/profile");
    mkdir(dir, 0755);
    snprintf(nro, sizeof nro, "t_aot_host.tmp/cutcall/cutcall.nro");
    snprintf(pack, sizeof pack, "t_aot_host.tmp/cutcall/cutcall%s", PACK_EXT);
    mkdir("t_aot_host.tmp/cutcall/misses", 0755);
    snprintf(pack_m, sizeof pack_m, "t_aot_host.tmp/cutcall/misses/cutcall%s", PACK_EXT);
    snprintf(misses, sizeof misses, "%s/cutcall.misses.tsv", dir);
    remove(misses);
    result r;
    memset(&r, 0, sizeof r);
    static char no_lines[1];
    r.guest = no_lines;
    const int wrote = len && write_file(nro, img, len);
    if (!wrote || nxaot_pack(nxaot, nro, pack, line, sizeof line) != 0) {
        snprintf(note, sizeof note, "%s: %s", wrote ? "nxaot build failed" : "cannot write the NRO", line);
        row("cutcall", "first", 0, &r, note);
        return;
    }
    // 1. Without misses: the cases run in the JIT fallback (the dispatcher has no entry there), the sum right, the host's misses file naming both cases
    set_env("DM_AOT_PROFILE", dir);
    run_child(self, pack, nro, "", &r);
    set_env("DM_AOT_PROFILE", NULL);
    char *text = read_text(misses);
    char row0[64], row1[64];
    snprintf(row0, sizeof row0, "0x%llx indirect ", (unsigned long long)c0);
    snprintf(row1, sizeof row1, "0x%llx indirect ", (unsigned long long)c1);
    // Case 0 is gap code discovery compiles (no walk reaches it: the br is unbounded), a function the dispatcher enters; case 1 lies inside it, and the loop's head
    // inside F: both misses
    const int file_ok = text && strstr(text, row1) && !strstr(text, row0);
    int ok = r.status == 0 && !strcmp(r.guest, "cutcall PASS\n") && file_ok;
    snprintf(note, sizeof note, "without misses: sum %llu, %llu JIT runs, the misses file %s%s%s", (unsigned long long)want, r.jit[0],
             file_ok ? "names case 1 (indirect)" : "lacks case 1's row or has case 0's", r.red[0] ? "; " : "", r.red);
    row("cutcall", "first", ok, &r, note);
    if (r.guest != no_lines) free(r.guest);
    free(text);
    // 2. With them: both cases entries, case 0 a function of its own whose call of g returns into case 1's entry: the right sum, no fault
    memset(&r, 0, sizeof r);
    r.guest = no_lines;
    if (!file_ok || nxaot_pack_misses(nxaot, nro, pack_m, misses, line, sizeof line) != 0) {
        snprintf(note, sizeof note, "nxaot build --misses failed or no misses file: %s", line);
        row("cutcall", "cut", 0, &r, note);
        return;
    }
    run_child(self, pack_m, nro, "", &r);
    ok = r.status == 0 && !strcmp(r.guest, "cutcall PASS\n");
    snprintf(note, sizeof note, "with the misses (cases +0x%llx and +0x%llx entries, the call at +0x%llx returning into the second): %s, status %d, %llu JIT runs%s%s",
             (unsigned long long)c0, (unsigned long long)c1, (unsigned long long)c0, r.guest[0] ? r.guest : "no output\n", r.status, r.jit[0], r.red[0] ? "; " : "",
             r.red);
    note[strcspn(note, "\n")] = note[strcspn(note, "\n")] ? ' ' : 0;
    row("cutcall", "cut", ok, &r, note);
    if (r.guest != no_lines) free(r.guest);
    // 3. noret: a call to a function the summaries say does not return, which returns
    uint64_t ret_at = 0;
    const size_t nlen = noret_nro(img, sizeof img, &ret_at);
    char ndir[200], nnro[200], npack[200], nmiss[260];
    mkdir("t_aot_host.tmp/noret", 0755);
    snprintf(ndir, sizeof ndir, "t_aot_host.tmp/noret/profile");
    mkdir(ndir, 0755);
    snprintf(nnro, sizeof nnro, "t_aot_host.tmp/noret/noret.nro");
    snprintf(npack, sizeof npack, "t_aot_host.tmp/noret/noret%s", PACK_EXT);
    snprintf(nmiss, sizeof nmiss, "%s/noret.misses.tsv", ndir);
    remove(nmiss);
    memset(&r, 0, sizeof r);
    r.guest = no_lines;
    if (!nlen || !write_file(nnro, img, nlen) || nxaot_pack(nxaot, nnro, npack, line, sizeof line) != 0) {
        snprintf(note, sizeof note, "nxaot build failed: %s", line);
        row("noret", "return", 0, &r, note);
        return;
    }
    set_env("DM_AOT_PROFILE", ndir);
    run_child(self, npack, nnro, "", &r);
    set_env("DM_AOT_PROFILE", NULL);
    text = read_text(nmiss);
    snprintf(row0, sizeof row0, "0x%llx noreturn ", (unsigned long long)ret_at);
    const int miss_ok = text && strstr(text, row0);
    ok = r.status == 0 && !strcmp(r.guest, "noret PASS\n") && miss_ok && strstr(r.red, "returned") != NULL;
    snprintf(note, sizeof note, "a call of a function the summaries say does not return, which returns: %s, status %d, the misses file %s%s%s", r.guest[0] ? r.guest : "no output\n",
             r.status, miss_ok ? "with its noreturn row" : "without a noreturn row", r.red[0] ? "; " : "", r.red);
    note[strcspn(note, "\n")] = note[strcspn(note, "\n")] ? ' ' : 0;
    row("noret", "return", ok, &r, note);
    if (r.guest != no_lines) free(r.guest);
    free(text);
}

// ---------- A miss as a case of its function's br (dm/tools/aot/README.md, "A miss as a case of its function's br") ----------
// The NRO casecall, written here word by word: cutcall's loop with its cases inside F (between F's jump to the loop's head and the head, where the walk does
// not reach them: the br is unbounded), so that the host's misses there lie in F, which has a br left to the dispatcher. Case 0 calls g and returns into case
// 1, as cutcall's does. A third piece of F, CX (5, then ret), is reached only from G, through a pointer G computes (adr F plus an offset: no source of
// discovery's names it). start adds F(0), F(1) and G(). The first pack runs the cases in the JIT (the misses: both cases and CX, indirect); the second makes
// them cases of F's br (no entry, no cut: F one function, its br a switch whose default is the dispatcher) and runs F with no JIT run, but G still reaches CX
// through the dispatcher, which the host now logs as indirect-case (a case of the pack); the third pack makes CX an entry, and no miss is left
static size_t casecall_nro(uint8_t *img, size_t cap, uint64_t *want, uint64_t *c0, uint64_t *c1, uint64_t *cx) {
    enum { TEXT = 0x1000, RO = 0x1000, DATA = 0x2000, SIZE = 0x3000, BSS = 0x1000, PASS_AT = RO + 0x100, FAIL_AT = RO + 0x120, IDX_AT = RO + 0x140 };
    if (cap < SIZE) return 0;
    memset(img, 0, SIZE);
    uint32_t w[256] = {0};
    size_t n = 0x80 / 4;
    const size_t start = n;
    w[n++] = 0xA9BE7BFDu;               // stp x29, x30, [sp, #-32]!
    w[n++] = 0x910003FDu;               // mov x29, sp
    w[n++] = 0xA90153F3u;               // stp x19, x20, [sp, #16]
    w[n] = a_adr(10, (int64_t)DATA - (int64_t)n * 4); n++;   // adr x10, DATA
    const size_t adr_leaf = n++;        // adr x9, leaf
    w[n++] = 0xF9000149u;               // str x9, [x10]
    const size_t adr_spfn = n++;        // adr x11, spfn
    w[n++] = 0xF900054Bu;               // str x11, [x10, #8]
    w[n++] = a_movz(0, 0);
    const size_t call_f0 = n++;
    w[n++] = a_mov(19, 0);
    w[n++] = a_movz(0, 1);
    const size_t call_f1 = n++;
    w[n++] = a_addr(20, 19, 0, 0);      // add x20, x19, x0
    const size_t call_g2 = n++;         // bl G
    w[n++] = a_addr(20, 20, 0, 0);      // add x20, x20, x0
    uint64_t sum = 5;
    for (unsigned s0 = 0; s0 < 2; s0++)
        for (unsigned i = s0; i < CUT_N; i++) sum += i + (cut_idx[i] == 0 ? 7 : 0);
    *want = sum;
    w[n++] = a_movz(9, (unsigned)sum);
    w[n++] = a_cmp(20, 9);
    const size_t to_fail = n++;
    w[n] = a_adr(0, (int64_t)PASS_AT - (int64_t)n * 4); n++;
    w[n++] = a_movz(1, 13);             // "casecall PASS"
    w[n++] = 0xD40004E1u;               // svc #0x27
    const size_t to_exit = n++;
    w[to_fail] = a_bcond(1, (int64_t)(n - to_fail) * 4);
    w[n] = a_adr(0, (int64_t)FAIL_AT - (int64_t)n * 4); n++;
    w[n++] = a_movz(1, 13);             // "casecall FAIL"
    w[n++] = 0xD40004E1u;
    w[to_exit] = a_b((int64_t)(n - to_exit) * 4);
    w[n++] = 0xD40000E1u;               // svc #0x7 (ExitProcess)
    w[n++] = 0xD4200000u;               // brk #0
    // F(s): the cases first, the loop's head after them
    const size_t F = n;
    w[call_f0] = a_bl((int64_t)(F - call_f0) * 4);
    w[call_f1] = a_bl((int64_t)(F - call_f1) * 4);
    w[n++] = 0xA9BD7BFDu;               // stp x29, x30, [sp, #-48]!
    w[n++] = 0x910003FDu;               // mov x29, sp
    w[n++] = 0xA90153F3u;               // stp x19, x20, [sp, #16]
    w[n++] = 0xF90013FCu;               // str x28, [sp, #32]
    w[n] = a_adr(28, (int64_t)IDX_AT - (int64_t)n * 4); n++;   // adr x28, the case table
    w[n++] = a_mov(19, 0);
    w[n++] = a_movz(20, 0);
    const size_t to_loop = n++;         // b loop
    const size_t case0 = n;
    const size_t call_g = n++;          // case 0: bl g, whose return address is case 1
    const size_t case1 = n;
    w[n++] = a_addr(20, 20, 19, 0);     // case 1: add x20, x20, x19
    w[n++] = a_addr(20, 20, 0, 0);      // add x20, x20, x0
    w[n++] = a_addi(19, 19, 1);
    w[n++] = a_cmpi(19, CUT_N);         // the loop's test here, after the case: no compare bounds the index at the br
    const size_t to_done = n++;         // b.hs done
    const size_t back = n++;            // b loop
    const size_t cxs = n;               // CX: 5, then ret (G calls it through a pointer)
    w[n++] = a_movz(0, 5);
    w[n++] = 0xD65F03C0u;               // ret
    const size_t done = n;              // done, before the head too: only the cases reach it
    w[to_done] = a_bcond(2, (int64_t)(done - to_done) * 4);   // b.hs (cs)
    w[n++] = a_mov(0, 20);
    w[n++] = 0xF94013FCu;               // ldr x28, [sp, #32]
    w[n++] = 0xA94153F3u;               // ldp x19, x20, [sp, #16]
    w[n++] = 0xA8C37BFDu;               // ldp x29, x30, [sp], #48
    w[n++] = 0xD65F03C0u;               // ret
    const size_t loop = n;              // the loop's head, the last of F
    w[to_loop] = a_b((int64_t)(loop - to_loop) * 4);
    w[back] = a_b((int64_t)(loop - back) * 4);
    w[n++] = a_movz(0, 0);
    w[n++] = 0x38736B8Au;               // ldrb w10, [x28, x19]
    w[n] = a_adr(9, (int64_t)(case0 - n) * 4); n++;   // adr x9, case 0
    w[n++] = a_addr(9, 9, 10, 2);       // add x9, x9, x10, lsl #2
    w[n++] = 0xD61F0120u;               // br x9
    // G: CX through a pointer it computes
    const size_t G = n;
    w[call_g2] = a_bl((int64_t)(G - call_g2) * 4);
    w[n++] = 0xA9BF7BFDu;               // stp x29, x30, [sp, #-16]!
    w[n++] = 0x910003FDu;               // mov x29, sp
    w[n] = a_adr(9, (int64_t)(F - n) * 4); n++;   // adr x9, F
    w[n++] = a_addi(9, 9, (unsigned)((cxs - F) * 4));   // add x9, x9, #(CX - F)
    w[n++] = 0xD63F0120u;               // blr x9
    w[n++] = 0xA8C17BFDu;               // ldp x29, x30, [sp], #16
    w[n++] = 0xD65F03C0u;               // ret
    // g, the stub, leaf and spfn: cutcall's
    const size_t g = n;
    w[call_g] = a_bl((int64_t)(g - call_g) * 4);
    w[n++] = 0xA9BE7BFDu;
    w[n++] = 0x910003FDu;
    w[n++] = 0xF9000BF3u;               // str x19, [sp, #16]
    w[n++] = a_movz(19, 0xBAD);
    w[n++] = 0xD40003C1u;               // svc #0x1E
    const size_t call_stub = n++;
    w[n++] = 0xF9400BF3u;               // ldr x19, [sp, #16]
    w[n++] = 0xA8C27BFDu;
    w[n++] = a_movz(0, 7);
    w[n++] = 0xD65F03C0u;
    const size_t stub = n;
    w[call_stub] = a_bl((int64_t)(stub - call_stub) * 4);
    w[n] = a_adr(16, (int64_t)DATA - (int64_t)n * 4); n++;
    w[n++] = 0xF9400211u;               // ldr x17, [x16]
    w[n++] = 0xD61F0220u;               // br x17
    const size_t leaf = n;
    w[adr_leaf] = a_adr(9, (int64_t)(leaf - adr_leaf) * 4);
    w[n++] = 0xD65F03C0u;
    const size_t spfn = n;
    w[adr_spfn] = a_adr(11, (int64_t)(spfn - adr_spfn) * 4);
    w[n++] = 0x910043FFu;               // add sp, sp, #16
    w[n++] = 0xD65F03C0u;
    w[0] = a_b((int64_t)start * 4);
    w[1] = RO;
    memcpy(img, w, n * 4);
    memcpy(img + 0x10, "NRO0", 4);
    const uint32_t hdr[] = {0, SIZE, 0, 0, TEXT, RO, DATA - RO, DATA, SIZE - DATA, BSS};
    memcpy(img + 0x14, hdr, sizeof hdr);
    memcpy(img + RO, "MOD0", 4);
    const int32_t mod0[] = {0x20, SIZE - RO, SIZE + BSS - RO, 0x40, 0x40, SIZE - RO};
    memcpy(img + RO + 4, mod0, sizeof mod0);
    memcpy(img + PASS_AT, "casecall PASS", 13);
    memcpy(img + FAIL_AT, "casecall FAIL", 13);
    memcpy(img + IDX_AT, cut_idx, CUT_N);
    *c0 = case0 * 4; *c1 = case1 * 4; *cx = cxs * 4;
    return SIZE;
}

// The rows: casecall packed three times, each run under the same DM_AOT_PROFILE folder (its misses file accumulates, as a player's does). Packed with
// --no-guess: F's br would get its cases from the code (guesscase below), and these rows check the misses' path
static void casecall_rows_packed(const char *self, const char *nxaot);
static void casecall_rows(const char *self, const char *nxaot) {
    pack_extra = " --no-guess";
    casecall_rows_packed(self, nxaot);
    pack_extra = "";
}
static void casecall_rows_packed(const char *self, const char *nxaot) {
    if (!nxaot || !*nxaot || !NXHOST_JIT) {
        printf("%-14s %-9s note: %s\n", "casecall", "first", !NXHOST_JIT ? "a host built without the JIT fallback: casecall needs it" : "no nxaot (--nxaot, NXAOT)");
        return;
    }
    static uint8_t img[0x3000];
    uint64_t want = 0, c0 = 0, c1 = 0, cx = 0;
    const size_t len = casecall_nro(img, sizeof img, &want, &c0, &c1, &cx);
    char dir[200], nro[200], pack[3][200], misses[260], line[600] = "", note[1600], row0[64], row1[64], rowx[64], rowc[64];
    mkdir("t_aot_host.tmp", 0755);
    mkdir("t_aot_host.tmp/casecall", 0755);
    snprintf(dir, sizeof dir, "t_aot_host.tmp/casecall/profile");
    mkdir(dir, 0755);
    snprintf(nro, sizeof nro, "t_aot_host.tmp/casecall/casecall.nro");
    for (int k = 0; k < 3; k++) {
        char sub[200];
        snprintf(sub, sizeof sub, "t_aot_host.tmp/casecall/p%d", k);
        mkdir(sub, 0755);
        snprintf(pack[k], sizeof pack[k], "%s/casecall%s", sub, PACK_EXT);
    }
    snprintf(misses, sizeof misses, "%s/casecall.misses.tsv", dir);
    remove(misses);
    snprintf(row0, sizeof row0, "0x%llx indirect ", (unsigned long long)c0);
    snprintf(row1, sizeof row1, "0x%llx indirect ", (unsigned long long)c1);
    snprintf(rowx, sizeof rowx, "0x%llx indirect ", (unsigned long long)cx);
    snprintf(rowc, sizeof rowc, "0x%llx indirect-case ", (unsigned long long)cx);
    result r;
    static char no_lines[1];
    const char *names[3] = {"first", "cases", "entry"};
    for (int k = 0; k < 3; k++) {
        memset(&r, 0, sizeof r);
        r.guest = no_lines;
        const int built = k == 0 ? (len && write_file(nro, img, len) && nxaot_pack(nxaot, nro, pack[0], line, sizeof line) == 0)
                                 : nxaot_pack_misses(nxaot, nro, pack[k], misses, line, sizeof line) == 0;
        if (!built) {
            snprintf(note, sizeof note, "nxaot build%s failed: %s", k ? " --misses" : "", line);
            row("casecall", names[k], 0, &r, note);
            return;
        }
        set_env("DM_AOT_PROFILE", dir);
        run_child(self, pack[k], nro, "", &r);
        set_env("DM_AOT_PROFILE", NULL);
        char *text = read_text(misses);
        const int pass = r.status == 0 && !strcmp(r.guest, "casecall PASS\n");
        int ok = pass;
        if (k == 0) {   // the cases and CX run in the JIT: the misses file names the three, indirect
            ok = ok && text && strstr(text, row0) && strstr(text, row1) && strstr(text, rowx);
            snprintf(note, sizeof note, "packed without misses: sum %llu, %llu JIT runs, the misses file %s", (unsigned long long)want, r.jit[0],
                     ok ? "names both cases and CX (indirect)" : "lacks a row");
        } else if (k == 1) {   // F one function, its br a switch: no JIT run but G's call of CX, which the host names indirect-case
            ok = ok && r.have_jit && r.jit[0] == 1 && text && strstr(text, rowc);
            snprintf(note, sizeof note, "packed with them (cases of F's br, F not cut): %llu JIT run (want 1: G's call of CX through the dispatcher), the misses file %s",
                     r.jit[0], text && strstr(text, rowc) ? "names CX indirect-case" : "has no indirect-case row");
        } else {   // CX an entry: no JIT run, no miss left
            ok = ok && r.have_jit && r.jit[0] == 0;
            snprintf(note, sizeof note, "packed with the indirect-case row (CX an entry): %llu JIT runs (want 0)", r.jit[0]);
        }
        if (r.red[0] && strlen(note) + strlen(r.red) + 3 < sizeof note) { strcat(note, "; "); strcat(note, r.red); }
        row("casecall", names[k], ok, &r, note);
        if (r.guest != no_lines) free(r.guest);
        free(text);
    }
}

// ---------- Guarded cases from a function's own code (dm/tools/aot/README.md, "Static cases of a br left to the dispatcher") ----------
// The NRO guesscase, written here word by word: casecall's F without CX (a table read by an index no compare bounds at its br: the loop's test lies in case
// 1; its base x28 set before the loop, which calls g, an open function whose union may change x28) and H, a loop over a table whose index is compared right
// before it (cmp x19, #2; b.hi), its base x21 set before the loop that calls g through a register (blr x9: every register may change, for the analysis). Packed
// with --no-guess, both brs are left to the dispatcher and their cases run
// in the JIT (the misses file names F's case 0 and H's case 0, where each first reaches the dispatcher, indirect); packed as a first pack is (guesses on),
// discovery gives each br its cases from the code (F's table scanned from index 0, H's bounded by its compare once the blr is taken to keep x21) and
// the run has no JIT run at all
static size_t guesscase_nro(uint8_t *img, size_t cap, uint64_t *want, uint64_t *f0, uint64_t *h0) {
    enum { TEXT = 0x1000, RO = 0x1000, DATA = 0x2000, SIZE = 0x3000, BSS = 0x1000, PASS_AT = RO + 0x100, FAIL_AT = RO + 0x120, IDX_AT = RO + 0x140, HTAB_AT = RO + 0x180 };
    if (cap < SIZE) return 0;
    memset(img, 0, SIZE);
    uint32_t w[256] = {0};
    size_t n = 0x80 / 4;
    const size_t start = n;
    w[n++] = 0xA9BE7BFDu;               // stp x29, x30, [sp, #-32]!
    w[n++] = 0x910003FDu;               // mov x29, sp
    w[n++] = 0xA90153F3u;               // stp x19, x20, [sp, #16]
    w[n] = a_adr(10, (int64_t)DATA - (int64_t)n * 4); n++;   // adr x10, DATA
    const size_t adr_leaf = n++;        // adr x9, leaf
    w[n++] = 0xF9000149u;               // str x9, [x10]
    const size_t adr_spfn = n++;        // adr x11, spfn
    w[n++] = 0xF900054Bu;               // str x11, [x10, #8]
    w[n++] = a_movz(0, 0);
    const size_t call_f0 = n++;
    w[n++] = a_mov(19, 0);
    w[n++] = a_movz(0, 1);
    const size_t call_f1 = n++;
    w[n++] = a_addr(19, 19, 0, 0);      // add x19, x19, x0
    w[n++] = a_movz(0, 0);
    const size_t call_h = n++;
    w[n++] = a_addr(20, 19, 0, 0);      // add x20, x19, x0
    uint64_t sum = 1 + 2 + 4;           // H(0): its cases 0, 1 and 2
    for (unsigned s0 = 0; s0 < 2; s0++)
        for (unsigned i = s0; i < CUT_N; i++) sum += i + (cut_idx[i] == 0 ? 7 : 0);
    *want = sum;
    w[n++] = a_movz(9, (unsigned)sum);
    w[n++] = a_cmp(20, 9);
    const size_t to_fail = n++;
    w[n] = a_adr(0, (int64_t)PASS_AT - (int64_t)n * 4); n++;
    w[n++] = a_movz(1, 14);             // "guesscase PASS"
    w[n++] = 0xD40004E1u;               // svc #0x27
    const size_t to_exit = n++;
    w[to_fail] = a_bcond(1, (int64_t)(n - to_fail) * 4);
    w[n] = a_adr(0, (int64_t)FAIL_AT - (int64_t)n * 4); n++;
    w[n++] = a_movz(1, 14);             // "guesscase FAIL"
    w[n++] = 0xD40004E1u;
    w[to_exit] = a_b((int64_t)(n - to_exit) * 4);
    w[n++] = 0xD40000E1u;               // svc #0x7 (ExitProcess)
    w[n++] = 0xD4200000u;               // brk #0
    // F(s): casecall's, the cases first, the loop's head after them
    const size_t F = n;
    w[call_f0] = a_bl((int64_t)(F - call_f0) * 4);
    w[call_f1] = a_bl((int64_t)(F - call_f1) * 4);
    w[n++] = 0xA9BD7BFDu;               // stp x29, x30, [sp, #-48]!
    w[n++] = 0x910003FDu;               // mov x29, sp
    w[n++] = 0xA90153F3u;               // stp x19, x20, [sp, #16]
    w[n++] = 0xF90013FCu;               // str x28, [sp, #32]
    w[n] = a_adr(28, (int64_t)IDX_AT - (int64_t)n * 4); n++;   // adr x28, the case table
    w[n++] = a_mov(19, 0);
    w[n++] = a_movz(20, 0);
    const size_t to_loop = n++;         // b loop
    const size_t case0 = n;
    const size_t call_g = n++;          // case 0: bl g, whose return address is case 1
    const size_t case1 = n;
    w[n++] = a_addr(20, 20, 19, 0);     // case 1: add x20, x20, x19
    w[n++] = a_addr(20, 20, 0, 0);      // add x20, x20, x0
    w[n++] = a_addi(19, 19, 1);
    w[n++] = a_cmpi(19, CUT_N);         // the loop's test here, after the case: no compare bounds the index at the br
    const size_t to_done = n++;         // b.hs done
    const size_t back = n++;            // b loop
    const size_t done = n;
    w[to_done] = a_bcond(2, (int64_t)(done - to_done) * 4);
    w[n++] = a_mov(0, 20);
    w[n++] = 0xF94013FCu;               // ldr x28, [sp, #32]
    w[n++] = 0xA94153F3u;               // ldp x19, x20, [sp, #16]
    w[n++] = 0xA8C37BFDu;               // ldp x29, x30, [sp], #48
    w[n++] = 0xD65F03C0u;               // ret
    const size_t loop = n;              // the loop's head, the last of F
    w[to_loop] = a_b((int64_t)(loop - to_loop) * 4);
    w[back] = a_b((int64_t)(loop - back) * 4);
    w[n++] = a_movz(0, 0);
    w[n++] = 0x38736B8Au;               // ldrb w10, [x28, x19]
    w[n] = a_adr(9, (int64_t)(case0 - n) * 4); n++;   // adr x9, case 0
    w[n++] = a_addr(9, 9, 10, 2);       // add x9, x9, x10, lsl #2
    w[n++] = 0xD61F0120u;               // br x9
    // H(s): the cases first, then done, next and the loop's head; the index compared right before the table
    const size_t H = n;
    w[call_h] = a_bl((int64_t)(H - call_h) * 4);
    w[n++] = 0xA9BD7BFDu;               // stp x29, x30, [sp, #-48]!
    w[n++] = 0x910003FDu;               // mov x29, sp
    w[n++] = 0xA90153F3u;               // stp x19, x20, [sp, #16]
    w[n++] = 0xF90013F5u;               // str x21, [sp, #32]
    w[n] = a_adr(21, (int64_t)HTAB_AT - (int64_t)n * 4); n++;   // adr x21, H's table: before the loop
    w[n++] = a_mov(19, 0);
    w[n++] = a_movz(20, 0);
    const size_t hto_loop = n++;        // b loop
    const size_t hc0 = n;
    w[n++] = a_addi(20, 20, 1);         // case 0: add x20, x20, #1
    const size_t hb0 = n++;             // b next
    const size_t hc1 = n;
    w[n++] = a_addi(20, 20, 2);
    const size_t hb1 = n++;
    const size_t hc2 = n;
    w[n++] = a_addi(20, 20, 4);
    const size_t hb2 = n++;
    const size_t hdone = n;
    w[n++] = a_mov(0, 20);
    w[n++] = 0xF94013F5u;               // ldr x21, [sp, #32]
    w[n++] = 0xA94153F3u;               // ldp x19, x20, [sp, #16]
    w[n++] = 0xA8C37BFDu;               // ldp x29, x30, [sp], #48
    w[n++] = 0xD65F03C0u;               // ret
    const size_t hnext = n;
    w[hb0] = a_b((int64_t)(hnext - hb0) * 4);
    w[hb1] = a_b((int64_t)(hnext - hb1) * 4);
    w[hb2] = a_b((int64_t)(hnext - hb2) * 4);
    w[n++] = a_addi(19, 19, 1);
    const size_t hloop = n;
    w[hto_loop] = a_b((int64_t)(hloop - hto_loop) * 4);
    const size_t adr_gh = n++;          // adr x9, g
    w[n++] = 0xD63F0120u;               // blr x9: a call whose effect is every register, x21 included
    w[n++] = a_cmpi(19, 2);
    const size_t hto_done = n++;        // b.hi done
    w[hto_done] = a_bcond(8, (int64_t)(hdone - hto_done) * 4);
    w[n++] = 0x38736AAAu;               // ldrb w10, [x21, x19]
    w[n] = a_adr(9, (int64_t)(hc0 - n) * 4); n++;   // adr x9, case 0
    w[n++] = a_addr(9, 9, 10, 2);       // add x9, x9, x10, lsl #2
    w[n++] = 0xD61F0120u;               // br x9
    // g, the stub, leaf and spfn: casecall's
    const size_t g = n;
    w[call_g] = a_bl((int64_t)(g - call_g) * 4);
    w[adr_gh] = a_adr(9, (int64_t)(g - adr_gh) * 4);
    w[n++] = 0xA9BE7BFDu;
    w[n++] = 0x910003FDu;
    w[n++] = 0xF9000BF3u;               // str x19, [sp, #16]
    w[n++] = a_movz(19, 0xBAD);
    w[n++] = 0xD40003C1u;               // svc #0x1E
    const size_t call_stub = n++;
    w[n++] = 0xF9400BF3u;               // ldr x19, [sp, #16]
    w[n++] = 0xA8C27BFDu;
    w[n++] = a_movz(0, 7);
    w[n++] = 0xD65F03C0u;
    const size_t stub = n;
    w[call_stub] = a_bl((int64_t)(stub - call_stub) * 4);
    w[n] = a_adr(16, (int64_t)DATA - (int64_t)n * 4); n++;
    w[n++] = 0xF9400211u;               // ldr x17, [x16]
    w[n++] = 0xD61F0220u;               // br x17
    const size_t leaf = n;
    w[adr_leaf] = a_adr(9, (int64_t)(leaf - adr_leaf) * 4);
    w[n++] = 0xD65F03C0u;
    const size_t spfn = n;
    w[adr_spfn] = a_adr(11, (int64_t)(spfn - adr_spfn) * 4);
    w[n++] = 0x910043FFu;               // add sp, sp, #16
    w[n++] = 0xD65F03C0u;
    w[0] = a_b((int64_t)start * 4);
    w[1] = RO;
    memcpy(img, w, n * 4);
    memcpy(img + 0x10, "NRO0", 4);
    const uint32_t hdr[] = {0, SIZE, 0, 0, TEXT, RO, DATA - RO, DATA, SIZE - DATA, BSS};
    memcpy(img + 0x14, hdr, sizeof hdr);
    memcpy(img + RO, "MOD0", 4);
    const int32_t mod0[] = {0x20, SIZE - RO, SIZE + BSS - RO, 0x40, 0x40, SIZE - RO};
    memcpy(img + RO + 4, mod0, sizeof mod0);
    memcpy(img + PASS_AT, "guesscase PASS", 14);
    memcpy(img + FAIL_AT, "guesscase FAIL", 14);
    memcpy(img + IDX_AT, cut_idx, CUT_N);
    const uint8_t htab[3] = {0, (uint8_t)(hc1 - hc0), (uint8_t)(hc2 - hc0)};
    memcpy(img + HTAB_AT, htab, sizeof htab);
    *f0 = case0 * 4; *h0 = hc0 * 4;
    return SIZE;
}

// The NRO guessbad: dm/tools/aot/tests/test_discover.py's guess_garbage as a program. F(j, i) (x1, x2) is br2, a table bounded by its compare on x1 (j <= 1:
// 10 or 20), and else br1, a table read by x2, an index nothing bounds, whose two entries are its cases (100, 200) and whose third lands on br2's table
// read; then one past F (E follows it). br1's guess would enter br2's region from outside it and leave br2's table underived, so discovery withdraws it,
// and the pack is the one --no-guess makes: the same run, br1's cases through the dispatcher, and the same misses
static size_t guessbad_nro(uint8_t *img, size_t cap, uint64_t *want) {
    enum { TEXT = 0x1000, RO = 0x1000, DATA = 0x2000, SIZE = 0x3000, BSS = 0x1000, PASS_AT = RO + 0x100, FAIL_AT = RO + 0x120, T1_AT = RO + 0x140, T2_AT = RO + 0x150 };
    if (cap < SIZE) return 0;
    memset(img, 0, SIZE);
    uint32_t w[160] = {0};
    size_t n = 0x80 / 4;
    const size_t start = n;
    w[n++] = 0xA9BE7BFDu;               // stp x29, x30, [sp, #-32]!
    w[n++] = 0x910003FDu;               // mov x29, sp
    w[n++] = 0xA90153F3u;               // stp x19, x20, [sp, #16]
    w[n++] = a_movz(19, 0);
    size_t calls[4];
    const unsigned js[4] = {0, 1, 2, 3}, is[4] = {0, 0, 0, 1};
    for (int k = 0; k < 4; k++) {
        w[n++] = a_movz(1, js[k]);
        w[n++] = a_movz(2, is[k]);
        calls[k] = n++;                 // bl F
        w[n++] = a_addr(19, 19, 0, 0);  // add x19, x19, x0
    }
    const size_t call_e = n++;          // bl E
    *want = 10 + 20 + 100 + 200;
    w[n++] = a_movz(9, (unsigned)*want);
    w[n++] = a_cmp(19, 9);
    const size_t to_fail = n++;
    w[n] = a_adr(0, (int64_t)PASS_AT - (int64_t)n * 4); n++;
    w[n++] = a_movz(1, 13);             // "guessbad PASS"
    w[n++] = 0xD40004E1u;               // svc #0x27
    const size_t to_exit = n++;
    w[to_fail] = a_bcond(1, (int64_t)(n - to_fail) * 4);
    w[n] = a_adr(0, (int64_t)FAIL_AT - (int64_t)n * 4); n++;
    w[n++] = a_movz(1, 13);             // "guessbad FAIL"
    w[n++] = 0xD40004E1u;
    w[to_exit] = a_b((int64_t)(n - to_exit) * 4);
    w[n++] = 0xD40000E1u;               // svc #0x7 (ExitProcess)
    w[n++] = 0xD4200000u;               // brk #0
    const size_t F = n;
    for (int k = 0; k < 4; k++) w[calls[k]] = a_bl((int64_t)(F - calls[k]) * 4);
    w[n++] = a_cmpi(1, 1);
    const size_t to_join = n++;         // b.hi join
    const size_t r = n;                 // br2's table read
    w[n] = a_adr(14, (int64_t)T2_AT - (int64_t)n * 4); n++;
    const size_t adr_d0 = n++;          // adr x15, d0
    w[n++] = 0x386169D0u;               // ldrb w16, [x14, x1]
    w[n++] = a_addr(15, 15, 16, 2);     // add x15, x15, x16, lsl #2
    w[n++] = 0xD61F01E0u;               // br x15 (br2)
    const size_t d0 = n;
    w[adr_d0] = a_adr(15, (int64_t)(d0 - adr_d0) * 4);
    w[n++] = a_movz(0, 10);
    w[n++] = 0xD65F03C0u;
    const size_t d1 = n;
    w[n++] = a_movz(0, 20);
    w[n++] = 0xD65F03C0u;
    const size_t join = n;
    w[to_join] = a_bcond(8, (int64_t)(join - to_join) * 4);
    w[n] = a_adr(11, (int64_t)T1_AT - (int64_t)n * 4); n++;
    w[n] = a_adr(12, (int64_t)(r - n) * 4); n++;   // adr x12, r: br1's base
    w[n++] = 0x3862696Du;               // ldrb w13, [x11, x2]
    w[n++] = a_addr(12, 12, 13, 2);     // add x12, x12, x13, lsl #2
    const size_t to_br1 = n++;          // b br1, over its cases
    const size_t c0 = n;
    w[n++] = a_movz(0, 100);
    w[n++] = 0xD65F03C0u;
    const size_t c1 = n;
    w[n++] = a_movz(0, 200);
    w[n++] = 0xD65F03C0u;
    const size_t br1 = n;
    w[to_br1] = a_b((int64_t)(br1 - to_br1) * 4);
    w[n++] = 0xD61F0180u;               // br x12 (br1)
    const size_t e = n;                 // E: the function after F, where br1's fourth entry lands
    w[call_e] = a_bl((int64_t)(e - call_e) * 4);
    w[n++] = 0xD65F03C0u;
    w[0] = a_b((int64_t)start * 4);
    w[1] = RO;
    memcpy(img, w, n * 4);
    memcpy(img + 0x10, "NRO0", 4);
    const uint32_t hdr[] = {0, SIZE, 0, 0, TEXT, RO, DATA - RO, DATA, SIZE - DATA, BSS};
    memcpy(img + 0x14, hdr, sizeof hdr);
    memcpy(img + RO, "MOD0", 4);
    const int32_t mod0[] = {0x20, SIZE - RO, SIZE + BSS - RO, 0x40, 0x40, SIZE - RO};
    memcpy(img + RO + 4, mod0, sizeof mod0);
    memcpy(img + PASS_AT, "guessbad PASS", 13);
    memcpy(img + FAIL_AT, "guessbad FAIL", 13);
    const uint8_t t1[4] = {(uint8_t)(c0 - r), (uint8_t)(c1 - r), 0, 0xFF}, t2[2] = {0, (uint8_t)(d1 - d0)};
    memcpy(img + T1_AT, t1, sizeof t1);
    memcpy(img + T2_AT, t2, sizeof t2);
    return SIZE;
}

// The misses file's rows (its lines that are no comment), for comparing two sessions' files
static void miss_rows(const char *text, char *out, size_t n) {
    out[0] = 0;
    for (const char *p = text; p && *p;) {
        const char *e = strchr(p, '\n');
        const size_t len = e ? (size_t)(e - p) : strlen(p);
        if (*p != '#' && len && strlen(out) + len + 2 < n) { strncat(out, p, len); strcat(out, ";"); }
        p = e ? e + 1 : NULL;
    }
}

// The rows: guesscase packed with --no-guess (its cases in the JIT, the misses file naming F's case 0 and H's case 0) and as a first pack (no JIT run);
// guessbad packed both ways (the same run, the same JIT runs and misses: its guess withdrawn)
static void guess_rows(const char *self, const char *nxaot) {
    if (!nxaot || !*nxaot || !NXHOST_JIT) {
        printf("%-14s %-9s note: %s\n", "guesscase", "first", !NXHOST_JIT ? "a host built without the JIT fallback: the guess rows need it" : "no nxaot (--nxaot, NXAOT)");
        return;
    }
    static uint8_t img[0x3000];
    static char no_lines[1];
    char nro[200], pack[2][200], dir[2][200], misses[2][260], line[600] = "", note[1600], rowf[64], rowh[64];
    result r;
    for (int which = 0; which < 2; which++) {   // 0: guesscase, 1: guessbad
        const char *name = which ? "guessbad" : "guesscase";
        uint64_t want = 0, f0 = 0, h0 = 0;
        const size_t len = which ? guessbad_nro(img, sizeof img, &want) : guesscase_nro(img, sizeof img, &want, &f0, &h0);
        char base[200];
        snprintf(base, sizeof base, "t_aot_host.tmp/%s", name);
        mkdir("t_aot_host.tmp", 0755);
        mkdir(base, 0755);
        snprintf(nro, sizeof nro, "%s/%s.nro", base, name);
        if (!len || !write_file(nro, img, len)) { memset(&r, 0, sizeof r); r.guest = no_lines; row(name, "write", 0, &r, "cannot write the NRO"); continue; }
        char *text[2] = {NULL, NULL};
        uint64_t jit[2] = {0, 0};
        int pass[2] = {0, 0}, have[2] = {0, 0};
        for (int k = 0; k < 2; k++) {   // 0: --no-guess, 1: guesses on (a first pack)
            snprintf(dir[k], sizeof dir[k], "%s/profile%d", base, k);
            mkdir(dir[k], 0755);
            char sub[200];
            snprintf(sub, sizeof sub, "%s/p%d", base, k);
            mkdir(sub, 0755);
            snprintf(pack[k], sizeof pack[k], "%s/%s%s", sub, name, PACK_EXT);
            snprintf(misses[k], sizeof misses[k], "%s/%s.misses.tsv", dir[k], name);
            remove(misses[k]);
            memset(&r, 0, sizeof r);
            r.guest = no_lines;
            pack_extra = k ? "" : " --no-guess";
            const int built = nxaot_pack(nxaot, nro, pack[k], line, sizeof line) == 0;
            pack_extra = "";
            if (!built) {
                snprintf(note, sizeof note, "nxaot build%s failed: %s", k ? "" : " --no-guess", line);
                row(name, k ? "guess" : "no-guess", 0, &r, note);
                continue;
            }
            set_env("DM_AOT_PROFILE", dir[k]);
            run_child(self, pack[k], nro, "", &r);
            set_env("DM_AOT_PROFILE", NULL);
            char want_line[64];
            snprintf(want_line, sizeof want_line, "%s PASS\n", name);
            pass[k] = r.status == 0 && !strcmp(r.guest, want_line);
            have[k] = r.have_jit;
            jit[k] = r.jit[0];
            text[k] = read_text(misses[k]);
            if (r.guest != no_lines) free(r.guest);
            if (which == 0) {
                snprintf(rowf, sizeof rowf, "0x%llx indirect ", (unsigned long long)f0);
                snprintf(rowh, sizeof rowh, "0x%llx indirect ", (unsigned long long)h0);
                const int named = text[k] && strstr(text[k], rowf) && strstr(text[k], rowh);
                const int ok = k ? pass[k] && have[k] && jit[k] == 0 && !(text[k] && (strstr(text[k], rowf) || strstr(text[k], rowh)))
                                 : pass[k] && have[k] && jit[k] > 0 && named;
                snprintf(note, sizeof note, k ? "a first pack (guarded cases from the code): sum %llu, %llu JIT runs (want 0)%s"
                                              : "--no-guess: sum %llu, %llu JIT runs, the misses file %s",
                         (unsigned long long)want, jit[k], k ? "" : named ? "names F's case 0 and H's case 0 (indirect)" : "lacks F's case 0 or H's case 0");
                if (r.red[0] && strlen(note) + strlen(r.red) + 3 < sizeof note) { strcat(note, "; "); strcat(note, r.red); }
                row(name, k ? "guess" : "no-guess", ok, &r, note);
            }
        }
        if (which == 1) {   // the guess withdrawn: both packs run alike
            char a[800], b[800];
            miss_rows(text[0], a, sizeof a);
            miss_rows(text[1], b, sizeof b);
            const int ok = pass[0] && pass[1] && have[0] && have[1] && jit[0] == jit[1] && jit[0] > 0 && !strcmp(a, b);
            snprintf(note, sizeof note, "its guess withdrawn: sum %llu both ways, %llu and %llu JIT runs (--no-guess, guesses on), misses %s", (unsigned long long)want,
                     jit[0], jit[1], !strcmp(a, b) ? "the same" : "differ");
            row(name, "withdrawn", ok, &r, note);
        }
        free(text[0]);
        free(text[1]);
    }
}

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "--run")) return child(argc, argv);
    if (argc == 3 && !strcmp(argv[1], "--log-race")) return log_race((unsigned)atoi(argv[2]));
    const char *packs = NULL, *corpus = T_AOT_CORPUS, *nxaot = getenv("NXAOT");
    const char *only[32]; int nonly = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--packs") && i + 1 < argc) packs = argv[++i];
        else if (!strcmp(argv[i], "--corpus") && i + 1 < argc) corpus = argv[++i];
        else if (!strcmp(argv[i], "--nro") && i + 1 < argc && nonly < 32) only[nonly++] = argv[++i];
        else if (!strcmp(argv[i], "--nxaot") && i + 1 < argc) nxaot = argv[++i];
        else { fprintf(stderr, "usage: t_aot_host [--packs <dir>] [--corpus <dir>] [--nro <name>]... [--nxaot <nxaot>]\n"); return 2; }
    }
    if (!packs) packs = getenv("NXAOT_PACKS");
    if (!packs || !*packs) packs = T_AOT_PACKS;
    if (!*packs) { puts("note: no corpus packs (NXAOT_PACKS unset; dm/tools/aot/tests/build_packs.sh makes them in the nxnative-aot image): skipped"); return 0; }
    char path[1024];
    snprintf(path, sizeof path, "%s/corpus.tsv", corpus);
    char *tsv = read_text(path);
    if (!tsv) { printf("FAIL: cannot read %s (the corpus: --corpus)\n", path); return 1; }
    printf("t_aot_host: packs %s, corpus %s, JIT fallback %s\n", packs, corpus, NXHOST_JIT ? "built in (NXHOST_JIT)" : "not built in");
    printf("%-14s %-9s %-4s %3s %9s %6s  %s\n", "nro", "run", "ok", "st", "ms", "lines", "note");
    {   // 0. The capture's premise: every log line and every guest line whole when two threads write them at once (what the rows below read)
        char cmd[1200], *want = calloc((size_t)LOG_RACE_LINES, 24), note[700], diff[600];
        if (!want) abort();
        size_t o = 0;
        for (unsigned i = 0; i < LOG_RACE_LINES; i++) o += (size_t)sprintf(want + o, "log race %u\n", i);
#if defined(_WIN32)
        snprintf(cmd, sizeof cmd, "\"\"%s\" --log-race %d 2>&1\"", argv[0], LOG_RACE_LINES);
#else
        snprintf(cmd, sizeof cmd, "'%s' --log-race %d 2>&1", argv[0], LOG_RACE_LINES);
#endif
        result r;
        run_cmd(cmd, &r);
        const int ok = r.status == 0 && !strcmp(r.guest, want) && r.ndm >= LOG_RACE_LINES && !r.spliced;
        diff_note(want, &r, diff, sizeof diff);
        snprintf(note, sizeof note, "%u log lines from one thread, guest lines from another%s%s%s", r.ndm, ok ? "" : diff, r.red[0] ? "; " : "", r.red);
        row("log-lines", "race", ok, &r, note);
        free(r.guest); free(want);
    }
    for (char *ln = tsv, *next; ln && *ln; ln = next) {
        next = strchr(ln, '\n');
        if (next) *next++ = 0;
        if (ln[0] == '#' || !ln[0]) continue;
        char nro[64], program[64];
        if (sscanf(ln, "%63[^\t]\t%63[^\t]", nro, program) != 2) continue;
        char name[64]; snprintf(name, sizeof name, "%s", nro);
        if (strstr(name, ".nro")) *strstr(name, ".nro") = 0;
        int want = !nonly;
        for (int i = 0; i < nonly; i++) want |= !strcmp(only[i], name) || !strcmp(only[i], nro);
        if (!want) continue;
        char pack[1024], nrop[1024], exp_path[1024], note[1400], diff[700];
        snprintf(pack, sizeof pack, "%s/%s" PACK_EXT, packs, name);
        snprintf(nrop, sizeof nrop, "%s/nro/%s", corpus, nro);
        snprintf(exp_path, sizeof exp_path, "%s/expected/%s.out", corpus, program);
        char *expected = read_text(exp_path);
        if (!expected) { checks++; fails++; printf("%-14s FAIL: no %s\n", name, exp_path); continue; }
        FILE *pf = fopen(pack, "rb");
        if (!pf) { checks++; fails++; printf("%-14s FAIL: no pack %s\n", name, pack); free(expected); continue; }
        fclose(pf);
        result r;
        // 1. The plain run
        run_child(argv[0], pack, nrop, "", &r);
        const int jit = !NXHOST_JIT && is_jit_needed(name);
        int ok;
        if (r.status == 0 && !strcmp(r.guest, expected)) {
            ok = 1;
            snprintf(note, sizeof note, "%zu bytes of lines as expected%s", r.nguest, jit ? "; listed as needing the JIT, ran without it: update JIT_NEEDED" : "");
        } else if (jit && r.status == DM_AOT_JIT_NEEDED && strstr(r.red, "needs the JIT fallback") && !strncmp(r.guest, expected, r.nguest)) {
            ok = 1;
            snprintf(note, sizeof note, "needs the JIT (status 3 after %zu of %zu bytes): %s", r.nguest, strlen(expected), r.red);
        } else {
            ok = 0;
            size_t k = 0; while (k < r.nguest && r.guest[k] == expected[k]) k++;
            diff_note(expected, &r, diff, sizeof diff);
            snprintf(note, sizeof note, "lines differ from byte %zu of %zu (got %zu)%s%s%s", k, strlen(expected), r.nguest, diff, r.red[0] ? "; " : "", r.red);
        }
        row(name, "plain", ok, &r, note);
        const int plain_ji = r.have_ji, plain_on_use = (int)r.ji[2];
        free(r.guest);
        if (NXHOST_JIT && !strcmp(program, "fp")) {
            // 1b. The JIT's first use (aot_run.c run_jit, jit.h dm_jit_prepare): its instance made ahead on a helper thread from the pack's load, with dynarmic's decoder table.
            // The run starts once it is ready, as a game's boot gives it time before its first miss: the first use takes it, made nothing and waited for nothing; and in the
            // plain run above, whose miss comes a millisecond after the load, the first use waited for it rather than make one. The time bound is generous (a ready
            // instance is a pointer taken under a lock; making one took 35 ms and the decoder table 181 to 197 ms more before)
            run_child(argv[0], pack, nrop, "--jit-ready", &r);
            ok = r.status == 0 && !strcmp(r.guest, expected) && r.have_ji && plain_ji && r.ji_ready == 1 && r.ji[1] >= 1 && r.ji[2] == 0 && r.ji[3] == 0 && plain_on_use == 0 &&
                 r.ji_first_ms < 20.0;
            diff_note(expected, &r, diff, sizeof diff);
            snprintf(note, sizeof note, "ready before the run %d: %llu made ahead, %llu taken, %llu made on first use, %llu waits, first use %.3f ms; plain run: %d made on first use%s%s%s",
                     r.ji_ready, r.ji[0], r.ji[1], r.ji[2], r.ji[3], r.ji_first_ms, plain_on_use, diff, r.red[0] ? "; " : "", r.red);
            row(name, "jit-start", ok, &r, note);
            free(r.guest);
        }
        if (!strcmp(program, "excl")) {
            // 2. The threaded totals again; 3. the pause test
            for (int i = 0; i < EXCL_RUNS; i++) {
                run_child(argv[0], pack, nrop, "", &r);
                ok = r.status == 0 && !strcmp(r.guest, expected) && r.have_stat && r.stat[0] >= 3;
                diff_note(expected, &r, diff, sizeof diff);
                snprintf(note, sizeof note, "%llu threads, %llu SVCs%s%s%s", r.stat[0], r.stat[1], diff, r.red[0] ? "; " : "", r.red);
                row(name, "threads", ok, &r, note);
                free(r.guest);
            }
            unsigned polls = 0, invariant = 0;
            for (unsigned seed = 1; seed <= PAUSE_SEEDS; seed++) {
                char extra[200];
                // The loop's cap only (the tester ends it by count through excl_pause_stop); the 30 s limit fails a row whose pauses stopped landing
                snprintf(extra, sizeof extra, "--pause %d --seed %u --set excl_pause_iterations=100000000000", PAUSES, seed);
                run_child(argv[0], pack, nrop, extra, &r);
                ok = r.status == 0 && !strcmp(r.guest, expected) && r.have_pt && r.pt[1] == PAUSES && r.pt[3] == 0;
                polls += r.have_stat ? (unsigned)r.stat[5] : 0; invariant += r.have_pt ? r.pt[2] : 0;
                diff_note(expected, &r, diff, sizeof diff);
                snprintf(note, sizeof note, "seed %u: %u pauses (%u threaded), %llu at polls, %u invariant checks, %u inconsistent%s%s%s", seed, r.pt[0], r.pt[1],
                         r.stat[5], r.pt[2], r.pt[3], diff, r.red[0] ? "; " : "", r.red);
                row(name, "pause", ok, &r, note);
                free(r.guest);
            }
            checks++;
            if (!polls || !invariant) { fails++; printf("%-14s %-9s FAIL over %d seeds: %u stops at polls, %u invariant checks (want both above 0)\n", name, "pauses", PAUSE_SEEDS, polls, invariant); }
            else printf("%-14s %-9s ok   over %d seeds: %u stops at polls, %u register-invariant checks\n", name, "pauses", PAUSE_SEEDS, polls, invariant);
        }
        if (!strcmp(program, "fnptr")) {
            // 4. A guest call into the host-function page
            run_child(argv[0], pack, nrop, "--hidden-host", &r);
            ok = r.status == 0 && !strcmp(r.guest, expected) && r.have_stat && r.stat[7] >= 1 && r.stat[2] >= 1;
            diff_note(expected, &r, diff, sizeof diff);
            snprintf(note, sizeof note, "hidden_ptr -> the host: %llu calls of the host function, %llu host calls%s%s%s", r.stat[7], r.stat[2], diff, r.red[0] ? "; " : "", r.red);
            row(name, "host-call", ok, &r, note);
            free(r.guest);
        }
        if (!strcmp(program, "int_ops") || !strcmp(program, "excl") || (NXHOST_JIT && !strcmp(program, "fiber"))) {
            // 5. Dirty tracking: every data page write-protected, opened by the hook at its first write and protected again every 0.5 ms (excl: on four threads,
            // their stacks in its bss; fiber: stores of the JIT fallback too, whose fastmem handler must come after the hook)
            run_child(argv[0], pack, nrop, "--dirty", &r);
            ok = r.status == 0 && !strcmp(r.guest, expected) && r.have_stat && r.stat[8] >= 1 && r.stat[10] == 0 && (r.stat[9] >= 1 || !strcmp(program, "fiber"));
            diff_note(expected, &r, diff, sizeof diff);
            snprintf(note, sizeof note, "%llu write faults resumed by the hook: %llu stores of compiled code, %llu elsewhere (the JIT's code, the host), %llu in the JIT's callbacks%s%s%s",
                     r.stat[8], r.stat[9], r.stat[8] - r.stat[9], r.stat[10], diff, r.red[0] ? "; " : "", r.red);
            row(name, "dirty", ok, &r, note);
            free(r.guest);
        }
        if (!strcmp(program, "int_ops")) {
            // 6. The sampler's positions: the profile of a run with a hot loop names offsets inside it (design 12), and with nxaot it weighs the pack's code
            positions_rows(argv[0], name, pack, nrop, corpus, expected, nxaot);
        }
        free(expected);
    }
    free(tsv);
    int lrtail = !nonly;
    for (int i = 0; i < nonly; i++) lrtail |= !strcmp(only[i], "lrtail");
    if (lrtail) lrtail_row(argv[0], nxaot);   // 7. A br that is no tail call: the JIT's continuation comes back to compiled code
    int spkeep = !nonly;
    for (int i = 0; i < nonly; i++) spkeep |= !strcmp(only[i], "spkeep");
    if (spkeep) spkeep_row(argv[0], nxaot);   // 7b. A callee that keeps SP leaves no SP of its own in the block for a caller that reloads it
    int fpret = !nonly;
    for (int i = 0; i < nonly; i++) fpret |= !strcmp(only[i], "fpret");
    if (fpret) fpret_row(argv[0], nxaot);     // 7c. A return under another FPCR, which the frames propagate, is no non-local transfer
    int fpresume = !nonly;
    for (int i = 0; i < nonly; i++) fpresume |= !strcmp(only[i], "fpresume");
    if (fpresume) fpresume_row(argv[0], nxaot);   // 7d. A resume point takes a transfer only under the packs' FPCR
    int pollkeep = !nonly;
    for (int i = 0; i < nonly; i++) pollkeep |= !strcmp(only[i], "pollkeep");
    if (pollkeep) pollkeep_row(argv[0], nxaot);   // 7e. A register a poll stored with a callee's value of the moment is stored again at its return
    int bindstub = !nonly;
    for (int i = 0; i < nonly; i++) bindstub |= !strcmp(only[i], "bindstub");
    if (bindstub) bindstub_rows(argv[0], nxaot);  // 7f. PLT stubs bound to their own module: the host's bindings file, the pack built with it, a wrong one refused
    int guestbt = !nonly;
    for (int i = 0; i < nonly; i++) guestbt |= !strcmp(only[i], "guestbt");
    if (guestbt) guestbt_rows(argv[0], nxaot);   // 8. The guest's call site and backtrace in an abort, an svcBreak and a fault
    int polls = !nonly;
    for (int i = 0; i < nonly; i++) polls |= !strcmp(only[i], "polls");
    if (polls) polls_rows(argv[0], nxaot);    // 9. The polls left out of bounded loops, and the loops that must keep theirs: every pause prompt
    int cutcall = !nonly;
    for (int i = 0; i < nonly; i++) cutcall |= !strcmp(only[i], "cutcall");
    if (cutcall) cutcall_rows(argv[0], nxaot);
    int casecall = !nonly;
    for (int i = 0; i < nonly; i++) casecall |= !strcmp(only[i], "casecall");
    if (casecall) casecall_rows(argv[0], nxaot);
    int guess = !nonly;
    for (int i = 0; i < nonly; i++) guess |= !strcmp(only[i], "guesscase") || !strcmp(only[i], "guessbad");
    if (guess) guess_rows(argv[0], nxaot);   // 12. Guarded cases from a function's own code: none of the cases in the JIT; a guess that would lose a table withdrawn   // 11. A miss as a case of its function's br: the function kept whole; a case reached from outside, an entry   // 10. A call whose return starts another function; a call of a function the summaries say does not return, which returns
    printf("t_aot_host: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
