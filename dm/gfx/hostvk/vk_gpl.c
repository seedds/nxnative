// The Vulkan backend's graphics pipelines without stutter (docs/design/vulkan-backend.md 3.1, 3.2, 3.5, 3.6; phase P5). vk_pso.h says what the pieces share.
//
// Keys (3.1). A pipeline is keyed by its shaders, its layout and hvk_gfx_key: the attachment formats and sample count, the fragment entry point, the depth mode, the topology class,
// and only the state the device cannot set dynamically (the blend group, the raster group, the vertex input, the logic op, the patch size: the HOSTVK_EDS_* bits and dynamic vertex
// input it enabled). The class leaves the key where the device draws any class from one pipeline (dynamicPrimitiveTopologyUnrestricted), except patches, which tessellation needs.
//
// Two phases (3.2), levels A and B. A pipeline is the link of four libraries, each cached by a key of its own: vertex input (the topology class and, without dynamic vertex input, the
// bindings and attributes), pre-rasterization (the vertex, tessellation and geometry shaders, the layout, the depth mode and the raster state that is not dynamic), fragment shader
// (the fragment shader with its entry point and the layout; depth and stencil state are all dynamic in Vulkan 1.3) and fragment output (the formats, the samples and the blend state
// that is not dynamic). Libraries are made with RETAIN_LINK_TIME_OPTIMIZATION_INFO. A key first seen links them without link-time optimization (the fast link) and the draw uses
// that pipeline at once; the same libraries are linked again with LINK_TIME_OPTIMIZATION on the optimizing worker, and the next draw with the key binds the optimized pipeline
// (the fast one is released after the submissions that may use it). Both run the same SPIR-V with the same state: t_vk_gpl_identity compares their pixels with the monolithic
// pipelines' bit for bit; a device where they differ gets HVK_QUIRK_NO_OPT in the quirk table below (never a game profile key). The libraries are used only where the device
// reports fast linking and independent interpolation decorations: the vertex variants matched to their fragment partners' interpolation (3.2) are not built yet, so a device
// without the second gets the monolithic pipelines of level C, said once.
//
// Levels C and D (3.6). No libraries: a key first seen is compiled whole on the submit thread and the draw waits for it, counted; the prewarm, the prediction, the prebuild and
// the disk cache make those waits rare. D bakes the blend, raster and vertex input state into the key as the Metal key does. A draw is always made with its own shaders and its
// own state: nothing is skipped or drawn with a stand-in while a pipeline compiles.
//
// Waits. A draw whose pipeline is not ready waits for it at the bind: the time (plat_now_ns) per reason (a fast link, a library compile, a monolithic compile, a build a worker had
// under way), the count, the longest and those over the 2 ms budget of the P5 acceptance go into be_pso_line; DM_VK_PSO_TRACE=1 logs each one with its key.
//
// Threads (3.5): vk_pso.h. The QoS classes come from the platform layer (plat_qos_set_self): background builds at UTILITY, which is a below-normal priority on Linux and Windows;
// the idle class of 3.5 and starting the optimized relinks only while the frame step reports slack need the platform layer and the presenter (P4), and are declared.
#include "plat/plat.h"
#include "vk_pso.h"
#include "../nvn/nvn_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const VkShaderStageFlagBits k_vkstage[6] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                                   VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_COMPUTE_BIT};
const char *const hvk_wait_why[HVK_W_N] = {"fast link", "library compile", "monolithic compile", "build under way on a worker"};

