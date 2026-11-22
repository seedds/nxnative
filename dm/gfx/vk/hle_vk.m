// Vulkan → Metal layer for games that render with Vulkan (Hades: The Forge renderer). The game's vk* imports bind to the functions here through the host's import
// table (hle.h swhle); the Vulkan loader (subsdk0) and NVIDIA's driver (subsdk1) are not loaded. vkGet{Instance,Device}ProcAddr look names up in the same table
// (a name ending in KHR is also tried without it, for extensions promoted to core 1.1); every name the game asks for is logged once.
// Model:
//   · one physical device shaped like the Switch's Tegra X1: Vulkan 1.1, one queue family, 3 memory types on one heap;
//   · device memory = one shared-storage MTLBuffer, created on the first buffer bind or map (memory that only backs images never gets one); images are
//     standalone private MTLTextures and ignore their memory binding, except linear 2D images, whose texture is made over the bound buffer;
//   · command buffers are recorded into a host list and replayed at vkQueueSubmit into one MTLCommandBuffer on this layer's own queue, which then signals g_ev
//     with the next value; fences complete from the completion handler; semaphores and pipeline barriers do nothing (one serial queue, Metal tracks hazards); events are
//     set by the host or by a submit (vkCmdWaitEvents waits for nothing: see docs/reference/rendering.md, Vulkan layer);
//     queries are results of their submit: timestamps its completion time, occlusion counts from a visibility buffer;
//   · shaders come from the offline library built by dm/gfx/tools/vk_shaders.py (<DM_MSL_DIR>/vk/): a module is found by the hash of its SPIR-V, and its table
//     row says which Metal slot each (set, binding) went to (an array of n descriptors: n slots from there; push constants: their own buffer slot);
//     vertex buffers sit at slot 30 - binding;
//     Compute shaders are kernels dispatched at their table row's local size on the same command buffer's compute encoder;
//   · swapchain images are registered with the NVN layer's present path (mtl_ext_tex), which shows a frame once g_ev reaches the value of the frame's last submit.
// Environment: DM_VK_LOG=1 logs object creation; DM_VK_CAPTURE=<dir> writes every SPIR-V module the game creates (sh_capture).
#define VK_NO_PROTOTYPES
#include "../../host/hle.h"
#include "../nvn/nvn_mtl.h"
#include "../nvn/nvn_dbnd.h"
#import "../nvn/host_msl.h"
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_vi.h>
#import <Metal/Metal.h>
#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <math.h>
#include <stdatomic.h>
#include <time.h>

int mtl_fps_target(void);   // nvn_mtl.m: 60, or 30 when the frame rate is halved
int mtl_link_active(void); int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns);   // nvn_mtl.m: whether the display link presents, and the wait for its ticks
void dm_vi_layer_crop(int c[4]);   // hle_vi.c: x, y, w, h the game last cropped its layer to (all 0 = none)

#define VLOG(...) LOG("[vk] " __VA_ARGS__)
#define RED1(...) do { static int once_; if (!once_) { once_ = 1; LOG("[vk] [RED] " __VA_ARGS__); } } while (0)
static int g_log;
#define OLOG(...) do { if (g_log) VLOG(__VA_ARGS__); } while (0)

// VK: one Vulkan function, checked against its PFN_ type and registered in the host's import table under its own name
#define VK(name, ret, ...)                                                                       \
    static ret h_##name(__VA_ARGS__);                                                            \
    static const PFN_##name chk_##name __attribute__((unused)) = h_##name;                       \
    DM_REGISTER("swhle", hle_ent, e_##name, {#name, (void *)h_##name}); \
    static ret h_##name(__VA_ARGS__)
// VKB: the same, run on the thread's big host stack (hle_bigstack.c; register arguments only): for the functions that call into Metal on game threads, whose stacks can be small
#define VKB(name, ret, ...)                                                                      \
    static ret h_##name(__VA_ARGS__) __attribute__((used));                                      \
    static const PFN_##name chk_##name __attribute__((unused)) = h_##name;                       \
    extern const char h_##name##_bs[];                                                           \
    __asm__(".text\n.p2align 2\n.globl _h_" #name "_bs\n.private_extern _h_" #name "_bs\n_h_" #name "_bs:\n" \
            "    adrp x16, _h_" #name "@PAGE\n    add x16, x16, _h_" #name "@PAGEOFF\n    b _dm_bigstack_enter\n"); \
    DM_REGISTER("swhle", hle_ent, e_##name, {#name, (void *)h_##name##_bs}); \
    static ret h_##name(__VA_ARGS__)

static uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t umax(uint32_t a, uint32_t b) { return a > b ? a : b; }

// ---------- Formats ----------
enum { K_COLOR, K_INT, K_DEPTH, K_DS, K_STENCIL, K_BLOCK };
typedef struct { VkFormat vk; MTLPixelFormat pf; MTLVertexFormat vf; uint8_t bytes, bw, bh, kind, conv; } fmt_t;   // bytes per texel, or per block (bw x bh) for compressed formats; conv: see FX
#define FC(v, p, x, b) {VK_FORMAT_##v, MTLPixelFormat##p, MTLVertexFormat##x, b, 1, 1, K_COLOR}
#define FI(v, p, x, b) {VK_FORMAT_##v, MTLPixelFormat##p, MTLVertexFormat##x, b, 1, 1, K_INT}
#define FD(v, p, b, k) {VK_FORMAT_##v, MTLPixelFormat##p, MTLVertexFormatInvalid, b, 1, 1, k}
#define FB(v, p, b, w, h) {VK_FORMAT_##v, MTLPixelFormat##p, MTLVertexFormatInvalid, b, w, h, K_BLOCK}
#define FV(v, x, b) {VK_FORMAT_##v, MTLPixelFormatInvalid, MTLVertexFormat##x, b, 1, 1, K_COLOR}
// Packed formats Metal does not have: the texture has a wider format and the host unpacks the texels into it (conv_unpack). Only for linear images the CPU writes (conv_sync):
// Super Mario 3D All-Stars' Super Mario 64 keeps its N64 textures so, R4G4 among them
enum { CV_NONE, CV_R4G4, CV_B4G4R4A4, CV_B5G6R5, CV_B5G5R5A1 };
#define FX(v, p, b, c) {VK_FORMAT_##v, MTLPixelFormat##p, MTLVertexFormatInvalid, b, 1, 1, K_COLOR, c}
static const fmt_t g_fmts[] = {
    FC(R8_UNORM, R8Unorm, UCharNormalized, 1), FC(R8_SNORM, R8Snorm, CharNormalized, 1), FI(R8_UINT, R8Uint, UChar, 1), FI(R8_SINT, R8Sint, Char, 1),
    FC(R8G8_UNORM, RG8Unorm, UChar2Normalized, 2), FC(R8G8_SNORM, RG8Snorm, Char2Normalized, 2), FI(R8G8_UINT, RG8Uint, UChar2, 2), FI(R8G8_SINT, RG8Sint, Char2, 2),
    FC(R8G8B8A8_UNORM, RGBA8Unorm, UChar4Normalized, 4), FC(R8G8B8A8_SNORM, RGBA8Snorm, Char4Normalized, 4), FI(R8G8B8A8_UINT, RGBA8Uint, UChar4, 4),
    FI(R8G8B8A8_SINT, RGBA8Sint, Char4, 4), FC(R8G8B8A8_SRGB, RGBA8Unorm_sRGB, Invalid, 4),
    FC(B8G8R8A8_UNORM, BGRA8Unorm, UChar4Normalized_BGRA, 4), FC(B8G8R8A8_SRGB, BGRA8Unorm_sRGB, Invalid, 4),
    FC(A8B8G8R8_UNORM_PACK32, RGBA8Unorm, UChar4Normalized, 4), FC(A8B8G8R8_SRGB_PACK32, RGBA8Unorm_sRGB, Invalid, 4),
    FC(A2B10G10R10_UNORM_PACK32, RGB10A2Unorm, UInt1010102Normalized, 4), FI(A2B10G10R10_UINT_PACK32, RGB10A2Uint, Invalid, 4),
    FC(B10G11R11_UFLOAT_PACK32, RG11B10Float, FloatRG11B10, 4), FC(E5B9G9R9_UFLOAT_PACK32, RGB9E5Float, FloatRGB9E5, 4),
    FC(R5G6B5_UNORM_PACK16, B5G6R5Unorm, Invalid, 2), FC(A1R5G5B5_UNORM_PACK16, BGR5A1Unorm, Invalid, 2), FC(R5G5B5A1_UNORM_PACK16, A1BGR5Unorm, Invalid, 2),
    FC(R4G4B4A4_UNORM_PACK16, ABGR4Unorm, Invalid, 2),   // Metal names a packed format's components from the low bits, Vulkan from the high bits
    FX(R4G4_UNORM_PACK8, RG8Unorm, 1, CV_R4G4), FX(B4G4R4A4_UNORM_PACK16, RGBA8Unorm, 2, CV_B4G4R4A4), FX(B5G6R5_UNORM_PACK16, RGBA8Unorm, 2, CV_B5G6R5),
    FX(B5G5R5A1_UNORM_PACK16, RGBA8Unorm, 2, CV_B5G5R5A1),
    FC(R16_UNORM, R16Unorm, UShortNormalized, 2), FC(R16_SNORM, R16Snorm, ShortNormalized, 2), FI(R16_UINT, R16Uint, UShort, 2), FI(R16_SINT, R16Sint, Short, 2),
    FC(R16_SFLOAT, R16Float, Half, 2),
    FC(R16G16_UNORM, RG16Unorm, UShort2Normalized, 4), FC(R16G16_SNORM, RG16Snorm, Short2Normalized, 4), FI(R16G16_UINT, RG16Uint, UShort2, 4),
    FI(R16G16_SINT, RG16Sint, Short2, 4), FC(R16G16_SFLOAT, RG16Float, Half2, 4),
    FC(R16G16B16A16_UNORM, RGBA16Unorm, UShort4Normalized, 8), FC(R16G16B16A16_SNORM, RGBA16Snorm, Short4Normalized, 8),
    FI(R16G16B16A16_UINT, RGBA16Uint, UShort4, 8), FI(R16G16B16A16_SINT, RGBA16Sint, Short4, 8), FC(R16G16B16A16_SFLOAT, RGBA16Float, Half4, 8),
    FI(R32_UINT, R32Uint, UInt, 4), FI(R32_SINT, R32Sint, Int, 4), FC(R32_SFLOAT, R32Float, Float, 4),
    FI(R32G32_UINT, RG32Uint, UInt2, 8), FI(R32G32_SINT, RG32Sint, Int2, 8), FC(R32G32_SFLOAT, RG32Float, Float2, 8),
    FI(R32G32B32_UINT, Invalid, UInt3, 12), FI(R32G32B32_SINT, Invalid, Int3, 12), FC(R32G32B32_SFLOAT, Invalid, Float3, 12),
    FI(R32G32B32A32_UINT, RGBA32Uint, UInt4, 16), FI(R32G32B32A32_SINT, RGBA32Sint, Int4, 16), FC(R32G32B32A32_SFLOAT, RGBA32Float, Float4, 16),
    FD(D16_UNORM, Depth16Unorm, 2, K_DEPTH), FD(X8_D24_UNORM_PACK32, Depth32Float, 4, K_DEPTH), FD(D32_SFLOAT, Depth32Float, 4, K_DEPTH),
    FD(S8_UINT, Stencil8, 1, K_STENCIL), FD(D24_UNORM_S8_UINT, Depth32Float_Stencil8, 4, K_DS), FD(D32_SFLOAT_S8_UINT, Depth32Float_Stencil8, 8, K_DS),
    FB(BC1_RGB_UNORM_BLOCK, BC1_RGBA, 8, 4, 4), FB(BC1_RGB_SRGB_BLOCK, BC1_RGBA_sRGB, 8, 4, 4), FB(BC1_RGBA_UNORM_BLOCK, BC1_RGBA, 8, 4, 4),
    FB(BC1_RGBA_SRGB_BLOCK, BC1_RGBA_sRGB, 8, 4, 4), FB(BC2_UNORM_BLOCK, BC2_RGBA, 16, 4, 4), FB(BC2_SRGB_BLOCK, BC2_RGBA_sRGB, 16, 4, 4),
    FB(BC3_UNORM_BLOCK, BC3_RGBA, 16, 4, 4), FB(BC3_SRGB_BLOCK, BC3_RGBA_sRGB, 16, 4, 4), FB(BC4_UNORM_BLOCK, BC4_RUnorm, 8, 4, 4),
    FB(BC4_SNORM_BLOCK, BC4_RSnorm, 8, 4, 4), FB(BC5_UNORM_BLOCK, BC5_RGUnorm, 16, 4, 4), FB(BC5_SNORM_BLOCK, BC5_RGSnorm, 16, 4, 4),
    FB(BC6H_UFLOAT_BLOCK, BC6H_RGBUfloat, 16, 4, 4), FB(BC6H_SFLOAT_BLOCK, BC6H_RGBFloat, 16, 4, 4), FB(BC7_UNORM_BLOCK, BC7_RGBAUnorm, 16, 4, 4),
    FB(BC7_SRGB_BLOCK, BC7_RGBAUnorm_sRGB, 16, 4, 4),
    FB(ASTC_4x4_UNORM_BLOCK, ASTC_4x4_LDR, 16, 4, 4), FB(ASTC_4x4_SRGB_BLOCK, ASTC_4x4_sRGB, 16, 4, 4), FB(ASTC_5x5_UNORM_BLOCK, ASTC_5x5_LDR, 16, 5, 5),
    FB(ASTC_5x5_SRGB_BLOCK, ASTC_5x5_sRGB, 16, 5, 5), FB(ASTC_6x6_UNORM_BLOCK, ASTC_6x6_LDR, 16, 6, 6), FB(ASTC_6x6_SRGB_BLOCK, ASTC_6x6_sRGB, 16, 6, 6),
    FB(ASTC_8x8_UNORM_BLOCK, ASTC_8x8_LDR, 16, 8, 8), FB(ASTC_8x8_SRGB_BLOCK, ASTC_8x8_sRGB, 16, 8, 8),
    // Vertex-only formats: integers read as unnormalized floats (USCALED / SSCALED, Metal converts the integer formats to the shader's float type) and three components.
    // Super Mario Galaxy's GX vertex data is mostly these (positions as R16G16B16_SSCALED, colours as R8G8B8A8_USCALED…)
    FV(R8_USCALED, UChar, 1), FV(R8_SSCALED, Char, 1), FV(R8G8_USCALED, UChar2, 2), FV(R8G8_SSCALED, Char2, 2),
    FV(R8G8B8_UNORM, UChar3Normalized, 3), FV(R8G8B8_SNORM, Char3Normalized, 3), FV(R8G8B8_USCALED, UChar3, 3), FV(R8G8B8_SSCALED, Char3, 3),
    FV(R8G8B8_UINT, UChar3, 3), FV(R8G8B8_SINT, Char3, 3), FV(R8G8B8A8_USCALED, UChar4, 4), FV(R8G8B8A8_SSCALED, Char4, 4),
    FV(R16_USCALED, UShort, 2), FV(R16_SSCALED, Short, 2), FV(R16G16_USCALED, UShort2, 4), FV(R16G16_SSCALED, Short2, 4),
    FV(R16G16B16_UNORM, UShort3Normalized, 6), FV(R16G16B16_SNORM, Short3Normalized, 6), FV(R16G16B16_USCALED, UShort3, 6), FV(R16G16B16_SSCALED, Short3, 6),
    FV(R16G16B16_UINT, UShort3, 6), FV(R16G16B16_SINT, Short3, 6), FV(R16G16B16_SFLOAT, Half3, 6),
    FV(R16G16B16A16_USCALED, UShort4, 8), FV(R16G16B16A16_SSCALED, Short4, 8),
};
static const fmt_t *g_fmt_by[VK_FORMAT_ASTC_12x12_SRGB_BLOCK + 1];
static void fmt_init(void) { for (size_t i = 0; i < sizeof g_fmts / sizeof *g_fmts; i++) g_fmt_by[g_fmts[i].vk] = &g_fmts[i]; }
static const fmt_t *fmt_of(VkFormat f) {
    static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, fmt_init);
    return (unsigned)f < sizeof g_fmt_by / sizeof *g_fmt_by ? g_fmt_by[f] : NULL;
}
static int fmt_has_depth(const fmt_t *f) { return f && (f->kind == K_DEPTH || f->kind == K_DS); }
static int fmt_has_stencil(const fmt_t *f) { return f && (f->kind == K_STENCIL || f->kind == K_DS); }
static int fmt_is_signed_int(const fmt_t *f) {
    if (!f || f->kind != K_INT) return 0;
    switch (f->vk) {
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_SINT: case VK_FORMAT_R32G32B32A32_SINT: return 1;
    default: return 0;
    }
}
// VkClearColorValue is a union whose member the cleared format names: int32 for a signed integer format, uint32 for an unsigned one, float32 for every other. Metal takes
// doubles and converts them to the attachment's type, so the member must be read as its own type (a uint32 above INT32_MAX saturates a signed attachment)
static MTLClearColor fmt_clear_color(const fmt_t *f, const VkClearColorValue *v) {
    if (fmt_is_signed_int(f)) return MTLClearColorMake(v->int32[0], v->int32[1], v->int32[2], v->int32[3]);
    if (f && f->kind == K_INT) return MTLClearColorMake(v->uint32[0], v->uint32[1], v->uint32[2], v->uint32[3]);
    return MTLClearColorMake(v->float32[0], v->float32[1], v->float32[2], v->float32[3]);
}

// ---------- Device-wide state ----------
static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_q;
static id<MTLSharedEvent> g_ev;                 // signalled with ++g_ev_val at the end of every submit
static id<MTLDepthStencilState> g_dss_off;      // pipelines without depth / stencil
static MTLReadWriteTextureTier g_rw_tex_tier;
static MTLArgumentBuffersTier g_ab_tier;
static NSUInteger g_ab_sampler_limit;
static uint64_t g_ev_val;                       // under g_qmu
static _Atomic uint64_t g_done;                 // highest value whose command buffer has completed
static pthread_mutex_t g_qmu = PTHREAD_MUTEX_INITIALIZER, g_done_mu = PTHREAD_MUTEX_INITIALIZER, g_mem_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_done_cv = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t g_n_draw, g_n_pass, g_n_submit, g_n_copy, g_n_pso, g_n_disp, g_n_skip, g_n_skip_disp, g_n_comp_enc, g_mem_bytes;   // skipped draws / dispatches, and compute encoders created
static _Atomic uint64_t g_n_resume;   // render encoders that resumed a split pass (t_vk_fan)
static _Atomic uint32_t g_fan_pipes;   // triangle-fan pipelines created: until there is one, no command buffer is scanned for fans (fan_prepare)
static pthread_mutex_t g_ab_sampler_mu = PTHREAD_MUTEX_INITIALIZER;
static NSUInteger g_ab_sampler_count;
@interface VkSamplerMetal : NSObject
@property (nonatomic, strong) MTLSamplerDescriptor *descriptor;
@property (nonatomic, strong) id<MTLSamplerState> state;
@property (nonatomic, strong) id<MTLSamplerState> argumentState;
- (id<MTLSamplerState>)argumentSampler;
@end
static id<MTLSamplerState> ab_sampler_fail(void) {
    RED1("argument-buffer sampler creation failed or reached maxArgumentBufferSamplerCount device limit %lu", (unsigned long)g_ab_sampler_limit);
    return nil;
}
@implementation VkSamplerMetal
- (id<MTLSamplerState>)argumentSampler {
    pthread_mutex_lock(&g_ab_sampler_mu);
    if (!_argumentState) {
        if (g_ab_sampler_count >= g_ab_sampler_limit) {
            id<MTLSamplerState> failed = ab_sampler_fail(); pthread_mutex_unlock(&g_ab_sampler_mu); return failed;
        }
        MTLSamplerDescriptor *d = [_descriptor copy]; d.supportArgumentBuffers = YES;
        _argumentState = [g_dev newSamplerStateWithDescriptor:d];
        if (!_argumentState) {
            id<MTLSamplerState> failed = ab_sampler_fail(); pthread_mutex_unlock(&g_ab_sampler_mu); return failed;
        }
        g_ab_sampler_count++;
    }
    id<MTLSamplerState> result = _argumentState; pthread_mutex_unlock(&g_ab_sampler_mu); return result;
}
- (void)dealloc {
    if (_argumentState) { pthread_mutex_lock(&g_ab_sampler_mu); g_ab_sampler_count--; pthread_mutex_unlock(&g_ab_sampler_mu); }
}
@end

// Shader table (<DM_MSL_DIR>/vk/vk_shaders.tsv): resource kind u / b / t / s / c, its (set, binding) in the game's SPIR-V and the Metal slot it was moved to;
// p is the push-constant block; U / B / T / S are members of a set's argument buffer at buffer slot `slot`, with a separate Metal ID (+ element);
// o is a dynamic offset's index in q, and q is the slot of that offset buffer
typedef struct { char kind; uint8_t set, slot; uint16_t count, binding, arg_id; } shres;
typedef struct { char key[17]; int frag, comp, nres, geom_vp; uint16_t wg[3]; shres *res; } shinfo;   // res: owned for the loaded table's lifetime; geom_vp: location + 1
enum { ST_VERT, ST_FRAG, ST_COMP };
static int is_arg(char k) { return k && strchr("UBTS", k); }
static shinfo *g_sh; static int g_nsh;
static id<MTLLibrary> g_lib;
static int sh_cmp(const void *a, const void *b) { return memcmp(((const shinfo *)a)->key, ((const shinfo *)b)->key, 16); }
static int sh_number(const char **p, unsigned *out) {
    if (**p < '0' || **p > '9') return 0;
    char *end; unsigned long value = strtoul(*p, &end, 10);
    if (value > UINT16_MAX) return 0;   // every numeric table field fits in the metadata's uint16_t
    *p = end; *out = (unsigned)value; return 1;
}
static int sh_read_resources(shinfo *s, char *res) {
    size_t count = 0;
    for (const char *p = res ? res : ""; *p;) {
        p += strspn(p, " \r"); if (!*p) break;
        count++; p += strcspn(p, " \r");
    }
    if (count > INT32_MAX) die("[vk] shader resource table is too large");
    s->nres = 0; s->res = NULL;
    if (!count) return 1;
    shres *entries = calloc(count, sizeof *entries);
    if (!entries) die("[vk] out of memory for shader resources");
    char *save = NULL;
    for (char *t = res ? strtok_r(res, " \r", &save) : NULL; t; t = strtok_r(NULL, " \r", &save)) {
        char k = t[0]; const char *p = t + 1; unsigned st, b, sl, n = 1, id;
        if (!strchr("ubtscpUBTSoq", k) || *p++ != ':' || !sh_number(&p, &st) || *p++ != '.' || !sh_number(&p, &b) ||
            *p++ != '=' || !sh_number(&p, &sl)) goto invalid;
        id = b;   // older argument-buffer tables used a binding as the ID
        if (*p == '*') { p++; if (!sh_number(&p, &n)) goto invalid; }
        if (*p == '@') { p++; if (!is_arg(k) || !sh_number(&p, &id)) goto invalid; }
        unsigned limit = k == 'o' ? 32 : k == 's' || k == 'c' ? 16 : 31;
        if (*p || st >= 8 || b >= 128 || !n || n > 4096 || sl >= limit || (size_t)s->nres >= count ||
            (!is_arg(k) && n > limit - sl) || (strchr("poq", k) && n != 1)) goto invalid;
        entries[s->nres++] = (shres){k, (uint8_t)st, (uint8_t)sl, (uint16_t)n, (uint16_t)b, (uint16_t)id};
        continue;
    invalid:
        LOG("[vk] [RED] shader table %s: invalid resource %s; shader refused", s->key, t);
        free(entries); s->nres = 0; return 0;
    }
    s->res = entries; return 1;
}
static void sh_read_table(FILE *f) {
    char *line = NULL; size_t length = 0; int cap = 0;
    while (getline(&line, &length, f) >= 0) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *rest = line, *key = strsep(&rest, "\t"), *stage = strsep(&rest, "\t"), *res = strsep(&rest, "\t\n");   // strsep keeps the empty resource field of a shader without resources
        if (!key || !stage || !res || strlen(key) != 16) { LOG("[vk] [RED] malformed shader table row; shader refused"); continue; }
        shinfo s = {0}; memcpy(s.key, key, 16); s.frag = !strcmp(stage, "frag");
        if (!strncmp(stage, "comp:", 5)) {
            const char *p = stage + 5; unsigned x, y, z;
            if (!sh_number(&p, &x) || *p++ != ',' || !sh_number(&p, &y) || *p++ != ',' || !sh_number(&p, &z) || *p || !x || !y || !z) goto invalid_stage;
            s.comp = 1; s.wg[0] = (uint16_t)x; s.wg[1] = (uint16_t)y; s.wg[2] = (uint16_t)z;
        } else if (!strncmp(stage, "geomvp:", 7)) {
            const char *p = stage + 7; unsigned loc;
            if (!sh_number(&p, &loc) || *p || loc >= 32) goto invalid_stage;
            s.geom_vp = (int)loc + 1;
        } else if (strcmp(stage, "vert") && !s.frag) goto invalid_stage;
        if (!sh_read_resources(&s, res)) continue;
        if (g_nsh == cap) { cap = cap ? cap * 2 : 128; shinfo *q = realloc(g_sh, cap * sizeof *g_sh); if (!q) die("[vk] out of memory for the shader table"); g_sh = q; }
        g_sh[g_nsh++] = s;
        continue;
    invalid_stage:
        LOG("[vk] [RED] shader table %s: invalid stage %s; shader refused", key, stage);
    }
    free(line);
    if (ferror(f)) die("[vk] cannot read the shader table");
    qsort(g_sh, g_nsh, sizeof *g_sh, sh_cmp);   // looked up by key (Super Mario Galaxy creates over 3,000 modules at boot)
}
static void sh_load(void) {
    const char *d = getenv("DM_MSL_DIR"); char p[1024];
    if (!d) { LOG("[vk] [RED] DM_MSL_DIR not set: the Vulkan shader library cannot be found"); return; }
    snprintf(p, sizeof p, "%s/vk/vk_shaders.tsv", d);
    FILE *f = fopen(p, "r");
    if (!f) { LOG("[vk] [RED] cannot open %s (run dm/gfx/tools/vk_shaders.py first)", p); return; }
    sh_read_table(f); fclose(f);
    snprintf(p, sizeof p, "%s/vk/vk_shaders.metallib", d);
    NSError *err = nil;
    g_lib = [g_dev newLibraryWithURL:[NSURL fileURLWithPath:@(p)] error:&err];
    if (!g_lib) LOG("[vk] [RED] shader library failed to load %s: %s", p, err.localizedDescription.UTF8String);
}
static const shinfo *sh_find(const char *key) { shinfo k; memcpy(k.key, key, 16); return g_nsh ? bsearch(&k, g_sh, g_nsh, sizeof *g_sh, sh_cmp) : NULL; }

static void dev_init_once(void) {
    @autoreleasepool {
        g_dev = (__bridge id<MTLDevice>)mtl_device_classic();
        g_rw_tex_tier = g_dev.readWriteTextureSupport;
        g_ab_tier = g_dev.argumentBuffersSupport;
        g_ab_sampler_limit = g_dev.maxArgumentBufferSamplerCount;
        g_q = [g_dev newCommandQueueWithMaxCommandBufferCount:64]; g_q.label = @"vk";
        g_ev = [g_dev newSharedEvent];
        g_dss_off = [g_dev newDepthStencilStateWithDescriptor:[MTLDepthStencilDescriptor new]];
        sh_load();
        VLOG("Metal device %s; %d shader table entries", g_dev.name.UTF8String, g_nsh);
    }
}
static void dev_init(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, dev_init_once); }
enum { MAX_VIEWPORTS = 16 };
static uint32_t max_viewports(void) {
    dev_init();
    return [g_dev supportsFamily:MTLGPUFamilyApple5] ? MAX_VIEWPORTS : 1;   // the host runs on Apple Silicon
}

// ---------- Objects ----------
// Dispatchable handles: their first word is where a loader keeps its dispatch table
struct VkInstance_T { void *loader; };
struct VkPhysicalDevice_T { void *loader; };
struct VkDevice_T { void *loader; };
struct VkQueue_T { void *loader; };
static struct VkInstance_T g_inst; static struct VkPhysicalDevice_T g_pdev; static struct VkDevice_T g_device; static struct VkQueue_T g_queue;

struct VkSurfaceKHR_T { void *win; };
struct VkDeviceMemory_T { VkDeviceSize size; uint32_t type; __strong id<MTLBuffer> buf; _Atomic(void *) raw; };
struct VkBuffer_T { VkDeviceSize size; VkBufferUsageFlags usage; VkDeviceMemory mem; VkDeviceSize off; };
struct VkBufferView_T { VkBuffer buf; const fmt_t *f; VkDeviceSize off, range; __strong id<MTLTexture> tex; };
struct VkImage_T { __strong id<MTLTexture> tex; const fmt_t *f; VkFormat fmt; VkImageType type; uint32_t w, h, d, mips, layers, samples; VkDeviceSize size; uint64_t serial;
                  VkImageUsageFlags usage; uint32_t pitch;   // pitch: row pitch of a linear image (its texture is made over the bound memory), 0 otherwise
                  VkDeviceMemory cmem; VkDeviceSize coff; uint8_t *shadow;   // a linear image of a converted format (fmt_t.conv): its memory, and the texels last unpacked
                  int swap; };   // one of a swap chain's images: the swap chain owns it
struct VkImageView_T { __strong id<MTLTexture> tex; VkImage img; };
struct VkSampler_T { __strong id<MTLSamplerState> s; __strong VkSamplerMetal *ref; };
struct VkShaderModule_T { const shinfo *si; __strong id<MTLFunction> fn, fn_d; char key[17]; int tried_d; };   // fn_d: the dual-source variant vk_<key>_d, looked up on first use
struct VkPipelineCache_T { int unused; };
// A result is valid once the Metal command buffer that wrote it has completed (g_done >= ticket).
// Timestamps use that command buffer's completion time in ns (timestampPeriod 1).
struct VkQueryPool_T { VkQueryType type; uint32_t n; _Atomic uint64_t *val, *ticket; };
struct VkSemaphore_T { int unused; };
#define FENCE_NONE UINT64_MAX
struct VkFence_T { _Atomic uint64_t val; };   // 0 = signalled; FENCE_NONE = unsignalled with nothing pending; otherwise signalled once g_done reaches it
struct VkEvent_T { _Atomic uint64_t val; };   // the same encoding: set by the host (0), reset (FENCE_NONE), or set once the submit that sets it (its value) has completed

#define MAXB 64
typedef struct { uint32_t binding, count, base; VkDescriptorType type; int dyn, imm; } lbind;   // base: first element in a set's array; dyn: first dynamic offset in the set (-1 if not dynamic)
// refs: the handle and each set allocated from it (dsl_put frees on the last): Vulkan lets sets, and pipelines made with it, outlive vkDestroyDescriptorSetLayout
struct VkDescriptorSetLayout_T { _Atomic int refs; uint32_t n, total, ndyn; lbind b[MAXB]; int8_t idx[128]; __strong VkSamplerMetal **imm; };
// A descriptor holds the Metal objects it was written with (retained), as a hardware descriptor holds a copy of the object's handle: a set may still name a view, sampler
// or buffer the game has destroyed (the hardware reads it only when a draw samples it), and the replay binds every element of an array the shader takes. Descriptors
// used to point at this layer's view / sampler / buffer structs, and Super Mario Galaxy 2 crashed reading a freed view's texture from element 1 of its 8-texture array
// when the ship took off after its first galaxy (GitHub issue 68). buf / off: the buffer's memory and its offset there plus the descriptor's; tex: an image view's
// texture or a texel buffer's
typedef struct { __strong id<MTLBuffer> buf; VkDeviceSize off; __strong id<MTLTexture> tex; __strong VkSamplerMetal *smp; } desc_t;
struct VkDescriptorPool_T { struct VkDescriptorSet_T *sets; };
struct VkDescriptorSet_T { VkDescriptorSetLayout L; VkDescriptorPool pool; struct VkDescriptorSet_T *prev, *next; desc_t *d; uint64_t gen; };   // d: L->total descriptors (ARC objects: no flexible array)
static _Atomic uint64_t g_set_gen;   // a new value at allocation and after every write/copy: cached argument buffers become stale
struct VkDescriptorUpdateTemplate_T { uint32_t n; VkDescriptorUpdateTemplateEntry e[]; };
struct VkPipelineLayout_T { uint32_t nset, pc_size; VkDescriptorSetLayout set[8]; };   // pc_size: end of the highest push-constant range; pipelines copy what they use (pipe_make)

