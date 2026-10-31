// The Vulkan backend's textures and samplers (docs/design/vulkan-backend.md 5.1-5.5). A texture object's host texture is a VkImage with optimal tiling, made the first time the GPU
// uses it, in the format the NVN → Vulkan table's path gives (dm/gfx/tex/nvn_fmt_vk.c, checked against the device's format features): the row's own format, its substitute read
// through a component mapping, a format the upload converts into, the RGBA8 decode of ASTC (decision D1) or a depth format whose guest packing the backend converts (D10). Content:
// - a texture the CPU fills (a packaged layout, be_tex_cpu_written, linear storage) is uploaded in the submission's prologue: through the GPU (step P7, design 4.3: a compute pass
//   reads its storage from the imported pool, deswizzles or decodes it into the upload scratch, and a copy writes the image) where the device has buffer device addresses, the
//   texture needs no CPU conversion and its storage is 64 KiB or more (DM_VK_DESWIZZLE=cpu|gpu forces either way), else through the CPU (dm/gfx/tex/nvn_deswizzle.c into the
//   ring, with the conversions of the table's paths); its storage is then write-protected (dm/gfx/nvn/nvn_dirty.c) and it is uploaded again when its pages change, until the GPU
//   writes it;
// - every other texture starts at zero (the image is cleared when it is made), as a new Metal texture does (t_clear_ds's last case); a decoded ASTC texture starts as the decode
//   of zero blocks, the error colour;
// - what the GPU writes (attachments, copies, clears, storage images) stays in the image: the texture is GPU-owned from then on and never uploaded again. A CPU read
//   (be_read_texels) reads the storage of a CPU-filled texture and the image of a GPU-owned one (depth and stencil joined into the guest's packing).
// Images stay in VK_IMAGE_LAYOUT_GENERAL (one transition from UNDEFINED when made): every use is valid in it, and the full barriers between operations (vk_commands.c, design 4.9)
// order their accesses. Usage: transfer, sampled, attachment and storage wherever the format allows (attachment use costs compression on some GPUs: to be measured per vendor,
// design 5.5); storage needs the IMAGE flag or a registration as an image before the texture is first used.
// Views: by kind (sampled with the composed channel mapping, attachment and storage with the identity), level and layer range, format and type; the type a shader declares wins
// (a 2D array sampled as 2D reads layer 0, a six-layer square 2D array can be a cube), as the Metal backend's tex_fit does. Buffer textures are texel buffer views over the pool.
// Samplers from NVN's sampler parameters: exact border colours with VK_EXT_custom_border_color, integer borders for integer textures (a sampler per numeric class; hle_nvn.c stores
// integer border values as floats, nvnSamplerBuilderSetBorderColori), min / max reduction, LOD bias and clamps, anisotropy, depth compare.
// Switches: DM_VK_ASTC=native|decode (the ASTC formats held compressed where the device has them, or decoded into RGBA8 anyway; rgba16f, bc7 and bc7cache are the other options
// of decision D1, declared with a red line), DM_VK_DESWIZZLE=cpu|gpu, DM_VK_DEPTH24=d32f (NVN depth 24 in the 32-bit float formats even where the device has D24).
// Declared, with a red line: the other options of D1, texture → buffer copies of a decoded ASTC texture (no re-encode), buffer → texture copies into a texture whose upload
// converts on the CPU, render targets held in a substitute read through a component mapping, a texture registered as an image after it was made without storage use, linear
// textures the GPU renders into (not written back to their storage), CPU reads of GPU-written colour textures held in a substitute format.
#include "plat/plat.h"
#include "vk_backend.h"
#include "../nvn/nvn_dirty.h"
#include "../nvn/nvn_refl_textype.h"
#include "../tex/nvn_deswizzle.h"
#include "../tex/astc_decode.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { NVIEW = 24 };
enum { GPU_MIN_BYTES = 64 << 10 };   // DM_VK_DESWIZZLE unset: smaller textures go through the CPU (a kernel and two barriers cost more than the copy)
typedef struct { uint8_t kind, type, aspect, swz[4]; uint32_t fmt, level, nlevels, layer0, nlayers; } view_key;
enum { HOW_NONE, HOW_VK, HOW_ALT, HOW_CONV, HOW_DECODE };   // which format of the row the image has
struct hvk_tex {
    uint64_t serial;
    tex_t t;                   // the descriptor as the texture was made (storage, swizzle, flags)
    int live, made, failed, wants_storage;
    VkImage img; VkDeviceMemory mem;
    nvn_vk_fmt f; int how, use_alt; uint8_t alt_swz[4];
    VkFormat fmt; VkImageAspectFlags aspect; VkImageType type; VkImageCreateFlags flags; VkImageUsageFlags usage;
    uint32_t w, h, d, layers, levels; VkSampleCountFlagBits samples;
    int cpu_content, gpu_written, uploaded, expand3, convert, astc_dec, astc_srgb, d24sub;
    int64_t mem_bytes, astc_block_bytes;   // the image's allocation; for a decoded ASTC texture what its blocks take (the memory line, decision D1)
    uint64_t stamp, seen; const void *span;
    struct { view_key k; VkImageView v; } views[NVIEW]; int nviews;
    VkBufferView bview[2]; uint8_t bcls;   // a buffer texture's texel buffer views (sampled, storage)
};
static nvn_hmap g_tex;   // serial → hvk_tex, under hvk.tex_mu

// ---------- Switches ----------
enum { ASTC_NATIVE, ASTC_DECODE };
enum { DESWZ_AUTO, DESWZ_GPU, DESWZ_CPU };
static struct { int astc, deswz, d24_d32f, force_alt; } g_opt;
static pthread_once_t g_opt_once = PTHREAD_ONCE_INIT;
static void opts_read(void) {
    const char *e = getenv("DM_VK_ASTC");
    if (e && *e) {
        if (!strcmp(e, "native")) g_opt.astc = ASTC_NATIVE;
        else if (!strcmp(e, "decode") || !strcmp(e, "rgba8")) g_opt.astc = ASTC_DECODE;
        else if (!strcmp(e, "rgba16f") || !strcmp(e, "bc7") || !strcmp(e, "bc7cache")) {
            g_opt.astc = ASTC_DECODE;
            hostvk_log("[RED] [hostvk] DM_VK_ASTC=%s: not done (decision D1's other options: RGBA16F for linear formats, BC7 encoded at upload, BC7 encoded once and cached on disk); "
                       "ASTC is decoded into RGBA8 instead", e);
        } else hostvk_log("[RED] [hostvk] DM_VK_ASTC=%s is not native, decode, rgba16f, bc7 or bc7cache: native where the device has ASTC, decoded into RGBA8 elsewhere", e);
    }
    e = getenv("DM_VK_DESWIZZLE");
    if (e && *e) {
        if (!strcmp(e, "cpu")) g_opt.deswz = DESWZ_CPU;
        else if (!strcmp(e, "gpu")) g_opt.deswz = DESWZ_GPU;
        else hostvk_log("[RED] [hostvk] DM_VK_DESWIZZLE=%s is not cpu or gpu: textures of 64 KiB and more go through the GPU, smaller ones through the CPU", e);
    }
    e = getenv("DM_VK_DEPTH24");
    if (e && *e) {
        if (!strcmp(e, "d32f")) g_opt.d24_d32f = 1;
        else if (strcmp(e, "native")) hostvk_log("[RED] [hostvk] DM_VK_DEPTH24=%s is not d32f or native: D24 where the device has it", e);
    }
}
static void opts(void) { pthread_once(&g_opt_once, opts_read); }
// For tests (dm/gfx/tests/vk): the switches for textures made from now on (-1 keeps one): ASTC decoded, the deswizzle (DESWZ_*: 0 by size, 1 GPU, 2 CPU), depth 24 in the float
// formats, and every row's substitute where it has one (alt, conv or the ASTC decode: the paths a device without those formats takes)
void hvk_test_tex_opts(int astc_decode, int deswizzle, int d24_d32f, int force_alt);
void hvk_test_tex_opts(int astc_decode, int deswizzle, int d24_d32f, int force_alt) {
    opts();
    if (astc_decode >= 0) g_opt.astc = astc_decode ? ASTC_DECODE : ASTC_NATIVE;
    if (deswizzle >= 0) g_opt.deswz = deswizzle;
    if (d24_d32f >= 0) g_opt.d24_d32f = d24_d32f;
    if (force_alt >= 0) g_opt.force_alt = force_alt;
}

// The bytes a band of the GPU upload paths writes into the scratch before its copy (HVK_SCRATCH_MAX; tests make it small to cut a texture into many bands)
static uint32_t g_band = HVK_SCRATCH_MAX;
void hvk_test_band_bytes(uint32_t n);
void hvk_test_band_bytes(uint32_t n) { g_band = n ? n : HVK_SCRATCH_MAX; }
// For tests: the depth bias of NVN depth 24 held in a float format as on a device without VK_EXT_depth_bias_control's float representation (vk_commands.c asks hvk_tex_dbc_float)
static int g_no_dbc;
void hvk_test_no_depth_bias_control(int on);
void hvk_test_no_depth_bias_control(int on) { g_no_dbc = on; }
int hvk_tex_dbc_float(void) { return hvk.dev.x.dbc_float && !g_no_dbc; }

// ---------- Formats ----------
static VkFormatFeatureFlags2 fmt_features(VkFormat f, int optimal) {
    if (!f) return 0;
    VkFormatProperties3 p3 = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
    VkFormatProperties2 p = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &p3};
    vkGetPhysicalDeviceFormatProperties2(hvk.dev.phys, f, &p);
    return optimal ? p3.optimalTilingFeatures : p3.bufferFeatures;
}
static VkFormatFeatureFlags2 fmt_features_buf(VkFormat f) { return fmt_features(f, 0); }
int hvk_fmt_linear_filter(VkFormat f) {
    static _Atomic uint8_t cache[256];   // 0 unknown, 1 no, 2 yes (the core formats; others are asked each time)
    if ((uint32_t)f < 256) { uint8_t v = atomic_load(&cache[f]); if (v) return v == 2; }
    int yes = (fmt_features(f, 1) & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    if ((uint32_t)f < 256) atomic_store(&cache[f], (uint8_t)(yes ? 2 : 1));
    return yes;
}
static int is_d24(int nf) { return nf == NVN_FORMAT_DEPTH24 || nf == NVN_FORMAT_DEPTH24_STENCIL8; }
// The image format for NVN format nf and which of the row's formats it is (HOW_*): the row's own where the device has its sampled use (or depth / stencil attachment use), else
// its substitute, else the format the upload converts or decodes into. 0 when none works
static VkFormat pick_format(int nf, nvn_vk_fmt *row, int *how) {
    opts();
    nvn_fmt_to_vk(nf, row); *how = HOW_NONE;
    VkFormatFeatureFlags2 need = row->cls == NVN_VK_DS ? VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT : VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;
    if (row->path == NVN_VK_PATH_ASTC) {
        if (hvk.dev.astc_ldr && g_opt.astc == ASTC_NATIVE && !g_opt.force_alt && (fmt_features(row->vk, 1) & need)) { *how = HOW_VK; return row->vk; }
        if (fmt_features(row->conv, 1) & need) { *how = HOW_DECODE; return row->conv; }
        return 0;
    }
    int skip = (g_opt.force_alt && (row->flags & NVN_VK_NEEDS_EXT)) || (is_d24(nf) && g_opt.d24_d32f) ||
               (row->vk == VK_FORMAT_A1B5G5R5_UNORM_PACK16 && !hvk.dev.x.maintenance5);   // A1B5G5R5 is maintenance5's
    if (row->vk && !skip && (fmt_features(row->vk, 1) & need)) { *how = HOW_VK; return row->vk; }
    if (row->alt && row->path != NVN_VK_PATH_CONVERT && (fmt_features(row->alt, 1) & need)) { *how = HOW_ALT; return row->alt; }   // a converting row's alt is a vertex format
    if (row->conv && (fmt_features(row->conv, 1) & need)) { *how = HOW_CONV; return row->conv; }
    return 0;
}
static int is_depth_fmt(VkFormat f) { return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }
static int is_stencil_fmt(VkFormat f) { return f == VK_FORMAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }
static VkImageAspectFlags fmt_aspect(VkFormat f) {
    VkImageAspectFlags a = 0;
    if (is_depth_fmt(f)) a |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (is_stencil_fmt(f)) a |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return a ? a : VK_IMAGE_ASPECT_COLOR_BIT;
}
// A format of the same texel size and component layout in another numeric class, for a storage view a shader declares uint / int / float (RGBA8 written as uint, R32F as uint)
static VkFormat class_variant(VkFormat f, uint8_t cls) {
    static const VkFormat fam[][3] = {   // float-ish, uint, sint
        {VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT}, {VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT},
        {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT}, {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT},
        {VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT}, {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT},
        {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT}, {VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT},
        {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT}, {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_UNDEFINED},
    };
    static const VkFormat alias[][2] = {{VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UNORM}, {VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UNORM}, {VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UNORM},
        {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM}, {VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SFLOAT}, {VK_FORMAT_R16_SNORM, VK_FORMAT_R16_SFLOAT},
        {VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SFLOAT}, {VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_SFLOAT}, {VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT},
        {VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_SFLOAT}, {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}};
    VkFormat base = f;
    for (size_t i = 0; i < sizeof alias / sizeof *alias; i++) if (alias[i][0] == f) base = alias[i][1];
    for (size_t i = 0; i < sizeof fam / sizeof *fam; i++)
        for (int c = 0; c < 3; c++) if (fam[i][c] == base) {
            if ((cls == NVN_VK_FLOAT && c == 0) || (cls == NVN_VK_UINT && c == 1) || (cls == NVN_VK_SINT && c == 2)) return f;   // already of that class
            VkFormat v = fam[i][cls == NVN_VK_UINT ? 1 : cls == NVN_VK_SINT ? 2 : 0];
            return v ? v : f;
        }
    return f;
}
static uint8_t fmt_cls(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT:
        case VK_FORMAT_R16G16B16_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32_UINT:
        case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_S8_UINT: return NVN_VK_UINT;
        case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT:
        case VK_FORMAT_R16G16B16_SINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_SINT:
        case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_A2B10G10R10_SINT_PACK32: return NVN_VK_SINT;
        default: return is_depth_fmt(f) || is_stencil_fmt(f) ? NVN_VK_DS : NVN_VK_FLOAT;
    }
}
// The format an attachment of NVN format nf gets, as an image of it is made (pick_format, the texture's own format), and the view's numeric class: what a prewarm row's
// attachments are keyed by (vk_pso.h). VK_FORMAT_UNDEFINED for 0 or a format the device cannot attach
VkFormat hvk_attach_format(int nf, uint8_t *cls);
VkFormat hvk_attach_format(int nf, uint8_t *cls) {
    nvn_vk_fmt row; int how = 0;
    VkFormat f = nf ? pick_format(nf, &row, &how) : VK_FORMAT_UNDEFINED;
    if (cls) *cls = fmt_cls(f);
    return f;
}

