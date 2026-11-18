// A fragment input interpolated without perspective, over triangles far larger than the window. Mario vs. Donkey Kong's result screen (the LevelClear model: one quad 16 times
// the window's width, its vertices' w from 100.8 to 109.7) and its wipes (the BlackScreen model) compute the screen position in the vertex shader from the vertex's own NDC and
// read it in the fragment shader through an input decorated NoPerspective (the vertex shader's output carries no decoration: the chain's translations decorate inputs only), so
// what the fragment shader reads is its own place in the window, whatever the triangle. Each picture is the quad drawn into a 1920 x 1080 RGBA32F target with npw.vert and
// npw.frag (npw_s.frag, the same input read with perspective correction, is the control: a number everywhere), its every texel compared with its place, for four quads: the
// game's, the same size with one w, one that just covers the window (nothing to clip) and one a half larger (w 1 and 2).
// - The driver alone: the same shaders, vertices and indices through plain Vulkan calls on the backend's device (a pipeline, a target and buffers of the test's own; nothing
//   of the backend's state in between), under each combination of what the backend sets that reaches clipping: depth clamp, negativeOneToOne (VK_EXT_depth_clip_control: NVN's
//   GL depth mode) and the viewport flipped (the upper-left origin), each where the device has it.
// - Through the backend: the NVN records replayed by dm/gfx/hostvk through fast-linked libraries, their optimized relink and a monolithic pipeline (levels A and C), each
//   picture with the same texels not a number and wrong as the driver alone gives with the backend's settings (no clamp, its depth mode, the lower-left origin).
// What must hold on every driver: every texel covered; the controls a number everywhere; the input the texel's place for the quads with nothing to clip and with one w. On every
// driver but lavapipe the two clipped quads whose w differ must be right too. lavapipe (Mesa's draw module, src/gallium/auxiliary/draw/draw_pipe_clip.c interp(), unchanged
// from Mesa 9.1 to 26.1-devel) gets those two wrong: for a vertex it makes, it chooses the axis of the no-perspective factor where the edge's clip coordinates differ but takes the
// ratio of window coordinates, and an edge with one window x and two w (a side of these quads, or an edge an earlier plane made on x = +-w) gives 0/0 (not a number) or a ratio
// of rounding errors there (docs/reference/rendering.md, "A NoPerspective input reads wrong values on lavapipe"; the backend names it in a red line at such a shader's first
// draw, vk_commands.c hvk_draw). On that driver the test records each of those pictures (a "known driver defect" line, the check passing wrong or right) and says so when a
// Mesa draws them right: the day to drop the red line. No Metal counterpart.
#include "t_vk_harness.h"
#include "vk_pso.h"
#include <math.h>
#include "shaders/gen/npw_vert.h"
#include "shaders/gen/npw_frag.h"
#include "shaders/gen/npw_s_frag.h"

enum { W = 1920, H = 1080 };
typedef struct { float x, y, z, w; } v4;
// the quads as clip positions, two triangles each: (0, 1, 2), (1, 3, 2)
static const struct { const char *name; v4 v[4]; int clipped_w; } quads[] = {   // clipped_w: clipped, and its vertices' w differ
    {"the game's quad (NDC to 17 x 29, w 100.8 and 109.7)", {{-1715.6195f, 2966.2842f, 8.6639f, 100.8420f}, {-1715.6195f, -3127.7168f, 18.8785f, 109.6947f},
                                                              {1715.6195f, 2966.2842f, 8.6639f, 100.8420f}, {1715.6195f, -3127.7168f, 18.8785f, 109.6947f}}, 1},
    {"the same size with one w", {{-1700, 2900, 10, 100}, {-1700, -2900, 10, 100}, {1700, 2900, 10, 100}, {1700, -2900, 10, 100}}, 0},
    {"a quad that just covers the window, w 1 and 2", {{-1, 1, 0.5f, 1}, {-2, -2, 1, 2}, {1, 1, 0.5f, 1}, {2, -2, 1, 2}}, 0},
    {"a quad one and a half times the window, w 1 and 2", {{-1.5f, 1.5f, 0.5f, 1}, {-3, -3, 1, 2}, {1.5f, 1.5f, 0.5f, 1}, {3, -3, 1, 2}}, 1},
};
enum { NQ = sizeof quads / sizeof *quads };
static const uint16_t idx[6] = {0, 1, 2, 1, 3, 2};
static const hostvk_dev *g_d;
static int g_lvp, g_lvp_wrong, g_lvp_right;   // lavapipe; its clipped pictures whose w differ, wrong and right