typedef struct { VkFormat fmt; VkAttachmentLoadOp load, sload; VkAttachmentStoreOp store, sstore; } ratt;
struct VkRenderPass_T { uint32_t natt, ncol; ratt att[16]; int col[8], res[8], depth; };
struct VkFramebuffer_T { uint32_t n, w, h, layers; VkImageView v[16]; };
// Each stage's argument buffer is encoded from one set; keep its resources alive and resident once per encoder.
typedef struct { uint8_t slot, set; __strong id<MTLArgumentEncoder> enc; VkDescriptorSet s; uint64_t gen, enc_id; __strong id<MTLBuffer> buf; __strong NSArray *keep;
                 __unsafe_unretained id<MTLResource> *ro, *rw; uint32_t nro, nrw; } abuf_t;
struct VkPipeline_T {
    __strong id<MTLRenderPipelineState> pso; __strong id<MTLDepthStencilState> dss;
    const shinfo *vs, *fs; uint32_t vb_mask; uint8_t vb_slot[14];
    __strong id<MTLComputePipelineState> cpso; const shinfo *cs; MTLSize wg;
    uint32_t pc_size, nset; VkDescriptorSetLayout set[8];   // from its pipeline layout, which may be destroyed first: the push-constant size, and the set layouts (held) for a missing binding's report
    MTLPrimitiveType prim; MTLCullMode cull; MTLWinding wind; MTLTriangleFillMode fill; MTLDepthClipMode clip;
    int fan, restart;   // a triangle fan, drawn as a list (fan_arrays, fan_indexed), and its primitive restart
    int bias, dyn_bias, dyn_blend; float bias_c, bias_s, bias_cl; float blend[4]; uint32_t sref_f, sref_b;
    int db_en, dyn_db, dbk; float db[2];   // depth bounds test (nvn_dbnd.h): pipeline enable, dynamic flag, and static resolved state (DBND_*, bounds)
    char keys[64]; int told;   // its shaders' keys, for the report of draws skipped (told: reported)
    uint32_t nviewport;
    MTLViewport vp0, *vp; VkRect2D sc0, *sc;   // static state, with heap storage only for multiple viewports
    abuf_t ab[3][2]; int nab[3];   // at most two argument buffers per stage (ST_*)
    // Dynamic stencil state (Super Mario 3D All-Stars' Super Mario 64): the reference is the encoder's; the masks are part of a Metal depth-stencil state, so the
    // pipeline keeps its depth-stencil description and builds the state for the masks set (one cached)
    VkLogicOp logic;   // not implemented (0 = none: VK_LOGIC_OP_CLEAR is not met)
    int dyn_sref, dyn_smask, stencil; VkStencilOpState sf, sb; MTLCompareFunction dcmp; int dwrite; uint64_t dss_key; __strong id<MTLDepthStencilState> dss_dyn;
};
static int ab_init(VkPipeline p, int st, id<MTLFunction> fn, const shinfo *si) {
    if (!fn || !si) return 1;
    for (int i = 0; i < si->nres; i++) {
        const shres *r = &si->res[i]; int k;
        if (!is_arg(r->kind)) continue;
        for (k = 0; k < p->nab[st] && p->ab[st][k].slot != r->slot; k++) {}
        if (k < p->nab[st]) continue;
        if (k == 2) { RED1("shader %s: more than 2 argument buffers", si->key); return 0; }
        abuf_t *a = &p->ab[st][k]; a->slot = r->slot; a->set = r->set; a->enc = [fn newArgumentEncoderWithBufferIndex:r->slot];
        if (!a->enc) { RED1("shader %s: no argument encoder for buffer %u", si->key, r->slot); return 0; }
        p->nab[st]++;
    }
    return 1;
}
static void ab_free(VkPipeline p) {
    for (int st = 0; st < 3; st++)
        for (int k = 0; k < 2; k++) { abuf_t *a = &p->ab[st][k]; a->enc = nil; a->buf = nil; a->keep = nil; free(a->ro); free(a->rw); a->ro = a->rw = NULL; }
}
static int argbuf_ok(const shinfo *si) {
    if (!si) return 1;
    for (int i = 0; i < si->nres; i++) if (is_arg(si->res[i].kind)) {
        if (g_ab_tier != MTLArgumentBuffersTier2) {
            RED1("shader %s: argument buffers require Metal Tier2 (device %s)", si->key,
                 g_ab_tier == MTLArgumentBuffersTier2 ? "Tier2" : "Tier1");
            return 0;
        }
        return 1;
    }
    return 1;
}
struct VkCommandPool_T { struct VkCommandBuffer_T *cbs; };
struct VkCommandBuffer_T { void *loader; VkCommandPool pool; struct VkCommandBuffer_T *prev, *next; uint8_t *buf; size_t n, cap; };
struct VkSwapchainKHR_T { uint32_t n, next; VkImage img[4]; uint64_t ticket[4]; };

static void *zalloc(size_t n) { void *p = calloc(1, n); if (!p) die("[vk] out of memory %zu", n); return p; }

// ---------- Instance and physical device ----------
static VkResult fill_names(const char *const *names, uint32_t n, uint32_t *count, VkExtensionProperties *p) {
    if (!p) { *count = n; return VK_SUCCESS; }
    uint32_t k = umin(*count, n);
    for (uint32_t i = 0; i < k; i++) { memset(&p[i], 0, sizeof p[i]); strlcpy(p[i].extensionName, names[i], sizeof p[i].extensionName); p[i].specVersion = 1; }
    *count = k; return k < n ? VK_INCOMPLETE : VK_SUCCESS;
}
static const char *const g_inst_ext[] = {"VK_KHR_surface", "VK_NN_vi_surface", "VK_KHR_get_physical_device_properties2", "VK_KHR_get_surface_capabilities2"};
static const char *const g_dev_ext[] = {"VK_KHR_swapchain", "VK_KHR_descriptor_update_template", "VK_KHR_maintenance1", "VK_KHR_get_memory_requirements2",
                                        "VK_KHR_dedicated_allocation", "VK_KHR_bind_memory2"};
static void join_names(char *out, size_t n, const char *const *names, uint32_t count) {
    out[0] = 0; size_t o = 0;
    for (uint32_t i = 0; i < count && o + 2 < n; i++) o += snprintf(out + o, n - o, "%s%s", i ? " " : "", names[i]);
    if (!count) strlcpy(out, "none", n);
}

VK(vkEnumerateInstanceVersion, VkResult, uint32_t *v) { *v = VK_API_VERSION_1_1; return VK_SUCCESS; }
VK(vkEnumerateInstanceLayerProperties, VkResult, uint32_t *n, VkLayerProperties *p) { (void)p; *n = 0; return VK_SUCCESS; }
VK(vkEnumerateInstanceExtensionProperties, VkResult, const char *layer, uint32_t *n, VkExtensionProperties *p) {
    if (layer) { *n = 0; return VK_ERROR_LAYER_NOT_PRESENT; }
    return fill_names(g_inst_ext, sizeof g_inst_ext / sizeof *g_inst_ext, n, p);
}
VK(vkCreateInstance, VkResult, const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out) {
    (void)a; g_log = getenv("DM_VK_LOG") != NULL;
    const VkApplicationInfo *ai = ci->pApplicationInfo; char ext[512];
    join_names(ext, sizeof ext, ci->ppEnabledExtensionNames, ci->enabledExtensionCount);
    VLOG("CreateInstance: application %s, engine %s, API %u.%u; extensions %s; %u layers", ai && ai->pApplicationName ? ai->pApplicationName : "-",
         ai && ai->pEngineName ? ai->pEngineName : "-", ai ? VK_API_VERSION_MAJOR(ai->apiVersion) : 0, ai ? VK_API_VERSION_MINOR(ai->apiVersion) : 0, ext, ci->enabledLayerCount);
    *out = &g_inst; return VK_SUCCESS;
}
VK(vkDestroyInstance, void, VkInstance i, const VkAllocationCallbacks *a) { (void)i; (void)a; }
// VK_EXT_debug_report: a game that enabled it calls its functions without checking the lookup (Super Mario 3D All-Stars' Super Mario 64 called the null pointer).
// The layer reports nothing, so the callback object is a token that is never called
VK(vkCreateDebugReportCallbackEXT, VkResult, VkInstance i, const VkDebugReportCallbackCreateInfoEXT *ci, const VkAllocationCallbacks *a, VkDebugReportCallbackEXT *out) {
    (void)i; (void)ci; (void)a; static uint64_t next; *out = (VkDebugReportCallbackEXT)(uintptr_t)(0x1000 + ++next); return VK_SUCCESS;
}
VK(vkDestroyDebugReportCallbackEXT, void, VkInstance i, VkDebugReportCallbackEXT cb, const VkAllocationCallbacks *a) { (void)i; (void)cb; (void)a; }
VK(vkDebugReportMessageEXT, void, VkInstance i, VkDebugReportFlagsEXT f, VkDebugReportObjectTypeEXT t, uint64_t o, size_t loc, int32_t code, const char *layer, const char *msg) {
    (void)i; (void)f; (void)t; (void)o; (void)loc; (void)code; VLOG("debug report from the game: %s: %s", layer ? layer : "-", msg ? msg : "-");
}
VK(vkEnumeratePhysicalDevices, VkResult, VkInstance i, uint32_t *n, VkPhysicalDevice *p) {
    (void)i; if (!p) { *n = 1; return VK_SUCCESS; }
    if (!*n) return VK_INCOMPLETE;
    *n = 1; p[0] = &g_pdev; return VK_SUCCESS;
}
// Properties and limits: a Tegra X1 under NVIDIA's Switch driver, except where Metal is stricter
static void pdev_props(VkPhysicalDeviceProperties *p) {
    memset(p, 0, sizeof *p);
    p->apiVersion = VK_API_VERSION_1_1; p->driverVersion = VK_MAKE_API_VERSION(0, 1, 0, 0); p->vendorID = 0x10de; p->deviceID = 0x92ba;
    p->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU; strlcpy(p->deviceName, "NVIDIA Tegra X1 (nvgpu)", sizeof p->deviceName);
    VkPhysicalDeviceLimits *l = &p->limits;
    l->maxImageDimension1D = l->maxImageDimension2D = l->maxImageDimensionCube = 16384; l->maxImageDimension3D = 2048; l->maxImageArrayLayers = 2048;
    l->maxTexelBufferElements = 1u << 27; l->maxUniformBufferRange = 65536; l->maxStorageBufferRange = 1u << 30; l->maxPushConstantsSize = 256;
    l->maxMemoryAllocationCount = 4096; l->maxSamplerAllocationCount = 4000; l->bufferImageGranularity = 1024; l->maxBoundDescriptorSets = 8;
    l->maxPerStageDescriptorSamplers = 16; l->maxPerStageDescriptorUniformBuffers = 15; l->maxPerStageDescriptorStorageBuffers = 16;
    l->maxPerStageDescriptorSampledImages = 31; l->maxPerStageDescriptorStorageImages = 8; l->maxPerStageDescriptorInputAttachments = 8; l->maxPerStageResources = 128;
    l->maxDescriptorSetSamplers = 4000; l->maxDescriptorSetUniformBuffers = 90; l->maxDescriptorSetUniformBuffersDynamic = 15;
    l->maxDescriptorSetStorageBuffers = 96; l->maxDescriptorSetStorageBuffersDynamic = 16; l->maxDescriptorSetSampledImages = 4096;
    l->maxDescriptorSetStorageImages = 4096; l->maxDescriptorSetInputAttachments = 8;
    l->maxVertexInputAttributes = 31; l->maxVertexInputBindings = 14; l->maxVertexInputAttributeOffset = 2047; l->maxVertexInputBindingStride = 2048; l->maxVertexOutputComponents = 128;
    l->maxFragmentInputComponents = 128; l->maxFragmentOutputAttachments = 8; l->maxFragmentDualSrcAttachments = 1; l->maxFragmentCombinedOutputResources = 16;
    l->maxComputeSharedMemorySize = 32768; l->maxComputeWorkGroupCount[0] = l->maxComputeWorkGroupCount[1] = l->maxComputeWorkGroupCount[2] = 65535;
    l->maxComputeWorkGroupInvocations = 1024; l->maxComputeWorkGroupSize[0] = l->maxComputeWorkGroupSize[1] = 1024; l->maxComputeWorkGroupSize[2] = 64;
    l->subPixelPrecisionBits = 8; l->subTexelPrecisionBits = 8; l->mipmapPrecisionBits = 8; l->maxDrawIndexedIndexValue = 0xffffffffu; l->maxDrawIndirectCount = 0xffffffffu;
    l->maxSamplerLodBias = 15; l->maxSamplerAnisotropy = 16; l->maxViewports = max_viewports(); l->maxViewportDimensions[0] = l->maxViewportDimensions[1] = 16384;
    l->viewportBoundsRange[0] = -32768; l->viewportBoundsRange[1] = 32767; l->viewportSubPixelBits = 8;
    l->minMemoryMapAlignment = 64; l->minTexelBufferOffsetAlignment = 256; l->minUniformBufferOffsetAlignment = 256; l->minStorageBufferOffsetAlignment = 256;
    l->minTexelOffset = -8; l->maxTexelOffset = 7; l->minTexelGatherOffset = -32; l->maxTexelGatherOffset = 31;
    l->minInterpolationOffset = -0.5f; l->maxInterpolationOffset = 0.4375f; l->subPixelInterpolationOffsetBits = 4;
    l->maxFramebufferWidth = l->maxFramebufferHeight = 16384; l->maxFramebufferLayers = 2048;
    l->framebufferColorSampleCounts = l->framebufferDepthSampleCounts = l->framebufferStencilSampleCounts = l->framebufferNoAttachmentsSampleCounts = 0x7;
    l->maxColorAttachments = 8; l->sampledImageColorSampleCounts = l->sampledImageIntegerSampleCounts = l->sampledImageDepthSampleCounts = l->sampledImageStencilSampleCounts = 0x7;
    l->storageImageSampleCounts = 1; l->maxSampleMaskWords = 1; l->timestampComputeAndGraphics = 1; l->timestampPeriod = 1;
    l->maxClipDistances = 8; l->maxCullDistances = 0; l->maxCombinedClipAndCullDistances = 8; l->discreteQueuePriorities = 2;
    l->pointSizeRange[0] = 1; l->pointSizeRange[1] = 511; l->lineWidthRange[0] = l->lineWidthRange[1] = 1; l->pointSizeGranularity = 0.0625f; l->lineWidthGranularity = 0;
    l->strictLines = 1; l->standardSampleLocations = 1; l->optimalBufferCopyOffsetAlignment = 256; l->optimalBufferCopyRowPitchAlignment = 256; l->nonCoherentAtomSize = 64;
}
static void pdev_feats(VkPhysicalDeviceFeatures *f) {
    memset(f, 0, sizeof *f);
    f->robustBufferAccess = f->fullDrawIndexUint32 = f->imageCubeArray = f->independentBlend = f->sampleRateShading = f->dualSrcBlend = 1;
    f->drawIndirectFirstInstance = f->depthClamp = f->depthBiasClamp = f->fillModeNonSolid = f->largePoints = f->samplerAnisotropy = 1;
    f->textureCompressionBC = f->textureCompressionASTC_LDR = f->occlusionQueryPrecise = f->fragmentStoresAndAtomics = 1;
    f->shaderImageGatherExtended = f->shaderClipDistance = f->shaderResourceMinLod = 1;
    f->multiViewport = max_viewports() > 1;
}
static void pdev_mem(VkPhysicalDeviceMemoryProperties *m) {
    memset(m, 0, sizeof *m);
    const VkMemoryPropertyFlags dl = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    m->memoryTypeCount = 3;
    m->memoryTypes[0] = (VkMemoryType){dl, 0}; m->memoryTypes[1] = (VkMemoryType){dl | hv, 0}; m->memoryTypes[2] = (VkMemoryType){dl | hv | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0};
    m->memoryHeapCount = 1; m->memoryHeaps[0] = (VkMemoryHeap){3ull << 30, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
}
static void dedicated_req(void *pn) {
    for (VkBaseOutStructure *s = pn; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) { VkMemoryDedicatedRequirements *d = (void *)s; d->prefersDedicatedAllocation = d->requiresDedicatedAllocation = VK_FALSE; }
}
VK(vkGetPhysicalDeviceProperties, void, VkPhysicalDevice pd, VkPhysicalDeviceProperties *p) { (void)pd; pdev_props(p); }
VK(vkGetPhysicalDeviceProperties2, void, VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p) { (void)pd; pdev_props(&p->properties); }
VK(vkGetPhysicalDeviceFeatures, void, VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f) { (void)pd; pdev_feats(f); }
VK(vkGetPhysicalDeviceFeatures2, void, VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f) { (void)pd; pdev_feats(&f->features); }
VK(vkGetPhysicalDeviceMemoryProperties, void, VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *m) { (void)pd; pdev_mem(m); }
VK(vkGetPhysicalDeviceMemoryProperties2, void, VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *m) { (void)pd; pdev_mem(&m->memoryProperties); }
VK(vkGetPhysicalDeviceQueueFamilyProperties, void, VkPhysicalDevice pd, uint32_t *n, VkQueueFamilyProperties *p) {
    (void)pd; if (!p) { *n = 1; return; }
    if (!*n) return;
    *n = 1; p[0] = (VkQueueFamilyProperties){VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, 16, 64, {1, 1, 1}};   // every VkQueue is the same serial Metal queue
}
// Apple Metal's read-write texture format tiers: tier 1 supports R32 float/uint/sint, and tier 2 adds the formats below.
// https://developer.apple.com/documentation/metal/mtlreadwritetexturetier/tier1
// https://developer.apple.com/documentation/metal/mtlreadwritetexturetier/tier2
static int storage_image_supported(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT:
            return g_rw_tex_tier == MTLReadWriteTextureTier1 || g_rw_tex_tier == MTLReadWriteTextureTier2;
        default: break;
    }
    if (g_rw_tex_tier != MTLReadWriteTextureTier2) return 0;
    switch (f) {
        case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT:
            return 1;
        default: return 0;
    }
}
VK(vkGetPhysicalDeviceFormatProperties, void, VkPhysicalDevice pd, VkFormat format, VkFormatProperties *p) {
    (void)pd; dev_init(); memset(p, 0, sizeof *p);
    const fmt_t *f = fmt_of(format); if (!f) return;
    if (f->vf != MTLVertexFormatInvalid) p->bufferFeatures |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
    if (f->pf == MTLPixelFormatInvalid) return;
    VkFormatFeatureFlags t = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;
    if (f->kind != K_INT && f->kind != K_STENCIL) t |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if (storage_image_supported(format)) t |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if (f->kind == K_COLOR || f->kind == K_INT) {
        t |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        if (f->kind == K_COLOR) t |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
        p->bufferFeatures |= VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
    }
    if (f->kind == K_DEPTH || f->kind == K_DS || f->kind == K_STENCIL) t |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    p->optimalTilingFeatures = t;
    p->linearTilingFeatures = f->kind == K_COLOR || f->kind == K_INT ? t : 0;
}
VK(vkGetPhysicalDeviceImageFormatProperties, VkResult, VkPhysicalDevice pd, VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage,
   VkImageCreateFlags flags, VkImageFormatProperties *p) {
    (void)pd; const fmt_t *f = fmt_of(format);
    if (!f || f->pf == MTLPixelFormatInvalid) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    memset(p, 0, sizeof *p);
    p->maxExtent = type == VK_IMAGE_TYPE_3D ? (VkExtent3D){2048, 2048, 2048} : (VkExtent3D){16384, type == VK_IMAGE_TYPE_1D ? 1 : 16384, 1};
    p->maxMipLevels = 15; p->maxArrayLayers = type == VK_IMAGE_TYPE_3D ? 1 : 2048;
    p->sampleCounts = type == VK_IMAGE_TYPE_2D && tiling == VK_IMAGE_TILING_OPTIMAL && !(flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) &&
                      (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) ? 0x7 : 1;
    p->maxResourceSize = 1ull << 32;
    return VK_SUCCESS;
}
VK(vkGetPhysicalDeviceImageFormatProperties2, VkResult, VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *i, VkImageFormatProperties2 *p) {
    memset(&p->imageFormatProperties, 0, sizeof p->imageFormatProperties);
    for (const VkBaseInStructure *x = i->pNext; x; x = x->pNext)
        if (x->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO && ((const VkPhysicalDeviceExternalImageFormatInfo *)x)->handleType)
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
    for (VkBaseOutStructure *x = p->pNext; x; x = x->pNext)
        if (x->sType == VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES)
            ((VkExternalImageFormatProperties *)x)->externalMemoryProperties = (VkExternalMemoryProperties){0};
    return h_vkGetPhysicalDeviceImageFormatProperties(pd, i->format, i->type, i->tiling, i->usage, i->flags, &p->imageFormatProperties);
}
VK(vkGetImageSparseMemoryRequirements, void, VkDevice d, VkImage i, uint32_t *n, VkSparseImageMemoryRequirements *r) { (void)d; (void)i; (void)r; *n = 0; }

// ---------- Surface ----------
static VkExtent2D surf_ext(void) { return dm_opmode() ? (VkExtent2D){1920, 1080} : (VkExtent2D){1280, 720}; }
VK(vkCreateViSurfaceNN, VkResult, VkInstance i, const VkViSurfaceCreateInfoNN *ci, const VkAllocationCallbacks *a, VkSurfaceKHR *out) {
    (void)i; (void)a; VkSurfaceKHR s = zalloc(sizeof *s); s->win = ci->window; *out = s;
    VLOG("CreateViSurfaceNN window %p", ci->window); return VK_SUCCESS;
}
VK(vkDestroySurfaceKHR, void, VkInstance i, VkSurfaceKHR s, const VkAllocationCallbacks *a) { (void)i; (void)a; free(s); }
VK(vkGetPhysicalDeviceSurfaceSupportKHR, VkResult, VkPhysicalDevice pd, uint32_t fam, VkSurfaceKHR s, VkBool32 *ok) { (void)pd; (void)s; *ok = fam == 0; return VK_SUCCESS; }
VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, VkResult, VkPhysicalDevice pd, VkSurfaceKHR s, VkSurfaceCapabilitiesKHR *c) {
    (void)pd; (void)s; memset(c, 0, sizeof *c);
    c->minImageCount = 2; c->maxImageCount = 4; c->currentExtent = surf_ext(); c->minImageExtent = (VkExtent2D){1, 1}; c->maxImageExtent = (VkExtent2D){1920, 1080};
    // INHERIT as well: Super Mario Galaxy creates its swapchain with preTransform INHERIT and asserts when the surface does not list it (transforms are ignored here)
    c->maxImageArrayLayers = 1; c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR; c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    c->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR | VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR;
    c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    return VK_SUCCESS;
}
VK(vkGetPhysicalDeviceSurfaceCapabilities2KHR, VkResult, VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR *s, VkSurfaceCapabilities2KHR *c) {
    return h_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, s->surface, &c->surfaceCapabilities);
}
static const VkSurfaceFormatKHR surface_formats[] = {
    {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}, {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}, {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
};
VK(vkGetPhysicalDeviceSurfaceFormatsKHR, VkResult, VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t *n, VkSurfaceFormatKHR *f) {
    (void)pd; (void)s;
    const uint32_t k = sizeof surface_formats / sizeof *surface_formats;
    if (!f) { *n = k; return VK_SUCCESS; }
    uint32_t m = umin(*n, k); memcpy(f, surface_formats, m * sizeof *f); *n = m; return m < k ? VK_INCOMPLETE : VK_SUCCESS;
}
VK(vkGetPhysicalDeviceSurfaceFormats2KHR, VkResult, VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR *s, uint32_t *n, VkSurfaceFormat2KHR *f) {
    VkSurfaceFormatKHR formats[sizeof surface_formats / sizeof *surface_formats];
    VkResult result = h_vkGetPhysicalDeviceSurfaceFormatsKHR(pd, s->surface, n, f ? formats : NULL);
    if (f) for (uint32_t i = 0; i < *n; i++) f[i].surfaceFormat = formats[i];
    return result;
}
VK(vkGetPhysicalDeviceSurfacePresentModesKHR, VkResult, VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t *n, VkPresentModeKHR *m) {
    (void)pd; (void)s; if (!m) { *n = 1; return VK_SUCCESS; }
    if (!*n) return VK_INCOMPLETE;
    *n = 1; m[0] = VK_PRESENT_MODE_FIFO_KHR; return VK_SUCCESS;
}

// ---------- Device and queue ----------
VK(vkEnumerateDeviceLayerProperties, VkResult, VkPhysicalDevice pd, uint32_t *n, VkLayerProperties *p) { (void)pd; (void)p; *n = 0; return VK_SUCCESS; }
VK(vkEnumerateDeviceExtensionProperties, VkResult, VkPhysicalDevice pd, const char *layer, uint32_t *n, VkExtensionProperties *p) {
    (void)pd; if (layer) { *n = 0; return VK_ERROR_LAYER_NOT_PRESENT; }
    return fill_names(g_dev_ext, sizeof g_dev_ext / sizeof *g_dev_ext, n, p);
}
VKB(vkCreateDevice, VkResult, VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out) {
    (void)pd; (void)a; dev_init();
    char ext[512]; join_names(ext, sizeof ext, ci->ppEnabledExtensionNames, ci->enabledExtensionCount);
    uint32_t nq = 0; for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++) nq += ci->pQueueCreateInfos[i].queueCount;
    VLOG("CreateDevice: %u queues; extensions %s", nq, ext);
    *out = &g_device; return VK_SUCCESS;
}
VK(vkDestroyDevice, void, VkDevice d, const VkAllocationCallbacks *a) { (void)d; (void)a; }
VK(vkGetDeviceQueue, void, VkDevice d, uint32_t fam, uint32_t idx, VkQueue *q) { (void)d; (void)fam; (void)idx; *q = &g_queue; }

VK(vkGetDeviceQueue2, void, VkDevice d, const VkDeviceQueueInfo2 *i, VkQueue *q) {
    if (i->flags) { *q = VK_NULL_HANDLE; return; }   // no protected queues
    h_vkGetDeviceQueue(d, i->queueFamilyIndex, i->queueIndex, q);
}

// ---------- Memory and buffers ----------
static id<MTLBuffer> mem_buf(VkDeviceMemory m) {
    void *r = atomic_load_explicit(&m->raw, memory_order_acquire);
    if (r) return (__bridge id<MTLBuffer>)r;
    pthread_mutex_lock(&g_mem_mu);
    if (!m->buf) {
        m->buf = [g_dev newBufferWithLength:m->size ? m->size : 16 options:MTLResourceStorageModeShared];
        if (!m->buf) LOG("[vk] [RED] cannot create an MTLBuffer of %llu bytes", (unsigned long long)m->size);
        else { atomic_store_explicit(&m->raw, (__bridge void *)m->buf, memory_order_release); g_mem_bytes += m->size; }
    }
    pthread_mutex_unlock(&g_mem_mu);
    return m->buf;
}
VK(vkAllocateMemory, VkResult, VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *out) {
    (void)d; (void)a;
    if (ai->memoryTypeIndex >= 3) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkDeviceMemory m = zalloc(sizeof *m); m->size = ai->allocationSize; m->type = ai->memoryTypeIndex;
    OLOG("AllocateMemory %p %llu bytes type %u", (void *)m, (unsigned long long)m->size, m->type);
    *out = m; return VK_SUCCESS;
}
VKB(vkFreeMemory, void, VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) {
    (void)d; (void)a; if (!m) return;
    if (m->buf) g_mem_bytes -= m->size;
    m->buf = nil; free(m);
}
VKB(vkMapMemory, VkResult, VkDevice d, VkDeviceMemory m, VkDeviceSize off, VkDeviceSize size, VkMemoryMapFlags fl, void **pp) {
    (void)d; (void)size; (void)fl;
    if (m->type == 0) RED1("mapped invisible memory (type 0)");
    id<MTLBuffer> b = mem_buf(m); if (!b) return VK_ERROR_MEMORY_MAP_FAILED;
    *pp = (uint8_t *)b.contents + off; return VK_SUCCESS;
}
VK(vkUnmapMemory, void, VkDevice d, VkDeviceMemory m) { (void)d; (void)m; }
VK(vkFlushMappedMemoryRanges, VkResult, VkDevice d, uint32_t n, const VkMappedMemoryRange *r) { (void)d; (void)n; (void)r; return VK_SUCCESS; }   // shared storage is coherent
VK(vkInvalidateMappedMemoryRanges, VkResult, VkDevice d, uint32_t n, const VkMappedMemoryRange *r) { (void)d; (void)n; (void)r; return VK_SUCCESS; }

VK(vkCreateBuffer, VkResult, VkDevice d, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *out) {
    (void)d; (void)a; VkBuffer b = zalloc(sizeof *b); b->size = ci->size; b->usage = ci->usage; *out = b; return VK_SUCCESS;
}
VK(vkDestroyBuffer, void, VkDevice d, VkBuffer b, const VkAllocationCallbacks *a) { (void)d; (void)a; free(b); }
VK(vkGetBufferMemoryRequirements, void, VkDevice d, VkBuffer b, VkMemoryRequirements *r) {
    (void)d; r->alignment = 256; r->size = (b->size + 255) & ~255ull; r->memoryTypeBits = 7;   // 256: Metal's constant-buffer offset alignment on the Mac
}
VK(vkGetBufferMemoryRequirements2, void, VkDevice d, const VkBufferMemoryRequirementsInfo2 *i, VkMemoryRequirements2 *r) {
    h_vkGetBufferMemoryRequirements(d, i->buffer, &r->memoryRequirements); dedicated_req(r->pNext);
}
VKB(vkBindBufferMemory, VkResult, VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize off) {
    (void)d; b->mem = m; b->off = off; return mem_buf(m) ? VK_SUCCESS : VK_ERROR_OUT_OF_DEVICE_MEMORY;
}
VKB(vkBindBufferMemory2, VkResult, VkDevice d, uint32_t n, const VkBindBufferMemoryInfo *bi) {
    for (uint32_t i = 0; i < n; i++) { VkResult r = h_vkBindBufferMemory(d, bi[i].buffer, bi[i].memory, bi[i].memoryOffset); if (r) return r; }
    return VK_SUCCESS;
}
VK(vkCreateBufferView, VkResult, VkDevice d, const VkBufferViewCreateInfo *ci, const VkAllocationCallbacks *a, VkBufferView *out) {
    (void)d; (void)a; VkBufferView v = zalloc(sizeof *v); v->buf = ci->buffer; v->f = fmt_of(ci->format); v->off = ci->offset; v->range = ci->range;
    if (!v->f || v->f->pf == MTLPixelFormatInvalid) RED1("buffer view format %d not mapped", ci->format);
    *out = v; return VK_SUCCESS;
}
VK(vkDestroyBufferView, void, VkDevice d, VkBufferView v, const VkAllocationCallbacks *a) { (void)d; (void)a; if (v) { v->tex = nil; free(v); } }
static id<MTLTexture> bview_tex(VkBufferView v) {   // texel buffer as a linear texture over the buffer's storage, made at first use
    if (v->tex || !v->f || v->f->pf == MTLPixelFormatInvalid || !v->buf->mem) return v->tex;
    VkDeviceSize range = v->range == VK_WHOLE_SIZE ? v->buf->size - v->off : v->range, n = range / v->f->bytes;
    MTLTextureDescriptor *td = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:v->f->pf width:n resourceOptions:MTLResourceStorageModeShared usage:MTLTextureUsageShaderRead];
    v->tex = [mem_buf(v->buf->mem) newTextureWithDescriptor:td offset:v->buf->off + v->off bytesPerRow:n * v->f->bytes];
    if (!v->tex) RED1("cannot create the texel buffer (offset %llu)", (unsigned long long)(v->buf->off + v->off));
    return v->tex;
}

