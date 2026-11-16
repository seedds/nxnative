// The sampler of the x86-64 execution engine (aot.h, "The sampler"; dm/tools/aot/README.md, "Tiers by profile" and "The sample profile at tier H"): DM_AOT_PROFILE=<folder> samples
// every thread that runs guest code at DM_AOT_PROFILE_HZ (1000 by default) per second of its own CPU time and writes, at process exit, the profile `nxaot build --profile` reads:
// <folder>/<module>.tsv per module with samples, one line `<module offset> <samples>` per sampled guest instruction (design 12's sampled instruction offsets), under a line per
// sampled function (the file, below). The packer
// (scripts/build_ipa_from_nsp.py --platform linux / windows) takes <NSP folder>/<id>-work/profile/<module>.tsv for the module's next pack: its functions with samples at tier H, the
// others at tier C (balanced mode), and the samples' positions inside each function at H weigh its code (branch weights, entry and call counts). Beside each profile the writer puts
// the JIT fallback's misses, <folder>/<module>.misses.tsv (write_misses; aot.h, "The JIT fallback's misses"), which the packer gives nxaot build --misses.
// With DM_AOT_PROFILE_HOST=1 the samples in the host are also counted by their native pc, and DM_CTL's `profmark <path>` writes them with every thread's totals at that
// moment (dm_aot_profile_mark, below: where a game's threads spend their CPU time in a stretch of play).
// A sample is counted by count_pc, the same on both systems, from the thread's interrupted rip:
//   in a pack's function       that function (aot_arena.c dm_aot_fn_of_code: the function map, a split function's parts and a group's functions each to their own entry), and the guest
//                              instruction the pack's positions give for the pc (dm_aot_offset_of_code: the instruction whose code holds it, in code inlined into the function the call
//                              the inlining replaced; include/nxaot/aot_module.h, "The sampler's positions"): none in code they name no instruction for (line 0, a standard entry), in
//                              a pack without positions, or when they would name one outside the function (counted apart: none expected)
//   elsewhere, in a host call  the guest function that called it (dm_aot_thread.prof_at: the call's return address, an SVC's instruction; aot_run.c sets it), at no instruction: the
//                              time a host call takes is no measure of how often its block runs, which is what the positions weigh
//   in the JIT fallback        counted as such per thread (dynarmic keeps the guest pc in host registers while a block runs: not readable from here)
//   anything else              the host (the run time, a host call for code no pack holds, a thread between guest calls)
// count_pc allocates nothing and takes no lock: a function's count and an instruction's are relaxed atomic adds into their pack's arrays (made at the pack's load, aot_arena.c: one per
// function, one per guest instruction of the text), the lookup a binary search of the pack's function map and of the function's positions; a thread's totals are fields of its own
// record, which is never freed.
// The file: a row per guest instruction with samples, and a row at its entry for a function with samples at none of its instructions (all of them in host calls and SVCs, in code
// the positions name none for, or in a pack without positions), the form the sampler wrote before positions: nxaot makes a function with a sample anywhere in it hot (tier H), so
// such a function keeps its tier, and does not weigh a function sampled only at its entry. A function with samples at its instructions has no other row: the rest of its samples
// (its host calls, its code with no instruction) would make its entry block look hotter than its loops (the header counts them). Each sampled function's rows follow its line
// `# function 0x<entry> <samples> <rows>`: its samples are all of them (at its instructions, in its host calls and SVCs, in its code with no instruction: the sum the sampler wrote at
// its entry before positions), its rows the lines after it. nxaot skips the line as a comment; the packer and the launcher count the sampled functions (nxaot's tier H) by these
// lines, not by the rows, whose number grows with a session's length inside the same functions. Each session writes the file anew (no merge with an earlier session's)
// Linux: each thread arms a timer on its own CPU-time clock (timer_create with CLOCK_THREAD_CPUTIME_ID, SIGEV_THREAD_ID) when it makes its state block (aot_thread.c thread_make),
// and disarms it before the block goes. Its SIGPROF runs on the thread's alternate signal stack (aot_fault.c crash_thread_init) and reads the interrupted rip from the signal context.
// The weight of a sample is 1 + the timer's overrun: the kernel checks CPU-time timers at its ticks (HZ 250 gives 4 expirations a check), so the counts stay in milliseconds of CPU time.
// Windows has no per-thread CPU-time timer that interrupts the thread, so one sampler thread does it from outside. A guest thread registers itself (dm_aot_profile_thread_begin, from
// thread_make: only threads the engine made state blocks for are ever suspended) with a handle of its own (OpenThread: suspend, get context, query) and its CPU time so far. Every
// 1 / DM_AOT_PROFILE_HZ seconds of wall time the sampler, under the registry's lock, reads each registered thread's CPU time (QueryThreadCycleTime: the thread's cycles in units of
// the invariant TSC, which the sampler measures against QueryPerformanceCounter at its start; GetThreadTimes is charged at the scheduler's tick, 15.6 ms, too coarse) and adds what
// it ran since the last look to the thread's credit. A thread whose credit holds a whole period (1 / DM_AOT_PROFILE_HZ s of CPU time) and that ran since the last look is sampled
// once, with the weight of the whole periods in its credit (the rest is kept): SuspendThread, GetThreadContext with CONTEXT_CONTROL (which also waits until the suspension took
// effect), Rip to count_pc, ResumeThread. A thread that sleeps accrues no credit and is never suspended, as no Linux timer fires on it; the counts stay in periods of CPU time
// however late the sampler wakes. The difference from Linux: the sample lands where the thread is when the sampler looks, not where its CPU time ran out (a thread that ran and
// then blocked within the last period is sampled in its wait, counted as the host).
// While a thread is suspended the sampler only calls the kernel (GetThreadContext, ResumeThread) and count_pc: no allocation, no lock, no C library and nothing of the loader, so a
// thread suspended while it holds the heap's lock, a stdio lock or the loader lock (a host thread's key destructor runs under it at the thread's end, before it unregisters)
// cannot stop the sampler. The registry's lock is held around each pass and taken by a thread to register or unregister, never while that thread is suspended (it is not
// suspended while it waits for the lock, and the sampler holds it); unregistering closes the handle under it, so the sampler never touches a thread whose block is gone.
// Overhead (reasoned, not measured: no Windows machine here): per pass one QueryThreadCycleTime per registered thread (a system call, about a microsecond); per sample a
// suspension, a context read and a resume (three system calls and, when the thread runs on another core, an interrupt to stop it: some tens of microseconds the sampled thread
// stands still). At 1000 Hz that is a few percent of a busy thread's time, about what Linux's signal delivery and handler cost, and nothing for an idle one
#include "aot.h"
#include <errno.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <windows.h>
#include <x86intrin.h>   // __rdtsc
#else
#include <dlfcn.h>     // dladdr: the host's sampled pcs named by object file at a mark
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#define PROF_HZ_DEFAULT 1000
#define PROF_HZ_MAX 10000

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_on;              // DM_AOT_PROFILE is set and the sampler runs
static void init_once(void);
void dm_aot_profile_init(void) { pthread_once(&g_once, init_once); }
int dm_aot_profile_on(void) { dm_aot_profile_init(); return g_on; }

static char g_dir[1024];
static unsigned g_hz = PROF_HZ_DEFAULT;

