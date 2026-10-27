// Synthetic ARM replay of a mutex unlock + LightEvent fast paths. No game data.
// "original" measures allowed lost wakes; "release" must observe none. The
// original may observe none on a particular run, so it is not a CI failure.
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>

typedef struct { _Alignas(128) uint32_t mutex; uint8_t pad[3], event; } words;
static words data;
static _Alignas(128) atomic_uint phase, done;
static unsigned seen_mutex, iterations;
static int release_store;

static void *unlocker(void *unused) {
    (void)unused;
    for (unsigned i = 1; i <= iterations; i++) {
        while (atomic_load_explicit(&phase, memory_order_acquire) != i) {}
        if (release_store == 2) {
            __asm__ volatile(
                "stlur wzr, [%0, #-4]\n"
                "ldarb w8, [%1]\n"
                "cmp w8, #1\n"
                "b.eq 1f\n"
                "mov w8, #1\n"
                "stlrb w8, [%1]\n"
                "1: dmb ish\n"
                : : "r"((uint8_t *)&data.mutex + 4), "r"(&data.event) : "x8", "memory", "cc");
        } else if (release_store) {
            __asm__ volatile(
                "stlr wzr, [%0]\n"
                "ldarb w8, [%1]\n"
                "cmp w8, #1\n"
                "b.eq 1f\n"
                "mov w8, #1\n"
                "stlrb w8, [%1]\n"
                "1: dmb ish\n"
                : : "r"(&data.mutex), "r"(&data.event) : "x8", "memory", "cc");
        } else {
            __asm__ volatile(
                "str wzr, [%0]\n"
                "ldarb w8, [%1]\n"
                "cmp w8, #1\n"
                "b.eq 1f\n"
                "mov w8, #1\n"
                "stlrb w8, [%1]\n"
                "1: dmb ish\n"
                : : "r"(&data.mutex), "r"(&data.event) : "x8", "memory", "cc");
        }
        atomic_fetch_add_explicit(&done, 1, memory_order_release);
    }
    return NULL;
}

static void *waiter(void *unused) {
    (void)unused;
    for (unsigned i = 1; i <= iterations; i++) {
        while (atomic_load_explicit(&phase, memory_order_acquire) != i) {}
        unsigned observed;
        __asm__ volatile(
            "mov w9, #255\n"
            "1: ldaxrb w8, [%1]\n"
            "cmp w8, #1\n"
            "b.ne 2f\n"
            "stlxrb w10, w9, [%1]\n"
            "cbnz w10, 1b\n"
            "dmb ish\n"
            "ldxr %w0, [%2]\n"
            "clrex\n"
            "b 3f\n"
            "2: clrex\n"
            "mov %w0, #0\n"
            "3:\n"
            : "=&r"(observed) : "r"(&data.event), "r"(&data.mutex)
            : "x8", "x9", "x10", "memory", "cc");
        seen_mutex = observed;
        atomic_fetch_add_explicit(&done, 1, memory_order_release);
    }
    return NULL;
}

int main(int argc, char **argv) {
    release_store = argc > 1 && !strcmp(argv[1], "unscaled") ? 2 : argc > 1 && !strcmp(argv[1], "release");
    iterations = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 2000000;
    pthread_t u, w;
    pthread_create(&u, NULL, unlocker, NULL);
    pthread_create(&w, NULL, waiter, NULL);
    unsigned lost = 0;
    for (unsigned i = 1; i <= iterations; i++) {
        data.mutex = 1; data.event = 1; seen_mutex = 0;
        atomic_store_explicit(&done, 0, memory_order_relaxed);
        atomic_store_explicit(&phase, i, memory_order_release);
        while (atomic_load_explicit(&done, memory_order_acquire) != 2) {}
        if (seen_mutex == 1 && data.mutex == 0 && data.event == 255) lost++;
    }
    pthread_join(u, NULL); pthread_join(w, NULL);
    printf("%s: %u lost-wake outcomes / %u iterations\n", release_store == 2 ? "STLUR" : release_store ? "STLR" : "STR", lost, iterations);
    return release_store && lost != 0;
}