// ---------- Images, views, samplers ----------
static VkDeviceSize img_bytes(const fmt_t *f, uint32_t w, uint32_t h, uint32_t d, uint32_t mips, uint32_t layers, uint32_t samples) {
    VkDeviceSize s = 0;
    for (uint32_t m = 0; m < mips; m++) {
        uint32_t mw = umax(1, w >> m), mh = umax(1, h >> m), md = umax(1, d >> m);
        s += (VkDeviceSize)((mw + f->bw - 1) / f->bw) * ((mh + f->bh - 1) / f->bh) * md * f->bytes;
    }
    return s * layers * samples;
}
// ---------- Converted formats (fmt_t.conv) ----------
static VkImage *g_conv; static size_t g_nconv, g_capconv; static pthread_mutex_t g_convmu = PTHREAD_MUTEX_INITIALIZER;
static void conv_add(VkImage im) {
    pthread_mutex_lock(&g_convmu);
    if (g_nconv == g_capconv) {
        size_t cap = g_capconv ? g_capconv * 2 : 256; VkImage *n = realloc(g_conv, cap * sizeof *n);
        if (!n) die("[vk] out of memory %zu", cap * sizeof *n);
        g_conv = n; g_capconv = cap;
    }
    g_conv[g_nconv++] = im; pthread_mutex_unlock(&g_convmu);
}
static void conv_remove(VkImage im) {
    pthread_mutex_lock(&g_convmu);
    for (size_t i = 0; i < g_nconv; i++) if (g_conv[i] == im) { g_conv[i] = g_conv[--g_nconv]; break; }
    pthread_mutex_unlock(&g_convmu);
}
static uint32_t x4(uint32_t v) { return v * 17; } static uint32_t x5(uint32_t v) { return (v << 3) | (v >> 2); } static uint32_t x6(uint32_t v) { return (v << 2) | (v >> 4); }
static void conv_unpack(VkImage im, const uint8_t *src, uint8_t *dst) {   // one row after another; dst rows are tightly packed
    uint32_t ob = im->f->conv == CV_R4G4 ? 2 : 4;
    for (uint32_t y = 0; y < im->h; y++) {
        const uint8_t *r = src + (size_t)y * im->pitch; uint8_t *o = dst + (size_t)y * im->w * ob;
        for (uint32_t x = 0; x < im->w; x++, o += ob) {
            if (im->f->conv == CV_R4G4) { o[0] = (uint8_t)x4(r[x] >> 4); o[1] = (uint8_t)x4(r[x] & 15); continue; }
            uint32_t v = r[2 * x] | (uint32_t)r[2 * x + 1] << 8;
            switch (im->f->conv) {
                case CV_B4G4R4A4: o[0] = (uint8_t)x4(v >> 4 & 15); o[1] = (uint8_t)x4(v >> 8 & 15); o[2] = (uint8_t)x4(v >> 12); o[3] = (uint8_t)x4(v & 15); break;
                case CV_B5G6R5: o[0] = (uint8_t)x5(v & 31); o[1] = (uint8_t)x6(v >> 5 & 63); o[2] = (uint8_t)x5(v >> 11); o[3] = 255; break;
                case CV_B5G5R5A1: o[0] = (uint8_t)x5(v >> 1 & 31); o[1] = (uint8_t)x5(v >> 6 & 31); o[2] = (uint8_t)x5(v >> 11); o[3] = v & 1 ? 255 : 0; break;
            }
        }
    }
}
// Before a submit: every converted linear image whose memory changed since it was last unpacked is unpacked again (the textures are small: N64 textures)
static void conv_sync(void) {
    static uint8_t *tmp; static size_t tcap;
    pthread_mutex_lock(&g_convmu);
    for (size_t i = 0; i < g_nconv; i++) {
        VkImage im = g_conv[i]; id<MTLBuffer> b = mem_buf(im->cmem); if (!b || !im->tex) continue;
        const uint8_t *src = (const uint8_t *)b.contents + im->coff;
        if (!memcmp(src, im->shadow, im->size)) continue;
        memcpy(im->shadow, src, im->size);
        uint32_t ob = im->f->conv == CV_R4G4 ? 2 : 4; size_t need = (size_t)im->w * im->h * ob;
        if (!need) continue;
        if (need > tcap) { uint8_t *n = realloc(tmp, need); if (!n) die("[vk] out of memory %zu", need); tmp = n; tcap = need; }
        conv_unpack(im, im->shadow, tmp);
        [im->tex replaceRegion:MTLRegionMake2D(0, 0, im->w, im->h) mipmapLevel:0 withBytes:tmp bytesPerRow:(NSUInteger)im->w * ob];
    }
    pthread_mutex_unlock(&g_convmu);
}
VKB(vkCreateImage, VkResult, VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out) {
    (void)d; (void)a; dev_init();
    const fmt_t *f = fmt_of(ci->format);
    if (!f || f->pf == MTLPixelFormatInvalid) { LOG("[vk] [RED] image format %d not mapped", ci->format); return VK_ERROR_FORMAT_NOT_SUPPORTED; }
    // Linear 2D images with one level and layer alias their memory, as on the Switch: the texture is made over the bound buffer at vkBindImageMemory, with rows at
    // Metal's linear-texture alignment (which vkGetImageSubresourceLayout reports). Super Mario Galaxy fills staging images from the CPU this way and samples a 4x4 one
    if (ci->tiling == VK_IMAGE_TILING_LINEAR && ci->imageType == VK_IMAGE_TYPE_2D && ci->mipLevels <= 1 && ci->arrayLayers <= 1 && ci->samples <= 1 && f->kind != K_BLOCK && !fmt_has_depth(f) && !fmt_has_stencil(f)) {
        NSUInteger al = [g_dev minimumLinearTextureAlignmentForPixelFormat:f->pf]; if (!al) al = 256;
        VkImage im = zalloc(sizeof *im);
        im->f = f; im->fmt = ci->format; im->type = ci->imageType; im->w = ci->extent.width; im->h = umax(1, ci->extent.height); im->d = im->mips = im->layers = im->samples = 1;
        im->usage = ci->usage; im->pitch = (uint32_t)(((NSUInteger)im->w * f->bytes + al - 1) / al * al); im->size = (VkDeviceSize)im->pitch * im->h;
        OLOG("CreateImage %p %ux%u format %d linear (pitch %u) usage %#x", (void *)im, im->w, im->h, ci->format, im->pitch, ci->usage);
        *out = im; return VK_SUCCESS;
    }
    if (f->conv) { LOG("[vk] [RED] image format %d is converted only for linear images (this one is optimal)", ci->format); return VK_ERROR_FORMAT_NOT_SUPPORTED; }
    if (ci->tiling == VK_IMAGE_TILING_LINEAR) RED1("linear image %ux%u format %d levels %u layers %u: content does not follow the bound memory", ci->extent.width, ci->extent.height, ci->format, ci->mipLevels, ci->arrayLayers);
    @autoreleasepool {
        uint32_t layers = umax(1, ci->arrayLayers), samples = umax(1, ci->samples);
        int cube = (ci->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) && layers % 6 == 0;
        MTLTextureDescriptor *td = [MTLTextureDescriptor new];
        td.pixelFormat = f->pf; td.width = ci->extent.width; td.height = umax(1, ci->extent.height); td.depth = umax(1, ci->extent.depth);
        td.mipmapLevelCount = umax(1, ci->mipLevels); td.sampleCount = samples;
        // A 1D image is a one-row 2D (array) texture, as in the NVN layer and in MoltenVK: Metal's 1D texture is no render target, has one level and no depth format, all of which
        // Vulkan's 1D image allows (Hades draws into a 512x1 colour target), and vk_shaders.py translates 1D images as texture2d (spirv-cross --msl-texture-1d-as-2d)
        if (ci->imageType == VK_IMAGE_TYPE_3D) td.textureType = MTLTextureType3D;
        else if (samples > 1) td.textureType = layers > 1 ? MTLTextureType2DMultisampleArray : MTLTextureType2DMultisample;
        else if (cube) td.textureType = layers > 6 ? MTLTextureTypeCubeArray : MTLTextureTypeCube;
        else td.textureType = layers > 1 ? MTLTextureType2DArray : MTLTextureType2D;
        td.arrayLength = cube ? layers / 6 : layers;
        MTLTextureUsage u = MTLTextureUsageShaderRead;
        if (ci->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) u |= MTLTextureUsageRenderTarget;   // a transfer destination may be cleared (C_CLEARI: a clearing pass)
        if (samples > 1 && (ci->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && f->kind == K_COLOR) u |= MTLTextureUsageRenderTarget;
        if (ci->usage & VK_IMAGE_USAGE_STORAGE_BIT) u |= MTLTextureUsageShaderWrite;
        // A stencil-aspect view of a depth-stencil image is an X32_Stencil8 view, which Metal allows only with the format-view usage (Vulkan needs no mutable-format flag for it;
        // the API validation layer aborts without it). As in the NVN layer, it costs nothing measured on a depth-stencil texture
        if ((ci->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) || f->pf == MTLPixelFormatDepth32Float_Stencil8) u |= MTLTextureUsagePixelFormatView;
        td.usage = u; td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> t = [g_dev newTextureWithDescriptor:td];
        if (!t) { LOG("[vk] [RED] cannot create image %ux%ux%u format %d levels %u layers %u", ci->extent.width, ci->extent.height, ci->extent.depth, ci->format, ci->mipLevels, layers); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
        VkImage im = zalloc(sizeof *im);
        im->tex = t; im->f = f; im->fmt = ci->format; im->type = ci->imageType; im->w = (uint32_t)td.width; im->h = (uint32_t)td.height; im->d = (uint32_t)td.depth;
        im->usage = ci->usage;
        im->mips = (uint32_t)td.mipmapLevelCount; im->layers = layers; im->samples = samples; im->size = img_bytes(f, im->w, im->h, im->d, im->mips, layers, samples);
        OLOG("CreateImage %p %ux%ux%u format %d levels %u layers %u usage %#x", (void *)im, im->w, im->h, im->d, ci->format, im->mips, layers, ci->usage);
        *out = im;
    }
    return VK_SUCCESS;
}
// A swap chain's image is not the game's to destroy, but Super Mario 3D All-Stars' Super Mario 64 destroys the images vkGetSwapchainImagesKHR gave it as it tears down (main+0x9500) and keeps the
// swap chain, as the console's driver lets it: ignored, or the swap chain would go on using freed memory
VKB(vkDestroyImage, void, VkDevice d, VkImage im, const VkAllocationCallbacks *a) { (void)d; (void)a; if (im && im->swap) return; if (im) { if (im->shadow) conv_remove(im); im->tex = nil; free(im->shadow); free(im); } }
VK(vkGetImageMemoryRequirements, void, VkDevice d, VkImage im, VkMemoryRequirements *r) {
    (void)d; r->alignment = 4096; r->size = (im->size + 4095) & ~4095ull; r->memoryTypeBits = 7;
}
VK(vkGetImageMemoryRequirements2, void, VkDevice d, const VkImageMemoryRequirementsInfo2 *i, VkMemoryRequirements2 *r) {
    h_vkGetImageMemoryRequirements(d, i->image, &r->memoryRequirements); dedicated_req(r->pNext);
}
VK(vkGetImageSubresourceLayout, void, VkDevice d, VkImage im, const VkImageSubresource *sr, VkSubresourceLayout *l) {   // a linear image's own pitch; others tightly packed, mip levels then layers
    (void)d; const fmt_t *f = im->f; VkDeviceSize off = 0;
    if (im->pitch) { l->offset = 0; l->rowPitch = im->pitch; l->depthPitch = l->arrayPitch = l->size = im->size; return; }
    for (uint32_t m = 0; m < sr->mipLevel && m < im->mips; m++) off += img_bytes(f, umax(1, im->w >> m), umax(1, im->h >> m), umax(1, im->d >> m), 1, im->layers, im->samples);
    uint32_t w = umax(1, im->w >> sr->mipLevel), h = umax(1, im->h >> sr->mipLevel), dd = umax(1, im->d >> sr->mipLevel);
    l->rowPitch = (VkDeviceSize)((w + f->bw - 1) / f->bw) * f->bytes; l->depthPitch = l->rowPitch * ((h + f->bh - 1) / f->bh);
    l->arrayPitch = l->depthPitch * dd; l->size = l->arrayPitch; l->offset = off + l->arrayPitch * sr->arrayLayer;
}
VKB(vkBindImageMemory, VkResult, VkDevice d, VkImage im, VkDeviceMemory m, VkDeviceSize off) {   // other images keep their own storage
    (void)d; if (!im->pitch) return VK_SUCCESS;
    id<MTLBuffer> b = mem_buf(m); if (!b) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (im->f->conv) {   // a texture of its own, unpacked from the memory before each submit that follows a change (conv_sync)
        @autoreleasepool {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:im->f->pf width:im->w height:im->h mipmapped:NO];
            td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead; im->tex = [g_dev newTextureWithDescriptor:td];
        }
        im->cmem = m; im->coff = off; im->shadow = calloc(1, im->size);
        memset(im->shadow, 0xA5, im->size);   // differs from anything the game wrote, so the first submit unpacks it
        conv_add(im);
        return im->tex ? VK_SUCCESS : VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    @autoreleasepool {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:im->f->pf width:im->w height:im->h mipmapped:NO];
        td.storageMode = MTLStorageModeShared; td.resourceOptions = MTLResourceStorageModeShared;
        td.usage = MTLTextureUsageShaderRead | (im->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT ? MTLTextureUsageRenderTarget : 0) | (im->usage & VK_IMAGE_USAGE_STORAGE_BIT ? MTLTextureUsageShaderWrite : 0);
        im->tex = [b newTextureWithDescriptor:td offset:off bytesPerRow:im->pitch];
    }
    if (!im->tex) { LOG("[vk] [RED] cannot create linear image %ux%u format %d (offset %llu pitch %u)", im->w, im->h, im->fmt, (unsigned long long)off, im->pitch); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    return VK_SUCCESS;
}
VKB(vkBindImageMemory2, VkResult, VkDevice d, uint32_t n, const VkBindImageMemoryInfo *bi) {
    for (uint32_t i = 0; i < n; i++) { VkResult r = h_vkBindImageMemory(d, bi[i].image, bi[i].memory, bi[i].memoryOffset); if (r) return r; }
    return VK_SUCCESS;
}

static MTLTextureSwizzle swz(VkComponentSwizzle s, MTLTextureSwizzle own) {
    switch (s) {
        case VK_COMPONENT_SWIZZLE_ZERO: return MTLTextureSwizzleZero;
        case VK_COMPONENT_SWIZZLE_ONE: return MTLTextureSwizzleOne;
        case VK_COMPONENT_SWIZZLE_R: return MTLTextureSwizzleRed;
        case VK_COMPONENT_SWIZZLE_G: return MTLTextureSwizzleGreen;
        case VK_COMPONENT_SWIZZLE_B: return MTLTextureSwizzleBlue;
        case VK_COMPONENT_SWIZZLE_A: return MTLTextureSwizzleAlpha;
        default: return own;
    }
}
VKB(vkCreateImageView, VkResult, VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *out) {
    (void)d; (void)a;
    VkImage im = ci->image; const fmt_t *f = fmt_of(ci->format);
    MTLPixelFormat pf = f && f->pf != MTLPixelFormatInvalid ? f->pf : im->f->pf;
    const VkImageSubresourceRange *r = &ci->subresourceRange;
    if (r->aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT && pf == MTLPixelFormatDepth32Float_Stencil8) pf = MTLPixelFormatX32_Stencil8;
    uint32_t lv = r->levelCount == VK_REMAINING_MIP_LEVELS ? im->mips - r->baseMipLevel : r->levelCount;
    uint32_t ly = r->layerCount == VK_REMAINING_ARRAY_LAYERS ? im->layers - r->baseArrayLayer : r->layerCount;
    MTLTextureType tt;
    switch (ci->viewType) {
        case VK_IMAGE_VIEW_TYPE_1D_ARRAY: tt = MTLTextureType2DArray; break;   // a 1D image is a one-row 2D texture (vkCreateImage); a 1D view takes the 2D default below
        case VK_IMAGE_VIEW_TYPE_3D: tt = MTLTextureType3D; break;
        case VK_IMAGE_VIEW_TYPE_CUBE: tt = MTLTextureTypeCube; break;
        case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: tt = MTLTextureTypeCubeArray; break;
        case VK_IMAGE_VIEW_TYPE_2D_ARRAY: tt = im->samples > 1 ? MTLTextureType2DMultisampleArray : MTLTextureType2DArray; break;
        default: tt = im->samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D; break;
    }
    int bc1rgb = im->fmt == VK_FORMAT_BC1_RGB_UNORM_BLOCK || im->fmt == VK_FORMAT_BC1_RGB_SRGB_BLOCK;   // Metal decodes BC1 with punch-through alpha; Vulkan's RGB variant has none
    MTLTextureSwizzleChannels sw = MTLTextureSwizzleChannelsMake(swz(ci->components.r, MTLTextureSwizzleRed), swz(ci->components.g, MTLTextureSwizzleGreen),
                                                                 swz(ci->components.b, MTLTextureSwizzleBlue), bc1rgb ? MTLTextureSwizzleOne : swz(ci->components.a, MTLTextureSwizzleAlpha));
    int ident = sw.red == MTLTextureSwizzleRed && sw.green == MTLTextureSwizzleGreen && sw.blue == MTLTextureSwizzleBlue && sw.alpha == MTLTextureSwizzleAlpha;
    VkImageView v = zalloc(sizeof *v); v->img = im;
    if (pf == im->tex.pixelFormat && tt == im->tex.textureType && !r->baseMipLevel && lv == im->mips && !r->baseArrayLayer && ly == im->layers && ident) v->tex = im->tex;
    else @autoreleasepool {
        v->tex = [im->tex newTextureViewWithPixelFormat:pf textureType:tt levels:NSMakeRange(r->baseMipLevel, lv)
                                                  slices:NSMakeRange(tt == MTLTextureType3D ? 0 : r->baseArrayLayer, tt == MTLTextureType3D ? 1 : ly) swizzle:sw];
        if (!v->tex) LOG("[vk] [RED] cannot create image view: format %d → %lu type %lu levels %u+%u layers %u+%u", im->fmt, (unsigned long)pf, (unsigned long)tt, r->baseMipLevel, lv, r->baseArrayLayer, ly);
    }
    *out = v; return VK_SUCCESS;
}
VKB(vkDestroyImageView, void, VkDevice d, VkImageView v, const VkAllocationCallbacks *a) { (void)d; (void)a; if (v) { v->tex = nil; free(v); } }

static MTLSamplerAddressMode addr_mode(VkSamplerAddressMode m) {
    switch (m) {
        case VK_SAMPLER_ADDRESS_MODE_REPEAT: return MTLSamplerAddressModeRepeat;
        case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: return MTLSamplerAddressModeMirrorRepeat;
        case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER: return MTLSamplerAddressModeClampToBorderColor;
        case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE: return MTLSamplerAddressModeMirrorClampToEdge;
        default: return MTLSamplerAddressModeClampToEdge;
    }
}
VKB(vkCreateSampler, VkResult, VkDevice d, const VkSamplerCreateInfo *ci, const VkAllocationCallbacks *a, VkSampler *out) {
    (void)d; (void)a; dev_init();
    @autoreleasepool {
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = ci->minFilter == VK_FILTER_LINEAR ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.magFilter = ci->magFilter == VK_FILTER_LINEAR ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.mipFilter = ci->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
        sd.sAddressMode = addr_mode(ci->addressModeU); sd.tAddressMode = addr_mode(ci->addressModeV); sd.rAddressMode = addr_mode(ci->addressModeW);
        sd.maxAnisotropy = ci->anisotropyEnable ? (NSUInteger)fminf(fmaxf(ci->maxAnisotropy, 1), 16) : 1;
        if (ci->compareEnable) sd.compareFunction = (MTLCompareFunction)ci->compareOp;   // VkCompareOp and MTLCompareFunction share their values
        sd.borderColor = ci->borderColor >= VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE ? MTLSamplerBorderColorOpaqueWhite :
                         ci->borderColor >= VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK ? MTLSamplerBorderColorOpaqueBlack : MTLSamplerBorderColorTransparentBlack;
        sd.lodMinClamp = ci->minLod; sd.lodMaxClamp = ci->maxLod;
        sd.normalizedCoordinates = !ci->unnormalizedCoordinates;
        sd.lodBias = fminf(fmaxf(ci->mipLodBias, -15), 15);   // Metal 26's sampler bias; the device reports maxSamplerLodBias 15
        VkSampler s = zalloc(sizeof *s); s->ref = [VkSamplerMetal new]; s->ref.descriptor = [sd copy];
        s->s = [g_dev newSamplerStateWithDescriptor:sd]; s->ref.state = s->s;
        if (!s->s) LOG("[vk] [RED] cannot create the sampler");
        *out = s;
    }
    return VK_SUCCESS;
}
VKB(vkDestroySampler, void, VkDevice d, VkSampler s, const VkAllocationCallbacks *a) {
    (void)d; (void)a;
    if (s) { s->s = nil; s->ref = nil; free(s); }
}

// ---------- Descriptor sets ----------
static int is_dyn(VkDescriptorType t) { return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC; }
VK(vkCreateDescriptorSetLayout, VkResult, VkDevice d, const VkDescriptorSetLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorSetLayout *out) {
    (void)d; (void)a;
    if (ci->bindingCount > MAXB) { LOG("[vk] [RED] descriptor set layout with %u bindings, over %d", ci->bindingCount, MAXB); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    VkDescriptorSetLayout L = zalloc(sizeof *L); memset(L->idx, -1, sizeof L->idx); L->refs = 1;
    const VkDescriptorSetLayoutBinding *src[MAXB];
    for (uint32_t i = 0; i < ci->bindingCount; i++) {   // sorted by binding number: the order dynamic offsets are consumed in
        uint32_t j = i; src[i] = &ci->pBindings[i];
        while (j && src[j - 1]->binding > src[j]->binding) { const VkDescriptorSetLayoutBinding *t = src[j]; src[j] = src[j - 1]; src[j - 1] = t; j--; }
    }
    int nimm = 0;
    for (uint32_t i = 0; i < ci->bindingCount; i++) {
        const VkDescriptorSetLayoutBinding *b = src[i]; lbind *o = &L->b[L->n];
        if (b->binding >= 128) { LOG("[vk] [RED] binding number %u over 127", b->binding); continue; }
        o->binding = b->binding; o->count = b->descriptorCount; o->type = b->descriptorType; o->base = L->total; o->dyn = -1;
        if (is_dyn(b->descriptorType)) { o->dyn = (int)L->ndyn; L->ndyn += b->descriptorCount; }
        o->imm = b->pImmutableSamplers && (b->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER || b->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        nimm += o->imm;
        L->idx[b->binding] = (int8_t)L->n++; L->total += b->descriptorCount;
    }
    if (nimm) {
        L->imm = (__strong VkSamplerMetal **)zalloc(umax(1, L->total) * sizeof *L->imm);
        for (uint32_t i = 0, k = 0; i < ci->bindingCount; i++) {
            const VkDescriptorSetLayoutBinding *b = src[i]; if (b->binding >= 128) continue;
            if (L->b[k].imm) for (uint32_t j = 0; j < b->descriptorCount; j++)
                L->imm[L->b[k].base + j] = b->pImmutableSamplers[j] ? b->pImmutableSamplers[j]->ref : nil;
            k++;
        }
    }
    if (L->ndyn > 16) LOG("[vk] [RED] %u dynamic offsets in one set, over 16", L->ndyn);
    *out = L; return VK_SUCCESS;
}
static void dsl_put(VkDescriptorSetLayout L) {
    if (!L || atomic_fetch_sub(&L->refs, 1) != 1) return;
    if (L->imm) for (uint32_t i = 0; i < L->total; i++) L->imm[i] = nil;
    free(L->imm); free(L);
}
VK(vkDestroyDescriptorSetLayout, void, VkDevice d, VkDescriptorSetLayout L, const VkAllocationCallbacks *a) { (void)d; (void)a; dsl_put(L); }   // sets allocated from it keep it (refs)
VK(vkCreateDescriptorPool, VkResult, VkDevice d, const VkDescriptorPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorPool *out) {
    (void)d; (void)ci; (void)a; *out = zalloc(sizeof **out); return VK_SUCCESS;
}
// The sets that exist: a set freed with its pool (reset or destroyed) may still be named to vkFreeDescriptorSets, against the Vulkan rules: Super Mario 3D All-Stars' Super Mario 64 does so
// as it tears down on the way back to the launcher, and freeing the set again read freed memory (a crash in set_free). The console's driver lets it pass, and a free of a set that is gone is ignored here
static pthread_mutex_t g_sets_mu = PTHREAD_MUTEX_INITIALIZER; static CFMutableSetRef g_sets;
static void set_live(VkDescriptorSet s, int on) {
    pthread_mutex_lock(&g_sets_mu);
    if (!g_sets) g_sets = CFSetCreateMutable(NULL, 0, NULL);
    if (on) CFSetAddValue(g_sets, s); else CFSetRemoveValue(g_sets, s);
    pthread_mutex_unlock(&g_sets_mu);
}
static int set_is_live(VkDescriptorSet s) { pthread_mutex_lock(&g_sets_mu); int r = g_sets && CFSetContainsValue(g_sets, s); pthread_mutex_unlock(&g_sets_mu); return r; }
static void set_free(VkDescriptorSet s) {
    VkDescriptorPool p = s->pool; set_live(s, 0);
    if (s->prev) s->prev->next = s->next; else p->sets = s->next;
    if (s->next) s->next->prev = s->prev;
    for (uint32_t k = 0; k < s->L->total; k++) s->d[k] = (desc_t){0};   // releases what the descriptors hold
    dsl_put(s->L); free(s->d); free(s);
}
VK(vkResetDescriptorPool, VkResult, VkDevice d, VkDescriptorPool p, VkDescriptorPoolResetFlags f) { (void)d; (void)f; while (p->sets) set_free(p->sets); return VK_SUCCESS; }
VK(vkDestroyDescriptorPool, void, VkDevice d, VkDescriptorPool p, const VkAllocationCallbacks *a) { (void)a; if (p) { h_vkResetDescriptorPool(d, p, 0); free(p); } }
VK(vkAllocateDescriptorSets, VkResult, VkDevice d, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out) {
    (void)d;
    for (uint32_t i = 0; i < ai->descriptorSetCount; i++) {
        VkDescriptorSetLayout L = ai->pSetLayouts[i];
        VkDescriptorSet s = zalloc(sizeof *s); s->d = zalloc(umax(1, L->total) * sizeof(desc_t));
        atomic_fetch_add(&L->refs, 1); s->L = L; s->pool = ai->descriptorPool; s->next = s->pool->sets; if (s->next) s->next->prev = s; s->pool->sets = s; set_live(s, 1); s->gen = ++g_set_gen;
        if (L->imm) for (uint32_t k = 0; k < L->total; k++) s->d[k].smp = L->imm[k];
        out[i] = s;
    }
    return VK_SUCCESS;
}
VK(vkFreeDescriptorSets, VkResult, VkDevice d, VkDescriptorPool p, uint32_t n, const VkDescriptorSet *s) {
    (void)d; (void)p; for (uint32_t i = 0; i < n; i++) if (s[i] && set_is_live(s[i])) set_free(s[i]);
    return VK_SUCCESS;
}
// Element `elem` of `binding`; an index past the binding's count continues into the next bindings (Vulkan's consecutive-binding updates)
static desc_t *set_elem(VkDescriptorSet s, uint32_t binding, uint32_t elem, const lbind **lb) {
    const VkDescriptorSetLayout L = s->L; int i = binding < 128 ? L->idx[binding] : -1;
    if (i < 0) { RED1("write descriptor: binding %u not in the layout", binding); return NULL; }
    while (elem >= L->b[i].count) { elem -= L->b[i].count; if (++i >= (int)L->n) { RED1("write descriptor past the end of the layout (binding %u)", binding); return NULL; } }
    *lb = &L->b[i]; return &s->d[L->b[i].base + elem];
}
static void desc_write(desc_t *e, const lbind *lb, VkDescriptorType t, const VkDescriptorImageInfo *ii, const VkDescriptorBufferInfo *bi, const VkBufferView *bv) {
    switch (t) {
        case VK_DESCRIPTOR_TYPE_SAMPLER: if (!lb->imm) e->smp = ii->sampler ? ii->sampler->ref : nil; break;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: e->tex = ii->imageView ? ii->imageView->tex : nil; if (!lb->imm) e->smp = ii->sampler ? ii->sampler->ref : nil; break;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: e->tex = ii->imageView ? ii->imageView->tex : nil; break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: e->tex = *bv ? bview_tex(*bv) : nil; break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
            VkBuffer b = bi->buffer; e->buf = b && b->mem ? mem_buf(b->mem) : nil; e->off = b ? b->off + bi->offset : 0; break;   // a buffer is bound to its memory before it is written
        }
        default: RED1("descriptor type %d not implemented", t); break;
    }
}
// A copy takes the source descriptor whole, but a destination binding with immutable samplers keeps its own sampler, as a write does: Vulkan's updates (copies too)
// change only the image of a combined image sampler there, and a sampler-only copy into one is not allowed. A source's immutable sampler is part of what it copies
static void desc_copy(desc_t *e, const lbind *lb, const desc_t *s) {
    VkSamplerMetal *keep = e->smp;
    *e = *s;
    if (lb->imm) e->smp = keep;
}
VK(vkUpdateDescriptorSets, void, VkDevice d, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *c) {
    (void)d;
    for (uint32_t i = 0; i < nw; i++)
        for (uint32_t j = 0; j < w[i].descriptorCount; j++) {
            const lbind *lb; desc_t *e = set_elem(w[i].dstSet, w[i].dstBinding, w[i].dstArrayElement + j, &lb); if (!e) break;
            desc_write(e, lb, w[i].descriptorType, w[i].pImageInfo ? &w[i].pImageInfo[j] : NULL, w[i].pBufferInfo ? &w[i].pBufferInfo[j] : NULL,
                       w[i].pTexelBufferView ? &w[i].pTexelBufferView[j] : NULL);
        }
    for (uint32_t i = 0; i < nc; i++)
        for (uint32_t j = 0; j < c[i].descriptorCount; j++) {
            const lbind *la, *lb; desc_t *s = set_elem(c[i].srcSet, c[i].srcBinding, c[i].srcArrayElement + j, &la), *t = set_elem(c[i].dstSet, c[i].dstBinding, c[i].dstArrayElement + j, &lb);
            if (s && t) desc_copy(t, lb, s);
        }
    for (uint32_t i = 0; i < nw; i++) if (w[i].dstSet) w[i].dstSet->gen = ++g_set_gen;
    for (uint32_t i = 0; i < nc; i++) if (c[i].dstSet) c[i].dstSet->gen = ++g_set_gen;
}
VK(vkCreateDescriptorUpdateTemplate, VkResult, VkDevice d, const VkDescriptorUpdateTemplateCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorUpdateTemplate *out) {
    (void)d; (void)a;
    if (ci->templateType != VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET) RED1("descriptor update template type %d not implemented", ci->templateType);
    VkDescriptorUpdateTemplate t = zalloc(sizeof *t + ci->descriptorUpdateEntryCount * sizeof(VkDescriptorUpdateTemplateEntry));
    t->n = ci->descriptorUpdateEntryCount; memcpy(t->e, ci->pDescriptorUpdateEntries, t->n * sizeof *t->e);
    *out = t; return VK_SUCCESS;
}
VK(vkDestroyDescriptorUpdateTemplate, void, VkDevice d, VkDescriptorUpdateTemplate t, const VkAllocationCallbacks *a) { (void)d; (void)a; free(t); }
VK(vkUpdateDescriptorSetWithTemplate, void, VkDevice d, VkDescriptorSet s, VkDescriptorUpdateTemplate t, const void *data) {
    (void)d;
    for (uint32_t i = 0; i < t->n; i++) {
        const VkDescriptorUpdateTemplateEntry *en = &t->e[i];
        for (uint32_t j = 0; j < en->descriptorCount; j++) {
            const uint8_t *p = (const uint8_t *)data + en->offset + j * en->stride;
            const lbind *lb; desc_t *e = set_elem(s, en->dstBinding, en->dstArrayElement + j, &lb); if (!e) break;
            desc_write(e, lb, en->descriptorType, (const VkDescriptorImageInfo *)p, (const VkDescriptorBufferInfo *)p, (const VkBufferView *)p);
        }
    }
    s->gen = ++g_set_gen;
}
VK(vkCreatePipelineLayout, VkResult, VkDevice d, const VkPipelineLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkPipelineLayout *out) {
    (void)d; (void)a; VkPipelineLayout l = zalloc(sizeof *l);
    if (ci->setLayoutCount > 8) LOG("[vk] [RED] pipeline layout with %u sets, over 8", ci->setLayoutCount);
    l->nset = umin(ci->setLayoutCount, 8); memcpy(l->set, ci->pSetLayouts, l->nset * sizeof *l->set);
    for (uint32_t i = 0; i < ci->pushConstantRangeCount; i++) l->pc_size = umax(l->pc_size, ci->pPushConstantRanges[i].offset + ci->pPushConstantRanges[i].size);
    if (l->pc_size > 256) { LOG("[vk] [RED] push constant range %u bytes, over 256", l->pc_size); l->pc_size = 256; }
    *out = l; return VK_SUCCESS;
}
VK(vkDestroyPipelineLayout, void, VkDevice d, VkPipelineLayout l, const VkAllocationCallbacks *a) { (void)d; (void)a; free(l); }   // pipelines copied what they use; commands ignore the layout

// ---------- Render passes, framebuffers ----------
VK(vkCreateRenderPass, VkResult, VkDevice d, const VkRenderPassCreateInfo *ci, const VkAllocationCallbacks *a, VkRenderPass *out) {
    (void)d; (void)a;
    if (ci->attachmentCount > 16 || !ci->subpassCount) { LOG("[vk] [RED] render pass: %u attachments, %u subpasses", ci->attachmentCount, ci->subpassCount); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    if (ci->subpassCount > 1) RED1("render pass has %u subpasses: only the first is used", ci->subpassCount);
    VkRenderPass rp = zalloc(sizeof *rp); rp->natt = ci->attachmentCount;
    for (uint32_t i = 0; i < rp->natt; i++) {
        const VkAttachmentDescription *at = &ci->pAttachments[i];
        rp->att[i] = (ratt){at->format, at->loadOp, at->stencilLoadOp, at->storeOp, at->stencilStoreOp};
    }
    const VkSubpassDescription *sp = &ci->pSubpasses[0];
    rp->ncol = umin(sp->colorAttachmentCount, 8);
    for (uint32_t i = 0; i < rp->ncol; i++) {
        uint32_t c = sp->pColorAttachments[i].attachment, r = sp->pResolveAttachments ? sp->pResolveAttachments[i].attachment : VK_ATTACHMENT_UNUSED;
        rp->col[i] = c == VK_ATTACHMENT_UNUSED ? -1 : (int)c; rp->res[i] = r == VK_ATTACHMENT_UNUSED ? -1 : (int)r;
    }
    rp->depth = sp->pDepthStencilAttachment && sp->pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED ? (int)sp->pDepthStencilAttachment->attachment : -1;
    if (sp->inputAttachmentCount) RED1("input attachments not implemented");
    *out = rp; return VK_SUCCESS;
}
// Commands are replayed at vkQueueSubmit and pipelines copy the formats, so nothing reads a render pass after its destroy: a command buffer that recorded it may not be submitted again (Vulkan), as for framebuffers
VK(vkDestroyRenderPass, void, VkDevice d, VkRenderPass rp, const VkAllocationCallbacks *a) { (void)d; (void)a; free(rp); }
VK(vkCreateFramebuffer, VkResult, VkDevice d, const VkFramebufferCreateInfo *ci, const VkAllocationCallbacks *a, VkFramebuffer *out) {
    (void)d; (void)a;
    if (ci->attachmentCount > 16) { LOG("[vk] [RED] framebuffer with %u attachments", ci->attachmentCount); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    VkFramebuffer fb = zalloc(sizeof *fb); fb->n = ci->attachmentCount; fb->w = ci->width; fb->h = ci->height; fb->layers = ci->layers;
    memcpy(fb->v, ci->pAttachments, fb->n * sizeof *fb->v);
    *out = fb; return VK_SUCCESS;
}
VK(vkDestroyFramebuffer, void, VkDevice d, VkFramebuffer fb, const VkAllocationCallbacks *a) { (void)d; (void)a; free(fb); }

// ---------- Shaders and pipelines ----------
// DM_VK_CAPTURE=<dir> (Mac): every SPIR-V module the game creates is written to <dir>/<key>.spv. Super Mario Galaxy generates its GX shaders as SPIR-V at run time
// (over 3,000 at boot, from the pipeline database), so no game file holds them; vk_shaders.py translates a capture folder like a RomFS
static void sh_capture(const char *key, const void *code, size_t n) {
    static const char *dir; static int init;
    if (!init) { init = 1; dir = getenv("DM_VK_CAPTURE"); if (dir) VLOG("shader capture → %s", dir); }
    if (!dir) return;
    char p[1024]; snprintf(p, sizeof p, "%s/%s.spv", dir, key);
    if (access(p, F_OK) == 0) return;
    char t[1100]; snprintf(t, sizeof t, "%s.%d.tmp", p, getpid());
    FILE *f = fopen(t, "wb"); if (!f) { RED1("shader capture cannot write to %s", dir); return; }
    int ok = fwrite(code, 1, n, f) == n; ok &= fclose(f) == 0;
    if (ok) rename(t, p); else unlink(t);   // whole files only: a capture run may be killed at any time
}
VKB(vkCreateShaderModule, VkResult, VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *out) {
    (void)d; (void)a; dev_init();
    uint8_t h[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(ci->pCode, (CC_LONG)ci->codeSize, h);
    VkShaderModule m = zalloc(sizeof *m);
    for (int i = 0; i < 8; i++) snprintf(m->key + 2 * i, 3, "%02x", h[i]);
    sh_capture(m->key, ci->pCode, ci->codeSize);
    m->si = sh_find(m->key);
    @autoreleasepool {
        if (m->si && !m->si->geom_vp && g_lib) {
            m->fn = [g_lib newFunctionWithName:[NSString stringWithFormat:@"vk_%s", m->key]];
        }
    }
    if (!m->fn && !(m->si && m->si->geom_vp)) LOG("[vk] [RED] shader %s (%zu bytes of SPIR-V) is not in the library", m->key, ci->codeSize);
    else OLOG("CreateShaderModule %s", m->key);
    *out = m; return VK_SUCCESS;
}
VKB(vkDestroyShaderModule, void, VkDevice d, VkShaderModule m, const VkAllocationCallbacks *a) { (void)d; (void)a; if (m) { m->fn = m->fn_d = nil; free(m); } }
// Pipeline caches hold nothing: the game's initial data is NVIDIA's binary cache (Super Mario Galaxy's data/database/vulkan/*.cache), and Metal keeps its own
VK(vkCreatePipelineCache, VkResult, VkDevice d, const VkPipelineCacheCreateInfo *ci, const VkAllocationCallbacks *a, VkPipelineCache *out) {
    (void)d; (void)ci; (void)a; *out = zalloc(sizeof **out); return VK_SUCCESS;
}
VK(vkDestroyPipelineCache, void, VkDevice d, VkPipelineCache c, const VkAllocationCallbacks *a) { (void)d; (void)a; free(c); }
VK(vkGetPipelineCacheData, VkResult, VkDevice d, VkPipelineCache c, size_t *n, void *data) { (void)d; (void)c; (void)data; *n = 0; return VK_SUCCESS; }
VK(vkMergePipelineCaches, VkResult, VkDevice d, VkPipelineCache dst, uint32_t n, const VkPipelineCache *src) { (void)d; (void)dst; (void)n; (void)src; return VK_SUCCESS; }

static MTLBlendFactor blend_f(VkBlendFactor f) {
    static const MTLBlendFactor t[] = {MTLBlendFactorZero, MTLBlendFactorOne, MTLBlendFactorSourceColor, MTLBlendFactorOneMinusSourceColor, MTLBlendFactorDestinationColor,
        MTLBlendFactorOneMinusDestinationColor, MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha, MTLBlendFactorDestinationAlpha, MTLBlendFactorOneMinusDestinationAlpha,
        MTLBlendFactorBlendColor, MTLBlendFactorOneMinusBlendColor, MTLBlendFactorBlendAlpha, MTLBlendFactorOneMinusBlendAlpha, MTLBlendFactorSourceAlphaSaturated,
        MTLBlendFactorSource1Color, MTLBlendFactorOneMinusSource1Color, MTLBlendFactorSource1Alpha, MTLBlendFactorOneMinusSource1Alpha};
    return (unsigned)f < sizeof t / sizeof *t ? t[f] : MTLBlendFactorOne;
}
static MTLColorWriteMask write_mask(VkColorComponentFlags m) {   // the bit order is reversed: Vulkan R = 1 … A = 8, Metal R = 8 … A = 1
    return (m & VK_COLOR_COMPONENT_R_BIT ? MTLColorWriteMaskRed : 0) | (m & VK_COLOR_COMPONENT_G_BIT ? MTLColorWriteMaskGreen : 0) |
           (m & VK_COLOR_COMPONENT_B_BIT ? MTLColorWriteMaskBlue : 0) | (m & VK_COLOR_COMPONENT_A_BIT ? MTLColorWriteMaskAlpha : 0);
}
static MTLStencilDescriptor *stencil_d(const VkStencilOpState *s) {   // VkStencilOp / VkCompareOp share their values with Metal's
    MTLStencilDescriptor *d = [MTLStencilDescriptor new];
    d.stencilCompareFunction = (MTLCompareFunction)s->compareOp; d.stencilFailureOperation = (MTLStencilOperation)s->failOp;
    d.depthFailureOperation = (MTLStencilOperation)s->depthFailOp; d.depthStencilPassOperation = (MTLStencilOperation)s->passOp;
    d.readMask = s->compareMask; d.writeMask = s->writeMask;
    return d;
}
static MTLViewport vp_of(const VkViewport *v) { return (MTLViewport){v->x, v->y, v->width, v->height, v->minDepth, v->maxDepth}; }   // the shaders flip y, so the viewport stays as given
static int src1_factor(VkBlendFactor f) { return f >= VK_BLEND_FACTOR_SRC1_COLOR && f <= VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA; }
static int dual_source(const VkPipelineColorBlendStateCreateInfo *cb) {   // a blend factor reads the second source
    for (uint32_t i = 0; cb && i < cb->attachmentCount; i++) {
        const VkPipelineColorBlendAttachmentState *b = &cb->pAttachments[i];
        if (b->blendEnable && (src1_factor(b->srcColorBlendFactor) || src1_factor(b->dstColorBlendFactor) || src1_factor(b->srcAlphaBlendFactor) || src1_factor(b->dstAlphaBlendFactor))) return 1;
    }
    return 0;
}
// Specialization constants: SPIRV-Cross keeps each SpecId as a Metal function constant of the same index, and Metal builds no pipeline from a function that has constants
// until they are given (Super Mario 3D All-Stars' Super Mario 64). The values the pipeline gives are set; the others keep the defaults the translation wrote
static id<MTLFunction> spec_fn(id<MTLFunction> fn, const VkSpecializationInfo *si) {
    if (!fn || !fn.functionConstantsDictionary.count) return fn;
    MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
    for (MTLFunctionConstant *c in fn.functionConstantsDictionary.allValues)
        for (uint32_t i = 0; si && i < si->mapEntryCount; i++) {
            const VkSpecializationMapEntry *e = &si->pMapEntries[i];
            if (e->constantID != c.index || e->offset + e->size > si->dataSize) continue;
            const uint8_t *v = (const uint8_t *)si->pData + e->offset; uint32_t w = 0; memcpy(&w, v, e->size < 4 ? e->size : 4);
            if (c.type == MTLDataTypeBool) { bool b = w != 0; [cv setConstantValue:&b type:MTLDataTypeBool atIndex:c.index]; }   // a VkBool32
            else if (c.type == MTLDataTypeInt || c.type == MTLDataTypeUInt || c.type == MTLDataTypeFloat) [cv setConstantValue:&w type:c.type atIndex:c.index];
            else RED1("specialization constant of Metal type %lu not handled", (unsigned long)c.type);
        }
    NSError *err = nil;
    id<MTLFunction> f = [g_lib newFunctionWithName:fn.name constantValues:cv error:&err];
    if (!f) LOG("[vk] [RED] shader %s with its specialization constants: %s", fn.name.UTF8String, err.localizedDescription.UTF8String);
    return f;
}
static void pipe_layout_hold(VkPipeline p, VkPipelineLayout layout) {
    if (!layout) return;
    p->pc_size = layout->pc_size; p->nset = layout->nset;
    for (uint32_t i = 0; i < p->nset; i++) if ((p->set[i] = layout->set[i])) atomic_fetch_add(&p->set[i]->refs, 1);
}
static VkPipeline pipe_make(const VkGraphicsPipelineCreateInfo *ci) {
    VkPipeline p = zalloc(sizeof *p);
    pipe_layout_hold(p, ci->layout);
    @autoreleasepool {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        char keys[64] = ""; int dual = dual_source(ci->pColorBlendState), lacking = 0;   // lacking: the pipeline requests a fragment stage whose Metal function is missing from the library
        int geom_vp = 0;
        for (uint32_t s = 0; s < ci->stageCount; s++) {
            const VkPipelineShaderStageCreateInfo *st = &ci->pStages[s];
            if (st->stage == VK_SHADER_STAGE_GEOMETRY_BIT && st->module->si) geom_vp = st->module->si->geom_vp;
        }
        VkPrimitiveTopology topology = ci->pInputAssemblyState ? ci->pInputAssemblyState->topology : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        for (uint32_t s = 0; s < ci->stageCount; s++) {
            const VkPipelineShaderStageCreateInfo *st = &ci->pStages[s]; VkShaderModule m = st->module;
            if (st->stage == VK_SHADER_STAGE_VERTEX_BIT) {
                id<MTLFunction> fn = m->fn;
                if (geom_vp) {
                    fn = fn && topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && max_viewports() > 1
                        ? [g_lib newFunctionWithName:[NSString stringWithFormat:@"vk_%s_vp%d", m->key, geom_vp - 1]] : nil;
                    if (!fn) LOG("[vk] [RED] geometry viewport lowering unavailable for vertex shader %s at location %d (topology %d)", m->key, geom_vp - 1, topology);
                }
                pd.vertexFunction = spec_fn(fn, st->pSpecializationInfo); p->vs = m->si;
            }
            else if (st->stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
                if (dual && m->fn) {   // pipelines may be built on several threads
                    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; pthread_mutex_lock(&mu);
                    if (!m->tried_d) { m->tried_d = 1; m->fn_d = [g_lib newFunctionWithName:[NSString stringWithFormat:@"vk_%s_d", m->key]]; }
                    pthread_mutex_unlock(&mu);
                }
                pd.fragmentFunction = spec_fn(dual && m->fn_d ? m->fn_d : m->fn, st->pSpecializationInfo); p->fs = m->si;
                lacking |= !pd.fragmentFunction;
            }
            else if (st->stage != VK_SHADER_STAGE_GEOMETRY_BIT || !geom_vp) RED1("shader stage %#x not implemented", st->stage);
            strlcat(keys, s ? " " : "", sizeof keys); strlcat(keys, m->key, sizeof keys);
        }
        strlcpy(p->keys, keys, sizeof p->keys);
        if (!pd.vertexFunction) { LOG("[vk] [RED] pipeline has no usable vertex shader (%s)", keys); return p; }
        // A fragment stage the pipeline requests and the library has no function for refuses the pipeline: built without it, Metal makes a depth-only pipeline whose draws write no colour
        // and say nothing (Re:ZERO's 2D batches). A pipeline that requests no fragment stage at all (depth-only) is built, and so is one with a stage Metal has none of (reported above)
        if (lacking) { OLOG("pipeline %s not built: a stage is missing", keys); return p; }
        if (!argbuf_ok(p->vs) || !argbuf_ok(p->fs)) return p;
        const VkPipelineVertexInputStateCreateInfo *vi = ci->pVertexInputState;
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        uint32_t buffer_used = 0, used = 0;   // descriptor/push buffers are allocated first; vertex inputs take free slots below 31
        if (p->vs) for (int i = 0; i < p->vs->nres; i++) {
            const shres *r = &p->vs->res[i];
            if (r->slot >= 31) continue;
            if (strchr("UBTS", r->kind) || r->kind == 'p' || r->kind == 'q') buffer_used |= 1u << r->slot;
            else if (strchr("ub", r->kind)) for (uint32_t k = 0; k < r->count && r->slot + k < 31; k++) buffer_used |= 1u << (r->slot + k);
        }
        for (uint32_t i = 0; vi && i < vi->vertexAttributeDescriptionCount; i++) {
            const VkVertexInputAttributeDescription *at = &vi->pVertexAttributeDescriptions[i]; const fmt_t *f = fmt_of(at->format);
            if (!f || f->vf == MTLVertexFormatInvalid || at->location >= 31 || at->binding >= 14) { LOG("[vk] [RED] vertex attribute %u format %d not mapped", at->location, at->format); continue; }
            vd.attributes[at->location].format = f->vf; vd.attributes[at->location].offset = at->offset; used |= 1u << at->binding;
        }
        for (uint32_t i = 0; vi && i < vi->vertexBindingDescriptionCount; i++) {
            const VkVertexInputBindingDescription *b = &vi->pVertexBindingDescriptions[i];
            if (b->binding >= 14) { LOG("[vk] [RED] vertex binding %u over 13", b->binding); continue; }
            if (!(used >> b->binding & 1)) continue;
            int slot = 30; while (slot >= 0 && (buffer_used >> slot & 1)) slot--;
            if (slot < 0) { LOG("[vk] [RED] pipeline %s has no free Metal buffer slot for vertex input %u", keys, b->binding); return p; }
            buffer_used |= 1u << slot; p->vb_slot[b->binding] = (uint8_t)slot;
            MTLVertexBufferLayoutDescriptor *l = vd.layouts[slot];
            if (b->stride) { l.stride = b->stride; l.stepFunction = b->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex; l.stepRate = 1; }
            else { l.stride = 16; l.stepFunction = MTLVertexStepFunctionConstant; l.stepRate = 0; }
            p->vb_mask |= 1u << b->binding;
        }
        for (uint32_t i = 0; vi && i < vi->vertexAttributeDescriptionCount; i++) {
            const VkVertexInputAttributeDescription *at = &vi->pVertexAttributeDescriptions[i];
            if (at->location < 31 && at->binding < 14) vd.attributes[at->location].bufferIndex = p->vb_slot[at->binding];
        }
        pd.vertexDescriptor = vd;
        switch (topology) {
            case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: p->prim = MTLPrimitiveTypePoint; pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassPoint; break;
            case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: p->prim = MTLPrimitiveTypeLine; pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassLine; break;
            case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: p->prim = MTLPrimitiveTypeLineStrip; pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassLine; break;
            case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: p->prim = MTLPrimitiveTypeTriangleStrip; break;
            case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: p->prim = MTLPrimitiveTypeTriangle; break;
            case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: p->prim = MTLPrimitiveTypeTriangle; p->fan = 1; p->restart = ci->pInputAssemblyState->primitiveRestartEnable; g_fan_pipes++; break;
            default: RED1("primitive topology %d not implemented", ci->pInputAssemblyState->topology); p->prim = MTLPrimitiveTypeTriangle; break;
        }
        const VkPipelineRasterizationStateCreateInfo *rs = ci->pRasterizationState;
        p->cull = !rs || rs->cullMode == VK_CULL_MODE_NONE ? MTLCullModeNone : rs->cullMode == VK_CULL_MODE_FRONT_BIT ? MTLCullModeFront : rs->cullMode == VK_CULL_MODE_BACK_BIT ? MTLCullModeBack : MTLCullModeNone;
        if (rs && rs->cullMode == VK_CULL_MODE_FRONT_AND_BACK) RED1("culling both front and back faces not implemented");
        p->wind = rs && rs->frontFace == VK_FRONT_FACE_CLOCKWISE ? MTLWindingClockwise : MTLWindingCounterClockwise;   // framebuffer winding is the same once the shaders flip y
        p->fill = rs && rs->polygonMode == VK_POLYGON_MODE_LINE ? MTLTriangleFillModeLines : MTLTriangleFillModeFill;
        p->clip = rs && rs->depthClampEnable ? MTLDepthClipModeClamp : MTLDepthClipModeClip;
        if (rs && rs->depthBiasEnable) { p->bias = 1; p->bias_c = rs->depthBiasConstantFactor; p->bias_s = rs->depthBiasSlopeFactor; p->bias_cl = rs->depthBiasClamp; }
        if (rs && rs->rasterizerDiscardEnable) pd.rasterizationEnabled = NO;
        const VkPipelineMultisampleStateCreateInfo *ms = ci->pMultisampleState;
        pd.rasterSampleCount = ms ? umax(1, ms->rasterizationSamples) : 1; pd.alphaToCoverageEnabled = ms && ms->alphaToCoverageEnable;
        VkRenderPass rp = ci->renderPass;
        if (ci->subpass) RED1("pipeline uses subpass %u", ci->subpass);
        const VkPipelineColorBlendStateCreateInfo *cb = ci->pColorBlendState;
        if (cb && cb->logicOpEnable && cb->logicOp != VK_LOGIC_OP_COPY) p->logic = cb->logicOp;   // reported when it draws (pre_draw): Hagi creates every pipeline of its database at boot
        for (uint32_t i = 0; i < rp->ncol; i++) {
            int at = rp->col[i]; if (at < 0) continue;
            const fmt_t *f = fmt_of(rp->att[at].fmt); MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[i];
            ca.pixelFormat = f ? f->pf : MTLPixelFormatInvalid;
            if (cb && i < cb->attachmentCount) {
                const VkPipelineColorBlendAttachmentState *b = &cb->pAttachments[cb->attachmentCount > 1 ? i : 0];
                ca.writeMask = write_mask(b->colorWriteMask);
                if (b->blendEnable && f && f->kind == K_COLOR) {
                    ca.blendingEnabled = YES;
                    ca.sourceRGBBlendFactor = blend_f(b->srcColorBlendFactor); ca.destinationRGBBlendFactor = blend_f(b->dstColorBlendFactor); ca.rgbBlendOperation = (MTLBlendOperation)b->colorBlendOp;
                    ca.sourceAlphaBlendFactor = blend_f(b->srcAlphaBlendFactor); ca.destinationAlphaBlendFactor = blend_f(b->dstAlphaBlendFactor); ca.alphaBlendOperation = (MTLBlendOperation)b->alphaBlendOp;
                }
            }
        }
        if (cb) memcpy(p->blend, cb->blendConstants, sizeof p->blend);
        const fmt_t *df = rp->depth >= 0 ? fmt_of(rp->att[rp->depth].fmt) : NULL;
        if (fmt_has_depth(df)) pd.depthAttachmentPixelFormat = df->pf;
        if (fmt_has_stencil(df)) pd.stencilAttachmentPixelFormat = df->pf;
        const VkPipelineDepthStencilStateCreateInfo *ds = ci->pDepthStencilState;
        if (df && ds) {
            MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
            if (fmt_has_depth(df) && ds->depthTestEnable) { dd.depthCompareFunction = (MTLCompareFunction)ds->depthCompareOp; dd.depthWriteEnabled = ds->depthWriteEnable != 0; }
            if (fmt_has_stencil(df) && ds->stencilTestEnable) { dd.frontFaceStencil = stencil_d(&ds->front); dd.backFaceStencil = stencil_d(&ds->back); p->sref_f = ds->front.reference; p->sref_b = ds->back.reference;
                                                                p->stencil = 1; p->sf = ds->front; p->sb = ds->back; p->dcmp = dd.depthCompareFunction; p->dwrite = dd.depthWriteEnabled; }
            // Depth bounds test (nvn_dbnd.h): static bounds are resolved here; dynamic bounds (VK_DYNAMIC_STATE_DEPTH_BOUNDS) are resolved in pre_draw from vkCmdSetDepthBounds.
            // Without a depth format, or with rasterization off, it has nothing to test
            if (ds->depthBoundsTestEnable && fmt_has_depth(df) && !(rs && rs->rasterizerDiscardEnable)) {
                p->db_en = 1; p->dbk = dbnd_resolve(1, ds->minDepthBounds, ds->maxDepthBounds, p->db);
            }
            p->dss = [g_dev newDepthStencilStateWithDescriptor:dd];
        } else p->dss = g_dss_off;
        const VkPipelineDynamicStateCreateInfo *dy = ci->pDynamicState;
        int dyn_vp = 0, dyn_sc = 0;
        for (uint32_t i = 0; dy && i < dy->dynamicStateCount; i++) {
            VkDynamicState s = dy->pDynamicStates[i];
            if (s == VK_DYNAMIC_STATE_VIEWPORT) dyn_vp = 1; else if (s == VK_DYNAMIC_STATE_SCISSOR) dyn_sc = 1;
            else if (s == VK_DYNAMIC_STATE_STENCIL_REFERENCE) p->dyn_sref = 1;
            else if (s == VK_DYNAMIC_STATE_LINE_WIDTH) continue;   // Metal draws lines one pixel wide whatever the width (Super Mario 64 sets 1)
            else if (s == VK_DYNAMIC_STATE_DEPTH_BIAS) p->dyn_bias = 1; else if (s == VK_DYNAMIC_STATE_BLEND_CONSTANTS) p->dyn_blend = 1;
            else if (s == VK_DYNAMIC_STATE_DEPTH_BOUNDS) { p->dyn_db = 1; p->dbk = DBND_OFF; p->db[0] = 0; p->db[1] = 1; }   // Vulkan ignores static min/maxDepthBounds when dynamic
            else if (s == VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK || s == VK_DYNAMIC_STATE_STENCIL_WRITE_MASK) p->dyn_smask |= s == VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK ? 1 : 2;
            else RED1("dynamic state %d not implemented (the imports have no matching set function)", s);
        }
        const VkPipelineViewportStateCreateInfo *vs = ci->pViewportState;
        p->nviewport = vs && vs->viewportCount ? vs->viewportCount : 1;
        if (p->nviewport > max_viewports() || (vs && vs->scissorCount && vs->scissorCount != p->nviewport)) {
            LOG("[vk] [RED] pipeline %s has unsupported viewport/scissor counts %u/%u", keys, p->nviewport, vs ? vs->scissorCount : 0);
            return p;
        }
        if (!dyn_vp && vs && vs->pViewports && vs->viewportCount) {
            p->vp = p->nviewport == 1 ? &p->vp0 : zalloc(p->nviewport * sizeof *p->vp);
            for (uint32_t i = 0; i < p->nviewport; i++) p->vp[i] = vp_of(&vs->pViewports[i]);
        }
        if (!dyn_sc && vs && vs->pScissors && vs->scissorCount) {
            p->sc = p->nviewport == 1 ? &p->sc0 : zalloc(p->nviewport * sizeof *p->sc);
            memcpy(p->sc, vs->pScissors, p->nviewport * sizeof *p->sc);
        }
        NSError *err = nil;
        p->pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!p->pso) { static int n; if (n++ < 20) LOG("[vk] [RED] cannot build the pipeline (%s): %s", keys, err.localizedDescription.UTF8String); }
        else if (!ab_init(p, ST_VERT, pd.vertexFunction, p->vs) || !ab_init(p, ST_FRAG, pd.fragmentFunction, p->fs)) {
            p->pso = nil; ab_free(p); LOG("[vk] [RED] graphics pipeline %s refused: argument-buffer initialization failed", keys);
        } else {
            g_n_pso++;
            if (geom_vp) OLOG("geometry viewport pipeline %s uses location %d and %u viewport(s)", keys, geom_vp - 1, p->nviewport);
        }
    }
    return p;
}
VKB(vkCreateGraphicsPipelines, VkResult, VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    (void)d; (void)c; (void)a; dev_init();
    for (uint32_t i = 0; i < n; i++) out[i] = pipe_make(&ci[i]);   // a pipeline Metal refuses is still handed out: its draws are skipped
    return VK_SUCCESS;
}
// A compute pipeline uses the shader's full local size; reducing it would change gl_LocalInvocationID.
static VkPipeline cpipe_make(const VkComputePipelineCreateInfo *ci) {
    VkPipeline p = zalloc(sizeof *p); pipe_layout_hold(p, ci->layout);
    const VkPipelineShaderStageCreateInfo *st = &ci->stage; VkShaderModule m = st->module;
    strlcpy(p->keys, m->key, sizeof p->keys);
    if (st->stage != VK_SHADER_STAGE_COMPUTE_BIT || !m->fn || !m->si || !m->si->comp) {
        LOG("[vk] [RED] compute pipeline: shader %s is not a compute shader of the library", m->key); return p;
    }
    p->cs = m->si; p->wg = MTLSizeMake(m->si->wg[0], m->si->wg[1], m->si->wg[2]);
    if (!argbuf_ok(p->cs)) {
        LOG("[vk] [RED] compute pipeline %s refused: argument buffers require Metal Tier2 (device %s)", m->key,
            g_ab_tier == MTLArgumentBuffersTier2 ? "Tier2" : "Tier1"); return p;
    }
    @autoreleasepool {
        id<MTLFunction> fn = spec_fn(m->fn, st->pSpecializationInfo); NSError *err = nil;
        if (fn) p->cpso = [g_dev newComputePipelineStateWithFunction:fn error:&err];
        if (!p->cpso) { LOG("[vk] [RED] cannot build the compute pipeline (%s): %s", m->key, err ? err.localizedDescription.UTF8String : "no function"); return p; }
        NSUInteger n = p->wg.width * p->wg.height * p->wg.depth;
        if (n > p->cpso.maxTotalThreadsPerThreadgroup) {
            LOG("[vk] [RED] compute pipeline %s refused: local size %lux%lux%lu exceeds the %lu threads per threadgroup Metal allows", m->key,
                (unsigned long)p->wg.width, (unsigned long)p->wg.height, (unsigned long)p->wg.depth, (unsigned long)p->cpso.maxTotalThreadsPerThreadgroup);
            p->cpso = nil; return p;
        }
        if (!ab_init(p, ST_COMP, fn, p->cs)) { p->cpso = nil; ab_free(p); LOG("[vk] [RED] compute pipeline %s refused: argument-buffer initialization failed", m->key); return p; }
        g_n_pso++; OLOG("compute pipeline %s, local size %lux%lux%lu", m->key, (unsigned long)p->wg.width, (unsigned long)p->wg.height, (unsigned long)p->wg.depth);
    }
    return p;
}
VKB(vkCreateComputePipelines, VkResult, VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    (void)d; (void)c; (void)a; dev_init();
    for (uint32_t i = 0; i < n; i++) out[i] = cpipe_make(&ci[i]);   // as for graphics: a pipeline Metal refuses is handed out, and its dispatches are skipped
    return VK_SUCCESS;
}
VKB(vkDestroyPipeline, void, VkDevice d, VkPipeline p, const VkAllocationCallbacks *a) {
    (void)d; (void)a;
    if (!p) return;
    p->pso = nil; p->dss = nil; p->cpso = nil; p->dss_dyn = nil; ab_free(p);
    for (uint32_t i = 0; i < p->nset; i++) dsl_put(p->set[i]);
    if (p->nviewport > 1) { free(p->vp); free(p->sc); }
    free(p);
}

// ---------- Command buffers: recording ----------
enum { C_BRP = 1, C_ERP, C_PIPE, C_SETS, C_VB, C_IB, C_VP, C_SC, C_DRAWI, C_COPYB, C_COPYB2I, C_DRAW, C_PUSH, C_COPYI, C_COPYI2B, C_QRESET, C_QTS, C_QBEGIN, C_QEND, C_CLEARI, C_STENCIL,
       C_DISPATCH, C_DYN, C_DRAWIND, C_FILL, C_UPDATE, C_RESOLVE, C_BLIT, C_EV, C_EXEC, C_QCOPY, C_CLEARA, C_BARRIER };
typedef struct { uint32_t op, size; } chdr;
typedef struct { VkRenderPass rp; VkFramebuffer fb; VkRect2D area; uint32_t nclear; VkClearValue clear[]; } c_brp;
typedef struct { VkPipeline p; int compute; } c_pipe;
typedef struct { uint32_t first, n, ndyn; int compute; VkDescriptorSet s[8]; uint32_t dyn[]; } c_sets;
typedef struct { uint32_t first, n; VkBuffer b[14]; VkDeviceSize o[14]; } c_vb;
typedef struct { VkBuffer b; VkDeviceSize o; VkIndexType t; } c_ib;
typedef struct { uint32_t first, n; VkViewport v[]; } c_vp;
typedef struct { uint32_t first, n; VkRect2D r[]; } c_sc;
typedef struct { uint32_t count, inst, first; int32_t voff; uint32_t first_inst; } c_drawi;
typedef struct { VkBuffer src, dst; uint32_t n; VkBufferCopy r[]; } c_copyb;
typedef struct { VkBuffer src; VkImage dst; uint32_t n; VkBufferImageCopy r[]; } c_copyb2i;
typedef struct { uint32_t count, inst, first, first_inst; } c_draw;
typedef struct { uint32_t off, size; VkShaderStageFlags stages; uint8_t data[]; } c_push;
typedef struct { VkImage src, dst; uint32_t n; VkImageCopy r[]; } c_copyi;
typedef struct { VkImage src; VkBuffer dst; uint32_t n; VkBufferImageCopy r[]; } c_copyi2b;
typedef struct { VkQueryPool q; uint32_t first, n; } c_query;
typedef struct { int what; VkStencilFaceFlags faces; uint32_t v; } c_stencil;   // what: 0 compare mask, 1 write mask, 2 reference
typedef struct { VkImage im; int ds; VkClearValue v; uint32_t n; VkImageSubresourceRange r[]; } c_cleari;   // vkCmdClearColorImage / vkCmdClearDepthStencilImage
typedef struct { int what; float v[4]; } c_dyn;   // what: 0 blend constants, 1 depth bias (constant factor, clamp, slope factor)
typedef struct { VkBuffer b; VkDeviceSize o; uint32_t count, stride; int indexed; } c_drawind;   // Vk(DrawIndexed)IndirectCommand has the layout of Metal's indirect arguments
typedef struct { VkBuffer dst; VkDeviceSize off, size; uint32_t data; } c_fill;
typedef struct { VkBuffer dst; VkDeviceSize off; uint32_t size; uint8_t data[]; } c_update;
typedef struct { uint32_t x, y, z; VkBuffer b; VkDeviceSize o; } c_dispatch;   // b: VkDispatchIndirectCommand buffer, NULL for direct dispatch
typedef struct { VkImage src, dst; uint32_t n; VkImageResolve r[]; } c_resolve;
typedef struct { VkImage src, dst; VkFilter filter; uint32_t n; VkImageBlit r[]; } c_blit;
typedef struct { VkEvent e; int set; } c_ev;
typedef struct { uint32_t n; VkCommandBuffer cb[]; } c_exec;   // vkCmdExecuteCommands: secondary command buffers, replayed in place
typedef struct { VkQueryPool q; uint32_t first, n; VkBuffer dst; VkDeviceSize off, stride; VkQueryResultFlags fl; } c_qcopy;
typedef struct { uint32_t n, rects; VkClearAttachment attachments[]; } c_cleara;
static void *rec(VkCommandBuffer cb, uint32_t op, size_t sz) {
    size_t need = (sizeof(chdr) + sz + 7) & ~(size_t)7;
    if (cb->n + need > cb->cap) {
        size_t c = cb->cap ? cb->cap * 2 : 4096; while (c < cb->n + need) c *= 2;
        uint8_t *q = realloc(cb->buf, c); if (!q) die("[vk] out of command buffer memory %zu", c); cb->buf = q; cb->cap = c;
    }
    chdr *h = (chdr *)(cb->buf + cb->n); h->op = op; h->size = (uint32_t)need; cb->n += need;
    return h + 1;
}
VK(vkCreateCommandPool, VkResult, VkDevice d, const VkCommandPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkCommandPool *out) {
    (void)d; (void)ci; (void)a; *out = zalloc(sizeof **out); return VK_SUCCESS;
}
static void cb_free(VkCommandBuffer cb) {
    VkCommandPool p = cb->pool;
    if (cb->prev) cb->prev->next = cb->next; else p->cbs = cb->next;
    if (cb->next) cb->next->prev = cb->prev;
    free(cb->buf); free(cb);
}
VK(vkDestroyCommandPool, void, VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a) { (void)d; (void)a; if (p) { while (p->cbs) cb_free(p->cbs); free(p); } }
VK(vkResetCommandPool, VkResult, VkDevice d, VkCommandPool p, VkCommandPoolResetFlags f) { (void)d; (void)f; for (VkCommandBuffer c = p->cbs; c; c = c->next) c->n = 0; return VK_SUCCESS; }
VK(vkAllocateCommandBuffers, VkResult, VkDevice d, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *out) {
    (void)d;
    (void)ai->level;   // secondary command buffers are recorded alike and replayed where vkCmdExecuteCommands names them
    for (uint32_t i = 0; i < ai->commandBufferCount; i++) {
        VkCommandBuffer cb = zalloc(sizeof *cb); cb->pool = ai->commandPool;
        cb->next = cb->pool->cbs; if (cb->next) cb->next->prev = cb; cb->pool->cbs = cb;
        out[i] = cb;
    }
    return VK_SUCCESS;
}
VK(vkFreeCommandBuffers, void, VkDevice d, VkCommandPool p, uint32_t n, const VkCommandBuffer *cbs) { (void)d; (void)p; for (uint32_t i = 0; i < n; i++) if (cbs[i]) cb_free(cbs[i]); }
VK(vkBeginCommandBuffer, VkResult, VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi) { (void)bi; cb->n = 0; return VK_SUCCESS; }
VK(vkEndCommandBuffer, VkResult, VkCommandBuffer cb) { (void)cb; return VK_SUCCESS; }
VK(vkResetCommandBuffer, VkResult, VkCommandBuffer cb, VkCommandBufferResetFlags f) { (void)f; cb->n = 0; return VK_SUCCESS; }
VK(vkCmdBeginRenderPass, void, VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents sc) {
    (void)sc; uint32_t nc = umin(bi->clearValueCount, 16);
    c_brp *c = rec(cb, C_BRP, sizeof *c + nc * sizeof(VkClearValue));
    c->rp = bi->renderPass; c->fb = bi->framebuffer; c->area = bi->renderArea; c->nclear = nc;
    if (nc) memcpy(c->clear, bi->pClearValues, nc * sizeof(VkClearValue));
}
VK(vkCmdEndRenderPass, void, VkCommandBuffer cb) { rec(cb, C_ERP, 0); }
static int bind_point(VkPipelineBindPoint bp) {   // 1 compute, 0 graphics, -1 other
    if (bp == VK_PIPELINE_BIND_POINT_COMPUTE) return 1;
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) return 0;
    RED1("pipeline bind point %d not implemented", bp); return -1;
}
VK(vkCmdBindPipeline, void, VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    int k = bind_point(bp); if (k < 0) return;
    c_pipe *c = rec(cb, C_PIPE, sizeof *c); c->p = p; c->compute = k;
}
VK(vkCmdBindDescriptorSets, void, VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet *s, uint32_t ndyn, const uint32_t *dyn) {
    (void)l; int k = bind_point(bp); if (k < 0) return;
    c_sets *c = rec(cb, C_SETS, sizeof *c + ndyn * sizeof(uint32_t));
    c->first = first; c->n = umin(n, 8); c->ndyn = ndyn; c->compute = k; memcpy(c->s, s, c->n * sizeof *s); if (ndyn) memcpy(c->dyn, dyn, ndyn * sizeof *dyn);
}
VK(vkCmdBindVertexBuffers, void, VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) {
    c_vb *c = rec(cb, C_VB, sizeof *c); c->first = first; c->n = first < 14 ? umin(n, 14 - first) : 0;
    memcpy(c->b, b, c->n * sizeof *b); memcpy(c->o, o, c->n * sizeof *o);
}
VK(vkCmdBindIndexBuffer, void, VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t) { c_ib *c = rec(cb, C_IB, sizeof *c); c->b = b; c->o = o; c->t = t; }
VK(vkCmdSetViewport, void, VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v) {
    if (!n || first >= MAX_VIEWPORTS || n > MAX_VIEWPORTS - first) return;
    c_vp *c = rec(cb, C_VP, sizeof *c + n * sizeof *v);
    c->first = first; c->n = n; memcpy(c->v, v, n * sizeof *v);
}
VK(vkCmdSetScissor, void, VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *r) {
    if (!n || first >= MAX_VIEWPORTS || n > MAX_VIEWPORTS - first) return;
    c_sc *c = rec(cb, C_SC, sizeof *c + n * sizeof *r);
    c->first = first; c->n = n; memcpy(c->r, r, n * sizeof *r);
}
VK(vkCmdDrawIndexed, void, VkCommandBuffer cb, uint32_t count, uint32_t inst, uint32_t first, int32_t voff, uint32_t first_inst) {
    *(c_drawi *)rec(cb, C_DRAWI, sizeof(c_drawi)) = (c_drawi){count, inst, first, voff, first_inst};
}
VK(vkCmdCopyBuffer, void, VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy *r) {
    c_copyb *c = rec(cb, C_COPYB, sizeof *c + n * sizeof *r); c->src = src; c->dst = dst; c->n = n; memcpy(c->r, r, n * sizeof *r);
}
VK(vkCmdCopyBufferToImage, void, VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout lay, uint32_t n, const VkBufferImageCopy *r) {
    (void)lay; c_copyb2i *c = rec(cb, C_COPYB2I, sizeof *c + n * sizeof *r); c->src = src; c->dst = dst; c->n = n; memcpy(c->r, r, n * sizeof *r);
}
VK(vkCmdDraw, void, VkCommandBuffer cb, uint32_t count, uint32_t inst, uint32_t first, uint32_t first_inst) {
    *(c_draw *)rec(cb, C_DRAW, sizeof(c_draw)) = (c_draw){count, inst, first, first_inst};
}
VK(vkCmdDispatch, void, VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) { *(c_dispatch *)rec(cb, C_DISPATCH, sizeof(c_dispatch)) = (c_dispatch){x, y, z, NULL, 0}; }
VK(vkCmdDispatchIndirect, void, VkCommandBuffer cb, VkBuffer b, VkDeviceSize o) { *(c_dispatch *)rec(cb, C_DISPATCH, sizeof(c_dispatch)) = (c_dispatch){0, 0, 0, b, o}; }
// Metal has no base-workgroup coordinate: report and skip a non-zero base rather than dispatching with wrong global ids.
VK(vkCmdDispatchBase, void, VkCommandBuffer cb, uint32_t bx, uint32_t by, uint32_t bz, uint32_t x, uint32_t y, uint32_t z) {
    if (bx || by || bz) { RED1("dispatch with a base workgroup (%u, %u, %u) not implemented", bx, by, bz); return; }
    h_vkCmdDispatch(cb, x, y, z);
}
VK(vkCmdSetBlendConstants, void, VkCommandBuffer cb, const float c[4]) { c_dyn *x = rec(cb, C_DYN, sizeof *x); x->what = 0; memcpy(x->v, c, 4 * sizeof *c); }
VK(vkCmdSetDepthBias, void, VkCommandBuffer cb, float c, float clamp, float slope) { c_dyn *x = rec(cb, C_DYN, sizeof *x); x->what = 1; x->v[0] = c; x->v[1] = clamp; x->v[2] = slope; x->v[3] = 0; }
VK(vkCmdSetLineWidth, void, VkCommandBuffer cb, float w) { (void)cb; (void)w; }   // Metal draws lines one pixel wide (wideLines is not advertised: the width is 1)
VK(vkCmdSetDepthBounds, void, VkCommandBuffer cb, float lo, float hi) { c_dyn *x = rec(cb, C_DYN, sizeof *x); x->what = 2; x->v[0] = lo; x->v[1] = hi; x->v[2] = x->v[3] = 0; }
VK(vkCmdSetDeviceMask, void, VkCommandBuffer cb, uint32_t m) { (void)cb; if (m != 1) RED1("device mask %#x: there is one device", m); }   // device groups of one device
static void rec_drawind(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t count, uint32_t stride, int indexed) {
    *(c_drawind *)rec(cb, C_DRAWIND, sizeof(c_drawind)) = (c_drawind){b, o, count, stride, indexed};
}
VK(vkCmdDrawIndirect, void, VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t count, uint32_t stride) { rec_drawind(cb, b, o, count, stride, 0); }
VK(vkCmdDrawIndexedIndirect, void, VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t count, uint32_t stride) { rec_drawind(cb, b, o, count, stride, 1); }
VK(vkCmdFillBuffer, void, VkCommandBuffer cb, VkBuffer dst, VkDeviceSize off, VkDeviceSize size, uint32_t data) { *(c_fill *)rec(cb, C_FILL, sizeof(c_fill)) = (c_fill){dst, off, size, data}; }
VK(vkCmdUpdateBuffer, void, VkCommandBuffer cb, VkBuffer dst, VkDeviceSize off, VkDeviceSize size, const void *data) {   // at most 65536 bytes, by the spec
    if (!(dst->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) || !size || size > 65536 || ((off | size) & 3) || off > dst->size || size > dst->size - off) {
        RED1("vkCmdUpdateBuffer destination usage, alignment or range"); return;
    }
    c_update *c = rec(cb, C_UPDATE, sizeof *c + size); c->dst = dst; c->off = off; c->size = (uint32_t)size; memcpy(c->data, data, size);
}
static int transfer_images(VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl) {
    return (src->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) && (dst->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
        (sl == VK_IMAGE_LAYOUT_GENERAL || sl == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL || sl == VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR) &&
        (dl == VK_IMAGE_LAYOUT_GENERAL || dl == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL || dl == VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR);
}
VK(vkCmdBlitImage, void, VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f) {
    if (!transfer_images(src, sl, dst, dl) || src->samples != 1 || dst->samples != 1 || (f != VK_FILTER_NEAREST && f != VK_FILTER_LINEAR)) {
        RED1("vkCmdBlitImage usage, layout, sample count or filter"); return;
    }
    c_blit *c = rec(cb, C_BLIT, sizeof *c + n * sizeof *r);
    c->src = src; c->dst = dst; c->filter = f; c->n = n; memcpy(c->r, r, n * sizeof *r);
}
VK(vkCmdResolveImage, void, VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageResolve *r) {
    if (!transfer_images(src, sl, dst, dl) || src->fmt != dst->fmt || src->samples <= 1 || dst->samples != 1) {
        RED1("vkCmdResolveImage usage, layout, format or sample count"); return;
    }
    c_resolve *c = rec(cb, C_RESOLVE, sizeof *c + n * sizeof *r);
    c->src = src; c->dst = dst; c->n = n; memcpy(c->r, r, n * sizeof *r);
}
VK(vkCmdClearAttachments, void, VkCommandBuffer cb, uint32_t n, const VkClearAttachment *a, uint32_t nr, const VkClearRect *r) {
    c_cleara *c = rec(cb, C_CLEARA, sizeof *c + n * sizeof *a + nr * sizeof *r);
    c->n = n; c->rects = nr; memcpy(c->attachments, a, n * sizeof *a); memcpy(c->attachments + n, r, nr * sizeof *r);
}
VK(vkCmdNextSubpass, void, VkCommandBuffer cb, VkSubpassContents sc) { (void)cb; (void)sc; RED1("render passes with several subpasses not implemented: the next subpass draws into the first one's attachments"); }
// Events: one serial queue runs every command in order, so a wait on an event set earlier on the queue is already met; the event's state is for the host to read
VK(vkCreateEvent, VkResult, VkDevice d, const VkEventCreateInfo *ci, const VkAllocationCallbacks *a, VkEvent *out) {
    (void)d; (void)ci; (void)a; VkEvent e = zalloc(sizeof *e); atomic_store(&e->val, FENCE_NONE); *out = e; return VK_SUCCESS;
}
VK(vkDestroyEvent, void, VkDevice d, VkEvent e, const VkAllocationCallbacks *a) { (void)d; (void)a; free(e); }
VK(vkGetEventStatus, VkResult, VkDevice d, VkEvent e) { (void)d; uint64_t v = atomic_load(&e->val); return v != FENCE_NONE && atomic_load(&g_done) >= v ? VK_EVENT_SET : VK_EVENT_RESET; }
VK(vkSetEvent, VkResult, VkDevice d, VkEvent e) { (void)d; atomic_store(&e->val, 0); return VK_SUCCESS; }
VK(vkResetEvent, VkResult, VkDevice d, VkEvent e) { (void)d; atomic_store(&e->val, FENCE_NONE); return VK_SUCCESS; }
VK(vkCmdSetEvent, void, VkCommandBuffer cb, VkEvent e, VkPipelineStageFlags st) { (void)st; *(c_ev *)rec(cb, C_EV, sizeof(c_ev)) = (c_ev){e, 1}; }
VK(vkCmdResetEvent, void, VkCommandBuffer cb, VkEvent e, VkPipelineStageFlags st) { (void)st; *(c_ev *)rec(cb, C_EV, sizeof(c_ev)) = (c_ev){e, 0}; }
VK(vkCmdWaitEvents, void, VkCommandBuffer cb, uint32_t n, const VkEvent *e, VkPipelineStageFlags s, VkPipelineStageFlags d, uint32_t nm, const VkMemoryBarrier *m,
   uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *i) {
    (void)cb; (void)n; (void)e; (void)s; (void)d; (void)nm; (void)m; (void)nb; (void)b; (void)ni; (void)i;
}
VK(vkCmdExecuteCommands, void, VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *cbs) { c_exec *c = rec(cb, C_EXEC, sizeof *c + n * sizeof *cbs); c->n = n; memcpy(c->cb, cbs, n * sizeof *cbs); }
// Query results copied into a buffer are written when the submit completes (the pools' results are the CPU's): the CPU sees them after the submit's fence, a later GPU command
// of the same submit does not
VK(vkCmdCopyQueryPoolResults, void, VkCommandBuffer cb, VkQueryPool q, uint32_t first, uint32_t n, VkBuffer dst, VkDeviceSize off, VkDeviceSize stride, VkQueryResultFlags fl) {
    if (!n) return;
    uint32_t word = fl & VK_QUERY_RESULT_64_BIT ? 8 : 4;
    uint32_t row = word * (fl & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT ? 2 : 1);
    if (first > q->n || n > q->n - first || !(dst->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) || (off % word) || off > dst->size || row > dst->size - off ||
        (n > 1 && (stride < row || (stride % word) || stride > (dst->size - off - row) / (n - 1)))) {
        RED1("query copy range, destination usage or alignment"); return;
    }
    *(c_qcopy *)rec(cb, C_QCOPY, sizeof(c_qcopy)) = (c_qcopy){q, first, n, dst, off, stride, fl};
}
VK(vkCmdPushConstants, void, VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t size, const void *v) {
    (void)l;
    if (off >= 256 || size > 256 - off) { RED1("push constant out of range: offset %u length %u", off, size); return; }
    c_push *c = rec(cb, C_PUSH, sizeof *c + size); c->off = off; c->size = size; c->stages = st; memcpy(c->data, v, size);
}
VK(vkCmdCopyImage, void, VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageCopy *r) {
    (void)sl; (void)dl; c_copyi *c = rec(cb, C_COPYI, sizeof *c + n * sizeof *r); c->src = src; c->dst = dst; c->n = n; memcpy(c->r, r, n * sizeof *r);
}
static void rec_stencil(VkCommandBuffer cb, int what, VkStencilFaceFlags faces, uint32_t v) { *(c_stencil *)rec(cb, C_STENCIL, sizeof(c_stencil)) = (c_stencil){what, faces, v}; }
VK(vkCmdSetStencilCompareMask, void, VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v) { rec_stencil(cb, 0, f, v); }
VK(vkCmdSetStencilWriteMask, void, VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v) { rec_stencil(cb, 1, f, v); }
VK(vkCmdSetStencilReference, void, VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v) { rec_stencil(cb, 2, f, v); }
static void rec_clear(VkCommandBuffer cb, VkImage im, int ds, const VkClearValue *v, uint32_t n, const VkImageSubresourceRange *r) {
    c_cleari *c = rec(cb, C_CLEARI, sizeof *c + n * sizeof *r); c->im = im; c->ds = ds; c->v = *v; c->n = n; memcpy(c->r, r, n * sizeof *r);
}
VK(vkCmdClearColorImage, void, VkCommandBuffer cb, VkImage im, VkImageLayout lay, const VkClearColorValue *col, uint32_t n, const VkImageSubresourceRange *r) {
    (void)lay; VkClearValue v; v.color = *col; rec_clear(cb, im, 0, &v, n, r);
}
VK(vkCmdClearDepthStencilImage, void, VkCommandBuffer cb, VkImage im, VkImageLayout lay, const VkClearDepthStencilValue *dsv, uint32_t n, const VkImageSubresourceRange *r) {
    (void)lay; VkClearValue v; v.depthStencil = *dsv; rec_clear(cb, im, 1, &v, n, r);
}
VK(vkCmdCopyImageToBuffer, void, VkCommandBuffer cb, VkImage src, VkImageLayout lay, VkBuffer dst, uint32_t n, const VkBufferImageCopy *r) {
    (void)lay; c_copyi2b *c = rec(cb, C_COPYI2B, sizeof *c + n * sizeof *r); c->src = src; c->dst = dst; c->n = n; memcpy(c->r, r, n * sizeof *r);
}
VK(vkCmdResetQueryPool, void, VkCommandBuffer cb, VkQueryPool q, uint32_t first, uint32_t n) { *(c_query *)rec(cb, C_QRESET, sizeof(c_query)) = (c_query){q, first, n}; }
VK(vkCmdWriteTimestamp, void, VkCommandBuffer cb, VkPipelineStageFlagBits st, VkQueryPool q, uint32_t i) { (void)st; *(c_query *)rec(cb, C_QTS, sizeof(c_query)) = (c_query){q, i, 1}; }
VK(vkCmdBeginQuery, void, VkCommandBuffer cb, VkQueryPool q, uint32_t i, VkQueryControlFlags f) { (void)f; *(c_query *)rec(cb, C_QBEGIN, sizeof(c_query)) = (c_query){q, i, 1}; }
VK(vkCmdEndQuery, void, VkCommandBuffer cb, VkQueryPool q, uint32_t i) { *(c_query *)rec(cb, C_QEND, sizeof(c_query)) = (c_query){q, i, 1}; }
// Barriers order nothing here: one serial queue, and Metal tracks hazards between the passes and blits it encodes
VK(vkCmdPipelineBarrier, void, VkCommandBuffer cb, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f, uint32_t nm, const VkMemoryBarrier *m,
   uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *i) {
    (void)cb; (void)s; (void)d; (void)f; (void)nm; (void)m; (void)nb; (void)b; (void)ni; (void)i;
}

VK(vkCmdPipelineBarrier2, void, VkCommandBuffer cb, const VkDependencyInfo *i) {
    // The layer uses one queue and tracked Metal resources. Close the encoder at the dependency so the next one sees the previous writes.
    if (i->memoryBarrierCount || i->bufferMemoryBarrierCount || i->imageMemoryBarrierCount) rec(cb, C_BARRIER, 0);
}

// ---------- Command buffers: replay at submit ----------
// Each render encoder counting an occlusion query gets a slot in the submit's visibility buffer.
// Apply the writes in replay order on Metal completion, including starts and timestamps.
enum { QUERY_START = -1, QUERY_TIMESTAMP = -2 };
typedef struct { VkQueryPool q; uint32_t idx; int64_t slot; } query_write;
typedef struct { uint64_t v; id<MTLBuffer> vis; uint32_t nvis, cap, nwrites, write_cap; query_write *writes; } subq_t;
typedef struct { uint64_t offset; uint32_t count, isz, restart, inst; int32_t base; uint32_t first_inst; } fan_p;   // vk_fan.metal's Fan
typedef struct { const void *cmd; uint64_t at; VkBuffer ib; fan_p p; } fan_job;   // an indexed fan converted ahead (fan_prepare): its arguments at s->fan_out + at, its indices 32 bytes on
typedef struct {
    id<MTLCommandBuffer> cb; id<MTLRenderCommandEncoder> re; id<MTLBlitCommandEncoder> be; id<MTLComputeCommandEncoder> ce;
    const chdr *cmd; const uint8_t *cmd_end;   // the command being replayed and the end of its command buffer (fan_prepare scans ahead)
    id<MTLBuffer> fan_out; fan_job *fj; uint32_t nfj, fjcap, fji; const void *fan_from, *fan_to;   // the indexed fans of the current stretch of the pass, and the commands its scan covered
    VkPipeline pipe, applied, cpipe; uint32_t rt_w, rt_h, rt_layers;   // cpipe: the compute pipeline bound
    struct { VkDescriptorSet s; uint32_t dyn[16]; } set[8], cset[8];   // graphics and compute bind points have separate sets
    VkBuffer vb[14]; VkDeviceSize vbo[14];
    VkBuffer ib; VkDeviceSize ibo; MTLIndexType it; uint32_t isz;
    VkViewport vp[MAX_VIEWPORTS]; VkRect2D sc[MAX_VIEWPORTS]; uint32_t have_vp, have_sc; int vp_dirty, sc_dirty;
    uint8_t pc[3][256];   // push writes can address the same bytes independently by shader stage
    subq_t *sq;
    // The current render pass: its descriptor (attachments created with deferred store actions) and the store actions it ends with; cut = its encoder was ended
    // for a copy or dispatch in the middle of the pass and is resumed (loading the attachments) at the next draw
    MTLRenderPassDescriptor *rpd; const fmt_t *rt_color[8]; MTLStoreAction st_col[8], st_d, st_s; int cut;
    VkQueryPool aq; uint32_t aq_first;   // the occlusion query running (occ_start)
    uint32_t sdyn[3][2]; int sdyn_set[3], s_dirty;   // dynamic stencil compare mask / write mask / reference, front and back (C_STENCIL)
    float blend[4], bias[3], db[2], db_cur[2]; int have_blend, have_bias, have_db, db_live, d_dirty;   // dynamic blend constants, depth bias and depth bounds (C_DYN); db_live/db_cur: encoder's active bounds
    uint64_t enc_id;   // the render or compute encoder's number (g_enc_id), for argument-buffer residency
} rs_t;
static uint64_t g_enc_id;   // under g_qmu: numbers the render and compute encoders replay creates (rs_t.enc_id)
static void end_blit(rs_t *s) { if (s->be) { [s->be endEncoding]; s->be = nil; } }
static void fan_prepare(rs_t *s, const chdr *h);
static void split_submit(rs_t *s);
static void wait_done(uint64_t v);
static void end_comp(rs_t *s) { if (s->ce) { [s->ce endEncoding]; s->ce = nil; } }
static MTLStoreAction keep(MTLStoreAction a) { return a == MTLStoreActionMultisampleResolve || a == MTLStoreActionStoreAndMultisampleResolve ? MTLStoreActionStoreAndMultisampleResolve : MTLStoreActionStore; }
static void end_rp(rs_t *s, int final) {   // final: the pass's own store actions; otherwise everything is stored, to be loaded again when the pass resumes
    if (!s->re) return;
    MTLRenderPassDescriptor *d = s->rpd;
    for (int i = 0; i < 8; i++) if (d.colorAttachments[i].texture) [s->re setColorStoreAction:final ? s->st_col[i] : keep(s->st_col[i]) atIndex:i];
    if (d.depthAttachment.texture) [s->re setDepthStoreAction:final ? s->st_d : MTLStoreActionStore];
    if (d.stencilAttachment.texture) [s->re setStencilStoreAction:final ? s->st_s : MTLStoreActionStore];
    [s->re endEncoding]; s->re = nil;
}
// A copy inside a render pass, which NVIDIA's driver runs: Super Mario Galaxy copies from its EFB in the middle of a pass (vkCmdCopyImage). The encoder ends here and
// the pass resumes at its next draw (resume_rp); without that, the rest of the pass's draws were dropped
static id<MTLBlitCommandEncoder> blit(rs_t *s, uint32_t op) {
    if (s->re) { static int n; if (n++ < 3) OLOG("copy inside a render pass (command %u): the pass is split and the next draw continues it", op); end_rp(s, 0); s->cut = 1; }
    end_comp(s);
    if (!s->be) s->be = [s->cb blitCommandEncoder];
    return s->be;
}
// The occlusion query running counts the render encoder there is, each from a slot of its own in the submit's visibility buffer. It may have begun outside a render pass (Vulkan lets a query span
// from before a pass to after it: Super Mario 3D All-Stars' Sunshine, Hagi's GX) and a pass's encoder may be split for a copy: every encoder started while it runs gets a slot
static void query_record(subq_t *s, VkQueryPool pool, uint32_t idx, int64_t slot) {
    if (s->nwrites == s->write_cap) {
        uint32_t cap = s->write_cap ? 2 * s->write_cap : 16;
        query_write *next = realloc(s->writes, cap * sizeof *next);
        if (!next) die("[vk] out of memory for query writes");
        s->writes = next; s->write_cap = cap;
    }
    s->writes[s->nwrites++] = (query_write){pool, idx, slot};
}
static void occ_start(rs_t *s) {
    subq_t *q = s->sq; if (!s->aq || !s->re) return;
    if (q->nvis == q->cap) { RED1("occlusion query: more encoders than the visibility buffer has slots"); return; }
    query_record(q, s->aq, s->aq_first, q->nvis);
    [s->re setVisibilityResultMode:MTLVisibilityResultModeCounting offset:q->nvis * 8]; q->nvis++;
}
static void load_rp(rs_t *s) {
    end_blit(s); end_comp(s);
    MTLRenderPassDescriptor *d = s->rpd;
    for (int i = 0; i < 8; i++) if (d.colorAttachments[i].texture) d.colorAttachments[i].loadAction = MTLLoadActionLoad;
    if (d.depthAttachment.texture) d.depthAttachment.loadAction = MTLLoadActionLoad;
    if (d.stencilAttachment.texture) d.stencilAttachment.loadAction = MTLLoadActionLoad;
}
static void reopen_rp(rs_t *s) {
    fan_prepare(s, s->cmd);
    s->re = [s->cb renderCommandEncoderWithDescriptor:s->rpd]; s->cut = 0; g_n_resume++; s->enc_id = ++g_enc_id;
    s->applied = NULL; s->vp_dirty = s->sc_dirty = 1; s->db_live = 0;
    occ_start(s);
}
static void resume_rp(rs_t *s) { load_rp(s); reopen_rp(s); }

// An attachment opened with LOAD_OP_DONT_CARE is loaded, colour, depth and stencil alike: Vulkan leaves its contents undefined, the Switch's driver does no colour discard
// at render-pass begin or end (the macro it runs there is a no-op) and for depth and stencil only drops the ZCULL hints, so they survive there, and games read them.
// Re:ZERO's pfx pass blends its effects layer over the copy the pass before wrote into the same image, both opened with DONT_CARE; a DontCare load starts Apple's tiles
// empty, and the 3D world came out black. Its particle pass reopens the half-res depth with DONT_CARE and STORE, draws nothing, and the pfx pass samples that depth: a
// DontCare load stores whatever the tiles held (t_vk_dontcare; docs/reference/rendering.md, "Attachments opened with LOAD_OP_DONT_CARE")
static MTLLoadAction load_a(VkAttachmentLoadOp o) { return o == VK_ATTACHMENT_LOAD_OP_CLEAR ? MTLLoadActionClear : MTLLoadActionLoad; }
static MTLStoreAction store_a(VkAttachmentStoreOp o) { return o == VK_ATTACHMENT_STORE_OP_DONT_CARE ? MTLStoreActionDontCare : MTLStoreActionStore; }
static void begin_rp(rs_t *s, const c_brp *c) {
    end_blit(s); end_comp(s);
    end_rp(s, 1); s->cut = 0;
    VkRenderPass rp = c->rp; VkFramebuffer fb = c->fb;
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    uint32_t w = fb->w, h = fb->h;
    for (uint32_t i = 0; i < rp->ncol; i++) {
        int a = rp->col[i]; if (a < 0 || (uint32_t)a >= fb->n) continue;
        const ratt *at = &rp->att[a]; MTLRenderPassColorAttachmentDescriptor *ca = d.colorAttachments[i];
        ca.texture = fb->v[a]->tex; w = umin(w, (uint32_t)ca.texture.width); h = umin(h, (uint32_t)ca.texture.height);
        ca.loadAction = load_a(at->load); s->st_col[i] = store_a(at->store); ca.storeAction = MTLStoreActionUnknown;
        s->rt_color[i] = fmt_of(at->fmt);
        if (at->load == VK_ATTACHMENT_LOAD_OP_CLEAR && (uint32_t)a < c->nclear) ca.clearColor = fmt_clear_color(s->rt_color[i], &c->clear[a].color);
        int r = rp->res[i];
        if (r >= 0 && (uint32_t)r < fb->n) { ca.resolveTexture = fb->v[r]->tex; s->st_col[i] = at->store == VK_ATTACHMENT_STORE_OP_DONT_CARE ? MTLStoreActionMultisampleResolve : MTLStoreActionStoreAndMultisampleResolve; }
    }
    if (rp->depth >= 0 && (uint32_t)rp->depth < fb->n) {
        int a = rp->depth; const ratt *at = &rp->att[a]; const fmt_t *f = fmt_of(at->fmt); id<MTLTexture> t = fb->v[a]->tex;
        w = umin(w, (uint32_t)t.width); h = umin(h, (uint32_t)t.height);
        if (fmt_has_depth(f)) {
            d.depthAttachment.texture = t; d.depthAttachment.loadAction = load_a(at->load); s->st_d = store_a(at->store); d.depthAttachment.storeAction = MTLStoreActionUnknown;
            if ((uint32_t)a < c->nclear) d.depthAttachment.clearDepth = c->clear[a].depthStencil.depth;
        }
        if (fmt_has_stencil(f)) {
            d.stencilAttachment.texture = t; d.stencilAttachment.loadAction = load_a(at->sload); s->st_s = store_a(at->sstore); d.stencilAttachment.storeAction = MTLStoreActionUnknown;
            if ((uint32_t)a < c->nclear) d.stencilAttachment.clearStencil = c->clear[a].depthStencil.stencil;
        }
    }
    if (!rp->ncol && rp->depth < 0) { d.renderTargetWidth = fb->w; d.renderTargetHeight = fb->h; d.defaultRasterSampleCount = 1; }
    if (s->sq->vis) {
        d.visibilityResultBuffer = s->sq->vis;
        d.visibilityResultType = MTLVisibilityResultTypeAccumulate;   // Reset clears earlier encoders' slots too; the submit initialized this buffer to zero
    }
    fan_prepare(s, (const chdr *)((const uint8_t *)s->cmd + s->cmd->size));
    s->rpd = d; s->re = [s->cb renderCommandEncoderWithDescriptor:d]; s->enc_id = ++g_enc_id;
    s->applied = NULL; s->rt_w = w; s->rt_h = h; s->rt_layers = fb->layers; s->db_live = 0;
    // Dynamic viewport and scissor persist across render passes within a command buffer, as in Vulkan; the render target's size stands in only until the game sets them.
    // Resetting them at every pass drew Super Mario Galaxy's handheld UI 1.5x too big: it sets a 1280x720 viewport once and then draws through several passes into its 1920x1080 swapchain
    s->vp_dirty = s->sc_dirty = 1;
    g_n_pass++;
    occ_start(s);
}
// Bind the resources a shader's table row lists, for one stage (ST_*)
static void put_buf(rs_t *s, int st, id<MTLBuffer> b, NSUInteger off, NSUInteger i) {
    if (st == ST_COMP) [s->ce setBuffer:b offset:off atIndex:i]; else if (st == ST_FRAG) [s->re setFragmentBuffer:b offset:off atIndex:i]; else [s->re setVertexBuffer:b offset:off atIndex:i];
}
static void put_tex(rs_t *s, int st, id<MTLTexture> t, NSUInteger i) {
    if (st == ST_COMP) [s->ce setTexture:t atIndex:i]; else if (st == ST_FRAG) [s->re setFragmentTexture:t atIndex:i]; else [s->re setVertexTexture:t atIndex:i];
}
static void put_smp(rs_t *s, int st, id<MTLSamplerState> m, NSUInteger i) {
    if (st == ST_COMP) [s->ce setSamplerState:m atIndex:i]; else if (st == ST_FRAG) [s->re setFragmentSamplerState:m atIndex:i]; else [s->re setVertexSamplerState:m atIndex:i];
}
static void bind_res(rs_t *s, VkPipeline p, const shinfo *si, int st) {
    if (!si) return;
    __typeof__(s->set[0]) *sets = st == ST_COMP ? s->cset : s->set;
    for (int i = 0; i < si->nres; i++) {
        const shres *r = &si->res[i];
        if (!strchr("ubtcs", r->kind) || !r->kind) continue;   // p: bind_push; argument buffers (upper case, o, q): bind_args
        VkDescriptorSet ds = r->set < 8 ? sets[r->set].s : NULL;
        if (!ds) { RED1("shader %s: set %u is not bound", si->key, r->set); continue; }
        const VkDescriptorSetLayout L = ds->L; int bi = r->binding < 128 ? L->idx[r->binding] : -1;
        if (bi < 0 || !L->b[bi].count) {
            char have[256] = ""; size_t o = 0;
            for (uint32_t k = 0; k < L->n && o + 8 < sizeof have; k++) o += snprintf(have + o, sizeof have - o, " %u", L->b[k].binding);
            const VkDescriptorSetLayout PL = r->set < p->nset ? p->set[r->set] : NULL;
            RED1("shader %s: %u.%u is not in the bound set's layout %p (it has:%s); the pipeline layout's set %u is %p%s", si->key, r->set, r->binding, (void *)L, have, r->set, (void *)PL,
                 PL && r->binding < 128 && PL->idx[r->binding] >= 0 ? ", which has this binding: the set was not rebound with the pipeline" : "");
            continue;
        }
        const lbind *lb = &L->b[bi]; uint32_t n = umin(r->count, lb->count);
        for (uint32_t k = 0; k < n; k++) {
            const desc_t *e = &ds->d[lb->base + k]; NSUInteger slot = r->slot + k;
            switch (r->kind) {
                case 'u': case 'b': {
                    if (!e->buf) { RED1("shader %s: buffer %u.%u[%u] is empty", si->key, r->set, r->binding, k); break; }
                    VkDeviceSize off = e->off; if (lb->dyn >= 0 && lb->dyn + k < 16) off += sets[r->set].dyn[lb->dyn + k];
                    put_buf(s, st, e->buf, off, slot);
                    break;
                }
                case 't': case 'c':
                    if (!e->tex) { RED1("shader %s: texture %u.%u[%u] is empty", si->key, r->set, r->binding, k); break; }
                    put_tex(s, st, e->tex, slot);
                    if (r->kind == 'c' && e->smp) put_smp(s, st, e->smp.state, slot);
                    break;
                case 's':
                    if (!e->smp || !e->smp.state) { RED1("shader %s: sampler %u.%u[%u] is empty", si->key, r->set, r->binding, k); break; }
                    put_smp(s, st, e->smp.state, slot);
                    break;
            }
        }
    }
}
static void bind_push(rs_t *s, VkPipeline p, const shinfo *si, int st) {   // the push-constant block (kind p): the whole range of the pipeline layout, at least 16 bytes
    if (!si) return;
    for (int i = 0; i < si->nres; i++) {
        if (si->res[i].kind != 'p') continue;
        uint32_t n = (umax(p->pc_size, 16) + 15) & ~15u; NSUInteger slot = si->res[i].slot;
        if (st == ST_COMP) [s->ce setBytes:s->pc[st] length:n atIndex:slot];
        else if (st == ST_FRAG) [s->re setFragmentBytes:s->pc[st] length:n atIndex:slot];
        else [s->re setVertexBytes:s->pc[st] length:n atIndex:slot];
    }
}
static int ab_encode(abuf_t *a, VkDescriptorSet ds, const shinfo *si, uint64_t gen) {
    a->buf = [g_dev newBufferWithLength:umax(16, (uint32_t)a->enc.encodedLength) options:MTLResourceStorageModeShared];
    if (!a->buf) { RED1("cannot allocate an argument buffer for shader %s", si->key); return 0; }
    [a->enc setArgumentBuffer:a->buf offset:0];
    NSMutableOrderedSet *ro = [NSMutableOrderedSet orderedSet], *rw = [NSMutableOrderedSet orderedSet], *samplers = [NSMutableOrderedSet orderedSet];
    const struct VkDescriptorSetLayout_T *L = ds->L;
    for (int i = 0; i < si->nres; i++) {
        const shres *r = &si->res[i]; if (!is_arg(r->kind) || r->slot != a->slot) continue;
        int bi = r->binding < 128 ? L->idx[r->binding] : -1;
        if (bi < 0 || !L->b[bi].count) { RED1("shader %s: argument buffer member %u.%u is not in the bound set's layout", si->key, r->set, r->binding); continue; }
        const lbind *lb = &L->b[bi]; uint32_t n = umin(r->count, lb->count);
        for (uint32_t k = 0; k < n; k++) {
            const desc_t *e = &ds->d[lb->base + k]; NSUInteger aid = r->arg_id + k;
            switch (r->kind) {
                case 'T': [a->enc setTexture:e->tex atIndex:aid]; if (e->tex) [(e->tex.usage & MTLTextureUsageShaderWrite ? rw : ro) addObject:e->tex]; break;
                case 'S': {
                    id<MTLSamplerState> state = e->smp ? [e->smp argumentSampler] : nil; if (e->smp && !state) return 0;
                    [a->enc setSamplerState:state atIndex:aid]; if (e->smp) [samplers addObject:e->smp]; break;
                }
                default: [a->enc setBuffer:e->buf offset:e->off atIndex:aid]; if (e->buf) [(r->kind == 'B' ? rw : ro) addObject:e->buf]; break;   // U, B: the dynamic offset is added in the shader
            }
        }
    }
    a->keep = [[ro.array arrayByAddingObjectsFromArray:rw.array] arrayByAddingObjectsFromArray:samplers.array];
    a->nro = (uint32_t)ro.count; a->nrw = (uint32_t)rw.count;
    __unsafe_unretained id<MTLResource> *ro_ptr = (__unsafe_unretained id<MTLResource> *)realloc(a->ro, umax(1, a->nro) * sizeof *a->ro);
    if (!ro_ptr) die("[vk] out of memory");
    a->ro = ro_ptr;
    __unsafe_unretained id<MTLResource> *rw_ptr = (__unsafe_unretained id<MTLResource> *)realloc(a->rw, umax(1, a->nrw) * sizeof *a->rw);
    if (!rw_ptr) die("[vk] out of memory");
    a->rw = rw_ptr;
    for (uint32_t i = 0; i < a->nro; i++) a->ro[i] = ro[i];
    for (uint32_t i = 0; i < a->nrw; i++) a->rw[i] = rw[i];
    a->s = ds; a->gen = gen; a->enc_id = 0; return 1;
}
static int bind_args(rs_t *s, VkPipeline p, const shinfo *si, int st) {   // a stage's argument buffers and the dynamic offsets of the buffers in them
    if (!si || !p->nab[st]) return 1;
    __typeof__(s->set[0]) *sets = st == ST_COMP ? s->cset : s->set;
    for (int k = 0; k < p->nab[st]; k++) {
        abuf_t *a = &p->ab[st][k]; VkDescriptorSet ds = a->set < 8 ? sets[a->set].s : NULL;
        if (!ds) { RED1("shader %s: set %u (an argument buffer) is not bound", si->key, a->set); return 0; }
        // Use the same generation for the cache lookup and re-encoding. A new generation invalidates the cached
        // argument buffer when descriptors change or an allocation reuses a set address.
        uint64_t gen = ds->gen;
        if ((a->s != ds || a->gen != gen || !a->buf) && !ab_encode(a, ds, si, gen)) return 0;
        put_buf(s, st, a->buf, 0, a->slot);
        if (a->enc_id == s->enc_id) continue;
        a->enc_id = s->enc_id;
        if (st == ST_COMP) {
            if (a->nro) [s->ce useResources:a->ro count:a->nro usage:MTLResourceUsageRead];
            if (a->nrw) [s->ce useResources:a->rw count:a->nrw usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        } else {
            MTLRenderStages rs = st == ST_FRAG ? MTLRenderStageFragment : MTLRenderStageVertex;
            if (a->nro) [s->re useResources:a->ro count:a->nro usage:MTLResourceUsageRead stages:rs];
            if (a->nrw) [s->re useResources:a->rw count:a->nrw usage:MTLResourceUsageRead | MTLResourceUsageWrite stages:rs];
        }
    }
    uint32_t off[32] = {0}; int q = -1, n = 0;
    for (int i = 0; i < si->nres; i++) {
        const shres *r = &si->res[i];
        if (r->kind == 'q') { q = r->slot; continue; }
        if (r->kind != 'o' || r->slot >= 32) continue;
        VkDescriptorSet ds = r->set < 8 ? sets[r->set].s : NULL; int bi = ds && r->binding < 128 ? ds->L->idx[r->binding] : -1;
        if (bi >= 0 && ds->L->b[bi].dyn >= 0 && ds->L->b[bi].dyn < 16) off[r->slot] = sets[r->set].dyn[ds->L->b[bi].dyn];
        if (r->slot + 1 > n) n = r->slot + 1;
    }
    if (q >= 0) {
        NSUInteger len = ((NSUInteger)umax(4, n * 4) + 15) & ~(NSUInteger)15;
        if (st == ST_COMP) [s->ce setBytes:off length:len atIndex:q]; else if (st == ST_FRAG) [s->re setFragmentBytes:off length:len atIndex:q]; else [s->re setVertexBytes:off length:len atIndex:q];
    }
    return 1;
}
static MTLScissorRect metal_scissor(VkRect2D r, uint32_t w, uint32_t h) {
    // Metal requires the scissor to lie inside the render target.
    int64_t x1 = (int64_t)r.offset.x + r.extent.width, y1 = (int64_t)r.offset.y + r.extent.height;
    x1 = x1 < 0 ? 0 : x1 > w ? w : x1;
    y1 = y1 < 0 ? 0 : y1 > h ? h : y1;
    int64_t x0 = r.offset.x < 0 ? 0 : r.offset.x > x1 ? x1 : r.offset.x;
    int64_t y0 = r.offset.y < 0 ? 0 : r.offset.y > y1 ? y1 : r.offset.y;
    return (MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)};
}
static void apply_viewports(rs_t *s, VkPipeline p) {
    uint32_t n = p->nviewport;
    if (s->vp_dirty) {
        MTLViewport v[MAX_VIEWPORTS];
        const MTLViewport *vp = p->vp;
        if (!vp) {
            MTLViewport full = {0, 0, s->rt_w, s->rt_h, 0, 1};
            for (uint32_t i = 0; i < n; i++) v[i] = s->have_vp >> i & 1 ? vp_of(&s->vp[i]) : full;
            vp = v;
        }
        if (n == 1) [s->re setViewport:vp[0]]; else [s->re setViewports:vp count:n];
        s->vp_dirty = 0;
    }
    if (s->sc_dirty) {
        MTLScissorRect rects[MAX_VIEWPORTS];
        VkRect2D full = {{0, 0}, {s->rt_w, s->rt_h}};
        for (uint32_t i = 0; i < n; i++) {
            VkRect2D r = p->sc ? p->sc[i] : s->have_sc >> i & 1 ? s->sc[i] : full;
            rects[i] = metal_scissor(r, s->rt_w, s->rt_h);
        }
        if (n == 1) [s->re setScissorRect:rects[0]]; else [s->re setScissorRects:rects count:n];
        s->sc_dirty = 0;
    }
}
static int pre_draw(rs_t *s) {
    if (!s->re && s->cut) resume_rp(s);
    VkPipeline p = s->pipe; id<MTLRenderCommandEncoder> re = s->re;
    if (!re) { RED1("draw outside a render pass"); return 0; }
    if (!p) return 0;
    if (!p->pso) {   // a pipeline Metal did not build (a shader missing from the library, or refused): its draws are skipped, and each such pipeline is named once
        g_n_skip++;
        if (!p->told) { p->told = 1; LOG("[vk] [RED] draws skipped: the pipeline of shaders %s was not built", p->keys[0] ? p->keys : "(none)"); }
        return 0;
    }
    if (p->logic) { static uint64_t n; if (!n++) LOG("[vk] [RED] logic op %d not implemented: the pipeline draws with its shader's colour", p->logic); if (!(n % 1000)) LOG("[vk] logic op draws so far: %llu", (unsigned long long)n); }
    int dbk = DBND_OFF; float dbb[2] = {0, 1};
    if (p->db_en) {
        if (p->dyn_db) { if (s->have_db) dbk = dbnd_resolve(1, s->db[0], s->db[1], dbb); }
        else { dbk = p->dbk; dbb[0] = p->db[0]; dbb[1] = p->db[1]; }
    }
    if (dbk == DBND_EMPTY && mtl_depth_bounds_hw()) return 0;   // bounds no stored depth meets: nothing passes, and Metal refuses them (nvn_dbnd.h); drawn where the GPU has no test, as before
    if (s->applied != p) {
        [re setRenderPipelineState:p->pso]; [re setDepthStencilState:p->dss ? p->dss : g_dss_off];
        [re setCullMode:p->cull]; [re setFrontFacingWinding:p->wind]; [re setTriangleFillMode:p->fill]; [re setDepthClipMode:p->clip];
        [re setDepthBias:p->bias ? p->bias_c : 0 slopeScale:p->bias ? p->bias_s : 0 clamp:p->bias ? p->bias_cl : 0];
        [re setBlendColorRed:p->blend[0] green:p->blend[1] blue:p->blend[2] alpha:p->blend[3]];
        s->d_dirty = 1;
        [re setStencilFrontReferenceValue:p->sref_f backReferenceValue:p->sref_b];
        s->s_dirty = 1;
        VkPipeline old = s->applied;
        if (!old || old->nviewport != p->nviewport) s->vp_dirty = s->sc_dirty = 1;
        else {
            if (old->vp != p->vp && (!old->vp || !p->vp || memcmp(old->vp, p->vp, p->nviewport * sizeof *p->vp))) s->vp_dirty = 1;
            if (old->sc || p->sc) s->sc_dirty = 1;
        }
        s->applied = p;
    }
    if (s->d_dirty) {   // dynamic blend constants and depth bias, after the pipeline's own (a pipeline's depth bias applies only when it enables depth bias)
        if (p->dyn_blend && s->have_blend) [re setBlendColorRed:s->blend[0] green:s->blend[1] blue:s->blend[2] alpha:s->blend[3]];
        if (p->dyn_bias && p->bias && s->have_bias) [re setDepthBias:s->bias[0] slopeScale:s->bias[2] clamp:s->bias[1]];
        s->d_dirty = 0;
    }
    if (dbk == DBND_TEST && mtl_depth_bounds_hw()) {
        if (!s->db_live || s->db_cur[0] != dbb[0] || s->db_cur[1] != dbb[1]) {
            [re setDepthTestMinBound:dbb[0] maxBound:dbb[1]];
            s->db_cur[0] = dbb[0]; s->db_cur[1] = dbb[1]; s->db_live = 1;
        }
    } else if (s->db_live) {
        [re setDepthTestMinBound:0 maxBound:1];
        s->db_live = 0;
    }
    if (s->s_dirty && p->stencil && (p->dyn_sref || p->dyn_smask)) {   // dynamic stencil state, after the pipeline's own
        if (p->dyn_sref) [re setStencilFrontReferenceValue:s->sdyn_set[2] ? s->sdyn[2][0] : p->sref_f backReferenceValue:s->sdyn_set[2] ? s->sdyn[2][1] : p->sref_b];
        if (p->dyn_smask) {
            VkStencilOpState f = p->sf, b = p->sb;
            if ((p->dyn_smask & 1) && s->sdyn_set[0]) { f.compareMask = s->sdyn[0][0]; b.compareMask = s->sdyn[0][1]; }
            if ((p->dyn_smask & 2) && s->sdyn_set[1]) { f.writeMask = s->sdyn[1][0]; b.writeMask = s->sdyn[1][1]; }
            uint64_t key = (uint64_t)(f.compareMask & 0xFF) | (uint64_t)(f.writeMask & 0xFF) << 8 | (uint64_t)(b.compareMask & 0xFF) << 16 | (uint64_t)(b.writeMask & 0xFF) << 24 | 1ull << 32;
            if (key != p->dss_key) {
                MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
                dd.depthCompareFunction = p->dcmp; dd.depthWriteEnabled = p->dwrite; dd.frontFaceStencil = stencil_d(&f); dd.backFaceStencil = stencil_d(&b);
                p->dss_dyn = [g_dev newDepthStencilStateWithDescriptor:dd]; p->dss_key = key;
            }
            [re setDepthStencilState:p->dss_dyn];
        }
    }
    s->s_dirty = 0;
    if (s->vp_dirty || s->sc_dirty) apply_viewports(s, p);
    bind_res(s, p, p->vs, ST_VERT); bind_res(s, p, p->fs, ST_FRAG); bind_push(s, p, p->vs, ST_VERT); bind_push(s, p, p->fs, ST_FRAG);
    if (!bind_args(s, p, p->vs, ST_VERT) || !bind_args(s, p, p->fs, ST_FRAG)) return 0;
    for (uint32_t i = 0; i < 14; i++)
        if (p->vb_mask >> i & 1) {
            VkBuffer b = s->vb[i]; if (!b || !b->mem) { RED1("vertex buffer %u not bound", i); return 0; }
            [re setVertexBuffer:mem_buf(b->mem) offset:b->off + s->vbo[i] atIndex:p->vb_slot[i]];
        }
    return 1;
}
// ---------- Triangle fans ----------
// Metal has no fan topology: a fan draws as a triangle list whose triangle i is (i+1, i+2, 0), Vulkan's order, so the first (provoking) vertex is the rim's; putting the anchor
// first changes flat-shaded attributes. A draw without indices indexes one shared list, its first vertex the base vertex, which keeps VertexIndex and BaseVertex.
// Indexed fans are converted on the GPU (dm/gfx/msl/vk_fan.metal), since copies earlier in the submission may write their indices. All of them up to the next command that may
// split the render encoder are converted in one compute pass before the encoder starts (fan_prepare): Vulkan copies nothing inside a render pass, and the copies this layer
// runs there anyway split the pass (blit), so the indices a stretch of the pass reads are final when it begins. Converting at each draw would split the encoder there, storing
// and loading every attachment once per draw. A restart index ends a fan (compared before vertexOffset is added); the conversion then compacts the runs into one list and
// writes its length into the draw's indirect arguments. docs/reference/rendering.md, "Vulkan triangle fans"; t_vk_fan
static id<MTLBuffer> g_fan_ib; static uint64_t g_fan_tris;   // the shared list (replay holds g_qmu); one it replaced stays alive in the command buffers that use it
static id<MTLBuffer> fan_shared(uint32_t tris) {
    if (tris > g_fan_tris) {
        uint64_t n = g_fan_tris ? g_fan_tris : 4096; while (n < tris) n *= 2;
        if (n * 12 > g_dev.maxBufferLength) n = tris;
        id<MTLBuffer> b = n * 12 <= g_dev.maxBufferLength ? [g_dev newBufferWithLength:n * 12 options:MTLResourceStorageModeShared] : nil;
        if (!b) { RED1("cannot allocate triangle fan indices (%u triangles)", tris); return nil; }
        uint32_t *ix = b.contents;
        for (uint64_t i = 0; i < n; i++) { ix[3 * i] = (uint32_t)i + 1; ix[3 * i + 1] = (uint32_t)i + 2; ix[3 * i + 2] = 0; }
        g_fan_ib = b; g_fan_tris = n;
    }
    return g_fan_ib;
}
static void fan_arrays(rs_t *s, const c_draw *x) {
    if (x->count < 3) return;
    id<MTLBuffer> b = fan_shared(x->count - 2); if (!b) return;
    [s->re drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:(NSUInteger)(x->count - 2) * 3 indexType:MTLIndexTypeUInt32 indexBuffer:b
              indexBufferOffset:0 instanceCount:x->inst baseVertex:x->first baseInstance:x->first_inst];
}
static int fan_scan_passes(uint32_t op) {   // commands that never end a render encoder; any other ends the scan
    switch (op) {
        case C_PIPE: case C_SETS: case C_VB: case C_IB: case C_VP: case C_SC: case C_DRAWI: case C_DRAW: case C_PUSH:
        case C_QRESET: case C_QTS: case C_QBEGIN: case C_QEND: case C_STENCIL: case C_DYN: return 1;
    }
    return 0;
}
static id<MTLBuffer> fan_convert(id<MTLCommandBuffer> cb, const fan_job *jobs, uint32_t n, uint64_t size) {
    if (!n) return nil;
    static id<MTLComputePipelineState> ps; static int failed;
    if (!ps && !failed) {
        NSError *e = nil; id<MTLLibrary> lib = host_msl_lib(g_dev, nil, "vk_fan", &e);
        id<MTLFunction> fn = [lib newFunctionWithName:@"vk_fan"];
        ps = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&e] : nil;
        if (!ps) { failed = 1; LOG("[vk] [RED] cannot build the triangle fan conversion (indexed fans are not drawn): %s", e.localizedDescription.UTF8String); }
    }
    if (!ps) return nil;
    id<MTLBuffer> out = size <= g_dev.maxBufferLength ? [g_dev newBufferWithLength:size options:MTLResourceStorageModeShared] : nil;
    if (!out) { RED1("cannot allocate triangle fan indices (%llu bytes)", (unsigned long long)size); return nil; }
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:ps];
    for (uint32_t i = 0; i < n; i++) {
        const fan_job *j = &jobs[i];
        [ce setBuffer:mem_buf(j->ib->mem) offset:0 atIndex:0]; [ce setBuffer:out offset:j->at + 32 atIndex:1]; [ce setBuffer:out offset:j->at atIndex:2];
        [ce setBytes:&j->p length:sizeof j->p atIndex:3];
        NSUInteger threads = j->p.restart ? 1 : j->p.count - 2;   // restart compacts the runs without crossing a marker
        [ce dispatchThreads:MTLSizeMake(threads, 1, 1) threadsPerThreadgroup:MTLSizeMake(MIN(threads, ps.threadExecutionWidth), 1, 1)];
    }
    [ce endEncoding];
    return out;
}
static void fan_draw_converted(rs_t *s, id<MTLBuffer> out, uint64_t at) {
    [s->re drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexType:MTLIndexTypeUInt32 indexBuffer:out indexBufferOffset:at + 32
                  indirectBuffer:out indirectBufferOffset:at];
}
// Converts the indexed fans from command h up to the end of the stretch of the pass the encoder about to start covers, into one buffer, in one compute pass
static void fan_prepare(rs_t *s, const chdr *h) {
    s->nfj = s->fji = 0; s->fan_out = nil; s->fan_from = s->fan_to = h;
    if (!g_fan_pipes) return;
    VkPipeline pipe = s->pipe; VkBuffer ib = s->ib; VkDeviceSize ibo = s->ibo; uint32_t isz = s->isz; uint64_t size = 0;
    for (; (const uint8_t *)h < s->cmd_end && fan_scan_passes(h->op); h = (const chdr *)((const uint8_t *)h + h->size)) {
        const void *c = h + 1;
        if (h->op == C_PIPE && !((const c_pipe *)c)->compute) pipe = ((const c_pipe *)c)->p;
        else if (h->op == C_IB) { const c_ib *x = c; ib = x->b; ibo = x->o; isz = x->t == VK_INDEX_TYPE_UINT32 ? 4 : 2; }
        else if (h->op == C_DRAWI) {
            const c_drawi *x = c;
            if (!pipe || !pipe->fan || x->count < 3 || !x->inst || !ib || !ib->mem) continue;
            if (s->nfj == s->fjcap) { s->fjcap = s->fjcap ? 2 * s->fjcap : 16; fan_job *q = realloc(s->fj, s->fjcap * sizeof *s->fj); if (!q) die("[vk] out of memory"); s->fj = q; }
            s->fj[s->nfj++] = (fan_job){c, size, ib, {ib->off + ibo + (VkDeviceSize)x->first * isz, x->count, isz, (uint32_t)pipe->restart, x->inst, x->voff, x->first_inst}};
            size += (32 + (uint64_t)(x->count - 2) * 12 + 31) & ~31ull;   // arguments, then indices
        }
    }
    s->fan_to = h;
    s->fan_out = fan_convert(s->cb, s->fj, s->nfj, size);
    if (!s->fan_out) s->nfj = 0;
}
static const fan_job *fan_job_of(rs_t *s, const void *cmd) {   // draws replay in the order they were scanned
    while (s->fji < s->nfj && (const uint8_t *)s->fj[s->fji].cmd < (const uint8_t *)cmd) s->fji++;
    return s->fji < s->nfj && s->fj[s->fji].cmd == cmd ? &s->fj[s->fji] : NULL;
}
static void fan_indexed(rs_t *s, const c_drawi *x) {
    if (x->count < 3) return;
    const fan_job *j = fan_job_of(s, x);
    if (!j && ((const uint8_t *)x < (const uint8_t *)s->fan_from || (const uint8_t *)x >= (const uint8_t *)s->fan_to)) {
        // Past the scan, which a command stopped that did not end the encoder after all: split it here, converting from this draw on
        end_rp(s, 0); s->cut = 1;
        if (!pre_draw(s)) return;
        j = fan_job_of(s, x);
    }
    if (!j) return;   // not converted: reported by fan_prepare
    fan_draw_converted(s, s->fan_out, j->at);
}

static void fan_indirect(rs_t *s, const c_drawind *x) {
    // The argument buffer may have been written by the GPU. Finish that work before
    // sizing the generated indices, then use the direct fan draw and conversion paths.
    split_submit(s);
    const uint8_t *args = (const uint8_t *)mem_buf(x->b->mem).contents + x->b->off + x->o;
    if (!x->indexed) {
        if (!pre_draw(s)) return;
        for (uint32_t i = 0; i < x->count; i++) {
            VkDrawIndirectCommand v;
            memcpy(&v, args + (size_t)i * x->stride, sizeof v);
            if (v.vertexCount < 3 || !v.instanceCount) continue;
            c_draw draw = {v.vertexCount, v.instanceCount, v.firstVertex, v.firstInstance};
            fan_arrays(s, &draw); g_n_draw++;
        }
        return;
    }
    fan_job *jobs = zalloc((size_t)x->count * sizeof *jobs);
    uint64_t size = 0; uint32_t n = 0;
    for (uint32_t i = 0; i < x->count; i++) {
        VkDrawIndexedIndirectCommand v;
        memcpy(&v, args + (size_t)i * x->stride, sizeof v);
        if (v.indexCount < 3 || !v.instanceCount) continue;
        VkDeviceSize offset = s->ib->off + s->ibo + (VkDeviceSize)v.firstIndex * s->isz;
        fan_p p = {offset, v.indexCount, s->isz, (uint32_t)s->pipe->restart, v.instanceCount, v.vertexOffset, v.firstInstance};
        jobs[n++] = (fan_job){NULL, size, s->ib, p};
        size += (32 + (uint64_t)(v.indexCount - 2) * 12 + 31) & ~31ull;
    }
    id<MTLBuffer> out = fan_convert(s->cb, jobs, n, size);
    if (out && pre_draw(s)) {
        for (uint32_t i = 0; i < n; i++) fan_draw_converted(s, out, jobs[i].at);
        g_n_draw += n;
    }
    free(jobs);
}
static void write_query_result(uint8_t *out, uint64_t value, int available, VkQueryResultFlags flags) {
    size_t word = flags & VK_QUERY_RESULT_64_BIT ? 8 : 4;
    if (available || (flags & VK_QUERY_RESULT_PARTIAL_BIT)) memcpy(out, &value, word);
    if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {
        uint64_t status = available;
        memcpy(out + word, &status, word);
    }
}

static void copy_queries(rs_t *s, const c_qcopy *x) {
    if (!x->dst->mem) { RED1("query results copied into a buffer without bound memory"); return; }
    if (x->first >= x->q->n) return;
    uint32_t n = umin(x->n, x->q->n - x->first);
    if (!n) return;
    uint64_t pending = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t t = x->q->ticket[x->first + i];
        if (t != FENCE_NONE && t > pending) pending = t;
    }
    // The host's query values are collected on Metal completion. Resolve the prefix when
    // needed, then encode a transfer so later GPU work observes the writes in order.
    if (pending == s->sq->v) split_submit(s);
    else if (pending && (x->fl & VK_QUERY_RESULT_WAIT_BIT)) wait_done(pending);
    uint64_t done = atomic_load(&g_done);
    size_t word = x->fl & VK_QUERY_RESULT_64_BIT ? 8 : 4;
    size_t elem = word * (x->fl & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT ? 2 : 1);
    id<MTLBuffer> stage = [g_dev newBufferWithLength:n * elem options:MTLResourceStorageModeShared];
    if (!stage) { RED1("cannot allocate staging for query copies"); return; }
    uint8_t *dst = stage.contents;
    id<MTLBuffer> buffer = mem_buf(x->dst->mem);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = x->first + i;
        int available = x->q->ticket[k] <= done;
        size_t start = available || (x->fl & VK_QUERY_RESULT_PARTIAL_BIT) ? 0 : word;
        if (start == elem) continue;
        write_query_result(dst + i * elem, available ? x->q->val[k] : 0, available, x->fl);
        id<MTLBlitCommandEncoder> be = blit(s, C_QCOPY);
        [be copyFromBuffer:stage sourceOffset:i * elem + start toBuffer:buffer
          destinationOffset:x->dst->off + x->off + i * x->stride + start size:elem - start];
    }
}

static uint32_t mip_dim(uint32_t size, uint32_t level) { return level < 32 ? umax(1, size >> level) : 1; }
static int transfer_layers(VkImage im, VkImageSubresourceLayers layers) {
    return im->tex && layers.mipLevel < im->mips &&
        layers.layerCount && layers.baseArrayLayer < im->layers && layers.layerCount <= im->layers - layers.baseArrayLayer;
}
static int offset_in_2d(VkOffset3D p, uint32_t w, uint32_t h) { return p.x >= 0 && p.y >= 0 && (uint32_t)p.x <= w && (uint32_t)p.y <= h; }
static int blit_offset(VkImage im, uint32_t mip, VkOffset3D p) {
    return offset_in_2d(p, mip_dim(im->w, mip), mip_dim(im->h, mip)) && p.z >= 0 && (uint32_t)p.z <= mip_dim(im->d, mip);
}
static void split_for_transfer(rs_t *s) {
    if (s->re) { end_rp(s, 0); s->cut = 1; }
    end_blit(s); end_comp(s);
}

static const char transfer_msl[] =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 pos [[position]]; float2 uv; };\n"
    "vertex V blit_vs(uint i [[vertex_id]], constant float4 &uv [[buffer(0)]]) {"
    " float2 t=float2(i&1,i>>1); return {float4(t*float2(2,-2)+float2(-1,1),0,1), mix(uv.xy,uv.zw,t)}; }\n"
    "fragment float4 blit_float(V v [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { return t.sample(s,v.uv); }\n"
    "uint2 pixel_coord(float2 uv, uint2 size) { return min(uint2(max(uv,0.0)*float2(size)),size-1); }\n"
    "fragment uint4 blit_uint(V v [[stage_in]], texture2d<uint> t [[texture(0)]]) { return t.read(pixel_coord(v.uv,uint2(t.get_width(),t.get_height()))); }\n"
    "fragment int4 blit_int(V v [[stage_in]], texture2d<int> t [[texture(0)]]) { return t.read(pixel_coord(v.uv,uint2(t.get_width(),t.get_height()))); }\n"
    "fragment uint4 resolve_uint(V v [[stage_in]], texture2d_ms<uint> t [[texture(0)]]) { return t.read(pixel_coord(v.uv,uint2(t.get_width(),t.get_height())),0u); }\n"
    "fragment int4 resolve_int(V v [[stage_in]], texture2d_ms<int> t [[texture(0)]]) { return t.read(pixel_coord(v.uv,uint2(t.get_width(),t.get_height())),0u); }\n";
