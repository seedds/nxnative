// The Vulkan backend's index conversions (docs/design/vulkan-backend.md 2.7, 4.8; step "Index conversions", 2026-11-02): what a direct draw's indices become where Vulkan cannot
// draw NVN's primitive as it is. Every conversion runs on the GPU: shaders/index_expand.comp (no restart: one thread per index drawn) and shaders/index_restart.comp (with restart:
// one workgroup, a scan over its threads' chunks, the count into an indexed indirect record), both writing a 32-bit list into the ring of the open submission. vk_commands.c
// records them into the submission's prologue when nothing of this submission (or of one still running) writes the indices before the draw, so a render pass is not split for
// them and they run together before it; when the GPU writes them earlier in the submission, before the draw's pass at its place. The CPU converts only past the GPU path's limit
// (HVK_IX_LIMIT: 16 MiB of converted indices, 4,194,304, for one draw) when it can read the indices, or without a device address for them; counted, not red: a limit.
//
// The rules (a segment is the run of indices between two restart indices, or the whole stream; k counts a segment's primitives from 0 and n is its length). Primitive restart
// applies to indexed draws only; the restart index is compared with the index as the buffer holds it, before the base vertex is added (nvn_mtl.m:8670); it ends the segment, the
// vertices of a primitive it cuts short are dropped and the next index starts a new primitive (nvn_primitive.h:13 `have = 0`; nvn_geometry.h:29-31 splits the runs the same way).
//   NVN primitive    drawn on Vulkan as               primitive k of a segment (vertex order: the winding)                            dropped vertices   from
//   points           point list                       (k)                                                                             -                  GL
//   lines            line list                        (2k, 2k+1)                                                                      n mod 2            nvn_geometry.h:51
//   line loop        line list (converted)            (k, k+1) for k < n-1, then (n-1, 0): n lines when n >= 2                        n < 2              nvn_geometry.h:36, 52
//   line strip       line strip                       (k, k+1)                                                                        n < 2              nvn_geometry.h:53
//   triangles        triangle list                    (3k, 3k+1, 3k+2)                                                                n mod 3            nvn_geometry.h:54
//   triangle strip   triangle strip                   (k, k+1, k+2), odd k (k+1, k, k+2)                                              n < 3              nvn_geometry.h:55
//   triangle fan     triangle fan, or list            (0, k+1, k+2)                                                                   n < 3              nvn_primitive.h:14-18
//   quads            triangle list (converted)        quad j: (4j, 4j+1, 4j+2) (4j, 4j+2, 4j+3)                                       n mod 4            nvn_primitive.h:19-25
//   quad strip       triangle list (converted)        quad j = (2j, 2j+1, 2j+3, 2j+2) split as quads: (2j, 2j+1, 2j+3) (2j, 2j+3, 2j+2)  an odd last; n < 4  GL, the quad rule
//   polygon          triangle fan, or list            as the fan: (0, k+1, k+2)                                                       n < 3              GL, the fan rule
//   with adjacency   their list                       groups of 4 and 6 (patches: of the patch size)                                  n mod the group    nvn_geometry.h:41, 43
// A list, strip or fan is drawn from the game's indices where Vulkan can (restart at the all-ones index for a strip or fan, and for a list where the device restarts lists); a
// list with restart the device does not restart is converted into a list without (index_restart.comp); one with another restart index, or 8-bit indices without the device's
// support, is widened to 32 bits with the restart index made all ones (index_expand.comp). A draw without indices draws the implicit 0 .. n-1 by the same rules (restart does not apply), with first as the vertex offset: quads
// over the backend's shared list (0 1 2) (0 2 3) per quad, as the Metal backend's g_quad_ib (nvn_mtl.m:6298-6303, 8649-8651); fans (0, k+1, k+2) as its g_fan_ib (nvn_mtl.m:6304-6309,
// 8652-8654). The Metal backend draws points, lines, line strips, triangles and triangle strips natively, expands fans and quads (nvn_mtl.m:8561) and skips line loops, quad
// strips and polygons with a red line (nvn_mtl.m:8562-8564): their rows follow GL (OpenGL 4.6 compatibility profile 10.1: a line loop closes from its last vertex to its first, a
// quad strip's quad j is (2j, 2j+1, 2j+3, 2j+2) and an odd last vertex is ignored, a polygon is the convex polygon of its vertices), drawn by the Metal backend's quad and fan rules.
// Two differences from what the Metal backend draws, both its limits: 8-bit indices in a strip restart at 0xff here (it widens them to 16 bits, and 0x00ff does not restart:
// nvn_mtl.m:8680), and a list with restart drops the primitive a restart cuts short here, NVN's rule (it hands a list's indices to Metal as they are: nvn_mtl.m:8700).
// What the device draws natively, said once per device: triangle fans and polygons as VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN unless the device lists VK_KHR_portability_subset (whose
// triangleFans may be off: then they are expanded); 8-bit indices with VK_KHR_index_type_uint8; restart on point, line and triangle lists and their adjacency forms with
// VK_EXT_primitive_topology_list_restart's primitiveTopologyListRestart (vk_device.c enables it; patch lists stay converted). The extension lets primitiveRestartEnable apply to
// lists, where the restart index "restarts the assembly of primitives" (VkPipelineInputAssemblyStateCreateInfo): the vertices gathered for the primitive in progress make none,
// and the next index starts a new one, the dropping rule above; t_vk_index_convert draws both paths against the same oracle. Native fans keep the triangles, their order and
// their winding; Vulkan's vertex order within a triangle is (k+1, k+2, 0).
// The provoking vertex (2026-11-02; t_vk_provoking): the vertex a flat varying takes. NVN's is GL's last-vertex convention (OpenGL 4.6 compatibility profile, table 13.2, the
// "last" column; the frontend records no other: nvnCommandBufferSetProvokingVertexMode is unimplemented), and every pipeline of the backend is made with Vulkan's last-vertex
// mode where the device has VK_EXT_provoking_vertex (vk_device.c, vk_pipeline.c hvk_raster_next). Per row, primitive k of a segment (j: the quad), GL's vertex / this port's:
//   points k / k; lines 2k+1 / 2k+1; line loop k+1, the closing line 0 / the same (the converted list's second vertex); line strip k+1 / k+1; triangles 3k+2 / 3k+2;
//   triangle strip k+2 / k+2; triangle fan k+2 / k+2 (Vulkan's last-vertex mode takes k+2 of a native fan, VK_EXT_provoking_vertex; the expanded list (0, k+1, k+2) ends with it);
//   lists and strips with adjacency 4k+2, k+2, 6k+4, 2k+4 / the same (Vulkan's last-vertex mode skips the adjacent vertices as GL does).
// Three rows differ, declared (the conversions keep the Metal backend's triangles; changing them is the shader kernels' and the shared quad list's, not this table's):
//   quads: GL's quad takes 4j+3 for both its triangles; the split (4j, 4j+1, 4j+2) (4j, 4j+2, 4j+3) gives 4j+2 to the first, and no rotation of a triangle without vertex 4j+3
//     can give it (the split along (4j+1, 4j+3): (4j, 4j+1, 4j+3) (4j+1, 4j+2, 4j+3) would, with the other diagonal);
//   quad strips: GL takes 2j+3; the second triangle (2j, 2j+3, 2j+2) gives 2j+2 (its rotation (2j+2, 2j, 2j+3), the same winding, would give 2j+3);
//   polygons: GL takes vertex 0 in both conventions; the fan gives k+2 (a list (k+1, k+2, 0), the same winding, would give 0).
// Without the extension (a red line once per device) Vulkan's first-vertex mode is left: lists 3k, strips k, native fans k+1, expanded fans and converted quads 0 / 4j.
// Draws whose count the GPU holds (2026-11-02): DrawArraysIndirect and DrawElementsIndirect, their indirect-count multi-draws, and DrawTransformFeedback, whose records (or the
// capture's byte count) a compute shader or the CPU writes, are converted by the same rules on the GPU from the record: shaders/index_restart.comp, one workgroup a record, reads
// the count, instances, first index, base vertex (an array draw's first vertex) and base instance there (DrawTransformFeedback: the control buffer's byte offset over the
// capture's vertex size, as vkCmdDrawIndirectByteCountEXT counts), converts that stream as a direct draw's (the forms of the plan: quads, quad strips, line loops, fans without
// native fans, lists with restart the device does not restart, indices widened; quads without indices index the shared quad list and only the record is written), and writes
// an indexed indirect record that the draw reads with vkCmdDrawIndexedIndirect (a multi-draw's records in one dispatch and one call: a record at or past the u32 count draws
// nothing). Placed as a direct conversion: in the prologue when nothing of the submission or of one still running writes the records, the count, the counter or the indices
// from the index buffer to its pool's end (vk_commands.c cpu_readable), else before the draw's pass. The count is unknown when the draw is recorded, so the lists of the
// submission's such draws share one arena in the ring (HVK_IX_ARENA, allocated at the first of them) from which each takes, on the GPU, exactly what its record asks (a
// compare-and-swap on the arena's header): no per-draw reservation from a bound the CPU cannot know. What the GPU cannot do is cut there and counted in statistics words the
// CPU reads (a [RED] line once each, the shutdown line): a list past what is left of the arena (cut to it), a draw reading more than HVK_IX_IND_IN indices or drawing more quads
// than the shared list holds (cut there), indices past the end of the index buffer's memory pool (the count stops there; the GPU reads nothing outside the pool).
#include "plat/plat.h"
#include "vk_index.h"
#include "../nvn/nvn_primitive.h"
#include "../nvn/nvn_pools.h"
#include <stdlib.h>
#include <string.h>

