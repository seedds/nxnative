// The Vulkan backend's textures and samplers (docs/design/vulkan-backend.md 5.1, 5.3, 5.4, 5.5). A texture object's host texture is a VkImage with optimal tiling, made the first
// time the GPU uses it, in the format of the NVN → Vulkan table (dm/gfx/tex/nvn_fmt_vk.c, checked against the device's format features). Its content:
// - a texture the CPU fills (a packaged layout, be_tex_cpu_written, linear storage) is deswizzled on the CPU (dm/gfx/tex/nvn_deswizzle.c) into the ring and copied into the image in
//   the submission's prologue; its storage is then write-protected (dm/gfx/nvn/nvn_dirty.c) and it is uploaded again when its pages change, until the GPU writes it;
// - every other texture starts at zero (the image is cleared when it is made), as a new Metal texture does (t_clear_ds's last case);
// - what the GPU writes (attachments, copies, clears, storage images) stays in the image: the texture is GPU-owned from then on and never uploaded again. A CPU read
//   (be_read_texels) reads the storage of a CPU-filled texture and the image of a GPU-owned one.
// Images stay in VK_IMAGE_LAYOUT_GENERAL (one transition from UNDEFINED when made): every use is valid in it, and the full barriers between operations (vk_commands.c, design 4.9)
// order their accesses. Usage: transfer, sampled, attachment and storage wherever the format allows (attachment use costs compression on some GPUs: to be measured per vendor,
// design 5.5); storage needs the IMAGE flag or a registration as an image before the texture is first used.
// Views: by kind (sampled with the composed channel mapping, attachment and storage with the identity), level and layer range, format and type; the type a shader declares wins
// (a 2D array sampled as 2D reads layer 0, a six-layer square 2D array can be a cube), as the Metal backend's tex_fit does. Buffer textures are texel buffer views over the pool.
// Samplers from NVN's sampler parameters: exact border colours with VK_EXT_custom_border_color, integer borders for integer textures (a sampler per numeric class; hle_nvn.c stores
// integer border values as floats, nvnSamplerBuilderSetBorderColori), min / max reduction, LOD bias and clamps, anisotropy, depth compare.
// Declared, with a red line: uploads of depth-stencil textures (D24S8's packing, phase P7), the GPU deswizzle (design 5.3, later), ASTC on devices without it (decision D1),
// a texture registered as an image after it was made without storage use, linear textures the GPU renders into (not written back to their storage).
#include "plat/plat.h"
#include "vk_backend.h"
#include "../nvn/nvn_dirty.h"
#include "../nvn/nvn_refl_textype.h"
#include "../tex/nvn_deswizzle.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { NVIEW = 24 };
typedef struct { uint8_t kind, type, aspect, swz[4]; uint32_t fmt, level, nlevels, layer0, nlayers; } view_key;
struct hvk_tex {
    uint64_t serial;
    tex_t t;                   // the descriptor as the texture was made (storage, swizzle, flags)
    int live, made, failed, wants_storage;
    VkImage img; VkDeviceMemory mem;
    nvn_vk_fmt f; int use_alt; uint8_t alt_swz[4];
    VkFormat fmt; VkImageAspectFlags aspect; VkImageType type; VkImageCreateFlags flags; VkImageUsageFlags usage;
    uint32_t w, h, d, layers, levels; VkSampleCountFlagBits samples;
    int cpu_content, gpu_written, uploaded, expand3;
    uint64_t stamp, seen; const void *span;
    struct { view_key k; VkImageView v; } views[NVIEW]; int nviews;
    VkBufferView bview[2]; uint8_t bcls;   // a buffer texture's texel buffer views (sampled, storage)
};
static nvn_hmap g_tex;   // serial → hvk_tex, under hvk.tex_mu