enum { BLIT_FLOAT, BLIT_UINT, BLIT_INT, RESOLVE_UINT, RESOLVE_INT };
static id<MTLRenderPipelineState> transfer_pso(MTLPixelFormat format, int variant) {
    static id<MTLLibrary> lib;
    static NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *psos;
    static NSString *const fn[] = {@"blit_float", @"blit_uint", @"blit_int", @"resolve_uint", @"resolve_int"};
    NSNumber *key = @((NSUInteger)format * 8 + variant);
    id<MTLRenderPipelineState> p = psos[key];   // replay is serial under g_qmu
    if (p) return p;
    NSError *err = nil;
    if (!lib) lib = [g_dev newLibraryWithSource:@(transfer_msl) options:nil error:&err];
    if (lib) {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [lib newFunctionWithName:@"blit_vs"];
        d.fragmentFunction = [lib newFunctionWithName:fn[variant]];
        d.colorAttachments[0].pixelFormat = format;
        p = [g_dev newRenderPipelineStateWithDescriptor:d error:&err];
    }
    if (!p) { LOG("[vk] [RED] image transfer pipeline for format %lu: %s", (unsigned long)format, err.localizedDescription.UTF8String); return nil; }
    if (!psos) psos = [NSMutableDictionary new];
    psos[key] = p;
    return p;
}

