// The uniform copy hand-off (nvn_rec.h ubc_push / ubc_wait / ubc_thread): a waiter and a pusher wake the sleeping copy thread with one call per sleep, a waiter that outlasts
// its spin sleeps and is woken once, and no push is lost (each one is made without a waiter calling in). Before, ubc_wait locked the copy mutex and signalled on every spin
// while the thread slept, and the woken thread could not get that mutex back: Ocarina of Time's replay thread spent half its busy time in ubc_wait on one build (170-330k
// instructions a draw against 32-48k). No GPU work (CPU-only list in tests.yml).
static _Atomic int g_late;   // case 5: the copy thread is held on its way to sleep (flag set, word read), where a late wake call can land
#define UBC_T_SLEEPING() do { if (atomic_load(&g_late)) usleep(20000); } while (0)
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, ...) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static int asleep(void) {   // the copy thread went to sleep (bounded: 2 s)
    for (int i = 0; i < 2000; i++) { if (atomic_load(&g_ubc_sleeping)) return 1; usleep(1000); }
    return 0;
}
static uint64_t ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static int cmp64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }

// A second waiter, as the encode thread waits at a commit while the replay thread queues
static _Atomic int g_stop; static _Atomic uint64_t g_side_waits;
static void *side_waiter(void *arg) { (void)arg; while (!atomic_load(&g_stop)) { ubc_wait(ubc_head()); atomic_fetch_add(&g_side_waits, 1); } return NULL; }

