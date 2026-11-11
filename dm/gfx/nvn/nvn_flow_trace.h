// Optional command-buffer and CPU-wait intervals. No file I/O on the hot path.
#ifndef NVN_FLOW_TRACE_H
#define NVN_FLOW_TRACE_H
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#ifndef NVN_FLOW_TIME_ZERO
#define NVN_FLOW_TIME_ZERO 0.0
#endif

enum { NVN_FLOW_CAP = 524288 };
typedef struct {
    const char *kind;
    uint64_t frame, thread;
    double start, end;
    unsigned gpu;
    _Atomic unsigned ready;
} nvn_flow_entry;
static nvn_flow_entry *g_flow_entries;
static const char *g_flow_path;
static _Atomic uint64_t g_flow_count;
static _Atomic uint64_t g_flow_filtered;
static double g_flow_from = -HUGE_VAL, g_flow_to = HUGE_VAL;
static _Atomic unsigned g_flow_writers, g_flow_frozen;
static pthread_once_t g_flow_once = PTHREAD_ONCE_INIT;

static double nvn_flow_clock(void) {
#ifdef __APPLE__
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) * 1e-9;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}
static void nvn_flow_dump(void) {
    if (!g_flow_entries) return;
    atomic_store(&g_flow_frozen, 1);
    double deadline = nvn_flow_clock() + .01;
    while (atomic_load(&g_flow_writers) && nvn_flow_clock() < deadline) {
        struct timespec pause = {0, 100000}; nanosleep(&pause, NULL);
    }
    if (atomic_load(&g_flow_writers)) {
        fprintf(stderr, "[flow] unfinished writers; trace not exported\n"); return;
    }
    FILE *f = fopen(g_flow_path, "w");
    if (!f) { fprintf(stderr, "[flow] cannot write %s\n", g_flow_path); return; }
    uint64_t total = atomic_load_explicit(&g_flow_count, memory_order_acquire);
    fprintf(f, "# capacity=%u requested=%llu overflow=%llu from=%.9f to=%.9f filtered=%llu\n", NVN_FLOW_CAP,
            (unsigned long long)total,
            (unsigned long long)(total > NVN_FLOW_CAP ? total - NVN_FLOW_CAP : 0),
            g_flow_from, g_flow_to, (unsigned long long)atomic_load(&g_flow_filtered));
    fputs("kind\tframe\tthread\tgpu\tstart_s\tend_s\n", f);
    uint64_t limit = total < NVN_FLOW_CAP ? total : NVN_FLOW_CAP;
    for (uint64_t i = 0; i < limit; i++) {
        nvn_flow_entry *e = &g_flow_entries[i];
        if (!atomic_load_explicit(&e->ready, memory_order_acquire)) continue;
        fprintf(f, "%s\t%llu\t%llu\t%u\t%.9f\t%.9f\n", e->kind,
                (unsigned long long)e->frame, (unsigned long long)e->thread,
                e->gpu, e->start, e->end);
    }
    fclose(f);
}
static void nvn_flow_init(void) {
    g_flow_path = getenv("DM_NVN_FLOW");
    if (!g_flow_path || !*g_flow_path) return;
    const char *from = getenv("DM_NVN_FLOW_FROM"), *to = getenv("DM_NVN_FLOW_TO");
    if (from) g_flow_from = strtod(from, NULL);
    if (to) g_flow_to = strtod(to, NULL);
    if (!(g_flow_from < g_flow_to)) { fprintf(stderr, "[flow] invalid time window\n"); return; }
    g_flow_entries = calloc(NVN_FLOW_CAP, sizeof *g_flow_entries);
    if (!g_flow_entries) { fprintf(stderr, "[flow] allocation failed\n"); return; }
    atexit(nvn_flow_dump);
}
static int nvn_flow_on(void) {
    pthread_once(&g_flow_once, nvn_flow_init);
    return g_flow_entries != NULL;
}
// kind must have static lifetime. GPU feedback intervals keep the submitted
// frame ID; CPU waits have no reliable cross-thread frame ID (UINT64_MAX).
static void nvn_flow_event(const char *kind, uint64_t frame, unsigned gpu,
                           double start, double end) {
    if (!nvn_flow_on() || !isfinite(start) || !isfinite(end) || !(start > 0 && end >= start)) return;
    double zero = NVN_FLOW_TIME_ZERO, a = start - zero, b = end - zero;
    if (a >= g_flow_to || (b <= g_flow_from && a < g_flow_from)) {
        atomic_fetch_add_explicit(&g_flow_filtered, 1, memory_order_relaxed); return;
    }
    atomic_fetch_add(&g_flow_writers, 1);
    if (atomic_load(&g_flow_frozen)) { atomic_fetch_sub(&g_flow_writers, 1); return; }
    uint64_t i = atomic_fetch_add_explicit(&g_flow_count, 1, memory_order_relaxed);
    if (i >= NVN_FLOW_CAP) { atomic_fetch_sub(&g_flow_writers, 1); return; }
    nvn_flow_entry *e = &g_flow_entries[i];
    e->kind = kind; e->frame = frame; e->gpu = gpu; e->start = start; e->end = end;
#ifdef __APPLE__
    pthread_threadid_np(NULL, &e->thread);
#else
    e->thread = (uint64_t)(uintptr_t)pthread_self();
#endif
    atomic_store_explicit(&e->ready, 1, memory_order_release);
    atomic_fetch_sub(&g_flow_writers, 1);
}
static double nvn_flow_begin(void) { return nvn_flow_on() ? nvn_flow_clock() : 0; }
static void nvn_flow_cpu(const char *kind, double start) {
    if (!start) return;
    double end = nvn_flow_clock();
    if (end - start >= 1e-6) nvn_flow_event(kind, UINT64_MAX, 0, start, end);
}
#endif
