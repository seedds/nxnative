// Threads and the mutex of rt/thread.h on Horizon's SVCs (rt/thread.S).
#include "thread.h"

#define RT_WAIT_MASK 0x40000000u  // Horizon's HandleWaitMask

// The entry the kernel starts a thread at (x0 = its rt_thread): its TLS block in tpidr_el0 (the local-exec model reads it, as crt0.S sets the
// main thread's), its function, then svcExitThread (Horizon threads must not return from their entry)
static void rt_thread_entry(void *p) {
    rt_thread *t = (rt_thread *)p;
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(t->tls) : "memory");
    t->fn(t, t->arg);
    rt_svc_exit_thread();
}

uint32_t rt_thread_start(rt_thread *t, void (*fn)(rt_thread *, void *), void *arg, void *stack, size_t stack_size) {
    for (size_t i = 0; i < sizeof t->tls; ++i) {
        t->tls[i] = 0;
    }
    t->fn = fn;
    t->arg = arg;
    void *top = (void *)(((uintptr_t)stack + stack_size) & ~(uintptr_t)15);
    uint32_t r = rt_svc_create_thread(&t->handle, rt_thread_entry, t, top, 44, -2);
    if (r == 0) {
        r = rt_svc_start_thread(t->handle);
    }
    return r;
}

void rt_thread_join(rt_thread *t) {
    int32_t index = -1;
    const uint32_t r = rt_svc_wait_synchronization(&index, &t->handle, 1, -1);
    if (r != 0 || index != 0) {
        rt_say("[RED] rt_thread_join: svcWaitSynchronization failed");
        rt_svc_exit_process();
    }
    rt_svc_close_handle(t->handle);
}

// libnx's mutex: the word is 0 when free, else the owner's handle with the waiters bit set once a thread waits in ArbitrateLock. The fast paths
// are exclusive loops (compare-and-swap: ldaxr / stlxr); the kernel hands the lock to the first waiter at ArbitrateUnlock
void rt_mutex_lock(volatile uint32_t *m, uint32_t self) {
    for (;;) {
        uint32_t cur = 0;
        if (__atomic_compare_exchange_n(m, &cur, self, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return;
        }
        if (!(cur & RT_WAIT_MASK)) {
            if (!__atomic_compare_exchange_n(m, &cur, cur | RT_WAIT_MASK, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                continue;  // it changed: look again
            }
        }
        rt_svc_arbitrate_lock(cur & ~RT_WAIT_MASK, m, self);
        if ((__atomic_load_n(m, __ATOMIC_ACQUIRE) & ~RT_WAIT_MASK) == self) {
            return;  // handed over by ArbitrateUnlock
        }
    }
}

void rt_mutex_unlock(volatile uint32_t *m, uint32_t self) {
    uint32_t cur = self;
    if (__atomic_compare_exchange_n(m, &cur, 0, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
        return;  // nobody waits
    }
    rt_svc_arbitrate_unlock(m);
}
