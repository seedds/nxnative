// HLE entries of t_aot_host.c: host functions that recompiled guest code reaches through the host-function page (docs/design/aot-recompiler.md 2.4), as a game
// reaches the host's. The test's CMake rule runs dm/host/tools/gen_hle_thunks.py over this file into gen/t_aot_host_thunks.c, the thunk table the test links
#include "../hle.h"

_Atomic uint64_t g_t_aot_hidden_calls;

// The fnptr program's hidden_target in the host (dm/tools/aot/corpus/src/fnptr.c: x ^ 0x5a5a5a5a5a5a5a5a): with hidden_ptr set to this entry's slot, the program's
// check "pointer.hidden" passes only when its blr went through the dispatcher into the host-function page, the thunk and back
HLE("t_aot_hidden", uint64_t, t_aot_hidden, uint64_t x) {
    g_t_aot_hidden_calls++;
    return x ^ 0x5a5a5a5a5a5a5a5aull;
}

// The pause test's thread: a guest thread whose entry is this host function, as a garbage collector's thread runs host code on the guest's thread layer
// (t_aot_host.c t_aot_pause_body: SetThreadActivity and GetThreadContext3 on the program's threads)
void t_aot_pause_body(uint64_t arg);
HLE("t_aot_pause_tester", void, t_aot_pause_tester, uint64_t arg) { t_aot_pause_body(arg); }

// The pollkeep test's request (t_aot_host.c pollkeep_nro): the calling thread's poll byte set, as SetThreadActivity's pause request sets another thread's
// (hle_thread.c), so that the next poll of compiled code takes its cold path (aot_run.c safepoint clears it again). Returns its argument
void t_aot_poll_request(void);   // t_aot_host.c
HLE("t_aot_poll_self", uint64_t, t_aot_poll_self, uint64_t x) {
    t_aot_poll_request();
    return x;
}

// The fzsec test's host function (t_aot_host.c fzsec_nro), called from a flush-to-zero section of compiled code: float work in C, whose results need the host's MXCSR
// (FTZ and DAZ clear: aot_run.c, the MXCSR invariant): 2^-126 * 0.5 is the denormal 2^-127 (0 under FTZ), 2^-127 * 2 is 2^-126 (0 under DAZ). Returns
// 0x0040000000800000 + x
HLE("t_aot_fz_host", uint64_t, t_aot_fz_host, uint64_t x) {
    volatile float a = 0x1p-126f, half = 0.5f, d = 0x1p-127f, two = 2.0f;
    const float p = a * half, q = d * two;
    uint32_t pb, qb;
    memcpy(&pb, &p, 4);
    memcpy(&qb, &q, 4);
    return ((uint64_t)pb << 32 | qb) + x;
}
