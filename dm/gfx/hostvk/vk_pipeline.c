// The Vulkan backend's pipeline layouts, descriptors and compute pipelines (docs/design/vulkan-backend.md 2.3; phase P3). Graphics pipelines are vk_gpl.c's (phase P5).
// Pipeline layouts: one universal layout per stage combination (vertex and fragment; with geometry; with tessellation, which includes geometry; compute), over the binding numbers the
// Vulkan translator gives each resource class (dm/gfx/tools/dmsh_xlate_vk/Program.cs): set 0 the uniform buffers (binding 0 the support buffer, then 1 + 18 s + c for constant
// buffer c of translator stage s, c0 and c2 dynamic: the driver banks of each draw are written into the ring and a draw changes offsets, not descriptors), set 1 the storage buffers
// (16 s + i), set 2 the combined textures (512 s + slot, the 32 combined and 32 separate slots of a stage), set 3 the storage images (512 s + slot) and texel buffers (512 s + 8 + t).
// A pipeline whose shaders declare a binding the universal layout does not hold with that type (an array of handles, a slot past those declared, an image buffer in an image slot,
// what the translator renumbers past the slots) gets a layout made from its shaders' own bindings, logged once; both are cached by content.
// Descriptor sets: per set, the resolved content of the bindings the pipeline's shaders use is hashed and looked up in the submission slot's cache; a miss allocates from the slot's
// pools and writes the set. Dynamic offsets select each stage's c0 / c2 bank.
// Compute pipelines by their shader and layout, through the disk cache (vk_cache.c).
// The backend's own pipelines: the clear triangle of design 2.7 (partial channel and stencil masks) and the compute kernels that write counter reports and conditional-render
// predicates on the GPU, in order (design 4.6, 4.7), through buffer device addresses (core in Vulkan 1.3) and push constants.
#include "plat/plat.h"
#include "vk_backend.h"
#include "vk_pso.h"
#include "../nvn/nvn_state.h"
#include "../nvn/nvn_resolve.h"
#include "../nvn/nvn_gpuwrite.h"
#include "../nvn/nvn_refl_textype.h"
#include "shaders/gen/clear_vert.h"
#include "shaders/gen/clear_frag.h"
#include "shaders/gen/report_comp.h"
#include "shaders/gen/predicate_comp.h"
#include "shaders/gen/quad_index_comp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NCOMB = 32, NSEP = 32, NTEXBUF = 32, NIMG = 8, NSLOT = 4 };
static const int k_tstage[6] = {0, 4, 3, 1, 2, 0};   // NVN stage → the translator's stage index (vertex and compute 0, tess control 1, tess evaluation 2, geometry 3, fragment 4)
static const VkShaderStageFlagBits k_vkstage[6] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                                   VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_COMPUTE_BIT};

