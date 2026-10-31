// The exclusive monitor with one thread: the C11 atomics of every width compile to ld(a)xr / st(l)xr retry loops on the Cortex-A57 (no LSE, and
// -mno-outline-atomics keeps them inline), compare-and-swap with its early exit, 128-bit pairs (ldaxp / stlxp), acquire / release accesses
// (ldar, stlr), clrex (the store after it fails), a spin lock, and a lock-free pop whose window loads through the value it loaded (the
// ABA-sensitive shape of design section 6.5). Each loop runs many times with known results.
// Then with threads (design sections 6.1 and 6.5; rt/thread.h): two workers released together (WaitForAddress / SignalToAddress) increment
// shared counters with an ldaxr / stlxr loop, a compare-and-swap loop, a 128-bit pair through ldaxp / stlxp and a libnx-style mutex
// (ArbitrateLock / ArbitrateUnlock when contended); a third thread runs the register-invariant loop of rt/pause.S, which the host's pause test
// checks paused threads against (excl_pause_iterations long); the main thread sleeps, releases them and joins them (WaitSynchronization). The
// totals must be exact.
#include "rt.h"
#include "thread.h"

static volatile uint8_t u8v;
static volatile uint16_t u16v;
static volatile uint32_t u32v;
static volatile uint64_t u64v;
static uint64_t pair[2] __attribute__((aligned(16)));

// 128-bit compare-and-swap through a pair window: returns 1 when it stored
static RT_NOINLINE int cas128(uint64_t *p, uint64_t old_lo, uint64_t old_hi, uint64_t new_lo, uint64_t new_hi) {
    uint64_t lo, hi;
    uint32_t status;
    int ok;
    __asm__ volatile(
        "1: ldaxp %[lo], %[hi], [%[p]]\n"
        "   cmp %[lo], %[olo]\n"
        "   ccmp %[hi], %[ohi], #0, eq\n"
        "   b.ne 2f\n"
        "   stlxp %w[st], %[nlo], %[nhi], [%[p]]\n"
        "   cbnz %w[st], 1b\n"
        "   mov %w[ok], #1\n"
        "   b 3f\n"
        "2: clrex\n"
        "   mov %w[ok], #0\n"
        "3:\n"
        : [lo] "=&r"(lo), [hi] "=&r"(hi), [st] "=&r"(status), [ok] "=&r"(ok)
        : [p] "r"(p), [olo] "r"(old_lo), [ohi] "r"(old_hi), [nlo] "r"(new_lo), [nhi] "r"(new_hi)
        : "cc", "memory");
    return ok;
}

// ldxr, clrex, stxr: the monitor is cleared, so the store fails (status 1) and memory keeps its value
static RT_NOINLINE uint32_t store_after_clrex(volatile uint64_t *p, uint64_t v) {
    uint64_t old;
    uint32_t status;
    __asm__ volatile("ldxr %[old], [%[p]]\n clrex\n stxr %w[st], %[v], [%[p]]" : [old] "=&r"(old), [st] "=&r"(status) : [p] "r"(p), [v] "r"(v) : "memory");
    return status;
}