enum { M_MAP, M_QUADS, M_QSTRIP, M_FAN, M_LOOP, M_LIST };
enum { QUAD_SHARED = 65536 };   // quads the shared list holds (vk_commands.c g_quad_ib)
#define HVK_IX_LIMIT (16ull << 20)   // the bytes of one draw's list the GPU path takes from the ring
#define HVK_IX_ARENA (3u << 20)      // the bytes of a submission's arena for the lists of draws whose count the GPU holds (786,432 indices; a ring chunk is 4 MiB)
#define HVK_IX_IND_IN (1u << 22)     // the indices one such draw reads at most (4,194,304, the direct path's GPU limit in indices): its one workgroup's work stays bounded

static struct {
    int known;                      // the device's answers are in (caps)
    int fans, uint8;                // triangle fans, 8-bit indices: drawn natively
    int portability;                // VK_KHR_portability_subset is listed
    int list_restart;               // restart on lists drawn natively (hostvk_dev.x.list_restart: VK_EXT_primitive_topology_list_restart, enabled by vk_device.c)
    int t_no_fans, t_no_uint8, t_no_list_restart;   // tests: the device's fans, 8-bit indices and restart on lists taken as absent
    uint64_t t_limit;               // tests: the GPU path's limit in bytes (0: HVK_IX_LIMIT)
    uint32_t t_arena, t_in;         // tests: the arena's bytes and the indices a draw whose count the GPU holds reads at most (0: HVK_IX_ARENA, HVK_IX_IND_IN)
} g_ix;
// What ran: on the GPU (in the prologue, in the main command buffer, with restart), on the CPU (past the limit, without a device address), the indices read and drawn (at most, for
// the GPU's restart form), the submit thread's time
static struct {
    _Atomic uint64_t gpu, gpu_pre, gpu_restart, gpu_in, gpu_out, gpu_ns, cpu, cpu_over, cpu_noaddr, cpu_in, cpu_out, cpu_ns;
    _Atomic uint64_t ind, ind_pre, ind_records, ind_xfb, ind_mdi, ind_ns;   // draws whose count the GPU holds: conversions, in the prologue, records, DrawTransformFeedback, multi-draws, ns
} g_st;

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) { for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1; return 0; }
static uint64_t ix_limit(void) { return g_ix.t_limit ? g_ix.t_limit : HVK_IX_LIMIT; }
static void caps(void) {
    if (g_ix.known) return;
    g_ix.known = 1;
    uint32_t n = 0; VkExtensionProperties *e = NULL;
    if (vkEnumerateDeviceExtensionProperties(hvk.dev.phys, NULL, &n, NULL) == VK_SUCCESS && n && (e = calloc(n, sizeof *e)) && vkEnumerateDeviceExtensionProperties(hvk.dev.phys, NULL, &n, e) == VK_SUCCESS)
        g_ix.portability = has_ext(e, n, "VK_KHR_portability_subset");
    free(e);
    g_ix.fans = !g_ix.portability && !g_ix.t_no_fans;
    g_ix.list_restart = hvk.dev.x.list_restart && !g_ix.t_no_list_restart;
    g_ix.uint8 = hvk.dev.x.index_uint8 && !g_ix.t_no_uint8;
    hostvk_log("[hostvk] index conversions on the GPU (shaders/index_expand.comp, index_restart.comp; in the submission's prologue, or before the draw's pass for indices the GPU "
               "writes earlier in the submission); native: triangle fans %s, 8-bit indices %s, restart on strips and fans at the all-ones index, restart on lists %s; the CPU "
               "converts past %llu bytes of indices a draw", g_ix.fans ? "yes" : g_ix.portability ? "no (VK_KHR_portability_subset)" : "no (a test)",
               g_ix.uint8 ? "yes" : hvk.dev.x.index_uint8 ? "no (a test)" : "no (no VK_KHR_index_type_uint8)",
               g_ix.list_restart ? "yes (VK_EXT_primitive_topology_list_restart)" : hvk.dev.x.list_restart ? "no (a test): converted" : "no (no VK_EXT_primitive_topology_list_restart): converted",
               (unsigned long long)ix_limit());
}
// The indices a segment of n vertices draws in a mode (the kernels' segc)
static uint64_t segc(int mode, uint32_t g, uint64_t n) {
    switch (mode) {
        case M_QUADS: return n / 4 * 6;
        case M_QSTRIP: return n >= 4 ? (n - 2) / 2 * 6 : 0;
        case M_FAN: return n >= 3 ? 3 * (n - 2) : 0;
        case M_LOOP: return n >= 2 ? 2 * n : 0;
        case M_LIST: return g ? n / g * g : 0;
        default: return n;
    }
}
static int list_group(int prim) {
    switch (prim) {
        case NVN_DRAW_PRIMITIVE_POINTS: return 1; case NVN_DRAW_PRIMITIVE_LINES: return 2; case NVN_DRAW_PRIMITIVE_TRIANGLES: return 3;
        case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY: return 4; case NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY: return 6; case NVN_DRAW_PRIMITIVE_PATCHES: return -1;
        default: return 0;
    }
}

