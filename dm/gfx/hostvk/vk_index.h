// The Vulkan backend's index conversions (vk_index.c; docs/design/vulkan-backend.md 2.7, 4.8): what a draw's indices become where Vulkan cannot draw NVN's primitive as it is,
// by the rules table of vk_index.c, converted on the GPU (shaders/index_expand.comp, shaders/index_restart.comp) into a 32-bit list in the ring of the open submission; for a
// draw whose count the GPU holds (indirect, multi-draw, DrawTransformFeedback), by index_restart.comp from the game's record, into the submission's arena
#pragma once
#include "vk_backend.h"

// A draw's indices: its NVN primitive, the index size (0: a draw without indices, whose implicit indices are 0 .. n-1 and whose first vertex becomes the vertex offset), their guest
// address, the count, primitive restart (indexed draws only) and its index, and for patches the patch size (a patch list with restart drops the patch a restart cuts short)
typedef struct { int prim, isz; uint64_t src; uint32_t n; int restart; uint32_t ri, patch; } hvk_ix_in;
// The forms (vk_index.c's table): the game's indices or vertices as they are; a draw without indices over the backend's shared quad list; the indices widened to 32 bits as they
// are (8-bit ones without the device's support, a restart index other than all ones); a list expanded without restart (the count known); a list expanded with restart (the
// count is the GPU's: the draw reads an indexed indirect record)
enum { HVK_IX_NATIVE, HVK_IX_SHARED, HVK_IX_MAP, HVK_IX_EXPAND, HVK_IX_RESTART };
typedef struct {
    int form, mode;              // mode: the kernels' (0 widen, 1 quads, 2 quad strips, 3 fans and polygons, 4 line loops, 5 lists)
    VkPrimitiveTopology topo;    // the topology the draw is made with
    int restart;                 // primitive restart enable for it: an indexed strip or fan, or list where the device restarts lists, of the game's or widened indices
    uint32_t nout;               // the indices drawn (MAP, EXPAND, SHARED), or at most (RESTART: a segment of the whole stream draws the most)
    uint32_t group;              // a list's vertices per primitive (mode 5: 1 points, 2 lines, 3 triangles, 4 and 6 with adjacency, the patch size)
} hvk_ix_plan;
// The converted list: in the ring, with its count (direct) or the records the GPU wrote (indirect: count, instances, first index, vertex offset, base instance; records of them,
// 20 bytes apart; a draw whose count the GPU holds over the shared quad list leaves buf to the caller)
typedef struct { VkBuffer buf; VkDeviceSize off; uint32_t count; int indirect; VkBuffer abuf; VkDeviceSize aoff; uint32_t records; } hvk_ix_out;
// A draw whose count the GPU holds: kind 0 DrawArraysIndirect (records {count, instances, first, base instance}), 1 DrawElementsIndirect ({count, instances, first index, base
// vertex, base instance}), 2 DrawTransformFeedback (rec: the control buffer's byte offset of the buffer it counts by, divided by stride, the capture's vertex size; instances and
// base instance the draw's own); a multi-draw's records, stride bytes apart, and its count (cnt; record first + i is drawn while first + i < the u32 there). hvk_ix_ind_prepare
// fills the rest: the guest bytes the conversion reads (the records or the counter, the count, the indices from the index buffer to its pool's end), for the caller's placement
typedef struct {
    int kind; uint64_t rec; uint32_t records, stride, first; uint64_t cnt; uint32_t instances, base_instance;
    uint64_t rd_lo, rd_hi, ib_bytes; VkDeviceAddress drec, dcnt, dsrc; uint32_t avail;
} hvk_ix_ind;

// The form of a draw (native: its topology without a conversion). Logs once per device what it draws natively
void hvk_ix_plan_make(const hvk_ix_in *in, VkPrimitiveTopology native, hvk_ix_plan *pl);
// Whether the GPU converts it: 1, with the source's device address in *src (0 for a draw without indices); 0, the CPU does (past the GPU path's limit while the CPU can read the
// indices, no device address for them; counted). readable: nothing of this submission or of one still running writes the indices before the draw (vk_commands.c cpu_readable)
int hvk_ix_gpu_takes(const hvk_ix_in *in, const hvk_ix_plan *pl, int readable, VkDeviceAddress *src);
// The GPU conversion recorded into cb (the prologue, or the main command buffer outside a pass after a barrier), its list and record allocated in the ring; instances, vertex
// offset and base instance go into a RESTART form's record. 1 done, 0 failed (logged)
int hvk_ix_gpu(VkCommandBuffer cb, const hvk_ix_in *in, const hvk_ix_plan *pl, VkDeviceAddress src, uint32_t instances, int32_t vertex_offset, uint32_t base_instance, hvk_ix_out *out);
// A draw whose count the GPU holds: what its conversion reads, resolved (1), or 0 when the GPU cannot address it or the device has no buffer device address (said once: the draw
// is skipped). Then its conversion recorded into cb (the prologue when nothing of this submission or of one still running writes what it reads, pre; else the main command buffer
// outside the pass after a barrier): the records written in the ring, the lists in the submission's arena (allocated at its first such draw); counted. 1 done, 0 failed (logged)
int hvk_ix_ind_prepare(const hvk_ix_in *in, const hvk_ix_plan *pl, hvk_ix_ind *ii);
int hvk_ix_ind_gpu(VkCommandBuffer cb, int pre, const hvk_ix_in *in, const hvk_ix_plan *pl, const hvk_ix_ind *ii, hvk_ix_out *out);
// The CPU conversion into the ring, the same list the kernels write (quads and fans through nvn_primitive.h, the Metal backend's own expansion). 1 done, 0 failed
int hvk_ix_cpu(const hvk_ix_in *in, const hvk_ix_plan *pl, hvk_ix_out *out);
// A conversion done (gpu: on the GPU; pre: in the prologue): counted with the submit thread's time it took, for the shutdown line and the resource line
void hvk_ix_count(int gpu, int pre, const hvk_ix_in *in, const hvk_ix_plan *pl, uint64_t ns);
void hvk_ix_shutdown(void);   // the shutdown line "[hostvk] index conversions: ..."; the device's answers are asked again on the next device
