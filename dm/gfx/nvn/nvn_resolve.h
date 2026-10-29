// Handle resolution of the NVN replay, shared by every graphics backend (pure C, every host): the handle a texture binding reads, what the NVN driver's constant segments c0 and c2
// hold, handles and addresses read from uniform memory as the GPU will read them (with the inline updates queued in this submission), the IDs such a handle names in the descriptor
// pools bound, and the handles of deferred binds read again. Moved out of the Metal replay layer (nvn_mtl.m: tex_slot_h, c2_handle, c0_ssbo_base, the driver segment fill of encode_args,
// ubo_read, ubo_handle, ubo_tex_id, ubo_samp_id, bh_fresh) with no change of logic (docs/design/vulkan-backend.md, sections 1.3 and 1.7, step M2b); nvn_mtl.m keeps the old names as
// aliases. Called on the submit thread only, under hle_nvn.c's submission lock, as before the move
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "nvn_obj.h"
#include "nvn_state.h"   // S (the bindings read here), NTEX

// The NVN driver's constant segments, one layout in every game (dm/gfx/tools/dmsh_build.py), byte offsets: c2 holds the combined handles at 0x20 + 8 r (41 slots up to the separate table:
// past slot 31 they lie in the image table, 0x120 + 8 r, 8 slots), the separate textures at 0x168 + 8 r and the separate samplers at 0x568 + 8 r (128 each);
// c0 the storage buffer table at base + 0x10 r (address 64 bits, size 32 bits), base vertex 0x110, compute 0x310, geometry 0x410, pixel 0x510. The replay fills one DRV_SIZE buffer per segment a shader reads
enum { C2_TEX = 0x20, C2_TEX_SLOTS = 41, C2_IMG = 0x120, C2_IMG_SLOTS = 8, C2_SEPTEX = 0x168, C2_SAMP = 0x568, DRV_SIZE = 0xA00 };
_Static_assert(C2_TEX + 8 * C2_TEX_SLOTS == C2_SEPTEX && C2_SEPTEX + 8 * 128 == C2_SAMP && C2_SAMP + 8 * 128 <= DRV_SIZE && NVN_SEPTEX_SLOT0 + 128 == NTEX, "NVN driver handle tables");

// The handle a texture binding of slot reg reads: as c2 holds it (nvn_fill_driver_bank fills c2 the same way), a combined slot past 31 shares its word with an image slot, and a bound image
// is what lies there. A texture instruction on an image handle names such a slot: a size query of the image a compute shader writes (Unreal's GetDimensions on a RWTexture3D,
// dmsh_build.py decode_tex). DRAGON QUEST VII Reimagined's volumetric lightmap indirection patch read the empty texture slot 32 there, got a 1x1 size and wrote only cell (0, 0, 0)
static inline uint64_t nvn_tex_slot_handle(const uint64_t *tex, const uint64_t *img, unsigned reg) {
    return reg >= 32 && reg - 32 < C2_IMG_SLOTS && img[reg - 32] ? img[reg - 32] : reg < NTEX ? tex[reg] : 0;
}
// The handle c2 holds at byte b, as nvn_fill_driver_bank fills the table: a combined slot (past slot 31 the image there, nvn_tex_slot_handle), a separate texture, else none
uint64_t nvn_c2_handle(int stage, unsigned b);
// The NVN driver constant segment bank (0: c0, 2: c2; any other value is taken as 2) of stage as a shader of it reads the segment, into d: all DRV_SIZE bytes are written, from S and the
// arguments (dyn: the shader has dynamic handle arrays, nvn_refl.h; base_instance: the draw's, c0 + 0x644; dispatch_dim: the dispatch's workgroup counts, c0 + 0x200 for compute).
// 0 when bank is 0 and the storage buffer table base of the stage was never measured (that table is left 0, the rest is written; the caller reports it), else 1
int nvn_fill_driver_bank(int stage, int bank, uint8_t *d, int dyn, uint32_t base_instance, const uint32_t dispatch_dim[3]);

// Bindless handle (TEXU / SAMPU): the 32-bit word at byte offset off of uniform slot reg = the low 32 bits of the NVN handle the game wrote there (low 20 bits texture ID, high 12 bits sampler ID).
// Read on the CPU side at replay time (the memory at the same moment a real GPU would read it at execution); slot unbound / out of range / unreadable = 0 (empty slot: dummy texture / dummy sampler).
// A location with a GPU-side write queued in this submit (nvn_gw_pend_hits) is still read at its current value; the first case is reported red.
// The same read serves storage buffer addresses kept in uniform memory (SBUFU: 8 bytes)
void nvn_ubo_read(int stage, int reg, int off, size_t n, void *out);
uint32_t nvn_ubo_handle(int stage, int reg, int off);
// A handle read from a uniform buffer names IDs of the pools bound when the draw runs: as bound handles are moved at recording (hle_nvn.c bind_handles), these take the bases here (0 = none)
int nvn_tid_from_handle(uint32_t h);
int nvn_sid_from_handle(uint32_t h);

// Handles of deferred binds (BindTexturesDeferred) are read at execution: the record keeps the address of the handles (see hle_nvn.c); every reader of a bind record refreshes it first. Command memory is writable, so it is updated in place
static inline void nvn_bind_fresh(const p_bind_handles *b) {
    if (b->defer && b->src && b->n > 0 && b->n <= 32) memcpy((void *)b->h, (const void *)(uintptr_t)b->src, 8 * (size_t)b->n);
}

// ---------- Hooks the backend defines (nvn_mtl.m), for the parts of nvn_ubo_read that live in the backend ----------
// The n bytes at a as the backend's own copies of inline updates hold them (Metal: the updates deferred in this pass, ubd; a partial shadow of the updates queued over memory, ubp): 1 when it
// read them into out, else 0
int nvn_resolve_hook_ubo_copy(uint64_t a, size_t n, void *out);
// The bytes read have GPU-side writes queued in this submission that the ledger cannot give (kind as nvn_gw_pend_read gives it): reported, and the current value is read
void nvn_resolve_hook_ubo_stale(int stage, int reg, int off, int kind);
// The n bytes of guest memory at a, read through the backend's pool lookup (Metal: mtl_addr, which also makes the pool resident): out is left as it is when a is in no pool
void nvn_resolve_hook_mem_read(uint64_t a, size_t n, void *out);