// A list past 2^30 indices (4 GiB, more than a Switch has: a corrupt count) is not converted, and the draw draws nothing (nout 0); the kernels' 32-bit arithmetic stays exact below
static uint32_t nout_of(uint64_t c, uint32_t n) {
    if (c <= 1u << 30) return (uint32_t)c;
    HVK_RED_ONCE("a draw of %u indices would convert into %llu indices, past 2^30 (a corrupt count): skipped", n, (unsigned long long)c);
    return 0;
}
void hvk_ix_plan_make(const hvk_ix_in *in, VkPrimitiveTopology native, hvk_ix_plan *pl) {
    caps();
    memset(pl, 0, sizeof *pl);
    pl->form = HVK_IX_NATIVE; pl->topo = native; pl->nout = in->n;
    int idx = in->isz != 0, restart = idx && in->restart, narrow = in->isz == 1 && !g_ix.uint8;
    uint32_t all = in->isz == 1 ? 0xffu : in->isz == 2 ? 0xffffu : 0xffffffffu;
    int prim = in->prim, mode = -1;
    switch (prim) {
        case NVN_DRAW_PRIMITIVE_QUADS: mode = M_QUADS; break;
        case NVN_DRAW_PRIMITIVE_QUAD_STRIP: mode = M_QSTRIP; break;
        case NVN_DRAW_PRIMITIVE_LINE_LOOP: mode = M_LOOP; break;
        case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: case NVN_DRAW_PRIMITIVE_POLYGON: if (!g_ix.fans) mode = M_FAN; break;
        default: break;
    }
    if (mode >= 0) {   // no Vulkan primitive: a list
        pl->topo = mode == M_LOOP ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        pl->mode = mode; pl->nout = nout_of(segc(mode, 0, in->n), in->n);
        pl->form = restart ? HVK_IX_RESTART : !idx && mode == M_QUADS && in->n / 4 <= QUAD_SHARED ? HVK_IX_SHARED : HVK_IX_EXPAND;
        return;
    }
    if (!idx) return;   // the vertices as they are
    int g = list_group(prim);
    if (g && restart && !(g > 0 && g_ix.list_restart)) {   // a list with restart the device does not restart: converted, the primitive a restart cuts short dropped
        pl->form = HVK_IX_RESTART; pl->mode = M_LIST; pl->group = g > 0 ? (uint32_t)g : in->patch ? in->patch : 1;
        pl->nout = nout_of(segc(M_LIST, pl->group, in->n), in->n);
        return;
    }
    pl->restart = restart;   // a strip or fan, or a list the device restarts (VK_EXT_primitive_topology_list_restart): Vulkan's restart at the all-ones index
    if (narrow || (restart && in->ri != all)) { pl->form = HVK_IX_MAP; pl->mode = M_MAP; pl->nout = nout_of(in->n, in->n); }   // widened (a restart index other than all ones becomes all ones)
}