// A thread's record: its totals, written by whoever samples it only (its own handler on Linux, the sampler thread under g_lock on Windows); kept to the end (the writer reads it)
typedef struct prof_thread {
    struct prof_thread *next;   // every thread sampled (g_lock)
    long tid;
    uint64_t samples, code, hostcall, jit, host;   // all, in a pack's function, in a host call or SVC of one, in the JIT fallback, in the host
    int idx;                    // its number in registration order (the host's samples by pc keep it: DM_AOT_PROFILE_HOST)
    uint64_t placed, crossed;   // of code: at a guest instruction of the pack's positions; at one outside its function (not counted at it: none expected)
    // The JIT fallback's runs on this thread and its CPU time in them (dm_aot_profile_jit_run, below): all; begun under another FPCR than the packs'; the stretches of CPU time
    // under another FPCR (from the first run begun under it, or ended under it, to the end of the run that leaves it), their number and the last such FPCR
    uint64_t jit_runs, jit_ns, fpcr_runs, fpcr_ns, other_n, other_ns, other_t0;
    uint32_t fpcr_last;
    int in_other;
    uint64_t origin;            // the miss that began the thread's current JIT continuation (dm_aot_profile_jit_origin): its runs' time goes to it
    uint32_t origin_reason;
    char name[32];              // the thread's name at its first JIT run (Linux; empty elsewhere)
#if defined(_WIN32)
    HANDLE h;                   // the thread, while it is registered (g_lock); NULL once it unregistered
    const dm_aot_thread *t;     // its state block, while h is set (prof_at and depth are read from it while the thread is suspended)
    uint64_t cycles, credit;    // its CPU time (QueryThreadCycleTime) at the last look; TSC cycles of CPU time not sampled yet
#else
    timer_t timer;
    int armed;
    uint64_t stack_lo, stack_hi;   // the thread's stack (pthread_getattr_np at its registration), outside which count_caller reads nothing; 0 when unknown
#endif
} prof_thread;
static _Thread_local prof_thread *t_prof;   // the calling thread's record while it is sampled, NULL otherwise (the Linux handler then does nothing)
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static prof_thread *g_threads;
static int g_nthreads;
static _Atomic int g_sampling;              // 0 once the files are being written: nothing more is counted
static _Atomic int g_written;

static void count(int pack, long fn, uint64_t w) {
    const dm_aot_pack *p = dm_aot_pack_at(pack);
    _Atomic uint64_t *c = p ? dm_aot_pack_samples(p) : NULL;
    if (c) atomic_fetch_add_explicit(&c[fn], w, memory_order_relaxed);
}

static int count_at(int pack, uint64_t off, uint64_t w) {   // a sample at the guest instruction at module offset off: 1 when counted
    const dm_aot_pack *p = dm_aot_pack_at(pack);
    uint64_t words = 0;
    _Atomic uint32_t *c = p ? dm_aot_pack_pos_samples(p, &words) : NULL;
    if (!c || off / 4 >= words) return 0;
    atomic_fetch_add_explicit(&c[off / 4], (uint32_t)w, memory_order_relaxed);
    return 1;
}

// ---------- The host's samples by pc (DM_AOT_PROFILE_HOST; DM_CTL profmark) ----------
// With DM_AOT_PROFILE_HOST=1 the samples count_pc puts on the host (in a host call or SVC of a compiled function, or elsewhere in the host) are also counted by their native pc
// and class and the thread's number, in a table of HOSTPC_N slots made when the switches are read: open addressing on a key set once by a compare-and-swap, the counts relaxed
// atomic adds, no lock and no allocation, as count_pc must be. A sample that finds no free slot within HOSTPC_PROBE of its hash is counted apart (g_hostpc_lost). On Linux a
// sample outside the host program (in the C library, the Vulkan driver) is also counted for the first word of the 64 at the top of the thread's stack that lies in the
// program's text (class 3, its caller as far as the stack shows: a return address most often, though a stale word can be one too). dm_aot_profile_mark writes the table
// with each pc's object file, its offset there (for addr2line) and the nearest exported symbol, after every sampled thread's totals: the difference of two marks around a
// stretch of play says where the game's threads spent their CPU time in it (their guest code, the JIT fallback, which host code: the NVN front end, the Vulkan backend, the
// driver called on the game's thread, the services)
#define HOSTPC_N (1u << 16)
#define HOSTPC_PROBE 64
typedef struct { _Atomic uint64_t key, n; } hostpc_slot;   // key: the pc (below 2^48) | thread number << 48 | class << 62 (1 in a host call or SVC, 2 elsewhere, 3 a caller); 0 while free
#define HOSTPC_PC(k) ((k) & ((1ull << 48) - 1))
#define HOSTPC_THREAD(k) ((int)((k) >> 48 & 0xFF))
static hostpc_slot *g_hostpc;
static _Atomic uint64_t g_hostpc_lost;
static void count_host(uint64_t pc, int thread, uint64_t cls, uint64_t w) {
    hostpc_slot *t = g_hostpc;
    if (!t) return;
    const uint64_t key = (pc & ((1ull << 48) - 1)) | (uint64_t)(thread & 0xFF) << 48 | cls << 62;
    uint32_t h = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 40) & (HOSTPC_N - 1);
    for (int i = 0; i < HOSTPC_PROBE; i++, h = (h + 1) & (HOSTPC_N - 1)) {
        uint64_t k = atomic_load_explicit(&t[h].key, memory_order_relaxed);
        if (!k && atomic_compare_exchange_strong_explicit(&t[h].key, &k, key, memory_order_relaxed, memory_order_relaxed)) k = key;   // k holds the winner's key otherwise
        if (k == key) { atomic_fetch_add_explicit(&t[h].n, w, memory_order_relaxed); return; }
    }
    atomic_fetch_add_explicit(&g_hostpc_lost, w, memory_order_relaxed);
}

// One sample of weight w at native pc on the thread r records, whose state block is t (NULL when it has none): the rules at the top of this file. Async-signal-safe (Linux runs
// it in the thread's own SIGPROF handler) and safe while the thread is suspended (Windows runs it on the sampler thread): no allocation, no lock
#if defined(_WIN32)
static void count_caller(prof_thread *r, uint64_t pc, uint64_t sp, uint64_t w) { (void)r; (void)pc; (void)sp; (void)w; }
#else
extern char __executable_start[], etext[];   // the host program's text (the GNU linkers' symbols)
static void count_caller(prof_thread *r, uint64_t pc, uint64_t sp, uint64_t w) {   // a sample outside the program: the first program address in the 64 words at the stack's top
    const uint64_t lo = (uint64_t)(uintptr_t)__executable_start, hi = (uint64_t)(uintptr_t)etext;
    if (!g_hostpc || !r->stack_hi || sp < r->stack_lo || sp >= r->stack_hi || (pc >= lo && pc < hi)) return;   // sp elsewhere (a signal's alternate stack): not read
    const uint64_t *s = (const uint64_t *)(uintptr_t)sp;   // the interrupted thread's own stack, mapped from sp to its end
    const uint64_t words = (r->stack_hi - sp) / 8 < 64 ? (r->stack_hi - sp) / 8 : 64;
    for (uint64_t i = 0; i < words; i++) if (s[i] >= lo && s[i] < hi) { count_host(s[i], r->idx, 3, w); return; }
}
#endif
static void count_pc(prof_thread *r, const dm_aot_thread *t, uint64_t pc, uint64_t sp, uint64_t w) {
    r->samples += w;
    int pack = -1;
    long fn = dm_aot_fn_of_code(pc, &pack);
    if (fn >= 0) {
        count(pack, fn, w);
        r->code += w;
        const int64_t off = dm_aot_offset_of_code(pc, pack, fn);
        if (off >= 0 && count_at(pack, (uint64_t)off, w)) r->placed += w;
        else if (off == -2) r->crossed += w;
        return;
    }
    const uint64_t at = fn != -2 && t && t->depth ? t->prof_at : 0;   // -2: the pack's run-time code (enter, jit_entry, the slow-path wrappers)
    if (at == DM_AOT_PROF_JIT) r->jit += w;
    else if (at > DM_AOT_PROF_JIT && (fn = dm_aot_fn_of_guest(at, &pack)) >= 0) { count(pack, fn, w); r->hostcall += w; count_host(pc, r->idx, 1, w); count_caller(r, pc, sp, w); }
    else { r->host += w; count_host(pc, r->idx, 2, w); count_caller(r, pc, sp, w); }
}

