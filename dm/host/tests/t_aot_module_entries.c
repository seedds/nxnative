// HLE entries of t_aot_module.c: the host function the synthetic game module (dm/tools/tests/aot_synth.py) imports by name, as a game imports the host's. The loader binds
// the import into the module's GOT at the entry's guest-visible slot (aot_arena.c dm_aot_bind); the test's CMake rule runs dm/host/tools/gen_hle_thunks.py over this file
// into gen/t_aot_module_thunks.c, the thunk table the test links. The module's other import, t_aot_mod_missing, has no entry: it binds to a named stub
#include "../hle.h"

_Atomic uint64_t g_t_aot_mod_calls;

// The module calls it with (7, 3) through its GOT and stores what it returns (results[0], 7003)
HLE("t_aot_mod_combine", uint64_t, t_aot_mod_combine, uint64_t a, uint64_t b) {
    g_t_aot_mod_calls++;
    return a * 1000 + b;
}