// ---------- Layouts ----------
typedef struct { uint32_t binding, count; VkDescriptorType type; VkShaderStageFlags stages; } lb;
typedef struct { int n; lb b[1]; } set_desc;   // variable length
struct hvk_layout {
    VkPipelineLayout pl;
    VkDescriptorSetLayout set[4];
    set_desc *sd[4];           // the bindings of each set, by binding number
    int universal, combo;
    int ndyn; uint32_t dyn_binding[16]; uint8_t dyn_stage[16], dyn_bank[16];   // set 0's dynamic uniform buffers in binding order: the NVN stage and bank of each
};
static hvk_layout *g_univ[HVK_LAYOUT_N];
static nvn_hmap g_dsl;     // set content hash → VkDescriptorSetLayout (boxed)
static nvn_hmap g_layouts; // the four set layouts → hvk_layout
static uint64_t g_nfallback;
static int lb_cmp(const void *a, const void *b) { const lb *x = a, *y = b; return x->binding < y->binding ? -1 : x->binding > y->binding; }
static VkDescriptorSetLayout dsl_get(const set_desc *s) {
    typedef struct { uint64_t h1, h2; uint32_t n, pad; } dkey;
    dkey k = {nvn_hm_hash(s->b, (uint32_t)(sizeof(lb) * s->n)), nvn_hm_hash(s->b, (uint32_t)(sizeof(lb) * s->n)) ^ 0x5bd1e995u * (uint64_t)s->n, (uint32_t)s->n, 0};
    if (!g_dsl.klen) nvn_hm_init(&g_dsl, sizeof k);
    VkDescriptorSetLayout *have = nvn_hm_get(&g_dsl, &k);
    if (have) return *have;
    VkDescriptorSetLayoutBinding *b = calloc((size_t)(s->n ? s->n : 1), sizeof *b);
    if (!b) return VK_NULL_HANDLE;
    for (int i = 0; i < s->n; i++) b[i] = (VkDescriptorSetLayoutBinding){s->b[i].binding, s->b[i].type, s->b[i].count, s->b[i].stages, NULL};
    VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, (uint32_t)s->n, b};
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    VkResult r = vkCreateDescriptorSetLayout(hvk.d, &ci, NULL, &l);
    free(b);
    if (r) { hostvk_log("[RED] [hostvk] vkCreateDescriptorSetLayout failed (%d bindings, VkResult %d)", s->n, r); return VK_NULL_HANDLE; }
    VkDescriptorSetLayout *box = malloc(sizeof *box); if (box) { *box = l; nvn_hm_put(&g_dsl, &k, box); }
    return l;
}
static set_desc *sd_new(int cap) { set_desc *s = calloc(1, sizeof *s + sizeof(lb) * (size_t)(cap ? cap : 1)); return s; }
static int sd_find(const set_desc *s, uint32_t binding) {   // binary search (sorted)
    int lo = 0, hi = s->n - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (s->b[m].binding == binding) return m; if (s->b[m].binding < binding) lo = m + 1; else hi = m - 1; }
    return -1;
}
static hvk_layout *layout_make(set_desc *sd[4], int universal, int combo) {
    struct { VkDescriptorSetLayout s[4]; } key;
    for (int i = 0; i < 4; i++) { qsort(sd[i]->b, (size_t)sd[i]->n, sizeof(lb), lb_cmp); key.s[i] = dsl_get(sd[i]); if (!key.s[i]) return NULL; }
    if (!g_layouts.klen) nvn_hm_init(&g_layouts, sizeof key);
    hvk_layout *l = nvn_hm_get(&g_layouts, &key);
    if (l) { for (int i = 0; i < 4; i++) free(sd[i]); return l; }
    if (!(l = calloc(1, sizeof *l))) return NULL;
    memcpy(l->set, key.s, sizeof key.s);
    for (int i = 0; i < 4; i++) l->sd[i] = sd[i];
    l->universal = universal; l->combo = combo;
    for (int i = 0; i < sd[0]->n && l->ndyn < 16; i++) if (sd[0]->b[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
        uint32_t b = sd[0]->b[i].binding, s = (b - 1) / 18, c = (b - 1) % 18;
        int st = 0; for (int k = 0; k < 6; k++) if ((sd[0]->b[i].stages & k_vkstage[k]) && k_tstage[k] == (int)s) st = k;
        l->dyn_binding[l->ndyn] = b; l->dyn_stage[l->ndyn] = (uint8_t)st; l->dyn_bank[l->ndyn] = (uint8_t)c; l->ndyn++;
    }
    VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 4, l->set, 0, NULL};
    if (vkCreatePipelineLayout(hvk.d, &pi, NULL, &l->pl)) { hostvk_log("[RED] [hostvk] vkCreatePipelineLayout failed"); free(l); return NULL; }
    nvn_hm_put(&g_layouts, &key, l);
    return l;
}
static const int k_combo_stages[HVK_LAYOUT_N][6] = {   // the NVN stages of each combination (-1 ends)
    {0, 1, -1}, {0, 2, 1, -1}, {0, 3, 4, 2, 1, -1}, {5, -1},
};
static void add_lb(set_desc *s, uint32_t binding, VkDescriptorType t, uint32_t n, VkShaderStageFlags st) { s->b[s->n++] = (lb){binding, n, t, st}; }
// The universal layout of a combination, sized to the device's limits (a smaller limit leaves those bindings to the shader-made layouts)
static hvk_layout *univ_get(int combo) {
    if (g_univ[combo]) return g_univ[combo];
    const int *st = k_combo_stages[combo]; int nst = 0; while (st[nst] >= 0) nst++;
    set_desc *sd[4] = {sd_new(1 + 18 * nst), sd_new(16 * nst), sd_new((NCOMB + NSEP) * nst), sd_new((NIMG + NTEXBUF) * nst)};
    if (!sd[0] || !sd[1] || !sd[2] || !sd[3]) return NULL;
    VkShaderStageFlags all = 0; for (int i = 0; i < nst; i++) all |= k_vkstage[st[i]];
    uint32_t ndyn = 0, dyn_max = hvk.lim.maxDescriptorSetUniformBuffersDynamic;
    if (combo != HVK_LAYOUT_CS) add_lb(sd[0], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, all);   // the support buffer (the translator's, every stage)
    else add_lb(sd[0], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
    for (int i = 0; i < nst; i++) {
        int s = k_tstage[st[i]]; VkShaderStageFlags f = k_vkstage[st[i]];
        for (uint32_t c = 0; c < 18; c++) {
            int dyn = (c == 0 || c == 2) && ndyn < dyn_max;
            if (dyn) ndyn++;
            add_lb(sd[0], 1 + 18 * (uint32_t)s + c, dyn ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, f);
        }
        for (uint32_t c = 0; c < 16; c++) add_lb(sd[1], 16 * (uint32_t)s + c, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, f);
        for (uint32_t c = 0; c < NCOMB; c++) add_lb(sd[2], 512 * (uint32_t)s + c, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, f);
        for (uint32_t c = 0; c < NSEP; c++) add_lb(sd[2], 512 * (uint32_t)s + 128 + c, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, f);
        for (uint32_t c = 0; c < NIMG; c++) add_lb(sd[3], 512 * (uint32_t)s + c, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, f);
        for (uint32_t c = 0; c < NTEXBUF; c++) add_lb(sd[3], 512 * (uint32_t)s + 8 + c, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, f);
    }
    // the per-stage limits the universal layout needs: where the device has fewer, every pipeline of the combination takes a shader-made layout
    uint32_t ubo = 19, ssbo = 16, samp = NCOMB + NSEP, img = NIMG + NTEXBUF;
    if (ubo > hvk.lim.maxPerStageDescriptorUniformBuffers || ssbo > hvk.lim.maxPerStageDescriptorStorageBuffers || samp > hvk.lim.maxPerStageDescriptorSamplers ||
        samp + NTEXBUF > hvk.lim.maxPerStageDescriptorSampledImages || img > hvk.lim.maxPerStageDescriptorStorageImages || 18u * (uint32_t)nst + 1 > hvk.lim.maxDescriptorSetUniformBuffers ||
        16u * (uint32_t)nst > hvk.lim.maxDescriptorSetStorageBuffers || (uint32_t)(samp * nst) > hvk.lim.maxDescriptorSetSamplers) {
        hostvk_log("[hostvk] the device's descriptor limits are below the universal layout of combination %d: its pipelines take layouts made from their shaders", combo);
        for (int i = 0; i < 4; i++) free(sd[i]);
        return NULL;
    }
    return g_univ[combo] = layout_make(sd, 1, combo);
}
static int combo_of(hvk_shader *const sh[6]) {
    if (sh[5] && !sh[0]) return HVK_LAYOUT_CS;
    if (sh[3] || sh[4]) return HVK_LAYOUT_TESS;
    if (sh[2]) return HVK_LAYOUT_GS;
    return HVK_LAYOUT_VSFS;
}
static int fits(const hvk_layout *u, hvk_shader *const sh[6], char *why, size_t n) {
    for (int st = 0; st < 6; st++) if (sh[st]) for (int i = 0; i < sh[st]->nb; i++) {
        const hvk_binding *b = &sh[st]->b[i];
        if (b->set > 3) { snprintf(why, n, "set %u", b->set); return 0; }
        int k = sd_find(u->sd[b->set], b->binding);
        if (k < 0) { snprintf(why, n, "binding %u of set %u", b->binding, b->set); return 0; }
        const lb *x = &u->sd[b->set]->b[k];
        int same = x->type == b->type || (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && x->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
        if (!same || x->count != b->count || !(x->stages & k_vkstage[st])) { snprintf(why, n, "binding %u of set %u (type %d count %u)", b->binding, b->set, (int)b->type, b->count); return 0; }
        // c0 / c2 are dynamic in the universal layout: a binding that is not a driver bank there cannot be one either
        if (x->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC && b->rb_tex >= 0) {
            uint8_t kd = sh[st]->rf.b[b->rb_tex].kind;
            if (kd != NVN_RB_DRV0 && kd != NVN_RB_DRV2) { snprintf(why, n, "binding %u of set 0 is not a driver bank", b->binding); return 0; }
        }
    }
    return 1;
}
static hvk_layout *layout_for(hvk_shader *const sh[6]);
static pthread_mutex_t g_layout_mu = PTHREAD_MUTEX_INITIALIZER;   // the layout and set-layout tables: the draws and the prewarm's workers (vk_prewarm.c) both make layouts
hvk_layout *hvk_layout_for(hvk_shader *const sh[6]) {
    pthread_mutex_lock(&g_layout_mu);
    hvk_layout *l = layout_for(sh);
    pthread_mutex_unlock(&g_layout_mu);
    return l;
}
static hvk_layout *layout_for(hvk_shader *const sh[6]) {
    int combo = combo_of(sh);
    hvk_layout *u = univ_get(combo);
    char why[160] = "";
    if (u && fits(u, sh, why, sizeof why)) return u;
    // a layout from the shaders' own bindings
    int cap[4] = {1, 0, 0, 0};
    for (int st = 0; st < 6; st++) if (sh[st]) for (int i = 0; i < sh[st]->nb; i++) if (sh[st]->b[i].set < 4) cap[sh[st]->b[i].set]++;
    set_desc *sd[4]; for (int i = 0; i < 4; i++) if (!(sd[i] = sd_new(cap[i]))) return NULL;
    uint32_t ndyn = 0;
    for (int st = 0; st < 6; st++) if (sh[st]) for (int i = 0; i < sh[st]->nb; i++) {
        const hvk_binding *b = &sh[st]->b[i];
        if (b->set > 3) { HVK_RED_ONCE("shader %s declares descriptor set %u: past the four sets of design 2.3", sh[st]->key, b->set); continue; }
        set_desc *s = sd[b->set]; int k = 0; while (k < s->n && s->b[k].binding != b->binding) k++;
        VkDescriptorType t = b->type;
        if (t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->rb_tex >= 0) { uint8_t kd = sh[st]->rf.b[b->rb_tex].kind;
            if ((kd == NVN_RB_DRV0 || kd == NVN_RB_DRV2) && ndyn < hvk.lim.maxDescriptorSetUniformBuffersDynamic) { t = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; } }
        if (k < s->n) {
            if (s->b[k].type != t && !(s->b[k].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC && t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)) {
                HVK_RED_ONCE("two stages declare set %u binding %u with different descriptor types (%d, %d): the second is not bound", b->set, b->binding, (int)s->b[k].type, (int)t); continue; }
            s->b[k].stages |= k_vkstage[st];
            if (b->count > s->b[k].count) s->b[k].count = b->count;
        } else { if (t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) ndyn++; add_lb(s, b->binding, t, b->count, k_vkstage[st]); }
    }
    hvk_layout *l = layout_make(sd, 0, combo);
    if (l && u) { g_nfallback++; HVK_NOTE_ONCE("pipeline layout made from its shaders' bindings (first case: %s does not fit the universal layout of combination %d): such pipelines cannot share descriptor sets", why, combo); }
    return l;
}
VkPipelineLayout hvk_layout_vk(const hvk_layout *l) { return l ? l->pl : VK_NULL_HANDLE; }

// ---------- Static buffers: the support buffer, the zero bank ----------
static VkBuffer g_support, g_zero; static VkDeviceMemory g_support_mem, g_zero_mem;
static int static_buffers(void) {
    if (g_support) return 1;
    void *map;
    // the translator's support buffer (Ryujinx SupportBuffer.cs): render scale 1.0 at 192 + 16 i (73 entries), the rest zero: no BGRA swap, the alpha test off, as the Metal backend's support_buf
    if (hvk_buffer_make(65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 1, &g_support, &g_support_mem, &map)) return 0;
    memset(map, 0, 65536); for (int i = 0; i < 73; i++) { float one = 1; memcpy((uint8_t *)map + 192 + 16 * i, &one, 4); }
    if (hvk_buffer_make(65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, &g_zero, &g_zero_mem, &map)) return 0;
    memset(map, 0, 65536);
    return 1;
}

// ---------- Descriptor sets ----------
typedef struct { VkDescriptorPool *pools; int n, cap, cur; nvn_hmap cache; } slot_sets;
static slot_sets g_slots[16];
typedef struct { VkDescriptorSetLayout l; uint64_t h1, h2; } set_key;
static uint64_t g_hits, g_misses, g_writes;
void hvk_desc_stats(uint64_t *hits, uint64_t *misses, uint64_t *writes) { *hits = g_hits; *misses = g_misses; *writes = g_writes; }
void hvk_pipeline_slot_reset(int slot) {
    slot_sets *s = &g_slots[slot & 15];
    for (int i = 0; i < s->n; i++) vkResetDescriptorPool(hvk.d, s->pools[i], 0);
    s->cur = 0;
    if (s->cache.klen) nvn_hm_clear(&s->cache, free); else nvn_hm_init(&s->cache, sizeof(set_key));
}
static VkDescriptorPool pool_new(void) {
    VkDescriptorPoolSize ps[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16384}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4096}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096}, {VK_DESCRIPTOR_TYPE_SAMPLER, 4096}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 8192}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1024}};
    VkDescriptorPoolCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1024, sizeof ps / sizeof *ps, ps};
    VkDescriptorPool p = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(hvk.d, &ci, NULL, &p)) return VK_NULL_HANDLE;
    return p;
}
static VkDescriptorSet set_alloc(slot_sets *s, VkDescriptorSetLayout l) {
    for (int tries = 0; tries < 2; tries++) {
        if (s->cur == s->n) {
            if (s->n == s->cap) { s->cap = s->cap ? 2 * s->cap : 4; s->pools = realloc(s->pools, (size_t)s->cap * sizeof *s->pools); if (!s->pools) return VK_NULL_HANDLE; }
            if (!(s->pools[s->n] = pool_new())) return VK_NULL_HANDLE;
            s->n++;
        }
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, s->pools[s->cur], 1, &l};
        VkDescriptorSet set; VkResult r = vkAllocateDescriptorSets(hvk.d, &ai, &set);
        if (r == VK_SUCCESS) return set;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) return VK_NULL_HANDLE;
        s->cur++;   // this pool is full: the next one
    }
    return VK_NULL_HANDLE;
}
// One set's content as it is resolved: the writes and the bytes that identify it
enum { MAXW = 256, MAXINFO = 512 };
typedef struct {
    VkWriteDescriptorSet w[MAXW]; int nw;
    VkDescriptorBufferInfo bi[MAXINFO]; int nbi;
    VkDescriptorImageInfo ii[MAXINFO]; int nii;
    VkBufferView tv[MAXINFO]; int ntv;
    uint64_t h1, h2;
} set_build;
static void mix(set_build *b, const void *p, size_t n) {   // two running hashes of the content
    const uint8_t *q = p;
    for (size_t i = 0; i < n; i++) { b->h1 = (b->h1 ^ q[i]) * 0x100000001b3ull; b->h2 = (b->h2 + q[i] + 0x9e3779b97f4a7c15ull) * 0xff51afd7ed558ccdull; b->h2 ^= b->h2 >> 29; }
}
static VkWriteDescriptorSet *w_new(set_build *b, uint32_t binding, VkDescriptorType t, uint32_t count) {
    if (b->nw == MAXW) return NULL;
    VkWriteDescriptorSet *w = &b->w[b->nw++]; memset(w, 0, sizeof *w);
    w->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w->dstBinding = binding; w->descriptorCount = count; w->descriptorType = t;
    mix(b, &binding, 4); mix(b, &t, 4); mix(b, &count, 4);
    return w;
}
static int w_buffer(set_build *b, uint32_t binding, VkDescriptorType t, VkBuffer buf, VkDeviceSize off, VkDeviceSize range) {
    VkWriteDescriptorSet *w = w_new(b, binding, t, 1); if (!w || b->nbi == MAXINFO) return 0;
    VkDescriptorBufferInfo *i = &b->bi[b->nbi++]; *i = (VkDescriptorBufferInfo){buf, off, range};
    w->pBufferInfo = i; mix(b, i, sizeof *i); return 1;
}
static int w_image(set_build *b, uint32_t binding, VkDescriptorType t, const VkDescriptorImageInfo *ii, uint32_t n) {
    VkWriteDescriptorSet *w = w_new(b, binding, t, n); if (!w || b->nii + (int)n > MAXINFO) return 0;
    memcpy(&b->ii[b->nii], ii, n * sizeof *ii); w->pImageInfo = &b->ii[b->nii]; b->nii += (int)n; mix(b, ii, n * sizeof *ii); return 1;
}
static int w_texel(set_build *b, uint32_t binding, VkDescriptorType t, VkBufferView v) {
    VkWriteDescriptorSet *w = w_new(b, binding, t, 1); if (!w || b->ntv == MAXINFO) return 0;
    b->tv[b->ntv] = v; w->pTexelBufferView = &b->tv[b->ntv++]; mix(b, &v, sizeof v); return 1;
}
// A texture binding's view: by its ID through the registry, else the dummy of the declared type and class
static VkImageView tex_view_of(int st, int tid, const hvk_binding *d, const nvn_rbind *rb, int storage, uint8_t *cls, int write) {
    *cls = d->cls;
    tex_t t; view_t v; int has_v = 0;
    pthread_mutex_lock(&hvk.tex_mu);
    int ok = tid > 0 && hvk_reg_texture(tid, &t, &v, &has_v);
    pthread_mutex_unlock(&hvk.tex_mu);
    if (ok && !hvk_tex_is_buffer(&t)) {
        hvk_tex *x = hvk_tex_get(&t);
        extern int hvk_tex_view_typed(hvk_tex *x, const view_t *v, int kind, int want_type, uint8_t want_cls, hvk_view *out);
        hvk_view hv;
        if (x && hvk_tex_view_typed(x, has_v ? &v : NULL, storage ? HVK_VIEW_STORAGE : HVK_VIEW_SAMPLED, d->dim, d->cls, &hv)) {
            if (write) hvk_tex_gpu_written(x);
            if (hv.cls != d->cls && !(hv.cls == NVN_VK_DS && d->cls == NVN_VK_FLOAT) && !storage)
                HVK_RED_ONCE("stage %d binding %u: a texture of numeric class %d read through a declaration of class %d (first case, texture ID %d): bound as it is (the census of design 2.2)",
                             st, d->binding, hv.cls, d->cls, tid);
            *cls = hv.cls;
            return hv.view;
        }
        if (x) HVK_RED_ONCE("stage %d binding %u: no view of texture ID %d fits the declared type %d (first case): the dummy is bound", st, d->binding, tid, d->dim);
    } else if (tid > 0 && !ok) HVK_NOTE_ONCE("texture ID %d is not registered (first case): an unbound slot, the dummy is read", tid);
    return hvk_dummy_view(rb ? rb->dtt : NVN_DTT_2D, rb ? rb->dpf : NVN_DPF_FLOAT, storage);
}
static int samp_id(int st, const nvn_rbind *s) {
    switch (s->kind) {
        case NVN_RB_SAMP: return s->reg < 128 ? (int)(S.samp[st][s->reg] >> 20) : 0;
        case NVN_RB_SAMPT: return s->reg < NTEX ? (int)(S.tex[st][s->reg] >> 20) : 0;
        case NVN_RB_SAMPI: return (int)s->reg;
        case NVN_RB_SAMPU: return nvn_sid_from_handle(nvn_ubo_handle(st, s->reg, s->off));
        default: return 0;
    }
}
static int tex_id(int st, const nvn_rbind *t) {
    switch (t->kind) {
        case NVN_RB_TEX: case NVN_RB_TBUF: return (int)(nvn_tex_slot_handle(S.tex[st], S.img[st], t->reg) & 0xfffff);
        case NVN_RB_TEXU: return nvn_tid_from_handle(nvn_ubo_handle(st, t->reg, t->off));
        case NVN_RB_IMG: return t->reg < 32 ? (int)(S.img[st][t->reg] & 0xfffff) : 0;
        default: return 0;
    }
}
// The driver bank of stage st (0: c0, 2: c2) for this draw, in the ring: its offset for the dynamic descriptor
static int bank_alloc(int st, int bank, const hvk_shader *sh, uint32_t base_instance, const uint32_t dispatch[3], hvk_alloc *a) {
    VkDeviceSize align = hvk.lim.minUniformBufferOffsetAlignment > 16 ? hvk.lim.minUniformBufferOffsetAlignment : 16;
    if (!hvk_ring_alloc(DRV_SIZE, align, a)) return 0;
    if (sh && sh->rf.drv_read[bank ? 2 : 0]) {
        static const uint32_t z3[3];
        if (!nvn_fill_driver_bank(st, bank, a->cpu, sh->rf.dyn, base_instance, dispatch ? dispatch : z3))
            HVK_RED_ONCE("stage %d shader reads driver segment c0; the storage buffer table base was never measured", st);
    } else memset(a->cpu, 0, DRV_SIZE);
    return 1;
}
static void dummy_ssbo(VkBuffer *buf, VkDeviceSize *off, VkDeviceSize *range) { *buf = g_zero; *off = 0; *range = 65536; }
// The writes of the bindings of the shaders in sh that fall in set k of layout l
static int build_set(set_build *b, const hvk_layout *l, int k, hvk_shader *const sh[6], uint32_t base_instance, const uint32_t dispatch[3], VkDeviceSize ubo_max) {
    for (int st = 0; st < 6; st++) if (sh[st]) for (int i = 0; i < sh[st]->nb; i++) {
        const hvk_binding *d = &sh[st]->b[i];
        if ((int)d->set != k) continue;
        int li = sd_find(l->sd[k], d->binding); if (li < 0) continue;
        const lb *x = &l->sd[k]->b[li];
        const nvn_rbind *rt = d->rb_tex >= 0 ? &sh[st]->rf.b[d->rb_tex] : NULL, *rs = d->rb_samp >= 0 ? &sh[st]->rf.b[d->rb_samp] : NULL;
        if (x->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) continue;   // the driver banks: written below for every dynamic binding
        if (d->count > 1 || (rt && (rt->kind == NVN_RB_TEXD || rt->kind == NVN_RB_TEXA || rt->n > 1)) || (rs && (rs->kind == NVN_RB_SAMPD || rs->kind == NVN_RB_SAMPA || rs->n > 1))) {
            HVK_RED_ONCE("shader %s: an array of %u descriptors at set %u binding %u (dynamic or indexed handles): every element reads the dummy (phase P4)", sh[st]->key, d->count, d->set, d->binding);
            VkDescriptorImageInfo ii[64]; uint32_t n = d->count > 64 ? 64 : d->count;
            for (uint32_t e = 0; e < n; e++) ii[e] = (VkDescriptorImageInfo){hvk_default_sampler(), x->type == VK_DESCRIPTOR_TYPE_SAMPLER ? VK_NULL_HANDLE : hvk_dummy_view(rt ? rt->dtt : 0, rt ? rt->dpf : 0, x->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE), VK_IMAGE_LAYOUT_GENERAL};
            if (x->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || x->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || x->type == VK_DESCRIPTOR_TYPE_SAMPLER || x->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
                { if (!w_image(b, d->binding, x->type, ii, n)) return 0; }
            continue;
        }
        switch (x->type) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: {
                VkBuffer buf = g_zero; VkDeviceSize off = 0, range = 65536 < ubo_max ? 65536 : ubo_max;
                if (rt && rt->kind == NVN_RB_C1) { if (sh[st]->c1buf) buf = sh[st]->c1buf; }
                else if (rt && rt->kind == NVN_RB_SUPPORT) buf = g_support;
                else if (rt && rt->kind == NVN_RB_CBUF) {
                    const p_bind_buf *u = rt->reg < 16 ? &S.ubo[st][rt->reg] : NULL;
                    if (u && u->addr && hvk_buf(u->addr, 1, &buf, &off, NULL)) {
                        range = u->size ? u->size : 16; if (range > ubo_max) range = ubo_max;
                        if (off % (hvk.lim.minUniformBufferOffsetAlignment ? hvk.lim.minUniformBufferOffsetAlignment : 1)) {
                            HVK_RED_ONCE("stage %d uniform slot %u at an address the device cannot bind (%#llx, alignment %llu): the zero bank is read", st, rt->reg, (unsigned long long)u->addr, (unsigned long long)hvk.lim.minUniformBufferOffsetAlignment);
                            buf = g_zero; off = 0; range = 65536 < ubo_max ? 65536 : ubo_max;
                        }
                    } else { buf = g_zero; off = 0; if (u && u->addr) HVK_RED_ONCE("stage %d uniform slot %u: its address %#llx is in no memory pool (first case): the zero bank is read", st, rt->reg, (unsigned long long)u->addr); }
                } else if (rt && (rt->kind == NVN_RB_DRV0 || rt->kind == NVN_RB_DRV2)) {   // a driver bank the layout holds as a plain uniform buffer (past the dynamic budget): written per draw
                    hvk_alloc a; if (!bank_alloc(st, rt->kind == NVN_RB_DRV2 ? 2 : 0, sh[st], base_instance, dispatch, &a)) return 0;
                    buf = a.buf; off = a.off; range = DRV_SIZE;
                }
                if (!w_buffer(b, d->binding, x->type, buf, off, range)) return 0;
                break;
            }
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
                VkBuffer buf; VkDeviceSize off = 0, range = 0; uint64_t addr = 0, size = 0;
                if (rt && (rt->kind == NVN_RB_SBUFP || rt->kind == NVN_RB_UBOP)) { const p_bind_buf *u = rt->reg < 16 ? (rt->kind == NVN_RB_UBOP ? &S.ubo[st][rt->reg] : &S.ssbo[st][rt->reg]) : NULL; if (u) { addr = u->addr; size = u->size; } }
                else if (rt && rt->kind == NVN_RB_SBUFU) { nvn_ubo_read(st, rt->reg, rt->off, 8, &addr); size = 0; }
                VkDeviceSize align = hvk.lim.minStorageBufferOffsetAlignment ? hvk.lim.minStorageBufferOffsetAlignment : 1;
                if (addr && hvk_buf(addr & ~(uint64_t)(align - 1), 1, &buf, &off, NULL)) {
                    VkBuffer b2; VkDeviceSize o2;
                    uint64_t lo = addr & ~(uint64_t)(align - 1), want = size ? size + (addr - lo) : 65536;
                    while (want > 16 && !hvk_buf(lo, want, &b2, &o2, NULL)) want /= 2;   // within the pool: an unknown size takes what the pool holds, up to 64 KiB
                    range = want;
                    if (rt->rw && rt->kind != NVN_RB_UBOP) nvn_gw_sbw_note(addr, size, SBW_GPU);
                } else dummy_ssbo(&buf, &off, &range);
                if (!w_buffer(b, d->binding, x->type, buf, off, range)) return 0;
                break;
            }
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: case VK_DESCRIPTOR_TYPE_SAMPLER: case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
                uint8_t cls = d->cls; VkDescriptorImageInfo ii = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
                if (x->type != VK_DESCRIPTOR_TYPE_SAMPLER) ii.imageView = tex_view_of(st, rt ? tex_id(st, rt) : 0, d, rt, x->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &cls, d->rw);
                if (x->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || x->type == VK_DESCRIPTOR_TYPE_SAMPLER) {
                    int sid = rs ? samp_id(st, rs) : 0;
                    pthread_mutex_lock(&hvk.tex_mu); ii.sampler = hvk_reg_sampler(sid, cls); pthread_mutex_unlock(&hvk.tex_mu);
                }
                if (!ii.imageView && x->type != VK_DESCRIPTOR_TYPE_SAMPLER) { HVK_RED_ONCE("set %u binding %u: no view and no dummy (type %d): the draw is skipped", d->set, d->binding, rt ? rt->dtt : -1); return 0; }
                if (!w_image(b, d->binding, x->type, &ii, 1)) return 0;
                break;
            }
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
                int storage = x->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER; VkBufferView v = VK_NULL_HANDLE;
                int tid = rt ? tex_id(st, rt) : 0; tex_t t; view_t vv; int hv = 0, ok;
                pthread_mutex_lock(&hvk.tex_mu); ok = tid > 0 && hvk_reg_texture(tid, &t, &vv, &hv); pthread_mutex_unlock(&hvk.tex_mu);
                if (ok && hvk_tex_is_buffer(&t)) v = hvk_tex_buffer_view(&t, storage, NULL);
                else if (ok) HVK_RED_ONCE("stage %d: texture ID %d in a buffer slot is not a buffer texture (first case): the slot reads 0", st, tid);
                if (!v) v = hvk_dummy_texel_buffer(rt ? rt->dpf : NVN_DPF_FLOAT, storage);
                if (!v || !w_texel(b, d->binding, x->type, v)) return 0;
                break;
            }
            default: HVK_RED_ONCE("descriptor type %d not implemented", (int)x->type); break;
        }
    }
    return 1;
}
int hvk_bind_resources(VkCommandBuffer cb, hvk_layout *l, hvk_shader *const sh[6], VkPipelineBindPoint bp, uint32_t base_instance, const uint32_t dispatch[3]) {
    if (!static_buffers()) return 0;
    slot_sets *ss = &g_slots[hvk_slot_index() & 15];
    if (!ss->cache.klen) nvn_hm_init(&ss->cache, sizeof(set_key));
    VkDeviceSize ubo_max = hvk.lim.maxUniformBufferRange ? hvk.lim.maxUniformBufferRange : 16384;
    static set_build b;   // submit thread only
    VkDescriptorSet sets[4]; uint32_t dyn[16]; int ndyn = 0;
    // the dynamic banks first: their buffers are part of set 0's content
    VkBuffer dbuf[16]; VkDeviceSize doff[16];
    for (int i = 0; i < l->ndyn; i++) {
        int st = l->dyn_stage[i], bank = l->dyn_bank[i];
        const hvk_shader *s = sh[st];
        hvk_alloc a;
        if (!bank_alloc(st, bank, s, base_instance, dispatch, &a)) return 0;
        dbuf[i] = a.buf; doff[i] = a.off;
    }
    for (int k = 0; k < 4; k++) {
        b.nw = b.nbi = b.nii = b.ntv = 0; b.h1 = 0xcbf29ce484222325ull; b.h2 = (uint64_t)k;
        if (k == 0) for (int i = 0; i < l->ndyn; i++) if (!w_buffer(&b, l->dyn_binding[i], VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, dbuf[i], 0, DRV_SIZE)) return 0;
        if (!build_set(&b, l, k, sh, base_instance, dispatch, ubo_max)) return 0;
        set_key key = {l->set[k], b.h1, b.h2};
        VkDescriptorSet *have = nvn_hm_get(&ss->cache, &key);
        if (have) { sets[k] = *have; g_hits++; continue; }
        VkDescriptorSet set = set_alloc(ss, l->set[k]);
        if (!set) { HVK_RED_ONCE("descriptor set allocation failed: the draw is skipped"); return 0; }
        for (int i = 0; i < b.nw; i++) b.w[i].dstSet = set;
        if (b.nw) vkUpdateDescriptorSets(hvk.d, (uint32_t)b.nw, b.w, 0, NULL);
        g_misses++; g_writes += (uint64_t)b.nw;
        VkDescriptorSet *box = malloc(sizeof *box); if (box) { *box = set; nvn_hm_put(&ss->cache, &key, box); }
        sets[k] = set;
    }
    for (int i = 0; i < l->ndyn; i++) dyn[ndyn++] = (uint32_t)doff[i];
    vkCmdBindDescriptorSets(cb, bp, l->pl, 0, 4, sets, (uint32_t)ndyn, dyn);
    return 1;
}