int hvk_ix_gpu_takes(const hvk_ix_in *in, const hvk_ix_plan *pl, int readable, VkDeviceAddress *src) {
    *src = 0;
    if (!hvk.dev.caps.buffer_device_address) { atomic_fetch_add(&g_st.cpu_noaddr, 1); HVK_NOTE_ONCE("index conversions: no buffer device address on this device, the CPU converts"); return 0; }
    uint64_t bytes = (uint64_t)pl->nout * 4;
    if (readable && bytes > ix_limit()) {
        atomic_fetch_add(&g_st.cpu_over, 1);
        static _Atomic int told;
        if (atomic_fetch_add(&told, 1) < 4) hostvk_log("[hostvk] index conversion of %u indices into %llu bytes, past the GPU path's %llu: converted on the CPU (counted in the shutdown line)", in->n,
                                                      (unsigned long long)bytes, (unsigned long long)ix_limit());
        return 0;
    }
    if (!in->isz) return 1;
    uint64_t lo = in->src & ~3ull, hi = (in->src + (uint64_t)in->n * (uint64_t)in->isz + 3) & ~3ull;
    VkBuffer b; VkDeviceSize o;
    if (!hvk_buf(lo, hi > lo ? hi - lo : 4, &b, &o, src) || !*src) {
        atomic_fetch_add(&g_st.cpu_noaddr, 1);
        HVK_NOTE_ONCE("index conversions: indices at %#llx are in no memory pool with a device address, the CPU converts them", (unsigned long long)in->src);
        *src = 0; return 0;
    }
    return 1;
}

// index_restart.comp's push constants: device addresses as two words (the list's source, the list, the records written; for a draw whose count the GPU holds the game's first
// record, the multi-draw count, the arena's header and the statistics words), then the stream and the draw (the kernel's header says what each holds in either form)
typedef struct {
    uint32_t src[2], dst[2], args[2], rec[2], cnt[2], heap[2], stat[2];
    uint32_t shift, isz, n, mode, group, ri, cap, instances, base_vertex, base_instance, flags, stride, avail, mdi;
} rs_push;
static void a2(uint32_t o[2], uint64_t a) { o[0] = (uint32_t)a; o[1] = (uint32_t)(a >> 32); }
int hvk_ix_gpu(VkCommandBuffer cb, const hvk_ix_in *in, const hvk_ix_plan *pl, VkDeviceAddress src, uint32_t instances, int32_t vertex_offset, uint32_t base_instance, hvk_ix_out *out) {
    memset(out, 0, sizeof *out);
    hvk_alloc a;
    if (!hvk_ring_alloc((VkDeviceSize)(pl->nout ? pl->nout : 1) * 4, 16, &a) || !a.dev) return 0;
    out->buf = a.buf; out->off = a.off; out->count = pl->nout;
    uint32_t shift = in->isz ? (uint32_t)(in->src & 3) : 0;
    if (pl->form == HVK_IX_RESTART) {
        hvk_alloc r;
        if (!hvk_ring_alloc(20, 16, &r) || !r.dev) return 0;
        rs_push p; memset(&p, 0, sizeof p);
        a2(p.src, src); a2(p.dst, a.dev); a2(p.args, r.dev);
        p.shift = shift; p.isz = (uint32_t)in->isz; p.n = in->n; p.mode = (uint32_t)pl->mode; p.group = pl->group ? pl->group : 1; p.ri = in->ri; p.cap = pl->nout;
        p.instances = instances; p.base_vertex = (uint32_t)vertex_offset; p.base_instance = base_instance; p.flags = 1;   // restart on
        if (!hvk_kernel_dispatch(cb, HVK_K_IX_RESTART, &p, sizeof p, 1)) return 0;
        out->indirect = 1; out->abuf = r.buf; out->aoff = r.off;
        return 1;
    }
    uint32_t groups = (pl->nout + 63) / 64; if (groups > 1024) groups = 1024; if (!groups) groups = 1;
    struct { uint32_t src[2], dst[2], shift, isz, n, nout, mode, restart, ri, stride; } p = {
        {(uint32_t)src, (uint32_t)(src >> 32)}, {(uint32_t)a.dev, (uint32_t)(a.dev >> 32)}, shift, (uint32_t)in->isz, in->n, pl->nout, (uint32_t)pl->mode,
        (uint32_t)(in->isz && in->restart), in->ri, groups * 64};
    return hvk_kernel_dispatch(cb, HVK_K_IX_EXPAND, &p, sizeof p, groups);
}