// ---------- Formats ----------
static VkFormatFeatureFlags2 fmt_features(VkFormat f, int optimal) {
    VkFormatProperties3 p3 = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
    VkFormatProperties2 p = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &p3};
    vkGetPhysicalDeviceFormatProperties2(hvk.dev.phys, f, &p);
    return optimal ? p3.optimalTilingFeatures : p3.bufferFeatures;
}
static VkFormatFeatureFlags2 fmt_features_buf(VkFormat f) { return fmt_features(f, 0); }
// The image format for NVN format nf: the table's, or its alt when the device lacks the sampled use of the first (or depth / stencil attachment use). 0 when neither works
static VkFormat pick_format(int nf, nvn_vk_fmt *row, int *alt) {
    nvn_fmt_to_vk(nf, row); *alt = 0;
    VkFormatFeatureFlags2 need = row->cls == NVN_VK_DS ? VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT : VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;
    if (row->vk && row->vk >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && row->vk <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK && !hvk.dev.astc_ldr) return 0;
    if (row->vk == VK_FORMAT_A1B5G5R5_UNORM_PACK16) {   // needs VK_KHR_maintenance5, which the device layer does not enable in P3: the table's alt
        if (row->alt && (fmt_features(row->alt, 1) & need)) { *alt = 1; return row->alt; }
        return 0;
    }
    if (row->vk && (fmt_features(row->vk, 1) & need)) return row->vk;
    if (row->alt && (fmt_features(row->alt, 1) & need)) { *alt = 1; return row->alt; }
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
static void tex_free(hvk_tex *x) {
    for (int i = 0; i < x->nviews; i++) hvk_defer(HVK_DEL_VIEW, x->views[i].v, 0, 0);
    for (int i = 0; i < 2; i++) if (x->bview[i]) hvk_defer(HVK_DEL_BUFVIEW, x->bview[i], 0, 0);
    if (x->img) hvk_defer(HVK_DEL_IMAGE, x->img, (uint64_t)(uintptr_t)x->mem, 0);
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
    if (x->f.flags & NVN_VK_BLOCK) {   // compressed: no clear command; zero blocks copied in
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
static int tex_make(hvk_tex *x) {
    const tex_t *t = &x->t; const nvn_texdesc *d = &t->d;
    if (d->target == NVN_TEXTURE_TARGET_BUFFER) { x->failed = 1; return 0; }
    VkFormat f = pick_format(d->format, &x->f, &x->use_alt);
    if (!f) {
        hostvk_log("[RED] [hostvk] texture ser=%llu: NVN format %#x (%s) has no Vulkan format this device supports%s%s", (unsigned long long)x->serial, d->format, nvn_fmt_name(d->format),
                   x->f.why ? ": " : "", x->f.why ? x->f.why : "");
        x->failed = 1; return 0;
    }
    if (x->use_alt) {
        nvn_fmt_vk_alt_swizzle(d->format, x->alt_swz);
        x->expand3 = (x->f.flags & NVN_VK_EXPAND3) != 0;
        HVK_NOTE_ONCE("texture format substitute (first case: NVN %s → VkFormat %u instead of %u%s%s)", nvn_fmt_name(d->format), (unsigned)f, (unsigned)x->f.vk, x->f.why ? ": " : "", x->f.why ? x->f.why : "");
    }
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
    VkCommandBuffer cb = hvk_pre();
    image_barrier_undefined(cb, x->img, x->aspect);
    // a texture the CPU fills is uploaded instead (its whole storage); a linear texture's storage is its content too
    if ((t->has_layout || (d->flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET))) && t->mem && !x->gpu_written) x->cpu_content = 1;
    if (!x->cpu_content || !t->mem) zero_fill(x, cb);
    return 1;
}

// ---------- Uploads (design 4.3, 5.3: the CPU deswizzle) ----------
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
static void upload(hvk_tex *x, VkCommandBuffer cb) {
    const tex_t *t = &x->t; const nvn_texdesc *d = &t->d;
    if (!t->mem) return;
    if (x->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) && (x->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
        HVK_RED_ONCE("upload of a depth-stencil texture from guest memory (first case ser=%llu, NVN %s): the packing of stencil with depth is not converted (phase P7); left as it is",
                     (unsigned long long)x->serial, nvn_fmt_name(d->format));
        return;
    }
    if (x->samples > 1) { HVK_RED_ONCE("upload of a multisampled texture from guest memory (first case ser=%llu): not done", (unsigned long long)x->serial); return; }
    int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    if (!bpb) return;
    uint64_t stamp = nvn_dirty_now();   // the content's stamp, taken before it is read (nvn_dirty.h)
    int linear = (d->flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) != 0;
    uint64_t bytes = 0;
    for (uint32_t l = 0; l < x->levels; l++) {
        uint32_t w = mipdim(x->w, l), h = mipdim(x->h, l), dz = x->type == VK_IMAGE_TYPE_3D ? mipdim(x->d, l) : 1;
        uint32_t row = (uint32_t)((w + bw - 1) / bw) * (uint32_t)bpb, rows = (h + bh - 1) / bh;
        size_t lin_n = (size_t)row * rows * dz;
        uint32_t out_bpb = x->expand3 ? (uint32_t)bpb / 3 * 4 : (uint32_t)bpb;
        for (uint32_t z = 0; z < (x->type == VK_IMAGE_TYPE_3D ? 1 : x->layers); z++) {
            hvk_alloc a; size_t out_n = x->expand3 ? lin_n / 3 * 4 : lin_n;
            if (!hvk_ring_alloc(out_n, 16, &a)) return;
            uint8_t *lin = x->expand3 ? malloc(lin_n) : a.cpu;
            if (!lin) return;
            int bad = 0;
            if (linear) {   // linear storage: rows at the texture's stride, pixel rows also for block formats (nvn_tex.c)
                uint32_t stride = d->stride ? d->stride : row;
                if (x->levels > 1 || x->layers > 1) { HVK_RED_ONCE("linear texture with levels or layers (first case ser=%llu): only level 0, layer 0 is uploaded", (unsigned long long)x->serial); if (l || z) bad = 1; }
                for (uint32_t y = 0; !bad && y < rows; y++) memcpy(lin + (size_t)y * row, t->mem + (size_t)y * stride * (uint32_t)bh, row);
            } else bad = nvn_deswizzle(d, l, z, t->mem, lin, row) != 0;
            if (bad) { if (x->expand3) free(lin); continue; }
            if (x->expand3) { expand3(a.cpu, lin, (uint32_t)(lin_n / bpb), (uint32_t)bpb / 3, x->f.cls, x->fmt); free(lin); }
            VkBufferImageCopy c = {a.off, (uint32_t)((row / bpb) * bw), rows * (uint32_t)bh, {x->aspect & ~(VkImageAspectFlags)VK_IMAGE_ASPECT_STENCIL_BIT, l, z, 1}, {0, 0, 0}, {w, h, dz}};
            if (x->type == VK_IMAGE_TYPE_3D) c.imageSubresource.baseArrayLayer = 0;
            (void)out_bpb;
            vkCmdCopyBufferToImage(cb, a.buf, x->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            bytes += out_n;
        }
    }
    atomic_fetch_add(x->uploaded ? &hvk.st_reuploads : &hvk.st_uploads, 1); atomic_fetch_add(&hvk.st_upload_bytes, bytes);
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
// (alpha 1 for RGBX and the like, the alt format's reordering) finds in the Vulkan format's components (the Metal backend's tex_by_id does the same)
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
        nvn_vk_fmt vr; int alt = 0; VkFormat vf = 0; nvn_fmt_to_vk(v->format, &vr);
        if (!x->use_alt) vf = vr.vk; else { vf = vr.alt ? vr.alt : vr.vk; alt = 1; }
        (void)alt;
        if (!vf) { HVK_RED_ONCE("texture view format NVN %s has no Vulkan format (first case)", nvn_fmt_name(v->format)); return 0; }
        f = vf; nvn_fmt = v->format;
    }
    VkImageAspectFlags aspect = x->aspect;
    int stencil_mode = v && v->set_dsmode ? v->dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL : x->t.stencil;
    if (kind == HVK_VIEW_SAMPLED && (aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
        if (stencil_mode && (aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
        else if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    if (kind == HVK_VIEW_STORAGE && want_cls <= NVN_VK_SINT) f = class_variant(f, want_cls);
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
    out->format = f; out->aspect = aspect; out->level = l0; out->layer0 = s0; out->layers = vt == VK_IMAGE_VIEW_TYPE_3D ? 1 : ns;
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
typedef struct { samp_par p; uint8_t intb, pad[7]; } samp_key;   // the parameters and whether the border is read as integers
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
static VkSampler samp_make(const samp_par *p, int intb) {
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = p->magf == NVN_MAG_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = p->minf == NVN_MIN_FILTER_LINEAR || p->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST || p->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = p->minf == NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR || p->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
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
    if (!g_default_samp) { samp_par p = {0}; p.minf = NVN_MIN_FILTER_NEAREST; p.magf = NVN_MAG_FILTER_NEAREST; p.ws = p.wt = p.wr = NVN_WRAP_MODE_CLAMP_TO_EDGE; p.lod_max = 1000; g_default_samp = samp_make(&p, 0); }
    return g_default_samp;
}
VkSampler hvk_reg_sampler(int sid, uint8_t cls) {
    if (sid <= 0 || sid >= g_nsreg || !g_sreg[sid].ok) return hvk_default_sampler();
    samp_key k; memset(&k, 0, sizeof k); k.p = g_sreg[sid].p; k.intb = cls == NVN_VK_UINT || cls == NVN_VK_SINT;
    if (!g_samp.klen) nvn_hm_init(&g_samp, sizeof k);
    VkSampler *have = nvn_hm_get(&g_samp, &k);
    if (have) return *have;
    VkSampler s = samp_make(&k.p, k.intb);
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
// copied out of its image into a staging buffer by a one-off command buffer on the queue, after everything submitted before it (the caller has waited for the GPU, as NVN requires)
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
    VkImage image = gpu ? x->img : VK_NULL_HANDLE; VkImageAspectFlags asp = gpu ? x->aspect : 0; int alt = gpu && (x->use_alt || x->expand3);
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
    VkBuffer sb; VkDeviceMemory sm; void *map; size_t n = wb * hb * (size_t)nz;
    if (hvk_buffer_make(n, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1, &sb, &sm, &map)) { HVK_RED_ONCE("ReadTexels: no staging buffer"); return; }
    VkCommandBuffer cb;
    if (hvk_oneoff_begin(&cb)) {
        hvk_full_barrier(cb);
        for (int z = 0; z < nz; z++) {
            VkBufferImageCopy c = {(VkDeviceSize)z * wb * hb, (uint32_t)(wb / (size_t)bpb * (size_t)bw), (uint32_t)(hb * (size_t)bh),
                                   {asp & VK_IMAGE_ASPECT_STENCIL_BIT && !(asp & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_STENCIL_BIT : asp & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                                    level, is3d ? 0 : layer0 + (uint32_t)r->zoffset + (uint32_t)z, 1},
                                   {r->xoffset, r->yoffset, is3d ? r->zoffset + z : 0}, {(uint32_t)r->width, (uint32_t)r->height, 1}};
            vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_GENERAL, sb, 1, &c);
        }
        VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT};
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb, 0, NULL, 0, NULL};
        vkCmdPipelineBarrier2(cb, &di);
        if (hvk_oneoff_end(cb))
            for (int z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) memcpy(dst + (size_t)z * img + y * row, (uint8_t *)map + (size_t)z * wb * hb + y * wb, wb);
    }
    vkDestroyBuffer(hvk.d, sb, NULL); vkFreeMemory(hvk.d, sm, NULL); hvk.dev.allocations--;
}

// For tests (dm/gfx/tests/vk): the image of a texture object, made (and uploaded) as for a use, and its format and aspects; VK_NULL_HANDLE when the backend cannot make it
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
