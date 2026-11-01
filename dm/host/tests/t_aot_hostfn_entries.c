// HLE entries and host functions of t_aot_hostfn.c, the ways a host function's address reaches the host-function module of dm/tools/tests/aot_synth.py (--hostfn): its
// imports bind to these entries' slots (aot_arena.c dm_aot_bind); t_hf_get returns a host function, which its thunk hands over at its guest-visible address (a function
// pointer return: dm/host/tools/gen_hle_thunks.py); t_hf_install writes host functions into guest memory through dm_guest_fnptr, as hle_uemalloc.c fills Unreal's vtable;
// t_hf_spawn makes a guest thread through hle_guestthread.c. The test's CMake rule runs the generator over this file, libc_pass.c, hle_ro.c and hle_guestthread.c into
// gen/t_aot_hostfn_thunks.c, the thunk table the test links
#include "../hle.h"
#include "../aot.h"
#include <stdatomic.h>

_Atomic uint64_t g_t_hf_calls[3];   // t_hf_mul, t_hf_add, the spawned thread's body

// Host functions the guest reaches only through a pointer the host hands it: HLE_LINKAGE, so each gets a host-call thunk (hle.h)
HLE_LINKAGE uint64_t t_hf_mul(uint64_t a, uint64_t b) { g_t_hf_calls[0]++; return a * b + 1000; }
HLE_LINKAGE uint64_t t_hf_add(uint64_t a, uint64_t b) { g_t_hf_calls[1]++; return a + b; }
// A host function with no thunk (static, so no generator rule covers it): dm_guest_fnptr gives the guest a stub, with a [RED] line
static uint64_t t_hf_nothunk(uint64_t a) { return a + 1; }

typedef uint64_t (*t_hf_fn)(uint64_t, uint64_t);   // a pointer to a function: an entry returning one hands the guest a guest-visible address

// The module calls it through its GOT and calls what it returns, then stores it in a table and calls it from there
HLE("t_hf_get", t_hf_fn, t_hf_get, void) { return t_hf_mul; }

// The module hands over its table hf_vt2: [0] a host function with a thunk, [1] one without
HLE("t_hf_install", void, t_hf_install, void **vt) {
    vt[0] = dm_guest_fnptr((const void *)t_hf_add);
    vt[1] = dm_guest_fnptr((const void *)t_hf_nothunk);
}

// A host-created guest thread (hle_guestthread.c, as the movie player and the audio renderer make theirs): its body runs on the new guest thread inside the host call of the
// SDK's thread entry, and calls the module's synth_triple there (a guest call on that thread). Returns triple(x) when the body ran on another guest thread than the caller's
void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack_size);
void dm_guest_thread_join(void *tt);
uint32_t thr_cur_handle(void);
static struct { uint64_t x, r; uint32_t handle; } g_body;
static void body(void *arg) {
    (void)arg;
    g_t_hf_calls[2]++;
    g_body.handle = thr_cur_handle();
    g_body.r = dm_guest_call(dm_aot_self_state(), (uint64_t)(uintptr_t)guest_sym("synth_triple"), 1, g_body.x);
}
HLE("t_hf_spawn", uint64_t, t_hf_spawn, uint64_t x) {
    g_body.x = x;
    void *tt = dm_guest_thread_spawn(body, NULL, 44, 0x10000);
    dm_guest_thread_join(tt);
    const uint32_t mine = thr_cur_handle();
    LOG("t_aot_hostfn: the host-created guest thread %#x (the caller %#x) computed %llu", g_body.handle, mine, (unsigned long long)g_body.r);
    return g_body.handle && g_body.handle != mine ? g_body.r : 0;
}