int main(void) { @autoreleasepool {
    alarm(60);   // a lost wake-up hangs: the alarm ends the test as a failure
    uint64_t a = 7, b = 0; ubc_wait(ubc_push(&b, &a, sizeof a));   // starts the copy thread
    CHECK(b == 7, "the first copy was not made");

    // 1. A wait on copies queued while the copy thread slept: the push wakes it, the wait adds no wake call (it used to signal on every spin until the thread had the mutex)
    enum { N1 = 300 }; uint64_t lat[N1]; int n1 = 0, maxw = 0; long sumw = 0;
    for (int i = 0; i < N1; i++) {
        if (!asleep()) { CHECK(0, "the copy thread did not go to sleep"); break; }
        uint64_t src = 0x1234567800ull + (uint64_t)i, dst = 0, w0 = atomic_load(&g_ubc_wakes), t0 = ns();
        ubc_wait(ubc_push(&dst, &src, sizeof src));
        lat[n1++] = ns() - t0;
        int w = (int)(atomic_load(&g_ubc_wakes) - w0); sumw += w; if (w > maxw) maxw = w;
        CHECK(dst == src, "trial %d: the wait returned before its copy was made", i);
    }
    CHECK(maxw <= 2, "a push and wait on a sleeping copy thread made %d wake calls (want 1, or 2 when the waiter outlasted its spin and slept)", maxw);
    qsort(lat, (size_t)n1, sizeof *lat, cmp64);

    // 2. A waiter that outlasts its spin sleeps on g_ubc_done and the copy thread wakes it once: a 64 MB copy
    enum { BIG = 64 << 20 }; uint8_t *bs = malloc(BIG), *bd = calloc(1, BIG); memset(bs, 0x5a, BIG);
    asleep(); uint64_t w0 = atomic_load(&g_ubc_wakes), t0 = ns();
    ubc_wait(ubc_push(bd, bs, BIG));
    uint64_t big_ns = ns() - t0; int bigw = (int)(atomic_load(&g_ubc_wakes) - w0);
    CHECK(bd[0] == 0x5a && bd[BIG - 1] == 0x5a && !memcmp(bs, bd, BIG), "the long copy was not made when its wait returned");
    CHECK(bigw <= 2 && !atomic_load(&g_ubc_waiting), "the long wait made %d wake calls, waiting flag %d (want at most 2, flag clear)", bigw, (int)atomic_load(&g_ubc_waiting));
    free(bs); free(bd);

    // 3. No lost push: pushes land at random points of the copy thread's way to sleep, nobody waits; each must be made from the push's own wake (polled, 2 s)
    int lost = 0;
    for (int i = 0; i < 2000 && !lost; i++) {
        uint64_t src = (uint64_t)i * 3 + 1, dst = 0; unsigned d = arc4random_uniform(4000);
        for (volatile unsigned k = 0; k < d; k++) {}
        if (i % 50 == 0) usleep(200);   // and sometimes fully asleep
        uint64_t seq = ubc_push(&dst, &src, sizeof src), t = ns();
        while (atomic_load_explicit(&g_ubc_done, memory_order_acquire) < seq) { if (ns() - t > 2000000000ull) { lost = 1; break; } usleep(10); }
        if (!lost) CHECK(dst == src, "push %d: done counted before the copy was made", i);
    }
    CHECK(!lost, "a push was not made within 2 s without a waiter (lost wake-up)");

    // 4. Stress, two waiters: the replay thread queues copies (ring wrap included) and waits for some, another thread waits for whatever is queued; every copy in
    // order, and the wake calls stay below one per push / wait. 40 rounds of 50,000: a waker that took the copy thread's flag with a late wake call (the hand-off's
    // first version) lost the next push's wake about once in 40 rounds here, and the alarm ends the hang
    enum { NS = 50000, ROUNDS = 40, SLOTS = 1024 }; static uint64_t ssrc[SLOTS], sdst[SLOTS];
    pthread_t th; pthread_create(&th, NULL, side_waiter, NULL);
    uint64_t w1 = atomic_load(&g_ubc_wakes), waits = 0, t1 = ns(); int bad = 0;
    for (int r = 0; r < ROUNDS; r++) {
        memset(sdst, 0, sizeof sdst);
        for (int i = 0; i < NS; i++) {
            int s = i % SLOTS; ssrc[s] = (uint64_t)i ^ ((uint64_t)r << 32) ^ 0xabcdef; uint64_t seq = ubc_push(&sdst[s], &ssrc[s], sizeof ssrc[s]);
            if (s == SLOTS - 1 || i % 7 == 0) { ubc_wait(seq); waits++; for (int k = 0; k <= s; k++) if (sdst[k] != ssrc[k]) bad++; if (s == SLOTS - 1) memset(sdst, 0, sizeof sdst); }
        }
        ubc_wait(ubc_head()); waits++;
        usleep(300);   // the copy thread goes to sleep between rounds
    }
    atomic_store(&g_stop, 1); pthread_join(th, NULL);
    uint64_t stress_ns = ns() - t1, sw = atomic_load(&g_ubc_wakes) - w1, side = atomic_load(&g_side_waits);
    CHECK(!bad, "%d copies not made (or out of order) when a wait for them returned", bad);
    CHECK(sw <= (uint64_t)NS * ROUNDS + waits + side, "%llu wake calls for %d pushes and %llu waits", (unsigned long long)sw, NS * ROUNDS, (unsigned long long)(waits + side));

    // 5. A late wake call: a pusher preempted between its store and its wake call makes the call after the copy thread made that op, set its flag and read the word
    // for its next sleep. The call must not take the flag, or the thread sleeps with it clear and the next push does not wake it (every later wait hangs). The hook
    // holds the thread in that spot, and the late call is made by hand, so this is deterministic
    int late_lost = 0;
    for (int i = 0; i < 5 && !late_lost; i++) {
        asleep(); uint64_t s1 = 11, d1 = 0, s2 = 22, d2 = 0;
        atomic_store(&g_late, 1); uint64_t q1 = ubc_push(&d1, &s1, sizeof s1);
        while (atomic_load(&g_ubc_done) < q1) usleep(100);
        usleep(5000); ubc_kick(); atomic_store(&g_late, 0);   // the first push's late wake call, while the thread is held
        usleep(40000);   // it sleeps now
        uint64_t seq = ubc_push(&d2, &s2, sizeof s2), t = ns();
        while (atomic_load_explicit(&g_ubc_done, memory_order_acquire) < seq) { if (ns() - t > 2000000000ull) { late_lost = 1; break; } usleep(100); }
        CHECK(!late_lost && d1 == 11 && d2 == 22, "a late wake call took the copy thread's flag: the next push was not made within 2 s (round %d)", i);
        if (late_lost) { fprintf(stderr, "t_ubc_wait: %d checks, %d failed (the copy thread is lost asleep)\n", checks, failed); _exit(1); }
    }

    printf("t_ubc_wait: sleeping-thread wait p50 %.1f us p99 %.1f us, %.2f wake calls a wait (max %d); 64 MB copy %.1f ms, %d wakes; stress %d pushes %llu+%llu waits %.0f ms, %llu wake calls; %d checks, %d failed\n",
           n1 ? lat[n1 / 2] / 1e3 : 0, n1 ? lat[n1 * 99 / 100] / 1e3 : 0, n1 ? (double)sumw / n1 : 0, maxw, big_ns / 1e6, bigw, NS * ROUNDS, (unsigned long long)waits, (unsigned long long)side, stress_ns / 1e6, (unsigned long long)sw, checks, failed);
    fflush(stdout);
    _exit(failed != 0);
}}
