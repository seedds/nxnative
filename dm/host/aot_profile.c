// The sampler of the x86-64 execution engine (aot.h, "The sampler"; dm/tools/aot/README.md, "Tiers by profile" and "The sample profile at tier H"): DM_AOT_PROFILE=<folder> samples
// every thread that runs guest code at DM_AOT_PROFILE_HZ (1000 by default) per second of its own CPU time and writes, at process exit, the profile `nxaot build --profile` reads:
// <folder>/<module>.tsv per module with samples, one line `<module offset> <samples>` per sampled guest instruction (design 12's sampled instruction offsets). The packer
// (scripts/build_ipa_from_nsp.py --platform linux / windows) takes <NSP folder>/<id>-work/profile/<module>.tsv for the module's next pack: its functions with samples at tier H, the
// others at tier C (balanced mode), and the samples' positions inside each function at H weigh its code (branch weights, entry and call counts). Beside each profile the writer puts
// the JIT fallback's misses, <folder>/<module>.misses.tsv (write_misses; aot.h, "The JIT fallback's misses"), which the packer gives nxaot build --misses.
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
// (its host calls, its code with no instruction) would make its entry block look hotter than its loops (the header counts them)
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
    uint64_t placed, crossed;   // of code: at a guest instruction of the pack's positions; at one outside its function (not counted at it: none expected)
#if defined(_WIN32)
    HANDLE h;                   // the thread, while it is registered (g_lock); NULL once it unregistered
    const dm_aot_thread *t;     // its state block, while h is set (prof_at and depth are read from it while the thread is suspended)
    uint64_t cycles, credit;    // its CPU time (QueryThreadCycleTime) at the last look; TSC cycles of CPU time not sampled yet
#else
    timer_t timer;
    int armed;
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

// One sample of weight w at native pc on the thread r records, whose state block is t (NULL when it has none): the rules at the top of this file. Async-signal-safe (Linux runs
// it in the thread's own SIGPROF handler) and safe while the thread is suspended (Windows runs it on the sampler thread): no allocation, no lock
static void count_pc(prof_thread *r, const dm_aot_thread *t, uint64_t pc, uint64_t w) {
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
    else if (at > DM_AOT_PROF_JIT && (fn = dm_aot_fn_of_guest(at, &pack)) >= 0) { count(pack, fn, w); r->hostcall += w; }
    else r->host += w;
}

static void write_at_exit(void) { dm_aot_profile_write(); }

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
            if (GetThreadContext(r->h, &ctx)) count_pc(r, r->t, (uint64_t)ctx.Rip, w);
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
    count_pc(r, dm_aot_self(), (uint64_t)((const ucontext_t *)ucv)->uc_mcontext.gregs[REG_RIP], w);
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

// The writer: one file per module with samples, most samples first
typedef struct { uint64_t off, n; } row_t;
static int row_cmp(const void *a, const void *b) {
    const row_t *x = a, *y = b;
    if (x->n != y->n) return x->n > y->n ? -1 : 1;
    return x->off < y->off ? -1 : x->off > y->off;
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
                   "other (no pack serves them)\n");
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
    pthread_mutex_unlock(&g_lock);
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
        // The rows: each guest instruction with samples, then the entry of each sampled function with none at an instruction (the file's header at the top of this file)
        size_t fns = 0, k = 0, offsets = 0, entries = 0;
        for (size_t j = 0; j < n; j++) fns += atomic_load_explicit(&c[j], memory_order_relaxed) != 0;
        if (!fns) continue;
        for (uint64_t w = 0; at && w < words; w++) offsets += atomic_load_explicit(&at[w], memory_order_relaxed) != 0;
        const size_t cap = offsets + fns;   // a sample on its way may still add one after the counts above: no row past it
        row_t *rows = malloc(cap * sizeof *rows);
        if (!rows) { bad++; continue; }
        uint64_t sum = 0, sum_at = 0, sum_entries = 0;
        for (uint64_t w = 0; at && w < words && k < offsets; w++) {
            const uint32_t v = atomic_load_explicit(&at[w], memory_order_relaxed);
            if (v) { rows[k++] = (row_t){4 * w, v}; sum_at += v; }
        }
        for (size_t j = 0; j < n; j++) {
            const uint64_t v = atomic_load_explicit(&c[j], memory_order_relaxed);
            if (!v) continue;
            sum += v;
            uint64_t in = 0;   // its samples at its instructions
            for (uint64_t o = dm_aot_pack_fn(p, j); at && o < dm_aot_pack_fn_end(p, j) && o / 4 < words && !in; o += 4) in = atomic_load_explicit(&at[o / 4], memory_order_relaxed);
            if (!in && k < cap) { rows[k++] = (row_t){dm_aot_pack_fn(p, j), v}; sum_entries += v; entries++; }
        }
        qsort(rows, k, sizeof *rows, row_cmp);
        const module_t *m = dm_aot_pack_module(p);
        char name[64], path[sizeof g_dir + 96], tmp[sizeof path + 8], sha[65];
        file_name(m, name, sizeof name);
        snprintf(path, sizeof path, "%s/%s.tsv", dir, name);
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        sha_hex(p, sha);
        FILE *f = fopen(tmp, "wb");   // binary: the same bytes on Windows (no \r before each \n)
        if (!f) { LOG("[RED] aot: profile: cannot write %s", tmp); free(rows); bad++; continue; }
        fprintf(f, "# nxaot profile: guest instructions sampled by the x86-64 engine (dm/host/aot_profile.c, DM_AOT_PROFILE); nxaot build --profile <this file>\n");
        fprintf(f, "# module %s, build ID %s, image SHA-256 %s\n", m->name, m->build_id[0] ? m->build_id : "none", sha);
        fprintf(f, "# pack %s (%s)\n", dm_aot_pack_path(p), at ? "its positions give each sample's instruction" : "no positions: every function's samples at its entry");
        fprintf(f, "# %llu samples in %zu of its %zu functions: %llu at %zu guest instructions, %llu at the entries of the %zu functions with none at an instruction, %llu not "
                   "written (host calls, SVCs and code with no instruction of functions with samples at their instructions); %s\n", (unsigned long long)sum, fns, n,
                (unsigned long long)sum_at, offsets, (unsigned long long)sum_entries, entries,
                (unsigned long long)(sum > sum_at + sum_entries ? sum - sum_at - sum_entries : 0), summary);
        fprintf(f, "# <module offset> <samples>: a guest instruction's, or a function's at its entry when it has none at an instruction\n");
        for (size_t j = 0; j < k; j++) fprintf(f, "0x%llx %llu\n", (unsigned long long)rows[j].off, (unsigned long long)rows[j].n);
        free(rows);
        if (fclose(f) || replace_file(tmp, path)) { LOG("[RED] aot: profile: cannot write %s", path); bad++; continue; }
        LOG("aot: profile: %s: %llu samples in %zu functions: %llu at %zu guest instructions, %llu at %zu entries (%s)", path, (unsigned long long)sum, fns,
            (unsigned long long)sum_at, offsets, (unsigned long long)sum_entries, entries, m->name);
        files++;
    }
    if (!files && !bad) LOG("aot: profile: no samples in any pack's code: nothing written to %s", dir);
    write_misses(dir, &bad);   // beside the profile (aot.h, "The JIT fallback's misses")
    return bad ? -1 : 0;
}
