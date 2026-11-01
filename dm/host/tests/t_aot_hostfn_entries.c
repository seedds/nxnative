// HLE entries and host functions of t_aot_hostfn.c, the ways a host function's address reaches the host-function module of dm/tools/tests/aot_synth.py (--hostfn): its
// imports bind to these entries' slots (aot_arena.c dm_aot_bind); t_hf_get returns a host function, which its thunk hands over at its guest-visible address (a function
// pointer return: dm/host/tools/gen_hle_thunks.py); t_hf_install writes host functions into guest memory through dm_guest_fnptr, as hle_uemalloc.c fills Unreal's vtable;
// t_hf_spawn makes a guest thread through hle_guestthread.c; the t_re_* entries call the module's code back (nested, from host-created threads, through hle_thread.c's
// system events, leaving from a nested call). The test's CMake rule runs the generator over this file, libc_pass.c, hle_ro.c and hle_guestthread.c into
// gen/t_aot_hostfn_thunks.c, the thunk table the test links
#include "../hle.h"
#include "../aot.h"
#include <pthread.h>
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

// ---------- The host calling guest code back (aot.h dm_gcall, DM_GUEST_CALL: the plain-pointer calls of Apple's text) ----------
// main registers its callback hf_cb(data, a1, ..., a9) with t_re_register, as a game registers NVN's command-memory callback; the entries below call it (and main's other
// functions) back in each context the host's sites have, and note what they saw for t_aot_hostfn.c (g_re_seen)
typedef uint64_t (*t_re_cb_fn)(const uint64_t *data, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9);
static t_re_cb_fn g_re_cb;
static const uint64_t *g_re_data;
struct t_re_seen { int fire_depth, leaf_depth, thread_other, host_fresh, host_ctx, signalled, stop_left, stop_depth, stop_status, stop_resumed; uint64_t stop_r; };
struct t_re_seen g_re_seen;
pthread_mutex_t g_re_stop_mu = PTHREAD_MUTEX_INITIALIZER;   // held by t_re_stop across its guest call: the process stop must leave it released
#define T_RE_CALL(x) DM_GUEST_CALL(g_re_cb, g_re_data, (x), 2, 3, 4, 5, 6, 7, 8, 9)   // ten arguments: the 9th and 10th on the guest stack

HLE("t_re_register", void, t_re_register, void *cb, void *data) { g_re_cb = (t_re_cb_fn)cb; g_re_data = data; }

// Nested re-entry: guest code called this host function, which calls guest code on the same thread before it returns
HLE("t_re_fire", uint64_t, t_re_fire, uint64_t x) {
    g_re_seen.fire_depth = dm_aot_self()->depth;   // 1: inside main's outermost guest call
    return T_RE_CALL(x);
}

// Two levels: main → t_re_deep (host) → hf_deep (guest) → t_re_leaf (host) → synth_triple (guest)
HLE("t_re_leaf", uint64_t, t_re_leaf, uint64_t x) {
    g_re_seen.leaf_depth = dm_aot_self()->depth;   // 2: inside hf_deep, a guest call nested in t_re_deep's host call
    uint64_t (*triple)(uint64_t) = (uint64_t (*)(uint64_t))guest_sym("synth_triple");
    return DM_GUEST_CALL(triple, x) + 100;
}
HLE("t_re_deep", uint64_t, t_re_deep, uint64_t x) {
    uint64_t (*deep)(uint64_t) = (uint64_t (*)(uint64_t))guest_sym("hf_deep");
    return DM_GUEST_CALL(deep, x);
}

// A guest thread the host creates (hle_guestthread.c), as the audio renderer's render thread and the movie player's event thread are: the callback on that thread
static uint64_t g_re_x, g_re_r;
static uint32_t g_re_handle;
static void re_body(void *arg) { (void)arg; g_re_handle = thr_cur_handle(); g_re_r = T_RE_CALL(g_re_x); }
HLE("t_re_thread", uint64_t, t_re_thread, uint64_t x) {
    g_re_x = x;
    void *tt = dm_guest_thread_spawn(re_body, NULL, 44, 0x10000);
    dm_guest_thread_join(tt);
    g_re_seen.thread_other = g_re_handle && g_re_handle != thr_cur_handle();
    return g_re_r;
}

// A host thread that is no guest thread (no state block before its call): the engine gives it a guest context of its own
static void *re_host_body(void *arg) {
    g_re_seen.host_fresh = dm_aot_self() == NULL;
    *(uint64_t *)arg = T_RE_CALL(g_re_x);
    g_re_seen.host_ctx = dm_aot_self() && dm_aot_self()->host && !dm_aot_self()->depth;
    return NULL;
}
HLE("t_re_hostthread", uint64_t, t_re_hostthread, uint64_t x) {
    uint64_t r = 0;
    pthread_t th;
    g_re_x = x;
    if (pthread_create(&th, NULL, re_host_body, &r)) return 0;
    pthread_join(th, NULL);
    return r;
}

// hle_thread.c's system-event path, as the audio and service take-overs reach it: the SDK's CreateSystemEvent and GetWritableHandleOfSystemEvent are main's, called as
// nested guest calls; then the host signals the event through its binding, as the audio thread does
void hle_bind_system_event(void *ev, int autoclear);
uint64_t hle_system_event_binding(void *ev);
int hle_signal_system_event_binding(uint64_t binding);
HLE("t_re_bind", uint64_t, t_re_bind, void *ev) {
    hle_bind_system_event(ev, 1);
    const uint64_t b = hle_system_event_binding(ev);
    g_re_seen.signalled = hle_signal_system_event_binding(b);
    return b;
}

// Leaving guest code from a nested call: hf_exit's svcExitProcess stops the process inside the guest call this host function makes while it holds a lock; the guest call
// returns 0 here (the innermost level), this function runs to its end and releases the lock, and its own host call leaves next (aot_run.c host_call)
HLE("t_re_stop", void, t_re_stop, void) {
    pthread_mutex_lock(&g_re_stop_mu);
    g_re_seen.stop_r = dm_gcall(guest_sym("hf_exit"), 0);
    g_re_seen.stop_left = dm_aot_self()->left; g_re_seen.stop_depth = dm_aot_self()->depth; g_re_seen.stop_status = dm_aot_stop_status();
    g_re_seen.stop_resumed = 1;
    pthread_mutex_unlock(&g_re_stop_mu);
}