// ---------- Draws whose count the GPU holds (the comment at the top) ----------
static struct { uint64_t fence; hvk_alloc list, head; } g_ar;   // the open submission's arena (its fence: one arena a submission) and its header (the indices taken)
// The statistics words the kernel adds to (a buffer of the backend's own, host-visible): lists cut at the arena, draws cut at a per-draw limit, records past their pool
static VkBuffer g_stb; static hvk_mem *g_stb_mem; static volatile uint32_t *g_stw; static VkDeviceAddress g_stw_dev; static int g_told[3];
static uint32_t ar_bytes(void) { return g_ix.t_arena ? g_ix.t_arena : HVK_IX_ARENA; }
static uint32_t in_max(void) { return g_ix.t_in ? g_ix.t_in : HVK_IX_IND_IN; }
static int stat_words(void) {   // words 0-2 the cuts; 3 all ones, the count of a draw that is not a multi-draw; 4-7 zero, what a draw without indices points its source at
    if (g_stw_dev) return 1;
    void *map = NULL;
    if (hvk_mem_buffer(32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, HVK_MEM_HOST, &g_stb, &g_stb_mem, &map)) { g_stb = VK_NULL_HANDLE; g_stb_mem = NULL; return 0; }
    memset(map, 0, 32); g_stw = map; g_stw[3] = 0xffffffffu;
    VkBufferDeviceAddressInfo ba = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, g_stb}; g_stw_dev = vkGetBufferDeviceAddress(hvk.d, &ba);
    return g_stw_dev != 0;
}
static uint32_t stat_word(int k) { return g_stw ? g_stw[k] : 0; }   // as far as the GPU has written it (a submission still running may add to it)
static void ind_tell(void) {   // the GPU's cuts, each said once when first seen
    for (int k = 0; k < 3; k++) {
        if (g_told[k] || !stat_word(k)) continue;
        g_told[k] = 1;
        if (k == 0) hostvk_log("[RED] [hostvk] index conversion of a draw whose count the GPU holds: its list did not fit what was left of the submission's arena of %u indices and was cut to it "
                               "(a limit; counted in the shutdown line)", ar_bytes() / 4);
        else if (k == 1) hostvk_log("[RED] [hostvk] index conversion of a draw whose count the GPU holds: it read more than %u indices, or drew more than %u quads over the shared quad list, "
                                    "and was cut there (a limit; counted in the shutdown line)", in_max(), (unsigned)QUAD_SHARED);
        else hostvk_log("[RED] [hostvk] index conversion of a draw whose count the GPU holds: its record names indices past the end of the index buffer's memory pool; the count stops "
                        "there (counted in the shutdown line)");
    }
}
static int arena(void) {   // the open submission's, allocated at its first draw whose count the GPU holds
    uint64_t f = hvk_open_fence();
    if (g_ar.list.buf && g_ar.fence == f) return 1;
    memset(&g_ar, 0, sizeof g_ar);
    if (!hvk_ring_alloc(ar_bytes(), 16, &g_ar.list) || !g_ar.list.dev || !hvk_ring_alloc(16, 16, &g_ar.head) || !g_ar.head.dev) { memset(&g_ar, 0, sizeof g_ar); return 0; }
    memset(g_ar.head.cpu, 0, 16);   // nothing taken (written before the submission: the host write is visible to its commands)
    g_ar.fence = f;
    return 1;
}
static uint64_t pool_tail(uint64_t a) {   // the bytes of a's memory pool from a (0: in no pool)
    const nvn_pool_ref *c = nvn_pool_cached(a, 1); nvn_pool_ref r;
    if (!c) { pthread_mutex_lock(&hvk.mu); int ok = nvn_pool_lookup(a, 1, &r); pthread_mutex_unlock(&hvk.mu); if (!ok) return 0; c = &r; }
    return c->hi > a ? c->hi - a : 0;
}
static const char *ind_name(int kind) { return kind == 2 ? "DrawTransformFeedback" : kind == 1 ? "an indirect indexed draw" : "an indirect draw"; }
int hvk_ix_ind_prepare(const hvk_ix_in *in, const hvk_ix_plan *pl, hvk_ix_ind *ii) {
    (void)pl;
    VkBuffer b; VkDeviceSize o;
    if (!hvk.dev.caps.buffer_device_address) { HVK_RED_ONCE("draws whose count the GPU holds and whose primitive needs an index conversion: no buffer device address on this device, skipped"); return 0; }
    uint32_t n = ii->records ? ii->records : 1, size = ii->kind == 2 ? 4 : ii->kind == 1 ? 20 : 16;
    ii->rd_lo = ii->rec; ii->rd_hi = ii->rec + (uint64_t)(n - 1) * ii->stride * (ii->kind != 2) + size;
    ii->drec = ii->dcnt = ii->dsrc = 0; ii->ib_bytes = 0; ii->avail = 0;
    if ((ii->rec & 3) || !hvk_buf(ii->rd_lo, ii->rd_hi - ii->rd_lo, &b, &o, &ii->drec) || !ii->drec) {
        HVK_RED_ONCE("%s to convert whose %s at %#llx is in no memory pool with a device address (or not 4-byte aligned): skipped", ind_name(ii->kind),
                     ii->kind == 2 ? "control buffer" : "record", (unsigned long long)ii->rec);
        return 0;
    }
    if (ii->cnt && ((ii->cnt & 3) || !hvk_buf(ii->cnt, 4, &b, &o, &ii->dcnt) || !ii->dcnt)) {
        HVK_RED_ONCE("a multi-draw to convert whose count at %#llx is in no memory pool with a device address (or not 4-byte aligned): skipped", (unsigned long long)ii->cnt);
        return 0;
    }
    if (in->isz) {   // the indices a record may name: from the index buffer to its pool's end
        uint64_t tail = pool_tail(in->src), lo = in->src & ~3ull, av = tail / (uint64_t)in->isz;
        if (av > 1u << 30) av = 1u << 30;   // the kernel's 32-bit arithmetic stays exact below
        if (!av || !hvk_buf(lo, in->src - lo + av * (uint64_t)in->isz, &b, &o, &ii->dsrc) || !ii->dsrc) {
            HVK_RED_ONCE("an indirect indexed draw to convert whose indices at %#llx are in no memory pool with a device address: skipped", (unsigned long long)in->src);
            return 0;
        }
        ii->avail = (uint32_t)av; ii->ib_bytes = av * (uint64_t)in->isz;
    }
    return 1;
}
int hvk_ix_ind_gpu(VkCommandBuffer cb, int pre, const hvk_ix_in *in, const hvk_ix_plan *pl, const hvk_ix_ind *ii, hvk_ix_out *out) {
    uint64_t t0 = plat_now_ns();
    memset(out, 0, sizeof *out);
    uint32_t n = ii->records ? ii->records : 1;
    int shared = pl->form == HVK_IX_SHARED;   // quads without indices: over the shared quad list, the record only
    hvk_alloc r;
    if (!stat_words() || (!shared && !arena()) || !hvk_ring_alloc((VkDeviceSize)n * 20, 16, &r) || !r.dev) {
        HVK_RED_ONCE("index conversion of a draw whose count the GPU holds: the ring, the arena or the statistics words could not be allocated, skipped");
        return 0;
    }
    ind_tell();
    rs_push p; memset(&p, 0, sizeof p);   // every address valid (the kernel's header): what a form does not use points at the statistics words
    a2(p.src, in->isz ? ii->dsrc : g_stw_dev + 16); a2(p.dst, shared ? g_stw_dev + 16 : g_ar.list.dev); a2(p.args, r.dev); a2(p.rec, ii->drec);
    a2(p.cnt, ii->cnt ? ii->dcnt : g_stw_dev + 12); a2(p.heap, shared ? g_stw_dev + 16 : g_ar.head.dev); a2(p.stat, g_stw_dev);
    p.shift = in->isz ? (uint32_t)(in->src & 3) : 0; p.isz = (uint32_t)in->isz; p.n = shared ? UINT32_MAX : in_max();
    p.mode = shared ? 6u : pl->form == HVK_IX_MAP ? 0u : (uint32_t)pl->mode; p.group = pl->group ? pl->group : 1; p.ri = in->ri;
    p.cap = shared ? (uint32_t)QUAD_SHARED * 6 : ar_bytes() / 4; p.instances = ii->instances; p.base_instance = ii->base_instance;
    p.flags = (in->isz && in->restart ? 1u : 0u) | (uint32_t)ii->kind << 1; p.stride = ii->stride; p.avail = ii->avail; p.mdi = ii->first;
    if (!hvk_kernel_dispatch(cb, HVK_K_IX_RESTART, &p, sizeof p, n)) return 0;
    out->buf = shared ? VK_NULL_HANDLE : g_ar.list.buf; out->off = shared ? 0 : g_ar.list.off; out->indirect = 1; out->abuf = r.buf; out->aoff = r.off; out->records = n;
    atomic_fetch_add(&g_st.ind, 1); atomic_fetch_add(&g_st.ind_records, n); atomic_fetch_add(&hvk.st_index_ind, 1);
    if (pre) atomic_fetch_add(&g_st.ind_pre, 1);
    if (ii->kind == 2) atomic_fetch_add(&g_st.ind_xfb, 1);
    if (ii->cnt) atomic_fetch_add(&g_st.ind_mdi, 1);
    atomic_fetch_add(&g_st.ind_ns, plat_now_ns() - t0);
    return 1;
}

