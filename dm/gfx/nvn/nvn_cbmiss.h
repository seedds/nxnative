// A uniform (constant) buffer a shader declares that the replay cannot give it (pure; unit test tests/t_cbmiss.c). nvn_mtl.m cbuf_slot binds the all-zero bank in its place in
// every case, so the shader reads 0; what differs is whether that is the game's own state or a binding the host lost:
//   CB_UNBOUND  the game left the slot unbound (address 0) on this queue: its own state, the host missed no bind (docs/reference/rendering.md, "An unbound uniform slot reads
//               the zero bank"). A plain log line for the first 8 shaders, no [RED]
//   CB_LOST     the game bound an address that is in no memory pool the host knows: the shader reads 0 where the game bound data. [RED], with the shader and the address
//   CB_SLOT     a slot past the 16 the replay keeps (a reflection the host cannot place). [RED]
#pragma once
#include <stdint.h>

enum { CB_BOUND = 0, CB_UNBOUND = 1, CB_LOST = 2, CB_SLOT = 3 };

static inline int nvn_cb_state(int reg, uint64_t addr, int found) { return reg < 0 || reg >= 16 ? CB_SLOT : !addr ? CB_UNBOUND : found ? CB_BOUND : CB_LOST; }
static inline int nvn_cb_red(int state) { return state == CB_LOST || state == CB_SLOT; }