// ---------- The texture table ----------
static hvk_tex *tex_find(uint64_t serial) { return serial ? nvn_hm_get(&g_tex, &serial) : NULL; }
static hvk_tex *tex_entry(const tex_t *t) {   // caller holds hvk.tex_mu
    if (!t || !t->serial) return NULL;
    if (!g_tex.klen) nvn_hm_init(&g_tex, 8);
    hvk_tex *x = tex_find(t->serial);
    if (x) return x;
    if (!(x = calloc(1, sizeof *x))) return NULL;
    x->serial = t->serial; x->t = *t; x->live = 1;
    nvn_hm_put(&g_tex, &x->serial, x);
    return x;
}
void hvk_tex_init(const tex_t *t);
void hvk_tex_init(const tex_t *t) { pthread_mutex_lock(&hvk.tex_mu); hvk_tex *x = tex_entry(t); if (x) { x->t = *t; x->live = 1; } pthread_mutex_unlock(&hvk.tex_mu); }
static void mem_count(const hvk_tex *x, int sign) {   // the texture memory counters (be_res_stats' line)
    atomic_fetch_add(&hvk.st_tex_bytes, sign * x->mem_bytes);
    if (x->f.path != NVN_VK_PATH_ASTC) return;
    if (x->astc_dec) { atomic_fetch_add(&hvk.st_astc_decoded_bytes, sign * x->mem_bytes); atomic_fetch_add(&hvk.st_astc_block_bytes, sign * x->astc_block_bytes); }
    else atomic_fetch_add(&hvk.st_astc_native_bytes, sign * x->mem_bytes);
}
static void tex_free(hvk_tex *x) {
    for (int i = 0; i < x->nviews; i++) hvk_defer(HVK_DEL_VIEW, x->views[i].v, 0, 0);
    for (int i = 0; i < 2; i++) if (x->bview[i]) hvk_defer(HVK_DEL_BUFVIEW, x->bview[i], 0, 0);
    if (x->img) { mem_count(x, -1); hvk_defer(HVK_DEL_IMAGE, x->img, (uint64_t)(uintptr_t)x->mem, 0); }
    hvk_defer(HVK_DEL_FREE, x, 0, 0);
}
void hvk_tex_release(tex_t *t);
void hvk_tex_release(tex_t *t) {
    pthread_mutex_lock(&hvk.tex_mu);
    hvk_tex *x = t && t->serial && g_tex.klen ? nvn_hm_del(&g_tex, &t->serial) : NULL;
    if (x) { x->live = 0; tex_free(x); }
    pthread_mutex_unlock(&hvk.tex_mu);
}
void hvk_tex_cpu_written(const tex_t *t);
void hvk_tex_cpu_written(const tex_t *t) {
    pthread_mutex_lock(&hvk.tex_mu);
    hvk_tex *x = tex_entry(t);
    if (x && !x->gpu_written) { x->cpu_content = 1; x->stamp = 0; x->seen = 0; }   // uploaded at its next use
    pthread_mutex_unlock(&hvk.tex_mu);
}
int hvk_tex_gpu_owned(uint64_t serial);
int hvk_tex_gpu_owned(uint64_t serial) {
    pthread_mutex_lock(&hvk.tex_mu); hvk_tex *x = g_tex.klen ? tex_find(serial) : NULL; int r = x && x->gpu_written; pthread_mutex_unlock(&hvk.tex_mu);
    return r;
}
const tex_t *hvk_tex_desc(hvk_tex *t) { return &t->t; }
VkImage hvk_tex_image(hvk_tex *t) { return t->img; }
void hvk_tex_gpu_written(hvk_tex *t) { if (t) { t->gpu_written = 1; t->cpu_content = 0; } }
int hvk_tex_is_buffer(const tex_t *t) { return t && t->d.target == NVN_TEXTURE_TARGET_BUFFER; }
int hvk_tex_d24_substitute(hvk_tex *x) { return x && x->d24sub; }
VkImageAspectFlags hvk_tex_copy_aspect(hvk_tex *x) { return x->t.d.format == NVN_FORMAT_STENCIL8 ? VK_IMAGE_ASPECT_STENCIL_BIT : x->aspect; }
int hvk_tex_info(hvk_tex *t, VkFormat *fmt, VkImageAspectFlags *aspect, uint32_t *w, uint32_t *h, uint32_t *depth, uint32_t *layers, uint32_t *levels, VkSampleCountFlagBits *samples, uint8_t *cls) {
    if (!t || !t->made) return 0;
    if (fmt) *fmt = t->fmt;
    if (aspect) *aspect = t->aspect;
    if (w) *w = t->w;
    if (h) *h = t->h;
    if (depth) *depth = t->d;
    if (layers) *layers = t->layers;
    if (levels) *levels = t->levels;
    if (samples) *samples = t->samples;
    if (cls) *cls = fmt_cls(t->fmt);
    return 1;
}