static void write_at_exit(void) { dm_aot_profile_write(); }

// ---------- The JIT fallback's runs and the FPCR ----------
// Besides the samples, the JIT fallback's own share of each thread's CPU time, measured rather than sampled, and what of it the FPCR caused (design 7.1: code runs compiled only
// under the FPCR the packs were specialised for; under any other, every guest instruction runs in the JIT fallback until the FPCR comes back). aot_run.c run_jit reports each run of
// dm_jit_run / dm_jit_run_past (the SVCs answered inside a run count in it; a host call or a compiled entry the run stops at does not): its CPU time on the thread's own clock, whether
// the FPCR was another than the packs' when it began and when it ended. Only with DM_AOT_PROFILE (two clock reads per run)
uint64_t dm_aot_profile_cpu_ns(void) {
#if defined(_WIN32)
    FILETIME c, e, k, u;   // the scheduler's charge (15.6 ms ticks): coarse, enough for the totals
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    return ((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) * 100;
#else
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}
// The JIT time by the miss that began it (dm_aot_profile_jit_origin): every run of a thread counts for the last miss that sent it to the JIT (a run going
// on, after compiled code it called returned into it, belongs to the continuation that miss began). A table in static memory with no lock: a row is claimed
// by a compare-and-swap of its key, its counts are atomic adds, so a thread's run never waits, and a thread the process's exit stops in the middle of an add
// holds nothing the writer then needs (a lock taken on every run was such a thing: under Wine one session in 20 faulted at exit, t_aot_module). At most
// ORIGINS rows (the rest counted apart); the profile's writer logs the heaviest (dm_aot_profile_write: "JIT time from")
#define ORIGINS 4096
typedef struct { _Atomic uint64_t key, runs, ns; _Atomic int named; char name[32]; } jit_origin;   // key: the miss's pc, its reason in the top 16 bits, plus 1
static jit_origin g_origins[ORIGINS];
static _Atomic uint64_t g_origin_lost_ns;
void dm_aot_profile_jit_origin(uint64_t pc, uint32_t reason) {
    prof_thread *r = t_prof;
    if (!r) return;
    r->origin = pc; r->origin_reason = reason;
}
static void origin_add(prof_thread *r, uint64_t d) {
    const uint64_t key = ((r->origin & 0xFFFFFFFFFFFFull) | (uint64_t)(r->origin_reason & 0xFFFF) << 48) + 1;
    for (unsigned h = (unsigned)((key >> 2) * 0x9E3779B97F4A7C15ull >> 52) & (ORIGINS - 1), k = 0; k < ORIGINS; h = (h + 1) & (ORIGINS - 1), k++) {
        jit_origin *o = &g_origins[h];
        uint64_t cur = atomic_load_explicit(&o->key, memory_order_acquire);
        if (!cur && atomic_compare_exchange_strong(&o->key, &cur, key)) cur = key;
        if (cur != key) continue;
        // The thread's name is read at its first run's end: the first thread with one names the row
        if (r->name[0] && !atomic_load_explicit(&o->named, memory_order_relaxed) && !atomic_exchange(&o->named, 1)) snprintf(o->name, sizeof o->name, "%s", r->name);
        atomic_fetch_add_explicit(&o->runs, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&o->ns, d, memory_order_relaxed);
        return;
    }
    atomic_fetch_add_explicit(&g_origin_lost_ns, d, memory_order_relaxed);
}
typedef struct { uint64_t pc, runs, ns; uint32_t reason; char name[32]; } origin_row;
static int origin_cmp(const void *a, const void *b) {
    const origin_row *x = a, *y = b;
    return x->ns < y->ns ? 1 : x->ns > y->ns ? -1 : 0;
}
static void origins_log(void) {
    static origin_row rows[ORIGINS];
    size_t n = 0;
    for (unsigned h = 0; h < ORIGINS; h++) {
        jit_origin *o = &g_origins[h];
        const uint64_t key = atomic_load_explicit(&o->key, memory_order_acquire);
        if (!key) continue;
        origin_row *w = &rows[n++];
        w->pc = (key - 1) & 0xFFFFFFFFFFFFull;
        w->reason = (uint32_t)((key - 1) >> 48);
        w->runs = atomic_load_explicit(&o->runs, memory_order_relaxed);
        w->ns = atomic_load_explicit(&o->ns, memory_order_relaxed);
        memcpy(w->name, o->name, sizeof w->name);
        w->name[sizeof w->name - 1] = 0;
    }
    qsort(rows, n, sizeof *rows, origin_cmp);
    for (size_t i = 0; i < n && i < 80; i++) {
        char at[64];
        dm_aot_guest_name(rows[i].pc, at, sizeof at);
        const char *word = dm_aot_miss_word(rows[i].reason);
        LOG("aot: profile: JIT time from %s (%s, thread '%s'): %llu runs, %.3f s", at, word ? word : "no miss", rows[i].name, (unsigned long long)rows[i].runs, rows[i].ns * 1e-9);
    }
    const uint64_t lost = atomic_load_explicit(&g_origin_lost_ns, memory_order_relaxed);
    if (n > 80 || lost) LOG("aot: profile: JIT time from %zu more origins, and %.3f s of origins the table had no room for", n > 80 ? n - 80 : 0, lost * 1e-9);
}
void dm_aot_profile_jit_run(uint64_t cpu0, uint32_t fpcr0, uint32_t fpcr1, int other0, int other1) {
    prof_thread *r = t_prof;
    if (!r) return;
    const uint64_t now = dm_aot_profile_cpu_ns(), d = now > cpu0 ? now - cpu0 : 0;
    r->jit_runs++; r->jit_ns += d;
    origin_add(r, d);
    if (other0) { r->fpcr_runs++; r->fpcr_ns += d; }
    if ((other0 || other1) && !r->in_other) { r->in_other = 1; r->other_n++; r->other_t0 = other0 ? cpu0 : now; }
    if (other1) r->fpcr_last = fpcr1;
    else if (other0) r->fpcr_last = fpcr0;
    if (!other1 && r->in_other) { r->in_other = 0; r->other_ns += now - r->other_t0; }
#if !defined(_WIN32)
    if (!r->name[0] && pthread_getname_np(pthread_self(), r->name, sizeof r->name)) r->name[0] = 0;
#endif
}

// The switches; 0 when the sampler may start
static int read_switches(void) {
    const char *dir = getenv("DM_AOT_PROFILE");
    if (!dir || !*dir) return -1;
    if (strlen(dir) >= sizeof g_dir - 32) { LOG("[RED] aot: DM_AOT_PROFILE: the folder's path is too long: no profile"); return -1; }
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    const char *hz = getenv("DM_AOT_PROFILE_HZ");
    if (hz && *hz) {
        const long v = strtol(hz, NULL, 10);
        if (v >= 1 && v <= PROF_HZ_MAX) g_hz = (unsigned)v;
        else LOG("[RED] aot: DM_AOT_PROFILE_HZ=%s is not 1 .. %d: %d", hz, PROF_HZ_MAX, PROF_HZ_DEFAULT);
    }
    const char *hp = getenv("DM_AOT_PROFILE_HOST");   // the host's samples by pc, for DM_CTL profmark (above count_pc)
    if (hp && *hp && strcmp(hp, "0") && !(g_hostpc = calloc(HOSTPC_N, sizeof *g_hostpc))) LOG("[RED] aot: DM_AOT_PROFILE_HOST: no memory for the table: the host's samples are not counted by pc");
    return 0;
}

#if defined(_WIN32)
// The TSC's rate against QueryPerformanceCounter over about 50 ms (QueryThreadCycleTime counts in the TSC's cycles, which Windows 10 on x86-64 keeps invariant)
static uint64_t tsc_hz(void) {
    LARGE_INTEGER f, q0, q1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q0);
    const uint64_t c0 = __rdtsc();
    plat_sleep_ns(50000000ull);
    QueryPerformanceCounter(&q1);
    const uint64_t c1 = __rdtsc();
    const uint64_t dq = (uint64_t)(q1.QuadPart - q0.QuadPart);
    return dq && f.QuadPart > 0 && c1 > c0 ? (uint64_t)((unsigned __int128)(c1 - c0) * (uint64_t)f.QuadPart / dq) : 0;
}