// The CPU's conversion: the kernels' list. Indexed quads and fans go through nvn_primitive.h, the Metal backend's expansion; the rest segment by segment
static uint32_t rd(const uint8_t *s, int isz, uint64_t k) {
    if (!isz) return (uint32_t)k;
    if (isz == 1) return s[k];
    if (isz == 2) { uint16_t v; memcpy(&v, s + 2 * k, 2); return v; }
    uint32_t v; memcpy(&v, s + 4 * k, 4); return v;
}
static uint32_t cpu_run(const hvk_ix_in *in, const hvk_ix_plan *pl, uint32_t *d) {
    const uint8_t *s = (const uint8_t *)(uintptr_t)in->src;
    int isz = in->isz, restart = isz && in->restart;
    uint32_t n = in->n, o = 0, g = pl->group ? pl->group : 1;
    if (pl->mode == M_MAP) { for (uint32_t k = 0; k < n; k++) { uint32_t v = rd(s, isz, k); d[k] = restart && v == in->ri ? 0xffffffffu : v; } return n; }
    if (isz && (pl->mode == M_QUADS || pl->mode == M_FAN)) return nvn_expand_primitive(d, s, n, (unsigned)isz, pl->mode == M_QUADS ? NVN_DRAW_PRIMITIVE_QUADS : NVN_DRAW_PRIMITIVE_TRIANGLE_FAN, restart, in->ri);
    for (uint64_t st = 0, i = 0; i <= n; i++) {
        if (i < n && !(restart && rd(s, isz, i) == in->ri)) continue;
        uint64_t m = i - st;   // the segment [st, i)
        #define V(k) rd(s, isz, st + (k))
        switch (pl->mode) {
            case M_QUADS: for (uint64_t q = 0; q + 4 <= m; q += 4) { uint32_t a = V(q), b = V(q + 1), c = V(q + 2), e = V(q + 3); d[o++] = a; d[o++] = b; d[o++] = c; d[o++] = a; d[o++] = c; d[o++] = e; } break;
            case M_QSTRIP: for (uint64_t k = 0; m >= 4 && k + 4 <= m; k += 2) { uint32_t a = V(k), b = V(k + 1), w = V(k + 2), c = V(k + 3); d[o++] = a; d[o++] = b; d[o++] = c; d[o++] = a; d[o++] = c; d[o++] = w; } break;
            case M_FAN: for (uint64_t k = 0; k + 3 <= m; k++) { d[o++] = V(0); d[o++] = V(k + 1); d[o++] = V(k + 2); } break;
            case M_LOOP: for (uint64_t k = 0; m >= 2 && k < m; k++) { d[o++] = V(k); d[o++] = V(k + 1 == m ? 0 : k + 1); } break;
            default: for (uint64_t k = 0; k + g <= m; k += g) for (uint32_t c = 0; c < g; c++) d[o++] = V(k + c); break;
        }
        #undef V
        st = i + 1;
    }
    return o;
}
int hvk_ix_cpu(const hvk_ix_in *in, const hvk_ix_plan *pl, hvk_ix_out *out) {
    memset(out, 0, sizeof *out);
    hvk_alloc a;
    if (!hvk_ring_alloc((VkDeviceSize)(pl->nout ? pl->nout : 1) * 4, 16, &a)) return 0;
    out->buf = a.buf; out->off = a.off;
    out->count = cpu_run(in, pl, (uint32_t *)(void *)a.cpu);
    return 1;
}

