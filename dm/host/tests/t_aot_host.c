// The x86-64 execution engine inside the host (docs/design/aot-recompiler.md 2.4 to 11; dm/host/aot_*.c), on the corpus of dm/tools/aot: self-checking NROs
// with no game files, recompiled by `nxaot build` into packs (dm/tools/aot/tests/build_packs.sh), run through the host's own parts. The host's loader maps
// each NRO one to one in the arena below 2^39 and relocates it (loader.c loader_add_image), the pack is loaded with its identity checked (aot_arena.c),
// dm_aot_run enters _start on the main guest thread, svcOutputDebugString is answered by hle_svc.c's svc_dispatch (its "[guest] " log lines are the
// program's output), and the threads of the excl program are hle_thread.c's guest threads (svcCreateThread, ArbitrateLock, WaitForAddress,
// WaitSynchronization, ...) on host threads with a state block each.
// Each run is a child process (this program with --run), as a game is a process; the parent compares its lines with corpus/expected/<program>.out and its
// exit status, and prints a table. The runs:
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
//   t_aot_host [--packs <dir>] [--corpus <dir>] [--nro <name>]...
//     the packs: --packs, else the environment variable NXAOT_PACKS, else the CMake option NXAOT_PACKS; with none, a note and exit 0 (CTest stays green where
//     no packs exist: the Windows runner, a build without the aot image)
//   t_aot_host --run <pack> <nro> [--set <symbol>=<value>]... [--hidden-host] [--dirty] [--pause N --seed S]      one run (the child)
#include "../aot.h"
#include "../aot_hostfn.h"
#if NXHOST_JIT
#include "rt/jit.h"   // dm_jit_access_pc: a fault inside the JIT's memory callbacks
#endif
#include <stdarg.h>
#include <stdatomic.h>
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
enum { EXCL_RUNS = 5, PAUSES = 50, PAUSE_SEEDS = 3 };

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

typedef struct { const char *pack, *nro; int nsets; const char *sets[8]; int hidden, dirty; unsigned pause, seed; int status; double secs; } run_args;

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