static void *sampler_main(void *arg) {
    (void)arg;
    plat_thread_set_name("aot profile");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);   // wakes on time while the guest threads keep every core busy
    const uint64_t hz = tsc_hz();   // about 50 ms, which the guest threads' credit does not count (below)
    if (hz < 1000000ull) {
        LOG("[RED] aot: DM_AOT_PROFILE: the TSC's rate could not be measured (%llu Hz): no samples, the profile stays empty", (unsigned long long)hz);
        return NULL;
    }
    const uint64_t period = hz / g_hz, wait_ns = 1000000000ull / g_hz;
    for (int started = 0;; started = 1) {
        plat_sleep_ns(wait_ns);   // a high-resolution waitable timer (plat_win32.c): about 0.5 ms at best; a late wake only makes the weights larger
        pthread_mutex_lock(&g_lock);
        if (!atomic_load(&g_sampling)) { pthread_mutex_unlock(&g_lock); break; }
        for (prof_thread *r = g_threads; r; r = r->next) {
            if (!r->h) continue;
            ULONG64 c = 0;
            if (!QueryThreadCycleTime(r->h, &c)) continue;
            // The first pass only takes the threads' CPU time: what they ran while the TSC was measured is not put on wherever they are now
            const uint64_t ran = c > r->cycles && started ? c - r->cycles : 0;
            r->cycles = c;
            r->credit += ran;
            if (!ran || r->credit < period) continue;   // asleep since the last look, or less than a period of CPU time: not stopped
            const uint64_t w = r->credit / period;
            r->credit -= w * period;
            if (SuspendThread(r->h) == (DWORD)-1) continue;
            // Suspended: kernel calls and count_pc only (top of this file)
            CONTEXT ctx;
            memset(&ctx, 0, sizeof ctx);
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(r->h, &ctx)) count_pc(r, r->t, (uint64_t)ctx.Rip, 0, w);
            ResumeThread(r->h);
        }
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

static void init_once(void) {
    if (read_switches()) return;
    atomic_store(&g_sampling, 1);
    pthread_t th;
    if (pthread_create(&th, NULL, sampler_main, NULL)) { atomic_store(&g_sampling, 0); LOG("[RED] aot: DM_AOT_PROFILE: no sampler thread: no profile"); return; }
    pthread_detach(th);
    atexit(write_at_exit);
    g_on = 1;
    LOG("aot: DM_AOT_PROFILE: every guest thread sampled %u times per second of its CPU time; the profile goes to %s/<module>.tsv at exit (nxaot build --profile)", g_hz, g_dir);
}

void dm_aot_profile_thread_begin(void) {
    if (!dm_aot_profile_on() || t_prof || !atomic_load(&g_sampling)) return;
    prof_thread *r = calloc(1, sizeof *r);
    if (!r) { LOG("[RED] aot: DM_AOT_PROFILE: no memory for a thread's record: the thread is not sampled"); return; }
    r->tid = (long)GetCurrentThreadId();
    r->h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
    ULONG64 c = 0;
    if (!r->h || !QueryThreadCycleTime(r->h, &c)) {
        static atomic_flag said = ATOMIC_FLAG_INIT;
        if (!atomic_flag_test_and_set(&said)) LOG("[RED] aot: DM_AOT_PROFILE: no handle or CPU time for a guest thread (error %lu): it is not sampled", (unsigned long)GetLastError());
        if (r->h) CloseHandle(r->h);
        free(r);
        return;
    }
    r->cycles = c;   // its CPU time counts from here, as a Linux timer counts from its arming
    r->t = dm_aot_self();
    pthread_mutex_lock(&g_lock);
    r->idx = g_nthreads;
    r->next = g_threads; g_threads = r; g_nthreads++;
    pthread_mutex_unlock(&g_lock);
    t_prof = r;
}

// A host thread's context ends in a key destructor, where the thread's own variables may be gone (aot_thread.c host_end): then t_prof reads NULL and the
// record stays registered, which is harmless: an ended thread's CPU time no longer grows, so the sampler never suspends it nor reads its block again
void dm_aot_profile_thread_end(void) {
    prof_thread *r = t_prof;
    if (!r) return;
    t_prof = NULL;
    pthread_mutex_lock(&g_lock);   // never inside a pass: the sampler holds the lock for the whole of one and resumed every thread it stopped before it lets go
    CloseHandle(r->h);
    r->h = NULL; r->t = NULL;
    pthread_mutex_unlock(&g_lock);
}

// Windows' rename does not replace an existing file; the C runtime's paths are UTF-8 (plat_win32.c)
static int replace_file(const char *from, const char *to) { return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1; }
#else
static void on_prof(int sig, siginfo_t *si, void *ucv) {
    (void)sig;
    prof_thread *r = t_prof;
    if (!r || !atomic_load_explicit(&g_sampling, memory_order_relaxed)) return;
    const int saved_errno = errno;
    const uint64_t w = 1 + (uint64_t)(si && si->si_code == SI_TIMER && si->si_overrun > 0 ? si->si_overrun : 0);
    const ucontext_t *uc = ucv;
    count_pc(r, dm_aot_self(), (uint64_t)uc->uc_mcontext.gregs[REG_RIP], (uint64_t)uc->uc_mcontext.gregs[REG_RSP], w);
    errno = saved_errno;
}

static void init_once(void) {
    if (read_switches()) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;   // the thread's alternate stack, as aot_fault.c's handlers: guest code may be deep in its native stack
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, NULL)) { LOG("[RED] aot: DM_AOT_PROFILE: no SIGPROF handler (errno %d): no profile", errno); return; }
    atomic_store(&g_sampling, 1);
    atexit(write_at_exit);
    g_on = 1;
    LOG("aot: DM_AOT_PROFILE: every guest thread sampled %u times per second of its CPU time; the profile goes to %s/<module>.tsv at exit (nxaot build --profile)", g_hz, g_dir);
}

void dm_aot_profile_thread_begin(void) {
    if (!dm_aot_profile_on() || t_prof || !atomic_load(&g_sampling)) return;
    prof_thread *r = calloc(1, sizeof *r);
    if (!r) { LOG("[RED] aot: DM_AOT_PROFILE: no memory for a thread's record: the thread is not sampled"); return; }
    r->tid = (long)syscall(SYS_gettid);
    pthread_attr_t sa;
    void *sa_base; size_t sa_size;
    if (g_hostpc && !pthread_getattr_np(pthread_self(), &sa)) {   // the bounds count_caller reads within (DM_AOT_PROFILE_HOST only: the main thread's read parses /proc/self/maps)
        if (!pthread_attr_getstack(&sa, &sa_base, &sa_size)) { r->stack_lo = (uint64_t)(uintptr_t)sa_base; r->stack_hi = r->stack_lo + sa_size; }
        pthread_attr_destroy(&sa);
    }
    struct sigevent se;
    memset(&se, 0, sizeof se);
    se.sigev_notify = SIGEV_THREAD_ID;
    se.sigev_signo = SIGPROF;
#ifdef sigev_notify_thread_id
    se.sigev_notify_thread_id = (pid_t)r->tid;
#else
    se._sigev_un._tid = (pid_t)r->tid;
#endif
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &se, &r->timer)) {
        static atomic_flag said = ATOMIC_FLAG_INIT;
        if (!atomic_flag_test_and_set(&said)) LOG("[RED] aot: DM_AOT_PROFILE: timer_create failed (errno %d): a guest thread is not sampled", errno);
        free(r);
        return;
    }
    pthread_mutex_lock(&g_lock);
    r->idx = g_nthreads;
    r->next = g_threads; g_threads = r; g_nthreads++;
    pthread_mutex_unlock(&g_lock);
    t_prof = r;
    atomic_signal_fence(memory_order_seq_cst);   // the record is the thread's before its first signal
    const long ns = 1000000000L / (long)g_hz;
    struct itimerspec it = {{ns / 1000000000L, ns % 1000000000L}, {ns / 1000000000L, ns % 1000000000L}};
    r->armed = !timer_settime(r->timer, 0, &it, NULL);
}