// ---------- A picture of npw.frag: its texels against their places ----------
typedef struct { size_t uncovered, nan, bad; double ex, ey; int wx, wy; } pic;
static pic measure(const float *px) {   // x exactly; y from the top or from the bottom (the window origin), one of the two for the whole picture
    pic r; memset(&r, 0, sizeof r); double ey[2] = {0, 0};
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        const float *o = px + ((size_t)y * W + x) * 4;
        if (o[0] != o[0] || o[1] != o[1]) { r.nan++; continue; }
        if (o[3] < 0.5f) { r.uncovered++; continue; }   // the clear's -1
        double dx = fabs((double)o[0] - (x + 0.5) / W), d0 = fabs((double)o[1] - (y + 0.5) / H), d1 = fabs((double)o[1] - (1 - (y + 0.5) / H));
        if (dx > r.ex) { r.ex = dx; r.wx = x; r.wy = y; }
        if (d0 > ey[0]) ey[0] = d0;
        if (d1 > ey[1]) ey[1] = d1;
        if (dx > 1e-3 || (d0 > 1e-3 && d1 > 1e-3)) r.bad++;
    }
    r.ey = ey[0] < ey[1] ? ey[0] : ey[1];
    return r;
}
// The checks of a NoPerspective picture of quad k (smooth: the control, read with perspective correction), what: how it was drawn
static void judge(const char *what, unsigned k, int smooth, int ok, const pic *r, const float *px) {
    char name[400];
    if (smooth) {   // a number everywhere (its values are the place only where w is one value)
        snprintf(name, sizeof name, "%s, %s, read with perspective correction: every texel covered, none not a number (%zu, %zu)", what, quads[k].name, r->uncovered, r->nan);
        t(name, ok && !r->uncovered && !r->nan); return;
    }
    snprintf(name, sizeof name, "%s, %s: every texel covered and read back (%zu not)", what, quads[k].name, r->uncovered); t(name, ok && !r->uncovered);
    int right = ok && r->ex <= 1e-3 && r->ey <= 1e-3 && !r->nan && !r->bad;
    if (g_lvp && quads[k].clipped_w) {   // Mesa's clipper (draw_pipe_clip.c interp): recorded, either way
        if (right) {
            g_lvp_right++;
            printf("note: %s, %s: right on this lavapipe: its clipper's no-perspective factor is fixed (draw_pipe_clip.c interp); the red line of vk_commands.c and the docs entry can go\n", what, quads[k].name);
        } else {
            g_lvp_wrong++;
            printf("known driver defect (lavapipe's clipper, draw_pipe_clip.c interp: a NoPerspective input over a clipped triangle whose w differ): %s, %s: %zu texels not a number, %zu with a wrong place\n",
                   what, quads[k].name, r->nan, r->bad);
        }
        snprintf(name, sizeof name, "%s, %s: as lavapipe's clipper leaves it, recorded (%zu texels not a number, %zu wrong)", what, quads[k].name, r->nan, r->bad); t(name, ok);
        return;
    }
    if (!right && ok) {
        const float *o = px + ((size_t)r->wy * W + r->wx) * 4, *o2 = px + ((size_t)(H - 30) * W + 900) * 4, *o3 = px + ((size_t)300 * W + 300) * 4;
        fprintf(stderr, "%s, %s: x off by up to %g (at %d,%d: %g %g), y by %g; texel 900,%d reads %g %g, texel 300,300 reads %g %g; %zu texels with a wrong place, %zu not a number\n", what, quads[k].name,
                r->ex, r->wx, r->wy, (double)o[0], (double)o[1], r->ey, H - 30, (double)o2[0], (double)o2[1], (double)o3[0], (double)o3[1], r->bad, r->nan);
    }
    snprintf(name, sizeof name, "%s, %s: the input is the texel's place in the window (off by at most %.2g, %.2g of the window; %zu texels wrong, %zu not a number)", what, quads[k].name,
             r->ex, r->ey, r->bad, r->nan);
    t(name, right);
}