// A lock-free stack pop: the window loads node->next through the node it loaded (ABA-sensitive under compare-and-swap semantics)
typedef struct node {
    struct node *next;
    uint64_t value;
} node;
static node nodes[16];
static node *head;
static RT_NOINLINE node *pop(node **top) {
    node *n, *next;
    uint32_t status;
    __asm__ volatile(
        "1: ldaxr %[n], [%[top]]\n"
        "   cbz %[n], 2f\n"
        "   ldr %[next], [%[n]]\n"
        "   stlxr %w[st], %[next], [%[top]]\n"
        "   cbnz %w[st], 1b\n"
        "   b 3f\n"
        "2: clrex\n"
        "3:\n"
        : [n] "=&r"(n), [next] "=&r"(next), [st] "=&r"(status)
        : [top] "r"(top)
        : "memory");
    return n;
}
static RT_NOINLINE void push(node **top, node *n) {
    node *old = __atomic_load_n(top, __ATOMIC_RELAXED);
    do {
        n->next = old;
    } while (!__atomic_compare_exchange_n(top, &old, n, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

// A spin lock: test-and-set with acquire, release with stlr
static volatile uint32_t lock_word;
static uint64_t guarded;
static RT_NOINLINE void spin_lock(volatile uint32_t *l) {
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(l, __ATOMIC_RELAXED)) {
        }
    }
}
static RT_NOINLINE void spin_unlock(volatile uint32_t *l) {
    __atomic_store_n(l, 0, __ATOMIC_RELEASE);
}

// ---- Threads ----

#define WORKERS 2
#define WORK_ITERATIONS 100000u
// The register-invariant loop's iterations: exported, so that a runner can lengthen the threaded part (the safepoint test: nxaot_host --set)
volatile uint64_t excl_pause_iterations = 20000000u;
static uint8_t thread_stacks[WORKERS + 1][64 * 1024] __attribute__((aligned(16)));
static rt_thread threads[WORKERS + 1];
static volatile uint32_t go;
static volatile uint64_t mt_add, mt_cas;
static uint64_t mt_pair[2] __attribute__((aligned(16)));
static volatile uint32_t mt_mutex;
static uint64_t mt_locked;  // only under mt_mutex
static volatile uint32_t mt_done;
static volatile uint64_t mt_pause_result;

static void wait_go(void) {
    while (__atomic_load_n(&go, __ATOMIC_ACQUIRE) == 0) {
        rt_svc_wait_for_address(&go, RT_ARB_WAIT_IF_EQUAL, 0, -1);
    }
}

static void worker(rt_thread *self, void *arg) {
    (void)arg;
    wait_go();
    for (uint32_t i = 0; i < WORK_ITERATIONS; ++i) {
        __atomic_fetch_add(&mt_add, 1, __ATOMIC_SEQ_CST);
        uint64_t old = __atomic_load_n(&mt_cas, __ATOMIC_RELAXED);
        while (!__atomic_compare_exchange_n(&mt_cas, &old, old + 1, 1, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        }
        if ((i & 3) == 0) {
            uint64_t lo, hi;
            do {
                lo = __atomic_load_n(&mt_pair[0], __ATOMIC_RELAXED);
                hi = __atomic_load_n(&mt_pair[1], __ATOMIC_RELAXED);
            } while (!cas128(mt_pair, lo, hi, lo + 1, hi + 2));
        }
        if ((i & 15) == 0) {
            rt_mutex_lock(&mt_mutex, self->handle);
            mt_locked += 1;
            rt_mutex_unlock(&mt_mutex, self->handle);
        }
    }
    __atomic_fetch_add(&mt_done, 1, __ATOMIC_RELEASE);
}

static void pauser(rt_thread *self, void *arg) {
    (void)self;
    (void)arg;
    wait_go();
    mt_pause_result = nxaot_pause_loop(excl_pause_iterations) == excl_pause_iterations;
}

static void threaded(void) {
    uint32_t started = 0;
    for (unsigned i = 0; i < WORKERS; ++i) {
        started |= rt_thread_start(&threads[i], worker, 0, thread_stacks[i], sizeof thread_stacks[i]);
    }
    started |= rt_thread_start(&threads[WORKERS], pauser, 0, thread_stacks[WORKERS], sizeof thread_stacks[WORKERS]);
    RT_CHECK32("threads.started", started, 0);
    rt_svc_sleep_thread(1000000);  // 1 ms: the threads reach their wait
    __atomic_store_n(&go, 1, __ATOMIC_RELEASE);
    RT_CHECK32("threads.released", rt_svc_signal_to_address(&go, RT_SIGNAL, 0, -1), 0);
    for (unsigned i = 0; i <= WORKERS; ++i) {
        rt_thread_join(&threads[i]);
    }
    RT_CHECK32("threads.done", mt_done, WORKERS);
    RT_CHECK64("threads.fetch_add", mt_add, (uint64_t)WORKERS * WORK_ITERATIONS);
    RT_CHECK64("threads.cas", mt_cas, (uint64_t)WORKERS * WORK_ITERATIONS);
    rt_check128("threads.pair", mt_pair[0], mt_pair[1], (uint64_t)WORKERS * WORK_ITERATIONS / 4, (uint64_t)WORKERS * WORK_ITERATIONS / 2);
    RT_CHECK64("threads.mutex", mt_locked, (uint64_t)WORKERS * WORK_ITERATIONS / 16);
    RT_CHECK32("threads.mutex_free", mt_mutex, 0);
    RT_CHECK32("threads.pause_loop", mt_pause_result, 1);
}

int main(void) {
    for (unsigned i = 0; i < 1000; ++i) {
        __atomic_fetch_add(&u8v, 3, __ATOMIC_SEQ_CST);
        __atomic_fetch_sub(&u16v, 7, __ATOMIC_ACQ_REL);
        __atomic_fetch_add(&u32v, i, __ATOMIC_RELAXED);
        __atomic_fetch_xor(&u64v, (uint64_t)i * 0x9e3779b97f4a7c15ull, __ATOMIC_SEQ_CST);
    }
    RT_CHECK32("fetch_add.u8", u8v, (uint8_t)3000);
    RT_CHECK32("fetch_sub.u16", u16v, (uint16_t)(0 - 7000));
    RT_CHECK32("fetch_add.u32", u32v, 999 * 1000 / 2);
    uint64_t x = 0;
    for (unsigned i = 0; i < 1000; ++i) {
        x ^= (uint64_t)i * 0x9e3779b97f4a7c15ull;
    }
    RT_CHECK64("fetch_xor.u64", u64v, x);

    __atomic_fetch_or(&u32v, 0xf0000000u, __ATOMIC_SEQ_CST);
    __atomic_fetch_and(&u32v, 0xf00fffffu, __ATOMIC_SEQ_CST);
    RT_CHECK32("fetch_or_and.u32", u32v, (499500u | 0xf0000000u) & 0xf00fffffu);
    RT_CHECK32("exchange.u16", __atomic_exchange_n(&u16v, 0xbeef, __ATOMIC_SEQ_CST), (uint16_t)(0 - 7000));
    RT_CHECK32("exchange.after", u16v, 0xbeef);

    uint64_t expected = 5;
    RT_CHECK32("cas.fail", __atomic_compare_exchange_n(&u64v, &expected, 9, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST), 0);
    RT_CHECK64("cas.fail.observed", expected, x);
    RT_CHECK32("cas.success", __atomic_compare_exchange_n(&u64v, &expected, 9, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST), 1);
    RT_CHECK64("cas.success.stored", u64v, 9);
    uint8_t e8 = 0xb8;
    unsigned weak_tries = 0;
    while (!__atomic_compare_exchange_n(&u8v, &e8, 0x42, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        ++weak_tries;
    }
    RT_CHECK32("cas.weak.u8", u8v, 0x42);
    rt_note("cas.weak.retries", weak_tries < 100 ? 0 : weak_tries, 8);

    pair[0] = 1;
    pair[1] = 2;
    RT_CHECK32("cas128.fail", cas128(pair, 1, 3, 10, 20), 0);
    RT_CHECK32("cas128.success", cas128(pair, 1, 2, 10, 20), 1);
    rt_check128("cas128.value", pair[0], pair[1], 10, 20);
    unsigned n128 = 0;
    for (uint64_t i = 0; i < 500; ++i) {
        n128 += cas128(pair, pair[0], pair[1], pair[0] + i, pair[1] ^ i);
    }
    RT_CHECK32("cas128.loop", n128, 500);
    rt_check128("cas128.loop.value", pair[0], pair[1], 10 + 499 * 500 / 2, 20);  // the XOR of 0..499 is 0

    RT_CHECK32("clrex.store_fails", store_after_clrex(&u64v, 77), 1);
    RT_CHECK64("clrex.memory_kept", u64v, 9);

    for (unsigned i = 0; i < 16; ++i) {
        nodes[i].value = i * i;
        push(&head, &nodes[i]);
    }
    uint64_t popped = 0, order = 0;
    for (node *n; (n = pop(&head));) {
        popped += n->value;
        order = order * 17 + n->value;
    }
    RT_CHECK64("stack.sum", popped, 1240);
    rt_note("stack.order", order, 16);
    RT_CHECK64("stack.empty", (uint64_t)(uintptr_t)pop(&head), 0);

    for (unsigned i = 0; i < 2000; ++i) {
        spin_lock(&lock_word);
        guarded += i;
        spin_unlock(&lock_word);
    }
    RT_CHECK64("spinlock.guarded", guarded, 1999 * 2000 / 2);
    RT_CHECK32("spinlock.free", __atomic_load_n(&lock_word, __ATOMIC_ACQUIRE), 0);

    threaded();
    return rt_finish();
}