// ---------- Making the image ----------
static void image_barrier_undefined(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                               VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &b};
    vkCmdPipelineBarrier2(cb, &di);
}
static uint32_t mipdim(uint32_t s, uint32_t l) { s >>= l; return s ? s : 1; }
static void upload(hvk_tex *x, VkCommandBuffer cb);
static void zero_fill(hvk_tex *x, VkCommandBuffer cb) {
    VkImageSubresourceRange rr = {x->aspect, 0, x->levels, 0, x->layers};
    if (x->astc_dec) {   // zero ASTC blocks decode as the error colour: what a new texture of them reads on the hardware and through a native ASTC image
        VkClearColorValue m = {{1, 0, 1, 1}}; vkCmdClearColorImage(cb, x->img, VK_IMAGE_LAYOUT_GENERAL, &m, 1, &rr);
    } else if (x->f.flags & NVN_VK_BLOCK) {   // compressed: no clear command; zero blocks copied in
        int bw, bh, bpb = nvn_fmt_bpb(x->t.d.format, &bw, &bh);
        for (uint32_t l = 0; l < x->levels && bpb; l++) {
            uint32_t w = mipdim(x->w, l), h = mipdim(x->h, l), dd = x->type == VK_IMAGE_TYPE_3D ? mipdim(x->d, l) : 1;
            VkDeviceSize n = (VkDeviceSize)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * dd * (uint32_t)bpb;
            hvk_alloc a; if (!hvk_ring_alloc(n, 16, &a)) return;
            memset(a.cpu, 0, n);
            VkBufferImageCopy c = {a.off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, x->layers}, {0, 0, 0}, {w, h, dd}};
            if (x->layers > 1) { for (uint32_t z = 0; z < x->layers; z++) { c.imageSubresource.baseArrayLayer = z; c.imageSubresource.layerCount = 1; vkCmdCopyBufferToImage(cb, a.buf, x->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c); } }
            else vkCmdCopyBufferToImage(cb, a.buf, x->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        }
    } else if (x->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        VkClearDepthStencilValue z = {0, 0}; vkCmdClearDepthStencilImage(cb, x->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rr);
    } else { VkClearColorValue z = {{0}}; vkCmdClearColorImage(cb, x->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rr); }
}
static int64_t astc_blocks(const hvk_tex *x) {   // the bytes a decoded texture's ASTC blocks take
    int bw, bh; nvn_fmt_bpb(x->t.d.format, &bw, &bh);
    int64_t n = 0;
    for (uint32_t l = 0; l < x->levels; l++)
        n += (int64_t)((mipdim(x->w, l) + bw - 1) / bw) * ((mipdim(x->h, l) + bh - 1) / bh) * 16 * (x->type == VK_IMAGE_TYPE_3D ? mipdim(x->d, l) : 1);
    return n * x->layers;
}
static int tex_make(hvk_tex *x) {
    const tex_t *t = &x->t; const nvn_texdesc *d = &t->d;
    if (d->target == NVN_TEXTURE_TARGET_BUFFER) { x->failed = 1; return 0; }
    VkFormat f = pick_format(d->format, &x->f, &x->how);
    if (!f) {
        hostvk_log("[RED] [hostvk] texture ser=%llu: NVN format %#x (%s) has no Vulkan format this device supports%s%s", (unsigned long long)x->serial, d->format, nvn_fmt_name(d->format),
                   x->f.why ? ": " : "", x->f.why ? x->f.why : "");
        x->failed = 1; return 0;
    }
    x->use_alt = x->how == HOW_ALT; x->convert = x->how == HOW_CONV; x->astc_dec = x->how == HOW_DECODE;
    x->astc_srgb = x->astc_dec && (x->f.flags & NVN_VK_SRGB); x->d24sub = is_d24(d->format) && x->use_alt;
    if (x->use_alt) {
        nvn_fmt_vk_alt_swizzle(d->format, x->alt_swz);
        x->expand3 = (x->f.flags & NVN_VK_EXPAND3) != 0;
        HVK_NOTE_ONCE("texture format substitute (first case: NVN %s → VkFormat %u instead of %u%s%s)", nvn_fmt_name(d->format), (unsigned)f, (unsigned)x->f.vk, x->f.why ? ": " : "", x->f.why ? x->f.why : "");
    }
    if (x->convert) HVK_NOTE_ONCE("texture format converted at upload (first case: NVN %s → VkFormat %u%s%s)", nvn_fmt_name(d->format), (unsigned)f, x->f.why ? ": " : "", x->f.why ? x->f.why : "");
    if (x->astc_dec)
        HVK_NOTE_ONCE("ASTC decoded at upload into RGBA8 (decision D1; first case NVN %s): the device %s textureCompressionASTC_LDR%s", nvn_fmt_name(d->format),
                      hvk.dev.astc_ldr ? "has" : "lacks", hvk.dev.astc_ldr ? ", DM_VK_ASTC=decode or a test asked for the decode" : "");
    if (x->d24sub) HVK_NOTE_ONCE("NVN %s held in VkFormat %u (decision D10): the constant depth bias %s", nvn_fmt_name(d->format), (unsigned)f,
                                 hvk.dev.x.dbc_float ? "in 24-bit units through VK_EXT_depth_bias_control" : "as the game gives it (exact for depths in [0.5, 1))");
    x->fmt = f; x->aspect = fmt_aspect(f);
    uint32_t w = d->width ? d->width : 1, h = d->height ? d->height : 1, dz = d->depth ? d->depth : 1, layers = 1;
    VkImageType type = VK_IMAGE_TYPE_2D; VkImageCreateFlags flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    switch (d->target) {
        case NVN_TEXTURE_TARGET_1D: type = VK_IMAGE_TYPE_1D; h = 1; dz = 1; break;
        case NVN_TEXTURE_TARGET_1D_ARRAY: type = VK_IMAGE_TYPE_1D; layers = h; h = 1; dz = 1; break;
        case NVN_TEXTURE_TARGET_3D: type = VK_IMAGE_TYPE_3D; flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT; break;
        case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: layers = dz; dz = 1; break;
        case NVN_TEXTURE_TARGET_CUBEMAP: layers = 6; dz = 1; break;
        case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: layers = dz; dz = 1; break;
        default: dz = 1; break;   // 2D, RECTANGLE, 2D_MULTISAMPLE
    }
    if (type == VK_IMAGE_TYPE_2D && w == h && layers >= 6) flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;   // a square array of six layers or more can be read as a cube
    if (x->f.flags & NVN_VK_BLOCK) flags |= VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT * 0;   // uncompressed views of compressed images: not in P3 (design 5.5)
    uint32_t levels = d->levels ? d->levels : 1;
    VkSampleCountFlagBits samples = d->samples > 1 ? (VkSampleCountFlagBits)d->samples : VK_SAMPLE_COUNT_1_BIT;
    VkFormatFeatureFlags2 ff = fmt_features(f, 1);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (ff & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if ((ff & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT) && type != VK_IMAGE_TYPE_1D) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ff & VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ((ff & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT) && ((d->flags & NVN_TEXTURE_FLAGS_IMAGE) || x->wants_storage) && samples == VK_SAMPLE_COUNT_1_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if ((d->flags & NVN_TEXTURE_FLAGS_IMAGE) && !(usage & VK_IMAGE_USAGE_STORAGE_BIT))
        HVK_RED_ONCE("texture with the IMAGE flag in VkFormat %u, which this device cannot use as a storage image (first case ser=%llu): its image binds read the dummy", (unsigned)f, (unsigned long long)x->serial);
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, flags, type, f, {w, h, dz}, levels, layers, samples, VK_IMAGE_TILING_OPTIMAL, usage,
                            VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImageFormatProperties ip;
    if (vkGetPhysicalDeviceImageFormatProperties(hvk.dev.phys, f, type, VK_IMAGE_TILING_OPTIMAL, usage, flags, &ip) != VK_SUCCESS || levels > ip.maxMipLevels ||
        layers > ip.maxArrayLayers || w > ip.maxExtent.width || h > ip.maxExtent.height || dz > ip.maxExtent.depth || !(ip.sampleCounts & samples)) {
        // drop the usages the device refuses together with this format and shape, then try once more
        usage &= ~(VkImageUsageFlags)(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        ci.flags &= ~(VkImageCreateFlags)VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; ci.usage = usage;
        if (vkGetPhysicalDeviceImageFormatProperties(hvk.dev.phys, f, type, VK_IMAGE_TILING_OPTIMAL, usage, ci.flags, &ip) != VK_SUCCESS || levels > ip.maxMipLevels ||
            layers > ip.maxArrayLayers || w > ip.maxExtent.width || h > ip.maxExtent.height || dz > ip.maxExtent.depth || !(ip.sampleCounts & samples)) {
            hostvk_log("[RED] [hostvk] texture ser=%llu %ux%ux%u layers %u levels %u samples %u, NVN format %s (VkFormat %u): this device cannot make it", (unsigned long long)x->serial,
                       w, h, dz, layers, levels, (unsigned)samples, nvn_fmt_name(d->format), (unsigned)f);
            x->failed = 1; return 0;
        }
    }
    if (vkCreateImage(hvk.d, &ci, NULL, &x->img)) { hostvk_log("[RED] [hostvk] vkCreateImage failed for texture ser=%llu", (unsigned long long)x->serial); x->img = VK_NULL_HANDLE; x->failed = 1; return 0; }
    VkMemoryRequirements req; vkGetImageMemoryRequirements(hvk.d, x->img, &req);
    uint32_t mt = hvk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, mt};
    if (mt == UINT32_MAX || ++hvk.dev.allocations > hvk.dev.caps.max_allocations || vkAllocateMemory(hvk.d, &ai, NULL, &x->mem)) {
        if (mt != UINT32_MAX) hvk.dev.allocations--;
        hostvk_log("[RED] [hostvk] texture ser=%llu: no memory for %llu bytes", (unsigned long long)x->serial, (unsigned long long)req.size);
        vkDestroyImage(hvk.d, x->img, NULL); x->img = VK_NULL_HANDLE; x->mem = VK_NULL_HANDLE; x->failed = 1; return 0;
    }
    vkBindImageMemory(hvk.d, x->img, x->mem, 0);
    x->type = type; x->flags = ci.flags; x->usage = usage; x->w = w; x->h = h; x->d = dz; x->layers = layers; x->levels = levels; x->samples = samples; x->made = 1;
    x->mem_bytes = (int64_t)req.size; x->astc_block_bytes = x->astc_dec ? astc_blocks(x) : 0; mem_count(x, 1);
    VkCommandBuffer cb = hvk_pre();
    image_barrier_undefined(cb, x->img, x->aspect);
    // a texture the CPU fills is uploaded instead (its whole storage); a linear texture's storage is its content too
    if ((t->has_layout || (d->flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET))) && t->mem && !x->gpu_written) x->cpu_content = 1;
    if (!x->cpu_content || !t->mem) zero_fill(x, cb);
    return 1;
}

// ---------- Conversions on the CPU (the table's paths) ----------
static void expand3(uint8_t *dst, const uint8_t *src, uint32_t n, uint32_t comp_bytes, uint8_t cls, VkFormat f) {
    for (uint32_t i = 0; i < n; i++) {
        memcpy(dst + (size_t)i * 4 * comp_bytes, src + (size_t)i * 3 * comp_bytes, 3 * comp_bytes);
        uint8_t *a = dst + (size_t)i * 4 * comp_bytes + 3 * comp_bytes;
        if (cls != NVN_VK_FLOAT) { memset(a, 0, comp_bytes); a[0] = 1; continue; }   // integer 1
        if (f == VK_FORMAT_R32G32B32A32_SFLOAT) { float one = 1; memcpy(a, &one, 4); }
        else if (f == VK_FORMAT_R16G16B16A16_SFLOAT) { uint16_t one = 0x3c00; memcpy(a, &one, 2); }
        else if (f == VK_FORMAT_R8G8B8A8_SNORM) a[0] = 0x7f;
        else if (f == VK_FORMAT_R16G16B16A16_SNORM) { uint16_t one = 0x7fff; memcpy(a, &one, 2); }
        else memset(a, 0xff, comp_bytes);   // unorm
    }
}
static int32_t sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }
// n texels of NVN format nf (the table's NVN_VK_PATH_CONVERT rows) into their conversion format: the floats an integer-as-float read gives, signed RGB10A2 unpacked
static void convert_texels(int nf, const uint8_t *src, uint8_t *dst, size_t n) {
    if (nf >= 0x51 && nf <= 0x68) {   // *_UI2F / *_I2F: groups of six (8, 16, 32 bits; unsigned, signed) of 1, 2, 3 and 4 components
        int comps = (nf - 0x51) / 6 + 1, k = (nf - 0x51) % 6, size = k < 2 ? 1 : k < 4 ? 2 : 4, sgn = k & 1, oc = comps == 3 ? 4 : comps;
        for (size_t i = 0; i < n; i++) {
            float *o = (float *)(void *)(dst + i * (size_t)oc * 4);
            for (int c = 0; c < comps; c++) {
                const uint8_t *p = src + (i * (size_t)comps + (size_t)c) * (size_t)size; uint32_t v = 0;
                memcpy(&v, p, (size_t)size);
                o[c] = sgn ? (float)sext(v, 8 * size) : (float)v;
            }
            if (comps == 3) o[3] = 1.0f;
        }
        return;
    }
    for (size_t i = 0; i < n; i++) {   // the RGB10A2 family: red in bits 0-9, alpha in 30-31
        uint32_t w; memcpy(&w, src + i * 4, 4);
        uint32_t c[4] = {w & 0x3FF, w >> 10 & 0x3FF, w >> 20 & 0x3FF, w >> 30};
        if (nf == NVN_FORMAT_RGB10A2SN) { float *o = (float *)(void *)(dst + i * 16); for (int k = 0; k < 4; k++) { float v = (float)sext(c[k], k < 3 ? 10 : 2) / (k < 3 ? 511.0f : 1.0f); o[k] = v < -1 ? -1 : v; } }
        else if (nf == NVN_FORMAT_RGB10A2I) { int16_t *o = (int16_t *)(void *)(dst + i * 8); for (int k = 0; k < 4; k++) o[k] = (int16_t)sext(c[k], k < 3 ? 10 : 2); }
        else { float *o = (float *)(void *)(dst + i * 16); for (int k = 0; k < 4; k++) o[k] = nf == NVN_FORMAT_RGB10A2_I2F ? (float)sext(c[k], k < 3 ? 10 : 2) : (float)c[k]; }   // _UI2F, _I2F
    }
}
static uint32_t conv_bpp(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R32_SFLOAT: return 4;
        case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R16G16B16A16_SINT: return 8;
        default: return 16;
    }
}
// Depth and stencil between the guest's packing and the per-aspect planes (the table's NVN_VK_PATH_DEPTH rows; shaders/ds_pack.comp does the same on the GPU)
enum { DS_D24S8, DS_D24S8_F, DS_D24_F, DS_D32FS8, DS_NONE };   // ds_pack.comp's fmt
static int ds_kind(const hvk_tex *x) {
    switch (x->t.d.format) {
        case NVN_FORMAT_DEPTH24_STENCIL8: return x->d24sub ? DS_D24S8_F : DS_D24S8;
        case NVN_FORMAT_DEPTH24: return x->d24sub ? DS_D24_F : DS_NONE;   // X8_D24's depth aspect is the guest's packing
        case NVN_FORMAT_DEPTH32F_STENCIL8: return DS_D32FS8;
        default: return DS_NONE;
    }
}
// D24 ↔ float, exactly as the hardware converts (shaders/ds_pack.comp has the same integer steps): z / (2^24 - 1) rounded to the nearest float, and f * (2^24 - 1) rounded to the
// nearest integer. Float arithmetic would not do: f * 16777215 + 0.5 rounds twice (0.75 gave 12582912 instead of 12582911). For 0 < z < 2^24 - 1 the quotient's mantissa is z
// normalized to 24 bits plus one: what is left after those bits, z' / (2^24 - 1) with z' in [2^23, 2^24 - 2], is always above one half and below one
static float depth_of24(uint32_t z) {
    z &= 0xFFFFFFu;
    if (!z) return 0.0f;
    if (z == 0xFFFFFFu) return 1.0f;
    int b = 31; while (!(z >> b)) b--;
    uint32_t m = (z << (23 - b)) + 1, bits = (uint32_t)(b + 103) << 23 | (m & 0x7FFFFFu);
    float f; memcpy(&f, &bits, 4);
    return f;
}
static uint32_t z24_of(float f) {
    if (!(f > 0)) return 0;
    if (f >= 1) return 0xFFFFFFu;
    uint32_t bits; memcpy(&bits, &f, 4);
    int ex = (int)(bits >> 23), t = 150 - ex;   // f * (2^24 - 1) = a * 2^-t, a = m * (2^24 - 1)
    if (!ex || t >= 49) return 0;
    uint64_t m = (bits & 0x7FFFFFu) | 0x800000u, a = (m << 24) - m;
    return (uint32_t)((a + (1ull << (t - 1))) >> t);
}
static void ds_split(int kind, const uint8_t *g, size_t n, uint32_t *depth, uint8_t *stencil) {
    for (size_t i = 0; i < n; i++) {
        uint32_t g0, g1 = 0; memcpy(&g0, g + i * (kind == DS_D32FS8 ? 8 : 4), 4);
        if (kind == DS_D32FS8) memcpy(&g1, g + i * 8 + 4, 4);
        if (kind == DS_D24S8 || kind == DS_D32FS8) depth[i] = g0; else { float f = depth_of24(g0); memcpy(&depth[i], &f, 4); }
        if (stencil) stencil[i] = (uint8_t)(kind == DS_D32FS8 ? g1 & 0xFF : g0 >> 24);
    }
}
static void ds_join(int kind, const uint32_t *depth, const uint8_t *stencil, size_t n, uint8_t *g) {
    for (size_t i = 0; i < n; i++) {
        uint32_t s = stencil ? stencil[i] : 0, d = depth[i], w; float f; memcpy(&f, &d, 4);
        switch (kind) {
            case DS_D24S8: w = (d & 0xFFFFFFu) | s << 24; memcpy(g + i * 4, &w, 4); break;
            case DS_D24S8_F: w = z24_of(f) | s << 24; memcpy(g + i * 4, &w, 4); break;
            case DS_D24_F: w = z24_of(f); memcpy(g + i * 4, &w, 4); break;
            default: memcpy(g + i * 8, &d, 4); memcpy(g + i * 8 + 4, &s, 4); break;
        }
    }
}