// ---------- The driver alone: plain Vulkan on the backend's device ----------
typedef struct { VkDevice D; VkImage img; VkDeviceMemory mem; VkImageView view; VkBuffer vb, rb; VkDeviceMemory vbm, rbm; void *vbp, *rbp; VkPipelineLayout pl; VkShaderModule vs, fs[2]; } raw_t;
static VkShaderModule raw_module(VkDevice D, const uint32_t *code, size_t n) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, n, code}; VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(D, &ci, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}
static int raw_up(raw_t *r) {   // the target (RGBA32F, GENERAL from the start), the vertex and index buffer, the readback buffer, the modules, an empty layout
    memset(r, 0, sizeof *r); r->D = g_d->device;
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT, {W, H, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(r->D, &ii, NULL, &r->img)) return 0;
    VkMemoryRequirements req; vkGetImageMemoryRequirements(r->D, r->img, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, UINT32_MAX};
    for (uint32_t i = 0; i < g_d->mem.memoryTypeCount && ai.memoryTypeIndex == UINT32_MAX; i++)
        if ((req.memoryTypeBits >> i & 1) && (g_d->mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) ai.memoryTypeIndex = i;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(r->D, &ai, NULL, &r->mem) || vkBindImageMemory(r->D, r->img, r->mem, 0)) return 0;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, r->img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(r->D, &vi, NULL, &r->view)) return 0;
    if (hvk_buffer_make(4096, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, 1, &r->vb, &r->vbm, &r->vbp)) return 0;
    if (hvk_buffer_make((VkDeviceSize)W * H * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1, &r->rb, &r->rbm, &r->rbp)) return 0;
    memcpy((uint8_t *)r->vbp + 1024, idx, sizeof idx);
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (vkCreatePipelineLayout(r->D, &li, NULL, &r->pl)) return 0;
    r->vs = raw_module(r->D, HV_SPV(npw_vert_spv)); r->fs[0] = raw_module(r->D, HV_SPV(npw_frag_spv)); r->fs[1] = raw_module(r->D, HV_SPV(npw_s_frag_spv));
    if (!r->vs || !r->fs[0] || !r->fs[1]) return 0;
    VkCommandBuffer cb; if (!hvk_oneoff_begin(&cb)) return 0;
    VkImageMemoryBarrier2 ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                                r->img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &ib}; vkCmdPipelineBarrier2(cb, &di);
    return hvk_oneoff_end(cb);
}
static void raw_down(raw_t *r) {
    if (r->vs) vkDestroyShaderModule(r->D, r->vs, NULL);
    for (int i = 0; i < 2; i++) if (r->fs[i]) vkDestroyShaderModule(r->D, r->fs[i], NULL);
    if (r->pl) vkDestroyPipelineLayout(r->D, r->pl, NULL);
    hvk_buffer_free(r->vb, r->vbm); hvk_buffer_free(r->rb, r->rbm);
    if (r->view) vkDestroyImageView(r->D, r->view, NULL);
    if (r->img) vkDestroyImage(r->D, r->img, NULL);
    if (r->mem) vkFreeMemory(r->D, r->mem, NULL);
}
// A monolithic pipeline: attribute 0 RGBA32F from stream 0 (16 bytes), triangles, no culling, the viewport and scissor dynamic, one RGBA32F target with nothing blended;
// clamp: depthClampEnable; ndc: negativeOneToOne (VK_EXT_depth_clip_control); smooth: npw_s.frag
static VkPipeline raw_pipeline(const raw_t *r, int clamp, int ndc, int smooth) {
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, r->vs, "main", NULL},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, r->fs[smooth], "main", NULL}};
    VkVertexInputBindingDescription vb = {0, 16, VK_VERTEX_INPUT_RATE_VERTEX}; VkVertexInputAttributeDescription va = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 1, &vb, 1, &va};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkPipelineViewportDepthClipControlCreateInfoEXT dcc = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT, NULL, VK_TRUE};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, ndc ? &dcc : NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, clamp ? VK_TRUE : VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE,
                                                 VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {VK_FALSE, 0, 0, 0, 0, 0, 0, 0xf};
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &ba, {0}};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR}; VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkFormat cf = VK_FORMAT_R32G32B32A32_SFLOAT;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &cf, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, 2, st, &vi, &ia, NULL, &vp, &rs, &ms, NULL, &cbs, &dy, r->pl, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, -1};
    VkPipeline p = VK_NULL_HANDLE;
    return vkCreateGraphicsPipelines(r->D, VK_NULL_HANDLE, 1, &ci, NULL, &p) == VK_SUCCESS ? p : VK_NULL_HANDLE;
}
// Quad k drawn with p into the target cleared to -1, the viewport flipped (y at the bottom, negative height) or not, read back into px
static int raw_draw(raw_t *r, VkPipeline p, unsigned k, int flip, float *px) {
    memcpy(r->vbp, quads[k].v, sizeof quads[k].v);
    VkCommandBuffer cb; if (!hvk_oneoff_begin(&cb)) return 0;
    hvk_full_barrier(cb);
    VkRenderingAttachmentInfo a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, r->view, VK_IMAGE_LAYOUT_GENERAL, 0, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {.color = {{-1, -1, -1, -1}}}};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {W, H}}, 1, 0, 1, &a, NULL, NULL};
    vkCmdBeginRendering(cb, &ri);
    VkViewport v = flip ? (VkViewport){0, H, W, -(float)H, 0, 1} : (VkViewport){0, 0, W, H, 0, 1}; VkRect2D sc = {{0, 0}, {W, H}};
    vkCmdSetViewport(cb, 0, 1, &v); vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    VkDeviceSize off = 0; vkCmdBindVertexBuffers(cb, 0, 1, &r->vb, &off); vkCmdBindIndexBuffer(cb, r->vb, 1024, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cb, 6, 1, 0, 0, 0);
    vkCmdEndRendering(cb);
    hvk_full_barrier(cb);
    VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
    vkCmdCopyImageToBuffer(cb, r->img, VK_IMAGE_LAYOUT_GENERAL, r->rb, 1, &c);
    hvk_full_barrier(cb);
    if (!hvk_oneoff_end(cb)) return 0;
    memcpy(px, r->rbp, (size_t)W * H * 16);
    return 1;
}