// ---------- Blend state and formats ----------
static VkBlendFactor bf(uint32_t f) {
    switch (f) {
        case NVN_BLEND_FUNC_ZERO: return VK_BLEND_FACTOR_ZERO; case NVN_BLEND_FUNC_ONE: return VK_BLEND_FACTOR_ONE;
        case NVN_BLEND_FUNC_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case NVN_BLEND_FUNC_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case NVN_BLEND_FUNC_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case NVN_BLEND_FUNC_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case NVN_BLEND_FUNC_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case NVN_BLEND_FUNC_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case NVN_BLEND_FUNC_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        case NVN_BLEND_FUNC_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case NVN_BLEND_FUNC_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp bo(uint32_t e) {
    switch (e) { case NVN_BLEND_EQUATION_SUB: return VK_BLEND_OP_SUBTRACT; case NVN_BLEND_EQUATION_REVERSE_SUB: return VK_BLEND_OP_REVERSE_SUBTRACT;
                 case NVN_BLEND_EQUATION_MIN: return VK_BLEND_OP_MIN; case NVN_BLEND_EQUATION_MAX: return VK_BLEND_OP_MAX; default: return VK_BLEND_OP_ADD; }
}
// Whether attachments of format f can blend: the device's answer, kept per core format (the draw asks for every attachment of every draw)
static _Atomic uint8_t g_blendable[256];   // 0 not asked yet, 1 no, 2 yes
static int fmt_blendable(VkFormat f) {
    if ((uint32_t)f < 256) { uint8_t v = atomic_load(&g_blendable[f]); if (v) return v == 2; }
    VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(hvk.dev.phys, f, &p);
    int yes = (p.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0;
    if ((uint32_t)f < 256) atomic_store(&g_blendable[f], (uint8_t)(yes ? 2 : 1));
    return yes;
}
void hvk_blend_of(const blend_st *b, int on, VkFormat f, uint32_t wmask4, uint32_t *en, VkColorBlendEquationEXT *eq, VkColorComponentFlags *wm) {
    *en = on && f && fmt_blendable(f);
    if (b->adv && *en) HVK_RED_ONCE("advanced blend mode %u: not implemented (blended with the blend functions)", (unsigned)b->adv);
    *eq = (VkColorBlendEquationEXT){bf(b->src_rgb), bf(b->dst_rgb), bo(b->eq_rgb), bf(b->src_a), bf(b->dst_a), bo(b->eq_a)};
    if (bo(b->eq_rgb) == VK_BLEND_OP_MIN || bo(b->eq_rgb) == VK_BLEND_OP_MAX) { eq->srcColorBlendFactor = eq->dstColorBlendFactor = VK_BLEND_FACTOR_ONE; }
    if (bo(b->eq_a) == VK_BLEND_OP_MIN || bo(b->eq_a) == VK_BLEND_OP_MAX) { eq->srcAlphaBlendFactor = eq->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; }
    *wm = (VkColorComponentFlags)(wmask4 & 0xf);
}

// ---------- Vertex input ----------
void hvk_vertex_input(const hvk_shader *vs, const va_st attr[16], const vs_st stream[16], uint32_t bound, hvk_vin *v) {
    memset(v, 0, sizeof *v);
    if (!vs) return;
    uint32_t zero_binding = 16;
    for (uint32_t loc = 0; loc < 32; loc++) {
        if (!vs->vin_types[loc]) continue;
        uint32_t ct = vs->vin_types[loc] - 1;
        const va_st *at = loc < 16 ? &attr[loc] : NULL;
        nvn_vk_fmt row; VkFormat f = 0;
        if (at && at->fmt) { nvn_fmt_to_vk((int)at->fmt, &row); f = row.vk ? row.vk : row.alt; }
        if (!f || !(bound >> at->stream & 1)) {   // no format or no buffer: the attribute reads zero, as on an unbound Maxwell attribute
            f = ct == NVN_CT_UINT ? VK_FORMAT_R32G32B32A32_UINT : ct == NVN_CT_INT ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_SFLOAT;
            v->a[v->na++] = (VkVertexInputAttributeDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, NULL, loc, zero_binding, f, 0};
            v->zero = 1; continue;
        }
        uint8_t fc = row.cls; int fint = fc == NVN_VK_UINT ? NVN_CT_UINT : fc == NVN_VK_SINT ? NVN_CT_INT : NVN_CT_FLOAT;
        if (fint != (int)ct) HVK_RED_ONCE("vertex input %u read as %s from a %s format (NVN %s): the class mismatch needs the shader's shadow inputs (<key>_bv, phase P4); bound as it is",
                                          loc, ct == NVN_CT_FLOAT ? "float" : ct == NVN_CT_UINT ? "uint" : "int", fint == NVN_CT_FLOAT ? "float" : "integer", nvn_fmt_name((int)at->fmt));
        v->a[v->na++] = (VkVertexInputAttributeDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, NULL, loc, at->stream, f, at->offset};
        v->used_streams |= 1u << at->stream;
    }
    for (uint32_t s = 0; s < 16; s++) if (v->used_streams >> s & 1) {
        const vs_st *st = &stream[s];
        VkVertexInputBindingDescription2EXT b = {VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, NULL, s, st->stride, st->divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX, 1};
        if (st->divisor > 1) { if (hvk.dev.x.vertex_divisor) b.divisor = st->divisor; else HVK_RED_ONCE("vertex stream divisor %u without VK_EXT_vertex_attribute_divisor: read per instance", st->divisor); }
        v->b[v->nb++] = b;
    }
    if (v->zero) v->b[v->nb++] = (VkVertexInputBindingDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, NULL, zero_binding, 0, VK_VERTEX_INPUT_RATE_VERTEX, 1};
}

// ---------- Keys (3.1) ----------
void hvk_key_fill(hvk_gfx_key *k, hvk_shader *const sh[6], int klass, const hvk_key_src *s) {
    memset(k, 0, sizeof *k);
    int nc = s->nc > 8 ? 8 : s->nc;
    k->ccount = (uint8_t)nc; memcpy(k->color, s->cfmt, sizeof k->color); k->depth = s->dfmt; k->stencil = s->sfmt; k->samples = s->samples;
    // the class: dynamic within it, and left out where one pipeline draws every class; patches stay (a pipeline with tessellation draws patches only)
    int tess = sh[3] || sh[4];
    k->topology_class = (uint8_t)(tess ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : klass == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST || !hvk.dev.x.topology_unrestricted ? klass : HVK_TOPOLOGY_ANY);
    k->depth_clip_ndc = !atomic_load(&hvk.depth_zero) && hvk.dev.x.depth_clip_control;   // NVN's default depth mode: near = -w (GL), in fixed function (design 2.2)
    int all_uint = nc > 0, any_uint = 0;
    for (int i = 0; i < nc; i++) { if (!s->cfmt[i]) continue; int u = s->ccls[i] == NVN_VK_UINT || s->ccls[i] == NVN_VK_SINT; all_uint &= u; any_uint |= u; }
    if (sh[1] && any_uint) k->entry[0] = all_uint && sh[1]->entry_u[0] ? 1 : sh[1]->entry_m[0] ? 2 : sh[1]->entry_u[0] ? 1 : 0;
    uint32_t e = hvk.dev.eds_present;
    if ((e & HOSTVK_EDS_BLEND) != HOSTVK_EDS_BLEND) {
        k->baked |= HVK_BAKE_BLEND;
        for (int i = 0; i < nc; i++) { uint32_t en; VkColorBlendEquationEXT q; VkColorComponentFlags wm;
            hvk_blend_of(&s->blend[i], (int)(s->blend_en >> i & 1), s->cfmt[i], s->chmask >> (4 * i), &en, &q, &wm);
            if (!en) memset(&q, 0, sizeof q);   // a target that does not blend: its factors are not part of the pipeline (NVN's reset leaves ONE / ZERO there, a row zero)
            k->blend[i] = (VkPipelineColorBlendAttachmentState){en, q.srcColorBlendFactor, q.dstColorBlendFactor, q.colorBlendOp, q.srcAlphaBlendFactor, q.dstAlphaBlendFactor, q.alphaBlendOp, wm}; }
        k->logic_en = s->logic_op != NVN_LOGIC_OP_COPY;
    } else for (int i = 0; i < nc; i++) k->blend[i].colorWriteMask = 0xf;
    if (!(e >> HOSTVK_EDS_LOGIC_OP & 1)) { k->baked |= HVK_BAKE_LOGIC_OP; k->logic_op = (uint8_t)s->logic_op; }
    if ((e & HOSTVK_EDS_RASTER) != HOSTVK_EDS_RASTER) {
        k->baked |= HVK_BAKE_RASTER;
        k->poly_mode = s->poly_mode == NVN_POLYGON_MODE_POINT ? VK_POLYGON_MODE_POINT : s->poly_mode == NVN_POLYGON_MODE_LINE ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        k->depth_clamp = s->depth_clamp != 0; k->a2c = s->a2c != 0;
    }
    if ((k->topology_class == VK_PRIMITIVE_TOPOLOGY_POINT_LIST || k->topology_class == HVK_TOPOLOGY_ANY) && klass == VK_PRIMITIVE_TOPOLOGY_POINT_LIST && !hvk.dev.x.maintenance5)
        HVK_RED_ONCE("points on a device without VK_KHR_maintenance5: a vertex stage that writes no PointSize draws them at an undefined size (the Vulkan chain writes none; nvnCommandBufferSetPointSize is not recorded)");
    if (k->topology_class == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST) { k->patch_points = 3; HVK_RED_ONCE("patches drawn with 3 control points: nvnCommandBufferSetPatchSize is not recorded (nvn_pipe.h's TODO list)"); }
    if (!hvk.dev.caps.vertex_input_dynamic && s->vin) {
        const hvk_vin *v = s->vin;
        k->baked |= HVK_BAKE_VERTEX;
        for (uint32_t i = 0; i < v->nb; i++) { k->vb[i] = (VkVertexInputBindingDescription){v->b[i].binding, v->b[i].stride, v->b[i].inputRate};
            if (v->b[i].divisor > 1) k->vd[k->nvd++] = (VkVertexInputBindingDivisorDescriptionKHR){v->b[i].binding, v->b[i].divisor}; }
        for (uint32_t i = 0; i < v->na; i++) k->va[i] = (VkVertexInputAttributeDescription){v->a[i].location, v->a[i].binding, v->a[i].format, v->a[i].offset};
        k->nvb = (uint8_t)v->nb; k->nva = (uint8_t)v->na;
    }
}

// ---------- Dynamic states: a whole pipeline's, and each library's share of them ----------
// The colour blend states only with colour attachments (with none, vkCmdSetColorBlend*EXT cannot be called for any, and a pipeline that declares them dynamic fails every
// draw's validation); the vertex binding strides only with vertex bindings (vkCmdBindVertexBuffers2 sets strides only for bindings it binds, so a draw with none could never
// set them: VUID-vkCmdDraw-pStrides-04913 on every device without dynamic vertex input, found when levels C and D were first forced, phase P5)
uint32_t hvk_dyn_states(VkDynamicState *d, int vertex_dynamic, int colors, int bindings) {
    uint32_t n = 0;
    const VkDynamicState core[] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_DEPTH_BOUNDS};
    for (size_t i = 0; i < sizeof core / sizeof *core; i++) d[n++] = core[i];
    uint32_t e = hvk.dev.eds_present;
    if (vertex_dynamic) d[n++] = VK_DYNAMIC_STATE_VERTEX_INPUT_EXT; else if (bindings) d[n++] = VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE;
    if (e >> HOSTVK_EDS_LOGIC_OP & 1) d[n++] = VK_DYNAMIC_STATE_LOGIC_OP_EXT;
    if (e >> HOSTVK_EDS_PATCH_POINTS & 1) d[n++] = VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT;
    if (e >> HOSTVK_EDS_LOGIC_OP_ENABLE & 1) d[n++] = VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT;
    if (colors && (e >> HOSTVK_EDS_BLEND_ENABLE & 1)) d[n++] = VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT;
    if (colors && (e >> HOSTVK_EDS_BLEND_EQUATION & 1)) d[n++] = VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT;
    if (colors && (e >> HOSTVK_EDS_WRITE_MASK & 1)) d[n++] = VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT;
    if (e >> HOSTVK_EDS_DEPTH_CLAMP & 1) d[n++] = VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT;
    if (e >> HOSTVK_EDS_POLYGON_MODE & 1) d[n++] = VK_DYNAMIC_STATE_POLYGON_MODE_EXT;
    if (e >> HOSTVK_EDS_SAMPLES & 1) d[n++] = VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT;
    if (e >> HOSTVK_EDS_SAMPLE_MASK & 1) d[n++] = VK_DYNAMIC_STATE_SAMPLE_MASK_EXT;
    if (e >> HOSTVK_EDS_ALPHA_TO_COVERAGE & 1) d[n++] = VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT;
    return n;
}
enum { L_VI, L_PR, L_FS, L_FO, L_N };   // the four libraries of design 3.2
static const VkGraphicsPipelineLibraryFlagsEXT k_part[L_N] = {VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
                                                             VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT};
static const char *const k_part_name[L_N] = {"vertex input", "pre-rasterization", "fragment shader", "fragment output"};
static int dyn_part(VkDynamicState s) {   // which library a dynamic state belongs to (the specification's table of dynamic state per pipeline subset)
    switch (s) {
        case VK_DYNAMIC_STATE_VERTEX_INPUT_EXT: case VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE: case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY: case VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE:
            return L_VI;
        case VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT: case VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT: case VK_DYNAMIC_STATE_LINE_WIDTH: case VK_DYNAMIC_STATE_DEPTH_BIAS:
        case VK_DYNAMIC_STATE_CULL_MODE: case VK_DYNAMIC_STATE_FRONT_FACE: case VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE: case VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE:
        case VK_DYNAMIC_STATE_PATCH_CONTROL_POINTS_EXT: case VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT: case VK_DYNAMIC_STATE_POLYGON_MODE_EXT:
            return L_PR;
        case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK: case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK: case VK_DYNAMIC_STATE_STENCIL_REFERENCE: case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE:
        case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE: case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP: case VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE: case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE:
        case VK_DYNAMIC_STATE_STENCIL_OP: case VK_DYNAMIC_STATE_DEPTH_BOUNDS:
            return L_FS;
        default: return L_FO;   // blend constants, logic op and its enable, the colour blend states, samples, sample mask, alpha to coverage
    }
}
static uint32_t dyn_states_part(VkDynamicState *d, int part, int vertex_dynamic, int colors, int bindings) {
    VkDynamicState all[48]; uint32_t n = hvk_dyn_states(all, vertex_dynamic, colors, bindings), m = 0;
    for (uint32_t i = 0; i < n; i++) if (dyn_part(all[i]) == part) d[m++] = all[i];
    return m;
}

// ---------- The level (3.6), the quirk table (3.2, risk 7) ----------
static char g_level; static int g_gpl, g_opt_on = 1, g_trace;
static uint32_t g_quirks; static const char *g_quirk_why;
static hostvk_caps g_full_caps; static uint32_t g_full_eds, g_full_topo; static int g_full_saved;
// A row per vendor and driver where the pipelines of the two phases or of the levels are found to give different pixels (t_vk_gpl_identity on that device). Matched on the vendor
// ID and the VkDriverId (0: any) and the driver version range [lo, hi] as the device reports it (0, 0: any). None is known: on Mesa's lavapipe 25.2.8 (VK_DRIVER_ID_MESA_LLVMPIPE)
// the fast-linked, optimized and monolithic pipelines of levels A to D gave the same bits for every draw of t_vk_gpl_identity. A new row says how it was found
static const struct { uint32_t vendor, driver_id, ver_lo, ver_hi, flags; const char *why; } k_quirks[] = {
    {0, 0, 0, 0, 0, NULL},
};
uint32_t hvk_quirks(const char **why) { if (why) *why = g_quirk_why; return g_quirks; }
static void quirks_find(void) {
    g_quirks = 0; g_quirk_why = NULL;
    for (size_t i = 0; i < sizeof k_quirks / sizeof *k_quirks; i++) {
        if (!k_quirks[i].flags) continue;
        if (k_quirks[i].vendor && k_quirks[i].vendor != hvk.dev.props.vendorID) continue;
        if (k_quirks[i].driver_id && k_quirks[i].driver_id != hvk.dev.driver_id) continue;
        if ((k_quirks[i].ver_lo || k_quirks[i].ver_hi) && (hvk.dev.props.driverVersion < k_quirks[i].ver_lo || hvk.dev.props.driverVersion > k_quirks[i].ver_hi)) continue;
        g_quirks |= k_quirks[i].flags; g_quirk_why = k_quirks[i].why;
    }
    if (g_quirks & HVK_QUIRK_NO_OPT) hostvk_log("[hostvk] pipeline quirk of this driver: optimized relinks are not used (%s)", g_quirk_why ? g_quirk_why : "-");
}
static void level_rules(void) {
    g_level = hvk.dev.caps.level; g_gpl = hvk.dev.caps.gpl;
    if (g_gpl && (!hvk.dev.caps.gpl_fast_link || !hvk.dev.caps.gpl_indep_interp)) {
        hostvk_log("[RED] [hostvk] pipeline libraries present but not used: %s%s%s (interpolation-matched vertex variants, design 3.2, are not built): monolithic pipelines with level C's waits",
                   hvk.dev.caps.gpl_fast_link ? "" : "no graphicsPipelineLibraryFastLinking", !hvk.dev.caps.gpl_fast_link && !hvk.dev.caps.gpl_indep_interp ? ", " : "",
                   hvk.dev.caps.gpl_indep_interp ? "" : "no graphicsPipelineLibraryIndependentInterpolationDecoration");
        g_gpl = 0; g_level = 'C';
    }
}
char hvk_pso_level(void) { return g_level; }
int hvk_pso_gpl(void) { return g_gpl; }
void hvk_pso_test_optimize(int on) { g_opt_on = on != 0; }

// ---------- Libraries and pipelines ----------
typedef struct { uint8_t cls, baked; uint8_t nvb, nva, nvd, pad[3]; VkVertexInputBindingDescription vb[16]; VkVertexInputAttributeDescription va[32]; VkVertexInputBindingDivisorDescriptionKHR vd[16]; } vi_key;
typedef struct { const void *sh[4]; const void *layout; uint32_t patch_points; uint8_t depth_clip_ndc, poly_mode, depth_clamp, baked; } pr_key;
typedef struct { const void *fs; const void *layout; uint8_t entry, pad[7]; } fs_key;
typedef struct { VkFormat color[8], depth, stencil; uint32_t samples, baked; uint8_t ccount, logic_en, logic_op, a2c; VkPipelineColorBlendAttachmentState blend[8]; } fo_key;
typedef struct { _Atomic int state; VkPipeline p; } lib_ent;   // state: 0 being made, 1 made, 2 failed
typedef struct { const void *sh[5]; const void *layout; hvk_gfx_key k; } gpl_key;   // a whole pipeline: vertex, tess control, tess evaluation, geometry, fragment
enum { K_FAST, K_OPT, K_MONO };
typedef struct pso_ent {
    _Atomic int state;           // 0 being made, 1 ready, 2 failed
    VkPipeline cur;              // what draws bind (written by the maker before state 1, then by the submit thread when it swaps)
    _Atomic(VkPipeline) opt;     // the optimized relink, waiting to be swapped in
    uint8_t kind, used;          // K_*; the submit thread has drawn with it (the prediction's first use)
    int row;                     // the list row that named it (-1: none)
    lib_ent *lib[L_N];
    hvk_shader *sh[6]; hvk_layout *l; hvk_gfx_key k;
} pso_ent;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;   // the tables below; g_cv: something being made is made
static nvn_hmap g_lib[L_N], g_ents;
static _Atomic uint64_t s_lib[L_N], s_lib_ns[L_N], s_fast, s_fast_ns, s_opt, s_opt_ns, s_mono, s_mono_ns, s_swapped, s_opt_dropped, s_draw_made, s_failed;
static _Atomic uint64_t s_wait_n[HVK_W_N], s_wait_ns[HVK_W_N], s_wait_max, s_wait_max_win, s_over, s_draws_kind[3], s_pending;
static void stat_max(_Atomic uint64_t *m, uint64_t v) { uint64_t o = atomic_load(m); while (v > o && !atomic_compare_exchange_weak(m, &o, v)) {} }
static void key_of(gpl_key *key, hvk_shader *const sh[6], hvk_layout *l, const hvk_gfx_key *k) {
    memset(key, 0, sizeof *key);
    static const int order[5] = {0, 3, 4, 2, 1};
    for (int i = 0; i < 5; i++) key->sh[i] = sh[order[i]];
    key->layout = l; key->k = *k;
}
static const char *entry_of(const hvk_shader *s, int which) { return which == 1 && s->entry_u[0] ? s->entry_u : which == 2 && s->entry_m[0] ? s->entry_m : s->entry; }
// A creation through the calling thread's disk cache, with its feedback; t: time
static VkResult create(const VkGraphicsPipelineCreateInfo *ci0, VkPipeline *p, uint64_t *t) {
    VkGraphicsPipelineCreateInfo ci = *ci0;
    VkPipelineCreationFeedback fb = {0};
    VkPipelineCreationFeedbackCreateInfo fci = {VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, (void *)ci.pNext, &fb, 0, NULL};
    ci.pNext = &fci;
    uint64_t t0 = plat_now_ns();
    VkPipelineCache pc = hvk_cache_begin();
    VkResult r = vkCreateGraphicsPipelines(hvk.d, pc, 1, &ci, NULL, p);
    hvk_cache_end(&fb, r == VK_SUCCESS);
    *t = plat_now_ns() - t0;
    if (r) *p = VK_NULL_HANDLE;
    return r;
}
static VkPipeline lib_make(int part, const pso_ent *e) {
    const hvk_gfx_key *k = &e->k; hvk_shader *const *sh = e->sh;
    int vdyn = hvk.dev.caps.vertex_input_dynamic;
    VkGraphicsPipelineLibraryCreateInfoEXT gl = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, NULL, k_part[part]};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, &gl, 0, 0, NULL, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT};
    VkDynamicState dsv[48]; VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, dyn_states_part(dsv, part, vdyn, k->ccount > 0, k->nvb > 0), dsv};
    ci.pDynamicState = &dy;
    VkPipelineVertexInputDivisorStateCreateInfoKHR vds = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR, NULL, k->nvd, k->vd};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, !vdyn && k->nvd ? &vds : NULL, 0, vdyn ? 0 : k->nvb, k->vb, vdyn ? 0 : k->nva, k->va};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
        k->topology_class == HVK_TOPOLOGY_ANY ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : (VkPrimitiveTopology)k->topology_class, VK_FALSE};
    VkPipelineShaderStageCreateInfo stg[4]; uint32_t ns = 0;
    VkPipelineTessellationStateCreateInfo ts = {VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO, NULL, 0, k->patch_points ? k->patch_points : 1};
    VkPipelineViewportDepthClipControlCreateInfoEXT dcc = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT, NULL, k->depth_clip_ndc};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, hvk.dev.x.depth_clip_control ? &dcc : NULL, 0, 0, NULL, 0, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, k->depth_clamp, VK_FALSE, (VkPolygonMode)k->poly_mode, VK_CULL_MODE_NONE,
                                                 VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, k->samples ? k->samples : VK_SAMPLE_COUNT_1_BIT, VK_FALSE, 0, NULL, k->a2c, VK_FALSE};
    VkPipelineColorBlendAttachmentState ba[8]; for (int i = 0; i < 8; i++) ba[i] = k->blend[i];
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, k->logic_en, (VkLogicOp)k->logic_op, k->ccount, ba, {0, 0, 0, 0}};
    switch (part) {
        case L_VI: ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia; break;
        case L_PR: {
            static const int order[4] = {0, 3, 4, 2};
            for (int i = 0; i < 4; i++) { const hvk_shader *s = sh[order[i]]; if (!s) continue;
                stg[ns++] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, k_vkstage[order[i]], s->module, s->entry, NULL}; }
            ci.stageCount = ns; ci.pStages = stg; ci.pViewportState = &vp; ci.pRasterizationState = &rs; ci.layout = hvk_layout_vk(e->l);
            if (sh[3] || sh[4]) ci.pTessellationState = &ts;
            break;
        }
        case L_FS:
            if (sh[1]) { stg[ns++] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, sh[1]->module, entry_of(sh[1], k->entry[0]), NULL}; }
            ci.stageCount = ns; ci.pStages = ns ? stg : NULL; ci.pDepthStencilState = &ds; ci.layout = hvk_layout_vk(e->l);
            break;
        case L_FO:
            ri.colorAttachmentCount = k->ccount; ri.pColorAttachmentFormats = k->color; ri.depthAttachmentFormat = k->depth; ri.stencilAttachmentFormat = k->stencil;
            ci.pColorBlendState = &cbs; ci.pMultisampleState = &ms;
            break;
    }
    VkPipeline p; uint64_t t;
    VkResult r = create(&ci, &p, &t);
    if (r) { hostvk_log("[RED] [hostvk] %s library (vertex %s fragment %s) failed: VkResult %d", k_part_name[part], sh[0] ? sh[0]->key : "-", sh[1] ? sh[1]->key : "-", r); return VK_NULL_HANDLE; }
    atomic_fetch_add(&s_lib[part], 1); atomic_fetch_add(&s_lib_ns[part], t);
    return p;
}
// The library of part for e: made here when no thread has it (*made), waited for when one is making it (*waited)
static lib_ent *lib_get(int part, const pso_ent *e, int *made, int *waited) {
    union { vi_key vi; pr_key pr; fs_key fs; fo_key fo; } u; memset(&u, 0, sizeof u);
    const hvk_gfx_key *k = &e->k; size_t klen = 0;
    switch (part) {
        case L_VI: u.vi.cls = k->topology_class; u.vi.baked = (uint8_t)(k->baked & HVK_BAKE_VERTEX);
            if (k->baked & HVK_BAKE_VERTEX) { u.vi.nvb = k->nvb; u.vi.nva = k->nva; u.vi.nvd = k->nvd; memcpy(u.vi.vb, k->vb, sizeof k->vb); memcpy(u.vi.va, k->va, sizeof k->va); memcpy(u.vi.vd, k->vd, sizeof k->vd); }
            klen = sizeof u.vi; break;
        case L_PR: u.pr.sh[0] = e->sh[0]; u.pr.sh[1] = e->sh[3]; u.pr.sh[2] = e->sh[4]; u.pr.sh[3] = e->sh[2]; u.pr.layout = e->l; u.pr.patch_points = k->patch_points;
            u.pr.depth_clip_ndc = k->depth_clip_ndc; u.pr.poly_mode = k->poly_mode; u.pr.depth_clamp = k->depth_clamp; u.pr.baked = (uint8_t)(k->baked & (HVK_BAKE_RASTER | HVK_BAKE_PATCH));
            klen = sizeof u.pr; break;
        case L_FS: u.fs.fs = e->sh[1]; u.fs.layout = e->l; u.fs.entry = k->entry[0]; klen = sizeof u.fs; break;
        case L_FO: memcpy(u.fo.color, k->color, sizeof k->color); u.fo.depth = k->depth; u.fo.stencil = k->stencil; u.fo.samples = k->samples; u.fo.ccount = k->ccount;
            u.fo.baked = k->baked & (HVK_BAKE_BLEND | HVK_BAKE_LOGIC_OP | HVK_BAKE_RASTER); u.fo.logic_en = k->logic_en; u.fo.logic_op = k->logic_op; u.fo.a2c = k->a2c;
            memcpy(u.fo.blend, k->blend, sizeof k->blend); klen = sizeof u.fo; break;
    }
    pthread_mutex_lock(&g_mu);
    if (!g_lib[part].klen) nvn_hm_init(&g_lib[part], (uint32_t)klen);
    lib_ent *l = nvn_hm_get(&g_lib[part], &u);
    if (!l) {
        if (!(l = calloc(1, sizeof *l))) { pthread_mutex_unlock(&g_mu); return NULL; }
        nvn_hm_put(&g_lib[part], &u, l);
        pthread_mutex_unlock(&g_mu);
        VkPipeline p = lib_make(part, e);
        pthread_mutex_lock(&g_mu);
        l->p = p; atomic_store(&l->state, p ? 1 : 2); pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
        *made = 1;
        return l;
    }
    while (atomic_load(&l->state) == 0) { *waited = 1; pthread_cond_wait(&g_cv, &g_mu); }
    pthread_mutex_unlock(&g_mu);
    return l;
}
static VkPipeline link(const pso_ent *e, int optimize, uint64_t *t) {
    VkPipeline libs[L_N]; for (int i = 0; i < L_N; i++) libs[i] = e->lib[i]->p;
    VkPipelineLibraryCreateInfoKHR li = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, NULL, L_N, libs};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &li, optimize ? VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT : 0};
    ci.layout = hvk_layout_vk(e->l); ci.basePipelineIndex = -1;
    VkPipeline p; VkResult r = create(&ci, &p, t);
    if (r) { hostvk_log("[RED] [hostvk] %s link (vertex %s fragment %s) failed: VkResult %d", optimize ? "optimized" : "fast", e->sh[0] ? e->sh[0]->key : "-", e->sh[1] ? e->sh[1]->key : "-", r); return VK_NULL_HANDLE; }
    return p;
}
static VkPipeline mono_make(const pso_ent *e, uint64_t *t) {   // levels C and D: the pipeline whole (what phase P3 made for every draw)
    const hvk_gfx_key *k = &e->k; hvk_shader *const *sh = e->sh;
    static const int order[5] = {0, 3, 4, 2, 1};
    VkPipelineShaderStageCreateInfo stg[5]; uint32_t ns = 0;
    for (int i = 0; i < 5; i++) { const hvk_shader *s = sh[order[i]]; if (!s) continue;
        stg[ns++] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, k_vkstage[order[i]], s->module, order[i] == 1 ? entry_of(s, k->entry[0]) : s->entry, NULL}; }
    int vdyn = hvk.dev.caps.vertex_input_dynamic;
    VkPipelineVertexInputDivisorStateCreateInfoKHR vds = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR, NULL, k->nvd, k->vd};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, !vdyn && k->nvd ? &vds : NULL, 0, vdyn ? 0 : k->nvb, k->vb, vdyn ? 0 : k->nva, k->va};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
        k->topology_class == HVK_TOPOLOGY_ANY ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : (VkPrimitiveTopology)k->topology_class, VK_FALSE};
    VkPipelineTessellationStateCreateInfo ts = {VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO, NULL, 0, k->patch_points ? k->patch_points : 1};
    VkPipelineViewportDepthClipControlCreateInfoEXT dcc = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT, NULL, k->depth_clip_ndc};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, hvk.dev.x.depth_clip_control ? &dcc : NULL, 0, 0, NULL, 0, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, k->depth_clamp, VK_FALSE, (VkPolygonMode)k->poly_mode, VK_CULL_MODE_NONE,
                                                 VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, k->samples ? k->samples : VK_SAMPLE_COUNT_1_BIT, VK_FALSE, 0, NULL, k->a2c, VK_FALSE};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState ba[8]; for (int i = 0; i < 8; i++) ba[i] = k->blend[i];
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, k->logic_en, (VkLogicOp)k->logic_op, k->ccount, ba, {0, 0, 0, 0}};
    VkDynamicState dsv[48]; uint32_t nd = hvk_dyn_states(dsv, vdyn, k->ccount > 0, k->nvb > 0);
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, nd, dsv};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, k->ccount, k->color, k->depth, k->stencil};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, ns, stg, &vi, &ia, (sh[3] || sh[4]) ? &ts : NULL, &vp, &rs, &ms, &ds, &cbs, &dy,
                                       hvk_layout_vk(e->l), VK_NULL_HANDLE, 0, VK_NULL_HANDLE, -1};
    VkPipeline p; VkResult r = create(&ci, &p, t);
    if (r) { hostvk_log("[RED] [hostvk] graphics pipeline (vertex %s fragment %s) failed: VkResult %d", sh[0] ? sh[0]->key : "-", sh[1] ? sh[1]->key : "-", r); return VK_NULL_HANDLE; }
    return p;
}

