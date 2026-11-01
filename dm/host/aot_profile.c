// The sampler of the x86-64 execution engine (aot.h, "The sampler"; dm/tools/aot/README.md, "Tiers by profile"): DM_AOT_PROFILE=<folder> samples every thread that runs guest code
// at DM_AOT_PROFILE_HZ (1000 by default) per second of its own CPU time and writes, at process exit, the profile `nxaot build --profile` reads: <folder>/<module>.tsv per module with
// samples, one line `<module offset of a function's entry> <samples>` per sampled function. The packer (scripts/build_ipa_from_nsp.py --platform linux / windows) takes
// <NSP folder>/<id>-work/profile/<module>.tsv for the module's next pack: its functions with samples at tier H, the others at tier C (balanced mode).
// Linux: each thread arms a timer on its own CPU-time clock (timer_create with CLOCK_THREAD_CPUTIME_ID, SIGEV_THREAD_ID) when it makes its state block (aot_thread.c thread_make),
// and disarms it before the block goes. Its SIGPROF runs on the thread's alternate signal stack (aot_fault.c crash_thread_init) and reads the interrupted rip:
//   in a pack's function       that function (aot_arena.c dm_aot_fn_of_code: the function map, a split function's parts and a group's functions each to their own entry)
//   elsewhere, in a host call  the guest function that called it (dm_aot_thread.prof_at: the call's return address, an SVC's instruction; aot_run.c sets it)
//   in the JIT fallback        counted as such per thread (dynarmic keeps the guest pc in host registers while a block runs: not readable from here)
//   anything else              the host (the run time, a host call for code no pack holds, a thread between guest calls)
// The weight of a sample is 1 + the timer's overrun: the kernel checks CPU-time timers at its ticks (HZ 250 gives 4 expirations a check), so the counts stay in milliseconds of CPU time.
// The handler allocates nothing and takes no lock: a function's count is a relaxed atomic add into its pack's array (made at the pack's load, aot_arena.c), a thread's totals are
// fields of its own record, which is never freed. Windows: not yet (a sampler thread with SuspendThread / GetThreadContext would do it): a [RED] line when the switch is set
#include "aot.h"
#if !defined(_WIN32)
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
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

#if defined(_WIN32)
static void init_once(void) {
    const char *dir = getenv("DM_AOT_PROFILE");
    if (dir && *dir) LOG("[RED] aot: DM_AOT_PROFILE=%s: the sampler is Linux-only for now (no profile is written on Windows; a profile recorded on Linux serves the Windows pack too)", dir);
}
void dm_aot_profile_thread_begin(void) {}
void dm_aot_profile_thread_end(void) {}
int dm_aot_profile_write(void) { return -1; }
#else
static char g_dir[1024];
static unsigned g_hz = PROF_HZ_DEFAULT;

// A thread's record: its totals, written by its own handler only; kept to the end (the handler may run on it until its timer is gone, and the writer reads it)
typedef struct prof_thread {
    struct prof_thread *next;   // every thread sampled (g_lock)
    long tid;
    uint64_t samples, code, hostcall, jit, host;   // all, in a pack's function, in a host call or SVC of one, in the JIT fallback, in the host
    timer_t timer;
    int armed;
} prof_thread;
static _Thread_local prof_thread *t_prof;   // the calling thread's record while its timer runs, NULL otherwise (the handler then does nothing)
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static prof_thread *g_threads;
static int g_nthreads;
static _Atomic int g_sampling;              // 0 once the files are being written: the handler counts nothing more
static _Atomic int g_written;

static void count(int pack, long fn, uint64_t w) {
    const dm_aot_pack *p = dm_aot_pack_at(pack);
    _Atomic uint64_t *c = p ? dm_aot_pack_samples(p) : NULL;
    if (c) atomic_fetch_add_explicit(&c[fn], w, memory_order_relaxed);
}

static void on_prof(int sig, siginfo_t *si, void *ucv) {
    (void)sig;
    prof_thread *r = t_prof;
    if (!r || !atomic_load_explicit(&g_sampling, memory_order_relaxed)) return;
    const int saved_errno = errno;
    const uint64_t w = 1 + (uint64_t)(si && si->si_code == SI_TIMER && si->si_overrun > 0 ? si->si_overrun : 0);
    const uint64_t pc = (uint64_t)((const ucontext_t *)ucv)->uc_mcontext.gregs[REG_RIP];
    r->samples += w;
    int pack = -1;
    long fn = dm_aot_fn_of_code(pc, &pack);
    if (fn >= 0) {
        count(pack, fn, w);
        r->code += w;
    } else {
        const dm_aot_thread *t = fn == -2 ? NULL : dm_aot_self();   // -2: the pack's run-time code (enter, jit_entry, the slow-path wrappers)
        const uint64_t at = t && t->depth ? t->prof_at : 0;
        if (at == DM_AOT_PROF_JIT) r->jit += w;
        else if (at > DM_AOT_PROF_JIT && (fn = dm_aot_fn_of_guest(at, &pack)) >= 0) { count(pack, fn, w); r->hostcall += w; }
        else r->host += w;
    }
    errno = saved_errno;
}

static void write_at_exit(void) { dm_aot_profile_write(); }

