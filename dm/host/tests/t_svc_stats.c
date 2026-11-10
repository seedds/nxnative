// DM_SVC_STATS (hle_thread.c svc_stats_line): SVCs by number, system counter reads and blocked waits per frame, summed over the threads, with an exited thread's counts kept
// and nothing counted while it is off. The cntpct gate and GetSystemTick count their reads in dm_clock_reads (hle_os.c guest_clock_read_wait), which this test adds to itself.
#include "../hle_thread.c"
#include <assert.h>

static void expect(const char *line, const char *want) {
    if (!strstr(line, want)) { fprintf(stderr, "[RED] \"%s\" not in: %s\n", want, line); exit(1); }
}
static uint64_t svc(int num, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) { uint64_t r[32] = {a0, a1, a2, a3}; svc_thread(num, r); return r[0]; }
static _Atomic uint32_t word;
static void *worker(void *arg) {
    thr_t *t = arg; pthread_setspecific(g_keythr, t);
    pthread_mutex_lock(&G); t->clock_reads = &dm_clock_reads; pthread_mutex_unlock(&G);
    for (int i = 0; i < 4; i++) svc(0x0C, 0, t->handle, 0, 0);   // GetThreadPriority
    for (int i = 0; i < 2; i++) assert(svc(0x34, (uint64_t)&word, 2, 0, 1000000) == R_TIMEOUT);   // WaitForAddress that blocks for 1 ms
    for (int i = 0; i < 2; i++) assert(svc(0x34, (uint64_t)&word, 2, 0, 0) == R_TIMEOUT);         // a poll: not a blocked wait
    dm_clock_reads += 2;   // two cntpct reads
    thread_exit(t);
    return NULL;
}
int main(void) {
    setenv("DM_SVC_STATS", "2", 1);
    uint32_t me = svc_thread_main_init(); assert(g_svc_stats == 2);
    char line[640]; svc_stats_line(line, sizeof line, 1);   // the baseline

    for (int i = 0; i < 8; i++) svc(0x0C, 0, me, 0, 0);
    for (int i = 0; i < 2; i++) { dm_clock_reads++; svc(0x1E, 0, 0, 0, 0); }   // GetSystemTick (hle_svc.c; svc_thread only counts it here)
    dm_clock_reads += 4;   // four cntpct reads
    thr_t *t = calloc(1, sizeof *t); assert(t);
    pthread_cond_init(&t->cv, NULL); pthread_cond_init(&t->pcv, NULL); t->started = 1; t->prio = 44; t->mask = 1;
    pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
    pthread_t pt; assert(!pthread_create(&pt, NULL, worker, t)); assert(!pthread_join(pt, NULL));
    assert(t->exited);
    svc_stats_line(line, sizeof line, 2);
    printf("  %s\n", line);
    expect(line, "SVCs 9 (0xc 6, WaitForAddress 2, GetSystemTick 1)");
    expect(line, "system counter reads 4 (cntpct 3, GetSystemTick 1)");
    expect(line, "waits that blocked 1 (lock 0, condition 0, address 1, handles 0)");
    puts("PASS: per frame SVCs and the most frequent, system counter reads and blocked waits, an exited thread's counts kept");

    svc_stats_line(line, sizeof line, 2);
    expect(line, "SVCs 0 (); system counter reads 0 (cntpct 0, GetSystemTick 0); waits that blocked 0 ");
    puts("PASS: nothing since the last line");

    g_svc_stats = 0;
    for (int i = 0; i < 8; i++) svc(0x0C, 0, me, 0, 0);
    assert(svc(0x34, (uint64_t)&word, 2, 0, 1000000) == R_TIMEOUT);
    g_svc_stats = 2;
    svc_stats_line(line, sizeof line, 1);
    expect(line, "SVCs 0 ()"); expect(line, "waits that blocked 0 ");
    puts("PASS: off, no SVC or wait is counted");
    return 0;
}
