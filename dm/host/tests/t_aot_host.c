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
//     row inside it (its tier: nxaot makes a function with a sample anywhere in it hot) and writes the same samples in the form the sampler wrote before positions
//     (each function's at its entry). The parent checks the profile: every row `0x<offset> <samples>` under the module's header, the hottest function ref_crc (by
//     corpus/nro/<name>.syms.tsv), most of its samples inside its loops (the range of its backward branches, read from the NRO's words) and away from its entry, at
//     several instructions. With nxaot (--nxaot or the environment variable NXAOT; the aot image), the profile round-trips: `nxaot build --profile` weighs functions
//     (its `aot: profile weights:` line), NXAOT_PROFILE_WEIGHTS=0 gives another pack, the same bytes as the entry form's (the tiers alone)
//   t_aot_host [--packs <dir>] [--corpus <dir>] [--nro <name>]... [--nxaot <nxaot>]
//     the packs: --packs, else the environment variable NXAOT_PACKS, else the CMake option NXAOT_PACKS; with none, a note and exit 0 (CTest stays green where
//     no packs exist: the Windows runner, a build without the aot image)
//   t_aot_host --run <pack> <nro> [--set <symbol>=<value>]... [--hidden-host] [--dirty] [--pause N --seed S] [--positions]      one run (the child)
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
// msr fpcr, except-gcc's opcode switch that discovery leaves unbounded)
static const char *const JIT_NEEDED[] = {"fp-gcc", "fp-clang", "fiber-gcc", "fiber-clang", "except-gcc"};
enum { EXCL_RUNS = 5, PAUSES = 50, PAUSE_SEEDS = 3, LOG_RACE_LINES = 20000 };

void guest_thread_init(void);          // thread.c
uint32_t svc_thread_main_init(void);   // hle_thread.c
uint32_t thr_cur_handle(void);
uint64_t svc_dispatch(int num, uint64_t *regs, uint64_t orig_addr);   // hle_svc.c
uint64_t t_aot_hidden(uint64_t x);   // t_aot_host_entries.c
void t_aot_pause_tester(uint64_t arg);
extern _Atomic uint64_t g_t_aot_hidden_calls;

// ---------- The child: one run ----------
static struct {
    unsigned pauses, seed, threaded, done, invariant, inconsistent, exited;
    uint32_t self;
    uint64_t head, text_lo, text_hi;
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

void t_aot_pause_body(uint64_t arg) {
    (void)arg;
    g_pt.self = thr_cur_handle();
    uint64_t seed = 0x9E3779B97F4A7C15ull * (g_pt.seed + 1);
    uint32_t hs[64];
    while (dm_aot_stop_status() < 0 && g_pt.threaded < g_pt.pauses) {
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
    if (a->hidden) {   // fnptr's hidden_ptr: the guest-visible address of a host function (2.4)
        uint64_t *hp = module_sym(m, "hidden_ptr");
        if (!hp) { printf("[RED] t_aot_host: no hidden_ptr in %s\n", name); return NULL; }
        *hp = dm_guest_fn((const void *)t_aot_hidden);
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
        g_pt.text_lo = (uint64_t)(uintptr_t)m->base; g_pt.text_hi = g_pt.text_lo + text_end;
        const uint64_t stack = (uint64_t)(uintptr_t)dm_guest_alloc(0x10000);
        uint64_t r[32] = {0, dm_guest_fn((const void *)t_aot_pause_tester), 0, stack + 0x10000, 44, (uint64_t)-2};
        svc_dispatch(0x08, r, 0);
        if (r[0]) { printf("[RED] t_aot_host: svcCreateThread for the pause tester: %#llx\n", (unsigned long long)r[0]); return NULL; }
        uint64_t s[32] = {r[1]};
        svc_dispatch(0x09, s, 0);
    }
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
    for (char *line = text, *next; line && line < text + len && *line; line = next) {
        next = memchr(line, '\n', (size_t)(text + len - line));
        if (next) *next++ = 0;
        else next = text + len;
        unsigned long long off, cnt;
        if (line[0] == '#' || sscanf(line, "%llx %llu", &off, &cnt) != 2) continue;
        if (nrows == cap && !(rows = realloc(rows, (cap = cap ? 2 * cap : 256) * sizeof *rows))) abort();
        rows[nrows++] = (prow){off, cnt};
    }
    qsort(rows, nrows, sizeof *rows, prow_cmp);
    _Atomic uint64_t *c = dm_aot_pack_samples(p);
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
        else if (!strcmp(argv[i], "--dirty")) a.dirty = 1;
        else if (!strcmp(argv[i], "--positions")) a.positions = 1;
        else if (!strcmp(argv[i], "--pause") && i + 1 < argc) a.pause = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) a.seed = (unsigned)atoi(argv[++i]);
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
    if (a.pause)
        printf("t_aot_host: pause test: %u pauses (%u with more than one thread running), %u register-invariant checks, %u inconsistent, %u exited%s%s\n",
               g_pt.done, g_pt.threaded, g_pt.invariant, g_pt.inconsistent, g_pt.exited, g_pt.inconsistent ? "; first: " : "", g_pt.bad);
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
    unsigned ndm, spliced; char splice[300];   // "[dm] " lines; lines holding a "[guest] " line after their start (the first of them)
    char pos[700], prof[900];                  // the positions' check of the child (--positions), the sampler's summary line (aot_profile.c)
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
        if (!strncmp(line, "t_aot_host: pause test: ", 24))
            r->have_pt = sscanf(line, "t_aot_host: pause test: %u pauses (%u with more than one thread running), %u register-invariant checks, %u inconsistent, %u exited",
                                &r->pt[0], &r->pt[1], &r->pt[2], &r->pt[3], &r->pt[4]) == 5;
        const char *pl = strstr(line, "t_aot_host: positions: "), *sl = strstr(line, "aot: profile: ");
        if (pl && !r->pos[0]) { snprintf(r->pos, sizeof r->pos, "%s", pl); r->pos[strcspn(r->pos, "\n")] = 0; }
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
                   nrows > 0 && crc >= 0 && hottest == crc && loops && in_loop * 2 > in_fn && (in_fn - at_entry) * 2 > in_fn && offsets_in >= 3;
    snprintf(note, sizeof note, "%zu rows, %llu samples (%llu of %llu in compiled code at an instruction); ref_crc [%#llx, %#llx) %llu: %llu in its loops [%#llx, %#llx], %llu at its entry, "
             "%d offsets, the hottest of %d functions%s; %llu code bytes of %llu functions: %llu at an instruction, %llu at none, %llu outside their function (%llu read otherwise "
             "by the host); %zu of %zu sampled functions with a row inside them%s%s", nrows, total, at_ins, code, crc >= 0 ? (unsigned long long)fns[crc].lo : 0ull,
             crc >= 0 ? (unsigned long long)fns[crc].hi : 0ull, in_fn, in_loop, (unsigned long long)llo, (unsigned long long)lhi, at_entry, offsets_in, nf,
             hottest == crc ? "" : " (not the hottest)", bytes, nfns, placed, none, crossed, mism, kept, sampled, run_ok ? "" : "; the run's lines or status differ", r.red[0] ? "; " : "");
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
        free(r.guest);
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
                snprintf(extra, sizeof extra, "--pause %d --seed %u --set excl_pause_iterations=200000000", PAUSES, seed);
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
    printf("t_aot_host: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