static int child(int argc, char **argv) {
    run_args a;
    memset(&a, 0, sizeof a);
    a.pack = argv[2]; a.nro = argv[3];
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--set") && i + 1 < argc && a.nsets < 8) a.sets[a.nsets++] = argv[++i];
        else if (!strcmp(argv[i], "--hidden-host")) a.hidden = 1;
        else if (!strcmp(argv[i], "--dirty")) a.dirty = 1;
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

// ---------- The parent ----------
typedef struct { int status; char *guest; size_t nguest; char red[400]; double secs; unsigned long long stat[12]; int have_stat; unsigned pt[5]; int have_pt; } result;

static void append(char **buf, size_t *n, size_t *cap, const char *s, size_t len) {
    if (*n + len + 1 > *cap) { *cap = (*n + len + 1) * 2; *buf = realloc(*buf, *cap); if (!*buf) abort(); }
    memcpy(*buf + *n, s, len); *n += len; (*buf)[*n] = 0;
}

static int run_child(const char *self, const char *pack, const char *nro, const char *extra, result *r) {
    char cmd[4096];
#if defined(_WIN32)
    snprintf(cmd, sizeof cmd, "\"\"%s\" --run \"%s\" \"%s\" %s 2>&1\"", self, pack, nro, extra);
#else
    snprintf(cmd, sizeof cmd, "'%s' --run '%s' '%s' %s 2>&1", self, pack, nro, extra);
#endif
    memset(r, 0, sizeof *r);
    FILE *f = popen(cmd, "r");
    if (!f) { snprintf(r->red, sizeof r->red, "popen failed"); r->status = -1; return -1; }
    char line[8192]; size_t cap = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "[guest] ", 8)) append(&r->guest, &r->nguest, &cap, line + 8, strlen(line + 8));
        else if (strstr(line, "[RED]") && !r->red[0]) { snprintf(r->red, sizeof r->red, "%s", strstr(line, "[RED]")); r->red[strcspn(r->red, "\n")] = 0; }
        if (!strncmp(line, "t_aot_host: status ", 19)) {
            r->have_stat = sscanf(line, "t_aot_host: status %*d after %lf s: %llu threads, %llu SVCs, %llu host calls, %llu dispatcher misses, %llu calls into another pack, "
                                        "%llu pauses at polls, %llu JIT addresses, hidden %llu, dirty faults %llu (%llu by compiled code, %llu in the JIT's memory callbacks)", &r->secs, &r->stat[0],
                                  &r->stat[1], &r->stat[2], &r->stat[3], &r->stat[4], &r->stat[5], &r->stat[6], &r->stat[7], &r->stat[8], &r->stat[9], &r->stat[10]) == 12;
        }
        if (!strncmp(line, "t_aot_host: pause test: ", 24))
            r->have_pt = sscanf(line, "t_aot_host: pause test: %u pauses (%u with more than one thread running), %u register-invariant checks, %u inconsistent, %u exited",
                                &r->pt[0], &r->pt[1], &r->pt[2], &r->pt[3], &r->pt[4]) == 5;
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

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "--run")) return child(argc, argv);
    const char *packs = NULL, *corpus = T_AOT_CORPUS;
    const char *only[32]; int nonly = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--packs") && i + 1 < argc) packs = argv[++i];
        else if (!strcmp(argv[i], "--corpus") && i + 1 < argc) corpus = argv[++i];
        else if (!strcmp(argv[i], "--nro") && i + 1 < argc && nonly < 32) only[nonly++] = argv[++i];
        else { fprintf(stderr, "usage: t_aot_host [--packs <dir>] [--corpus <dir>] [--nro <name>]...\n"); return 2; }
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
        char pack[1024], nrop[1024], exp_path[1024], note[600];
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
            snprintf(note, sizeof note, "lines differ from byte %zu of %zu (got %zu)%s%s", k, strlen(expected), r.nguest, r.red[0] ? "; " : "", r.red);
        }
        row(name, "plain", ok, &r, note);
        free(r.guest);
        if (!strcmp(program, "excl")) {
            // 2. The threaded totals again; 3. the pause test
            for (int i = 0; i < EXCL_RUNS; i++) {
                run_child(argv[0], pack, nrop, "", &r);
                ok = r.status == 0 && !strcmp(r.guest, expected) && r.have_stat && r.stat[0] >= 3;
                snprintf(note, sizeof note, "%llu threads, %llu SVCs%s%s", r.stat[0], r.stat[1], r.red[0] ? "; " : "", r.red);
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
                snprintf(note, sizeof note, "seed %u: %u pauses (%u threaded), %llu at polls, %u invariant checks, %u inconsistent%s%s", seed, r.pt[0], r.pt[1],
                         r.stat[5], r.pt[2], r.pt[3], r.red[0] ? "; " : "", r.red);
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
            snprintf(note, sizeof note, "hidden_ptr -> the host: %llu calls of the host function, %llu host calls%s%s", r.stat[7], r.stat[2], r.red[0] ? "; " : "", r.red);
            row(name, "host-call", ok, &r, note);
            free(r.guest);
        }
        if (!strcmp(program, "int_ops") || !strcmp(program, "excl") || (NXHOST_JIT && !strcmp(program, "fiber"))) {
            // 5. Dirty tracking: every data page write-protected, opened by the hook at its first write and protected again every 0.5 ms (excl: on four threads,
            // their stacks in its bss; fiber: stores of the JIT fallback too, whose fastmem handler must come after the hook)
            run_child(argv[0], pack, nrop, "--dirty", &r);
            ok = r.status == 0 && !strcmp(r.guest, expected) && r.have_stat && r.stat[8] >= 1 && r.stat[10] == 0 && (r.stat[9] >= 1 || !strcmp(program, "fiber"));
            snprintf(note, sizeof note, "%llu write faults resumed by the hook: %llu stores of compiled code, %llu elsewhere (the JIT's code, the host), %llu in the JIT's callbacks%s%s",
                     r.stat[8], r.stat[9], r.stat[8] - r.stat[9], r.stat[10], r.red[0] ? "; " : "", r.red);
            row(name, "dirty", ok, &r, note);
            free(r.guest);
        }
        free(expected);
    }
    free(tsv);
    printf("t_aot_host: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