// ---------- Compute pipelines (graphics pipelines are vk_gpl.c's, phase P5) ----------
static nvn_hmap g_cso;    // compute shader and layout → VkPipeline (boxed), under g_cso_mu: the draws' dispatches and the prewarm's C rows (vk_prewarm.c)
static pthread_mutex_t g_cso_mu = PTHREAD_MUTEX_INITIALIZER;
VkPipeline hvk_cs_pipeline(hvk_shader *cs, hvk_layout *l) {
    struct { const void *cs, *l; } key = {cs, l};
    pthread_mutex_lock(&g_cso_mu);
    if (!g_cso.klen) nvn_hm_init(&g_cso, sizeof key);
    VkPipeline *have = nvn_hm_get(&g_cso, &key);
    if (have) { VkPipeline p = *have; pthread_mutex_unlock(&g_cso_mu); return p; }
    VkPipelineCreationFeedback fb = {0};
    VkPipelineCreationFeedbackCreateInfo fci = {VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, NULL, &fb, 0, NULL};
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, &fci, 0,
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs->module, cs->entry, NULL}, l->pl, VK_NULL_HANDLE, -1};
    VkPipeline p = VK_NULL_HANDLE;
    VkPipelineCache pc = hvk_cache_begin();   // compute pipelines go into the disk cache too (vk_cache.c, design 3.4)
    VkResult r = vkCreateComputePipelines(hvk.d, pc, 1, &ci, NULL, &p);
    hvk_cache_end(&fb, r == VK_SUCCESS);
    if (r) { hostvk_log("[RED] [hostvk] compute pipeline %s failed", cs->key); p = VK_NULL_HANDLE; }
    atomic_fetch_add(&hvk.st_pipelines, 1);
    VkPipeline *box = malloc(sizeof *box); if (box) { *box = p; nvn_hm_put(&g_cso, &key, box); }
    pthread_mutex_unlock(&g_cso_mu);
    return p;
}