// ---------- Background optimization (3.2): the optimizing worker relinks with link-time optimization ----------
static void opt_job(void *arg) {
    pso_ent *e = arg;
    if (!g_opt_on || (g_quirks & HVK_QUIRK_NO_OPT) || atomic_load(&e->state) != 1 || e->kind != K_FAST) { atomic_fetch_add(&s_opt_dropped, 1); return; }
    uint64_t t; VkPipeline p = link(e, 1, &t);
    if (!p) { atomic_fetch_add(&s_opt_dropped, 1); return; }
    atomic_fetch_add(&s_opt, 1); atomic_fetch_add(&s_opt_ns, t);
    atomic_store(&e->opt, p);
}
// Make e on the calling thread: its libraries (made or waited for) and a fast link, or the monolithic pipeline. *why: what the wait was (for a draw)
static void ent_make(pso_ent *e, int *why) {
    VkPipeline p = VK_NULL_HANDLE; uint64_t t = 0; int made = 0, waited = 0;
    atomic_fetch_add(&s_pending, 1);
    if (g_gpl) {
        int ok = 1;
        for (int i = 0; i < L_N; i++) { e->lib[i] = lib_get(i, e, &made, &waited); ok &= e->lib[i] && atomic_load(&e->lib[i]->state) == 1; }
        if (ok && (p = link(e, 0, &t))) { atomic_fetch_add(&s_fast, 1); atomic_fetch_add(&s_fast_ns, t); }
        e->kind = K_FAST;
        *why = made ? HVK_W_LIB : waited ? HVK_W_INFLIGHT : HVK_W_LINK;
    } else {
        if ((p = mono_make(e, &t))) { atomic_fetch_add(&s_mono, 1); atomic_fetch_add(&s_mono_ns, t); }
        e->kind = K_MONO; *why = HVK_W_MONO;
    }
    if (!p) atomic_fetch_add(&s_failed, 1);
    pthread_mutex_lock(&g_mu);
    e->cur = p; atomic_store(&e->state, p ? 1 : 2); pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    atomic_fetch_sub(&s_pending, 1);
    if (p && e->kind == K_FAST && g_opt_on && !(g_quirks & HVK_QUIRK_NO_OPT)) hvk_work_submit(HVK_Q_OPT, opt_job, e);
}
static pso_ent *ent_new(hvk_shader *const sh[6], hvk_layout *l, const hvk_gfx_key *k, int row) {
    pso_ent *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    memcpy(e->sh, sh, sizeof e->sh); e->l = l; e->k = *k; e->row = row;
    return e;
}
static void trace_wait(int why, uint64_t ns, const pso_ent *e) {
    char f[160] = ""; size_t o = 0;
    for (int i = 0; i < e->k.ccount && o < sizeof f; i++) o += (size_t)snprintf(f + o, sizeof f - o, "%s%u", i ? "," : "", (unsigned)e->k.color[i]);
    hostvk_log("[hostvk] pipeline wait %.3f ms (%s): vertex %s fragment %s, colour formats %s, depth %u stencil %u, %u samples, topology class %u, entry %u, level %c",
               ns / 1e6, hvk_wait_why[why], e->sh[0] ? e->sh[0]->key : "-", e->sh[1] ? e->sh[1]->key : "-", o ? f : "-", (unsigned)e->k.depth, (unsigned)e->k.stencil,
               (unsigned)e->k.samples, (unsigned)e->k.topology_class, (unsigned)e->k.entry[0], g_level);
}
VkPipeline hvk_gfx_pipeline(hvk_shader *const sh[6], hvk_layout *l, const hvk_gfx_key *k, void (*miss)(void)) {
    gpl_key key; key_of(&key, sh, l, k);
    pthread_mutex_lock(&g_mu);
    if (!g_ents.klen) nvn_hm_init(&g_ents, sizeof key);
    pso_ent *e = nvn_hm_get(&g_ents, &key);
    if (!e || atomic_load(&e->state) == 0) {   // not ready: the draw waits for it, made here or by the worker that has it
        uint64_t t0 = plat_now_ns(); int why = HVK_W_INFLIGHT, fresh = 0;
        if (!e) {
            if (!(e = ent_new(sh, l, k, -1))) { pthread_mutex_unlock(&g_mu); return VK_NULL_HANDLE; }
            nvn_hm_put(&g_ents, &key, e); fresh = 1;
            pthread_mutex_unlock(&g_mu);
            ent_make(e, &why);
            atomic_fetch_add(&s_draw_made, 1); atomic_fetch_add(&hvk.st_pipelines, 1);
        } else {
            while (atomic_load(&e->state) == 0) pthread_cond_wait(&g_cv, &g_mu);
            pthread_mutex_unlock(&g_mu);
        }
        uint64_t w = plat_now_ns() - t0;
        atomic_fetch_add(&s_wait_n[why], 1); atomic_fetch_add(&s_wait_ns[why], w); stat_max(&s_wait_max, w); stat_max(&s_wait_max_win, w); atomic_fetch_add(&hvk.st_pipeline_ns, w);
        if (w > 2000000) atomic_fetch_add(&s_over, 1);
        if (g_trace) trace_wait(why, w, e);
        if (fresh) { hvk_prewarm_predict_pair(sh[0] ? sh[0]->key : NULL, sh[1] ? sh[1]->key : NULL); if (miss) miss(); }
    } else pthread_mutex_unlock(&g_mu);
    if (atomic_load(&e->state) != 1) { HVK_RED_ONCE("a draw whose pipeline could not be made is skipped (first case: vertex %s fragment %s)", sh[0] ? sh[0]->key : "-", sh[1] ? sh[1]->key : "-"); return VK_NULL_HANDLE; }
    VkPipeline o = atomic_load(&e->opt);
    if (o && e->kind == K_FAST) {   // the optimized pipeline is ready: swapped in, the fast one released after the submissions that may still use it
        hvk_defer(HVK_DEL_PIPELINE, (void *)e->cur, 0, 0);
        e->cur = o; e->kind = K_OPT; atomic_store(&e->opt, VK_NULL_HANDLE);
        atomic_fetch_add(&s_swapped, 1);
    }
    if (!e->used) { e->used = 1; if (e->row >= 0) hvk_prewarm_predict(e->row); }
    atomic_fetch_add(&s_draws_kind[e->kind], 1);
    return e->cur;
}
int hvk_pso_build(hvk_shader *const sh[6], hvk_layout *l, const hvk_gfx_key *k, int row) {
    gpl_key key; key_of(&key, sh, l, k);
    pthread_mutex_lock(&g_mu);
    if (!g_ents.klen) nvn_hm_init(&g_ents, sizeof key);
    pso_ent *e = nvn_hm_get(&g_ents, &key);
    if (e) { if (e->row < 0) e->row = row; int st = atomic_load(&e->state); pthread_mutex_unlock(&g_mu); return st != 2; }
    if (!(e = ent_new(sh, l, k, row))) { pthread_mutex_unlock(&g_mu); return 0; }
    nvn_hm_put(&g_ents, &key, e);
    pthread_mutex_unlock(&g_mu);
    int why; ent_make(e, &why);
    return atomic_load(&e->state) == 1;
}
int hvk_pso_ready(hvk_shader *const sh[6], hvk_layout *l, const hvk_gfx_key *k) {
    gpl_key key; key_of(&key, sh, l, k);
    pthread_mutex_lock(&g_mu);
    pso_ent *e = g_ents.klen ? nvn_hm_get(&g_ents, &key) : NULL;
    int ok = e && atomic_load(&e->state) == 1;
    pthread_mutex_unlock(&g_mu);
    return ok;
}
int hvk_pso_build_shader(hvk_shader *s) {
    if (!g_gpl || !s || (s->stage != 0 && s->stage != 1)) return 0;
    hvk_shader *sh[6] = {0}; sh[s->stage] = s;
    hvk_layout *l = hvk_layout_for(sh);
    if (!l) return 0;
    pso_ent e; memset(&e, 0, sizeof e); memcpy(e.sh, sh, sizeof e.sh); e.l = l;
    hvk_key_src src; memset(&src, 0, sizeof src); src.samples = VK_SAMPLE_COUNT_1_BIT; src.poly_mode = NVN_POLYGON_MODE_FILL; src.logic_op = NVN_LOGIC_OP_COPY;
    hvk_key_fill(&e.k, sh, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, &src);   // the libraries' share of the key does not depend on the pair (the layout is the stage combination's)
    int made = 0, waited = 0;
    lib_ent *le = lib_get(s->stage == 0 ? L_PR : L_FS, &e, &made, &waited);
    return le && atomic_load(&le->state) == 1;
}
uint64_t hvk_pso_take_window_max(void) { return atomic_exchange(&s_wait_max_win, 0); }
void hvk_pso_stats_get(hvk_pso_stats *s) {
    memset(s, 0, sizeof *s);
    for (int i = 0; i < L_N; i++) { s->lib[i] = s_lib[i]; s->lib_ns[i] = s_lib_ns[i]; }
    s->fast = s_fast; s->fast_ns = s_fast_ns; s->opt = s_opt; s->opt_ns = s_opt_ns; s->mono = s_mono; s->mono_ns = s_mono_ns; s->swapped = s_swapped; s->opt_dropped = s_opt_dropped;
    s->draw_made = s_draw_made; s->draw_ns = hvk.st_pipeline_ns;
    for (int i = 0; i < HVK_W_N; i++) { s->wait_n[i] = s_wait_n[i]; s->wait_ns[i] = s_wait_ns[i]; }
    s->wait_max_ns = s_wait_max; s->over_budget = s_over;
    s->draws_fast = s_draws_kind[K_FAST]; s->draws_opt = s_draws_kind[K_OPT]; s->draws_mono = s_draws_kind[K_MONO]; s->failed = s_failed; s->pending = s_pending;
}