void dm_aot_profile_thread_end(void) {
    prof_thread *r = t_prof;
    if (!r) return;
    t_prof = NULL;
    atomic_signal_fence(memory_order_seq_cst);   // a signal still pending finds no record
    timer_delete(r->timer);
    r->armed = 0;
}

static int replace_file(const char *from, const char *to) { return rename(from, to); }
#endif

// The writer: one file per module with samples, the sampled functions by samples (most first), each function's line `# function 0x<entry> <samples> <rows>` before its rows, its
// rows by samples (most first). The function lines are comments for nxaot (ReadProfile skips '#' to the end of a line); the packer (build_ipa_from_nsp.py profile_summary) and the
// launcher (launcher_model.cpp read_profile) count the sampled functions by them, since a function's instruction rows grow with a session's length and nxaot's tiers do not
typedef struct { uint64_t off, n; } row_t;
static int row_cmp(const void *a, const void *b) {
    const row_t *x = a, *y = b;
    if (x->n != y->n) return x->n > y->n ? -1 : 1;
    return x->off < y->off ? -1 : x->off > y->off;
}
typedef struct { uint64_t entry, n; size_t first, rows; } fn_rows_t;   // a sampled function: its entry, its samples, its rows rows[first .. first + rows)
static int fn_rows_cmp(const void *a, const void *b) {
    const fn_rows_t *x = a, *y = b;
    if (x->n != y->n) return x->n > y->n ? -1 : 1;
    return x->entry < y->entry ? -1 : x->entry > y->entry;
}
static int is_sep(char c) {
#if defined(_WIN32)
    if (c == '\\') return 1;   // a Windows path may use either separator
#endif
    return c == '/';
}
static void mkdirs(char *path) {   // mkdir -p
    for (char *s = path + 1; *s; s++) if (is_sep(*s)) { const char c = *s; *s = 0; mkdir(path, 0755); *s = c; }
    mkdir(path, 0755);
}
static void file_name(const module_t *m, char *out, size_t n) {   // a module's name as a file name
    snprintf(out, n, "%s", m->name);
    for (char *s = out; *s; s++) if (*s == '/' || *s == '\\') *s = '_';
}
static void sha_hex(const dm_aot_pack *p, char out[65]) {   // the image the pack was compiled from (its descriptor's SHA-256: the image's, as the loader checked it)
    const uint8_t *h = dm_aot_pack_desc(p)->sha256;
    for (int b = 0; b < 32; b++) snprintf(out + 2 * b, 3, "%02x", h[b]);
}

// The JIT fallback's misses (aot.h, "The JIT fallback's misses"): per module with a pack and misses this session, <dir>/<module>.misses.tsv, this session's (address, reason)
// pairs and the rows the file holds when an earlier session of the same image wrote it (their entries added up), by module offset, then reason. At exit, outside any signal
// handler: the pairs are a copy taken under the table's lock (dm_aot_jit_misses), the files written as the profile's are (a temporary name, then renamed over the old one)
typedef struct { uint64_t off, n; char word[16]; int now; } miss_row;
static int miss_cmp(const void *a, const void *b) {
    const miss_row *x = a, *y = b;
    if (x->off != y->off) return x->off < y->off ? -1 : 1;
    return strcmp(x->word, y->word);
}
static int miss_add(miss_row **rows, size_t *n, size_t *cap, uint64_t off, const char *word, uint64_t count, int now) {
    if (*n == *cap) {
        const size_t c = *cap ? 2 * *cap : 64;
        miss_row *r = realloc(*rows, c * sizeof *r);
        if (!r) return -1;
        *rows = r; *cap = c;
    }
    miss_row *r = &(*rows)[(*n)++];
    r->off = off; r->n = count; r->now = now;
    snprintf(r->word, sizeof r->word, "%s", word);
    return 0;
}
// The rows of the misses file at path when its header names image sha (an earlier session of this image wrote it), appended to rows: their number. *other: the file names another
// image (its rows are not taken: the file is replaced)
static size_t miss_earlier(const char *path, const char *sha, miss_row **rows, size_t *n, size_t *cap, int *other) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char line[512], mod[64], bid[80], hex[80], word[32];
    int same = 0;
    const size_t first = *n;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') {
            if (sscanf(line, "# module %63[^,], build ID %79[^,], image SHA-256 %79s", mod, bid, hex) == 3) { same = !strcmp(hex, sha); *other = !same; }
            continue;
        }
        unsigned long long off, cnt;
        if (same && sscanf(line, "%llx %31s %llu", &off, word, &cnt) == 3 && miss_add(rows, n, cap, off, word, cnt, 0)) break;
    }
    fclose(f);
    return *n - first;
}

