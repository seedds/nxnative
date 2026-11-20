// Actual per-thread gate stacks for component fixtures linking entry.S.
#pragma once
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
pthread_key_t g_keygate;
extern unsigned char dm_gate_stubs[];
extern uintptr_t dm_gate_top[];
static pthread_once_t fixture_gate_once = PTHREAD_ONCE_INIT;
static _Atomic unsigned fixture_gate_next;
static void fixture_gate_destroy(void *value) {
    unsigned slot = (unsigned)((unsigned char *)value - dm_gate_stubs) / 16;
    munmap((void *)(dm_gate_top[slot] - (1 << 20)), 1 << 20); dm_gate_top[slot] = 0;
}
static void fixture_gate_initialize(void) { assert(!pthread_key_create(&g_keygate, fixture_gate_destroy)); }
static void fixture_gate_prepare(void) {
    pthread_once(&fixture_gate_once, fixture_gate_initialize);
    if (pthread_getspecific(g_keygate)) return;
    unsigned slot = atomic_fetch_add(&fixture_gate_next, 1); assert(slot < 1024);
    void *memory = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); assert(memory != MAP_FAILED);
    dm_gate_top[slot] = (uintptr_t)memory + (1 << 20);
    assert(!pthread_setspecific(g_keygate, dm_gate_stubs + slot * 16));
}
void dm_gate_nested(uint64_t sp) { (void)sp; abort(); }
void dm_applet_enter(const char *name) { (void)name; }
void dm_applet_leave(void) {}