void hvk_ix_count(int gpu, int pre, const hvk_ix_in *in, const hvk_ix_plan *pl, uint64_t ns) {
    if (gpu) {
        atomic_fetch_add(&g_st.gpu, 1); atomic_fetch_add(&g_st.gpu_in, in->n); atomic_fetch_add(&g_st.gpu_out, pl->nout); atomic_fetch_add(&g_st.gpu_ns, ns);
        if (pre) atomic_fetch_add(&g_st.gpu_pre, 1);
        if (pl->form == HVK_IX_RESTART) atomic_fetch_add(&g_st.gpu_restart, 1);
        atomic_fetch_add(&hvk.st_index_gpu, 1);
    } else {
        atomic_fetch_add(&g_st.cpu, 1); atomic_fetch_add(&g_st.cpu_in, in->n); atomic_fetch_add(&g_st.cpu_out, pl->nout); atomic_fetch_add(&g_st.cpu_ns, ns);
        atomic_fetch_add(&hvk.st_index_cpu, 1);
    }
}
static uint64_t g_cut_past[3];   // the statistics words of the devices shut down
void hvk_ix_shutdown(void) {   // after the device is idle: the statistics words are final
    ind_tell();
    for (int k = 0; k < 3; k++) g_cut_past[k] += stat_word(k);
    uint64_t g = atomic_load(&g_st.gpu), c = atomic_load(&g_st.cpu), ind = atomic_load(&g_st.ind);
    if (g || c || ind)
        hostvk_log("[hostvk] index conversions: %llu on the GPU (%llu in the prologue, %llu after GPU writes; %llu with restart), %llu indices read, %llu drawn at most, %.3f ms on the "
                   "submit thread; %llu on the CPU (%llu past the GPU path's limit, %llu without a device address), %llu indices read, %llu drawn at most, %.3f ms; %llu of draws whose "
                   "count the GPU holds (%llu in the prologue, %llu records, %llu DrawTransformFeedback, %llu multi-draws), %.3f ms, cut on the GPU: %llu at the arena, %llu at a "
                   "per-draw limit, %llu records past their index buffer's pool",
                   (unsigned long long)g, (unsigned long long)atomic_load(&g_st.gpu_pre), (unsigned long long)(g - atomic_load(&g_st.gpu_pre)), (unsigned long long)atomic_load(&g_st.gpu_restart),
                   (unsigned long long)atomic_load(&g_st.gpu_in), (unsigned long long)atomic_load(&g_st.gpu_out), atomic_load(&g_st.gpu_ns) / 1e6, (unsigned long long)c,
                   (unsigned long long)atomic_load(&g_st.cpu_over), (unsigned long long)atomic_load(&g_st.cpu_noaddr), (unsigned long long)atomic_load(&g_st.cpu_in),
                   (unsigned long long)atomic_load(&g_st.cpu_out), atomic_load(&g_st.cpu_ns) / 1e6, (unsigned long long)ind, (unsigned long long)atomic_load(&g_st.ind_pre),
                   (unsigned long long)atomic_load(&g_st.ind_records), (unsigned long long)atomic_load(&g_st.ind_xfb), (unsigned long long)atomic_load(&g_st.ind_mdi),
                   atomic_load(&g_st.ind_ns) / 1e6, (unsigned long long)g_cut_past[0], (unsigned long long)g_cut_past[1], (unsigned long long)g_cut_past[2]);
    if (g_stb) hvk_mem_buffer_free(g_stb, g_stb_mem);
    g_stb = VK_NULL_HANDLE; g_stb_mem = NULL; g_stw = NULL; g_stw_dev = 0; memset(g_told, 0, sizeof g_told);
    memset(&g_ar, 0, sizeof g_ar);   // the next device's fences start again
    g_ix.known = 0;
}

