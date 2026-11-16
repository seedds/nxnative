// Guest waits and wakes through real thread SVCs (WaitForAddress 0x34, SignalToAddress 0x35) with the scheduler lock G busy: two threads hand a word back and forth
// 20,000 times while a third takes G over and over (GetThreadPriority), so wakes land between a waiter's unlock and its sleep and takers find G held. A lost wake hangs the
// test (alarm). Then a timed wait without a signal ends in a timeout no earlier than asked, and one signalled in time returns 0. The Makefile runs it as the host does by
// default (block's wait on the thread's word, a spin for a busy G) and with DM_WAIT_ON_ADDRESS=0 DM_G_SPIN=0 (condition variable, sleep on a busy G)
#include "../hle_thread.c"
#include <assert.h>
#include <unistd.h>

enum { ROUNDS = 20000, NT = 3 };
static thr_t ts[NT];
static _Atomic uint32_t word, tword;
static _Atomic int done;
static void wait_eq(_Atomic uint32_t *w, uint32_t v) { uint64_t r[32] = {(uint64_t)w, 2, v, (uint64_t)-1}; svc_thread(0x34, r); assert(r[0] == 0 || r[0] == R_BAD_STATE); }
static void wake_one(_Atomic uint32_t *w) { uint64_t r[32] = {(uint64_t)w, 0, 0, 1}; svc_thread(0x35, r); assert(r[0] == 0); }
static void *ping(void *arg) {   // word odd: ping's turn is over
    pthread_setspecific(g_keythr, arg);
    for (uint32_t i = 0; i < ROUNDS; i++) { atomic_store(&word, 2 * i + 1); wake_one(&word); while (atomic_load(&word) == 2 * i + 1) wait_eq(&word, 2 * i + 1); }
    return NULL;
}
static void *pong(void *arg) {
    pthread_setspecific(g_keythr, arg);
    for (uint32_t i = 0; i < ROUNDS; i++) { while (atomic_load(&word) == 2 * i) wait_eq(&word, 2 * i); atomic_store(&word, 2 * i + 2); wake_one(&word); }
    return NULL;
}
static void *noise(void *arg) {
    pthread_setspecific(g_keythr, arg);
    thr_t *t = arg; uint64_t n = 0;
    while (!atomic_load(&done)) { uint64_t r[32] = {0, t->handle}; svc_thread(0x0C, r); assert(r[0] == 0); n++; }
    return (void *)(uintptr_t)n;
}
static void *late_signal(void *arg) { (void)arg; usleep(2000); atomic_store(&tword, 1); g_lock(); thr_t *w = pick(3, (uint64_t)&tword); g_unlock(); assert(w); wake_one(&tword); return NULL; }
int main(void) {
    alarm(60);
    svc_thread_main_init();
    void *(*fn[NT])(void *) = {ping, pong, noise};
    pthread_t th[NT];
    for (int i = 0; i < NT; i++) {
        thr_t *t = &ts[i]; memset(t, 0, sizeof *t);
        pthread_cond_init(&t->cv, NULL); pthread_cond_init(&t->pcv, NULL);
        t->started = 1; t->prio = 44; t->core = i; t->mask = 7;
        g_lock(); t->handle = h_new(O_THREAD, t); t->all = g_all; g_all = t; g_unlock();
    }
    uint64_t t0 = now_ns();
    for (int i = 0; i < NT; i++) assert(!pthread_create(&th[i], NULL, fn[i], &ts[i]));
    pthread_join(th[0], NULL); pthread_join(th[1], NULL); atomic_store(&done, 1);
    void *nn; pthread_join(th[2], &nn);
    g_lock(); uint64_t waits = g_wao_waits, busy = g_lock_busy, spun = g_lock_spun; g_unlock();
    pthread_once(&g_gspin_once, gspin_init);
    int word_on = wao_on(), spin_on = mtl_sw_on(g_gspin);
    assert(atomic_load(&word) == 2 * ROUNDS);
    assert(word_on ? waits > 0 : waits == 0);
    assert(spin_on || spun == 0);
    printf("PASS: %d hand-offs in %.0f ms (waits on the word %llu, G found taken %llu times, %llu got by a spin; %llu priority reads beside them)\n", ROUNDS, (now_ns() - t0) / 1e6,
           (unsigned long long)waits, (unsigned long long)busy, (unsigned long long)spun, (unsigned long long)(uintptr_t)nn);

    uint64_t r[32] = {(uint64_t)&tword, 2, 0, 3000000}; uint64_t s0 = now_ns(); svc_thread(0x34, r); uint64_t el = now_ns() - s0;
    assert(r[0] == R_TIMEOUT && el >= 3000000 && el < 500000000);
    printf("PASS: a 3 ms wait without a signal times out after %.2f ms\n", el / 1e6);
    pthread_t ls; assert(!pthread_create(&ls, NULL, late_signal, NULL));
    uint64_t q[32] = {(uint64_t)&tword, 2, 0, 2000000000}; s0 = now_ns(); svc_thread(0x34, q); el = now_ns() - s0;
    pthread_join(ls, NULL);
    assert(q[0] == 0 && el < 1000000000);
    printf("PASS: a 2 s wait signalled after 2 ms returns 0 after %.2f ms\n", el / 1e6);
    return 0;
}
