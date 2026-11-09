// Stop-the-world stress, modelled on IL2CPP's Boehm collector: workers alternate a real SVC (SleepThread through the gate's thr_svc_enter / svc_thread / thr_svc_leave) with bursts of "guest
// code" that read a shared object word; a collector pauses every worker (SetThreadActivity), reads each context (GetThreadContext3), sets bit 0 of the object word (the liveness mark
// that turns a klass pointer into klass|1) and checks that no worker ran meanwhile, then resumes them. A worker that runs while the world is stopped is the bug: FMOD's stream thread
// jumped through klass|1. Usage: t_pause_stress [--seconds N] [--cycles N]; exit status 1 on any violation or a stuck pause.
// Build with -DDM_TEST_MODULE_OF: module_of reports guest code through a thread-local flag, so the SIGUSR2 handler stops a worker only inside a burst.
#include "../hle_thread.c"
#include <stdarg.h>
#include <unistd.h>

static __thread int in_guest;
module_t *module_of(const void *p) { (void)p; static char guest_module; return in_guest ? (module_t *)&guest_module : NULL; }

#define NW 4
enum { V_MOVED, V_MARK, V_ZERO, V_KINDS };
static const char *const kind_name[V_KINDS] = {"moved_while_paused", "saw_mark", "zero_context"};
static _Atomic uint64_t viol[V_KINDS];
static _Atomic int64_t viol_cycle[V_KINDS], viol_worker[V_KINDS];   // first occurrence (-1: none)
static _Atomic uint64_t g_cycle;
static const char *_Atomic g_phase = "start";
static _Atomic uint64_t object_word;   // the collector's mark bit lives here, as IL2CPP's liveness sets bit 0 of an object's klass word
static _Atomic int g_stop;

static void violation(int kind, int worker) {
    atomic_fetch_add(&viol[kind], 1);
    int64_t none = -1;
    if (atomic_compare_exchange_strong(&viol_cycle[kind], &none, (int64_t)atomic_load(&g_cycle))) atomic_store(&viol_worker[kind], worker);
}

typedef struct {
    thr_t t; pthread_t th; int idx;
    _Atomic uint64_t progress;
    uint64_t regs[100];   // the gate frame: ctx_fill reads r[96..99]
} wk_t;
static wk_t *g_w[NW];

static void *worker_main(void *p) {
    wk_t *w = p;
    pthread_setspecific(g_keythr, &w->t);
    w->t.mport = pthread_mach_thread_np(pthread_self());
    uint64_t rng = 0x9E3779B97F4A7C15ull * (uint64_t)(w->idx + 1);
    while (!atomic_load(&g_stop)) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        thr_svc_enter(w->regs, 0x71000);   // what the SVC gate does around a service call (hle.c gate_dispatch)
        uint64_t r[32] = {20000 + rng % 280000};   // SleepThread(20-300 µs): a real timed sleep
        svc_thread(0x0B, r);
        thr_svc_leave();
        uint64_t n = 1000 + (rng >> 20) % 40000;
        in_guest = 1;
        for (uint64_t i = 0; i < n; i++) {
            atomic_store_explicit(&w->progress, atomic_load_explicit(&w->progress, memory_order_relaxed) + 1, memory_order_relaxed);
            if (atomic_load_explicit(&object_word, memory_order_relaxed) & 1) { violation(V_MARK, w->idx); break; }
        }
        in_guest = 0;
    }
    return NULL;
}