static id<MTLTexture> transfer_view(id<MTLTexture> tex, NSUInteger level, NSUInteger layer, MTLTextureType type) {
    if (tex.textureType == type && tex.mipmapLevelCount == 1) return tex;
    return [tex newTextureViewWithPixelFormat:tex.pixelFormat textureType:type levels:NSMakeRange(level, 1) slices:NSMakeRange(layer, 1)];
}
static void draw_transfer(rs_t *s, id<MTLTexture> src, id<MTLTexture> dst, uint32_t mip, uint32_t layer,
                          VkOffset3D a, VkOffset3D b, MTLViewport vp, int variant, VkFilter filter) {
    id<MTLRenderPipelineState> pso = transfer_pso(dst.pixelFormat, variant);
    if (!pso) return;
    split_for_transfer(s);
    float uv[4] = {(float)a.x / src.width, (float)a.y / src.height, (float)b.x / src.width, (float)b.y / src.height};
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    MTLRenderPassColorAttachmentDescriptor *color = d.colorAttachments[0];
    color.texture = dst; color.level = mip; color.slice = layer;
    color.loadAction = MTLLoadActionLoad; color.storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [s->cb renderCommandEncoderWithDescriptor:d];
    [e setRenderPipelineState:pso]; [e setViewport:vp]; [e setVertexBytes:uv length:sizeof uv atIndex:0]; [e setFragmentTexture:src atIndex:0];
    if (variant == BLIT_FLOAT) {
        static id<MTLSamplerState> samplers[2];
        int linear = filter == VK_FILTER_LINEAR;
        if (!samplers[linear]) {
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
            sd.minFilter = sd.magFilter = linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
            samplers[linear] = [g_dev newSamplerStateWithDescriptor:sd];
        }
        [e setFragmentSamplerState:samplers[linear] atIndex:0];
    }
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4]; [e endEncoding]; g_n_copy++;
}