// ---------- Uploads (design 4.3, 5.3) ----------
// The CPU path: each level and layer deswizzled (or copied from linear rows) into the ring, converted when the table's path says so, and copied into the image
static uint64_t upload_cpu(hvk_tex *x, VkCommandBuffer cb) {
    const tex_t *t = &x->t; const nvn_texdesc *d = &t->d;
    int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    int linear = (d->flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) != 0;
    uint64_t bytes = 0;
    for (uint32_t l = 0; l < x->levels; l++) {
        uint32_t w = mipdim(x->w, l), h = mipdim(x->h, l), dz = x->type == VK_IMAGE_TYPE_3D ? mipdim(x->d, l) : 1;
        uint32_t row = (uint32_t)((w + bw - 1) / bw) * (uint32_t)bpb, rows = (h + bh - 1) / bh;
        size_t lin_n = (size_t)row * rows * dz, texels = (size_t)w * h * dz, out_n = lin_n;
        if (x->expand3) out_n = lin_n / 3 * 4;
        else if (x->astc_dec) out_n = texels * 4;
        else if (x->convert) out_n = texels * conv_bpp(x->fmt);
        int direct = !x->expand3 && !x->astc_dec && !x->convert;
        for (uint32_t z = 0; z < (x->type == VK_IMAGE_TYPE_3D ? 1 : x->layers); z++) {
            hvk_alloc a;
            if (!hvk_ring_alloc(out_n, 16, &a)) return bytes;
            uint8_t *lin = direct ? a.cpu : malloc(lin_n);
            if (!lin) return bytes;
            int bad = 0;
            if (linear) {   // linear storage: rows at the texture's stride, pixel rows also for block formats (nvn_tex.c)
                uint32_t stride = d->stride ? d->stride : row;
                if (x->levels > 1 || x->layers > 1) { HVK_RED_ONCE("linear texture with levels or layers (first case ser=%llu): only level 0, layer 0 is uploaded", (unsigned long long)x->serial); if (l || z) bad = 1; }
                for (uint32_t y = 0; !bad && y < rows; y++) memcpy(lin + (size_t)y * row, t->mem + (size_t)y * stride * (uint32_t)bh, row);
            } else bad = nvn_deswizzle(d, l, z, t->mem, lin, row) != 0;
            if (bad) { if (!direct) free(lin); continue; }
            if (x->expand3) expand3(a.cpu, lin, (uint32_t)(lin_n / (size_t)bpb), (uint32_t)bpb / 3, x->f.cls, x->fmt);
            else if (x->astc_dec) {
                for (uint32_t s = 0; s < dz; s++) astc_decode_image(lin + (size_t)s * row * rows, row, w, h, bw, bh, x->astc_srgb, a.cpu + (size_t)s * w * h * 4, (size_t)w * 4);
                atomic_fetch_add(&hvk.st_astc_decodes, 1);
            } else if (x->convert) convert_texels(d->format, lin, a.cpu, texels);
            if (!direct) free(lin);
            VkBufferImageCopy c = {a.off, direct || x->expand3 ? (uint32_t)((row / (uint32_t)bpb) * (uint32_t)bw) : w, direct || x->expand3 ? rows * (uint32_t)bh : h,
                                   {VK_IMAGE_ASPECT_COLOR_BIT, l, z, 1}, {0, 0, 0}, {w, h, dz}};
            if (x->type == VK_IMAGE_TYPE_3D) c.imageSubresource.baseArrayLayer = 0;
            vkCmdCopyBufferToImage(cb, a.buf, x->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            atomic_fetch_add(&hvk.st_up_cpu, 1);
            bytes += out_n;
        }
    }
    return bytes;
}
// The GPU path's ASTC decode (shaders/astc_decode.comp): the blocks of a region at device address src (linear rows src_pitch bytes apart, or block-linear: gobs_x, th, td) into
// the image at (ox, oy) of level / layer, in bands of block rows that fit the scratch, each band's kernel after the previous band's copy
static int astc_gpu(hvk_tex *x, VkCommandBuffer cb, VkDeviceAddress src, int linear, uint32_t src_pitch, uint32_t gobs_x, unsigned th, unsigned td,
                    uint32_t level, uint32_t layer, int32_t ox, int32_t oy, uint32_t w, uint32_t h, uint64_t *bytes) {
    int bw, bh; nvn_fmt_bpb(x->t.d.format, &bw, &bh);
    uint32_t pitch = w * 4, brows = (h + (uint32_t)bh - 1) / (uint32_t)bh, band = g_band / (pitch * (uint32_t)bh);
    if (!band) band = 1;
    if (band > brows) band = brows;
    if ((uint64_t)pitch * band * (uint32_t)bh > HVK_SCRATCH_MAX) return 0;
    VkBuffer sb; VkDeviceAddress sa;
    if (!hvk_scratch((VkDeviceSize)pitch * band * (uint32_t)bh, &sb, &sa)) return 0;
    for (uint32_t y0 = 0; y0 < brows; y0 += band) {
        uint32_t n = brows - y0 < band ? brows - y0 : band, ty = y0 * (uint32_t)bh, eh = n * (uint32_t)bh < h - ty ? n * (uint32_t)bh : h - ty;
        struct { uint64_t src, dst; uint32_t linear, src_pitch, gobs_x, rows, th, td, w, h, bw, bh, srgb, y0, nrows, dst_pitch; } pc =
            {src, sa, (uint32_t)linear, src_pitch, gobs_x, brows, th, td, w, h, (uint32_t)bw, (uint32_t)bh, (uint32_t)x->astc_srgb, y0, n, pitch};
        hvk_full_barrier(cb);   // the band before has been copied out of the scratch
        if (!hvk_kernel_dispatch3(cb, HVK_K_ASTC, &pc, sizeof pc, (w + 7) / 8, (n * (uint32_t)bh + 7) / 8, 1)) return 0;
        hvk_full_barrier(cb);
        VkBufferImageCopy c = {0, w, n * (uint32_t)bh, {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1}, {ox, oy + (int32_t)ty, 0}, {w, eh, 1}};
        vkCmdCopyBufferToImage(cb, sb, x->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        *bytes += (uint64_t)w * eh * 4;
    }
    atomic_fetch_add(&hvk.st_astc_decodes, 1);
    return 1;
}
// The GPU path's deswizzle (shaders/deswizzle.comp): one level / layer's block-linear storage at src into the image, in bands (every slice at once, groups of slices, or block
// rows of one slice) that fit the scratch; the rows padded to 16 bytes and to whole texels
static int deswz_gpu(hvk_tex *x, VkCommandBuffer cb, VkDeviceAddress src, uint32_t row_bytes, uint32_t rows, uint32_t slices, uint32_t gobs_x, unsigned th, unsigned td,
                     uint32_t level, uint32_t layer, uint32_t w, uint32_t h, int bpb, int bw, int bh, uint64_t *bytes) {
    uint32_t unit = bpb % 3 ? 16 : 48, pitch = (row_bytes + unit - 1) / unit * unit;
    uint64_t slice = (uint64_t)pitch * rows;
    uint32_t nz = slice ? (uint32_t)(g_band / slice) : 0, nr = rows;
    if (!nz) { nz = 1; nr = g_band / pitch; if (!nr) nr = 1; }
    if (nz > slices) nz = slices;
    if ((uint64_t)pitch * nr * nz > HVK_SCRATCH_MAX) return 0;
    VkBuffer sb; VkDeviceAddress sa;
    if (!hvk_scratch((VkDeviceSize)pitch * nr * nz, &sb, &sa)) return 0;
    for (uint32_t z0 = 0; z0 < slices; z0 += nz)
        for (uint32_t y0 = 0; y0 < rows; y0 += nr) {
            uint32_t bz = slices - z0 < nz ? slices - z0 : nz, by = rows - y0 < nr ? rows - y0 : nr;
            uint32_t ty = y0 * (uint32_t)bh, eh = by * (uint32_t)bh < h - ty ? by * (uint32_t)bh : h - ty;
            struct { uint64_t src, dst; uint32_t row_bytes, rows, slices, gobs_x, th, td, y0, nrows, z0, nz, dst_pitch; } pc =
                {src, sa, row_bytes, rows, slices, gobs_x, th, td, y0, by, z0, bz, pitch};
            hvk_full_barrier(cb);
            if (!hvk_kernel_dispatch3(cb, HVK_K_DESWIZZLE, &pc, sizeof pc, ((row_bytes + 15) / 16 + 63) / 64, by, bz)) return 0;
            hvk_full_barrier(cb);
            VkBufferImageCopy c = {0, pitch / (uint32_t)bpb * (uint32_t)bw, by * (uint32_t)bh, {VK_IMAGE_ASPECT_COLOR_BIT, level, x->type == VK_IMAGE_TYPE_3D ? 0 : layer, 1},
                                   {0, (int32_t)ty, x->type == VK_IMAGE_TYPE_3D ? (int32_t)z0 : 0}, {w, eh, x->type == VK_IMAGE_TYPE_3D ? bz : 1}};
            vkCmdCopyBufferToImage(cb, sb, x->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            *bytes += (uint64_t)row_bytes * by * bz;
        }
    return 1;
}
// The GPU path: storage read by the GPU from the imported pool (design 4.3). -1 when this texture takes the CPU path (no device address, a CPU conversion, below the size,
// DM_VK_DESWIZZLE=cpu), before anything is recorded
static int64_t upload_gpu(hvk_tex *x, VkCommandBuffer cb) {
    const tex_t *t = &x->t; const nvn_texdesc *d = &t->d;
    if (!hvk.dev.caps.buffer_device_address || g_opt.deswz == DESWZ_CPU || x->expand3 || x->convert) return -1;
    if (g_opt.deswz == DESWZ_AUTO && t->L.size < GPU_MIN_BYTES) return -1;
    if (x->astc_dec && x->type == VK_IMAGE_TYPE_3D) return -1;   // the decode kernel reads slice 0 of a level
    VkBuffer pb; VkDeviceSize poff; VkDeviceAddress pdev;
    if (!hvk_buf((uint64_t)(uintptr_t)t->mem, t->L.size ? t->L.size : 1, &pb, &poff, &pdev) || !pdev) return -1;
    int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    uint64_t bytes = 0;
    if (d->flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) {
        uint32_t row = (uint32_t)((x->w + bw - 1) / bw) * (uint32_t)bpb, rows = (x->h + bh - 1) / bh, stride = d->stride ? d->stride : row;
        if (x->levels > 1 || x->layers > 1 || x->type == VK_IMAGE_TYPE_3D) return -1;
        if (x->astc_dec) { if (!astc_gpu(x, cb, pdev, 1, stride * (uint32_t)bh, 0, 0, 0, 0, 0, 0, 0, x->w, x->h, &bytes)) return -1; }
        else {   // a copy straight from the pool: rows at the texture's stride (pixel rows also for block formats)
            if (stride % (uint32_t)bpb || poff % (VkDeviceSize)bpb) return -1;   // the copy's offset and rows in whole texels (blocks)
            VkBufferImageCopy c = {poff, stride / (uint32_t)bpb * (uint32_t)bw, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {x->w, x->h, 1}};
            vkCmdCopyBufferToImage(cb, pb, x->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            bytes = (uint64_t)row * rows;
        }
        atomic_fetch_add(&hvk.st_up_gpu, 1);
        return (int64_t)bytes;
    }
    uint32_t row_bytes, rows, slices, bpbx; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(d, 0, 0, &row_bytes, &rows, &slices, &base, &th, &td, &bpbx)) return -1;
    for (uint32_t l = 0; l < x->levels; l++)
        for (uint32_t z = 0; z < (x->type == VK_IMAGE_TYPE_3D ? 1 : x->layers); z++) {
            if (nvn_deswizzle_geom(d, l, z, &row_bytes, &rows, &slices, &base, &th, &td, &bpbx)) continue;
            uint32_t gobs = nvn_tex_level_gobs(d, l), gobs_x = gobs > (row_bytes + 63) >> 6 ? gobs : (row_bytes + 63) >> 6;   // GOB columns as laid out
            uint32_t w = mipdim(x->w, l), h = mipdim(x->h, l);
            int ok = x->astc_dec ? astc_gpu(x, cb, pdev + base, 0, 0, gobs_x, th, td, l, z, 0, 0, w, h, &bytes)
                                 : deswz_gpu(x, cb, pdev + base, row_bytes, rows, slices, gobs_x, th, td, l, z, w, h, bpb, bw, bh, &bytes);
            if (!ok) { HVK_RED_ONCE("GPU upload of texture ser=%llu level %u failed (first case): that subresource is not uploaded", (unsigned long long)x->serial, l); continue; }
            atomic_fetch_add(&hvk.st_up_gpu, 1);
        }
    return (int64_t)bytes;
}
// Depth and stencil textures (on the CPU: packaged depth textures are rare): deswizzled, split into the image's aspects where the guest packs them together, converted to float
// where the image is a 32-bit float substitute
static uint64_t upload_ds(hvk_tex *x, VkCommandBuffer cb) {
    const tex_t *t = &x->t; const nvn_texdesc *d = &t->d;
    int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh), kind = ds_kind(x), linear = (d->flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) != 0;
    int stencil_only = d->format == NVN_FORMAT_STENCIL8, has_s = (x->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && (kind == DS_D24S8 || kind == DS_D24S8_F || kind == DS_D32FS8);
    uint64_t bytes = 0;
    for (uint32_t l = 0; l < x->levels; l++)
        for (uint32_t z = 0; z < x->layers; z++) {
            uint32_t w = mipdim(x->w, l), h = mipdim(x->h, l), row = w * (uint32_t)bpb;
            size_t n = (size_t)w * h;
            uint8_t *lin = malloc((size_t)row * h);
            if (!lin) return bytes;
            int bad = 0;
            if (linear) { if (l || z) bad = 1; else for (uint32_t y = 0; y < h; y++) memcpy(lin + (size_t)y * row, t->mem + (size_t)y * (d->stride ? d->stride : row), row); }
            else bad = nvn_deswizzle(d, l, z, t->mem, lin, row) != 0;
            if (bad) { free(lin); continue; }
            VkBufferImageCopy c = {0, w, h, {0, l, z, 1}, {0, 0, 0}, {w, h, 1}};
            hvk_alloc a, s;
            if (kind == DS_NONE) {   // the guest's texels are the aspect's: D16, D32F, X8_D24, S8 (also into D32_SFLOAT_S8_UINT's stencil)
                if (!hvk_ring_alloc((VkDeviceSize)row * h, 16, &a)) { free(lin); return bytes; }
                memcpy(a.cpu, lin, (size_t)row * h);
                c.bufferOffset = a.off; c.imageSubresource.aspectMask = stencil_only ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
                vkCmdCopyBufferToImage(cb, a.buf, x->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            } else {
                if (!hvk_ring_alloc(n * 4, 16, &a) || (has_s && !hvk_ring_alloc(n, 16, &s))) { free(lin); return bytes; }
                ds_split(kind, lin, n, (uint32_t *)(void *)a.cpu, has_s ? s.cpu : NULL);
                c.bufferOffset = a.off; c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                vkCmdCopyBufferToImage(cb, a.buf, x->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
                if (has_s) { c.bufferOffset = s.off; c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT; vkCmdCopyBufferToImage(cb, s.buf, x->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c); }
            }
            free(lin);
            atomic_fetch_add(&hvk.st_up_cpu, 1);
            bytes += (uint64_t)row * h;
        }
    return bytes;
}
static void upload(hvk_tex *x, VkCommandBuffer cb) {
    const tex_t *t = &x->t; const nvn_texdesc *d = &t->d;
    if (!t->mem) return;
    if (x->samples > 1) { HVK_RED_ONCE("upload of a multisampled texture from guest memory (first case ser=%llu): not done", (unsigned long long)x->serial); return; }
    int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    if (!bpb) return;
    uint64_t stamp = nvn_dirty_now(), t0 = plat_now_ns();   // the content's stamp, taken before it is read (nvn_dirty.h)
    int64_t bytes;
    if (x->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) bytes = (int64_t)upload_ds(x, cb);
    else if ((bytes = upload_gpu(x, cb)) < 0) bytes = (int64_t)upload_cpu(x, cb);
    atomic_fetch_add(&hvk.st_upload_ns, plat_now_ns() - t0);
    atomic_fetch_add(x->uploaded ? &hvk.st_reuploads : &hvk.st_uploads, 1); atomic_fetch_add(&hvk.st_upload_bytes, (uint64_t)bytes);
    x->uploaded = 1; x->stamp = stamp; x->seen = stamp;
    if (!x->span) x->span = nvn_dirty_span(t->mem, t->L.size);
    if (x->span) nvn_dirty_protect(t->mem, t->L.size);   // a CPU write after this faults and stamps the pages (nvn_dirty.c): the next use uploads again
    hvk_need_barrier();
}
void hvk_tex_frame_begin(void) {}
hvk_tex *hvk_tex_get(const tex_t *t) {
    if (!t || !t->serial || t->d.target == NVN_TEXTURE_TARGET_BUFFER) return NULL;
    pthread_mutex_lock(&hvk.tex_mu);
    hvk_tex *x = tex_entry(t);
    if (x && !x->made && !x->failed) {
        if (x->t.d.format != t->d.format || x->t.d.width != t->d.width) x->t = *t;   // an entry made by a registration before the texture's own init
        tex_make(x);
    }
    if (x && x->made && x->cpu_content && !x->gpu_written) {
        uint64_t now = nvn_dirty_now();
        if (!x->uploaded || (x->seen != now && (!x->span || nvn_dirty_since_span(x->span, x->t.mem, x->t.L.size, x->stamp)))) upload(x, hvk_pre());
        else x->seen = now;
    }
    pthread_mutex_unlock(&hvk.tex_mu);
    return x && x->made ? x : NULL;
}

// ---------- Views ----------
static VkComponentSwizzle swz_vk(uint8_t s) {
    switch (s) { case NVN_SWZ_ZERO: return VK_COMPONENT_SWIZZLE_ZERO; case NVN_SWZ_ONE: return VK_COMPONENT_SWIZZLE_ONE; case NVN_SWZ_R: return VK_COMPONENT_SWIZZLE_R;
                 case NVN_SWZ_G: return VK_COMPONENT_SWIZZLE_G; case NVN_SWZ_B: return VK_COMPONENT_SWIZZLE_B; default: return VK_COMPONENT_SWIZZLE_A; }
}
// The channel mapping of a sampled view: the view's swizzle if set, else the texture builder's, each channel naming a component of the NVN format, which the format's own mapping
// (alpha 1 for RGBX and the three-component formats, the alt format's reordering) finds in the Vulkan format's components (the Metal backend's tex_by_id does the same)
static void compose_swizzle(const hvk_tex *x, const view_t *v, int nvn_fmt, uint8_t out[4]) {
    uint8_t sw[4] = {NVN_SWZ_R, NVN_SWZ_G, NVN_SWZ_B, NVN_SWZ_A}, fsw[4];
    nvn_vk_fmt row; nvn_fmt_to_vk(nvn_fmt, &row);
    if (x->use_alt && nvn_fmt == x->t.d.format) memcpy(fsw, x->alt_swz, 4);
    else memcpy(fsw, row.swz, 4);
    if (x->use_alt && nvn_fmt == x->t.d.format && x->expand3) memcpy(fsw, row.swz, 4);   // expanded: the four-component format reads as the three-component one (alpha 1)
    if (v && v->set_swizzle) memcpy(sw, v->swz, 4);
    else if (x->t.set_swz) for (int c = 0; c < 4; c++) sw[c] = (uint8_t)(x->t.swz >> (3 * c) & 7);
    for (int c = 0; c < 4; c++) {
        if (sw[c] > NVN_SWZ_A) sw[c] = (uint8_t)(NVN_SWZ_R + c);
        out[c] = sw[c] >= NVN_SWZ_R ? fsw[sw[c] - NVN_SWZ_R] : sw[c];
    }
}
static VkImageViewType type_of_target(int target, uint32_t layers) {
    switch (target) {
        case NVN_TEXTURE_TARGET_1D: return VK_IMAGE_VIEW_TYPE_1D;
        case NVN_TEXTURE_TARGET_1D_ARRAY: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        case NVN_TEXTURE_TARGET_3D: return VK_IMAGE_VIEW_TYPE_3D;
        case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case NVN_TEXTURE_TARGET_CUBEMAP: return VK_IMAGE_VIEW_TYPE_CUBE;
        case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        default: return layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}
int hvk_tex_view_typed(hvk_tex *x, const view_t *v, int kind, int want_type, uint8_t want_cls, hvk_view *out);
int hvk_tex_view_typed(hvk_tex *x, const view_t *v, int kind, int want_type, uint8_t want_cls, hvk_view *out) {
    memset(out, 0, sizeof *out);
    if (!x || !x->made) return 0;
    uint32_t l0 = v && v->set_levels ? v->base_level : 0, nl = v && v->set_levels && v->nlevels ? v->nlevels : x->levels - (l0 < x->levels ? l0 : 0);
    if (l0 >= x->levels) { HVK_RED_ONCE("texture view at level %u of a %u-level texture (first case ser=%llu): not made", l0, x->levels, (unsigned long long)x->serial); return 0; }
    if (nl > x->levels - l0) nl = x->levels - l0;   // cut to the levels the texture has, as deko3d's descriptor writer clips the maximum level
    uint32_t s0 = v && v->set_layers ? v->min_layer : 0;
    uint32_t ns = x->type == VK_IMAGE_TYPE_3D ? 1 : (v && v->set_layers && v->nlayers ? v->nlayers : x->layers - (s0 < x->layers ? s0 : 0));
    int target = v && v->set_target ? v->target : x->t.d.target;
    VkImageViewType vt = type_of_target(target, ns);
    if (kind != HVK_VIEW_SAMPLED) { nl = 1; vt = x->type == VK_IMAGE_TYPE_1D ? (ns > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D) : x->type == VK_IMAGE_TYPE_3D && kind == HVK_VIEW_STORAGE ? VK_IMAGE_VIEW_TYPE_3D : ns > 1 || x->type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D; }
    if (x->type == VK_IMAGE_TYPE_3D && kind == HVK_VIEW_ATTACH) { s0 = v && v->set_layers ? v->min_layer : 0; ns = v && v->set_layers && v->nlayers ? v->nlayers : mipdim(x->d, l0); }   // 3D slices through a 2D array view
    if (want_type >= 0 && (VkImageViewType)want_type != vt) {   // the type the shader declares (design 5.5; the Metal backend's tex_fit)
        VkImageViewType w = (VkImageViewType)want_type;
        if (w == VK_IMAGE_VIEW_TYPE_2D && vt == VK_IMAGE_VIEW_TYPE_2D_ARRAY) { ns = 1; vt = w; }
        else if (w == VK_IMAGE_VIEW_TYPE_2D_ARRAY && vt == VK_IMAGE_VIEW_TYPE_2D) vt = w;
        else if (w == VK_IMAGE_VIEW_TYPE_1D && vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY) { ns = 1; vt = w; }
        else if (w == VK_IMAGE_VIEW_TYPE_1D_ARRAY && vt == VK_IMAGE_VIEW_TYPE_1D) vt = w;
        else if ((w == VK_IMAGE_VIEW_TYPE_CUBE || w == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) && (x->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) && x->layers - s0 >= 6) {
            ns = w == VK_IMAGE_VIEW_TYPE_CUBE ? 6 : (x->layers - s0) / 6 * 6; vt = w; }
        else if (w == VK_IMAGE_VIEW_TYPE_2D_ARRAY && vt == VK_IMAGE_VIEW_TYPE_CUBE) vt = w;
        else if (w == VK_IMAGE_VIEW_TYPE_2D && vt == VK_IMAGE_VIEW_TYPE_CUBE) { ns = 1; vt = w; }
        else return 0;   // no view bridges it: the caller binds the dummy (logged there)
    }
    if ((vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) && ns % 6) {   // a cube view counts its layers in cubes, as the hardware's texture header does (the Metal backend's note)
        if ((s0 + ns) * 6 <= x->layers) { s0 *= 6; ns *= 6; } else vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    }
    if (vt == VK_IMAGE_VIEW_TYPE_2D && ns > 1) ns = 1;
    if (s0 + ns > (x->type == VK_IMAGE_TYPE_3D && kind == HVK_VIEW_ATTACH ? mipdim(x->d, l0) : x->layers) && x->type != VK_IMAGE_TYPE_3D) {
        HVK_RED_ONCE("texture view of layers %u + %u past the %u layers of texture ser=%llu (first case): not made", s0, ns, x->layers, (unsigned long long)x->serial); return 0; }
    VkFormat f = x->fmt; int nvn_fmt = x->t.d.format;
    if (v && v->set_format && v->format != x->t.d.format) {
        nvn_vk_fmt vr; VkFormat vf = 0; nvn_fmt_to_vk(v->format, &vr);
        if (x->astc_dec && vr.path == NVN_VK_PATH_ASTC) {   // a view of a decoded texture: its decode's format
            vf = vr.conv;
            if (!!(vr.flags & NVN_VK_SRGB) != !!x->astc_srgb)
                HVK_RED_ONCE("an sRGB-changing view of an ASTC texture held decoded (first case NVN %s as %s): its texels keep the texture's own decode (the sRGB expansion differs)",
                             nvn_fmt_name(x->t.d.format), nvn_fmt_name(v->format));
        } else if (x->use_alt) vf = vr.alt ? vr.alt : vr.vk;
        else if (x->convert) vf = vr.conv ? vr.conv : vr.vk;
        else vf = vr.vk;
        if (!vf) { HVK_RED_ONCE("texture view format NVN %s has no Vulkan format (first case)", nvn_fmt_name(v->format)); return 0; }
        f = vf; nvn_fmt = v->format;
    }
    VkImageAspectFlags aspect = x->aspect;
    int stencil_mode = v && v->set_dsmode ? v->dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL : x->t.stencil;
    if (x->t.d.format == NVN_FORMAT_STENCIL8 && (aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) stencil_mode = 1;   // STENCIL8 held in D32_SFLOAT_S8_UINT reads its stencil
    if (kind == HVK_VIEW_SAMPLED && (aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
        if (stencil_mode && (aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
        else if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    if (kind == HVK_VIEW_STORAGE && want_cls <= NVN_VK_SINT) f = class_variant(f, want_cls);
    if (kind == HVK_VIEW_ATTACH && aspect == VK_IMAGE_ASPECT_COLOR_BIT && x->use_alt && memcmp(x->alt_swz, (const uint8_t[4]){NVN_SWZ_R, NVN_SWZ_G, NVN_SWZ_B, NVN_SWZ_A}, 4) &&
        !x->expand3 && nvn_fmt == x->t.d.format)
        HVK_RED_ONCE("render target in NVN %s held as VkFormat %u, which is read through a component mapping (first case ser=%llu): an attachment writes the components in the "
                     "substitute's order", nvn_fmt_name(x->t.d.format), (unsigned)x->fmt, (unsigned long long)x->serial);
    if (kind == HVK_VIEW_ATTACH && x->convert)
        HVK_RED_ONCE("render target in NVN %s held as VkFormat %u, converted at upload (first case ser=%llu): what is drawn is not quantized to the NVN format",
                     nvn_fmt_name(x->t.d.format), (unsigned)x->fmt, (unsigned long long)x->serial);
    uint8_t sw[4] = {NVN_SWZ_R, NVN_SWZ_G, NVN_SWZ_B, NVN_SWZ_A};
    if (kind == HVK_VIEW_SAMPLED && aspect == VK_IMAGE_ASPECT_COLOR_BIT) compose_swizzle(x, v, nvn_fmt, sw);
    view_key k = {(uint8_t)kind, (uint8_t)vt, (uint8_t)aspect, {sw[0], sw[1], sw[2], sw[3]}, (uint32_t)f, l0, nl, s0, ns};
    for (int i = 0; i < x->nviews; i++) if (!memcmp(&x->views[i].k, &k, sizeof k)) { out->view = x->views[i].v; goto done; }
    VkImageViewUsageCreateInfo uci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL,
        kind == HVK_VIEW_SAMPLED ? VK_IMAGE_USAGE_SAMPLED_BIT : kind == HVK_VIEW_STORAGE ? VK_IMAGE_USAGE_STORAGE_BIT :
        (aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)};
    if (!(x->usage & uci.usage)) { if (kind != HVK_VIEW_STORAGE) HVK_RED_ONCE("texture ser=%llu cannot be viewed for use %#x (VkFormat %u has not that feature): not made", (unsigned long long)x->serial, (unsigned)uci.usage, (unsigned)x->fmt); return 0; }
    if (f != x->fmt && !(fmt_features(f, 1) & (kind == HVK_VIEW_STORAGE ? VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT : kind == HVK_VIEW_SAMPLED ? VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT : VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT)))
        { HVK_RED_ONCE("texture view in VkFormat %u of an image in %u: the device lacks that use of the view format (first case)", (unsigned)f, (unsigned)x->fmt); return 0; }
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &uci, 0, x->img, vt, f, {swz_vk(sw[0]), swz_vk(sw[1]), swz_vk(sw[2]), swz_vk(sw[3])}, {aspect, l0, nl, s0, ns}};
    if (vkCreateImageView(hvk.d, &vi, NULL, &out->view)) { out->view = VK_NULL_HANDLE; HVK_RED_ONCE("vkCreateImageView failed (first case ser=%llu)", (unsigned long long)x->serial); return 0; }
    if (x->nviews == NVIEW) { hvk_defer(HVK_DEL_VIEW, x->views[0].v, 0, 0); memmove(x->views, x->views + 1, sizeof x->views[0] * (NVIEW - 1)); x->nviews--; }
    x->views[x->nviews].k = k; x->views[x->nviews++].v = out->view;
done:
    out->format = f; out->aspect = aspect; out->level = l0; out->levels = nl; out->layer0 = s0; out->layers = vt == VK_IMAGE_VIEW_TYPE_3D ? 1 : ns;
    out->w = mipdim(x->w, l0); out->h = mipdim(x->h, l0); out->cls = fmt_cls(f); out->samples = x->samples; out->t = x;
    if (kind == HVK_VIEW_SAMPLED && aspect == VK_IMAGE_ASPECT_STENCIL_BIT) out->cls = NVN_VK_UINT;
    return out->view != VK_NULL_HANDLE;
}
int hvk_tex_view(hvk_tex *t, const view_t *v, int kind, hvk_view *out) { return hvk_tex_view_typed(t, v, kind, -1, 0xff, out); }

// ---------- Buffer textures: texel buffers over the pool ----------
VkBufferView hvk_tex_buffer_view(const tex_t *t, int storage, uint8_t *cls) {
    pthread_mutex_lock(&hvk.tex_mu);
    hvk_tex *x = tex_entry(t); VkBufferView r = VK_NULL_HANDLE;
    if (x && x->bview[storage]) { r = x->bview[storage]; if (cls) *cls = x->bcls; pthread_mutex_unlock(&hvk.tex_mu); return r; }
    pthread_mutex_unlock(&hvk.tex_mu);
    if (!x || !t->mem) return VK_NULL_HANDLE;
    nvn_vk_fmt row; nvn_fmt_to_vk(t->d.format, &row);
    VkFormat f = row.vk ? row.vk : row.alt;
    VkFormatFeatureFlags2 need = storage ? VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_BIT : VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT;
    if (!f || !(fmt_features_buf(f) & need)) { HVK_RED_ONCE("buffer texture in NVN %s: no Vulkan texel buffer format for it on this device (first case)", nvn_fmt_name(t->d.format)); return VK_NULL_HANDLE; }
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    uint64_t n = (uint64_t)t->d.width * (uint64_t)(bpb ? bpb : 1);
    VkBuffer buf; VkDeviceSize off;
    if (!hvk_buf((uint64_t)(uintptr_t)t->mem, n, &buf, &off, NULL)) { HVK_RED_ONCE("buffer texture storage %p is in no imported memory pool (first case)", (void *)t->mem); return VK_NULL_HANDLE; }
    VkDeviceSize align = hvk.lim.minTexelBufferOffsetAlignment ? hvk.lim.minTexelBufferOffsetAlignment : 16;
    if (off % align) { HVK_RED_ONCE("buffer texture at offset %llu, not a multiple of the device's %llu (first case): not bound", (unsigned long long)off, (unsigned long long)align); return VK_NULL_HANDLE; }
    uint64_t maxe = hvk.lim.maxTexelBufferElements;
    if (maxe && t->d.width > maxe) { HVK_RED_ONCE("buffer texture of %u elements, past the device's %llu (first case): cut", t->d.width, (unsigned long long)maxe); n = maxe * (uint64_t)bpb; }
    VkBufferViewCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, buf, f, off, n};
    if (vkCreateBufferView(hvk.d, &bi, NULL, &r)) { HVK_RED_ONCE("vkCreateBufferView failed (first case)"); return VK_NULL_HANDLE; }
    pthread_mutex_lock(&hvk.tex_mu);
    if (x->bview[storage]) { vkDestroyBufferView(hvk.d, r, NULL); r = x->bview[storage]; } else { x->bview[storage] = r; x->bcls = fmt_cls(f); }
    if (cls) *cls = x->bcls;
    pthread_mutex_unlock(&hvk.tex_mu);
    return r;
}

// ---------- The registries ----------
typedef struct { tex_t t; view_t v; uint8_t ok, has_v, image; } treg;
static treg *g_treg; static int g_ntreg;
typedef struct { samp_par p; uint8_t ok; } sreg;
static sreg *g_sreg; static int g_nsreg;
static int grow(void **a, int *n, int want, size_t sz) {
    if (want < *n) return 1;
    int m = want + 256; void *p = realloc(*a, (size_t)m * sz);
    if (!p) return 0;
    memset((char *)p + (size_t)*n * sz, 0, (size_t)(m - *n) * sz); *a = p; *n = m; return 1;
}
void hvk_reg_tex(int tid, const tex_t *t, const view_t *v, int image);
void hvk_reg_tex(int tid, const tex_t *t, const view_t *v, int image) {
    if (tid < 0) return;
    pthread_mutex_lock(&hvk.tex_mu);
    if (grow((void **)&g_treg, &g_ntreg, tid, sizeof *g_treg)) {
        treg *r = &g_treg[tid];
        if (!t) memset(r, 0, sizeof *r);
        else {
            r->t = *t; r->ok = 1; r->has_v = v != NULL; r->image = (uint8_t)image; if (v) r->v = *v; else memset(&r->v, 0, sizeof r->v);
            if (image) { hvk_tex *x = tex_entry(t); if (x) { x->wants_storage = 1;
                if (x->made && !(x->usage & VK_IMAGE_USAGE_STORAGE_BIT) && (fmt_features(x->fmt, 1) & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT))
                    HVK_RED_ONCE("texture ser=%llu registered as an image after it was made without storage use (first case): its image binds read the dummy (remaking it with its content is later)", (unsigned long long)t->serial); } }
        }
    }
    pthread_mutex_unlock(&hvk.tex_mu);
}
int hvk_reg_texture(int tid, tex_t *t, view_t *v, int *has_view) {
    int n = atomic_load(&hvk.tex_pool_n);
    if (tid <= 0 || tid >= g_ntreg || (n && tid >= n) || !g_treg[tid].ok) return 0;
    *t = g_treg[tid].t; *v = g_treg[tid].v; *has_view = g_treg[tid].has_v;
    return 1;
}
void hvk_reg_samp(int sid, const samp_par *p);
void hvk_reg_samp(int sid, const samp_par *p) {
    if (sid < 0) return;
    pthread_mutex_lock(&hvk.tex_mu);
    if (grow((void **)&g_sreg, &g_nsreg, sid, sizeof *g_sreg)) { g_sreg[sid].p = *p; g_sreg[sid].ok = 1; }
    pthread_mutex_unlock(&hvk.tex_mu);
}

// ---------- Samplers ----------
typedef struct { samp_par p; uint8_t intb, nearest, pad[6]; } samp_key;   // the parameters, whether the border is read as integers, whether filtering is forced nearest
static nvn_hmap g_samp;   // samp_key → VkSampler (boxed), under hvk.tex_mu
static VkSamplerAddressMode wrap_vk(uint8_t w) {
    switch (w) {
        case NVN_WRAP_MODE_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case NVN_WRAP_MODE_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case NVN_WRAP_MODE_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case NVN_WRAP_MODE_MIRROR_CLAMP: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER:
            if (w == NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER) HVK_RED_ONCE("wrap mode MIRROR_CLAMP_TO_BORDER: Vulkan has no border variant, mirror-clamp to edge is used");
            if (hvk.dev.x.mirror_clamp_to_edge) return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
            HVK_RED_ONCE("wrap mode MIRROR_CLAMP*: the device has no samplerMirrorClampToEdge, mirrored repeat is used");
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case NVN_WRAP_MODE_CLAMP: HVK_NOTE_ONCE("wrap mode CLAMP (GL_CLAMP, the border blended at the edges): clamp to edge, as Vulkan has no such mode"); return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}
static VkCompareOp cmp_vk(uint8_t f) {   // NVN 1 NEVER .. 8 ALWAYS → VkCompareOp 0 .. 7 (the same order)
    return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS;
}
static VkSampler samp_make(const samp_par *p, int intb, int nearest) {
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = p->magf == NVN_MAG_FILTER_LINEAR && !nearest ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = (p->minf == NVN_MIN_FILTER_LINEAR || p->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST || p->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR) && !nearest ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = (p->minf == NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR || p->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR) && !nearest ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = wrap_vk(p->ws); si.addressModeV = wrap_vk(p->wt); si.addressModeW = wrap_vk(p->wr);
    float mb = hvk.lim.maxSamplerLodBias;
    si.mipLodBias = p->lod_bias > mb ? mb : p->lod_bias < -mb ? -mb : p->lod_bias;   // NVN's LOD bias (the Metal backend never applied it, design 5.4)
    if (p->aniso > 1.f && hvk.lim.maxSamplerAnisotropy > 1.f) { si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = p->aniso > hvk.lim.maxSamplerAnisotropy ? hvk.lim.maxSamplerAnisotropy : p->aniso; }
    if (p->cmp_mode == NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE) { si.compareEnable = VK_TRUE; si.compareOp = cmp_vk(p->cmp_func); }
    if (p->minf == NVN_MIN_FILTER_NEAREST || p->minf == NVN_MIN_FILTER_LINEAR) { si.minLod = 0; si.maxLod = 0.25f; }   // no mipmapping: the base level, with the min / mag choice kept (GL's convention)
    else { si.minLod = p->lod_min; si.maxLod = p->lod_max < p->lod_min ? p->lod_min : p->lod_max; if (si.maxLod > VK_LOD_CLAMP_NONE) si.maxLod = VK_LOD_CLAMP_NONE; }
    void *chain = NULL;
    VkSamplerReductionModeCreateInfo rm = {VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO, NULL, VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE};
    if (p->reduction == NVN_SAMPLER_REDUCTION_MIN || p->reduction == NVN_SAMPLER_REDUCTION_MAX) {
        if (hvk.dev.x.sampler_minmax) { rm.reductionMode = p->reduction == NVN_SAMPLER_REDUCTION_MIN ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX; rm.pNext = chain; chain = &rm; }
        else HVK_RED_ONCE("sampler min / max reduction: the device has no samplerFilterMinmax, the weighted average is used");
    }
    VkSamplerCustomBorderColorCreateInfoEXT cb = {VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    int border = si.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || si.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || si.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    const float *bc = p->border;
    int preset = -1;   // 0 transparent black, 1 opaque black, 2 opaque white
    if (bc[0] == 0 && bc[1] == 0 && bc[2] == 0 && bc[3] == 0) preset = 0;
    else if (bc[0] == 0 && bc[1] == 0 && bc[2] == 0 && bc[3] == 1) preset = 1;
    else if (bc[0] == 1 && bc[1] == 1 && bc[2] == 1 && bc[3] == 1) preset = 2;
    static const VkBorderColor pf[3] = {VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK, VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE};
    static const VkBorderColor pi[3] = {VK_BORDER_COLOR_INT_TRANSPARENT_BLACK, VK_BORDER_COLOR_INT_OPAQUE_BLACK, VK_BORDER_COLOR_INT_OPAQUE_WHITE};
    si.borderColor = intb ? VK_BORDER_COLOR_INT_TRANSPARENT_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (border) {
        if (preset >= 0) si.borderColor = intb ? pi[preset] : pf[preset];
        else if (hvk.dev.x.border_without_format) {   // the exact colour (design 5.4): integer textures take the values as integers (hle_nvn.c keeps them as floats)
            si.borderColor = intb ? VK_BORDER_COLOR_INT_CUSTOM_EXT : VK_BORDER_COLOR_FLOAT_CUSTOM_EXT; cb.format = VK_FORMAT_UNDEFINED;
            for (int i = 0; i < 4; i++) { if (intb) cb.customBorderColor.int32[i] = (int32_t)bc[i]; else cb.customBorderColor.float32[i] = bc[i]; }
            cb.pNext = chain; chain = &cb;
        } else {   // the nearest preset (the Metal backend's rule)
            float lum = (bc[0] + bc[1] + bc[2]) / 3;
            si.borderColor = (intb ? pi : pf)[bc[3] < 0.5f ? 0 : lum < 0.5f ? 1 : 2];
            HVK_RED_ONCE("custom border colour %g,%g,%g,%g: the device has no format-free custom border colours, the nearest preset is used", bc[0], bc[1], bc[2], bc[3]);
        }
    }
    si.pNext = chain;
    VkSampler s = VK_NULL_HANDLE;
    if (vkCreateSampler(hvk.d, &si, NULL, &s)) { HVK_RED_ONCE("vkCreateSampler failed (first case)"); return VK_NULL_HANDLE; }
    return s;
}
static VkSampler g_default_samp;
VkSampler hvk_default_sampler(void) {
    if (!g_default_samp) { samp_par p = {0}; p.minf = NVN_MIN_FILTER_NEAREST; p.magf = NVN_MAG_FILTER_NEAREST; p.ws = p.wt = p.wr = NVN_WRAP_MODE_CLAMP_TO_EDGE; p.lod_max = 1000; g_default_samp = samp_make(&p, 0, 1); }
    return g_default_samp;
}
VkSampler hvk_reg_sampler(int sid, uint8_t cls) {
    if (sid <= 0 || sid >= g_nsreg || !g_sreg[sid].ok) return hvk_default_sampler();
    uint8_t c = cls & (uint8_t)~HVK_CLS_NO_LINEAR;
    samp_key k; memset(&k, 0, sizeof k); k.p = g_sreg[sid].p; k.intb = c == NVN_VK_UINT || c == NVN_VK_SINT; k.nearest = k.intb || (cls & HVK_CLS_NO_LINEAR) != 0;
    if (!g_samp.klen) nvn_hm_init(&g_samp, sizeof k);
    VkSampler *have = nvn_hm_get(&g_samp, &k);
    if (have) return *have;
    VkSampler s = samp_make(&k.p, k.intb, k.nearest);
    if (!s) return hvk_default_sampler();
    VkSampler *box = malloc(sizeof *box); if (!box) return s;
    *box = s; nvn_hm_put(&g_samp, &k, box);
    return s;
}

// ---------- Dummies ----------
static struct { VkImage img; VkDeviceMemory mem; VkImageView view; } g_dummy[8][4][2];   // [enum nvn_dtt][NVN_DPF_*][storage]
static struct { VkBuffer buf; VkDeviceMemory mem; VkBufferView view[4][2]; } g_dbuf;
VkImageView hvk_dummy_view(int dtt, int dpf, int storage) {
    dtt &= 7; dpf &= 3; storage = !!storage;
    if (g_dummy[dtt][dpf][storage].view) return g_dummy[dtt][dpf][storage].view;
    if (dtt == NVN_DTT_BUFFER) return VK_NULL_HANDLE;
    int depth = dpf == NVN_DPF_DEPTH && !storage && dtt != NVN_DTT_3D;
    VkFormat f = depth ? VK_FORMAT_D32_SFLOAT : dpf == NVN_DPF_UINT ? (storage ? VK_FORMAT_R32_UINT : VK_FORMAT_R8G8B8A8_UINT) : dpf == NVN_DPF_SINT ? (storage ? VK_FORMAT_R32_SINT : VK_FORMAT_R8G8B8A8_SINT)
                       : (storage ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM);
    VkImageType it = dtt == NVN_DTT_1D || dtt == NVN_DTT_1D_ARRAY ? VK_IMAGE_TYPE_1D : dtt == NVN_DTT_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    int cube = dtt == NVN_DTT_CUBE || dtt == NVN_DTT_CUBE_ARRAY;
    VkImageViewType vt = dtt == NVN_DTT_1D ? VK_IMAGE_VIEW_TYPE_1D : dtt == NVN_DTT_1D_ARRAY ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : dtt == NVN_DTT_2D_ARRAY ? VK_IMAGE_VIEW_TYPE_2D_ARRAY :
                         dtt == NVN_DTT_3D ? VK_IMAGE_VIEW_TYPE_3D : dtt == NVN_DTT_CUBE ? VK_IMAGE_VIEW_TYPE_CUBE : dtt == NVN_DTT_CUBE_ARRAY ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, it, f, {1, 1, 1}, 1, cube ? 6 : 1, VK_SAMPLE_COUNT_1_BIT,
                            VK_IMAGE_TILING_OPTIMAL, (storage ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT) | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(hvk.d, &ci, NULL, &g_dummy[dtt][dpf][storage].img)) return VK_NULL_HANDLE;
    VkImage img = g_dummy[dtt][dpf][storage].img;
    VkMemoryRequirements req; vkGetImageMemoryRequirements(hvk.d, img, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, hvk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    hvk.dev.allocations++;
    if (vkAllocateMemory(hvk.d, &ai, NULL, &g_dummy[dtt][dpf][storage].mem)) { hvk.dev.allocations--; return VK_NULL_HANDLE; }
    vkBindImageMemory(hvk.d, img, g_dummy[dtt][dpf][storage].mem, 0);
    VkImageAspectFlags asp = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkCommandBuffer cb = hvk_pre();
    image_barrier_undefined(cb, img, asp);
    VkImageSubresourceRange rr = {asp, 0, 1, 0, cube ? 6 : 1};
    if (depth) { VkClearDepthStencilValue z = {0, 0}; vkCmdClearDepthStencilImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rr); }
    else { VkClearColorValue z = {{0}}; vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rr); }
    hvk_need_barrier();
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, vt, f, {0}, {asp, 0, 1, 0, cube ? 6 : 1}};
    if (vkCreateImageView(hvk.d, &vi, NULL, &g_dummy[dtt][dpf][storage].view)) g_dummy[dtt][dpf][storage].view = VK_NULL_HANDLE;
    return g_dummy[dtt][dpf][storage].view;
}
VkBufferView hvk_dummy_texel_buffer(int dpf, int storage) {
    dpf &= 3; storage = !!storage;
    if (g_dbuf.view[dpf][storage]) return g_dbuf.view[dpf][storage];
    if (!g_dbuf.buf) {
        void *map;
        if (hvk_buffer_make(256, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT, 1, &g_dbuf.buf, &g_dbuf.mem, &map)) return VK_NULL_HANDLE;
        memset(map, 0, 256);
    }
    VkFormat f = dpf == NVN_DPF_UINT ? VK_FORMAT_R32_UINT : dpf == NVN_DPF_SINT ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_SFLOAT;
    VkBufferViewCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, g_dbuf.buf, f, 0, 256};
    if (vkCreateBufferView(hvk.d, &bi, NULL, &g_dbuf.view[dpf][storage])) g_dbuf.view[dpf][storage] = VK_NULL_HANDLE;
    return g_dbuf.view[dpf][storage];
}

// ---------- CPU reads (ReadTexels) ----------
// A texture the CPU filled has its content in guest memory: read there, deswizzled, on whatever thread asks (the Metal backend's mtl_read_texels does the same). A GPU-owned one is
// copied out of its image into a staging buffer by a one-off command buffer on the queue, after everything submitted before it (the caller has waited for the GPU, as NVN requires);
// a depth-stencil one whose guest packing joins its aspects has both copied and joined (ds_join)
extern int hvk_oneoff_begin(VkCommandBuffer *cb);
extern int hvk_oneoff_end(VkCommandBuffer cb);
void hvk_tex_read(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb || !dst || r->width <= 0 || r->height <= 0) return;
    size_t wb = (size_t)((r->width + bw - 1) / bw) * (size_t)bpb, hb = (size_t)((r->height + bh - 1) / bh), x0 = (size_t)(r->xoffset / bw) * (size_t)bpb, y0 = (size_t)(r->yoffset / bh);
    int nz = r->depth > 0 ? r->depth : 1, is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    pthread_mutex_lock(&hvk.tex_mu);
    hvk_tex *x = tex_entry(t);
    int gpu = x && x->gpu_written && x->made;
    int cpu = x && !gpu && (x->cpu_content || t->has_layout) && t->mem;
    VkImage image = gpu ? x->img : VK_NULL_HANDLE; VkImageAspectFlags asp = gpu ? hvk_tex_copy_aspect(x) : 0;
    int alt = gpu && (x->use_alt || x->expand3 || x->convert || x->astc_dec) && !(asp & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
    int dsk = gpu ? ds_kind(x) : DS_NONE;
    pthread_mutex_unlock(&hvk.tex_mu);
    if (!gpu && !cpu) {   // never written by either: what a new texture holds, zero (when the CPU wrote it without telling NVN, the storage: read it)
        if (t->mem && !(t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET))) cpu = 1;
        else { for (int z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) memset(dst + (size_t)z * img + y * row, 0, wb); return; }
    }
    if (cpu) {
        uint32_t lw = mipdim(t->d.width, level), lh = mipdim(t->d.height, level), lz = is3d ? mipdim(t->d.depth, level) : 1;
        size_t lp = (size_t)((lw + bw - 1) / bw) * (size_t)bpb, lr = (lh + bh - 1) / bh;
        uint8_t *lin = malloc(lp * lr * lz);
        if (!lin) return;
        for (int z = 0; z < nz; z++) {
            unsigned layer = is3d ? 0 : layer0 + (unsigned)r->zoffset + (unsigned)z;
            if ((is3d && z == 0) || !is3d) if (nvn_deswizzle(&t->d, level, layer, t->mem, lin, lp)) { HVK_RED_ONCE("ReadTexels: the storage could not be deswizzled (first case)"); free(lin); return; }
            const uint8_t *src = lin + (is3d ? (size_t)(r->zoffset + z) * lp * lr : 0);
            for (size_t y = 0; y < hb; y++) memcpy(dst + (size_t)z * img + y * row, src + (y0 + y) * lp + x0, wb);
        }
        free(lin);
        return;
    }
    if (alt) { HVK_RED_ONCE("ReadTexels of a GPU-written texture held in a substitute format (first case): its bytes are not in the NVN format's layout"); }
    size_t texels = (size_t)r->width * (size_t)r->height, dplane = dsk != DS_NONE ? (texels * 4 + 15) & ~(size_t)15 : wb * hb, splane = dsk != DS_NONE && dsk != DS_D24_F ? texels : 0;
    VkBuffer sb; VkDeviceMemory sm; void *map; size_t per = (dplane + splane + 15) & ~(size_t)15, n = per * (size_t)nz;   // each copy's offset 16-byte aligned
    if (hvk_buffer_make(n, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1, &sb, &sm, &map)) { HVK_RED_ONCE("ReadTexels: no staging buffer"); return; }
    VkCommandBuffer cb;
    if (hvk_oneoff_begin(&cb)) {
        hvk_full_barrier(cb);
        for (int z = 0; z < nz; z++) {
            VkBufferImageCopy c = {(VkDeviceSize)z * per, (uint32_t)(wb / (size_t)bpb * (size_t)bw), (uint32_t)(hb * (size_t)bh),
                                   {asp & VK_IMAGE_ASPECT_STENCIL_BIT && !(asp & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_STENCIL_BIT : asp & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                                    level, is3d ? 0 : layer0 + (uint32_t)r->zoffset + (uint32_t)z, 1},
                                   {r->xoffset, r->yoffset, is3d ? r->zoffset + z : 0}, {(uint32_t)r->width, (uint32_t)r->height, 1}};
            if (dsk != DS_NONE) { c.bufferRowLength = (uint32_t)r->width; c.bufferImageHeight = (uint32_t)r->height; }
            vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_GENERAL, sb, 1, &c);
            if (splane) { c.bufferOffset = (VkDeviceSize)z * per + dplane; c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT; vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_GENERAL, sb, 1, &c); }
        }
        VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT};
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb, 0, NULL, 0, NULL};
        vkCmdPipelineBarrier2(cb, &di);
        if (hvk_oneoff_end(cb))
            for (int z = 0; z < nz; z++) {
                const uint8_t *base = (uint8_t *)map + (size_t)z * per;
                if (dsk == DS_NONE) { for (size_t y = 0; y < hb; y++) memcpy(dst + (size_t)z * img + y * row, base + y * wb, wb); continue; }
                for (size_t y = 0; y < (size_t)r->height; y++)   // joined into the guest's packing (wb bytes a row: the texels of the region)
                    ds_join(dsk, (const uint32_t *)(const void *)(base + y * (size_t)r->width * 4), splane ? base + dplane + y * (size_t)r->width : NULL, (size_t)r->width, dst + (size_t)z * img + y * row);
            }
    }
    vkDestroyBuffer(hvk.d, sb, NULL); vkFreeMemory(hvk.d, sm, NULL); hvk.dev.allocations--;
}

// ---------- Buffer copies the image's texels cannot take as they are (vk_backend.h hvk_tex_copy_special) ----------
int hvk_tex_copy_special(hvk_tex *x, VkCommandBuffer cb, int to_buffer, uint64_t src, size_t row, size_t img, unsigned level, unsigned layer0, const NVNcopyRegion *r) {
    int dsk = ds_kind(x);
    if (!x->astc_dec && !x->convert && !x->expand3 && dsk == DS_NONE) return 0;
    if (!cb) return 1;   // the probe form: whether this texture's copies come here
    int nz = r->depth > 0 ? r->depth : 1;
    VkBuffer b; VkDeviceSize off; VkDeviceAddress dev = 0;
    if (!hvk_buf(src, img * (size_t)nz, &b, &off, &dev) || !dev) {
        HVK_RED_ONCE("%s copy of NVN %s needs a conversion on the GPU, and its buffer has no device address (first case): skipped", to_buffer ? "texture→buffer" : "buffer→texture", nvn_fmt_name(x->t.d.format));
        return -1;
    }
    if (x->astc_dec) {
        if (to_buffer) { HVK_RED_ONCE("texture→buffer copy of an ASTC texture held decoded (first case NVN %s): the blocks are not re-encoded, skipped", nvn_fmt_name(x->t.d.format)); return -1; }
        uint64_t bytes = 0;
        for (int z = 0; z < nz; z++)
            if (!astc_gpu(x, cb, dev + (VkDeviceAddress)z * img, 1, (uint32_t)row, 0, 0, 0, level, layer0 + (uint32_t)r->zoffset + (uint32_t)z, r->xoffset, r->yoffset,
                          (uint32_t)r->width, (uint32_t)r->height, &bytes)) { HVK_RED_ONCE("buffer→texture copy into a decoded ASTC texture: the decode failed (first case)"); return -1; }
        return 1;
    }
    if (dsk == DS_NONE) {
        HVK_RED_ONCE("%s copy of NVN %s, held in VkFormat %u converted at upload (first case): not converted, skipped", to_buffer ? "texture→buffer" : "buffer→texture", nvn_fmt_name(x->t.d.format), (unsigned)x->fmt);
        return -1;
    }
    if ((src | row | img) & 3) { HVK_RED_ONCE("depth-stencil buffer copy at an address or stride not a multiple of 4 (first case): skipped"); return -1; }
    uint32_t w = (uint32_t)r->width, h = (uint32_t)r->height, drow = (w * 4 + 15) & ~15u, srow = (w + 15) & ~15u;
    int has_s = dsk != DS_D24_F;
    hvk_alloc dp, sp;
    if (!hvk_ring_alloc((VkDeviceSize)drow * h * (uint32_t)nz, 16, &dp) || (has_s && !hvk_ring_alloc((VkDeviceSize)srow * h * (uint32_t)nz, 16, &sp))) return -1;
    if (!has_s) sp = dp;
    struct { uint64_t guest, dpl, spl; uint32_t w, h, nz, grow, gimg, drow, srow, mode, fmt; } pc = {dev, dp.dev, sp.dev, w, h, (uint32_t)nz, (uint32_t)row, (uint32_t)img, drow, srow, (uint32_t)to_buffer, (uint32_t)dsk};
    VkBufferImageCopy c[2] = {{dp.off, drow / 4, h, {VK_IMAGE_ASPECT_DEPTH_BIT, level, layer0 + (uint32_t)r->zoffset, (uint32_t)nz}, {r->xoffset, r->yoffset, 0}, {w, h, 1}},
                              {sp.off, srow, h, {VK_IMAGE_ASPECT_STENCIL_BIT, level, layer0 + (uint32_t)r->zoffset, (uint32_t)nz}, {r->xoffset, r->yoffset, 0}, {w, h, 1}}};
    uint32_t groups[3] = {((w + 3) / 4 + 15) / 16, (h + 3) / 4, (uint32_t)nz};
    hvk_full_barrier(cb);
    if (to_buffer) {   // the image's aspects into the planes, joined into the guest's texels
        vkCmdCopyImageToBuffer(cb, x->img, VK_IMAGE_LAYOUT_GENERAL, dp.buf, 1, &c[0]);
        if (has_s) vkCmdCopyImageToBuffer(cb, x->img, VK_IMAGE_LAYOUT_GENERAL, sp.buf, 1, &c[1]);
        hvk_full_barrier(cb);
        if (!hvk_kernel_dispatch3(cb, HVK_K_DSPACK, &pc, sizeof pc, groups[0], groups[1], groups[2])) return -1;
    } else {           // the guest's texels split into the planes, copied into the aspects
        if (!hvk_kernel_dispatch3(cb, HVK_K_DSPACK, &pc, sizeof pc, groups[0], groups[1], groups[2])) return -1;
        hvk_full_barrier(cb);
        vkCmdCopyBufferToImage(cb, dp.buf, x->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c[0]);
        if (has_s) vkCmdCopyBufferToImage(cb, sp.buf, x->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c[1]);
    }
    return 1;
}

// ---------- For tests (dm/gfx/tests/vk) ----------
// The image of a texture object, made (and uploaded) as for a use, and its format and aspects; VK_NULL_HANDLE when the backend cannot make it
VkImage hvk_test_image(const tex_t *t, VkFormat *fmt, VkImageAspectFlags *aspect);
VkImage hvk_test_image(const tex_t *t, VkFormat *fmt, VkImageAspectFlags *aspect) {
    extern void hvk_sub_begin(void); extern uint64_t hvk_sub_commit(void); extern void hvk_finish(void);
    int own = !hvk_main();   // a texture made outside a submission: its creation (and zero fill) needs one
    if (own) hvk_sub_begin();
    hvk_tex *x = hvk_tex_get(t);
    if (own) { hvk_sub_commit(); hvk_finish(); }
    if (!x) return VK_NULL_HANDLE;
    if (fmt) *fmt = x->fmt;
    if (aspect) *aspect = x->aspect;
    return x->img;
}
// The texture memory counters: all images, ASTC held compressed, ASTC decodes, the block bytes of those; and the uploads by path and the ASTC subresources decoded
void hvk_test_tex_mem(int64_t mem[4], uint64_t up[3]);
void hvk_test_tex_mem(int64_t mem[4], uint64_t up[3]) {
    if (mem) { mem[0] = atomic_load(&hvk.st_tex_bytes); mem[1] = atomic_load(&hvk.st_astc_native_bytes); mem[2] = atomic_load(&hvk.st_astc_decoded_bytes); mem[3] = atomic_load(&hvk.st_astc_block_bytes); }
    if (up) { up[0] = atomic_load(&hvk.st_up_gpu); up[1] = atomic_load(&hvk.st_up_cpu); up[2] = atomic_load(&hvk.st_astc_decodes); }
}
void hvk_tex_shutdown(void) {
    pthread_mutex_lock(&hvk.tex_mu);
    for (uint32_t i = 0; i < g_tex.cap; i++) if (g_tex.vals && g_tex.vals[i]) tex_free(g_tex.vals[i]);
    nvn_hm_clear(&g_tex, NULL);
    for (uint32_t i = 0; i < g_samp.cap; i++) if (g_samp.vals && g_samp.vals[i]) { vkDestroySampler(hvk.d, *(VkSampler *)g_samp.vals[i], NULL); free(g_samp.vals[i]); }
    nvn_hm_clear(&g_samp, NULL);
    if (g_default_samp) vkDestroySampler(hvk.d, g_default_samp, NULL);
    g_default_samp = VK_NULL_HANDLE;
    for (int a = 0; a < 8; a++) for (int b = 0; b < 4; b++) for (int c = 0; c < 2; c++) {
        if (g_dummy[a][b][c].view) vkDestroyImageView(hvk.d, g_dummy[a][b][c].view, NULL);
        if (g_dummy[a][b][c].img) vkDestroyImage(hvk.d, g_dummy[a][b][c].img, NULL);
        if (g_dummy[a][b][c].mem) { vkFreeMemory(hvk.d, g_dummy[a][b][c].mem, NULL); hvk.dev.allocations--; }
    }
    memset(g_dummy, 0, sizeof g_dummy);
    for (int b = 0; b < 4; b++) for (int c = 0; c < 2; c++) if (g_dbuf.view[b][c]) vkDestroyBufferView(hvk.d, g_dbuf.view[b][c], NULL);
    if (g_dbuf.buf) { vkDestroyBuffer(hvk.d, g_dbuf.buf, NULL); vkFreeMemory(hvk.d, g_dbuf.mem, NULL); hvk.dev.allocations--; }
    memset(&g_dbuf, 0, sizeof g_dbuf);
    free(g_treg); g_treg = NULL; g_ntreg = 0; free(g_sreg); g_sreg = NULL; g_nsreg = 0;
    pthread_mutex_unlock(&hvk.tex_mu);
}