// ---------- The backend's own pipelines ----------
static VkPipelineLayout g_kpl;   // the kernels' layout: 128 bytes of push constants, no set
static VkPipeline g_kernel[HVK_K_N];
static int kernels_init(void) {
    if (g_kpl) return 1;
    VkPushConstantRange pc = {VK_SHADER_STAGE_ALL, 0, 128};
    VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &pc};
    if (vkCreatePipelineLayout(hvk.d, &pi, NULL, &g_kpl)) { g_kpl = VK_NULL_HANDLE; return 0; }
    const struct { const uint32_t *c; size_t n; } src[HVK_K_N] = {{report_comp_spv, sizeof report_comp_spv}, {predicate_comp_spv, sizeof predicate_comp_spv}, {quad_index_comp_spv, sizeof quad_index_comp_spv}};
    for (int i = 0; i < HVK_K_N; i++) {
        VkShaderModule m = hvk_module(src[i].c, src[i].n);
        if (!m) return 0;
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", NULL}, g_kpl, VK_NULL_HANDLE, -1};
        VkResult r = vkCreateComputePipelines(hvk.d, VK_NULL_HANDLE, 1, &ci, NULL, &g_kernel[i]);
        vkDestroyShaderModule(hvk.d, m, NULL);
        if (r) { g_kernel[i] = VK_NULL_HANDLE; return 0; }
    }
    return 1;
}
int hvk_kernel_dispatch(VkCommandBuffer cb, hvk_kernel k, const void *push, uint32_t push_bytes, uint32_t groups) {
    if (!kernels_init() || !g_kernel[k]) { HVK_RED_ONCE("the backend's compute kernels could not be made (buffer device address: %d)", hvk.dev.caps.buffer_device_address); return 0; }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_kernel[k]);
    vkCmdPushConstants(cb, g_kpl, VK_SHADER_STAGE_ALL, 0, push_bytes, push);
    vkCmdDispatch(cb, groups ? groups : 1, 1, 1);
    return 1;
}
// The clear triangle's fragment modules: one per colour slot and value type (shaders/clear.frag, compiled 24 times by gen_spirv.sh)
#define CF(s) {{clear_frag_s##s##_k0_spv, sizeof clear_frag_s##s##_k0_spv}, {clear_frag_s##s##_k1_spv, sizeof clear_frag_s##s##_k1_spv}, {clear_frag_s##s##_k2_spv, sizeof clear_frag_s##s##_k2_spv}}
static const struct { const uint32_t *c; size_t n; } k_clear_frag[8][3] = {CF(0), CF(1), CF(2), CF(3), CF(4), CF(5), CF(6), CF(7)};
#undef CF
const uint32_t *hvk_clear_frag_code(int slot, int kind, size_t *bytes);
const uint32_t *hvk_clear_frag_code(int slot, int kind, size_t *bytes) {
    if (slot < 0 || slot > 7 || kind < 0 || kind > 2) return NULL;
    *bytes = k_clear_frag[slot][kind].n; return k_clear_frag[slot][kind].c;
}
// The clear triangle: a pipeline per pass formats, slot, value type and mask (and the depth / stencil writes, set as dynamic stencil state), with the fragment shader of its slot and type
typedef struct { VkFormat color[8], depth, stencil; uint32_t samples, ncolor; int32_t slot; uint32_t kind, mask, wdepth, stencil_on; } clr_key;
static nvn_hmap g_clr;
static VkShaderModule g_clr_vs, g_clr_fs[8][3];
int hvk_clear_tri(VkCommandBuffer cb, const VkFormat color[8], int ncolor, VkFormat depth, VkFormat stencil, VkSampleCountFlagBits samples, int slot, uint8_t kind,
                  uint32_t mask, const uint32_t value[4], float z, int write_depth, uint32_t stencil_mask, uint32_t stencil_ref, VkRect2D rect, VkExtent2D extent) {
    if (!kernels_init()) return 0;
    clr_key k; memset(&k, 0, sizeof k);
    memcpy(k.color, color, sizeof k.color); k.depth = depth; k.stencil = stencil; k.samples = samples; k.ncolor = (uint32_t)ncolor; k.slot = slot; k.kind = kind; k.mask = slot >= 0 ? mask : 0;
    k.wdepth = (uint32_t)(slot < 0 && write_depth); k.stencil_on = slot < 0 && stencil_mask != 0;
    if (!g_clr.klen) nvn_hm_init(&g_clr, sizeof k);
    VkPipeline *have = nvn_hm_get(&g_clr, &k), p = have ? *have : VK_NULL_HANDLE;
    if (!have) {
        if (!g_clr_vs) g_clr_vs = hvk_module(clear_vert_spv, sizeof clear_vert_spv);
        if (slot >= 0 && !g_clr_fs[slot][kind]) { size_t n; const uint32_t *c = hvk_clear_frag_code(slot, kind, &n); g_clr_fs[slot][kind] = c ? hvk_module(c, n) : VK_NULL_HANDLE; }
        VkPipelineShaderStageCreateInfo stg[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, g_clr_vs, "main", NULL},
                                                  {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, slot >= 0 ? g_clr_fs[slot][kind] : VK_NULL_HANDLE, "main", NULL}};
        if (!g_clr_vs || (slot >= 0 && !stg[1].module)) return 0;
        VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
        VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
        VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1};
        VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, samples ? samples : VK_SAMPLE_COUNT_1_BIT};
        VkStencilOpState so = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS, 0xff, 0, 0};
        VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL, 0, k.wdepth, k.wdepth, VK_COMPARE_OP_ALWAYS, VK_FALSE, k.stencil_on, so, so, 0, 1};
        VkPipelineColorBlendAttachmentState ba[8]; memset(ba, 0, sizeof ba);
        if (slot >= 0) ba[slot].colorWriteMask = mask & 0xf;
        VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, (uint32_t)ncolor, ba};
        VkDynamicState dsv[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
        VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 4, dsv};
        VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, (uint32_t)ncolor, color, depth, stencil};
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, slot >= 0 ? 2u : 1u, stg, &vi, &ia, NULL, &vp, &rs, &ms, &ds, &cbs, &dy, g_kpl, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, -1};
        if (vkCreateGraphicsPipelines(hvk.d, VK_NULL_HANDLE, 1, &ci, NULL, &p)) { p = VK_NULL_HANDLE; HVK_RED_ONCE("the clear-triangle pipeline could not be made (slot %d)", slot); }
        VkPipeline *box = malloc(sizeof *box); if (box) { *box = p; nvn_hm_put(&g_clr, &k, box); }
    }
    if (!p) return 0;
    struct { uint32_t v[4]; float z; } pc; memcpy(pc.v, value, 16); pc.z = z;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdPushConstants(cb, g_kpl, VK_SHADER_STAGE_ALL, 0, sizeof pc, &pc);
    VkViewport v = {0, 0, (float)extent.width, (float)extent.height, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &v); vkCmdSetScissor(cb, 0, 1, &rect);
    if (stencil) { vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_mask); vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_ref); }
    vkCmdDraw(cb, 3, 1, 0, 0);
    return 1;
}