static void blit_image(rs_t *s, const c_blit *x) {
    VkImage a = x->src, b = x->dst;
    int sk = a->f->kind, dk = b->f->kind, sign = fmt_is_signed_int(a->f);
    int volume = a->type == VK_IMAGE_TYPE_3D || b->type == VK_IMAGE_TYPE_3D;
    for (uint32_t i = 0; i < x->n; i++) {
        const VkImageBlit *r = &x->r[i];
        VkImageSubresourceLayers src = r->srcSubresource, dst = r->dstSubresource;
        VkOffset3D a0 = r->srcOffsets[0], a1 = r->srcOffsets[1], b0 = r->dstOffsets[0], b1 = r->dstOffsets[1];
        VkImageAspectFlags aspect = src.aspectMask;
        if (!transfer_layers(a, src) || !transfer_layers(b, dst) || src.layerCount != dst.layerCount || aspect != dst.aspectMask ||
            !blit_offset(a, src.mipLevel, a0) || !blit_offset(a, src.mipLevel, a1) || !blit_offset(b, dst.mipLevel, b0) || !blit_offset(b, dst.mipLevel, b1)) {
            RED1("invalid or unsupported vkCmdBlitImage region"); continue;
        }
        int dx = b1.x - b0.x, dy = b1.y - b0.y, dz = b1.z - b0.z;
        if (!dx || !dy || !dz || a0.x == a1.x || a0.y == a1.y || a0.z == a1.z) continue;
        int color = aspect == VK_IMAGE_ASPECT_COLOR_BIT;
        int exact_aspect = (color && (sk == K_COLOR || sk == K_INT || sk == K_BLOCK)) ||
                           (sk == K_DEPTH && aspect == VK_IMAGE_ASPECT_DEPTH_BIT) || (sk == K_STENCIL && aspect == VK_IMAGE_ASPECT_STENCIL_BIT);
        int copy = exact_aspect && a->fmt == b->fmt && a->type == b->type && dx > 0 && dy > 0 && dz > 0 &&
                   dx == a1.x - a0.x && dy == a1.y - a0.y && dz == a1.z - a0.z &&
                   (x->filter == VK_FILTER_NEAREST || (sk != K_INT && sk != K_DEPTH && sk != K_STENCIL));
        if (!copy && (volume || !color || a0.z || b0.z || a1.z != 1 || b1.z != 1 ||
            (sk != K_COLOR && sk != K_INT && sk != K_BLOCK) || (dk != K_COLOR && dk != K_INT) ||
            (sk == K_INT) != (dk == K_INT) || (sk == K_INT && (sign != fmt_is_signed_int(b->f) || x->filter != VK_FILTER_NEAREST)))) {
            RED1("unsupported vkCmdBlitImage format or region"); continue;
        }
        for (uint32_t l = 0; l < src.layerCount; l++) {
            uint32_t sl = src.baseArrayLayer + l, dl = dst.baseArrayLayer + l;
            if (copy) {
                [blit(s, C_BLIT) copyFromTexture:a->tex sourceSlice:sl sourceLevel:src.mipLevel sourceOrigin:MTLOriginMake(a0.x,a0.y,a0.z)
                                       sourceSize:MTLSizeMake(dx,dy,dz) toTexture:b->tex destinationSlice:dl destinationLevel:dst.mipLevel
                                destinationOrigin:MTLOriginMake(b0.x,b0.y,b0.z)];
                g_n_copy++; continue;
            }
            id<MTLTexture> view = transfer_view(a->tex, src.mipLevel, sl, MTLTextureType2D);
            if (!view) { RED1("vkCmdBlitImage source view unavailable"); continue; }
            VkOffset3D start = {dx < 0 ? a1.x : a0.x, dy < 0 ? a1.y : a0.y}, end = {dx < 0 ? a0.x : a1.x, dy < 0 ? a0.y : a1.y};
            MTLViewport vp = {umin(b0.x,b1.x), umin(b0.y,b1.y), abs(dx), abs(dy), 0, 1};
            draw_transfer(s, view, b->tex, dst.mipLevel, dl, start, end, vp, sk != K_INT ? BLIT_FLOAT : sign ? BLIT_INT : BLIT_UINT, x->filter);
        }
    }
}