static int write_misses(const char *dir, int *bad) {
    uint64_t dropped = 0;
    const size_t total = dm_aot_jit_misses(NULL, 0, &dropped);
    if (!total) {
        LOG("aot: misses: the JIT fallback ran for no pack this session: no misses file written to %s", dir);
        return 0;
    }
    dm_aot_miss *all = calloc(total, sizeof *all);
    uint8_t *taken = calloc(total, 1);
    if (!all || !taken) { LOG("[RED] aot: misses: no memory for %zu misses: none written", total); free(all); free(taken); (*bad)++; return 0; }
    const size_t got = dm_aot_jit_misses(all, total, &dropped);
    int files = 0;
    const int npacks = dm_aot_pack_count();
    for (int i = 0; i < npacks; i++) {
        const dm_aot_pack *p = dm_aot_pack_at(i);
        const module_t *m = dm_aot_pack_module(p);
        const uint64_t base = (uint64_t)(uintptr_t)m->base;
        miss_row *rows = NULL;
        size_t n = 0, cap = 0, now = 0;
        for (size_t k = 0; k < got; k++) {
            if (taken[k] || all[k].pc - base >= m->size) continue;
            taken[k] = 1;
            const char *word = dm_aot_miss_word(all[k].reason);
            if (word && !miss_add(&rows, &n, &cap, all[k].pc - base, word, all[k].count, 1)) now++;
        }
        if (!now) { free(rows); continue; }
        char name[64], path[sizeof g_dir + 96], tmp[sizeof path + 8], sha[65];
        file_name(m, name, sizeof name);
        snprintf(path, sizeof path, "%s/%s.misses.tsv", dir, name);
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        sha_hex(p, sha);
        int other = 0;
        const size_t earlier = miss_earlier(path, sha, &rows, &n, &cap, &other);
        qsort(rows, n, sizeof *rows, miss_cmp);
        size_t k = 0, addrs = 0, addrs_now = 0;
        uint64_t entries = 0;
        for (size_t j = 0; j < n; j++) {   // one row per address and reason: an earlier session's row and this one's added up
            if (k && rows[k - 1].off == rows[j].off && !strcmp(rows[k - 1].word, rows[j].word)) { rows[k - 1].n += rows[j].n; rows[k - 1].now |= rows[j].now; continue; }
            rows[k++] = rows[j];
        }
        for (size_t j = 0, e; j < k; j = e) {   // per address (its rows are adjacent)
            int seen_now = 0;
            for (e = j; e < k && rows[e].off == rows[j].off; e++) { entries += rows[e].n; seen_now |= rows[e].now; }
            addrs++; addrs_now += seen_now != 0;
        }
        FILE *f = fopen(tmp, "wb");   // binary: the same bytes on Windows
        if (!f) { LOG("[RED] aot: misses: cannot write %s", tmp); free(rows); (*bad)++; continue; }
        fprintf(f, "# nxaot misses: guest addresses the x86-64 engine's JIT fallback ran because the pack had no compiled code there (dm/host/aot_run.c, DM_AOT_PROFILE); "
                   "nxaot build --misses <this file>\n");
        fprintf(f, "# module %s, build ID %s, image SHA-256 %s\n", m->name, m->build_id[0] ? m->build_id : "none", sha);
        fprintf(f, "# pack %s\n", dm_aot_pack_path(p));
        fprintf(f, "# %zu addresses, %llu entries into the JIT fallback; %zu of the addresses this session, the rest from earlier sessions of this image\n", addrs,
                (unsigned long long)entries, addrs_now);
        fprintf(f, "# <module offset> <reason> <entries>: indirect, call, host, nocode (an entry of the next pack), nonlocal (a resume point), fpcr, refused, untranslated, "
                   "noreturn, other (no pack serves them)\n");
        for (size_t j = 0; j < k; j++) fprintf(f, "0x%llx %s %llu\n", (unsigned long long)rows[j].off, rows[j].word, (unsigned long long)rows[j].n);
        free(rows);
        if (fclose(f) || replace_file(tmp, path)) { LOG("[RED] aot: misses: cannot write %s", path); (*bad)++; continue; }
        LOG("aot: misses: %s: %zu addresses, %llu entries into the JIT fallback, %zu of the addresses this session (%s%s)", path, addrs, (unsigned long long)entries, addrs_now,
            m->name, other ? "; the file held another image's misses: replaced" : earlier ? "; merged with an earlier session's" : "");
        files++;
    }
    size_t outside = 0;
    for (size_t k = 0; k < got; k++) outside += !taken[k];
    if (outside || dropped)
        LOG("aot: misses: %zu outside every pack's module (not written), %llu entries the full tables did not keep (aot_run.c MAX_SEEN)", outside, (unsigned long long)dropped);
    free(all); free(taken);
    return files;
}

// The loader's bindings (aot.h, "The loader's bindings"): per module with a pack, not an NRO's, <dir>/<module>.bind.tsv, the PLT slots the loader bound to the module's own
// functions (loader_self_bindings), written as the misses are (a temporary name, then renamed over the old one); a module with none gets no file, and an older file of it goes
typedef struct { FILE *f; size_t n; } bind_out;
static void bind_row(uint64_t slot, uint64_t target, const char *name, void *arg) {
    bind_out *o = arg;
    fprintf(o->f, "0x%llx 0x%llx %s\n", (unsigned long long)slot, (unsigned long long)target, name);
    o->n++;
}
static int write_module_bindings(const char *dir, const module_t *m, const uint8_t sha256[32], const char *pack) {
    char name[64], path[sizeof g_dir + 96], tmp[sizeof path + 8], sha[65];
    file_name(m, name, sizeof name);
    snprintf(path, sizeof path, "%s/%s.bind.tsv", dir, name);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    for (int b = 0; b < 32; b++) snprintf(sha + 2 * b, 3, "%02x", sha256[b]);
    bind_out o = {fopen(tmp, "wb"), 0};   // binary: the same bytes on Windows
    if (!o.f) { LOG("[RED] aot: bindings: cannot write %s", tmp); return 1; }
    fprintf(o.f, "# nxaot bindings: the PLT slots the host's loader bound to functions of this module itself (dm/host/loader.c, DM_AOT_PROFILE); nxaot build --bind <this file>\n");
    fprintf(o.f, "# module %s, build ID %s, image SHA-256 %s\n", m->name, m->build_id[0] ? m->build_id : "none", sha);
    fprintf(o.f, "# pack %s\n", pack);
    fprintf(o.f, "# <slot offset> <target offset> <symbol>\n");
    loader_self_bindings(m, bind_row, &o);
    if (fclose(o.f)) { LOG("[RED] aot: bindings: cannot write %s", tmp); remove(tmp); return 1; }
    if (!o.n) { remove(tmp); remove(path); return 0; }
    if (replace_file(tmp, path)) { LOG("[RED] aot: bindings: cannot write %s", path); return 1; }
    LOG("aot: bindings: %s: %zu PLT slots bound to %s's own functions", path, o.n, m->name);
    return 0;
}
static void write_bindings(const char *dir, int *bad) {
    const int npacks = dm_aot_pack_count();
    for (int i = 0; i < npacks; i++) {
        const dm_aot_pack *p = dm_aot_pack_at(i);
        const module_t *m = dm_aot_pack_module(p);
        if (!m->nro) *bad += write_module_bindings(dir, m, dm_aot_pack_desc(p)->sha256, dm_aot_pack_path(p));
    }
}
// A pack refused for its bindings (aot_arena.c bindings_check): this session's bindings of its module written now, with DM_AOT_PROFILE, so that the next pack is built
// with them although the session ends here
void dm_aot_bindings_now(const module_t *m, const uint8_t sha256[32], const char *pack) {
    if (!dm_aot_profile_on() || m->nro) return;
    mkdirs(g_dir);
    write_module_bindings(g_dir, m, sha256, pack);
}
__attribute__((constructor)) static void bindings_hook(void) { atomic_store(&dm_aot_bindings_refused, dm_aot_bindings_now); }
// nxhost-run --bindings (loader.c load_modules): a module's bindings into dir, the profile's form, with no pack loaded
int dm_aot_write_module_bindings(const char *dir, const module_t *m, const uint8_t sha256[32], const char *pack) {
    char d[sizeof g_dir];
    snprintf(d, sizeof d, "%s", dir);
    mkdirs(d);
    return write_module_bindings(d, m, sha256, pack);
}