// ---------- Worker threads (3.5) ----------
typedef struct job { void (*fn)(void *); void *arg; struct job *next; } job;
static struct { job *head, *tail; int n, running; } g_q[HVK_Q_N];
static pthread_mutex_t g_wmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_wcv = PTHREAD_COND_INITIALIZER, g_wdone = PTHREAD_COND_INITIALIZER;
enum { MAXW = 64 };
static pthread_t g_bg[2], g_optw, g_prep[MAXW]; static int g_nbg, g_nprep, g_started, g_wquit, g_prep_stop, g_opt_started;
static int bg_ready(void) { return g_q[HVK_Q_SPEC].n || g_q[HVK_Q_BG].n; }
static job *take(int q) { job *j = g_q[q].head; if (j) { g_q[q].head = j->next; if (!g_q[q].head) g_q[q].tail = NULL; g_q[q].n--; g_q[q].running++; } return j; }
static void run(int q, job *j) {
    j->fn(j->arg); free(j);
    pthread_mutex_lock(&g_wmu); g_q[q].running--; pthread_cond_broadcast(&g_wdone); pthread_mutex_unlock(&g_wmu);
}
static void *bg_main(void *arg) {   // the prewarm and the speculative builds; a prepare worker too (arg non-NULL), until the prepare step ends
    int prep = arg != NULL;
    plat_thread_set_name(prep ? "dm-vk-prepare" : "dm-vk-pso");
    plat_qos_set_self(prep ? PLAT_QOS_USER_INITIATED : PLAT_QOS_UTILITY);
    pthread_mutex_lock(&g_wmu);
    for (;;) {
        if (g_wquit || (prep && g_prep_stop)) break;
        int q = g_q[HVK_Q_SPEC].n ? HVK_Q_SPEC : g_q[HVK_Q_BG].n ? HVK_Q_BG : -1;
        if (q < 0) { pthread_cond_wait(&g_wcv, &g_wmu); continue; }
        job *j = take(q);
        pthread_mutex_unlock(&g_wmu);
        run(q, j);
        pthread_mutex_lock(&g_wmu);
    }
    pthread_mutex_unlock(&g_wmu);
    if (prep) hvk_cache_retire_thread();
    return NULL;
}
static void *opt_main(void *arg) {   // the optimized relinks, only while the other queues are empty, and the disk cache's periodic write
    (void)arg;
    plat_thread_set_name("dm-vk-opt");
    plat_qos_set_self(PLAT_QOS_UTILITY);
    pthread_mutex_lock(&g_wmu);
    for (;;) {
        if (g_wquit) break;
        int idle = !bg_ready() && !g_q[HVK_Q_SPEC].running && !g_q[HVK_Q_BG].running && !atomic_load(&s_pending);
        if (g_q[HVK_Q_OPT].n && idle) {
            job *j = take(HVK_Q_OPT);
            pthread_mutex_unlock(&g_wmu);
            run(HVK_Q_OPT, j);
            pthread_mutex_lock(&g_wmu);
            continue;
        }
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 1;
        pthread_cond_timedwait(&g_wcv, &g_wmu, &ts);
        if (g_wquit) break;
        pthread_mutex_unlock(&g_wmu);
        hvk_cache_tick(plat_now_ns());
        pthread_mutex_lock(&g_wmu);
    }
    pthread_mutex_unlock(&g_wmu);
    return NULL;
}
static void workers_start(void) {   // under g_wmu
    if (g_started) return;
    g_started = 1; g_wquit = 0;
    int cpu = plat_cpu_count(); g_nbg = cpu <= 4 ? 1 : 2;   // the low-core rule of 3.5: one background worker on a CPU of up to 4 hardware threads
    for (int i = 0; i < g_nbg; i++) if (pthread_create(&g_bg[i], NULL, bg_main, NULL)) { g_nbg = i; break; }
    g_opt_started = !pthread_create(&g_optw, NULL, opt_main, NULL);
    if (!g_opt_started) hostvk_log("[RED] [hostvk] the optimizing worker could not be started: no optimized relinks, no periodic cache write");
    hostvk_log("[hostvk] pipeline workers: %d background, 1 optimizing (%d hardware threads)", g_nbg, cpu);
}
void hvk_work_submit(int q, void (*fn)(void *), void *arg) {
    job *j = calloc(1, sizeof *j);
    if (!j) { fn(arg); return; }
    j->fn = fn; j->arg = arg;
    pthread_mutex_lock(&g_wmu);
    workers_start();
    if (g_q[q].tail) g_q[q].tail->next = j; else g_q[q].head = j;
    g_q[q].tail = j; g_q[q].n++;
    pthread_cond_broadcast(&g_wcv);
    pthread_mutex_unlock(&g_wmu);
}
int hvk_work_pending(int q) { pthread_mutex_lock(&g_wmu); int n = g_q[q].n + g_q[q].running; pthread_mutex_unlock(&g_wmu); return n; }
void hvk_work_drain(int q) {
    pthread_mutex_lock(&g_wmu);
    for (;;) {
        int busy = 0;
        for (int i = 0; i < HVK_Q_N; i++) if (q == HVK_Q_N || q == i) busy |= g_q[i].n || g_q[i].running;
        if (!busy || !g_started) break;
        pthread_cond_wait(&g_wdone, &g_wmu);
    }
    pthread_mutex_unlock(&g_wmu);
}
int hvk_work_prepare_begin(int total) {   // total: the workers wanted with the background ones (3.5: one per hardware thread less one); the prepare workers made
    pthread_mutex_lock(&g_wmu);
    workers_start();
    g_prep_stop = 0;
    int n = total - g_nbg; if (n < 0) n = 0;
    if (n > MAXW) n = MAXW;
    for (g_nprep = 0; g_nprep < n; g_nprep++) if (pthread_create(&g_prep[g_nprep], NULL, bg_main, (void *)1)) break;
    n = g_nprep;
    pthread_mutex_unlock(&g_wmu);
    return n;
}
void hvk_work_prepare_end(void) {
    pthread_mutex_lock(&g_wmu); g_prep_stop = 1; pthread_cond_broadcast(&g_wcv); int n = g_nprep; g_nprep = 0; pthread_mutex_unlock(&g_wmu);
    for (int i = 0; i < n; i++) pthread_join(g_prep[i], NULL);
}
static void workers_stop(void) {
    pthread_mutex_lock(&g_wmu);
    if (!g_started) { pthread_mutex_unlock(&g_wmu); return; }
    for (int q = 0; q < HVK_Q_N; q++) { job *j = g_q[q].head; while (j) { job *n = j->next; free(j); j = n; } g_q[q].head = g_q[q].tail = NULL; g_q[q].n = 0; }   // queued work dropped
    g_wquit = 1; g_prep_stop = 1; pthread_cond_broadcast(&g_wcv);
    pthread_mutex_unlock(&g_wmu);
    for (int i = 0; i < g_nbg; i++) pthread_join(g_bg[i], NULL);
    if (g_opt_started) pthread_join(g_optw, NULL);
    g_opt_started = 0;
    hvk_work_prepare_end();
    pthread_mutex_lock(&g_wmu); g_started = 0; g_nbg = 0; pthread_mutex_unlock(&g_wmu);
}

