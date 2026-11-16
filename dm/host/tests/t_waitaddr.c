// Address arbitration through real thread SVCs (WaitForAddress 0x34, SignalToAddress 0x35) with waiters on two addresses: each signal counts and wakes only the waiters
// of its own address, the highest priority first, and type 2 writes the value its waiter count calls for. Run with the waiters indexed by address (DM_WAIT_INDEX=1, the default)
// and with the whole wait chain walked (DM_WAIT_INDEX=0)
#include "../hle_thread.c"
#include <assert.h>
#include <unistd.h>

enum { NW = 5 };
static thr_t ws[NW];
static _Atomic uint32_t word_a, word_b;
static _Atomic int woken[NW];
static void *waiter(void *arg) {
    thr_t *t = arg; int i = (int)(t - ws);
    pthread_setspecific(g_keythr, t);
    uint64_t r[32] = {(uint64_t)(i < 3 ? &word_a : &word_b), 2, 0, (uint64_t)-1};   // wait while the word equals 0
    svc_thread(0x34, r);
    assert(r[0] == 0);
    atomic_store(&woken[i], 1);
    thread_exit(t);
    return NULL;
}
static int waiting(int i) { g_lock(); int w = ws[i].wkind == 3; g_unlock(); return w; }
static void settle(void) { usleep(20000); }
static void signal_addr(_Atomic uint32_t *w, int type, int32_t value, int count) { uint64_t r[32] = {(uint64_t)w, (uint64_t)type, (uint64_t)(uint32_t)value, (uint64_t)(int64_t)count}; svc_thread(0x35, r); assert(r[0] == 0); }
int main(void) {
    alarm(20);
    svc_thread_main_init();
    const int prio[NW] = {46, 45, 46, 44, 44};   // waiters 0-2 on A (1 first by priority), 3-4 on B
    pthread_t th[NW];
    for (int i = 0; i < NW; i++) {
        thr_t *t = &ws[i]; memset(t, 0, sizeof *t);
        pthread_cond_init(&t->cv, NULL); pthread_cond_init(&t->pcv, NULL);
        t->started = 1; t->prio = prio[i]; t->core = 0; t->mask = 7;
        g_lock(); t->handle = h_new(O_THREAD, t); t->all = g_all; g_all = t; g_unlock();
        assert(!pthread_create(&th[i], NULL, waiter, t));
        while (!waiting(i)) usleep(100);
    }
    signal_addr(&word_a, 2, 0, 1);   // three waiters, one to wake: the value stays
    settle();
    assert(atomic_load(&woken[1]) && !atomic_load(&woken[0]) && !atomic_load(&woken[2]) && !atomic_load(&woken[3]) && !atomic_load(&woken[4]));
    assert(atomic_load(&word_a) == 0 && waiting(0) && waiting(2) && waiting(3) && waiting(4));
    puts("PASS: a signal to one waiter of A wakes its highest priority, counts only A's waiters (the value stays) and leaves B's");
    signal_addr(&word_a, 2, 0, -1);   // all of A's two remaining waiters: the value goes down by 2
    settle();
    assert(atomic_load(&woken[0]) && atomic_load(&woken[2]) && !atomic_load(&woken[3]) && !atomic_load(&woken[4]));
    assert(atomic_load(&word_a) == (uint32_t)-2 && waiting(3) && waiting(4));
    puts("PASS: waking all of A's waiters writes the value less 2 and leaves B's waiting");
    signal_addr(&word_b, 1, 0, -1);   // increment if equal, wake all of B's
    settle();
    assert(atomic_load(&woken[3]) && atomic_load(&woken[4]) && atomic_load(&word_b) == 1);
    puts("PASS: B's waiters wake on their own signal");
    for (int i = 0; i < NW; i++) pthread_join(th[i], NULL);
    return 0;
}