int dm_aot_profile_write(void) {
    if (!dm_aot_profile_on() || atomic_exchange(&g_written, 1)) return -1;
    atomic_store(&g_sampling, 0);   // the handler counts nothing more; a sample on its way may still add one
#if !defined(_WIN32)
    // And no SIGPROF is delivered any more, on any thread: a sample after the files is of no use, and the threads' timers stay armed through exit's handlers (a running
    // thread's timer goes only with its block, dm_aot_profile_thread_end), which free what a signal might run on (dynarmic's alternate stack: aot_fault.c
    // dm_aot_fault_after_jit takes it off its thread). An ignored signal is discarded where it is generated
    signal(SIGPROF, SIG_IGN);
#endif
    uint64_t all = 0, code = 0, hostcall = 0, jit = 0, host = 0, placed = 0, crossed = 0;
    int threads = 0;
    pthread_mutex_lock(&g_lock);
    for (const prof_thread *r = g_threads; r; r = r->next) {
        all += r->samples; code += r->code; hostcall += r->hostcall; jit += r->jit; host += r->host; placed += r->placed; crossed += r->crossed;
        threads += r->samples > 0;
    }
    char summary[600];
    snprintf(summary, sizeof summary, "%u Hz of each guest thread's CPU time, %d of %d threads sampled; all modules: %llu samples, %llu in compiled code (%llu at a guest instruction, "
             "%llu at one outside their function: not counted there), %llu in host calls and SVCs of compiled functions, %llu in the JIT fallback, %llu elsewhere in the host", g_hz,
             threads, g_nthreads, (unsigned long long)all, (unsigned long long)code, (unsigned long long)placed, (unsigned long long)crossed, (unsigned long long)hostcall,
             (unsigned long long)jit, (unsigned long long)host);
    LOG("aot: profile: %s", summary);
    if (crossed) LOG("[RED] aot: profile: %llu samples whose pack's positions named a guest instruction outside the function holding the pc: counted for the function only",
                     (unsigned long long)crossed);
    int shown = 0;
    for (const prof_thread *r = g_threads; r && shown < 16; r = r->next) {
        if (!r->samples) continue;
        LOG("aot: profile: thread %ld: %llu samples: %llu compiled (%llu at an instruction), %llu host calls and SVCs, %llu JIT, %llu host", r->tid, (unsigned long long)r->samples,
            (unsigned long long)r->code, (unsigned long long)r->placed, (unsigned long long)r->hostcall, (unsigned long long)r->jit, (unsigned long long)r->host);
        shown++;
    }
    // The JIT fallback's measured share and the FPCR's part in it (dm_aot_profile_jit_run): in all, then every thread that ran the JIT, its CPU time the samples count
    uint64_t runs = 0, jit_ns = 0, fruns = 0, fns = 0, other_n = 0, other_ns = 0;
    for (const prof_thread *r = g_threads; r; r = r->next) {
        runs += r->jit_runs; jit_ns += r->jit_ns; fruns += r->fpcr_runs; fns += r->fpcr_ns; other_n += r->other_n; other_ns += r->other_ns;
    }
    const double cpu_s = (double)all / g_hz;
    LOG("aot: profile: the JIT fallback: %llu runs, %.3f s of CPU time (%.2f %% of the %.3f s the samples count); %llu runs, %.3f s (%.2f %%) begun under another FPCR than the packs' "
        "%#x; %llu stretches under another FPCR, %.3f s", (unsigned long long)runs, jit_ns * 1e-9, cpu_s > 0 ? jit_ns * 1e-7 / cpu_s : 0.0, cpu_s, (unsigned long long)fruns, fns * 1e-9,
        cpu_s > 0 ? fns * 1e-7 / cpu_s : 0.0, dm_aot_module_fpcr(), (unsigned long long)other_n, other_ns * 1e-9);
    for (const prof_thread *r = g_threads; r; r = r->next) {
        if (!r->jit_runs) continue;
        LOG("aot: profile: thread %ld '%s': %.3f s of CPU time sampled; JIT %llu runs, %.3f s; under another FPCR %llu runs, %.3f s, %llu stretches, %.3f s%s, last FPCR %#x",
            r->tid, r->name, (double)r->samples / g_hz, (unsigned long long)r->jit_runs, r->jit_ns * 1e-9, (unsigned long long)r->fpcr_runs, r->fpcr_ns * 1e-9,
            (unsigned long long)r->other_n, r->other_ns * 1e-9, r->in_other ? " (one still open)" : "", r->fpcr_last);
    }
    pthread_mutex_unlock(&g_lock);
    origins_log();   // the JIT time by the miss that began it, the heaviest first
    char dir[sizeof g_dir];
    snprintf(dir, sizeof dir, "%s", g_dir);
    mkdirs(dir);
    int files = 0, bad = 0;
    const int npacks = dm_aot_pack_count();
    for (int i = 0; i < npacks; i++) {
        const dm_aot_pack *p = dm_aot_pack_at(i);
        _Atomic uint64_t *c = dm_aot_pack_samples(p);
        const size_t n = dm_aot_pack_fn_count(p);
        if (!c) continue;
        uint64_t words = 0;
        _Atomic uint32_t *at = dm_aot_pack_pos_samples(p, &words);
        // The rows (the file's header at the top of this file), per sampled function in module-offset order: its guest instructions with samples in [entry, end), or its entry when
        // it has none. The functions' ranges do not overlap (nxaot's limit is at most the next entry; count_at only counts an instruction inside the sample's function); each range
        // is scanned from where the ranges before it ended, so no row is written twice
        size_t fns = 0, k = 0, offsets = 0, entries = 0, ng = 0, at_rows = 0;
        for (size_t j = 0; j < n; j++) fns += atomic_load_explicit(&c[j], memory_order_relaxed) != 0;
        if (!fns) continue;
        uint64_t all_at = 0;
        for (uint64_t w = 0; at && w < words; w++) {
            const uint32_t v = atomic_load_explicit(&at[w], memory_order_relaxed);
            offsets += v != 0; all_at += v;
        }
        const size_t cap = offsets + fns;   // a sample on its way may still add one after the counts above: no row or function past them
        row_t *rows = malloc(cap * sizeof *rows);
        fn_rows_t *g = malloc(fns * sizeof *g);
        if (!rows || !g) { free(rows); free(g); bad++; continue; }
        uint64_t sum = 0, sum_at = 0, sum_entries = 0, scanned = 0;
        for (size_t j = 0; j < n && ng < fns; j++) {
            const uint64_t v = atomic_load_explicit(&c[j], memory_order_relaxed), e = dm_aot_pack_fn(p, j), end = dm_aot_pack_fn_end(p, j);
            if (!v) continue;
            fn_rows_t *fr = &g[ng++];
            fr->entry = e; fr->first = k;
            uint64_t in = 0;   // its samples at its instructions
            for (uint64_t o = e > scanned ? e : scanned; at && o < end && o / 4 < words && k < cap; o += 4) {
                const uint32_t s = atomic_load_explicit(&at[o / 4], memory_order_relaxed);
                if (s) { rows[k++] = (row_t){o, s}; in += s; at_rows++; }
            }
            if (end > scanned) scanned = end;
            if (in) sum_at += in;
            else if (k < cap) { rows[k++] = (row_t){e, v}; sum_entries += v; entries++; }
            fr->rows = k - fr->first;
            fr->n = v > in ? v : in;   // all its samples: at its instructions, in its host calls and SVCs, in its code with no instruction (never fewer than its rows')
            sum += fr->n;
            qsort(rows + fr->first, fr->rows, sizeof *rows, row_cmp);
        }
        qsort(g, ng, sizeof *g, fn_rows_cmp);
        // Samples at instructions in no sampled function's range: none expected (count_pc counts the function before its instruction, and the instruction only inside it)
        const uint64_t lost = all_at > sum_at ? all_at - sum_at : 0;
        const size_t lost_rows = offsets > at_rows ? offsets - at_rows : 0;
        const module_t *m = dm_aot_pack_module(p);
        if (lost_rows)
            LOG("[RED] aot: profile: %s: %llu samples at %zu guest instructions in no sampled function's range: not written", m->name, (unsigned long long)lost, lost_rows);
        char name[64], path[sizeof g_dir + 96], tmp[sizeof path + 8], sha[65];
        file_name(m, name, sizeof name);
        snprintf(path, sizeof path, "%s/%s.tsv", dir, name);
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        sha_hex(p, sha);
        FILE *f = fopen(tmp, "wb");   // binary: the same bytes on Windows (no \r before each \n)
        if (!f) { LOG("[RED] aot: profile: cannot write %s", tmp); free(rows); free(g); bad++; continue; }
        fprintf(f, "# nxaot profile: guest instructions sampled by the x86-64 engine (dm/host/aot_profile.c, DM_AOT_PROFILE); nxaot build --profile <this file>\n");
        fprintf(f, "# module %s, build ID %s, image SHA-256 %s\n", m->name, m->build_id[0] ? m->build_id : "none", sha);
        fprintf(f, "# pack %s (%s)\n", dm_aot_pack_path(p), at ? "its positions give each sample's instruction" : "no positions: every function's samples at its entry");
        fprintf(f, "# %llu samples in %zu of its %zu functions: %llu at %zu guest instructions, %llu at the entries of the %zu functions with none at an instruction, %llu not "
                   "written (host calls, SVCs and code with no instruction of functions with samples at their instructions); %s\n", (unsigned long long)sum, ng, n,
                (unsigned long long)sum_at, at_rows, (unsigned long long)sum_entries, entries,
                (unsigned long long)(sum > sum_at + sum_entries ? sum - sum_at - sum_entries : 0), summary);
        fprintf(f, "# functions: each sampled function's rows follow its line `# function 0x<entry> <samples> <rows>` (its samples: all of them, at its instructions, in its host "
                   "calls and SVCs and in its code with no instruction; its rows: the lines after it), most samples first; the packer and the launcher count the sampled "
                   "functions by these lines, nxaot reads them as comments\n");
        fprintf(f, "# <module offset> <samples>: a guest instruction's, or a function's at its entry when it has none at an instruction\n");
        for (size_t j = 0; j < ng; j++) {
            fprintf(f, "# function 0x%llx %llu %zu\n", (unsigned long long)g[j].entry, (unsigned long long)g[j].n, g[j].rows);
            for (size_t r = g[j].first; r < g[j].first + g[j].rows; r++) fprintf(f, "0x%llx %llu\n", (unsigned long long)rows[r].off, (unsigned long long)rows[r].n);
        }
        free(rows);
        free(g);
        if (fclose(f) || replace_file(tmp, path)) { LOG("[RED] aot: profile: cannot write %s", path); bad++; continue; }
        LOG("aot: profile: %s: %llu samples in %zu functions: %llu at %zu guest instructions, %llu at %zu entries (%s)", path, (unsigned long long)sum, ng,
            (unsigned long long)sum_at, at_rows, (unsigned long long)sum_entries, entries, m->name);
        files++;
    }
    if (!files && !bad) LOG("aot: profile: no samples in any pack's code: nothing written to %s", dir);
    write_misses(dir, &bad);   // beside the profile (aot.h, "The JIT fallback's misses")
    write_bindings(dir, &bad);   // beside them (aot.h, "The loader's bindings")
    return bad ? -1 : 0;
}