static void init_once(void) {
    const char *dir = getenv("DM_AOT_PROFILE");
    if (!dir || !*dir) return;
    if (strlen(dir) >= sizeof g_dir - 32) { LOG("[RED] aot: DM_AOT_PROFILE: the folder's path is too long: no profile"); return; }
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    const char *hz = getenv("DM_AOT_PROFILE_HZ");
    if (hz && *hz) {
        const long v = strtol(hz, NULL, 10);
        if (v >= 1 && v <= PROF_HZ_MAX) g_hz = (unsigned)v;
        else LOG("[RED] aot: DM_AOT_PROFILE_HZ=%s is not 1 .. %d: %d", hz, PROF_HZ_MAX, PROF_HZ_DEFAULT);
    }
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

// The writer: one file per module with samples, most samples first
typedef struct { uint64_t off, n; } row_t;
static int row_cmp(const void *a, const void *b) {
    const row_t *x = a, *y = b;
    if (x->n != y->n) return x->n > y->n ? -1 : 1;
    return x->off < y->off ? -1 : x->off > y->off;
}
static void mkdirs(char *path) {   // mkdir -p
    for (char *s = path + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(path, 0755); *s = '/'; }
    mkdir(path, 0755);
}

int dm_aot_profile_write(void) {
    if (!dm_aot_profile_on() || atomic_exchange(&g_written, 1)) return -1;
    atomic_store(&g_sampling, 0);   // the handler counts nothing more; a sample on its way may still add one
    uint64_t all = 0, code = 0, hostcall = 0, jit = 0, host = 0;
    int threads = 0;
    pthread_mutex_lock(&g_lock);
    for (const prof_thread *r = g_threads; r; r = r->next) {
        all += r->samples; code += r->code; hostcall += r->hostcall; jit += r->jit; host += r->host;
        threads += r->samples > 0;
    }
    char summary[400];
    snprintf(summary, sizeof summary, "%u Hz of each guest thread's CPU time, %d of %d threads sampled; all modules: %llu samples, %llu in compiled code, %llu in host calls and SVCs "
             "of compiled functions, %llu in the JIT fallback, %llu elsewhere in the host", g_hz, threads, g_nthreads, (unsigned long long)all, (unsigned long long)code,
             (unsigned long long)hostcall, (unsigned long long)jit, (unsigned long long)host);
    LOG("aot: profile: %s", summary);
    int shown = 0;
    for (const prof_thread *r = g_threads; r && shown < 16; r = r->next) {
        if (!r->samples) continue;
        LOG("aot: profile: thread %ld: %llu samples: %llu compiled, %llu host calls and SVCs, %llu JIT, %llu host", r->tid, (unsigned long long)r->samples,
            (unsigned long long)r->code, (unsigned long long)r->hostcall, (unsigned long long)r->jit, (unsigned long long)r->host);
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
        size_t k = 0;
        uint64_t sum = 0;
        for (size_t j = 0; j < n; j++) if (atomic_load_explicit(&c[j], memory_order_relaxed)) k++;
        if (!k) continue;
        row_t *rows = malloc(k * sizeof *rows);
        if (!rows) { bad++; continue; }
        k = 0;
        for (size_t j = 0; j < n; j++) {
            const uint64_t v = atomic_load_explicit(&c[j], memory_order_relaxed);
            if (v) { rows[k++] = (row_t){dm_aot_pack_fn(p, j), v}; sum += v; }
        }
        qsort(rows, k, sizeof *rows, row_cmp);
        const module_t *m = dm_aot_pack_module(p);
        char name[64], path[sizeof g_dir + 96], tmp[sizeof path + 8], sha[65];
        snprintf(name, sizeof name, "%s", m->name);
        for (char *s = name; *s; s++) if (*s == '/' || *s == '\\') *s = '_';
        snprintf(path, sizeof path, "%s/%s.tsv", dir, name);
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        const uint8_t *h = dm_aot_pack_desc(p)->sha256;
        for (int b = 0; b < 32; b++) snprintf(sha + 2 * b, 3, "%02x", h[b]);
        FILE *f = fopen(tmp, "w");
        if (!f) { LOG("[RED] aot: profile: cannot write %s", tmp); free(rows); bad++; continue; }
        fprintf(f, "# nxaot profile: guest functions sampled by the x86-64 engine (dm/host/aot_profile.c, DM_AOT_PROFILE); nxaot build --profile <this file>\n");
        fprintf(f, "# module %s, build ID %s, image SHA-256 %s\n", m->name, m->build_id[0] ? m->build_id : "none", sha);
        fprintf(f, "# pack %s\n", dm_aot_pack_path(p));
        fprintf(f, "# %llu samples in %zu of its %zu functions; %s\n", (unsigned long long)sum, k, n, summary);
        fprintf(f, "# <module offset of a function's entry> <samples>\n");
        for (size_t j = 0; j < k; j++) fprintf(f, "%#llx %llu\n", (unsigned long long)rows[j].off, (unsigned long long)rows[j].n);
        free(rows);
        if (fclose(f) || rename(tmp, path)) { LOG("[RED] aot: profile: cannot write %s", path); bad++; continue; }
        LOG("aot: profile: %s: %llu samples in %zu functions (%s)", path, (unsigned long long)sum, k, m->name);
        files++;
    }
    if (!files && !bad) LOG("aot: profile: no samples in any pack's code: nothing written to %s", dir);
    return bad ? -1 : 0;
}
#endif