// Tests (dm/gfx/tests/vk/t_vk_index_convert.c): the device's triangle fans, 8-bit indices and restart on lists taken as absent (no_fans, no_uint8, no_list_restart; -1 keeps), the
// GPU path's limit in bytes (0: the default, -1 keeps); asked again at the next draw. The arena's bytes and the indices a draw whose count the GPU holds reads at most (0: the
// defaults, -1 keeps; a multiple of 16 bytes). And the counters: GPU conversions, of them in the prologue and with restart, indices read and drawn, submit-thread ns; CPU
// conversions, past the limit, without an address, indices read and drawn, ns; conversions of draws whose count the GPU holds, in the prologue, records, DrawTransformFeedback,
// multi-draws, ns, and the GPU's cuts (at the arena, at a per-draw limit, records past their pool: as far as the GPU has written them, so read after a wait)
void hvk_test_index_opts(int no_fans, int no_uint8, int no_list_restart, int64_t limit);
void hvk_test_index_opts(int no_fans, int no_uint8, int no_list_restart, int64_t limit) {
    if (no_fans >= 0) g_ix.t_no_fans = no_fans;
    if (no_uint8 >= 0) g_ix.t_no_uint8 = no_uint8;
    if (no_list_restart >= 0) g_ix.t_no_list_restart = no_list_restart;
    if (limit >= 0) g_ix.t_limit = (uint64_t)limit;
    g_ix.known = 0;
}
void hvk_test_index_ind(int64_t arena_bytes, int64_t in_max);
void hvk_test_index_ind(int64_t arena_bytes, int64_t in_max) {
    if (arena_bytes >= 0) g_ix.t_arena = (uint32_t)arena_bytes;
    if (in_max >= 0) g_ix.t_in = (uint32_t)in_max;
}
void hvk_test_index_stats(uint64_t s[21]);
void hvk_test_index_stats(uint64_t s[21]) {
    s[0] = atomic_load(&g_st.gpu); s[1] = atomic_load(&g_st.gpu_pre); s[2] = atomic_load(&g_st.gpu_restart); s[3] = atomic_load(&g_st.gpu_in); s[4] = atomic_load(&g_st.gpu_out);
    s[5] = atomic_load(&g_st.gpu_ns); s[6] = atomic_load(&g_st.cpu); s[7] = atomic_load(&g_st.cpu_over); s[8] = atomic_load(&g_st.cpu_noaddr); s[9] = atomic_load(&g_st.cpu_in);
    s[10] = atomic_load(&g_st.cpu_out); s[11] = atomic_load(&g_st.cpu_ns);
    s[12] = atomic_load(&g_st.ind); s[13] = atomic_load(&g_st.ind_pre); s[14] = atomic_load(&g_st.ind_records); s[15] = atomic_load(&g_st.ind_xfb); s[16] = atomic_load(&g_st.ind_mdi);
    s[17] = atomic_load(&g_st.ind_ns);
    for (int k = 0; k < 3; k++) s[18 + k] = g_cut_past[k] + stat_word(k);
}