// ---------- Bring-up, the test hooks, shutdown ----------
void hvk_pso_init(void) {
    const char *e = getenv("DM_VK_PSO_TRACE"); g_trace = e && !strcmp(e, "1");
    g_full_caps = hvk.dev.caps; g_full_eds = hvk.dev.eds_present; g_full_topo = hvk.dev.x.topology_unrestricted; g_full_saved = 1;
    level_rules(); quirks_find();
    hostvk_log("[hostvk] pipelines: level %c, %s%s", g_level, g_gpl ? "libraries with a fast link and an optimized relink in the background" : "monolithic, a draw waits for a pipeline it is the first to use",
               hvk.dev.x.topology_unrestricted ? "; one pipeline draws every topology class" : "");
}
static void drop_all(void) {   // every pipeline and library (the GPU is idle, no worker is building)
    pthread_mutex_lock(&g_mu);
    for (uint32_t i = 0; i < g_ents.cap; i++) if (g_ents.vals && g_ents.vals[i]) {
        pso_ent *x = g_ents.vals[i];
        if (x->cur) vkDestroyPipeline(hvk.d, x->cur, NULL);
        VkPipeline o = atomic_load(&x->opt); if (o) vkDestroyPipeline(hvk.d, o, NULL);
        free(x);
    }
    nvn_hm_clear(&g_ents, NULL);
    for (int p = 0; p < L_N; p++) {
        for (uint32_t i = 0; i < g_lib[p].cap; i++) if (g_lib[p].vals && g_lib[p].vals[i]) { lib_ent *l = g_lib[p].vals[i]; if (l->p) vkDestroyPipeline(hvk.d, l->p, NULL); free(l); }
        nvn_hm_clear(&g_lib[p], NULL);
    }
    pthread_mutex_unlock(&g_mu);
}
void hvk_finish(void);
void hvk_pso_test_level(char level) {
    hvk_finish(); hvk_work_drain(HVK_Q_N); hvk_deferred_run(UINT64_MAX);
    drop_all();
    if (!g_full_saved) return;
    hvk.dev.caps = g_full_caps; hvk.dev.eds_present = g_full_eds; hvk.dev.x.topology_unrestricted = g_full_topo;
    hostvk_caps *c = &hvk.dev.caps;
    if (level == 'B' || level == 'D') { hvk.dev.eds_present = 0; c->eds3_blend = c->eds3_raster = 0; hvk.dev.x.topology_unrestricted = 0; }
    if (level == 'C' || level == 'D') c->gpl = c->gpl_fast_link = c->gpl_indep_interp = 0;
    if (level == 'D') c->vertex_input_dynamic = 0;
    if (level > c->device_level) c->level = level;
    level_rules();
    hostvk_log("[hostvk] pipelines made for level %c from now on (test)", g_level);
}
void hvk_pso_shutdown(void) {
    workers_stop();
    drop_all();
    for (int p = 0; p < L_N; p++) memset(&g_lib[p], 0, sizeof g_lib[p]);
    memset(&g_ents, 0, sizeof g_ents);
}
