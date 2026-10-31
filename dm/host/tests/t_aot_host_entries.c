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