// The mark (DM_CTL `profmark <path>`; above count_pc, "The host's samples by pc"): <path> gets, tab-separated, a `thread` row per sampled thread (its id, its name, its samples:
// all, in compiled code, in host calls and SVCs, in the JIT fallback, elsewhere in the host; the JIT fallback's measured CPU time in ns; its number) and, with
// DM_AOT_PROFILE_HOST, a `pc` row per sampled host pc and thread (its class, its object file, its offset in it, the nearest exported symbol and the offset from it, its
// samples, the thread's number), the `caller` rows besides the samples (Linux), then a `lost` row. The counts are this
// session's so far, read while the threads run (relaxed: a sample on its way may be missing); sampling goes on
static void pc_where(uint64_t pc, char *obj, size_t no, uint64_t *off, char *sym, size_t ns, uint64_t *symoff) {
    snprintf(obj, no, "?"); snprintf(sym, ns, "-"); *off = pc; *symoff = 0;
#if defined(_WIN32)
    HMODULE m = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)pc, &m) && m) {
        char p[MAX_PATH];
        const DWORD n = GetModuleFileNameA(m, p, sizeof p);
        if (n && n < sizeof p) { const char *b = p + n; while (b > p && b[-1] != '\\' && b[-1] != '/') b--; snprintf(obj, no, "%s", b); }
        *off = pc - (uint64_t)(uintptr_t)m;
    }
#else
    Dl_info di;
    if (dladdr((void *)(uintptr_t)pc, &di) && di.dli_fname) {
        const char *b = strrchr(di.dli_fname, '/');
        snprintf(obj, no, "%s", b ? b + 1 : di.dli_fname);
        *off = pc - (uint64_t)(uintptr_t)di.dli_fbase;
        if (di.dli_sname) { snprintf(sym, ns, "%s", di.dli_sname); *symoff = pc - (uint64_t)(uintptr_t)di.dli_saddr; }
    }
#endif
}
int dm_aot_profile_mark(const char *path) {
    if (!dm_aot_profile_on()) { LOG("[RED] aot: profile mark %s: DM_AOT_PROFILE is not set, so no thread is sampled: nothing written", path); return -1; }
    char dir[1024];   // its folder made first, as the writer makes the profile's (a mark into a folder the session has not written yet)
    snprintf(dir, sizeof dir, "%s", path);
    for (char *e = dir + strlen(dir); e > dir; e--) if (is_sep(e[-1])) { e[-1] = 0; if (dir[0]) mkdirs(dir); break; }
    FILE *f = fopen(path, "wb");
    if (!f) { LOG("[RED] aot: profile mark: cannot write %s", path); return -1; }
    fprintf(f, "# aot profile mark (dm/host/aot_profile.c dm_aot_profile_mark): %u Hz of each guest thread's CPU time; this session's counts so far\n", g_hz);
    fprintf(f, "# thread <tid> <name> <samples> <compiled> <host calls and SVCs> <JIT> <host> <JIT ns> <number>\n"
               "# pc <hostcall|host|caller> <object> <offset> <symbol>+<offset> <samples> <thread number>\n");
    uint64_t host = 0;
    int threads = 0;
    pthread_mutex_lock(&g_lock);
    for (const prof_thread *r = g_threads; r; r = r->next) {
        char name[64] = "";
#if !defined(_WIN32)
        char cp[64];
        snprintf(cp, sizeof cp, "/proc/self/task/%ld/comm", r->tid);
        FILE *c = fopen(cp, "r");
        if (c) { if (!fgets(name, sizeof name, c)) name[0] = 0; fclose(c); }
        name[strcspn(name, "\n")] = 0;
#endif
        if (!name[0]) snprintf(name, sizeof name, "%s", r->name[0] ? r->name : "-");
        for (char *s = name; *s; s++) if (*s == '\t') *s = ' ';
        fprintf(f, "thread\t%ld\t%s\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%d\n", r->tid, name, (unsigned long long)r->samples, (unsigned long long)r->code,
                (unsigned long long)r->hostcall, (unsigned long long)r->jit, (unsigned long long)r->host, (unsigned long long)r->jit_ns, r->idx);
        host += r->hostcall + r->host;
        threads++;
    }
    pthread_mutex_unlock(&g_lock);
    size_t pcs = 0;
    uint64_t in_pcs = 0;
    for (uint32_t i = 0; g_hostpc && i < HOSTPC_N; i++) {
        const uint64_t k = atomic_load_explicit(&g_hostpc[i].key, memory_order_relaxed), n = atomic_load_explicit(&g_hostpc[i].n, memory_order_relaxed);
        if (!k || !n) continue;
        char obj[128], sym[160];
        uint64_t off, symoff;
        pc_where(HOSTPC_PC(k), obj, sizeof obj, &off, sym, sizeof sym, &symoff);
        fprintf(f, "pc\t%s\t%s\t0x%llx\t%s+0x%llx\t%llu\t%d\n", k >> 62 == 1 ? "hostcall" : k >> 62 == 2 ? "host" : "caller", obj, (unsigned long long)off, sym,
                (unsigned long long)symoff, (unsigned long long)n, HOSTPC_THREAD(k));
        if (k >> 62 != 3) { pcs++; in_pcs += n; }
    }
    fprintf(f, "lost\t%llu\n", (unsigned long long)atomic_load(&g_hostpc_lost));
    const int bad = fclose(f) != 0;
    LOG("aot: profile mark %s: %d threads, %llu samples in the host%s, %zu host pcs holding %llu", path, threads, (unsigned long long)host,
        g_hostpc ? "" : " (DM_AOT_PROFILE_HOST is not set: not by pc)", pcs, (unsigned long long)in_pcs);
    return bad ? -1 : 0;
}