void hvk_pipeline_shutdown(void) {
    for (uint32_t i = 0; i < g_cso.cap; i++) if (g_cso.vals && g_cso.vals[i]) { if (*(VkPipeline *)g_cso.vals[i]) vkDestroyPipeline(hvk.d, *(VkPipeline *)g_cso.vals[i], NULL); free(g_cso.vals[i]); }
    nvn_hm_clear(&g_cso, NULL);
    for (uint32_t i = 0; i < g_clr.cap; i++) if (g_clr.vals && g_clr.vals[i]) { if (*(VkPipeline *)g_clr.vals[i]) vkDestroyPipeline(hvk.d, *(VkPipeline *)g_clr.vals[i], NULL); free(g_clr.vals[i]); }
    nvn_hm_clear(&g_clr, NULL);
    if (g_clr_vs) vkDestroyShaderModule(hvk.d, g_clr_vs, NULL);
    for (int i = 0; i < 8; i++) for (int j = 0; j < 3; j++) if (g_clr_fs[i][j]) vkDestroyShaderModule(hvk.d, g_clr_fs[i][j], NULL);
    memset(g_clr_fs, 0, sizeof g_clr_fs); g_clr_vs = VK_NULL_HANDLE;
    for (int i = 0; i < HVK_K_N; i++) if (g_kernel[i]) vkDestroyPipeline(hvk.d, g_kernel[i], NULL);
    memset(g_kernel, 0, sizeof g_kernel);
    if (g_kpl) vkDestroyPipelineLayout(hvk.d, g_kpl, NULL);
    g_kpl = VK_NULL_HANDLE;
    for (int s = 0; s < 16; s++) { for (int i = 0; i < g_slots[s].n; i++) vkDestroyDescriptorPool(hvk.d, g_slots[s].pools[i], NULL); free(g_slots[s].pools);
        if (g_slots[s].cache.klen) nvn_hm_clear(&g_slots[s].cache, free);
        memset(&g_slots[s], 0, sizeof g_slots[s]); }
    for (uint32_t i = 0; i < g_layouts.cap; i++) if (g_layouts.vals && g_layouts.vals[i]) { hvk_layout *l = g_layouts.vals[i]; vkDestroyPipelineLayout(hvk.d, l->pl, NULL); for (int k = 0; k < 4; k++) free(l->sd[k]); free(l); }
    nvn_hm_clear(&g_layouts, NULL); memset(g_univ, 0, sizeof g_univ);
    for (uint32_t i = 0; i < g_dsl.cap; i++) if (g_dsl.vals && g_dsl.vals[i]) { vkDestroyDescriptorSetLayout(hvk.d, *(VkDescriptorSetLayout *)g_dsl.vals[i], NULL); free(g_dsl.vals[i]); }
    nvn_hm_clear(&g_dsl, NULL);
    if (g_support) { vkDestroyBuffer(hvk.d, g_support, NULL); vkFreeMemory(hvk.d, g_support_mem, NULL); hvk.dev.allocations--; }
    if (g_zero) { vkDestroyBuffer(hvk.d, g_zero, NULL); vkFreeMemory(hvk.d, g_zero_mem, NULL); hvk.dev.allocations--; }
    g_support = g_zero = VK_NULL_HANDLE;
}