static void *watchdog(void *arg) {   // a pause that never returns would hang the collector: report and end the run
    (void)arg;
    uint64_t last = ~0ull; int idle = 0;
    for (;;) {
        sleep(1);
        uint64_t c = atomic_load(&g_cycle);
        if (c != last) { last = c; idle = 0; continue; }
        if (++idle >= 5) {
            printf("FAIL: no cycle completed for 5 s: collector stuck at cycle %llu in %s\n", (unsigned long long)c, (const char *)atomic_load(&g_phase));
            fflush(stdout); _exit(1);
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    int seconds = 3; uint64_t max_cycles = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cycles") && i + 1 < argc) max_cycles = strtoull(argv[++i], NULL, 10);
        else { fprintf(stderr, "usage: %s [--seconds N] [--cycles N]\n", argv[0]); return 2; }
    }
    setenv("DM_PREEMPT", "0", 1);
    uint32_t main_handle = svc_thread_main_init(); (void)main_handle;   // the collector is the main guest thread
    pthread_key_create(&g_key18, NULL);
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int k = 0; k < V_KINDS; k++) atomic_store(&viol_cycle[k], -1), atomic_store(&viol_worker[k], -1);
    for (int i = 0; i < NW; i++) {
        wk_t *w = g_w[i] = calloc(1, sizeof *w); thr_t *t = &w->t; w->idx = i;
        pthread_cond_init(&t->cv, NULL); pthread_cond_init(&t->pcv, NULL);
        t->started = 1; t->prio = 44; t->core = 1 + i % 2; t->mask = 1ull << t->core;
        w->regs[99] = 0x7fff0000;   // the gate block's sp
        pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
        if (pthread_create(&w->th, NULL, worker_main, w)) { perror("pthread_create"); return 2; }
        t->pt = w->th;
    }
    pthread_t wd; pthread_create(&wd, NULL, watchdog, NULL);
    usleep(10000);

    uint64_t t_end = now_ns() + (uint64_t)seconds * 1000000000ull, t_start = now_ns(), cycles = 0;
    while (now_ns() < t_end && (!max_cycles || cycles < max_cycles)) {
        atomic_store(&g_cycle, cycles + 1);
        atomic_store(&g_phase, "pause");
        for (int i = 0; i < NW; i++) {
            uint64_t r[32] = {g_w[i]->t.handle, 1};
            svc_thread(0x32, r);
            if (r[0]) { printf("FAIL: cycle %llu: pause of worker %d returned %#llx\n", (unsigned long long)cycles + 1, i, (unsigned long long)r[0]); return 1; }
        }
        // the world is stopped: roots are read from the contexts, then the marking runs
        atomic_store(&g_phase, "context");
        for (int i = 0; i < NW; i++) {
            hz_ctx_t o; uint64_t r[32] = {(uint64_t)&o, g_w[i]->t.handle};
            memset(&o, 0, sizeof o);
            svc_thread(0x33, r);
            if (r[0] || !o.sp) violation(V_ZERO, i);
        }
        atomic_store(&g_phase, "mark");
        uint64_t before[NW];
        for (int i = 0; i < NW; i++) before[i] = atomic_load(&g_w[i]->progress);
        atomic_fetch_or(&object_word, 1);
        uint64_t d = now_ns() + 200000; while (now_ns() < d) {}
        atomic_fetch_and(&object_word, ~1ull);
        for (int i = 0; i < NW; i++) if (atomic_load(&g_w[i]->progress) != before[i]) violation(V_MOVED, i);
        atomic_store(&g_phase, "resume");
        for (int i = 0; i < NW; i++) { uint64_t r[32] = {g_w[i]->t.handle, 0}; svc_thread(0x32, r); }
        cycles++;
        usleep(50);   // the mutators run between collections
    }
    atomic_store(&g_phase, "stop");
    atomic_store(&g_stop, 1);
    for (int i = 0; i < NW; i++) pthread_join(g_w[i]->th, NULL);

    uint64_t total = 0;
    printf("t_pause_stress: %d workers, %llu stop-the-world cycles in %.2f s\n", NW, (unsigned long long)cycles, (double)(now_ns() - t_start) / 1e9);
    for (int k = 0; k < V_KINDS; k++) {
        total += viol[k];
        if (viol[k]) printf("  %-20s %llu (first: cycle %lld, worker %lld)\n", kind_name[k], (unsigned long long)viol[k], (long long)viol_cycle[k], (long long)viol_worker[k]);
        else printf("  %-20s 0\n", kind_name[k]);
    }
    printf("%s: %llu violations\n", total ? "FAIL" : "PASS", (unsigned long long)total);
    return total ? 1 : 0;
}