static void rec_viewport(hv_cmds *c) {   // the whole target, depth 0..1
    p_viewports v; memset(&v, 0, sizeof v); v.first = 0; v.n = 1; v.v[0][2] = W; v.v[0][3] = H; v.v[0][5] = 1; hv_emit(c, OP_VIEWPORTS, &v, sizeof v);
}

int main(void) {
    const hostvk_dev *dev = hv_up("t_vk_noperspective");
    g_d = dev; g_lvp = dev->driver_id == VK_DRIVER_ID_MESA_LLVMPIPE;
    hv_device(0);
    float *px = malloc((size_t)W * H * 16); char name[400];
    if (!px) { t("readback buffer", 0); return hv_finish(); }
    int has_clamp = dev->enabled.depthClamp != 0, has_ndc = dev->x.depth_clip_control != 0;
    printf("device level %c, graphicsPipelineLibraryFastLinking %u, graphicsPipelineLibraryIndependentInterpolationDecoration %u; depthClamp %d, depthClipControl %d\n",
           dev->caps.device_level, dev->caps.gpl_fast_link, dev->caps.gpl_indep_interp, has_clamp, has_ndc);

    // The driver alone: every quad under each combination of depth clamp, negativeOneToOne and a flipped viewport the device has; the counts with the backend's settings kept
    pic alone[NQ]; int alone_ok[NQ]; memset(alone, 0, sizeof alone); memset(alone_ok, 0, sizeof alone_ok);
    raw_t r; int up = raw_up(&r);
    t("the driver alone: a target, buffers, modules and a layout of the test's own on the backend's device", up);
    for (int clamp = 0; up && clamp <= has_clamp; clamp++) for (int ndc = 0; ndc <= has_ndc; ndc++) {
        VkPipeline p[2] = {raw_pipeline(&r, clamp, ndc, 0), raw_pipeline(&r, clamp, ndc, 1)};
        snprintf(name, sizeof name, "the driver alone: pipelines with depth clamp %s, negativeOneToOne %s", clamp ? "on" : "off", ndc ? "on" : "off"); t(name, p[0] && p[1]);
        for (int flip = 0; p[0] && p[1] && flip < 2; flip++) for (unsigned k = 0; k < NQ; k++) for (int smooth = 0; smooth < 2; smooth++) {
            if (smooth && (clamp || ndc || flip)) continue;   // the control once, with nothing set
            char what[160]; snprintf(what, sizeof what, "the driver alone (depth clamp %s, negativeOneToOne %s, viewport %s)", clamp ? "on" : "off", ndc ? "on" : "off", flip ? "flipped" : "not flipped");
            int ok = raw_draw(&r, p[smooth], k, flip, px); pic m = measure(px);
            judge(what, k, smooth, ok, &m, px);
            if (!smooth && !clamp && ndc == has_ndc && !flip) { alone[k] = m; alone_ok[k] = ok; }   // the backend's settings: no clamp, NVN's GL depth mode where the device has it, lower-left origin
        }
        for (int i = 0; i < 2; i++) if (p[i]) vkDestroyPipeline(r.D, p[i], NULL);
    }
    raw_down(&r);

    // Through the backend: the same quads as NVN records
    NVNqueue q; NVNqueueBuilder qb; memset(&qb, 0, sizeof qb); h_nvnQueueBuilderSetDefaults(&qb); h_nvnQueueBuilderSetDevice(&qb, &g_dev); h_nvnQueueInitialize(&q, &qb);
    hv_pool p; if (!hv_pool_make(&p, 1 << 18)) { t("memory pool allocated", 0); free(px); return hv_finish(); }
    hv_cmds c = {p.mem + 65536, 0, 65536};
    NVNprogram prog;
    static const char vin[] = "[{\"location\":0,\"type\":\"float\"}]";
    const hv_shader sh[2] = {{"npw_vert", 0, HV_SPV(npw_vert_spv), NULL, vin}, {"npw_frag", 1, HV_SPV(npw_frag_spv), NULL, NULL}};
    hv_program(&prog, &p, 0, sh, 2);
    NVNprogram prog_s;   // the control: the same input interpolated with perspective correction
    const hv_shader shs[2] = {{"npw_vert", 0, HV_SPV(npw_vert_spv), NULL, vin}, {"npw_s_frag", 1, HV_SPV(npw_s_frag_spv), NULL, NULL}};
    hv_program(&prog_s, &p, 1024, shs, 2);
    tex_t target = hv_tex(NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA32F, W, H, 1, 1, 0);
    const tex_t *rt[1] = {&target};
    v4 *vb = (v4 *)(p.mem + 131072); uint16_t *ib = (uint16_t *)(p.mem + 131072 + 4096); memcpy(ib, idx, sizeof idx);
    struct { const char *name; char level; int opt, gpl; } modes[] = {{"fast-linked libraries (level A)", 'A', 0, 1}, {"the optimized relink (level A)", 'A', 1, 1}, {"a monolithic pipeline (level C)", 'C', 0, 0}};
    int gpl = dev->caps.device_level <= 'B' && dev->caps.gpl_fast_link;
    for (unsigned m = 0; m < sizeof modes / sizeof *modes; m++) {
        if (modes[m].gpl && !gpl) { printf("note: %s: the device has no pipeline libraries, not run\n", modes[m].name); continue; }
        hvk_pso_test_optimize(modes[m].opt); hvk_pso_test_level(modes[m].level);
        nvn_state_reset();
        for (unsigned kk = 0; kk < 2 * NQ; kk++) {
            int smooth = kk >= NQ; unsigned k = smooth ? kk - NQ : kk;
            memcpy(vb, quads[k].v, sizeof quads[k].v);
            for (int pass = 0; pass < (modes[m].opt ? 2 : 1); pass++) {   // the optimized pipeline is swapped in once the workers made it: drawn again then
                hv_rt(&c, 1, rt, NULL); rec_viewport(&c); hv_scissor(&c, 0, 0, W, H);
                hv_clear_color(&c, 0, 15, -1, -1, -1, -1);
                color_st cs; memset(&cs, 0, sizeof cs); cs.logic_op = NVN_LOGIC_OP_COPY; hv_state(&c, 1, &cs, sizeof cs);
                chmask_st cm = {0xffffffffu}; hv_state(&c, 2, &cm, sizeof cm);
                hv_bind(&c, smooth ? &prog_s : &prog, 3); hv_depth(&c, 0, 0, NVN_DEPTH_FUNC_ALWAYS); hv_cull(&c, NVN_FACE_NONE);
                va_st a[1]; memset(a, 0, sizeof a); a[0].fmt = NVN_FORMAT_RGBA32F; vs_st s[1] = {{16, 0}};
                hv_attrs(&c, 1, a); hv_streams(&c, 1, s); hv_vbuf(&c, 0, vb, sizeof quads[k].v);
                hv_draw_idx(&c, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT, 6, ib, 0, 1);
                hv_submit(&q, &c);
                if (modes[m].opt) hvk_work_drain(HVK_Q_N);
            }
            int ok = hv_read(&target, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 0, 0, W, H, 16, px); pic r2 = measure(px);
            judge(modes[m].name, k, smooth, ok, &r2, px);
            if (!smooth && up && alone_ok[k]) {   // nothing of the backend's own: the same texels not a number and wrong as the driver alone with its settings
                snprintf(name, sizeof name, "%s, %s: as the driver alone draws it with the backend's settings (%zu and %zu texels not a number, %zu and %zu wrong)", modes[m].name, quads[k].name,
                         r2.nan, alone[k].nan, r2.bad, alone[k].bad);
                t(name, ok && r2.nan == alone[k].nan && r2.bad == alone[k].bad);
            }
        }
    }
    free(px);
    hvk_pso_test_optimize(1); hvk_pso_test_level('A');
    hv_pool_free(&p);
    if (g_lvp) printf("lavapipe (%s): %d pictures of the clipped quads whose w differ wrong, %d right\n", dev->driver_info, g_lvp_wrong, g_lvp_right);
    return hv_finish();
}
