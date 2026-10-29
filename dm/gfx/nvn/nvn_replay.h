// The NVN command replay's decoder, shared by every graphics backend (pure C, every host): walks a command list's records (OP_JUMP within the list, OP_CALL into a sub-list with
// the call return stack below, OP_END), applies the state records to S and R (nvn_state_apply, nvn_state.c) and hands every record to the backend's per-operation functions
// (nvn_be.h) in record order. Moved out of the Metal replay layer (nvn_mtl.m mtl_exec) with no change of logic (docs/design/vulkan-backend.md, sections 1.3 and 1.7, step M3).
// Called on the submit thread only, under hle_nvn.c's submission lock, as before the move
#pragma once
#include <stdint.h>

// Replays the command list at p up to its OP_END (a called sub-list recursively)
void nvn_replay_exec(const uint8_t *p);

// The call return stack: at OP_CALL the record after the CALL is pushed while the sub-list runs, so a backend's look-ahead scan can go on in the caller after the sub-list ends
// (the Metal backend reads it as g_ret / g_ret_n). nvn_replay_ret_n is the call depth; only the first NVN_REPLAY_RET_MAX callers are stored (a deeper call is counted but not
// stored, and a scan stops when nvn_replay_ret_n is past NVN_REPLAY_RET_MAX). Backends only read it
#define NVN_REPLAY_RET_MAX 16
extern const uint8_t *nvn_replay_ret[NVN_REPLAY_RET_MAX];
extern int nvn_replay_ret_n;
