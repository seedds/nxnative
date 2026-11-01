// A priority-44 guest waking a same-core priority-41 notification thread must return
// when PopNotificationMessage blocks. The notification is produced only afterwards.
#include "../hle_thread.c"
#ifndef HLE_OE_SOURCE
#define HLE_OE_SOURCE "../hle_oe.c"
#endif
#include HLE_OE_SOURCE
#include <assert.h>
#include <unistd.h>

void dm_audio_suspend(int on) { (void)on; }
void guest_clock_freeze_for(int reason, int on) { (void)reason; (void)on; }
void dm_gfx_suspend(int on) { (void)on; }

static thr_t *worker;
static uint32_t event_read;
static _Atomic uint32_t received;
static void *notification_worker(void *arg) {
    (void)arg;
    pthread_setspecific(g_keythr, worker);
    uint32_t handle = event_read;
    uint64_t r[32] = {0, (uint64_t)&handle, 1, (uint64_t)-1};
    svc_thread(0x18, r);
    assert(r[0] == 0);
    atomic_store(&received, h_oe_PopMsg());
    pthread_mutex_lock(&G);
    assert(worker->native_wait == 0 && worker->nhand == 0);
    pthread_mutex_unlock(&G);
    thread_exit(worker);
    return NULL;
}

int main(void) {
    alarm(5);   // The old implementation never returns from the parent's SignalEvent.
    unsetenv("DM_OE_SCRIPT");
    setenv("DM_PREEMPT", "0", 1);
    svc_thread_main_init();
    thr_t *parent = cur();
    parent->prio = 44; parent->core = 0; parent->mask = 1;
    dm_guest_host_wait_begin(); dm_guest_host_wait_begin();
    assert(parent->native_wait == 2 && !thr_runnable(parent));
    dm_guest_host_wait_end(); assert(parent->native_wait == 1 && !thr_runnable(parent));
    dm_guest_host_wait_end(); assert(parent->native_wait == 0 && thr_runnable(parent));

    uint64_t r[32] = {0};
    svc_thread(0x45, r); assert(r[0] == 0);
    uint32_t event_write = r[1]; event_read = r[2];
    worker = calloc(1, sizeof *worker); assert(worker);
    pthread_cond_init(&worker->cv, NULL); pthread_cond_init(&worker->pcv, NULL);
    worker->started = 1; worker->prio = 41; worker->core = 0; worker->mask = 1;
    pthread_mutex_lock(&G);
    worker->handle = h_new(O_THREAD, worker); worker->all = g_all; g_all = worker;
    pthread_mutex_unlock(&G);
    pthread_t thread; assert(!pthread_create(&thread, NULL, notification_worker, NULL));
    for (;;) {
        pthread_mutex_lock(&G); int waiting = worker->wkind == 4; pthread_mutex_unlock(&G);
        if (waiting) break;
        usleep(100);
    }
    r[0] = event_write; svc_thread(0x11, r);
    pthread_mutex_lock(&G);
    assert(worker->native_wait == 1 && worker->nhand == 0 && !thr_runnable(worker));
    pthread_mutex_unlock(&G);
    assert(atomic_load(&received) == 0 && g_mqn == 0);
    // No fabricated startup message and no timeout: only the real producer releases the pop.
    mq_push(15);
    pthread_join(thread, NULL);
    assert(atomic_load(&received) == 15);
    uint32_t msg = 999;
    assert(!h_oe_TryPopMsg(&msg) && msg == 999);
    r[0] = worker->handle; svc_thread(0x16, r); worker = NULL;
    puts("PASS: native notification wait releases the same-core handoff; one real message delivered once; nested wait state balanced");
    return 0;
}