static void resolve_image(rs_t *s, const c_resolve *x) {
    VkImage a = x->src, b = x->dst;
    if (a->type != VK_IMAGE_TYPE_2D || b->type != VK_IMAGE_TYPE_2D || (a->f->kind != K_COLOR && a->f->kind != K_INT)) {
        RED1("vkCmdResolveImage requires a 2D color format"); return;
    }
    for (uint32_t i = 0; i < x->n; i++) {
        const VkImageResolve *r = &x->r[i];
        VkImageSubresourceLayers src = r->srcSubresource, dst = r->dstSubresource;
        VkOffset3D so = r->srcOffset, dest = r->dstOffset;
        VkExtent3D size = r->extent;
        uint32_t aw = mip_dim(a->w, src.mipLevel), ah = mip_dim(a->h, src.mipLevel), bw = mip_dim(b->w, dst.mipLevel), bh = mip_dim(b->h, dst.mipLevel);
        if (!transfer_layers(a, src) || !transfer_layers(b, dst) || src.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || dst.aspectMask != src.aspectMask ||
            src.layerCount != dst.layerCount || so.z || dest.z || size.depth != 1 || !size.width || !size.height ||
            !offset_in_2d(so, aw, ah) || !offset_in_2d(dest, bw, bh) || size.width > aw - so.x || size.height > ah - so.y || size.width > bw - dest.x || size.height > bh - dest.y) {
            RED1("invalid or unsupported vkCmdResolveImage region"); continue;
        }
        int full = !so.x && !so.y && !dest.x && !dest.y && size.width == aw && size.height == ah && aw == bw && ah == bh;
        for (uint32_t l = 0; l < src.layerCount; l++) {
            uint32_t sl = src.baseArrayLayer + l, dl = dst.baseArrayLayer + l;
            if (a->f->kind == K_INT) {
                id<MTLTexture> view = transfer_view(a->tex, src.mipLevel, sl, MTLTextureType2DMultisample);
                if (!view) { RED1("integer vkCmdResolveImage source view unavailable"); continue; }
                VkOffset3D end = {so.x + size.width, so.y + size.height};
                MTLViewport vp = {dest.x, dest.y, size.width, size.height, 0, 1};
                draw_transfer(s, view, b->tex, dst.mipLevel, dl, so, end, vp, fmt_is_signed_int(a->f) ? RESOLVE_INT : RESOLVE_UINT, VK_FILTER_NEAREST);
                continue;
            }
            id<MTLTexture> target = b->tex;
            if (!full) {   // Metal resolves the entire source; commit only the requested rectangle to the destination.
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:a->tex.pixelFormat width:aw height:ah mipmapped:NO];
                td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageRenderTarget;
                target = [g_dev newTextureWithDescriptor:td];
                if (!target) { RED1("cannot allocate partial resolve texture"); continue; }
            }
            split_for_transfer(s);
            MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
            MTLRenderPassColorAttachmentDescriptor *color = d.colorAttachments[0];
            color.texture = a->tex; color.level = src.mipLevel; color.slice = sl;
            color.resolveTexture = target; color.resolveLevel = full ? dst.mipLevel : 0; color.resolveSlice = full ? dl : 0;
            color.loadAction = MTLLoadActionLoad; color.storeAction = MTLStoreActionStoreAndMultisampleResolve;
            [[s->cb renderCommandEncoderWithDescriptor:d] endEncoding];
            if (!full) [blit(s, C_RESOLVE) copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(so.x,so.y,0)
                                                 sourceSize:MTLSizeMake(size.width,size.height,1) toTexture:b->tex destinationSlice:dl destinationLevel:dst.mipLevel
                                          destinationOrigin:MTLOriginMake(dest.x,dest.y,0)];
            g_n_copy++;
        }
    }
}
static int clear_covers_attachment(MTLRenderPassAttachmentDescriptor *a, const VkClearRect *rects, uint32_t n) {
    if (!a.texture) return 1;   // VK_ATTACHMENT_UNUSED and an absent depth / stencil aspect have no effect
    for (uint32_t i = 0; i < n; i++) {
        const VkClearRect *r = &rects[i];
        if (!r->baseArrayLayer && r->layerCount == 1 && !r->rect.offset.x && !r->rect.offset.y &&
            r->rect.extent.width == MAX(a.texture.width >> a.level, 1u) && r->rect.extent.height == MAX(a.texture.height >> a.level, 1u)) return 1;
    }
    return 0;
}
// vkCmdClearAttachments is independent of all game graphics state. A load action handles whole attachments; otherwise use the host's precompiled clear triangle in a
// separate load/store pass for each selected view layer. The game's encoder resumes through reopen_rp, restoring its bindings and allocating a new occlusion-query slot.
// Helper passes have no visibility buffer: Vulkan permits clears to contribute to occlusion queries, but does not require them to do so.
typedef struct { uint32_t color, depth, stencil, samples, kind; } attachment_clear_key;
static id<MTLRenderPipelineState> attachment_clear_pso(MTLRenderPassDescriptor *d, const fmt_t *f) {
    id<MTLTexture> color = d.colorAttachments[0].texture, depth = d.depthAttachment.texture, stencil = d.stencilAttachment.texture;
    id<MTLTexture> t = color ?: depth ?: stencil;
    attachment_clear_key k = {(uint32_t)color.pixelFormat, (uint32_t)depth.pixelFormat, (uint32_t)stencil.pixelFormat, (uint32_t)t.sampleCount,
        f && f->kind == K_INT ? (fmt_is_signed_int(f) ? 2u : 1u) : 0u};
    static NSMutableDictionary<NSData *, id<MTLRenderPipelineState>> *cache;
    NSData *key = [NSData dataWithBytes:&k length:sizeof k]; id<MTLRenderPipelineState> pso = cache[key]; if (pso) return pso;
    static id<MTLLibrary> lib; static int failed;
    if (failed) return nil;
    NSError *e = nil;
    if (!lib) lib = host_msl_lib(g_dev, nil, "clear", &e);
    if (!lib) { failed = 1; LOG("[vk] [RED] attachment clear shaders: %s", e.localizedDescription.UTF8String); return nil; }
    MTLRenderPipelineDescriptor *p = [MTLRenderPipelineDescriptor new];
    p.vertexFunction = [lib newFunctionWithName:@"clr_vs"];
    if (color) {
        static const char *types[] = {"float", "uint", "int"};
        p.fragmentFunction = [lib newFunctionWithName:[NSString stringWithFormat:@"clr_fs_0_%s", types[k.kind]]];
        p.colorAttachments[0].pixelFormat = color.pixelFormat;
    }
    p.depthAttachmentPixelFormat = depth.pixelFormat; p.stencilAttachmentPixelFormat = stencil.pixelFormat; p.rasterSampleCount = t.sampleCount;
    pso = [g_dev newRenderPipelineStateWithDescriptor:p error:&e];
    if (!pso) { LOG("[vk] [RED] attachment clear pipeline (%u/%u/%u, %u samples): %s", k.color, k.depth, k.stencil, k.samples, e.localizedDescription.UTF8String); return nil; }
    if (!cache) cache = [NSMutableDictionary new]; cache[key] = pso; return pso;
}
static id<MTLDepthStencilState> attachment_clear_dss(int depth, int stencil) {
    static id<MTLDepthStencilState> cache[2][2];
    if (cache[depth][stencil]) return cache[depth][stencil];
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new]; d.depthCompareFunction = MTLCompareFunctionAlways; d.depthWriteEnabled = depth;
    if (stencil) {
        MTLStencilDescriptor *st = [MTLStencilDescriptor new]; st.stencilCompareFunction = MTLCompareFunctionAlways; st.depthStencilPassOperation = MTLStencilOperationReplace;
        st.writeMask = 0xff; d.frontFaceStencil = d.backFaceStencil = st;
    }
    return cache[depth][stencil] = [g_dev newDepthStencilStateWithDescriptor:d];
}
static void attachment_clear_layer(MTLRenderPassAttachmentDescriptor *dst, MTLRenderPassAttachmentDescriptor *src, uint32_t layer) {
    dst.texture = src.texture; dst.level = src.level;
    if (src.texture.textureType == MTLTextureType3D) dst.depthPlane = src.depthPlane + layer;
    else dst.slice = src.slice + layer;   // src.texture is already the Vulkan view, including its base mip and array layer
    dst.loadAction = MTLLoadActionLoad; dst.storeAction = MTLStoreActionStore;
}
static void clear_attachment_rects(rs_t *s, const VkClearAttachment *a, const VkClearRect *rects, uint32_t n) {
    MTLRenderPassDescriptor *source = s->rpd;
    BOOL color = a->aspectMask == VK_IMAGE_ASPECT_COLOR_BIT;
    BOOL depth = (a->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) && source.depthAttachment.texture;
    BOOL stencil = (a->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) && source.stencilAttachment.texture;
    MTLRenderPassAttachmentDescriptor *at = color ? source.colorAttachments[a->colorAttachment] : depth ? source.depthAttachment : source.stencilAttachment;
    if (!at.texture || (!color && !depth && !stencil)) return;
    const fmt_t *f = color ? s->rt_color[a->colorAttachment] : NULL;
    float z = depth ? a->clearValue.depthStencil.depth : 0;
    for (uint32_t layer = 0; layer < s->rt_layers; layer++) {
        BOOL selected = NO;
        for (uint32_t j = 0; j < n; j++) if (layer >= rects[j].baseArrayLayer && layer - rects[j].baseArrayLayer < rects[j].layerCount) { selected = YES; break; }
        if (!selected) continue;
        MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
        if (color) attachment_clear_layer(d.colorAttachments[0], at, layer);
        else {
            // Bind both planes when present, preserving the plane this clear does not write.
            if (source.depthAttachment.texture) attachment_clear_layer(d.depthAttachment, source.depthAttachment, layer);
            if (source.stencilAttachment.texture) attachment_clear_layer(d.stencilAttachment, source.stencilAttachment, layer);
        }
        id<MTLRenderPipelineState> pso = attachment_clear_pso(d, f); if (!pso) return;
        id<MTLRenderCommandEncoder> en = [s->cb renderCommandEncoderWithDescriptor:d];
        [en setRenderPipelineState:pso]; [en setDepthStencilState:attachment_clear_dss(depth, stencil)];
        [en setVertexBytes:&z length:sizeof z atIndex:0];
        if (color) [en setFragmentBytes:&a->clearValue.color length:sizeof a->clearValue.color atIndex:0];   // retain integer bits, including values above INT32_MAX
        if (stencil) [en setStencilReferenceValue:a->clearValue.depthStencil.stencil];
        [en setViewport:(MTLViewport){0, 0, MAX(at.texture.width >> at.level, 1u), MAX(at.texture.height >> at.level, 1u), 0, 1}];
        [en setCullMode:MTLCullModeNone];
        for (uint32_t j = 0; j < n; j++) {
            const VkClearRect *r = &rects[j];
            if (layer < r->baseArrayLayer || layer - r->baseArrayLayer >= r->layerCount) continue;
            [en setScissorRect:(MTLScissorRect){r->rect.offset.x, r->rect.offset.y, r->rect.extent.width, r->rect.extent.height}];
            [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
        [en endEncoding];
    }
}
static void clear_attachments(rs_t *s, const c_cleara *x) {
    MTLRenderPassDescriptor *d = s->rpd;
    if ((!s->re && !s->cut) || !d) { RED1("vkCmdClearAttachments outside a render pass"); return; }
    if (!x->n || !x->rects) return;
    const VkClearRect *rects = (const void *)(x->attachments + x->n);
    BOOL whole = s->rt_layers == 1 && d.renderTargetArrayLength <= 1;
    for (uint32_t i = 0; i < x->n; i++) {
        const VkClearAttachment *a = &x->attachments[i];
        NSUInteger color = a->colorAttachment;
        if (a->aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && color >= 8) { RED1("vkCmdClearAttachments color attachment exceeds the host limit"); return; }
        if (a->aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && !clear_covers_attachment(d.colorAttachments[color], rects, x->rects)) whole = NO;
        if ((a->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) && !clear_covers_attachment(d.depthAttachment, rects, x->rects)) whole = NO;
        if ((a->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) && !clear_covers_attachment(d.stencilAttachment, rects, x->rects)) whole = NO;
    }
    if (s->re) end_rp(s, 0);
    load_rp(s);
    if (!whole) {
        for (uint32_t i = 0; i < x->n; i++) clear_attachment_rects(s, &x->attachments[i], rects, x->rects);
        reopen_rp(s); return;
    }
    for (uint32_t i = 0; i < x->n; i++) {
        const VkClearAttachment *a = &x->attachments[i];
        if (a->aspectMask == VK_IMAGE_ASPECT_COLOR_BIT) {
            MTLRenderPassColorAttachmentDescriptor *color = d.colorAttachments[a->colorAttachment];
            if (!color.texture) continue;
            color.loadAction = MTLLoadActionClear;
            color.clearColor = fmt_clear_color(s->rt_color[a->colorAttachment], &a->clearValue.color);
        }
        if (a->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
            d.depthAttachment.loadAction = MTLLoadActionClear;
            d.depthAttachment.clearDepth = a->clearValue.depthStencil.depth;
        }
        if (a->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
            d.stencilAttachment.loadAction = MTLLoadActionClear;
            d.stencilAttachment.clearStencil = a->clearValue.depthStencil.stencil;
        }
    }
    reopen_rp(s);
}
static void replay_ops(rs_t *s, VkCommandBuffer cb) {
    const chdr *prev_cmd = s->cmd; const uint8_t *prev_end = s->cmd_end;
    s->cmd_end = cb->buf + cb->n;
    for (size_t o = 0; o < cb->n;) {
        const chdr *h = (const chdr *)(cb->buf + o); const void *c = h + 1; o += h->size; s->cmd = h;
        switch (h->op) {
            case C_BRP: begin_rp(s, c); break;
            case C_ERP: end_rp(s, 1); s->cut = 0; s->rpd = nil; break;
            case C_PIPE: { const c_pipe *x = c; if (x->compute) s->cpipe = x->p; else s->pipe = x->p; break; }
            case C_SETS: {
                const c_sets *x = c; uint32_t k = 0; __typeof__(s->set[0]) *sets = x->compute ? s->cset : s->set;
                for (uint32_t i = 0; i < x->n && x->first + i < 8; i++) {
                    uint32_t si = x->first + i; VkDescriptorSet ds = x->s[i]; sets[si].s = ds;
                    for (uint32_t j = 0; ds && j < ds->L->ndyn; j++, k++) if (j < 16) sets[si].dyn[j] = k < x->ndyn ? x->dyn[k] : 0;
                }
                break;
            }
            case C_DISPATCH: {   // Vulkan dispatches are outside render passes; Metal tracks hazards between passes, blits and compute encoders
                const c_dispatch *x = c; VkPipeline p = s->cpipe;
                if (!p) { g_n_skip_disp++; break; }
                if (!p->cpso) {
                    g_n_skip_disp++;
                    if (!p->told) { p->told = 1; LOG("[vk] [RED] dispatch skipped: compute pipeline %s was not built", p->keys[0] ? p->keys : "(none)"); }
                    break;
                }
                if (!x->b && (!x->x || !x->y || !x->z)) break;
                if (x->b && !x->b->mem) { RED1("indirect dispatch: the buffer has no bound memory"); g_n_skip_disp++; break; }
                if (s->re) { end_rp(s, 0); s->cut = 1; }
                end_blit(s);
                if (!s->ce) { s->ce = [s->cb computeCommandEncoder]; s->enc_id = ++g_enc_id; if (s->ce) g_n_comp_enc++; }
                if (!s->ce) { RED1("cannot create a compute encoder"); g_n_skip_disp++; break; }
                [s->ce setComputePipelineState:p->cpso];
                bind_res(s, p, p->cs, ST_COMP); bind_push(s, p, p->cs, ST_COMP);
                if (!bind_args(s, p, p->cs, ST_COMP)) { g_n_skip_disp++; break; }
                if (x->b) [s->ce dispatchThreadgroupsWithIndirectBuffer:mem_buf(x->b->mem) indirectBufferOffset:x->b->off + x->o threadsPerThreadgroup:p->wg];
                else [s->ce dispatchThreadgroups:MTLSizeMake(x->x, x->y, x->z) threadsPerThreadgroup:p->wg];
                g_n_disp++;
                break;
            }
            case C_DYN: {
                const c_dyn *x = c;
                if (x->what == 0) { memcpy(s->blend, x->v, sizeof s->blend); s->have_blend = 1; }
                else if (x->what == 1) { memcpy(s->bias, x->v, sizeof s->bias); s->have_bias = 1; }
                else { s->db[0] = x->v[0]; s->db[1] = x->v[1]; s->have_db = 1; }
                s->d_dirty = 1; break;
            }
            case C_DRAWIND: {
                const c_drawind *x = c;
                if (!x->count || !pre_draw(s)) break;
                if (!x->b->mem) { RED1("indirect draw: the buffer has no bound memory"); break; }
                if (x->indexed && (!s->ib || !s->ib->mem)) { RED1("index buffer not bound"); break; }
                if (s->pipe->fan) { fan_indirect(s, x); break; }
                id<MTLBuffer> ab = mem_buf(x->b->mem);
                for (uint32_t i = 0; i < x->count; i++) {
                    NSUInteger ao = x->b->off + x->o + (NSUInteger)i * x->stride;
                    if (x->indexed) [s->re drawIndexedPrimitives:s->pipe->prim indexType:s->it indexBuffer:mem_buf(s->ib->mem) indexBufferOffset:s->ib->off + s->ibo indirectBuffer:ab indirectBufferOffset:ao];
                    else [s->re drawPrimitives:s->pipe->prim indirectBuffer:ab indirectBufferOffset:ao];
                    g_n_draw++;
                }
                break;
            }
            case C_FILL: {   // Metal fills bytes: a 4-byte pattern whose bytes differ is copied from a buffer filled with it
                const c_fill *x = c; VkBuffer b = x->dst;
                if (!b->mem) { RED1("a filled buffer has no bound memory"); break; }
                VkDeviceSize n = x->size == VK_WHOLE_SIZE ? (b->size - x->off) & ~(VkDeviceSize)3 : x->size;
                if (!n) break;
                id<MTLBlitCommandEncoder> be = blit(s, h->op); id<MTLBuffer> db = mem_buf(b->mem); uint8_t b0 = x->data & 0xFF;
                if (x->data == b0 * 0x01010101u) [be fillBuffer:db range:NSMakeRange(b->off + x->off, n) value:b0];
                else {
                    id<MTLBuffer> t = [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared]; uint32_t *w = t.contents;
                    for (VkDeviceSize i = 0; i < n / 4; i++) w[i] = x->data;
                    [be copyFromBuffer:t sourceOffset:0 toBuffer:db destinationOffset:b->off + x->off size:n];
                }
                g_n_copy++;
                break;
            }
            case C_UPDATE: {   // the recorded bytes, copied in submission order from a buffer of their own
                const c_update *x = c;
                if (!x->dst->mem) { RED1("an updated buffer has no bound memory"); break; }
                if (!x->size) break;
                id<MTLBuffer> t = [g_dev newBufferWithBytes:x->data length:x->size options:MTLResourceStorageModeShared];
                if (!t) { RED1("cannot make a buffer for the %u bytes of a buffer update", x->size); break; }
                [blit(s, h->op) copyFromBuffer:t sourceOffset:0 toBuffer:mem_buf(x->dst->mem) destinationOffset:x->dst->off + x->off size:x->size];
                g_n_copy++;
                break;
            }
            case C_RESOLVE: resolve_image(s, c); break;
            case C_BLIT: blit_image(s, c); break;
            case C_CLEARA: clear_attachments(s, c); break;
            case C_BARRIER: split_for_transfer(s); break;
            case C_EV: { const c_ev *x = c; atomic_store(&x->e->val, x->set ? s->sq->v : FENCE_NONE); break; }
            case C_EXEC: { const c_exec *x = c; for (uint32_t i = 0; i < x->n; i++) if (x->cb[i]) replay_ops(s, x->cb[i]); break; }   // inside the primary's render pass, if it is in one
            case C_QCOPY: copy_queries(s, c); break;
            case C_VB: { const c_vb *x = c; for (uint32_t i = 0; i < x->n; i++) { s->vb[x->first + i] = x->b[i]; s->vbo[x->first + i] = x->o[i]; } break; }
            case C_IB: { const c_ib *x = c; s->ib = x->b; s->ibo = x->o; s->it = x->t == VK_INDEX_TYPE_UINT32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16; s->isz = x->t == VK_INDEX_TYPE_UINT32 ? 4 : 2; break; }
            case C_VP: {
                const c_vp *x = c; memcpy(s->vp + x->first, x->v, x->n * sizeof *x->v);
                s->have_vp |= ((1u << x->n) - 1) << x->first; s->vp_dirty = 1; break;
            }
            case C_SC: {
                const c_sc *x = c; memcpy(s->sc + x->first, x->r, x->n * sizeof *x->r);
                s->have_sc |= ((1u << x->n) - 1) << x->first; s->sc_dirty = 1; break;
            }
            case C_DRAWI: {
                const c_drawi *x = c;
                if (!x->count || !x->inst || !pre_draw(s)) break;
                if (!s->ib || !s->ib->mem) { RED1("index buffer not bound"); break; }
                if (s->pipe->fan) { fan_indexed(s, x); g_n_draw++; break; }
                [s->re drawIndexedPrimitives:s->pipe->prim indexCount:x->count indexType:s->it indexBuffer:mem_buf(s->ib->mem)
                           indexBufferOffset:s->ib->off + s->ibo + (VkDeviceSize)x->first * s->isz instanceCount:x->inst baseVertex:x->voff baseInstance:x->first_inst];
                g_n_draw++;
                break;
            }
            case C_COPYB: {
                const c_copyb *x = c; if (!x->src->mem || !x->dst->mem) { RED1("a copied buffer has no bound memory"); break; }
                id<MTLBlitCommandEncoder> be = blit(s, h->op); id<MTLBuffer> sb = mem_buf(x->src->mem), db = mem_buf(x->dst->mem);
                for (uint32_t i = 0; i < x->n; i++)
                    [be copyFromBuffer:sb sourceOffset:x->src->off + x->r[i].srcOffset toBuffer:db destinationOffset:x->dst->off + x->r[i].dstOffset size:x->r[i].size];
                g_n_copy += x->n;
                break;
            }
            case C_COPYB2I: {
                const c_copyb2i *x = c; VkImage im = x->dst; const fmt_t *f = im->f;
                if (!x->src->mem || !im->tex) { RED1("the upload buffer has no bound memory or the image was not created"); break; }
                id<MTLBlitCommandEncoder> be = blit(s, h->op); id<MTLBuffer> sb = mem_buf(x->src->mem);
                for (uint32_t i = 0; i < x->n; i++) {
                    const VkBufferImageCopy *r = &x->r[i];
                    uint32_t rl = r->bufferRowLength ? r->bufferRowLength : r->imageExtent.width, ih = r->bufferImageHeight ? r->bufferImageHeight : r->imageExtent.height;
                    NSUInteger bpr = (NSUInteger)((rl + f->bw - 1) / f->bw) * f->bytes, bpi = bpr * ((ih + f->bh - 1) / f->bh);
                    int is3d = im->type == VK_IMAGE_TYPE_3D;
                    MTLBlitOption opt = f->kind == K_DS ? (r->imageSubresource.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT ? MTLBlitOptionStencilFromDepthStencil : MTLBlitOptionDepthFromDepthStencil) : MTLBlitOptionNone;
                    for (uint32_t l = 0; l < (is3d ? 1 : r->imageSubresource.layerCount); l++)
                        [be copyFromBuffer:sb sourceOffset:x->src->off + r->bufferOffset + l * bpi sourceBytesPerRow:bpr sourceBytesPerImage:is3d || im->layers > 1 ? bpi : 0
                                sourceSize:MTLSizeMake(r->imageExtent.width, r->imageExtent.height, is3d ? r->imageExtent.depth : 1) toTexture:im->tex
                          destinationSlice:is3d ? 0 : r->imageSubresource.baseArrayLayer + l destinationLevel:r->imageSubresource.mipLevel
                         destinationOrigin:MTLOriginMake(r->imageOffset.x, r->imageOffset.y, is3d ? r->imageOffset.z : 0) options:opt];
                }
                g_n_copy += x->n;
                break;
            }
            case C_DRAW: {
                const c_draw *x = c;
                if (!x->count || !x->inst || !pre_draw(s)) break;
                if (s->pipe->fan) { fan_arrays(s, x); g_n_draw++; break; }
                [s->re drawPrimitives:s->pipe->prim vertexStart:x->first vertexCount:x->count instanceCount:x->inst baseInstance:x->first_inst];
                g_n_draw++;
                break;
            }
            case C_PUSH: {
                const c_push *x = c;
                const VkShaderStageFlags stages[] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_COMPUTE_BIT};
                for (int st = 0; st < 3; st++) if (x->stages & stages[st]) memcpy(s->pc[st] + x->off, x->data, x->size);
                break;
            }
            case C_COPYI: {
                const c_copyi *x = c; VkImage a = x->src, b = x->dst;
                if (!a->tex || !b->tex) { RED1("a copied image was not created"); break; }
                if (a->tex.pixelFormat != b->tex.pixelFormat) { RED1("image copy between different formats (%d → %d) not implemented", a->fmt, b->fmt); break; }
                id<MTLBlitCommandEncoder> be = blit(s, h->op);
                for (uint32_t i = 0; i < x->n; i++) {
                    const VkImageCopy *r = &x->r[i]; int a3 = a->type == VK_IMAGE_TYPE_3D, b3 = b->type == VK_IMAGE_TYPE_3D;
                    uint32_t nl = a3 ? 1 : r->srcSubresource.layerCount;
                    for (uint32_t l = 0; l < nl; l++)
                        [be copyFromTexture:a->tex sourceSlice:a3 ? 0 : r->srcSubresource.baseArrayLayer + l sourceLevel:r->srcSubresource.mipLevel
                               sourceOrigin:MTLOriginMake(r->srcOffset.x, r->srcOffset.y, a3 ? r->srcOffset.z : 0)
                                 sourceSize:MTLSizeMake(r->extent.width, r->extent.height, a3 ? r->extent.depth : 1) toTexture:b->tex
                           destinationSlice:b3 ? 0 : r->dstSubresource.baseArrayLayer + l destinationLevel:r->dstSubresource.mipLevel
                          destinationOrigin:MTLOriginMake(r->dstOffset.x, r->dstOffset.y, b3 ? r->dstOffset.z : 0)];
                }
                g_n_copy += x->n;
                break;
            }
            case C_COPYI2B: {   // read-back into a buffer (shared storage: the CPU sees it once the submit's fence has signalled)
                const c_copyi2b *x = c; VkImage im = x->src; const fmt_t *f = im->f;
                if (!x->dst->mem || !im->tex) { RED1("the readback buffer has no bound memory or the image was not created"); break; }
                id<MTLBlitCommandEncoder> be = blit(s, h->op); id<MTLBuffer> db = mem_buf(x->dst->mem);
                for (uint32_t i = 0; i < x->n; i++) {
                    const VkBufferImageCopy *r = &x->r[i]; int is3d = im->type == VK_IMAGE_TYPE_3D;
                    uint32_t rl = r->bufferRowLength ? r->bufferRowLength : r->imageExtent.width, ih = r->bufferImageHeight ? r->bufferImageHeight : r->imageExtent.height;
                    MTLBlitOption opt = MTLBlitOptionNone; NSUInteger tb = f->bytes;
                    if (f->kind == K_DS) {   // one aspect: Metal writes 4-byte depth or 1-byte stencil, as Vulkan's buffer layout for that aspect
                        int st = r->imageSubresource.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT;
                        opt = st ? MTLBlitOptionStencilFromDepthStencil : MTLBlitOptionDepthFromDepthStencil; tb = st ? 1 : 4;
                    }
                    NSUInteger bpr = f->kind == K_BLOCK ? (NSUInteger)((rl + f->bw - 1) / f->bw) * f->bytes : rl * tb, bpi = bpr * ((ih + f->bh - 1) / f->bh);
                    for (uint32_t l = 0; l < (is3d ? 1 : r->imageSubresource.layerCount); l++)
                        [be copyFromTexture:im->tex sourceSlice:is3d ? 0 : r->imageSubresource.baseArrayLayer + l sourceLevel:r->imageSubresource.mipLevel
                               sourceOrigin:MTLOriginMake(r->imageOffset.x, r->imageOffset.y, is3d ? r->imageOffset.z : 0)
                                 sourceSize:MTLSizeMake(r->imageExtent.width, r->imageExtent.height, is3d ? r->imageExtent.depth : 1)
                                   toBuffer:db destinationOffset:x->dst->off + r->bufferOffset + l * bpi destinationBytesPerRow:bpr destinationBytesPerImage:bpi options:opt];
                }
                g_n_copy += x->n;
                break;
            }
            case C_QRESET: { const c_query *x = c; for (uint32_t i = 0; i < x->n && x->first + i < x->q->n; i++) x->q->ticket[x->first + i] = FENCE_NONE; break; }
            case C_QTS: {
                const c_query *x = c;
                if (x->first < x->q->n) {
                    x->q->ticket[x->first] = s->sq->v;
                    query_record(s->sq, x->q, x->first, QUERY_TIMESTAMP);
                }
                break;
            }
            case C_QBEGIN: {
                const c_query *x = c; subq_t *q = s->sq;
                if (!s->re && s->cut) resume_rp(s);
                if (x->q->type != VK_QUERY_TYPE_OCCLUSION) { RED1("query type %d not implemented", x->q->type); break; }
                if (!q->vis || x->first >= x->q->n) { RED1("occlusion query past its pool"); break; }
                s->aq = x->q; s->aq_first = x->first; x->q->ticket[x->first] = FENCE_NONE;
                query_record(q, x->q, x->first, QUERY_START);
                occ_start(s);
                break;
            }
            case C_STENCIL: { const c_stencil *x = c; if (x->faces & VK_STENCIL_FACE_FRONT_BIT) s->sdyn[x->what][0] = x->v; if (x->faces & VK_STENCIL_FACE_BACK_BIT) s->sdyn[x->what][1] = x->v;
                              s->sdyn_set[x->what] = 1; s->s_dirty = 1; break; }
            case C_CLEARI: {   // an image cleared outside a render pass (Super Mario 3D All-Stars' Super Mario 64 clears its depth buffer so): a pass per level and layer that only clears
                const c_cleari *x = c; VkImage im = x->im;
                if (!im || !im->tex) { RED1("a cleared image was not created"); break; }
                if (s->re) { end_rp(s, 0); s->cut = 1; }
                end_blit(s); end_comp(s);
                for (uint32_t i = 0; i < x->n; i++) {
                    const VkImageSubresourceRange *r = &x->r[i];
                    uint32_t nm = r->levelCount == VK_REMAINING_MIP_LEVELS ? im->mips - r->baseMipLevel : r->levelCount;
                    uint32_t nl = im->type == VK_IMAGE_TYPE_3D ? 1 : r->layerCount == VK_REMAINING_ARRAY_LAYERS ? im->layers - r->baseArrayLayer : r->layerCount;
                    for (uint32_t m = 0; m < nm; m++)
                        for (uint32_t l = 0; l < nl; l++) {
                            MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
                            if (!x->ds) {
                                MTLRenderPassColorAttachmentDescriptor *a = d.colorAttachments[0]; a.texture = im->tex; a.level = r->baseMipLevel + m; a.slice = r->baseArrayLayer + l;
                                a.loadAction = MTLLoadActionClear; a.storeAction = MTLStoreActionStore;
                                a.clearColor = fmt_clear_color(im->f, &x->v.color);
                            } else {
                                if (fmt_has_depth(im->f) && (r->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)) {
                                    MTLRenderPassDepthAttachmentDescriptor *a = d.depthAttachment; a.texture = im->tex; a.level = r->baseMipLevel + m; a.slice = r->baseArrayLayer + l;
                                    a.loadAction = MTLLoadActionClear; a.storeAction = MTLStoreActionStore; a.clearDepth = x->v.depthStencil.depth;
                                }
                                if (fmt_has_stencil(im->f) && (r->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT)) {
                                    MTLRenderPassStencilAttachmentDescriptor *a = d.stencilAttachment; a.texture = im->tex; a.level = r->baseMipLevel + m; a.slice = r->baseArrayLayer + l;
                                    a.loadAction = MTLLoadActionClear; a.storeAction = MTLStoreActionStore; a.clearStencil = x->v.depthStencil.stencil;
                                }
                                if (!d.depthAttachment.texture && !d.stencilAttachment.texture) continue;
                            }
                            [[s->cb renderCommandEncoderWithDescriptor:d] endEncoding];
                        }
                }
                break;
            }
            case C_QEND: {
                if (s->aq) s->aq->ticket[s->aq_first] = s->sq->v;
                s->aq = NULL;
                if (s->re) [s->re setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
                break;
            }
            default: LOG("[vk] [RED] unknown command %u in the command buffer", h->op); o = cb->n; break;
        }
    }
    s->cmd = prev_cmd; s->cmd_end = prev_end;
}
static void replay(rs_t *s, VkCommandBuffer cb) {
    replay_ops(s, cb);
    if (s->re || s->cut) { RED1("render pass not ended when the command buffer ended"); end_rp(s, 1); s->cut = 0; }
    end_blit(s); end_comp(s);
    free(s->fj); s->fj = NULL; s->nfj = s->fjcap = 0; s->fan_out = nil;
}
// Per command buffer, before it is replayed: occlusion queries begun and the encoders a query may span (a pass, or what splits a pass's encoder), secondary buffers included
static void count_ops(VkCommandBuffer c, uint32_t *cap, uint32_t *nsplit, int depth) {
    for (size_t o = 0; o < c->n; o += ((const chdr *)(c->buf + o))->size) {
        const chdr *h = (const chdr *)(c->buf + o); uint32_t op = h->op;
        *cap += op == C_QBEGIN;
        *nsplit += op == C_BRP || op == C_COPYB || op == C_COPYB2I || op == C_COPYI || op == C_COPYI2B || op == C_CLEARI || op == C_DISPATCH ||
                   op == C_FILL || op == C_UPDATE || op == C_RESOLVE || op == C_BLIT || op == C_QCOPY || op == C_DRAWIND || op == C_EXEC || op == C_BARRIER || op == C_CLEARA;
        if (op == C_EXEC && depth < 2) { const c_exec *x = (const void *)(h + 1); for (uint32_t i = 0; i < x->n; i++) if (x->cb[i]) count_ops(x->cb[i], cap, nsplit, depth + 1); }
    }
}

// ---------- Queue, fences, semaphores ----------
static int fence_done(VkFence f) { uint64_t v = atomic_load(&f->val); return v != FENCE_NONE && atomic_load(&g_done) >= v; }
static void done_advance(uint64_t v) {
    pthread_mutex_lock(&g_done_mu);
    if (v > atomic_load(&g_done)) atomic_store(&g_done, v);
    pthread_cond_broadcast(&g_done_cv);
    pthread_mutex_unlock(&g_done_mu);
}
static uint64_t submit_segment(id<MTLCommandBuffer> cb, subq_t *q, VkFence fence, int last) {
    uint64_t v = ++g_ev_val;
    [cb encodeSignalEvent:g_ev value:v];
    if (fence) atomic_store(&fence->val, v);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
        if (c.error) { static int k; if (k++ < 20) LOG("[vk] [RED] command buffer error (submission %llu): %s", (unsigned long long)v, c.error.localizedDescription.UTF8String); }
        uint64_t at = now_ns();
        const uint64_t *visibility = q->vis.contents;
        for (uint32_t i = 0; i < q->nwrites; i++) {
            const query_write *w = &q->writes[i];
            if (w->slot == QUERY_TIMESTAMP) w->q->val[w->idx] = at;
            else if (w->slot == QUERY_START) w->q->val[w->idx] = 0;
            else w->q->val[w->idx] += visibility[w->slot];
        }
        if (last) { q->vis = nil; free(q->writes); free(q); }
        done_advance(v);   // publish completion only after the query writes
    }];
    [cb commit];
    return v;
}

static void split_submit(rs_t *s) {
    if (s->re) { end_rp(s, 0); s->cut = 1; }
    end_blit(s); end_comp(s);
    wait_done(submit_segment(s->cb, s->sq, NULL, 0));
    s->sq->nwrites = 0; s->sq->v = g_ev_val + 1;
    s->cb = [g_q commandBuffer];
}

VKB(vkQueueSubmit, VkResult, VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f) {
    (void)q; dm_gfx_enter();   // blocks while the app is in the background (iOS allows no GPU work there)
    @autoreleasepool {
        conv_sync();
        pthread_mutex_lock(&g_qmu);
        id<MTLCommandBuffer> cb = [g_q commandBuffer];
        subq_t *sq = zalloc(sizeof *sq); sq->v = g_ev_val + 1; uint32_t nsplit = 0;
        for (uint32_t i = 0; i < n; i++)   // occlusion queries need a visibility buffer before the first render pass is encoded
            for (uint32_t j = 0; j < si[i].commandBufferCount; j++) count_ops(si[i].pCommandBuffers[j], &sq->cap, &nsplit, 0);
        if (sq->cap) {
            sq->cap += nsplit;
            sq->vis = [g_dev newBufferWithLength:sq->cap * 8 options:MTLResourceStorageModeShared]; memset(sq->vis.contents, 0, sq->cap * 8);
        }
        for (uint32_t i = 0; i < n; i++)
            for (uint32_t j = 0; j < si[i].commandBufferCount; j++) {
                rs_t s = {0}; s.cb = cb; s.sq = sq;
                replay(&s, si[i].pCommandBuffers[j]); cb = s.cb;
            }
        submit_segment(cb, sq, f, 1);
        g_n_submit++;
        pthread_mutex_unlock(&g_qmu);
    }
    dm_gfx_leave();
    return VK_SUCCESS;
}
VK(vkQueueBindSparse, VkResult, VkQueue q, uint32_t n, const VkBindSparseInfo *bi, VkFence f) {
    (void)q; (void)bi; if (n) RED1("sparse binding not implemented");
    if (f) atomic_store(&f->val, 0);
    return n ? VK_ERROR_FEATURE_NOT_PRESENT : VK_SUCCESS;
}
static void wait_done(uint64_t v) {
    pthread_mutex_lock(&g_done_mu);
    while (atomic_load(&g_done) < v) pthread_cond_wait(&g_done_cv, &g_done_mu);
    pthread_mutex_unlock(&g_done_mu);
}
void dm_vk_drain(void) { pthread_mutex_lock(&g_qmu); uint64_t v = g_ev_val; pthread_mutex_unlock(&g_qmu); wait_done(v); }   // dm_gfx_suspend (hle_nvn.c)
VKB(vkQueueWaitIdle, VkResult, VkQueue q) { (void)q; pthread_mutex_lock(&g_qmu); uint64_t v = g_ev_val; pthread_mutex_unlock(&g_qmu); wait_done(v); return VK_SUCCESS; }
VKB(vkDeviceWaitIdle, VkResult, VkDevice d) { (void)d; return h_vkQueueWaitIdle(&g_queue); }
VK(vkCreateFence, VkResult, VkDevice d, const VkFenceCreateInfo *ci, const VkAllocationCallbacks *a, VkFence *out) {
    (void)d; (void)a; VkFence f = zalloc(sizeof *f); atomic_store(&f->val, ci->flags & VK_FENCE_CREATE_SIGNALED_BIT ? 0 : FENCE_NONE); *out = f; return VK_SUCCESS;
}
VK(vkDestroyFence, void, VkDevice d, VkFence f, const VkAllocationCallbacks *a) { (void)d; (void)a; free(f); }
VK(vkResetFences, VkResult, VkDevice d, uint32_t n, const VkFence *f) { (void)d; for (uint32_t i = 0; i < n; i++) atomic_store(&f[i]->val, FENCE_NONE); return VK_SUCCESS; }
VK(vkGetFenceStatus, VkResult, VkDevice d, VkFence f) { (void)d; return fence_done(f) ? VK_SUCCESS : VK_NOT_READY; }
VKB(vkWaitForFences, VkResult, VkDevice d, uint32_t n, const VkFence *fs, VkBool32 all, uint64_t timeout) {
    (void)d; uint64_t t0 = now_ns(); int warned = 0;
    pthread_mutex_lock(&g_done_mu);
    for (;;) {
        uint32_t k = 0; for (uint32_t i = 0; i < n; i++) k += fence_done(fs[i]);
        if (all ? k == n : k > 0) { pthread_mutex_unlock(&g_done_mu); return VK_SUCCESS; }
        uint64_t el = now_ns() - t0;
        if (el >= timeout) { pthread_mutex_unlock(&g_done_mu); return VK_TIMEOUT; }
        if (!warned && el > 5000000000ull) { warned = 1; LOG("[vk] [RED] fence wait over 5 s (%u fences, %u done; a fence never submitted never completes)", n, k); }
        uint64_t left = timeout - el, step = left < 100000000ull ? left : 100000000ull;
        struct timespec ts = {(time_t)(step / 1000000000ull), (long)(step % 1000000000ull)};
        pthread_cond_timedwait_relative_np(&g_done_cv, &g_done_mu, &ts);
    }
}
VK(vkCreateQueryPool, VkResult, VkDevice d, const VkQueryPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkQueryPool *out) {
    (void)d; (void)a;
    if (ci->queryType != VK_QUERY_TYPE_TIMESTAMP && ci->queryType != VK_QUERY_TYPE_OCCLUSION) LOG("[vk] [RED] query pool type %d not implemented", ci->queryType);
    VkQueryPool q = zalloc(sizeof *q); q->type = ci->queryType; q->n = ci->queryCount;
    q->val = zalloc(umax(1, q->n) * sizeof *q->val); q->ticket = zalloc(umax(1, q->n) * sizeof *q->ticket);
    for (uint32_t i = 0; i < q->n; i++) q->ticket[i] = FENCE_NONE;
    VLOG("query pool %p: type %d, %u queries", (void *)q, q->type, q->n);
    *out = q; return VK_SUCCESS;
}
VK(vkDestroyQueryPool, void, VkDevice d, VkQueryPool q, const VkAllocationCallbacks *a) { (void)d; (void)a; if (q) { free(q->val); free(q->ticket); free(q); } }
VKB(vkGetQueryPoolResults, VkResult, VkDevice d, VkQueryPool q, uint32_t first, uint32_t n, size_t size, void *data, VkDeviceSize stride, VkQueryResultFlags fl) {
    (void)d; VkResult res = VK_SUCCESS; int w64 = fl & VK_QUERY_RESULT_64_BIT, av = fl & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;
    for (uint32_t i = 0; i < n && first + i < q->n; i++) {
        uint32_t k = first + i; uint64_t t = q->ticket[k];
        if (t != FENCE_NONE && (fl & VK_QUERY_RESULT_WAIT_BIT)) wait_done(t);   // a query no submit has written yet stays unavailable instead of waiting forever
        int ok = t != FENCE_NONE && atomic_load(&g_done) >= t;
        uint8_t *p = (uint8_t *)data + (size_t)i * stride; if ((size_t)(p - (uint8_t *)data) + (w64 ? 8 : 4) * (av ? 2 : 1) > size) break;
        write_query_result(p, ok ? q->val[k] : 0, ok, fl);
        if (!ok) res = VK_NOT_READY;
    }
    return res;
}
VK(vkCreateSemaphore, VkResult, VkDevice d, const VkSemaphoreCreateInfo *ci, const VkAllocationCallbacks *a, VkSemaphore *out) {
    (void)d; (void)ci; (void)a; *out = zalloc(sizeof **out); return VK_SUCCESS;   // every submit runs in order on one queue: waits are already satisfied
}
VK(vkDestroySemaphore, void, VkDevice d, VkSemaphore s, const VkAllocationCallbacks *a) { (void)d; (void)a; free(s); }

// ---------- Swapchain and present ----------
VKB(vkCreateSwapchainKHR, VkResult, VkDevice d, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSwapchainKHR *out) {
    (void)d; (void)a; dev_init();
    const fmt_t *f = fmt_of(ci->imageFormat);
    if (!f || f->pf == MTLPixelFormatInvalid) { LOG("[vk] [RED] swap chain format %d not mapped", ci->imageFormat); return VK_ERROR_FORMAT_NOT_SUPPORTED; }
    VkSwapchainKHR sc = zalloc(sizeof *sc); sc->n = umin(umax(ci->minImageCount, 2), 4);
    @autoreleasepool {
        for (uint32_t i = 0; i < sc->n; i++) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f->pf width:ci->imageExtent.width height:ci->imageExtent.height mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | (ci->imageUsage & VK_IMAGE_USAGE_STORAGE_BIT ? MTLTextureUsageShaderWrite : 0);
            td.storageMode = MTLStorageModePrivate;
            VkImage im = zalloc(sizeof *im); im->swap = 1;
            im->tex = [g_dev newTextureWithDescriptor:td]; im->f = f; im->fmt = ci->imageFormat; im->type = VK_IMAGE_TYPE_2D;
            im->usage = ci->imageUsage;
            im->w = ci->imageExtent.width; im->h = ci->imageExtent.height; im->d = im->mips = im->layers = im->samples = 1;
            im->serial = mtl_ext_tex((__bridge void *)im->tex);
            sc->img[i] = im;
        }
    }
    VLOG("swap chain %ux%u format %d, %u images (at least %u requested), present mode %d", ci->imageExtent.width, ci->imageExtent.height, ci->imageFormat, sc->n, ci->minImageCount, ci->presentMode);
    *out = sc; return VK_SUCCESS;
}
VKB(vkDestroySwapchainKHR, void, VkDevice d, VkSwapchainKHR sc, const VkAllocationCallbacks *a) {
    (void)d; (void)a; if (!sc) return;
    for (uint32_t i = 0; i < sc->n; i++) { if (sc->ticket[i]) mtl_present_wait(sc->ticket[i]); }
    // The present path keeps each texture until its presents have completed on the GPU, then lets it go (mtl_ext_tex_release); kept for good before, at every recreation
    for (uint32_t i = 0; i < sc->n; i++) { mtl_ext_tex_release(sc->img[i]->serial); sc->img[i]->tex = nil; free(sc->img[i]); }
    free(sc);
}
VK(vkGetSwapchainImagesKHR, VkResult, VkDevice d, VkSwapchainKHR sc, uint32_t *n, VkImage *out) {
    (void)d; if (!out) { *n = sc->n; return VK_SUCCESS; }
    uint32_t k = umin(*n, sc->n); memcpy(out, sc->img, k * sizeof *out); *n = k; return k < sc->n ? VK_INCOMPLETE : VK_SUCCESS;
}
static uint64_t g_next_acq;   // pacing without the display link: the earliest time of the next acquire (one frame period after the previous one)
VKB(vkAcquireNextImageKHR, VkResult, VkDevice d, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, VkFence f, uint32_t *idx) {
    (void)d; (void)timeout; (void)sem;
    uint32_t i = sc->next++ % sc->n;
    uint64_t period = 1000000000ull / (uint64_t)mtl_fps_target(), now = now_ns();
    // Paced by the display link when it runs, as NVN's acquire (hle_nvn.c nvnWindowAcquireTexture): FIFO presents one frame per vsync. A clock of our own drifts against the screen:
    // Super Mario Galaxy 2 on an iPhone put two frames into some display-link ticks and none into others (about one tick in seven), and on a Mac window it ran at 30 fps
    if (mtl_link_active() && mtl_link_wait_tick((int)(60 / mtl_fps_target()), 2 * period)) g_next_acq = now_ns() + period;
    else {
        if (g_next_acq > now) { uint64_t w = g_next_acq - now; struct timespec ts = {(time_t)(w / 1000000000ull), (long)(w % 1000000000ull)}; nanosleep(&ts, NULL); }
        now = now_ns(); g_next_acq = g_next_acq && now < g_next_acq + period ? g_next_acq + period : now + period;
    }
    if (sc->ticket[i]) mtl_present_wait(sc->ticket[i]);   // its previous present has been shown, so drawing into it again is safe
    if (f) atomic_store(&f->val, 0);
    *idx = i; return VK_SUCCESS;
}
static uint64_t g_frames;
static void frame_end(void) {
    static uint64_t pd, pp, ps, pc, pdisp, pq, psdisp;
    uint64_t f = ++g_frames, dr = g_n_draw, pa = g_n_pass, su = g_n_submit, co = g_n_copy, di = g_n_disp, sk = g_n_skip, sd = g_n_skip_disp;
    if (f <= 3 || f % 300 == 0)
        VLOG("frame %llu: draws %llu (skipped %llu), dispatches %llu (skipped %llu), passes %llu, submits %llu, copies %llu (since the last record, %llu frames); pipelines so far %llu, buffer memory %.1f MB",
             (unsigned long long)f, (unsigned long long)(dr - pd), (unsigned long long)(sk - pq), (unsigned long long)(di - pdisp), (unsigned long long)(sd - psdisp),
             (unsigned long long)(pa - pp), (unsigned long long)(su - ps), (unsigned long long)(co - pc), (unsigned long long)(f <= 3 ? 1 : 300), (unsigned long long)g_n_pso, g_mem_bytes / 1048576.0);
    if (f <= 3 || f % 300 == 0) { pd = dr; pp = pa; ps = su; pc = co; pdisp = di; pq = sk; psdisp = sd; }
    dm_guest_frame_tick();
}
VKB(vkQueuePresentKHR, VkResult, VkQueue q, const VkPresentInfoKHR *pi) {
    (void)q; dm_gfx_enter();
    pthread_mutex_lock(&g_qmu); uint64_t v = g_ev_val; pthread_mutex_unlock(&g_qmu);
    int crop[4]; dm_vi_layer_crop(crop);   // the vi layer crop (hle_vi.c): Super Mario Galaxy shows the top-left 1280x720 of its 1920x1080 swapchain in handheld mode
    for (uint32_t i = 0; i < pi->swapchainCount; i++) {
        VkSwapchainKHR sc = pi->pSwapchains[i]; uint32_t k = pi->pImageIndices[i];
        int fit = crop[2] > 0 && crop[3] > 0 && k < sc->n && (uint32_t)(crop[0] + crop[2]) <= sc->img[k]->w && (uint32_t)(crop[1] + crop[3]) <= sc->img[k]->h;
        if (k < sc->n) sc->ticket[k] = mtl_present_ext(sc->img[k]->serial, 1.0 / mtl_fps_target(), (__bridge void *)g_ev, v, fit ? crop : NULL);
        if (pi->pResults) pi->pResults[i] = VK_SUCCESS;
    }
    dm_gfx_leave();
    frame_end();
    return VK_SUCCESS;
}

// ---------- Function lookup ----------
static void *vk_lookup(const char *name) {
    static const hle_ent *sec; static unsigned long sz;
    if (!name || strncmp(name, "vk", 2)) return NULL;
    if (!sec) sec = (const hle_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swhle", &sz);
    void *fn = NULL; size_t n = strlen(name);
    for (int pass = 0; pass < 2 && !fn; pass++) {
        size_t len = pass ? n - 3 : n;
        if (pass && (n < 4 || strcmp(name + n - 3, "KHR"))) break;   // second pass: an extension function promoted to core under the name without KHR
        for (unsigned long i = 0; i < sz / sizeof *sec && !fn; i++) if (!strncmp(sec[i].name, name, len) && !sec[i].name[len]) fn = sec[i].fn;
    }
    static const char *seen[512]; static int nseen; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    int known = 0; for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], name)) { known = 1; break; }
    if (!known && nseen < 512) { seen[nseen++] = strdup(name); VLOG("lookup %s → %s", name, fn ? "host implementation" : "none (returns null)"); }
    pthread_mutex_unlock(&mu);
    return fn;
}
VK(vkGetInstanceProcAddr, PFN_vkVoidFunction, VkInstance i, const char *name) { (void)i; return (PFN_vkVoidFunction)vk_lookup(name); }
VK(vkGetDeviceProcAddr, PFN_vkVoidFunction, VkDevice d, const char *name) { (void)d; return (PFN_vkVoidFunction)vk_lookup(name); }
