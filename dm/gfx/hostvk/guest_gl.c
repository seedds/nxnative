// The guest OpenGL layer on the Vulkan backend (Windows and Linux; docs/design/vulkan-backend.md 8.2, step P9b): for the games that render with NVIDIA's desktop OpenGL 4.5
// core profile through EGL (Digimon Story Cyber Sleuth: Complete Edition, Media.Vision's MVGL engine; Balatro, LÖVE 11.4 on SDL), the state machine of dm/gfx/gl/hle_gl.m,
// which stays the Mac's and is this layer's specification of the game-facing behaviour, with a Vulkan encoder over the backend's device (be_native_device: vk_device.c) in
// place of its Metal one (decision D5, option 1). The game's EGL and GL imports bind to the functions here through the host's import table (the GL / EGL macros below, as
// guest_vk.c's VK), and eglGetProcAddress looks GL names up in the same table. The census is hle_gl.m's (every name the two games call: a glad loader asks for 2,956 names);
// every other name gets a null pointer, DM_GL_STUBS=1 a stub that reports its first call.
// Model (hle_gl.m's, in Vulkan terms):
//   · one context, used from the thread that made it current; GL calls are recorded as they come into the frame's command buffer, with a second one submitted before it for
//     the uploads into textures the frame has not read yet; both go to the backend's queue at eglSwapBuffers and signal the layer's timeline semaphore with the frame's serial.
//     Dynamic rendering, one pass per stretch of draws into one framebuffer, a full barrier between operations (the backend's rule), every image in GENERAL;
//   · GL's orientation: images are stored bottom row first, as hle_gl.m stores them. Vulkan's viewport maps NDC y = −1 to the first row, so GL's viewport and scissor
//     rectangles are Vulkan's as given (a positive height: what the NVN backend's lower-left origin uses, vk_commands.c) and gl_FragCoord needs no change; the winding is
//     reversed (Vulkan computes it with y down), and the present flips (be_set_origin_lower_left: the presenter copies the rows bottom up, vk_backend.c);
//   · GL's clip z (−w to w) becomes Vulkan's (0 to w) in the shaders (gl_shaders.py --vk), as --fixup-clipspace does for the Metal library;
//   · the window surface is three images registered with be_ext_tex, plus a depth / stencil image; eglSwapBuffers hands the current one to be_present_ext with the frame's
//     timeline value and paces by the swap interval, as hle_gl.m does;
//   · buffers live in host-visible coherent memory mapped for good (up to 256 KiB from 4 MiB slabs, larger ones with an allocation of their own): glBufferData on a buffer the
//     GPU may still read gets new memory (orphaning), glBufferSubData copies it first; glBufferStorage + glMapBufferRange hand the game the mapping itself (LÖVE's persistently
//     mapped streams), and glFenceSync / glClientWaitSync are values of the timeline (waiting on a fence of the frame being recorded submits it first);
//   · textures are made on first use from the levels given so far (Vulkan, as Metal, needs the level count up front), device-local; uploads go through transient host-visible
//     memory with a copy in command order;
//   · shaders come from the SPIR-V library gl_shaders.py --vk writes (<DM_MSL_DIR>/gl-vk/): a shader is found by the hash of its source and its stage; the table lists its
//     vertex inputs (locations), the members of its default uniform block (glUniform* writes CPU copies, copied at a draw into transient memory with the device's uniform
//     alignment and bound as a dynamic uniform buffer) and its combined samplers. The vertex stage's resources are descriptor set 0, the fragment stage's set 1. Each vertex input
//     is a vertex binding of its own; glGetAttribLocation returns the input's location (or glBindAttribLocation's index, mapped at the link). Varyings are matched by name at the
//     link, as GL links them: each fragment input takes the location of the vertex output of its name.
// Declared with a red line: what hle_gl.m declares (multisampled renderbuffers made with one sample, more than one colour attachment, client-memory vertex arrays, pbuffer
// surfaces, shared contexts, glBufferSubData into an immutable buffer in flight, ...), and what Vulkan cannot give or this layer does not do: a vertex format the device cannot
// fetch, a 32-bit integer attribute read as float (Vulkan has no 32-bit scaled format), divisors above 1 without VK_KHR_vertex_attribute_divisor, GL_TEXTURE_BASE_LEVEL above 0.
// Native here where Metal lacked it (hle_gl.m declares these): GL_TEXTURE_LOD_BIAS and MIN / MAX_LOD, culling front and back, glPolygonMode, swizzles on textures that are
// render targets (a view of each kind), GL_FRAMEBUFFER_SRGB (an sRGB attachment is written through a UNORM view unless it is on, as desktop GL does), glBlitFramebuffer with
// scaling, flipping and depth / stencil, a synchronized glMapBufferRange that waits for the GPU.
// Environment: DM_GL_LOG=1 logs object creation; DM_GL_CAPTURE=<dir> writes every shader source the game compiles (<key>.vert / <key>.frag, read by gl_shaders.py);
// DM_GL_STUBS=1 (above).
#include "../../host/hle.h"
#include "vk_backend.h"
#include "vk_pso.h"
#include "guest_vk.h"
#include "guest_gl.h"
#include "../nvn/nvn_be_ext.h"   // the backend's present path and its pacing: be_ext_tex, be_present_ext, be_present_wait, be_vsync_active, be_vsync_wait, be_set_origin_lower_left
#include "../tex/astc_decode.h"
#include <math.h>
#include <stdatomic.h>
#include <stddef.h>
#if defined(_WIN32)
#include <process.h>   // getpid
#else
#include <unistd.h>
#endif

void dm_vi_layer_size(int *w, int *h);                          // dm/gfx/hle_vi.c (part of nxhost): the size of the last sized vi::CreateLayer, 0 x 0 for none
const void *plat_table_swhle(size_t *bytes);                    // dm/host/hle.c (PLAT_TABLE_DEFINE): the swhle entries of every object file

#define GLOG(...) LOG("[gl] " __VA_ARGS__)
#define RED1(...) do { static _Atomic int once_; if (!atomic_exchange(&once_, 1)) LOG("[gl] [RED] " __VA_ARGS__); } while (0)
static int g_log;
#define OLOG(...) do { if (g_log) GLOG(__VA_ARGS__); } while (0)

typedef uint32_t GLenum, GLuint, GLbitfield; typedef int32_t GLint, GLsizei; typedef float GLfloat; typedef double GLdouble; typedef char GLchar; typedef intptr_t GLsizeiptr, GLintptr;
typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface; typedef int32_t EGLint; typedef uint32_t EGLBoolean, EGLenum;
typedef void (*__eglMustCastToProperFunctionPointerType)(void);   // eglGetProcAddress's return type (EGL 1.5): a function, so its thunk hands the guest a guest-visible address

// GL: one GL function, registered in the host's import table (PLAT_TABLE_ENTRY(swhle), plat.h) for both direct imports and eglGetProcAddress. Its C function h_<name> has
// HLE_LINKAGE (hle.h): external here, so the host-call thunks generated from these entries can call it (dm/host/tools/gen_hle_thunks.py reads GL( and EGL( as entries named
// <name>, C function h_<name>, as it reads guest_vk.c's VK(). GLboolean and other arguments narrower than 32 bits are taken as uint32_t and truncated here (hle.h: Switch
// callers do not extend them). GLB: hle_gl.m's entries that run on the thread's big host stack; without native execution the engine calls host code on the host thread's own
// stack (hle.h, HLE_FORCE_BIG), so it is GL. EGL: the EGL entries, a macro of their own so the generator counts them apart
#define GL(name, ret, ...)                                                                        \
    HLE_LINKAGE ret h_##name(__VA_ARGS__);                                                        \
    PLAT_TABLE_ENTRY(swhle) static const hle_ent e_##name = {#name, (void *)h_##name};            \
    HLE_LINKAGE ret h_##name(__VA_ARGS__)
#define GLB GL
#define EGL(name, ret, ...)                                                                       \
    HLE_LINKAGE ret h_##name(__VA_ARGS__);                                                        \
    PLAT_TABLE_ENTRY(swhle) static const hle_ent e_##name = {#name, (void *)h_##name};            \
    HLE_LINKAGE ret h_##name(__VA_ARGS__)

// ---------- GL enums (hle_gl.m's) ----------
enum {
    GL_POINTS = 0, GL_LINES, GL_LINE_LOOP, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN, GL_QUADS,
    GL_NEVER = 0x200, GL_SRC_COLOR = 0x300, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR,
    GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE,
    GL_FRONT = 0x404, GL_BACK = 0x405, GL_FRONT_AND_BACK = 0x408,
    GL_CULL_FACE = 0xb44, GL_DEPTH_TEST = 0xb71, GL_STENCIL_TEST = 0xb90, GL_VIEWPORT = 0xba2, GL_BLEND = 0xbe2, GL_SCISSOR_TEST = 0xc11,
    GL_UNPACK_ROW_LENGTH = 0xcf2, GL_UNPACK_ALIGNMENT = 0xcf5, GL_MAX_TEXTURE_SIZE = 0xd33, GL_INDEX_SHIFT = 0xd12, GL_INDEX_OFFSET = 0xd13,
    GL_TEXTURE_2D = 0xde1, GL_TEXTURE_BORDER_COLOR = 0x1004,
    GL_BYTE = 0x1400, GL_UNSIGNED_BYTE, GL_SHORT, GL_UNSIGNED_SHORT, GL_INT, GL_UNSIGNED_INT, GL_FLOAT, GL_HALF_FLOAT = 0x140b,
    GL_INVERT = 0x150a, GL_COLOR_INDEX = 0x1900, GL_STENCIL_INDEX = 0x1901, GL_DEPTH_COMPONENT = 0x1902, GL_RED = 0x1903, GL_GREEN, GL_BLUE, GL_ALPHA, GL_RGB, GL_RGBA,
    GL_LUMINANCE, GL_LUMINANCE_ALPHA,
    GL_POINT = 0x1b00, GL_LINE, GL_FILL,
    GL_KEEP = 0x1e00, GL_REPLACE, GL_INCR, GL_DECR, GL_VENDOR = 0x1f00, GL_RENDERER, GL_VERSION, GL_EXTENSIONS,
    GL_NEAREST = 0x2600, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST = 0x2700, GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR,
    GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER, GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_CLAMP = 0x2900, GL_REPEAT,
    GL_FUNC_ADD = 0x8006, GL_MIN, GL_MAX, GL_FUNC_SUBTRACT = 0x800a, GL_FUNC_REVERSE_SUBTRACT,
    GL_UNSIGNED_SHORT_4_4_4_4 = 0x8033, GL_UNSIGNED_SHORT_5_5_5_1, GL_POLYGON_OFFSET_FILL = 0x8037,
    GL_RGB8 = 0x8051, GL_RGBA4 = 0x8056, GL_RGB5_A1, GL_RGBA8, GL_RGB10_A2, GL_RGBA16 = 0x805b,
    GL_TEXTURE_WRAP_R = 0x8072, GL_COLOR_TABLE = 0x80d0, GL_BGRA = 0x80e1,
    GL_CLAMP_TO_BORDER = 0x812d, GL_CLAMP_TO_EDGE = 0x812f, GL_TEXTURE_MIN_LOD = 0x813a, GL_TEXTURE_MAX_LOD, GL_TEXTURE_BASE_LEVEL, GL_TEXTURE_MAX_LEVEL,
    GL_DEPTH_COMPONENT16 = 0x81a5, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT32,
    GL_MAJOR_VERSION = 0x821b, GL_MINOR_VERSION, GL_NUM_EXTENSIONS, GL_RG = 0x8227, GL_R8 = 0x8229, GL_RG8 = 0x822b, GL_R16F = 0x822d, GL_R32F, GL_RG16F, GL_RG32F,
    GL_UNSIGNED_SHORT_5_6_5 = 0x8363, GL_MIRRORED_REPEAT = 0x8370,
    GL_COMPRESSED_RGB_S3TC_DXT1 = 0x83f0, GL_COMPRESSED_RGBA_S3TC_DXT1, GL_COMPRESSED_RGBA_S3TC_DXT3, GL_COMPRESSED_RGBA_S3TC_DXT5,
    GL_TEXTURE0 = 0x84c0, GL_MAX_RENDERBUFFER_SIZE = 0x84e8, GL_DEPTH_STENCIL = 0x84f9, GL_UNSIGNED_INT_24_8 = 0x84fa, GL_MAX_TEXTURE_LOD_BIAS = 0x84fd,
    GL_TEXTURE_MAX_ANISOTROPY = 0x84fe, GL_MAX_TEXTURE_MAX_ANISOTROPY, GL_TEXTURE_LOD_BIAS = 0x8501,
    GL_INCR_WRAP = 0x8507, GL_DECR_WRAP, GL_TEXTURE_CUBE_MAP = 0x8513, GL_TEXTURE_CUBE_MAP_POSITIVE_X = 0x8515, GL_TEXTURE_CUBE_MAP_NEGATIVE_Z = 0x851a,
    GL_MAX_VERTEX_ATTRIBS = 0x8869, GL_MAX_TEXTURE_IMAGE_UNITS = 0x8872, GL_RGBA32F = 0x8814, GL_RGB32F, GL_RGBA16F = 0x881a, GL_RGB16F,
    GL_TEXTURE_COMPARE_MODE = 0x884c, GL_TEXTURE_COMPARE_FUNC,
    GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER, GL_DEPTH24_STENCIL8 = 0x88f0,
    GL_FRAGMENT_SHADER = 0x8b30, GL_VERTEX_SHADER, GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS = 0x8b4d, GL_SHADER_TYPE = 0x8b4f, GL_DELETE_STATUS = 0x8b80, GL_COMPILE_STATUS,
    GL_LINK_STATUS, GL_VALIDATE_STATUS, GL_INFO_LOG_LENGTH, GL_ATTACHED_SHADERS, GL_ACTIVE_UNIFORMS, GL_ACTIVE_UNIFORM_MAX_LENGTH, GL_SHADER_SOURCE_LENGTH,
    GL_ACTIVE_ATTRIBUTES, GL_ACTIVE_ATTRIBUTE_MAX_LENGTH, GL_SHADING_LANGUAGE_VERSION = 0x8b8c, GL_CURRENT_PROGRAM,
    GL_R11F_G11F_B10F = 0x8c3a, GL_SRGB8_ALPHA8 = 0x8c43, GL_COMPRESSED_SRGB_S3TC_DXT1 = 0x8c4c, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3,
    GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5, GL_DEPTH_COMPONENT32F = 0x8cac, GL_DEPTH32F_STENCIL8,
    GL_FRAMEBUFFER_BINDING = 0x8ca6, GL_RENDERBUFFER_BINDING, GL_COLOR_ATTACHMENT0 = 0x8ce0, GL_DEPTH_ATTACHMENT = 0x8d00, GL_STENCIL_ATTACHMENT = 0x8d20,
    GL_FRAMEBUFFER = 0x8d40, GL_RENDERBUFFER, GL_STENCIL_INDEX8 = 0x8d48, GL_RGB565 = 0x8d62, GL_MAX_SAMPLES = 0x8d57,
    GL_COMPRESSED_RED_RGTC1 = 0x8dbb, GL_COMPRESSED_SIGNED_RED_RGTC1, GL_COMPRESSED_RG_RGTC2, GL_COMPRESSED_SIGNED_RG_RGTC2,
    GL_MAX_VERTEX_UNIFORM_VECTORS = 0x8dfb, GL_MAX_VARYING_VECTORS, GL_MAX_FRAGMENT_UNIFORM_VECTORS,
    GL_COMPRESSED_RGBA_BPTC_UNORM = 0x8e8c, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT, GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT,
    GL_COMPRESSED_RGBA_ASTC_4x4 = 0x93b0, GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4 = 0x93d0,
    GL_DEPTH_STENCIL_ATTACHMENT = 0x821a,
    GL_CW = 0x900, GL_CCW, GL_POINT_SIZE = 0xb11, GL_POINT_SIZE_RANGE, GL_CULL_FACE_MODE = 0xb45, GL_FRONT_FACE, GL_SCISSOR_BOX = 0xc10, GL_PACK_ALIGNMENT = 0xd05,
    GL_MAX_VIEWPORT_DIMS = 0xd3a, GL_COLOR = 0x1800, GL_DEPTH, GL_STENCIL, GL_ZERO = 0, GL_ONE = 1,
    GL_TEXTURE_BINDING_3D = 0x806a, GL_TEXTURE_3D = 0x806f, GL_MAX_3D_TEXTURE_SIZE = 0x8073, GL_SAMPLE_BUFFERS = 0x80a8, GL_SAMPLES, GL_CONTEXT_FLAGS = 0x821e,
    GL_ALIASED_POINT_SIZE_RANGE = 0x846d, GL_TEXTURE_BINDING_CUBE_MAP = 0x8514, GL_MAX_CUBE_MAP_TEXTURE_SIZE = 0x851c, GL_MAX_DRAW_BUFFERS = 0x8824, GL_MAX_ARRAY_TEXTURE_LAYERS = 0x88ff,
    GL_FLOAT_VEC2 = 0x8b50, GL_FLOAT_VEC3, GL_FLOAT_VEC4, GL_INT_VEC2, GL_INT_VEC3, GL_INT_VEC4, GL_BOOL, GL_FLOAT_MAT2 = 0x8b5a, GL_FLOAT_MAT3, GL_FLOAT_MAT4, GL_SAMPLER_2D = 0x8b5e,
    GL_SAMPLER_3D, GL_SAMPLER_CUBE, GL_SAMPLER_2D_SHADOW = 0x8b62, GL_TEXTURE_2D_ARRAY = 0x8c1a, GL_TEXTURE_BINDING_2D_ARRAY = 0x8c1d,
    GL_READ_FRAMEBUFFER = 0x8ca8, GL_DRAW_FRAMEBUFFER, GL_READ_FRAMEBUFFER_BINDING, GL_RENDERBUFFER_SAMPLES, GL_FRAMEBUFFER_COMPLETE = 0x8cd5, GL_MAX_COLOR_ATTACHMENTS = 0x8cdf,
    GL_RENDERBUFFER_WIDTH = 0x8d42, GL_RENDERBUFFER_HEIGHT, GL_RENDERBUFFER_INTERNAL_FORMAT, GL_FRAMEBUFFER_SRGB = 0x8db9, GL_SAMPLER_2D_ARRAY = 0x8dc1,
    GL_UNSIGNED_INT_VEC2 = 0x8dc6, GL_UNSIGNED_INT_VEC3, GL_UNSIGNED_INT_VEC4, GL_TEXTURE_SWIZZLE_R = 0x8e42, GL_TEXTURE_SWIZZLE_A = 0x8e45,
    GL_ALREADY_SIGNALED = 0x911a, GL_TIMEOUT_EXPIRED, GL_CONDITION_SATISFIED, GL_WAIT_FAILED, GL_CONTEXT_PROFILE_MASK = 0x9126,
    GL_PRIMITIVE_RESTART_FIXED_INDEX = 0x8d69,
};
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x8
#define GL_MAP_UNSYNCHRONIZED_BIT 0x20
#define GL_MAP_PERSISTENT_BIT 0x40
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x1
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x100
#define GL_STENCIL_BUFFER_BIT 0x400

static uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t umax(uint32_t a, uint32_t b) { return a > b ? a : b; }
static void *zalloc(size_t n) { void *p = calloc(1, n ? n : 1); if (!p) die("[gl] out of memory %zu", n); return p; }
#define H(x) ((uint64_t)(uintptr_t)(x))   // a non-dispatchable handle as an integer (a pointer on the 64-bit hosts this runs on)

static guest_gl_stats g_st;
void guest_gl_stats_get(guest_gl_stats *s) { *s = g_st; }

// ---------- Device-wide state ----------
static int g_on;                     // the backend's device is up and the layer's objects are made: GL calls record GPU work (0: the state machine runs, nothing is drawn)
static VkSemaphore g_tl;             // the layer's timeline: each submission signals its serial (be_present_ext waits for a frame's)
static uint64_t g_serial = 1;        // the serial of the submission being recorded; submitted ones are lower
static uint64_t g_last;              // the last serial submitted
static VkDeviceSize g_ubo_align = 256;
static int64_t g_mem_tex, g_mem_buf; static uint32_t g_n_tex_img;
static uint64_t gpu_done(void) { uint64_t v = 0; if (g_tl) vkGetSemaphoreCounterValue(hvk.d, g_tl, &v); return v; }
static int serial_wait(uint64_t s, uint64_t timeout_ns) {   // 1 once the submission of serial s has completed
    if (!s || !g_tl) return 1;
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &g_tl, &s};
    return vkWaitSemaphores(hvk.d, &wi, timeout_ns) == VK_SUCCESS;
}
// An image barrier with a full memory dependency (the backend's rule: no finer barriers), for a layout change
static void img_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                               VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, from, to, VK_QUEUE_FAMILY_IGNORED,
                               VK_QUEUE_FAMILY_IGNORED, img, {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &b};
    vkCmdPipelineBarrier2(cb, &di);
}

// ---------- Objects the GPU may still use: released once the submission that last used them has completed ----------
enum { D_BUF, D_IMG, D_VIEW, D_PIPE, D_MOD, D_PLAYOUT, D_DSL, D_SLAB };
typedef struct { int kind; uint64_t serial, a, b, c; } defer_t;
static defer_t *g_def; static int g_ndef, g_capdef;
static void slab_put(void *slab, uint32_t idx);
static void mem_free(VkDeviceMemory m, VkDeviceSize size) { if (m) { vkFreeMemory(hvk.d, m, NULL); hvk.dev.allocations--; g_mem_tex -= (int64_t)size; } }
static void release(const defer_t *d) {
    switch (d->kind) {
        case D_BUF: hvk_buffer_free((VkBuffer)(uintptr_t)d->a, (VkDeviceMemory)(uintptr_t)d->b); g_mem_buf -= (int64_t)d->c; break;
        case D_IMG: vkDestroyImage(hvk.d, (VkImage)(uintptr_t)d->a, NULL); mem_free((VkDeviceMemory)(uintptr_t)d->b, d->c); g_n_tex_img--; break;
        case D_VIEW: vkDestroyImageView(hvk.d, (VkImageView)(uintptr_t)d->a, NULL); break;
        case D_PIPE: vkDestroyPipeline(hvk.d, (VkPipeline)(uintptr_t)d->a, NULL); break;
        case D_MOD: vkDestroyShaderModule(hvk.d, (VkShaderModule)(uintptr_t)d->a, NULL); break;
        case D_PLAYOUT: vkDestroyPipelineLayout(hvk.d, (VkPipelineLayout)(uintptr_t)d->a, NULL); break;
        case D_DSL: vkDestroyDescriptorSetLayout(hvk.d, (VkDescriptorSetLayout)(uintptr_t)d->a, NULL); break;
        case D_SLAB: slab_put((void *)(uintptr_t)d->a, (uint32_t)d->b); break;
    }
}
static void defer(int kind, uint64_t serial, uint64_t a, uint64_t b, uint64_t c) {   // serial: the last submission that may use it (0: none)
    if (!a) return;
    defer_t d = {kind, serial, a, b, c};
    if (serial <= gpu_done()) { release(&d); return; }
    if (g_ndef == g_capdef) { g_capdef = g_capdef ? 2 * g_capdef : 256; g_def = realloc(g_def, (size_t)g_capdef * sizeof *g_def); if (!g_def) die("[gl] out of memory"); }
    g_def[g_ndef++] = d;
}
static void defer_run(void) {
    uint64_t done = gpu_done(); int k = 0;
    for (int i = 0; i < g_ndef; i++) { if (g_def[i].serial <= done) release(&g_def[i]); else g_def[k++] = g_def[i]; }
    g_ndef = k;
}

// ---------- Transient memory: staging for uploads, the uniform blocks of draws, converted indices, the values of disabled attributes ----------
// Bump allocation in 4 MiB host-visible coherent buffers, each reused once the submission that last used it completed (hle_gl.m's arena)
#define ARENA_BYTES (4u << 20)
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *map; VkDeviceSize size, used; uint64_t serial; } arena_t;
static arena_t *g_arena; static int g_narena, g_caparena, g_cur_arena = -1;
typedef struct { VkBuffer buf; VkDeviceSize off; uint8_t *cpu; } galloc;
static VkBufferUsageFlags buf_usage(void) {
    return VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
}
static int arena_alloc(VkDeviceSize n, VkDeviceSize align, galloc *out) {
    if (align < 16) align = 16;
    if (g_cur_arena >= 0) {
        arena_t *a = &g_arena[g_cur_arena]; VkDeviceSize o = (a->used + align - 1) / align * align;
        if (o + n <= a->size) { a->used = o + n; a->serial = g_serial; *out = (galloc){a->buf, o, a->map + o}; return 1; }
    }
    VkDeviceSize sz = n > ARENA_BYTES ? n : ARENA_BYTES; uint64_t done = gpu_done(); int k = -1;
    for (int i = 0; i < g_narena && k < 0; i++) if (i != g_cur_arena && g_arena[i].serial <= done && g_arena[i].size >= sz) k = i;
    if (k < 0) {
        if (g_narena == g_caparena) { g_caparena = g_caparena ? 2 * g_caparena : 16; g_arena = realloc(g_arena, (size_t)g_caparena * sizeof *g_arena); if (!g_arena) die("[gl] out of memory"); }
        arena_t *a = &g_arena[g_narena]; memset(a, 0, sizeof *a); void *map = NULL;
        if (hvk_buffer_make(sz, buf_usage(), 1, &a->buf, &a->mem, &map)) { RED1("transient memory: a block of %llu bytes could not be made", (unsigned long long)sz); return 0; }
        a->map = map; a->size = sz; k = g_narena++;
        if (g_narena > 64) RED1("transient memory: over 64 blocks of 4 MiB in flight");
    }
    arena_t *a = &g_arena[k]; a->used = n; a->serial = g_serial; g_cur_arena = k;
    *out = (galloc){a->buf, 0, a->map};
    return 1;
}

// ---------- Buffer memory: host-visible coherent, mapped for good ----------
// Up to 256 KiB from 4 MiB slabs of one size class each (a power of two from 256 bytes), each slab one VkBuffer; a larger buffer is an allocation of its own. A game's many
// small vertex and index buffers would otherwise each take one of maxMemoryAllocationCount's allocations (4,096 on many desktop drivers)
#define SLAB_BYTES (4u << 20)
enum { SLAB_MIN = 8, SLAB_MAX = 18 };
typedef struct slab { VkBuffer buf; VkDeviceMemory mem; uint8_t *map; uint32_t cls, n, nfree; uint32_t *fl; struct slab *next; } slab_t;
static slab_t *g_slabs[SLAB_MAX - SLAB_MIN + 1];
typedef struct { VkBuffer buf; VkDeviceSize off, cap; uint8_t *map; slab_t *slab; uint32_t idx; VkDeviceMemory own; } bmem_t;
static void slab_put(void *slab, uint32_t idx) { slab_t *s = slab; s->fl[s->nfree++] = idx; }
static int bmem_alloc(VkDeviceSize n, bmem_t *m) {
    memset(m, 0, sizeof *m);
    if (!n) n = 16;
    int c = SLAB_MIN; while (c <= SLAB_MAX && (1ull << c) < n) c++;
    if (c > SLAB_MAX) {
        void *map = NULL; n = (n + 255) & ~(VkDeviceSize)255;
        if (hvk_buffer_make(n, buf_usage(), 1, &m->buf, &m->own, &map)) { LOG("[gl] [RED] a buffer of %llu bytes could not be made", (unsigned long long)n); m->buf = VK_NULL_HANDLE; return 0; }
        m->map = map; m->cap = n; g_mem_buf += (int64_t)n; return 1;
    }
    slab_t **head = &g_slabs[c - SLAB_MIN], *s = *head;
    while (s && !s->nfree) s = s->next;
    if (!s) {
        s = zalloc(sizeof *s); s->cls = (uint32_t)c; s->n = SLAB_BYTES >> c; void *map = NULL;
        if (hvk_buffer_make(SLAB_BYTES, buf_usage(), 1, &s->buf, &s->mem, &map)) { free(s); LOG("[gl] [RED] a buffer slab of %u bytes could not be made", SLAB_BYTES); return 0; }
        s->map = map; s->fl = zalloc(s->n * sizeof *s->fl);
        for (uint32_t i = 0; i < s->n; i++) s->fl[i] = s->n - 1 - i;
        s->nfree = s->n; s->next = *head; *head = s; g_mem_buf += SLAB_BYTES;
    }
    uint32_t i = s->fl[--s->nfree];
    m->buf = s->buf; m->off = (VkDeviceSize)i << c; m->cap = 1ull << c; m->map = s->map + m->off; m->slab = s; m->idx = i;
    return 1;
}
static void bmem_release(bmem_t *m, uint64_t use) {   // back to its slab (or freed) once the GPU no longer reads it
    if (!m->buf) return;
    if (m->slab) defer(D_SLAB, use, H(m->slab), m->idx, 0);
    else defer(D_BUF, use, H(m->buf), H(m->own), m->cap);
    memset(m, 0, sizeof *m);
}

// ---------- Shader table (<library>/gl_shaders.tsv, gl_shaders.py --vk) ----------
enum { T_FLOAT, T_VEC2, T_VEC3, T_VEC4, T_INT, T_IVEC2, T_IVEC3, T_IVEC4, T_UINT, T_UVEC2, T_UVEC3, T_UVEC4, T_BOOL, T_MAT2, T_MAT3, T_MAT4,
       T_SAMP2D, T_SAMPCUBE, T_SAMP3D, T_SAMP2DARR, T_SAMP2DSH, T_OTHER };
static const char *const g_tnames[] = {"float", "vec2", "vec3", "vec4", "int", "ivec2", "ivec3", "ivec4", "uint", "uvec2", "uvec3", "uvec4", "bool", "mat2", "mat3", "mat4",
                                       "sampler2D", "samplerCube", "sampler3D", "sampler2DArray", "sampler2DShadow"};
static const GLenum g_tenums[] = {GL_FLOAT, GL_FLOAT_VEC2, GL_FLOAT_VEC3, GL_FLOAT_VEC4, GL_INT, GL_INT_VEC2, GL_INT_VEC3, GL_INT_VEC4, GL_UNSIGNED_INT, GL_UNSIGNED_INT_VEC2,
                                  GL_UNSIGNED_INT_VEC3, GL_UNSIGNED_INT_VEC4, GL_BOOL, GL_FLOAT_MAT2, GL_FLOAT_MAT3, GL_FLOAT_MAT4, GL_SAMPLER_2D, GL_SAMPLER_CUBE, GL_SAMPLER_3D,
                                  GL_SAMPLER_2D_ARRAY, GL_SAMPLER_2D_SHADOW, GL_FLOAT};   // glGetActiveUniform's types
static int tcode(const char *s) { for (int i = 0; i < T_OTHER; i++) if (!strcmp(s, g_tnames[i])) return i; return T_OTHER; }
static int tcomps(int t) { static const int c[] = {1, 2, 3, 4, 1, 2, 3, 4, 1, 2, 3, 4, 1, 4, 9, 16}; return t < T_SAMP2D ? c[t] : 1; }
static int is_samp(int t) { return t >= T_SAMP2D && t < T_OTHER; }
static int is_intt(int t) { return t >= T_INT && t <= T_BOOL; }
static int in_class(int t) { return t >= T_INT && t <= T_IVEC4 ? 1 : t >= T_UINT && t <= T_BOOL ? 2 : 0; }   // a vertex input's numeric type: 0 float, 1 signed, 2 unsigned
typedef struct { char name[48]; uint8_t slot, type; } shattr;                         // a vertex input (its location)
typedef struct { char name[48]; uint16_t off, count, stride; uint8_t type; } shuni;   // a default block member (count 0 = not an array)
typedef struct { char name[48]; uint8_t slot, type; } shsamp;                         // a combined sampler (its binding)
typedef struct { char key[17]; int frag, ub_slot, ub_size, na, nu, nt; shattr *a; shuni *u; shsamp *t; uint32_t *spv; size_t nspv; int tried; } shinfo;
static shinfo *g_sh; static int g_nsh;
static char g_libdir[1024];
static int sh_cmp(const void *a, const void *b) { int c = memcmp(((const shinfo *)a)->key, ((const shinfo *)b)->key, 16); return c ? c : ((const shinfo *)a)->frag - ((const shinfo *)b)->frag; }
// The library's folder: <DM_MSL_DIR>/gl-vk, else under the shader cache folder the NVN backend uses (vk_shader.c hvk_shader_dir: DM_ROOT's or the platform's data folder),
// beside the NVN chain's vk-<DMSH_CHAIN_VK>/ and the Metal GL library gl/
static void lib_dir(void) {
    const char *m = getenv("DM_MSL_DIR"), *root = getenv("DM_ROOT"); char parent[900];
    if (m && *m) snprintf(parent, sizeof parent, "%s", m);
    else if (root && *root) snprintf(parent, sizeof parent, "%s/build/cache/metal_shader_cache", root);
    else plat_data_path(parent, sizeof parent, "build/cache/metal_shader_cache");
    snprintf(g_libdir, sizeof g_libdir, "%s/gl-vk", parent);
}
static char *next_field(char **s, const char *delim) {   // strsep's rule (not in every C library the port builds with): the text up to a delimiter, which ends it
    char *b = *s; if (!b) return NULL;
    char *e = b + strcspn(b, delim);
    if (*e) { *e = 0; *s = e + 1; } else *s = NULL;
    return b;
}
static void sh_load(void) {
    char p[1100]; lib_dir();
    snprintf(p, sizeof p, "%s/gl_shaders.tsv", g_libdir);
    FILE *f = fopen(p, "r");
    if (!f) { GLOG("[RED] cannot open %s (run dm/gfx/tools/gl_shaders.py --vk first): no GL shader can be used", p); return; }
    static char line[65536]; int cap = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *rest = line, *key = next_field(&rest, "\t"), *stage = next_field(&rest, "\t"), *res = next_field(&rest, "\t\r\n");
        if (!key || !stage || strlen(key) != 16) continue;
        if (g_nsh == cap) { cap = cap ? cap * 2 : 1024; g_sh = realloc(g_sh, (size_t)cap * sizeof *g_sh); if (!g_sh) die("[gl] out of memory"); }
        shinfo *s = &g_sh[g_nsh++]; memset(s, 0, sizeof *s);
        memcpy(s->key, key, 16); s->frag = !strcmp(stage, "frag"); s->ub_slot = -1;
        int na = 0, nu = 0, nt = 0;
        for (char *c = res; c && *c; c++) if (c == res || c[-1] == ' ') { na += *c == 'a'; nu += *c == 'u'; nt += *c == 't'; }
        s->a = zalloc((size_t)(na + 1) * sizeof *s->a); s->u = zalloc((size_t)(nu + 1) * sizeof *s->u); s->t = zalloc((size_t)(nt + 1) * sizeof *s->t);
        for (char *sp = NULL, *t = res ? strtok_r(res, " ", &sp) : NULL; t; t = strtok_r(NULL, " ", &sp)) {
            char nm[64], ty[32]; unsigned a1, a2, a3;
            if (t[0] == 'a' && sscanf(t, "a:%63[^=]=%u:%31s", nm, &a1, ty) == 3 && a1 < 16) { shattr *x = &s->a[s->na++]; snprintf(x->name, sizeof x->name, "%s", nm); x->slot = (uint8_t)a1; x->type = (uint8_t)tcode(ty); }
            else if (t[0] == 'u' && sscanf(t, "u:%63[^=]=%u:%31[^:]:%u:%u", nm, &a1, ty, &a2, &a3) == 5) {
                shuni *x = &s->u[s->nu++]; snprintf(x->name, sizeof x->name, "%s", nm); x->off = (uint16_t)a1; x->type = (uint8_t)tcode(ty); x->count = (uint16_t)a2; x->stride = (uint16_t)a3;
            } else if (t[0] == 't' && sscanf(t, "t:%63[^=]=%u:%31s", nm, &a1, ty) == 3) { shsamp *x = &s->t[s->nt++]; snprintf(x->name, sizeof x->name, "%s", nm); x->slot = (uint8_t)a1; x->type = (uint8_t)tcode(ty); }
            else if (t[0] == 'b' && sscanf(t, "b:%u:%u", &a1, &a2) == 2) { s->ub_slot = (int)a1; s->ub_size = (int)a2; }
            else if (t[0] == 'a') GLOG("[RED] shader %s: vertex input %s at a location above 15 (the layer's 16 attributes): not fed", s->key, t);
        }
    }
    fclose(f);
    qsort(g_sh, (size_t)g_nsh, sizeof *g_sh, sh_cmp);
}
static const shinfo *sh_find(const char *key, int frag) { shinfo k; memcpy(k.key, key, 16); k.frag = frag; return g_nsh ? bsearch(&k, g_sh, (size_t)g_nsh, sizeof *g_sh, sh_cmp) : NULL; }
// The module's words, read from the library at its first use (gl_<key>_v.spv / gl_<key>_f.spv); NULL when the file is missing or no SPIR-V
static const uint32_t *sh_words(shinfo *s, size_t *n) {
    if (!s->tried) {
        s->tried = 1;
        char p[1200]; snprintf(p, sizeof p, "%s/gl_%.16s_%c.spv", g_libdir, s->key, s->frag ? 'f' : 'v');
        FILE *f = fopen(p, "rb"); long sz = -1;
        if (f && !fseek(f, 0, SEEK_END)) { sz = ftell(f); fseek(f, 0, SEEK_SET); }
        if (f && sz >= 20 && !(sz & 3)) {
            s->spv = malloc((size_t)sz);
            if (s->spv && fread(s->spv, 1, (size_t)sz, f) == (size_t)sz && s->spv[0] == 0x07230203u) s->nspv = (size_t)sz / 4;
            else { free(s->spv); s->spv = NULL; }
        }
        if (f) fclose(f);
        if (!s->spv) GLOG("[RED] shader %s: %s is missing or not SPIR-V", s->key, p);
    }
    *n = s->nspv; return s->spv;
}

// ---------- The varyings of a program: matched by name, as GL links them ----------
// The Input or Output variables of a module that have a Location, by name (the variable's OpName; an interface block's instance without one: its type's name), with the word
// index of the Location's literal
typedef struct { char name[64]; uint32_t id, loc, at; } spv_var;
static void spv_str(const uint32_t *w, size_t n, size_t at, char *out, size_t cap) {   // the literal string at word at, bounded by the module and cap
    size_t k = 0;
    for (size_t i = at; i < n && k + 1 < cap; i++) for (int b = 0; b < 4 && k + 1 < cap; b++) { char c = (char)(w[i] >> (8 * b) & 0xff); if (!c) { out[k] = 0; return; } out[k++] = c; }
    out[k] = 0;
}
static int spv_iface(const uint32_t *w, size_t n, uint32_t storage, spv_var *out, int max) {
    uint32_t bound = w[3]; int k = 0;
    if (!bound || bound > 0x400000u) return 0;
    uint32_t *name_at = calloc(bound, 4), *loc_at = calloc(bound, 4), *ptr_to = calloc(bound, 4);
    if (!name_at || !loc_at || !ptr_to) { free(name_at); free(loc_at); free(ptr_to); return 0; }
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || wc > n - i) break;
        if (op == 5 && wc >= 3 && w[i + 1] < bound) name_at[w[i + 1]] = (uint32_t)(i + 2);                              // OpName
        else if (op == 71 && wc == 4 && w[i + 2] == 30 && w[i + 1] < bound) loc_at[w[i + 1]] = (uint32_t)(i + 3);      // OpDecorate Location
        else if (op == 32 && wc == 4 && w[i + 1] < bound) ptr_to[w[i + 1]] = w[i + 3];                                  // OpTypePointer
        i += wc;
    }
    for (size_t i = 5; i < n && k < max;) {
        uint32_t wc = w[i] >> 16;
        if (!wc || wc > n - i) break;
        if ((w[i] & 0xffff) == 59 && wc >= 4 && w[i + 3] == storage && w[i + 2] < bound && loc_at[w[i + 2]]) {   // OpVariable
            spv_var *v = &out[k++]; memset(v, 0, sizeof *v);
            v->id = w[i + 2]; v->at = loc_at[v->id]; v->loc = w[v->at];
            if (name_at[v->id]) spv_str(w, n, name_at[v->id], v->name, sizeof v->name);
            uint32_t t = w[i + 1] < bound ? ptr_to[w[i + 1]] : 0;
            if (!v->name[0] && t && t < bound && name_at[t]) spv_str(w, n, name_at[t], v->name, sizeof v->name);
        }
        i += wc;
    }
    free(name_at); free(loc_at); free(ptr_to);
    return k;
}
// The fragment module's words with each input at the location of the vertex output of its name (a copy the caller frees); *moved: the inputs that changed
static uint32_t *spv_link(const uint32_t *vs, size_t nvs, const uint32_t *fs, size_t nfs, GLuint prog, int *moved) {
    spv_var vo[64], fi[64]; int no = spv_iface(vs, nvs, 3, vo, 64), ni = spv_iface(fs, nfs, 1, fi, 64);
    uint32_t *w = malloc(nfs * 4); *moved = 0;
    if (!w) return NULL;
    memcpy(w, fs, nfs * 4);
    for (int i = 0; i < ni; i++) {
        int j = 0; while (j < no && strcmp(vo[j].name, fi[i].name)) j++;
        if (j == no) { LOG("[gl] [RED] program %u: fragment input %s has no vertex output of that name (GL would not link it): it reads what location %u holds", prog, fi[i].name, fi[i].loc); continue; }
        if (vo[j].loc != fi[i].loc) { w[fi[i].at] = vo[j].loc; (*moved)++; }
    }
    return w;
}

// ---------- Command buffers ----------
// NSLOT submissions in flight; each slot: a command pool with the frame's two command buffers (pre: uploads into textures the frame has not read yet, and new images' first
// layout; main: everything else, in GL's order), and the descriptor pools of its draws
#define NSLOT 4
typedef struct { VkCommandPool pool; VkCommandBuffer pre, main; uint64_t serial; int pre_on, main_on; VkDescriptorPool dp[64]; int ndp, cdp; } slot_t;
static slot_t g_slot[NSLOT]; static int g_cur = -1;
static void enc_reset(void);
static void pass_end(void);
static slot_t *slot_cur(void) {
    if (g_cur >= 0) return &g_slot[g_cur];
    int k = (int)(g_serial % NSLOT); slot_t *s = &g_slot[k];
    if (s->serial) serial_wait(s->serial, UINT64_MAX);   // its last submission has completed: its command buffers and descriptor sets are free again
    vkResetCommandPool(hvk.d, s->pool, 0);
    for (int i = 0; i < s->ndp; i++) vkResetDescriptorPool(hvk.d, s->dp[i], 0);
    s->cdp = 0; s->serial = g_serial; s->pre_on = s->main_on = 0; g_cur = k;
    defer_run();
    return s;
}
static void cb_begin(VkCommandBuffer cb) {
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkBeginCommandBuffer(cb, &bi);
    hvk_full_barrier(cb);   // after everything submitted before it on the queue (the previous frames, the presenter's copies)
}
static VkCommandBuffer cb_main(void) { slot_t *s = slot_cur(); if (!s->main_on) { cb_begin(s->main); s->main_on = 1; enc_reset(); } return s->main; }
static VkCommandBuffer cb_pre(void) { slot_t *s = slot_cur(); if (!s->pre_on) { cb_begin(s->pre); s->pre_on = 1; } return s->pre; }
// Submit the recorded commands (the pre command buffer first), signalling the timeline with their serial; the caller holds the graphics gate (dm_gfx_enter)
static uint64_t cb_commit(void) {
    pass_end();
    slot_t *s = slot_cur();   // an empty frame still signals its serial
    VkCommandBuffer cbs[2]; uint32_t n = 0;
    if (s->pre_on) { vkEndCommandBuffer(s->pre); cbs[n++] = s->pre; }
    if (s->main_on) { vkEndCommandBuffer(s->main); cbs[n++] = s->main; }
    uint64_t v = g_serial;
    VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 0, NULL, 1, &v};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts, 0, NULL, NULL, n, cbs, 1, &g_tl};
    pthread_mutex_lock(&hvk.queue_mu); VkResult r = vkQueueSubmit(hvk.dev.queue, 1, &si, VK_NULL_HANDLE); pthread_mutex_unlock(&hvk.queue_mu);
    if (r) {   // the value is still reached, so nothing waits for it forever
        LOG("[gl] [RED] submission %llu failed (VkResult %d): its commands are lost", (unsigned long long)v, r);
        VkSemaphoreSignalInfo ss = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, g_tl, v}; vkSignalSemaphore(hvk.d, &ss);
    }
    s->pre_on = s->main_on = 0; g_cur = -1; g_last = v; g_serial++; g_st.submits++;
    return v;
}
static VkDescriptorSet set_alloc(VkDescriptorSetLayout l) {   // from the open slot's pools (one more when they are full)
    slot_t *s = slot_cur();
    for (;;) {
        if (s->cdp == s->ndp) {
            if (s->ndp == 64) { RED1("descriptor pools: 64 in one submission"); return VK_NULL_HANDLE; }
            VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1024}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096}};
            VkDescriptorPoolCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1024, 2, ps};
            if (vkCreateDescriptorPool(hvk.d, &ci, NULL, &s->dp[s->ndp])) { RED1("a descriptor pool could not be made"); return VK_NULL_HANDLE; }
            s->ndp++;
        }
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, s->dp[s->cdp], 1, &l};
        VkDescriptorSet set = VK_NULL_HANDLE; VkResult r = vkAllocateDescriptorSets(hvk.d, &ai, &set);
        if (!r) return set;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) { RED1("descriptor sets cannot be allocated (VkResult %d)", r); return VK_NULL_HANDLE; }
        s->cdp++;
    }
}

// ---------- Formats ----------
// fmt: index into g_fmt. The view swizzle only where Vulkan's default differs from GL's: Vulkan reads a channel a format lacks as 0 (alpha 1), as GL does, so only the luminance /
// alpha formats have one (a texture's attachment view has none; GL_TEXTURE_SWIZZLE_* goes over it in the sampled view). Conversions on upload: CV_COPY (the bytes as given),
// CV_RGB8 (RGB → RGBA), CV_565 / CV_4444 / CV_5551 (16-bit packed → RGBA8), CV_INDEX (8-bit colour indices through the glColorTable palette). bw / bh: block size of a
// compressed format, bytes: per texel or per block
enum { CV_COPY, CV_RGB8, CV_565, CV_4444, CV_5551, CV_INDEX, CV_NONE };
enum { K_COL, K_DEPTH, K_DS, K_BLOCK };
typedef struct { VkFormat vf; uint8_t bytes, bw, bh, kind; VkComponentSwizzle swz[4]; const char *name; } fmt_t;
#define SWZ(r, g, b, a) {VK_COMPONENT_SWIZZLE_##r, VK_COMPONENT_SWIZZLE_##g, VK_COMPONENT_SWIZZLE_##b, VK_COMPONENT_SWIZZLE_##a}
#define SW0 SWZ(R, G, B, A)
enum { F_RGBA8, F_BGRA8, F_SRGBA8, F_R8, F_RG8, F_L8, F_A8, F_LA8, F_RGBA16F, F_RGBA32F, F_R16F, F_RG16F, F_R32F, F_RG32F, F_RG11B10F, F_RGB10A2, F_RGBA16,
       F_D16, F_D32F, F_D32FS8, F_S8, F_BC1_RGB, F_BC1, F_BC2, F_BC3, F_BC1_RGB_SRGB, F_BC1_SRGB, F_BC2_SRGB, F_BC3_SRGB, F_BC4, F_BC4S, F_BC5, F_BC5S, F_BC7, F_BC7_SRGB,
       F_BC6S, F_BC6U, F_ASTC4, F_ASTC4_SRGB, F_N };
static const fmt_t g_fmt[F_N] = {
    [F_RGBA8] = {VK_FORMAT_R8G8B8A8_UNORM, 4, 1, 1, K_COL, SW0, "RGBA8"}, [F_BGRA8] = {VK_FORMAT_B8G8R8A8_UNORM, 4, 1, 1, K_COL, SW0, "BGRA8"},
    [F_SRGBA8] = {VK_FORMAT_R8G8B8A8_SRGB, 4, 1, 1, K_COL, SW0, "SRGB8_ALPHA8"},
    [F_R8] = {VK_FORMAT_R8_UNORM, 1, 1, 1, K_COL, SW0, "R8"}, [F_RG8] = {VK_FORMAT_R8G8_UNORM, 2, 1, 1, K_COL, SW0, "RG8"},
    [F_L8] = {VK_FORMAT_R8_UNORM, 1, 1, 1, K_COL, SWZ(R, R, R, ONE), "LUMINANCE"}, [F_A8] = {VK_FORMAT_R8_UNORM, 1, 1, 1, K_COL, SWZ(ZERO, ZERO, ZERO, R), "ALPHA"},
    [F_LA8] = {VK_FORMAT_R8G8_UNORM, 2, 1, 1, K_COL, SWZ(R, R, R, G), "LUMINANCE_ALPHA"},
    [F_RGBA16F] = {VK_FORMAT_R16G16B16A16_SFLOAT, 8, 1, 1, K_COL, SW0, "RGBA16F"}, [F_RGBA32F] = {VK_FORMAT_R32G32B32A32_SFLOAT, 16, 1, 1, K_COL, SW0, "RGBA32F"},
    [F_R16F] = {VK_FORMAT_R16_SFLOAT, 2, 1, 1, K_COL, SW0, "R16F"}, [F_RG16F] = {VK_FORMAT_R16G16_SFLOAT, 4, 1, 1, K_COL, SW0, "RG16F"},
    [F_R32F] = {VK_FORMAT_R32_SFLOAT, 4, 1, 1, K_COL, SW0, "R32F"}, [F_RG32F] = {VK_FORMAT_R32G32_SFLOAT, 8, 1, 1, K_COL, SW0, "RG32F"},
    [F_RG11B10F] = {VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, 1, 1, K_COL, SW0, "R11F_G11F_B10F"},
    [F_RGB10A2] = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 1, 1, K_COL, SW0, "RGB10_A2"}, [F_RGBA16] = {VK_FORMAT_R16G16B16A16_UNORM, 8, 1, 1, K_COL, SW0, "RGBA16"},
    [F_D16] = {VK_FORMAT_D16_UNORM, 2, 1, 1, K_DEPTH, SW0, "DEPTH16"}, [F_D32F] = {VK_FORMAT_D32_SFLOAT, 4, 1, 1, K_DEPTH, SW0, "DEPTH24/32"},
    [F_D32FS8] = {VK_FORMAT_D32_SFLOAT_S8_UINT, 8, 1, 1, K_DS, SW0, "DEPTH24_STENCIL8"}, [F_S8] = {VK_FORMAT_S8_UINT, 1, 1, 1, K_DS, SW0, "STENCIL8"},
    [F_BC1_RGB] = {VK_FORMAT_BC1_RGB_UNORM_BLOCK, 8, 4, 4, K_BLOCK, SW0, "DXT1 RGB"}, [F_BC1] = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, 4, 4, K_BLOCK, SW0, "DXT1"},
    [F_BC2] = {VK_FORMAT_BC2_UNORM_BLOCK, 16, 4, 4, K_BLOCK, SW0, "DXT3"}, [F_BC3] = {VK_FORMAT_BC3_UNORM_BLOCK, 16, 4, 4, K_BLOCK, SW0, "DXT5"},
    [F_BC1_RGB_SRGB] = {VK_FORMAT_BC1_RGB_SRGB_BLOCK, 8, 4, 4, K_BLOCK, SW0, "DXT1 RGB sRGB"}, [F_BC1_SRGB] = {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 8, 4, 4, K_BLOCK, SW0, "DXT1 sRGB"},
    [F_BC2_SRGB] = {VK_FORMAT_BC2_SRGB_BLOCK, 16, 4, 4, K_BLOCK, SW0, "DXT3 sRGB"}, [F_BC3_SRGB] = {VK_FORMAT_BC3_SRGB_BLOCK, 16, 4, 4, K_BLOCK, SW0, "DXT5 sRGB"},
    [F_BC4] = {VK_FORMAT_BC4_UNORM_BLOCK, 8, 4, 4, K_BLOCK, SW0, "RGTC1"}, [F_BC4S] = {VK_FORMAT_BC4_SNORM_BLOCK, 8, 4, 4, K_BLOCK, SW0, "RGTC1 signed"},
    [F_BC5] = {VK_FORMAT_BC5_UNORM_BLOCK, 16, 4, 4, K_BLOCK, SW0, "RGTC2"}, [F_BC5S] = {VK_FORMAT_BC5_SNORM_BLOCK, 16, 4, 4, K_BLOCK, SW0, "RGTC2 signed"},
    [F_BC7] = {VK_FORMAT_BC7_UNORM_BLOCK, 16, 4, 4, K_BLOCK, SW0, "BPTC"}, [F_BC7_SRGB] = {VK_FORMAT_BC7_SRGB_BLOCK, 16, 4, 4, K_BLOCK, SW0, "BPTC sRGB"},
    [F_BC6S] = {VK_FORMAT_BC6H_SFLOAT_BLOCK, 16, 4, 4, K_BLOCK, SW0, "BPTC signed float"}, [F_BC6U] = {VK_FORMAT_BC6H_UFLOAT_BLOCK, 16, 4, 4, K_BLOCK, SW0, "BPTC unsigned float"},
    [F_ASTC4] = {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, 16, 4, 4, K_BLOCK, SW0, "ASTC 4x4"}, [F_ASTC4_SRGB] = {VK_FORMAT_ASTC_4x4_SRGB_BLOCK, 16, 4, 4, K_BLOCK, SW0, "ASTC 4x4 sRGB"},
};
static VkFormatFeatureFlags fmt_feats(VkFormat f) {   // optimal-tiling features, cached for the core formats
    static VkFormatFeatureFlags cache[256]; static uint8_t known[256];
    if ((uint32_t)f < 256 && known[f]) return cache[f];
    VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(hvk.dev.phys, f, &p);
    if ((uint32_t)f < 256) { cache[f] = p.optimalTilingFeatures; known[f] = 1; }
    return p.optimalTilingFeatures;
}
static VkFormatFeatureFlags fmt_buf_feats(VkFormat f) {
    static VkFormatFeatureFlags cache[256]; static uint8_t known[256];
    if ((uint32_t)f < 256 && known[f]) return cache[f];
    VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(hvk.dev.phys, f, &p);
    if ((uint32_t)f < 256) { cache[f] = p.bufferFeatures; known[f] = 1; }
    return p.bufferFeatures;
}
// The Vulkan format a GL format is held in: its own, else for a depth / stencil one the depth-stencil format the device can attach
static VkFormat fmt_vk(int f) {
    VkFormat v = g_fmt[f].vf;
    if (g_fmt[f].kind == K_DS && !(fmt_feats(v) & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        v = (fmt_feats(VK_FORMAT_D32_SFLOAT_S8_UINT) & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D24_UNORM_S8_UINT;
    }
    return v;
}
static VkFormat unorm_of(VkFormat f) { return f == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM : f == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : f; }
static int has_depth(VkFormat f) { return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D16_UNORM_S8_UINT; }
static int has_stencil(VkFormat f) { return f == VK_FORMAT_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT; }
static VkImageAspectFlags aspects_of(VkFormat f) { int d = has_depth(f), s = has_stencil(f); return d || s ? (d ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (s ? VK_IMAGE_ASPECT_STENCIL_BIT : 0) : VK_IMAGE_ASPECT_COLOR_BIT; }
static int is_astc(int f) { return f == F_ASTC4 || f == F_ASTC4_SRGB; }
// internal format (+ format / type for unsized ones) → fmt and the conversion of the given data; -1 when not mapped (hle_gl.m's table)
static int fmt_pick(GLint ifmt, GLenum format, GLenum type, int *cv) {
    *cv = CV_COPY;
    if (format == GL_COLOR_INDEX) { *cv = type == GL_UNSIGNED_BYTE ? CV_INDEX : CV_NONE; return F_RGBA8; }
    if (type == GL_UNSIGNED_SHORT_5_6_5) { *cv = CV_565; return F_RGBA8; }
    if (type == GL_UNSIGNED_SHORT_4_4_4_4) { *cv = CV_4444; return F_RGBA8; }
    if (type == GL_UNSIGNED_SHORT_5_5_5_1) { *cv = CV_5551; return F_RGBA8; }
    int half = type == GL_HALF_FLOAT, fl = type == GL_FLOAT;
    switch (ifmt) {
        case GL_RGBA: case GL_RGBA8: case 4:
            if (fl) return F_RGBA32F;
            if (half) return F_RGBA16F;
            if (format == GL_BGRA) return F_BGRA8;
            if (format == GL_RGB) { *cv = CV_RGB8; return F_RGBA8; }
            return F_RGBA8;
        case GL_RGB: case GL_RGB8: case 3:
            if (fl || half) { *cv = CV_NONE; return F_RGBA16F; }
            if (format == GL_RGBA) return F_RGBA8;
            *cv = CV_RGB8; return F_RGBA8;
        case GL_BGRA: return F_BGRA8;
        case GL_RGBA4: case GL_RGB5_A1: case GL_RGB565: return F_RGBA8;   // glTexStorage2D's sized formats (no data: the uploads are converted by their type)
        case GL_SRGB8_ALPHA8: return F_SRGBA8;
        case GL_RED: case GL_R8: return fl ? F_R32F : half ? F_R16F : F_R8;
        case GL_RG: case GL_RG8: return fl ? F_RG32F : half ? F_RG16F : F_RG8;
        case GL_LUMINANCE: case 1: return F_L8;
        case GL_ALPHA: return F_A8;
        case GL_LUMINANCE_ALPHA: case 2: return F_LA8;
        case GL_RGBA16F: if (fl) { *cv = CV_NONE; } return F_RGBA16F;
        case GL_RGBA32F: return F_RGBA32F;
        case GL_R16F: return F_R16F;
        case GL_RG16F: return F_RG16F;
        case GL_R32F: return F_R32F;
        case GL_RG32F: return F_RG32F;
        case GL_R11F_G11F_B10F: if (type != 0x8c3b) *cv = CV_NONE; return F_RG11B10F;
        case GL_RGB10_A2: return F_RGB10A2;
        case GL_RGBA16: return F_RGBA16;
        case GL_DEPTH_COMPONENT16: return F_D16;
        case GL_DEPTH_COMPONENT: case GL_DEPTH_COMPONENT24: case GL_DEPTH_COMPONENT32: case GL_DEPTH_COMPONENT32F: if (type != GL_FLOAT) *cv = CV_NONE; return F_D32F;
        case GL_DEPTH_STENCIL: case GL_DEPTH24_STENCIL8: case GL_DEPTH32F_STENCIL8: *cv = CV_NONE; return F_D32FS8;
        case GL_STENCIL_INDEX8: *cv = CV_NONE; return F_S8;
        case GL_COMPRESSED_RGB_S3TC_DXT1: return F_BC1_RGB;
        case GL_COMPRESSED_RGBA_S3TC_DXT1: return F_BC1;
        case GL_COMPRESSED_RGBA_S3TC_DXT3: return F_BC2;
        case GL_COMPRESSED_RGBA_S3TC_DXT5: return F_BC3;
        case GL_COMPRESSED_SRGB_S3TC_DXT1: return F_BC1_RGB_SRGB;
        case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1: return F_BC1_SRGB;
        case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3: return F_BC2_SRGB;
        case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5: return F_BC3_SRGB;
        case GL_COMPRESSED_RED_RGTC1: return F_BC4;
        case GL_COMPRESSED_SIGNED_RED_RGTC1: return F_BC4S;
        case GL_COMPRESSED_RG_RGTC2: return F_BC5;
        case GL_COMPRESSED_SIGNED_RG_RGTC2: return F_BC5S;
        case GL_COMPRESSED_RGBA_BPTC_UNORM: return F_BC7;
        case GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM: return F_BC7_SRGB;
        case GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT: return F_BC6S;
        case GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT: return F_BC6U;
        case GL_COMPRESSED_RGBA_ASTC_4x4: return F_ASTC4;
        case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4: return F_ASTC4_SRGB;
    }
    return -1;
}
static size_t fmt_pitch(int f, uint32_t w) { const fmt_t *m = &g_fmt[f]; return (size_t)((w + m->bw - 1) / m->bw) * m->bytes; }
static size_t fmt_rows(int f, uint32_t h) { const fmt_t *m = &g_fmt[f]; return (h + m->bh - 1) / m->bh; }

// ---------- Objects ----------
// GL names index plain tables; a name is handed out by glGen* / glCreate* and its slot freed by glDelete*
#define NAMES(T, tab, n, cap) static T **tab; static uint32_t n, cap;
static uint32_t name_new(void ***tab, uint32_t *n, uint32_t *cap, size_t sz) {
    for (uint32_t i = 1; i < *n; i++) if (!(*tab)[i]) { (*tab)[i] = zalloc(sz); return i; }
    if (*n == 0) *n = 1;
    if (*n >= *cap) { *cap = *cap ? *cap * 2 : 256; *tab = realloc(*tab, *cap * sizeof **tab); if (!*tab) die("[gl] out of memory"); memset(*tab + *n, 0, (*cap - *n) * sizeof **tab); }
    (*tab)[*n] = zalloc(sz); return (*n)++;
}
static void *name_at(void ***tab, uint32_t *n, uint32_t *cap, uint32_t k, size_t sz) {   // the object of name k, made if it is not there
    if (k >= *cap) { uint32_t c = *cap ? *cap : 256; while (c <= k) c *= 2; *tab = realloc(*tab, c * sizeof **tab); if (!*tab) die("[gl] out of memory"); memset(*tab + *cap, 0, (c - *cap) * sizeof **tab); *cap = c; }
    if (*n <= k) *n = k + 1;
    if (!(*tab)[k]) (*tab)[k] = zalloc(sz);
    return (*tab)[k];
}

// use: serial of the last submission that read it; immutable: made by glBufferStorage (LÖVE's persistently mapped streams), never orphaned, the game keeps a pointer into it
typedef struct { bmem_t m; size_t size; uint64_t use; int immutable; } gbuf_t;
NAMES(gbuf_t, g_bufs, g_nbufs, g_capbufs)
static gbuf_t *buf_of(GLuint n) { return n && n < g_nbufs ? g_bufs[n] : NULL; }

// An image of the layer (a texture's, a renderbuffer's, the window's): device-local, in GENERAL from its first command on, with the attachment views made of it so far
typedef struct { uint16_t level, layer; VkFormat f; VkImageView v; } attv_t;
typedef struct { VkImage img; VkDeviceMemory mem; VkDeviceSize size; VkFormat vf; uint32_t w, h, depth, layers, levels; uint8_t vol; attv_t att[8]; int natt; } gimg_t;
// Make an image: its memory (an allocation of its own, dedicated where the driver prefers it), and its first layout change (UNDEFINED → layout) at the start of cb
static int gimg_make(gimg_t *g, VkCommandBuffer cb, VkImageLayout layout, VkImageType type, VkFormat f, uint32_t w, uint32_t h, uint32_t depth, uint32_t layers, uint32_t levels,
                     VkImageUsageFlags usage, VkImageCreateFlags flags) {
    memset(g, 0, sizeof *g);
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, flags, type, f, {w, h, depth}, levels, layers, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, usage,
                            VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult r = vkCreateImage(hvk.d, &ci, NULL, &g->img);
    if (r) { LOG("[gl] [RED] the driver refuses an image %ux%ux%u format %d, %u levels, %u layers, usage %#x (VkResult %d)", w, h, depth, f, levels, layers, usage, r); g->img = VK_NULL_HANDLE; return 0; }
    VkImageMemoryRequirementsInfo2 ri = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, NULL, g->img};
    VkMemoryDedicatedRequirements dr = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dr};
    vkGetImageMemoryRequirements2(hvk.d, &ri, &req);
    uint32_t t = hvk_mem_type(req.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (t == UINT32_MAX || ++hvk.dev.allocations > hvk.dev.caps.max_allocations) {
        if (t != UINT32_MAX) hvk.dev.allocations--;
        RED1("an image could not get memory (no device-local type, or over maxMemoryAllocationCount)"); vkDestroyImage(hvk.d, g->img, NULL); g->img = VK_NULL_HANDLE; return 0;
    }
    VkMemoryDedicatedAllocateInfo di = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, g->img, VK_NULL_HANDLE};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, dr.prefersDedicatedAllocation || dr.requiresDedicatedAllocation ? &di : NULL, req.memoryRequirements.size, t};
    r = vkAllocateMemory(hvk.d, &ai, NULL, &g->mem);
    if (!r) r = vkBindImageMemory(hvk.d, g->img, g->mem, 0);
    if (r) {
        LOG("[gl] [RED] no device-local memory of %llu bytes for an image (VkResult %d)", (unsigned long long)req.memoryRequirements.size, r);
        if (g->mem) vkFreeMemory(hvk.d, g->mem, NULL);
        hvk.dev.allocations--; vkDestroyImage(hvk.d, g->img, NULL); g->img = VK_NULL_HANDLE; g->mem = VK_NULL_HANDLE; return 0;
    }
    g->size = req.memoryRequirements.size; g->vf = f; g->w = w; g->h = h; g->depth = depth; g->layers = layers; g->levels = levels; g->vol = type == VK_IMAGE_TYPE_3D;
    g_mem_tex += (int64_t)g->size; g_n_tex_img++;
    img_barrier(cb, g->img, aspects_of(f), VK_IMAGE_LAYOUT_UNDEFINED, layout);
    return 1;
}
static VkImageView view_make(VkImage img, VkImageViewType type, VkFormat f, VkImageAspectFlags aspect, uint32_t lv0, uint32_t nlv, uint32_t l0, uint32_t nl, VkComponentMapping cm) {
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, type, f, cm, {aspect, lv0, nlv, l0, nl}};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(hvk.d, &ci, NULL, &v)) { RED1("an image view could not be made (format %d, type %d)", f, type); return VK_NULL_HANDLE; }
    return v;
}
// The view a pass renders into: level, layer (a cube face, an array layer, a 3D slice), in format f (an sRGB image through its UNORM alias when GL_FRAMEBUFFER_SRGB is off)
static VkImageView gimg_att_view(gimg_t *g, int level, int layer, VkFormat f) {
    for (int i = 0; i < g->natt; i++) if (g->att[i].level == level && g->att[i].layer == layer && g->att[i].f == f) return g->att[i].v;
    static const VkComponentMapping id = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    VkImageView v = view_make(g->img, VK_IMAGE_VIEW_TYPE_2D, f, aspects_of(g->vf), (uint32_t)level, 1, (uint32_t)layer, 1, id);
    if (!v) return VK_NULL_HANDLE;
    if (g->natt == 8) { defer(D_VIEW, g_serial, H(g->att[0].v), 0, 0); memmove(g->att, g->att + 1, 7 * sizeof *g->att); g->natt = 7; }
    g->att[g->natt++] = (attv_t){(uint16_t)level, (uint16_t)layer, f, v};
    return v;
}
static void gimg_drop(gimg_t *g, uint64_t use) {   // its views, the image and its memory, once the GPU no longer uses them
    for (int i = 0; i < g->natt; i++) defer(D_VIEW, use, H(g->att[i].v), 0, 0);
    if (g->img) defer(D_IMG, use, H(g->img), H(g->mem), g->size);
    memset(g, 0, sizeof *g);
}

// A texture: target, format, levels given so far (pend[face][level], until its image is made), sampler parameters. d: depth (3D) or layers (2D array);
// slevels: the level count glTexStorage gave (LÖVE allocates every level up front); swz: GL_TEXTURE_SWIZZLE_R..A (LÖVE's LA8 is RG8 read as R, R, R, G in a core profile)
#define MAXLV 16
typedef struct { void *data; size_t pitch, size; uint32_t w, h; } level_t;
typedef struct {
    GLenum target; int made, fmt, levels, slevels; uint32_t w, h, d; uint64_t use, gen;
    level_t pend[6][MAXLV]; int defined;   // defined: bit per level whose face(s) were given
    gimg_t im; VkImageView view; int view_dirty;   // view: the sampled view (all levels and layers, the swizzle)
    VkSampler smp; GLint minf, magf, ws, wt, wr, maxlv, cmp, swz[4]; float aniso, border[4], bias, minlod, maxlod; int smp_dirty;
} gtex_t;
NAMES(gtex_t, g_texs, g_ntexs, g_captexs)
static gtex_t *tex_of(GLuint n) { return n && n < g_ntexs ? g_texs[n] : NULL; }

typedef struct { int fmt; uint32_t w, h; gimg_t im; uint64_t gen, use; } grb_t;
NAMES(grb_t, g_rbs, g_nrbs, g_caprbs)
static grb_t *rb_of(GLuint n) { return n && n < g_nrbs ? g_rbs[n] : NULL; }

typedef struct { int kind; GLuint name; int level, face; } att_t;   // kind 0 none, 1 texture, 2 renderbuffer; face: a cube face, an array layer or a 3D slice
typedef struct { att_t col, depth, stencil; uint64_t gen; } gfb_t;
NAMES(gfb_t, g_fbs, g_nfbs, g_capfbs)
static gfb_t *fb_of(GLuint n) { return n && n < g_nfbs ? g_fbs[n] : NULL; }

typedef struct { uint8_t on, size, norm, div, bgra; GLenum type; GLsizei stride; uint64_t off; GLuint buf; } vattr_t;   // div: glVertexAttribDivisor (0 = per vertex)
typedef struct { vattr_t a[16]; GLuint ebo; } vao_t;
NAMES(vao_t, g_vaos, g_nvaos, g_capvaos)
static vao_t g_vao0;   // the default vertex array (GL core has none, the compatibility profile does)

typedef struct { GLenum type; char *src; size_t len; char key[17]; shinfo *si; int compiled; } gsh_t;
typedef struct { char name[48]; uint8_t type; uint16_t count; int32_t off[2]; uint16_t stride[2]; int slot[2]; int unit; } puni_t;   // [0] vertex, [1] fragment
// A pipeline's key beyond the program: vertex formats, strides and divisors per input location, the attachment formats, blend, colour mask, topology class, polygon mode
typedef struct { uint32_t vf[16]; uint16_t stride[16]; uint8_t div[16]; uint32_t cpf, dpf, spf; uint8_t blend, mask, bsrc, bdst, bsa, bda, eqc, eqa, topo, poly, pad[2]; } pkey_t;
typedef struct { pkey_t k; VkPipeline p; } pent_t;   // p: VK_NULL_HANDLE = failed
// amap: the game's attribute index of each input location (glBindAttribLocation before the link, else the input's own location, which glGetAttribLocation returns);
// LÖVE binds VertexPosition, VertexTexCoord, VertexColor and ConstantColor to 0–3 in every program
typedef struct { char name[48]; int index; } abind_t;
typedef struct {
    GLuint vs, fs; int linked; const shinfo *si[2];
    VkShaderModule mod[2]; VkDescriptorSetLayout dsl[2]; VkPipelineLayout pl; int nres[2];   // nres: the stage's descriptors (0: its set is not bound)
    int nu; puni_t *u; uint8_t *ub[2]; uint64_t ub_gen[2]; uint32_t serial;
    galloc ubm[2]; uint64_t ubm_serial[2], ubm_gen[2];   // the block's copy in transient memory, for the submission ubm_serial, from the CPU copy of generation ubm_gen
    VkDescriptorSet set[2]; uint64_t set_serial[2]; uint64_t *sig[2];   // the stage's last descriptor set, the submission it was made in, what it holds
    pent_t *pc; int npc, cappc; uint64_t use;
    abind_t bind[16]; int nbind; int8_t amap[16];
} gprog_t;
// Shaders and programs share one name space, as in GL
typedef struct { int is_prog; union { gsh_t s; gprog_t p; }; } sp_t;
NAMES(sp_t, g_sps, g_nsps, g_capsps)
static gsh_t *shader_of(GLuint n) { return n && n < g_nsps && g_sps[n] && !g_sps[n]->is_prog ? &g_sps[n]->s : NULL; }
static gprog_t *prog_of(GLuint n) { return n && n < g_nsps && g_sps[n] && g_sps[n]->is_prog ? &g_sps[n]->p : NULL; }

// ---------- Context state ----------
// units: as many as GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS says (LÖVE binds a texture of each type to every one at start); fb / read_fb: the draw and read framebuffers;
// va: each attribute's current value (glVertexAttrib4f), what a vertex input reads when its array is disabled: LÖVE's VertexColor and ConstantColor
#define NUNITS 96
static struct {
    GLuint array_buf, rb, fb, read_fb, prog, vao; int unit;
    struct { GLuint t2d, tcube, t3d, t2da; } units[NUNITS];
    float clear_c[4]; double clear_d; int clear_s;
    uint8_t cmask, dmask, depth, blend, cull, scissor, stencil, poffset, srgb, restart;
    GLenum dfunc, cull_face, front, bsrc, bdst, bsa, bda, eqc, eqa, poly;
    GLenum sfunc, sfail, szfail, spass; GLint sref; GLuint smask, swmask;
    GLint vp[4], sc[4]; float pfactor, punits, psize;
    float va[16][4];
    int unpack_align, unpack_row, pack_align;
    uint32_t palette[256]; int npalette, color_table, index_shift, index_offset;
    int swap_interval;
} C = {.cmask = 0xf, .dmask = 1, .dfunc = 0x201, .cull_face = GL_BACK, .front = GL_CCW, .bsrc = 1, .bdst = 0, .bsa = 1, .bda = 0, .eqc = GL_FUNC_ADD, .eqa = GL_FUNC_ADD,
       .sfunc = 0x207, .sfail = GL_KEEP, .szfail = GL_KEEP, .spass = GL_KEEP, .smask = 0xffffffff, .swmask = 0xffffffff, .clear_d = 1, .unpack_align = 4, .pack_align = 4,
       .swap_interval = 1, .psize = 1, .poly = GL_FILL,
       .va = {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1},
              {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}}};
static vao_t *cur_vao(void) { return C.vao && C.vao < g_nvaos && g_vaos[C.vao] ? g_vaos[C.vao] : &g_vao0; }
static GLuint *unit_slot(int unit, GLenum target) {   // a unit's binding of a texture target (a cube face names the cube); NULL for a target this layer does not have
    if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) target = GL_TEXTURE_CUBE_MAP;
    if (unit < 0 || unit >= NUNITS) return NULL;
    switch (target) {
        case GL_TEXTURE_2D: return &C.units[unit].t2d; case GL_TEXTURE_CUBE_MAP: return &C.units[unit].tcube;
        case GL_TEXTURE_3D: return &C.units[unit].t3d; case GL_TEXTURE_2D_ARRAY: return &C.units[unit].t2da;
    }
    return NULL;
}
static gtex_t *bound_tex(GLenum target) { GLuint *s = unit_slot(C.unit, target); return s ? tex_of(*s) : NULL; }

// ---------- The window surface ----------
// Three colour images registered with the backend's present path (be_ext_tex, an hvk_ext_image: guest_vk.h), presented in HVK_EXT_LAYOUT and in GENERAL between their first
// use in a frame and its eglSwapBuffers; a depth / stencil image (32-bit float depth and stencil 8 where the device attaches it, else 24-bit depth)
#define NWIN 3
static struct { int made; uint32_t w, h; gimg_t col[NWIN], ds; int general[NWIN]; uint64_t serial[NWIN], ticket[NWIN], use[NWIN]; int cur; } g_win;
static void win_make(uint32_t w, uint32_t h) {
    if (!g_on) return;
    for (int i = 0; i < NWIN; i++) {   // the old surface: its presents finished, then its images released once nothing uses them
        if (!g_win.made) break;
        if (g_win.ticket[i]) be_present_wait(g_win.ticket[i]);
        hvk_ext_tex_drop(g_win.serial[i]); gimg_drop(&g_win.col[i], g_serial);
    }
    if (g_win.made) gimg_drop(&g_win.ds, g_serial);
    g_win.made = 0;
    VkCommandBuffer cb = cb_pre();
    for (int i = 0; i < NWIN; i++) {
        if (!gimg_make(&g_win.col[i], cb, HVK_EXT_LAYOUT, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1, 1,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0)) return;
        hvk_ext_image e = {g_win.col[i].img, VK_FORMAT_R8G8B8A8_UNORM, w, h};
        g_win.serial[i] = be_ext_tex(&e); g_win.ticket[i] = 0; g_win.general[i] = 0; g_win.use[i] = 0;
    }
    VkFormat ds = fmt_vk(F_D32FS8);
    if (!gimg_make(&g_win.ds, cb, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_TYPE_2D, ds, w, h, 1, 1, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0)) return;
    g_win.w = w; g_win.h = h; g_win.cur = 0; g_win.made = 1;
    GLOG("window surface %ux%u (RGBA8, depth-stencil format %d, %d images)", w, h, ds, NWIN);
}
static void win_size(int *w, int *h) {
    *w = *h = 0;
    dm_vi_layer_size(w, h);
    if (*w <= 0 || *h <= 0) { *w = 1280; *h = 720; }
}
static void win_ensure(void) { if (!g_win.made) { int w, h; win_size(&w, &h); win_make((uint32_t)w, (uint32_t)h); } }
// The current window image in GENERAL for a command of the frame (its first use after a present)
static void win_general(VkCommandBuffer cb) {
    int k = g_win.cur;
    if (!g_win.general[k]) { img_barrier(cb, g_win.col[k].img, VK_IMAGE_ASPECT_COLOR_BIT, HVK_EXT_LAYOUT, VK_IMAGE_LAYOUT_GENERAL); g_win.general[k] = 1; }
    g_win.use[k] = g_serial;
}

// The given pixels (GL unpack rules: rows of unpack_row or w pixels, each row aligned to unpack_align) → the image's bytes, rows packed. NULL data → zeros
static void *pixels_convert(const void *src, uint32_t w, uint32_t h, GLenum format, GLenum type, int f, int cv, size_t *pitch_out) {
    size_t dp = fmt_pitch(f, w), rows = fmt_rows(f, h);
    uint8_t *dst = zalloc(dp * rows + 1); *pitch_out = dp;
    if (!src) return dst;
    if (g_fmt[f].kind == K_BLOCK) { memcpy(dst, src, dp * rows); return dst; }   // compressed data has no unpack rows
    static const int comps_of[] = {[GL_RED - GL_RED] = 1, [GL_GREEN - GL_RED] = 1, [GL_BLUE - GL_RED] = 1, [GL_ALPHA - GL_RED] = 1, [GL_RGB - GL_RED] = 3, [GL_RGBA - GL_RED] = 4,
                                   [GL_LUMINANCE - GL_RED] = 1, [GL_LUMINANCE_ALPHA - GL_RED] = 2};
    int comps = format == GL_BGRA ? 4 : format == GL_RG ? 2 : format == GL_COLOR_INDEX || format == GL_DEPTH_COMPONENT ? 1 :
                format >= GL_RED && format <= GL_LUMINANCE_ALPHA ? comps_of[format - GL_RED] : 4;
    int tbytes = type == GL_FLOAT || type == GL_UNSIGNED_INT || type == GL_INT ? 4 : type == GL_HALF_FLOAT || type == GL_UNSIGNED_SHORT || type == GL_SHORT ? 2 : 1;
    if (cv == CV_565 || cv == CV_4444 || cv == CV_5551) { comps = 1; tbytes = 2; }
    size_t sp = (size_t)(C.unpack_row ? (uint32_t)C.unpack_row : w) * comps * tbytes; sp = (sp + C.unpack_align - 1) / C.unpack_align * C.unpack_align;
    const uint8_t *s = src;
    for (uint32_t y = 0; y < h; y++, s += sp) {
        uint8_t *d = dst + y * dp;
        switch (cv) {
            case CV_COPY: memcpy(d, s, umin((uint32_t)dp, (uint32_t)((size_t)w * comps * tbytes))); break;
            case CV_RGB8: for (uint32_t x = 0; x < w; x++) { d[4 * x] = s[3 * x]; d[4 * x + 1] = s[3 * x + 1]; d[4 * x + 2] = s[3 * x + 2]; d[4 * x + 3] = 255; } break;
            case CV_565: for (uint32_t x = 0; x < w; x++) { uint16_t v; memcpy(&v, s + 2 * x, 2); d[4 * x] = (uint8_t)((v >> 11) * 255 / 31); d[4 * x + 1] = (uint8_t)((v >> 5 & 63) * 255 / 63); d[4 * x + 2] = (uint8_t)((v & 31) * 255 / 31); d[4 * x + 3] = 255; } break;
            case CV_4444: for (uint32_t x = 0; x < w; x++) { uint16_t v; memcpy(&v, s + 2 * x, 2); d[4 * x] = (uint8_t)((v >> 12) * 17); d[4 * x + 1] = (uint8_t)((v >> 8 & 15) * 17); d[4 * x + 2] = (uint8_t)((v >> 4 & 15) * 17); d[4 * x + 3] = (uint8_t)((v & 15) * 17); } break;
            case CV_5551: for (uint32_t x = 0; x < w; x++) { uint16_t v; memcpy(&v, s + 2 * x, 2); d[4 * x] = (uint8_t)((v >> 11) * 255 / 31); d[4 * x + 1] = (uint8_t)((v >> 6 & 31) * 255 / 31); d[4 * x + 2] = (uint8_t)((v >> 1 & 31) * 255 / 31); d[4 * x + 3] = v & 1 ? 255 : 0; } break;
            case CV_INDEX:
                for (uint32_t x = 0; x < w; x++) {
                    int i = ((int)s[x] << C.index_shift) + C.index_offset; uint32_t c = C.npalette ? C.palette[i < 0 ? 0 : i >= C.npalette ? C.npalette - 1 : i] : 0xff000000u | s[x] * 0x10101u;
                    memcpy(d + 4 * x, &c, 4);
                }
                break;
        }
    }
    return dst;
}

// ---------- Textures ----------
static VkSamplerAddressMode wrap_of(GLint w) {
    switch (w) { case GL_CLAMP_TO_EDGE: case GL_CLAMP: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; case GL_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                 case GL_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; default: return VK_SAMPLER_ADDRESS_MODE_REPEAT; }
}
static void tex_init_params(gtex_t *t, GLenum target) {
    t->target = target; t->minf = GL_NEAREST_MIPMAP_LINEAR; t->magf = GL_LINEAR; t->ws = t->wt = t->wr = GL_REPEAT; t->maxlv = 1000; t->aniso = 1; t->smp_dirty = 1; t->fmt = -1;
    t->minlod = -1000; t->maxlod = 1000;
    for (int c = 0; c < 4; c++) t->swz[c] = GL_RED + c;
}
static int tex_faces(const gtex_t *t) { return t->target == GL_TEXTURE_CUBE_MAP ? 6 : 1; }
static uint32_t tex_layers(const gtex_t *t) { return t->target == GL_TEXTURE_CUBE_MAP ? 6 : t->target == GL_TEXTURE_2D_ARRAY ? umax(1, t->d) : 1; }
// The sampled view's swizzle: its format's own (only where Vulkan's reading differs from GL's), then GL_TEXTURE_SWIZZLE_R..A over it
static VkComponentMapping tex_swz(const gtex_t *t) {
    const VkComponentSwizzle *f = g_fmt[t->fmt].swz; VkComponentSwizzle o[4];
    for (int c = 0; c < 4; c++) { GLint p = t->swz[c]; o[c] = p >= GL_RED && p <= GL_ALPHA ? f[p - GL_RED] : p == GL_ONE ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_ZERO; }
    return (VkComponentMapping){o[0], o[1], o[2], o[3]};
}
// Samplers are shared: one per distinct parameter set. shadow: for a sampler2DShadow (a depth comparison, LEQUAL unless the texture says otherwise)
typedef struct { GLint minf, magf, ws, wt, wr, lv, cmp; float aniso, b, bias, minlod, maxlod; int lin; } skey;
static struct { skey k; VkSampler s; } g_smp[256]; static int g_nsmp;
static VkSampler tex_sampler(gtex_t *t, int shadow) {
    if (!t->smp_dirty && t->smp && !shadow) return t->smp;
    int mips = t->levels > 1;
    skey k; memset(&k, 0, sizeof k);   // padding too: the cache compares whole keys
    k.minf = t->minf; k.magf = t->magf; k.ws = t->ws; k.wt = t->wt; k.wr = t->target == GL_TEXTURE_CUBE_MAP ? GL_CLAMP_TO_EDGE : t->wr;
    k.lv = mips ? (GLint)umin((uint32_t)t->maxlv, (uint32_t)t->levels - 1) : 0; k.cmp = t->cmp ? t->cmp : shadow ? 0x203 : 0; k.aniso = t->aniso;
    k.b = t->border[3] > 0.5f ? (t->border[0] > 0.5f ? 2.f : 1.f) : 0.f; k.bias = t->bias; k.minlod = t->minlod; k.maxlod = t->maxlod;
    k.lin = hvk_fmt_linear_filter(fmt_vk(t->fmt));
    for (int i = 0; i < g_nsmp; i++) if (!memcmp(&g_smp[i].k, &k, sizeof k)) { if (!shadow) { t->smp = g_smp[i].s; t->smp_dirty = 0; } return g_smp[i].s; }
    const VkPhysicalDeviceLimits *L = &hvk.dev.props.limits;
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = k.magf == GL_NEAREST || !k.lin ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;   // a format the device cannot filter linearly is filtered nearest (the backend's rule)
    ci.minFilter = !k.lin || k.minf == GL_NEAREST || k.minf == GL_NEAREST_MIPMAP_NEAREST || k.minf == GL_NEAREST_MIPMAP_LINEAR ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    // a texture of one level samples level 0 whatever its minifying filter (GL would call it incomplete with a mipmap filter and sample black): the level range ends at 0.25,
    // so the magnifying and minifying filters still apply as in GL
    int mipf = mips && k.minf != GL_NEAREST && k.minf != GL_LINEAR;
    ci.mipmapMode = mipf && (k.minf == GL_NEAREST_MIPMAP_LINEAR || k.minf == GL_LINEAR_MIPMAP_LINEAR) && k.lin ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = wrap_of(k.ws); ci.addressModeV = wrap_of(k.wt); ci.addressModeW = wrap_of(k.wr);
    ci.mipLodBias = k.bias < -L->maxSamplerLodBias ? -L->maxSamplerLodBias : k.bias > L->maxSamplerLodBias ? L->maxSamplerLodBias : k.bias;
    float an = fmaxf(1, fminf(fminf(16, L->maxSamplerAnisotropy), k.aniso));
    ci.anisotropyEnable = an > 1 && hvk.dev.enabled.samplerAnisotropy && k.lin; ci.maxAnisotropy = ci.anisotropyEnable ? an : 1;
    ci.compareEnable = k.cmp != 0; ci.compareOp = k.cmp ? (VkCompareOp)(k.cmp - GL_NEVER) : VK_COMPARE_OP_NEVER;
    ci.minLod = mipf ? fmaxf(0, k.minlod) : 0; ci.maxLod = mipf ? fminf((float)k.lv, k.maxlod) : 0.25f;
    if (ci.maxLod < ci.minLod) ci.maxLod = ci.minLod;
    ci.borderColor = k.b == 2.f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : k.b == 1.f ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler s = VK_NULL_HANDLE;
    if (vkCreateSampler(hvk.d, &ci, NULL, &s)) { RED1("a sampler could not be made"); return VK_NULL_HANDLE; }
    if (g_nsmp < 256) { g_smp[g_nsmp].k = k; g_smp[g_nsmp].s = s; g_nsmp++; } else RED1("more than 256 distinct sampler parameter sets (a sampler is made per texture from then on, never released)");
    if (!shadow) { t->smp = s; t->smp_dirty = 0; }
    return s;
}
static void lv_free(level_t *l) { free(l->data); memset(l, 0, sizeof *l); }
static void tex_drop(gtex_t *t) {   // the image, its views and the levels given so far: a new texture under the same name
    for (int f = 0; f < 6; f++) for (int l = 0; l < MAXLV; l++) lv_free(&t->pend[f][l]);
    if (t->view) defer(D_VIEW, t->use, H(t->view), 0, 0);
    gimg_drop(&t->im, t->use);
    t->view = VK_NULL_HANDLE; t->made = 0; t->defined = 0; t->levels = 0; t->gen++;
}
// Copy one level's (or a part's) packed bytes into the image through transient memory, recorded in cb (outside a pass). face: the cube face or array layer; z: a 3D slice
static void tex_copy_in(VkCommandBuffer cb, gtex_t *t, int face, int lv, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, const void *data, size_t pitch) {
    size_t bytes = pitch * fmt_rows(t->fmt, h); galloc st;
    if (!arena_alloc(bytes, 16, &st)) return;
    memcpy(st.cpu, data, bytes);
    VkImageAspectFlags a = g_fmt[t->fmt].kind == K_COL || g_fmt[t->fmt].kind == K_BLOCK ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    int vol = t->target == GL_TEXTURE_3D;
    VkBufferImageCopy r = {st.off, 0, 0, {a, (uint32_t)lv, vol ? 0u : (uint32_t)face, 1}, {(int32_t)x, (int32_t)y, vol ? (int32_t)z : 0}, {w, h, 1}};
    hvk_full_barrier(cb);   // after the image's earlier writes and reads in this command buffer
    vkCmdCopyBufferToImage(cb, st.buf, t->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
}
// Make the image from the levels given so far: as many levels as are given in a row from level 0 (and fit the size), or as glTexStorage said; their data goes in with the
// image's first layout change, at the start of the frame (the pre command buffer: nothing has read a new texture yet)
static int tex_make(gtex_t *t) {
    if (t->made) return t->im.img != VK_NULL_HANDLE;
    if (t->fmt < 0 || !t->w || !t->h || !g_on) return 0;
    int faces = tex_faces(t), lv = 0, vol = t->target == GL_TEXTURE_3D;
    uint32_t full = 1; for (uint32_t m = umax(umax(t->w, t->h), vol ? t->d : 1); m > 1; m >>= 1) full++;
    if (t->slevels) lv = (int)umin(umin((uint32_t)t->slevels, full), MAXLV);
    else { while (lv < MAXLV && lv < (int)full && (t->defined >> lv & 1)) lv++; if (!lv) lv = 1; }
    const fmt_t *m = &g_fmt[t->fmt]; VkFormat vf = fmt_vk(t->fmt); VkFormatFeatureFlags ff = fmt_feats(vf);
    VkImageUsageFlags us = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (m->kind == K_COL && (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) us |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if ((m->kind == K_DEPTH || m->kind == K_DS) && (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) us |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VkImageCreateFlags fl = (faces == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) | (vol && (us & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0) |
                            (unorm_of(vf) != vf ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0);
    if (!(ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) RED1("texture format %s (Vulkan %d) cannot be sampled on this device", m->name, vf);
    VkCommandBuffer cb = cb_pre();
    t->made = 1;
    if (!gimg_make(&t->im, cb, VK_IMAGE_LAYOUT_GENERAL, vol ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, vf, t->w, faces == 6 ? t->w : t->h, vol ? umax(1, t->d) : 1, tex_layers(t),
                   (uint32_t)lv, us, fl)) {
        GLOG("[RED] texture %ux%u %s (%d levels) could not be made", t->w, t->h, m->name, lv); return 0;
    }
    for (int f = 0; f < faces; f++) for (int l = 0; l < lv; l++) {
        level_t *p = &t->pend[f][l];
        if (p->data) { tex_copy_in(cb, t, f, l, 0, 0, 0, umax(1, t->w >> l), umax(1, (faces == 6 ? t->w : t->h) >> l), p->data, p->pitch); g_st.uploads_pre++; }
    }
    for (int f = 0; f < 6; f++) for (int l = 0; l < MAXLV; l++) lv_free(&t->pend[f][l]);
    t->levels = lv; t->smp_dirty = 1; t->view_dirty = 1;
    OLOG("texture %ux%u %s, %d levels%s", t->w, t->h, m->name, lv, faces == 6 ? ", cube" : "");
    return 1;
}
// Write one level's (or a part's) bytes into a made texture, in command order: before the frame's commands when the frame has not read the texture yet (the GPU's earlier
// reads, in earlier submissions, come before it: a full barrier starts every command buffer), else among them, after the reads so far (the pass ends there)
static void tex_write(gtex_t *t, int face, int lv, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, const void *data, size_t pitch) {
    if (!t->im.img) return;
    VkCommandBuffer cb;
    if (t->use >= g_serial) { pass_end(); cb = cb_main(); g_st.uploads_ordered++; }
    else { cb = cb_pre(); g_st.uploads_pre++; }
    tex_copy_in(cb, t, face, lv, x, y, z, w, h, data, pitch);
}
static VkImageView tex_view(gtex_t *t) {   // the sampled view: every level and layer, the swizzle; made again after a swizzle changes
    if (t->view && !t->view_dirty) return t->view;
    if (t->view) defer(D_VIEW, t->use, H(t->view), 0, 0);
    VkImageViewType vt = t->target == GL_TEXTURE_CUBE_MAP ? VK_IMAGE_VIEW_TYPE_CUBE : t->target == GL_TEXTURE_3D ? VK_IMAGE_VIEW_TYPE_3D :
                         t->target == GL_TEXTURE_2D_ARRAY ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    VkImageAspectFlags a = has_depth(t->im.vf) ? VK_IMAGE_ASPECT_DEPTH_BIT : has_stencil(t->im.vf) ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    t->view = view_make(t->im.img, vt, t->im.vf, a, 0, (uint32_t)t->levels, 0, tex_layers(t), tex_swz(t)); t->view_dirty = 0;
    return t->view;
}
// glTexImage2D / glCompressedTexImage2D: the level's bytes, converted
static void tex_image(GLenum target, GLint level, int f, uint32_t w, uint32_t h, void *conv, size_t pitch) {
    gtex_t *t = target == GL_TEXTURE_2D || (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) ? bound_tex(target) : NULL;
    int face = target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
    if (!t || level < 0 || level >= MAXLV) { RED1("glTexImage2D target %#x level %d with no texture bound", target, level); free(conv); return; }
    uint32_t w0 = level ? w << level : w, h0 = level ? h << level : h;
    if (level == 0 && (t->fmt != f || t->w != w || t->h != h) && (t->made || t->defined)) tex_drop(t);   // level 0 respecified: a new texture
    if (!t->made && !t->defined) { t->fmt = f; t->w = level ? umax(w0, 1) : w; t->h = level ? umax(h0, 1) : h; }
    if (t->fmt != f) { RED1("texture level %d in format %s, its level 0 in %s", level, g_fmt[f].name, g_fmt[t->fmt].name); free(conv); return; }
    if (t->made && level < t->levels && t->im.img) { if (conv) tex_write(t, face, level, 0, 0, 0, w, h, conv, pitch); free(conv); return; }
    if (t->made) { RED1("texture level %d given after the texture was made with %d levels: ignored", level, t->levels); free(conv); return; }
    level_t *l = &t->pend[face][level]; lv_free(l);
    *l = (level_t){conv, pitch, pitch * fmt_rows(f, h), w, h};
    int faces = t->target == GL_TEXTURE_CUBE_MAP ? 6 : 1, all = 1;
    for (int k = 0; k < faces; k++) all &= t->pend[k][level].data != NULL || (level == 0 && !conv);
    if (all) t->defined |= 1 << level;
}

// The textures a sampler reads when its unit has none (or one that cannot be made): opaque black, as GL samples an incomplete texture; a depth one for shadow samplers
static gimg_t g_dummy[5]; static VkImageView g_dummy_view[5]; static VkSampler g_samp0, g_samp0_cmp;
static void dummies_make(void) {
    static const VkComponentMapping id = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    VkCommandBuffer cb = cb_pre();
    const VkImageUsageFlags us = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    gimg_make(&g_dummy[0], cb, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, 1, us, 0);
    gimg_make(&g_dummy[1], cb, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 6, 1, us, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
    gimg_make(&g_dummy[2], cb, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, 1, us, 0);
    gimg_make(&g_dummy[3], cb, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, 1, us, 0);
    gimg_make(&g_dummy[4], cb, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_TYPE_2D, VK_FORMAT_D32_SFLOAT, 1, 1, 1, 1, 1, us, 0);
    hvk_full_barrier(cb);
    VkClearColorValue black = {{0, 0, 0, 1}}; VkClearDepthStencilValue z = {0, 0};
    for (int i = 0; i < 4; i++) {
        VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, i == 1 ? 6u : 1u};
        if (g_dummy[i].img) vkCmdClearColorImage(cb, g_dummy[i].img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &r);
    }
    VkImageSubresourceRange rd = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (g_dummy[4].img) vkCmdClearDepthStencilImage(cb, g_dummy[4].img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rd);
    static const VkImageViewType vt[5] = {VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_2D};
    for (int i = 0; i < 5; i++) if (g_dummy[i].img) g_dummy_view[i] = view_make(g_dummy[i].img, vt[i], g_dummy[i].vf, i == 4 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, i == 1 ? 6 : 1, id);
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT,
                              VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, 0, VK_FALSE, 1, VK_FALSE, VK_COMPARE_OP_NEVER, 0, 0.25f, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_FALSE};
    vkCreateSampler(hvk.d, &si, NULL, &g_samp0);
    si.compareEnable = VK_TRUE; si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    vkCreateSampler(hvk.d, &si, NULL, &g_samp0_cmp);
}

// ---------- Render passes ----------
// The pass being recorded: framebuffer name, its attachments' generation (a changed attachment ends the pass), GL_FRAMEBUFFER_SRGB (the attachment views' formats), its formats;
// clears waiting for the pass to begin
static struct { GLuint fb; uint64_t gen; int open, srgb; uint32_t w, h; VkFormat cpf, dpf, spf; } P;
static struct { int bits; float c[4]; double d; int s; GLuint fb; } g_pend_clear;
// What the command buffer has set so far (-1 / NAN / VK_NULL_HANDLE = unset): the draws set only what changed
static struct { VkPipeline pso; int cull, front, topo, restart, dt, dw, dop, st, sref, scm, swm, sops[4], dbe; int vp[4], sc[4]; float pf, pu; } E;
static void enc_reset(void) {
    memset(&E, 0xff, sizeof E); E.pso = VK_NULL_HANDLE; E.pf = E.pu = NAN;
}
// A pass's attachment: the image, the view to render into, its level's size; win: 1 + the window image's index
typedef struct { gimg_t *im; VkImageView view; VkFormat vf; uint32_t w, h; int win; } attach_t;
static int att_get(const att_t *a, uint64_t *gen, attach_t *o, int srgb) {
    memset(o, 0, sizeof *o);
    gimg_t *im = NULL; int level = a->level, layer = a->face;
    if (a->kind == 1) {
        gtex_t *t = tex_of(a->name); if (!t) return 0;
        if (!t->made) tex_make(t);
        *gen += t->gen; t->use = g_serial; im = &t->im;
    } else if (a->kind == 2) {
        grb_t *r = rb_of(a->name); if (!r) return 0;
        *gen += r->gen; r->use = g_serial; im = &r->im; level = layer = 0;
    }
    if (!im || !im->img) return 0;
    VkFormat f = srgb ? im->vf : unorm_of(im->vf);   // desktop GL writes an sRGB attachment as it is unless GL_FRAMEBUFFER_SRGB is on
    o->im = im; o->vf = f; o->view = gimg_att_view(im, level, layer, f); o->w = umax(1, im->w >> level); o->h = umax(1, im->h >> level);
    return o->view != VK_NULL_HANDLE;
}
static uint64_t fb_gen(GLuint fb) {
    gfb_t *f = fb_of(fb); if (!f) return 0;
    uint64_t g = f->gen; attach_t o;
    att_get(&f->col, &g, &o, C.srgb); att_get(&f->depth, &g, &o, 1); att_get(&f->stencil, &g, &o, 1);
    return g;
}
// The size of a framebuffer (its first attachment's level), 0 when it has none
static void fb_size(GLuint fb, uint32_t *w, uint32_t *h) {
    *w = *h = 0;
    if (!fb) { win_ensure(); *w = g_win.w; *h = g_win.h; return; }
    gfb_t *f = fb_of(fb); if (!f) return;
    const att_t *a = f->col.kind ? &f->col : f->depth.kind ? &f->depth : &f->stencil; uint64_t g = 0; attach_t o;
    if (att_get(a, &g, &o, 1)) { *w = o.w; *h = o.h; }
}
static void pass_end(void) {
    if (P.open) { vkCmdEndRendering(g_slot[g_cur].main); P.open = 0; }
}
static void pass_begin(void) {
    if (!g_on) return;
    VkCommandBuffer cb = cb_main();
    attach_t c = {0}, d = {0}, s = {0}; uint64_t gen = 0;
    if (!C.fb) {
        win_ensure();
        if (!g_win.made) return;
        int k = g_win.cur;
        c = (attach_t){&g_win.col[k], gimg_att_view(&g_win.col[k], 0, 0, VK_FORMAT_R8G8B8A8_UNORM), VK_FORMAT_R8G8B8A8_UNORM, g_win.w, g_win.h, 1 + k};
        d = s = (attach_t){&g_win.ds, gimg_att_view(&g_win.ds, 0, 0, g_win.ds.vf), g_win.ds.vf, g_win.w, g_win.h, 0};
        win_general(cb);
    } else {
        gfb_t *f = fb_of(C.fb);
        if (f) { att_get(&f->col, &gen, &c, C.srgb); att_get(&f->depth, &gen, &d, 1); att_get(&f->stencil, &gen, &s, 1); gen += f->gen; }
        if (c.view && aspects_of(c.vf) != VK_IMAGE_ASPECT_COLOR_BIT) memset(&c, 0, sizeof c);
        if (d.view && !has_depth(d.vf)) memset(&d, 0, sizeof d);
        if (s.view && !has_stencil(s.vf)) memset(&s, 0, sizeof s);
        // a depth / stencil texture attached as GL's depth attachment alone (or stencil alone) is both: its format has both, and the pipelines say so
        if (d.view && !s.view && has_stencil(d.vf)) s = d;
        else if (s.view && !d.view && has_depth(s.vf)) d = s;
    }
    if (!c.view && !d.view && !s.view) { RED1("framebuffer %u has no usable attachment", C.fb); return; }
    hvk_full_barrier(cb);
    int pc = g_pend_clear.fb == C.fb ? g_pend_clear.bits : 0;
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, c.view, VK_IMAGE_LAYOUT_GENERAL, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE,
                                    VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingAttachmentInfo da = ca, sa = ca;
    if (pc & GL_COLOR_BUFFER_BIT) { ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; memcpy(ca.clearValue.color.float32, g_pend_clear.c, 16); }
    da.imageView = d.view; sa.imageView = s.view;
    if (pc & GL_DEPTH_BUFFER_BIT) { da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; da.clearValue.depthStencil.depth = (float)g_pend_clear.d; }
    if (pc & GL_STENCIL_BUFFER_BIT) { sa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; sa.clearValue.depthStencil.stencil = (uint32_t)g_pend_clear.s & 0xff; }
    if (g_pend_clear.fb == C.fb) { g_st.clears_load += g_pend_clear.bits != 0; g_pend_clear.bits = 0; }
    // the render area: what every attachment covers (GL draws into their intersection)
    uint32_t w = UINT32_MAX, h = UINT32_MAX;
    if (c.view) { w = umin(w, c.w); h = umin(h, c.h); }
    if (d.view) { w = umin(w, d.w); h = umin(h, d.h); }
    if (s.view) { w = umin(w, s.w); h = umin(h, s.h); }
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {w, h}}, 1, 0, c.view ? 1u : 0u, &ca, d.view ? &da : NULL, s.view ? &sa : NULL};
    vkCmdBeginRendering(cb, &ri);
    P.w = w; P.h = h; P.cpf = c.view ? c.vf : VK_FORMAT_UNDEFINED; P.dpf = d.view ? d.vf : VK_FORMAT_UNDEFINED; P.spf = s.view ? s.vf : VK_FORMAT_UNDEFINED;
    P.fb = C.fb; P.gen = gen; P.srgb = C.srgb; P.open = 1; g_st.passes++;
    enc_reset();
}
// The pass for the bound framebuffer (a new one when the binding, an attachment or GL_FRAMEBUFFER_SRGB changed)
static int pass_ensure(void) {
    if (!g_on) return 0;
    if (P.open && P.fb == C.fb && P.srgb == C.srgb && (!C.fb || fb_gen(C.fb) == P.gen)) return 1;
    pass_end(); pass_begin();
    return P.open;
}
// Clears no draw followed: a pass that only clears
static void clears_flush(void) {
    if (g_pend_clear.bits && g_on) { GLuint keep = C.fb; C.fb = g_pend_clear.fb; pass_end(); pass_begin(); pass_end(); C.fb = keep; }
    g_pend_clear.bits = 0;
}

// ---------- Pipelines ----------
static VkFormat vfmt_of(const vattr_t *a, int cls) {   // cls: the input's numeric type (in_class); 0 when Vulkan has no format for it
    int n = a->size < 1 ? 1 : a->size > 4 ? 4 : a->size, no = a->norm;
    if (a->bgra) return a->type == GL_UNSIGNED_BYTE && no && !cls ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_UNDEFINED;
#define FMT4(p, s) {VK_FORMAT_R##p##_##s, VK_FORMAT_R##p##G##p##_##s, VK_FORMAT_R##p##G##p##B##p##_##s, VK_FORMAT_R##p##G##p##B##p##A##p##_##s}
    static const VkFormat f32[] = FMT4(32, SFLOAT), f16[] = FMT4(16, SFLOAT), ubn[] = FMT4(8, UNORM), ubs[] = FMT4(8, USCALED), ubi[] = FMT4(8, UINT), sbn[] = FMT4(8, SNORM),
        sbs[] = FMT4(8, SSCALED), sbi[] = FMT4(8, SINT), usn[] = FMT4(16, UNORM), uss[] = FMT4(16, USCALED), usi[] = FMT4(16, UINT), ssn[] = FMT4(16, SNORM), sss[] = FMT4(16, SSCALED),
        ssi[] = FMT4(16, SINT), u32[] = FMT4(32, UINT), s32[] = FMT4(32, SINT);
#undef FMT4
    // an integer input reads the integer format of its own signedness (Vulkan matches the numeric type); a float one the normalized or scaled format
    switch (a->type) {
        case GL_FLOAT: return cls ? VK_FORMAT_UNDEFINED : f32[n - 1];
        case GL_HALF_FLOAT: return cls ? VK_FORMAT_UNDEFINED : f16[n - 1];
        case GL_UNSIGNED_BYTE: case GL_BYTE: {
            if (cls) return cls == 1 ? sbi[n - 1] : ubi[n - 1];
            int u = a->type == GL_UNSIGNED_BYTE; return no ? (u ? ubn : sbn)[n - 1] : (u ? ubs : sbs)[n - 1];
        }
        case GL_UNSIGNED_SHORT: case GL_SHORT: {
            if (cls) return cls == 1 ? ssi[n - 1] : usi[n - 1];
            int u = a->type == GL_UNSIGNED_SHORT; return no ? (u ? usn : ssn)[n - 1] : (u ? uss : sss)[n - 1];
        }
        case GL_UNSIGNED_INT: case GL_INT: return cls == 1 ? s32[n - 1] : cls == 2 ? u32[n - 1] : VK_FORMAT_UNDEFINED;   // Vulkan has no 32-bit scaled format
    }
    return VK_FORMAT_UNDEFINED;
}
static int vtype_bytes(GLenum t) { return t == GL_FLOAT || t == GL_INT || t == GL_UNSIGNED_INT ? 4 : t == GL_HALF_FLOAT || t == GL_SHORT || t == GL_UNSIGNED_SHORT ? 2 : 1; }
static VkBlendFactor bf_of(GLenum f) {
    switch (f) {
        case 0: return VK_BLEND_FACTOR_ZERO; case 1: return VK_BLEND_FACTOR_ONE;
        case GL_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR; case GL_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case GL_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA; case GL_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case GL_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA; case GL_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case GL_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR; case GL_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case GL_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    }
    RED1("blend factor %#x not mapped", f); return VK_BLEND_FACTOR_ONE;
}
static uint8_t beq_of(GLenum e) {
    switch (e) { case GL_FUNC_SUBTRACT: return VK_BLEND_OP_SUBTRACT; case GL_FUNC_REVERSE_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
                 case GL_MIN: return VK_BLEND_OP_MIN; case GL_MAX: return VK_BLEND_OP_MAX; default: return VK_BLEND_OP_ADD; }
}
static uint8_t bcode(GLenum f) { return f < 2 ? (uint8_t)f : f >= GL_SRC_COLOR && f <= GL_SRC_ALPHA_SATURATE ? (uint8_t)(f - GL_SRC_COLOR + 2) : f >= 0x8001 && f <= 0x8004 ? (uint8_t)(f - 0x8001 + 11) : 15; }
static GLenum bdecode(uint8_t c) { return c < 2 ? c : c < 11 ? GL_SRC_COLOR + c - 2 : c < 15 ? 0x8001 + c - 11 : 1; }
static VkStencilOp sop_of(GLenum g) {
    switch (g) { case 0: return VK_STENCIL_OP_ZERO; case GL_REPLACE: return VK_STENCIL_OP_REPLACE; case GL_INCR: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
                 case GL_DECR: return VK_STENCIL_OP_DECREMENT_AND_CLAMP; case GL_INVERT: return VK_STENCIL_OP_INVERT; case GL_INCR_WRAP: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
                 case GL_DECR_WRAP: return VK_STENCIL_OP_DECREMENT_AND_WRAP; default: return VK_STENCIL_OP_KEEP; }
}
// The dynamic states of every pipeline of the layer (Vulkan 1.3's core): a draw sets them from GL's state, so a pipeline holds only what its key says
static const VkDynamicState g_dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
    VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
    VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS};
static VkPipeline pso_get(gprog_t *p, const pkey_t *k) {
    for (int i = 0; i < p->npc; i++) if (!memcmp(&p->pc[i].k, k, sizeof *k)) return p->pc[i].p;
    const shinfo *vs = p->si[0];
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, p->mod[0], "main", NULL},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, p->mod[1], "main", NULL}};
    VkVertexInputBindingDescription vb[16]; VkVertexInputAttributeDescription va[16]; VkVertexInputBindingDivisorDescriptionKHR vd[16]; uint32_t nv = 0, nd = 0;
    for (int i = 0; i < vs->na; i++) {   // each input its own binding, at its location
        uint32_t s = vs->a[i].slot;
        vb[nv] = (VkVertexInputBindingDescription){s, k->stride[s], k->div[s] ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        va[nv] = (VkVertexInputAttributeDescription){s, s, (VkFormat)k->vf[s], 0};
        if (k->div[s] > 1 && hvk.dev.x.vertex_divisor) vd[nd++] = (VkVertexInputBindingDivisorDescriptionKHR){s, k->div[s]};
        nv++;
    }
    VkPipelineVertexInputDivisorStateCreateInfoKHR dv = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR, NULL, nd, vd};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nd ? &dv : NULL, 0, nv, vb, nv, va};
    static const VkPrimitiveTopology rep[3] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, rep[k->topo < 3 ? k->topo : 2], VK_FALSE};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPolygonMode pm = k->poly == 1 ? VK_POLYGON_MODE_LINE : k->poly == 2 ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_FALSE, pm, VK_CULL_MODE_NONE,
                                                 VK_FRONT_FACE_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState ba = {k->blend, bf_of(bdecode(k->bsrc)), bf_of(bdecode(k->bdst)), (VkBlendOp)k->eqc, bf_of(bdecode(k->bsa)), bf_of(bdecode(k->bda)),
                                              (VkBlendOp)k->eqa, k->mask};
    if (!k->blend) { ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; ba.colorBlendOp = ba.alphaBlendOp = VK_BLEND_OP_ADD; }
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, k->cpf ? 1u : 0u, &ba, {0, 0, 0, 0}};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, sizeof g_dyn / sizeof *g_dyn, g_dyn};
    VkFormat cf = (VkFormat)k->cpf;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, k->cpf ? 1u : 0u, &cf, (VkFormat)k->dpf, (VkFormat)k->spf};
    VkPipelineCreationFeedback fb = {0};
    VkPipelineCreationFeedbackCreateInfo fci = {VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO, &ri, &fb, 0, NULL};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &fci, 0, 2, st, &vi, &ia, NULL, &vp, &rs, &ms, &ds, &cbs, &dy, p->pl, VK_NULL_HANDLE, 0,
                                       VK_NULL_HANDLE, -1};
    VkPipeline pso = VK_NULL_HANDLE;
    VkPipelineCache pc = hvk_cache_begin();   // the backend's disk cache (vk_cache.c, design 3.4)
    VkResult r = vkCreateGraphicsPipelines(hvk.d, pc, 1, &ci, NULL, &pso);
    hvk_cache_end(&fb, r == VK_SUCCESS);
    if (r) { static int n; pso = VK_NULL_HANDLE; if (n++ < 20) GLOG("[RED] cannot build the pipeline (vs %s, fs %s): VkResult %d", p->si[0]->key, p->si[1]->key, r); }
    else g_st.pipelines++;
    if (p->npc == p->cappc) { p->cappc = p->cappc ? p->cappc * 2 : 4; p->pc = realloc(p->pc, (size_t)p->cappc * sizeof *p->pc); if (!p->pc) die("[gl] out of memory"); }
    p->pc[p->npc].k = *k; p->pc[p->npc].p = pso; p->npc++;
    return pso;
}

// ---------- Drawing ----------
// The dynamic state of a draw from GL's: the viewport and scissor as GL gives them (rows are GL's rows, see the file header), the winding reversed, the rest as it is
static void enc_state(VkCommandBuffer cb, VkPrimitiveTopology topo, int strip) {
    int vp[4] = {C.vp[0], C.vp[1], C.vp[2], C.vp[3]};
    if (!vp[2] && !vp[3]) { vp[2] = (int)P.w; vp[3] = (int)P.h; }
    if (memcmp(vp, E.vp, sizeof vp)) { memcpy(E.vp, vp, sizeof vp); VkViewport v = {(float)vp[0], (float)vp[1], (float)(vp[2] > 0 ? vp[2] : 1), (float)(vp[3] > 0 ? vp[3] : 1), 0, 1}; vkCmdSetViewport(cb, 0, 1, &v); }
    int sc[4] = {0, 0, (int)P.w, (int)P.h};
    if (C.scissor) {
        int x0 = C.sc[0] < 0 ? 0 : C.sc[0], y0 = C.sc[1] < 0 ? 0 : C.sc[1], x1 = C.sc[0] + C.sc[2], y1 = C.sc[1] + C.sc[3];
        if (x1 > (int)P.w) x1 = (int)P.w;
        if (y1 > (int)P.h) y1 = (int)P.h;
        sc[0] = x0 < (int)P.w ? x0 : (int)P.w; sc[1] = y0 < (int)P.h ? y0 : (int)P.h; sc[2] = x1 > sc[0] ? x1 - sc[0] : 0; sc[3] = y1 > sc[1] ? y1 - sc[1] : 0;
    }
    if (memcmp(sc, E.sc, sizeof sc)) { memcpy(E.sc, sc, sizeof sc); VkRect2D r = {{sc[0], sc[1]}, {(uint32_t)sc[2], (uint32_t)sc[3]}}; vkCmdSetScissor(cb, 0, 1, &r); }
    int cull = !C.cull ? VK_CULL_MODE_NONE : C.cull_face == GL_FRONT ? VK_CULL_MODE_FRONT_BIT : C.cull_face == GL_BACK ? VK_CULL_MODE_BACK_BIT : C.cull_face == GL_FRONT_AND_BACK ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE;
    if (cull != E.cull) { E.cull = cull; vkCmdSetCullMode(cb, (VkCullModeFlags)cull); }
    int front = C.front == GL_CW ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;   // GL's winding, reversed: rows are GL's, Vulkan counts the area with y down
    if (front != E.front) { E.front = front; vkCmdSetFrontFace(cb, (VkFrontFace)front); }
    if ((int)topo != E.topo) { E.topo = (int)topo; vkCmdSetPrimitiveTopology(cb, topo); }
    int restart = strip && C.restart;   // GL_PRIMITIVE_RESTART_FIXED_INDEX; a list restarts only with primitiveTopologyListRestart, which GL's lists do not need
    if (restart != E.restart) { E.restart = restart; vkCmdSetPrimitiveRestartEnable(cb, (VkBool32)restart); }
    int dt = C.depth && P.dpf, dw = dt && C.dmask, dop = (int)(C.dfunc - GL_NEVER);
    if (dt != E.dt) { E.dt = dt; vkCmdSetDepthTestEnable(cb, (VkBool32)dt); }
    if (dw != E.dw) { E.dw = dw; vkCmdSetDepthWriteEnable(cb, (VkBool32)dw); }
    if (dop != E.dop) { E.dop = dop; vkCmdSetDepthCompareOp(cb, (VkCompareOp)dop); }
    int stt = C.stencil && P.spf;
    if (stt != E.st) { E.st = stt; vkCmdSetStencilTestEnable(cb, (VkBool32)stt); }
    int ops[4] = {(int)sop_of(C.sfail), (int)sop_of(C.spass), (int)sop_of(C.szfail), (int)(C.sfunc - GL_NEVER)};
    if (memcmp(ops, E.sops, sizeof ops)) { memcpy(E.sops, ops, sizeof ops); vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, (VkStencilOp)ops[0], (VkStencilOp)ops[1], (VkStencilOp)ops[2], (VkCompareOp)ops[3]); }
    int scm = (int)(C.smask & 0xff), swm = (int)(C.swmask & 0xff), sref = (int)(C.sref & 0xff);
    if (scm != E.scm) { E.scm = scm; vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, (uint32_t)scm); }
    if (swm != E.swm) { E.swm = swm; vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, (uint32_t)swm); }
    if (sref != E.sref) { E.sref = sref; vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, (uint32_t)sref); }
    float pf = C.poffset ? C.pfactor : 0, pu = C.poffset ? C.punits : 0; int dbe = C.poffset != 0;
    if (dbe != E.dbe) { E.dbe = dbe; vkCmdSetDepthBiasEnable(cb, (VkBool32)dbe); }
    if (pf != E.pf || pu != E.pu) { E.pf = pf; E.pu = pu; vkCmdSetDepthBias(cb, pu, 0, pf); }
}
// The program's descriptor sets: per stage, its default uniform block (copied into transient memory when it changed or the submission did) and its samplers' textures,
// a set made again when what it holds changed, bound with the block's offset
static int bind_resources(gprog_t *p, VkCommandBuffer cb) {
    for (int st = 0; st < 2; st++) {
        const shinfo *si = p->si[st];
        if (!p->nres[st]) continue;
        int has_ub = si->ub_slot >= 0 && si->ub_size > 0; uint32_t dyn = 0;
        if (has_ub) {
            if (p->ubm_serial[st] != g_serial || p->ubm_gen[st] != p->ub_gen[st]) {
                galloc g; if (!arena_alloc((VkDeviceSize)si->ub_size, g_ubo_align, &g)) return 0;
                memcpy(g.cpu, p->ub[st], (size_t)si->ub_size); p->ubm[st] = g; p->ubm_serial[st] = g_serial; p->ubm_gen[st] = p->ub_gen[st];
            }
            dyn = (uint32_t)p->ubm[st].off;
        }
        uint64_t *sig = p->sig[st]; int ns = 0;
        VkDescriptorImageInfo ii[64]; uint32_t ib[64]; int ni = 0; uint64_t now[1 + 2 * 64];
        now[ns++] = has_ub ? H(p->ubm[st].buf) : 0;
        for (int i = 0; i < p->nu && ni < 64; i++) {
            puni_t *u = &p->u[i];
            if (!is_samp(u->type) || u->slot[st] < 0) continue;
            int unit = u->unit < 0 || u->unit >= NUNITS ? 0 : u->unit, shadow = u->type == T_SAMP2DSH;
            GLenum target = u->type == T_SAMPCUBE ? GL_TEXTURE_CUBE_MAP : u->type == T_SAMP3D ? GL_TEXTURE_3D : u->type == T_SAMP2DARR ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
            gtex_t *t = tex_of(*unit_slot(unit, target));
            VkImageView v = VK_NULL_HANDLE; VkSampler sm = VK_NULL_HANDLE;
            if (t && (t->made || tex_make(t)) && t->im.img && (!shadow || has_depth(t->im.vf))) { v = tex_view(t); sm = tex_sampler(t, shadow); t->use = g_serial; }
            if (!v || !sm) {
                int d = shadow ? 4 : u->type == T_SAMPCUBE ? 1 : u->type == T_SAMP3D ? 2 : u->type == T_SAMP2DARR ? 3 : 0;
                v = g_dummy_view[d]; sm = shadow ? g_samp0_cmp : g_samp0;
            }
            ii[ni] = (VkDescriptorImageInfo){sm, v, VK_IMAGE_LAYOUT_GENERAL}; ib[ni++] = (uint32_t)u->slot[st];
            now[ns++] = H(v); now[ns++] = H(sm);
        }
        if (p->set_serial[st] != g_serial || memcmp(now, sig, (size_t)ns * 8)) {
            VkDescriptorSet set = set_alloc(p->dsl[st]);
            if (!set) return 0;
            VkWriteDescriptorSet w[65]; int nw = 0;
            VkDescriptorBufferInfo bi = {has_ub ? p->ubm[st].buf : VK_NULL_HANDLE, 0, (VkDeviceSize)si->ub_size};
            if (has_ub) w[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, (uint32_t)si->ub_slot, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, NULL, &bi, NULL};
            for (int i = 0; i < ni; i++) w[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, ib[i], 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[i], NULL, NULL};
            vkUpdateDescriptorSets(hvk.d, (uint32_t)nw, w, 0, NULL);
            p->set[st] = set; p->set_serial[st] = g_serial; memcpy(sig, now, (size_t)ns * 8);
        }
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pl, (uint32_t)st, 1, &p->set[st], has_ub ? 1u : 0u, &dyn);
    }
    return 1;
}
// Everything a draw needs; 0 = skip it. topo: the draw's topology, class its class (0 points, 1 lines, 2 triangles)
static int draw_setup(VkPrimitiveTopology topo, int cls, int strip) {
    gprog_t *p = prog_of(C.prog);
    if (!p || !p->linked) { RED1("draw with no usable program (%u)", C.prog); return 0; }
    if (!pass_ensure()) return 0;
    pkey_t k; memset(&k, 0, sizeof k);
    vao_t *v = cur_vao(); const shinfo *vs = p->si[0];
    for (int i = 0; i < vs->na; i++) {
        int s = vs->a[i].slot, gi = p->amap[s], c = in_class(vs->a[i].type); vattr_t *a = &v->a[gi];
        if (!a->on) { k.vf[s] = c == 1 ? VK_FORMAT_R32G32B32A32_SINT : c == 2 ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT; continue; }   // its current value, stride 0
        gbuf_t *b = buf_of(a->buf);
        if (!b || !b->m.buf) { RED1("vertex attribute %d reads client memory or a buffer with no data (not implemented)", gi); return 0; }
        VkFormat f = vfmt_of(a, c);
        if (!f) { RED1("vertex attribute %d: type %#x size %d into a %s input has no Vulkan format (a 32-bit integer read as float, or a float read as integer)", gi, a->type, a->size, g_tnames[vs->a[i].type < T_OTHER ? vs->a[i].type : 0]); return 0; }
        if (!(fmt_buf_feats(f) & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) { RED1("vertex format %d (attribute %d, type %#x size %d): the device cannot fetch it", f, gi, a->type, a->size); return 0; }
        k.vf[s] = (uint32_t)f; k.stride[s] = (uint16_t)(a->stride ? a->stride : a->size * vtype_bytes(a->type)); k.div[s] = a->div;
        if (a->div > 1 && !hvk.dev.x.vertex_divisor) RED1("vertex attribute divisor %u: the device has no VK_KHR_vertex_attribute_divisor (read per instance)", a->div);
    }
    k.cpf = P.cpf; k.dpf = P.dpf; k.spf = P.spf; k.mask = C.cmask; k.topo = (uint8_t)cls; k.poly = C.poly == GL_LINE ? 1 : C.poly == GL_POINT ? 2 : 0;
    if (C.blend && P.cpf) { k.blend = 1; k.bsrc = bcode(C.bsrc); k.bdst = bcode(C.bdst); k.bsa = bcode(C.bsa); k.bda = bcode(C.bda); k.eqc = beq_of(C.eqc); k.eqa = beq_of(C.eqa); }
    VkPipeline pso = pso_get(p, &k);
    if (!pso) return 0;
    VkCommandBuffer cb = cb_main();
    if (pso != E.pso) { E.pso = pso; vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pso); }
    enc_state(cb, topo, strip);
    for (int i = 0; i < vs->na; i++) {
        int s = vs->a[i].slot, gi = p->amap[s]; vattr_t *a = &v->a[gi];
        if (a->on) { gbuf_t *b = buf_of(a->buf); b->use = g_serial; VkDeviceSize o = b->m.off + a->off; vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &b->m.buf, &o); continue; }
        galloc g; if (!arena_alloc(16, 16, &g)) return 0;
        int c = in_class(vs->a[i].type);
        if (c) { int32_t iv[4]; for (int j = 0; j < 4; j++) iv[j] = (int32_t)C.va[gi][j]; memcpy(g.cpu, iv, 16); } else memcpy(g.cpu, C.va[gi], 16);
        vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &g.buf, &g.off);
    }
    if (!bind_resources(p, cb)) return 0;
    p->use = g_serial; g_st.draws++;
    return 1;
}
// A GL primitive mode as a Vulkan topology and its class; conv: 1 line loop, 3 quads (made lists of indices), -1 not implemented
static VkPrimitiveTopology prim_of(GLenum m, int *cls, int *conv, int *strip) {
    *conv = 0; *strip = 0;
    switch (m) {
        case GL_POINTS: *cls = 0; return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case GL_LINES: *cls = 1; return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case GL_LINE_STRIP: *cls = 1; *strip = 1; return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case GL_LINE_LOOP: *cls = 1; *conv = 1; return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case GL_TRIANGLES: *cls = 2; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case GL_TRIANGLE_STRIP: *cls = 2; *strip = 1; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case GL_TRIANGLE_FAN: *cls = 2; *strip = 1; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;   // native in Vulkan (Metal converts it)
        case GL_QUADS: *cls = 2; *conv = 3; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
    RED1("primitive mode %#x not implemented", m); *cls = 2; *conv = -1; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}
// Indices of a mode Vulkan does not have (line loop, quads) as a list, from the given indices (or base … base + n − 1); returns the new count
static uint32_t idx_convert(int conv, const uint32_t *in, uint32_t n, uint32_t base, uint32_t *out) {
    uint32_t m = 0;
#define IX(i) (in ? in[i] : base + (i))
    if (conv == 1) { for (uint32_t i = 0; i < n; i++) out[m++] = IX(i); if (n) out[m++] = IX(0); }
    else if (conv == 3) for (uint32_t i = 0; i + 3 < n; i += 4) { out[m++] = IX(i); out[m++] = IX(i + 1); out[m++] = IX(i + 2); out[m++] = IX(i); out[m++] = IX(i + 2); out[m++] = IX(i + 3); }
#undef IX
    return m;
}
static void draw_arrays(GLenum mode, GLint first, GLsizei count, GLsizei inst) {
    if (count <= 0 || inst <= 0 || !g_on) return;
    int cls, conv, strip; VkPrimitiveTopology topo = prim_of(mode, &cls, &conv, &strip);
    if (conv < 0 || !draw_setup(topo, cls, strip)) return;
    VkCommandBuffer cb = cb_main();
    if (!conv) { vkCmdDraw(cb, (uint32_t)count, (uint32_t)inst, (uint32_t)first, 0); return; }
    galloc g; if (!arena_alloc((VkDeviceSize)count * 6 * 4 + 16, 16, &g)) return;
    uint32_t n = idx_convert(conv, NULL, (uint32_t)count, (uint32_t)first, (uint32_t *)g.cpu);
    if (n) { vkCmdBindIndexBuffer(cb, g.buf, g.off, VK_INDEX_TYPE_UINT32); vkCmdDrawIndexed(cb, n, (uint32_t)inst, 0, 0, 0); }
}
// base: added to every index (glDrawElementsBaseVertex: LÖVE draws its batched quads with one shared index buffer so)
static void draw_elements(GLenum mode, GLsizei count, GLenum type, const void *indices, GLint base, GLsizei inst) {
    if (count <= 0 || inst <= 0 || !g_on) return;
    int cls, conv, strip; VkPrimitiveTopology topo = prim_of(mode, &cls, &conv, &strip);
    if (conv < 0) return;
    gbuf_t *eb = buf_of(cur_vao()->ebo);
    uintptr_t io = (uintptr_t)indices; int isz = type == GL_UNSIGNED_INT ? 4 : type == GL_UNSIGNED_SHORT ? 2 : 1;
    const uint8_t *src = eb ? (eb->m.map ? eb->m.map + io : NULL) : (const uint8_t *)indices;
    if (!src || (eb && io + (size_t)count * isz > eb->size)) { RED1("glDrawElements: indices outside their buffer"); return; }
    if (!draw_setup(topo, cls, strip)) return;
    VkCommandBuffer cb = cb_main();
    if (!conv && isz > 1 && eb && io % (uintptr_t)isz == 0) {   // the game's own index buffer
        eb->use = g_serial;
        vkCmdBindIndexBuffer(cb, eb->m.buf, eb->m.off + io, isz == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cb, (uint32_t)count, (uint32_t)inst, 0, base, 0);
        return;
    }
    // client indices, 8-bit indices, an unaligned offset or a converted mode: 32-bit indices in transient memory
    galloc g; if (!arena_alloc((VkDeviceSize)count * 4 * (conv ? 6 : 1) + 16, 16, &g)) return;
    uint32_t *tmp = conv ? malloc((size_t)count * 4) : (uint32_t *)g.cpu;
    if (!tmp) return;
    for (GLsizei i = 0; i < count; i++) {
        if (isz == 4) { uint32_t x; memcpy(&x, src + 4 * i, 4); tmp[i] = x; }
        else if (isz == 2) { uint16_t x; memcpy(&x, src + 2 * i, 2); tmp[i] = x == 0xffff && C.restart ? 0xffffffffu : x; }
        else tmp[i] = src[i] == 0xff && C.restart ? 0xffffffffu : src[i];
    }
    uint32_t n = (uint32_t)count;
    if (conv) { n = idx_convert(conv, tmp, (uint32_t)count, 0, (uint32_t *)g.cpu); free(tmp); }
    if (n) { vkCmdBindIndexBuffer(cb, g.buf, g.off, VK_INDEX_TYPE_UINT32); vkCmdDrawIndexed(cb, n, (uint32_t)inst, 0, base, 0); }
}
GLB(glDrawArrays, void, GLenum mode, GLint first, GLsizei count) { draw_arrays(mode, first, count, 1); }
GLB(glDrawArraysInstanced, void, GLenum mode, GLint first, GLsizei count, GLsizei inst) { draw_arrays(mode, first, count, inst); }
GLB(glDrawElements, void, GLenum mode, GLsizei count, GLenum type, const void *indices) { draw_elements(mode, count, type, indices, 0, 1); }
GLB(glDrawElementsBaseVertex, void, GLenum mode, GLsizei count, GLenum type, const void *indices, GLint base) { draw_elements(mode, count, type, indices, base, 1); }
GLB(glDrawElementsInstanced, void, GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei inst) { draw_elements(mode, count, type, indices, 0, inst); }
GLB(glDrawElementsInstancedBaseVertex, void, GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei inst, GLint base) { draw_elements(mode, count, type, indices, base, inst); }

// ---------- Clears ----------
// The layer's own shaders for a masked clear: a triangle over the target at depth z, and the colour c (both push constants: vec4 c, float z). glslangValidator -V -g0 of
//   vertex:   layout(push_constant) uniform P { vec4 c; float z; } p;
//             void main() { vec2 t = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2); gl_Position = vec4(t * 2.0 - 1.0, p.z, 1.0); }
//   fragment: layout(push_constant) uniform P { vec4 c; float z; } p; layout(location = 0) out vec4 o; void main() { o = p.c; }
// (#version 450; glslang 15.1.0, the nxnative-build image)
static const uint32_t g_clr_vs[] = {
    0x07230203,0x00010000,0x0008000b,0x00000030,0x00000000,0x00020011,0x00000001,0x0006000b,0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0007000f,0x00000000,0x00000004,0x6e69616d,0x00000000,0x0000000c,0x0000001d,0x00040047,0x0000000c,0x0000000b,0x0000002a,0x00030047,0x0000001b,0x00000002,0x00050048,0x0000001b,
    0x00000000,0x0000000b,0x00000000,0x00050048,0x0000001b,0x00000001,0x0000000b,0x00000001,0x00050048,0x0000001b,0x00000002,0x0000000b,0x00000003,0x00050048,0x0000001b,0x00000003,
    0x0000000b,0x00000004,0x00030047,0x00000025,0x00000002,0x00050048,0x00000025,0x00000000,0x00000023,0x00000000,0x00050048,0x00000025,0x00000001,0x00000023,0x00000010,0x00020013,
    0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,0x00000002,0x00040020,0x00000008,0x00000007,0x00000007,0x00040015,
    0x0000000a,0x00000020,0x00000001,0x00040020,0x0000000b,0x00000001,0x0000000a,0x0004003b,0x0000000b,0x0000000c,0x00000001,0x0004002b,0x0000000a,0x0000000e,0x00000001,0x0004002b,
    0x0000000a,0x00000010,0x00000002,0x00040017,0x00000017,0x00000006,0x00000004,0x00040015,0x00000018,0x00000020,0x00000000,0x0004002b,0x00000018,0x00000019,0x00000001,0x0004001c,
    0x0000001a,0x00000006,0x00000019,0x0006001e,0x0000001b,0x00000017,0x00000006,0x0000001a,0x0000001a,0x00040020,0x0000001c,0x00000003,0x0000001b,0x0004003b,0x0000001c,0x0000001d,
    0x00000003,0x0004002b,0x0000000a,0x0000001e,0x00000000,0x0004002b,0x00000006,0x00000020,0x40000000,0x0004002b,0x00000006,0x00000022,0x3f800000,0x0004001e,0x00000025,0x00000017,
    0x00000006,0x00040020,0x00000026,0x00000009,0x00000025,0x0004003b,0x00000026,0x00000027,0x00000009,0x00040020,0x00000028,0x00000009,0x00000006,0x00040020,0x0000002e,0x00000003,
    0x00000017,0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x0004003b,0x00000008,0x00000009,0x00000007,0x0004003d,0x0000000a,0x0000000d,0x0000000c,
    0x000500c4,0x0000000a,0x0000000f,0x0000000d,0x0000000e,0x000500c7,0x0000000a,0x00000011,0x0000000f,0x00000010,0x0004006f,0x00000006,0x00000012,0x00000011,0x0004003d,0x0000000a,
    0x00000013,0x0000000c,0x000500c7,0x0000000a,0x00000014,0x00000013,0x00000010,0x0004006f,0x00000006,0x00000015,0x00000014,0x00050050,0x00000007,0x00000016,0x00000012,0x00000015,
    0x0003003e,0x00000009,0x00000016,0x0004003d,0x00000007,0x0000001f,0x00000009,0x0005008e,0x00000007,0x00000021,0x0000001f,0x00000020,0x00050050,0x00000007,0x00000023,0x00000022,
    0x00000022,0x00050083,0x00000007,0x00000024,0x00000021,0x00000023,0x00050041,0x00000028,0x00000029,0x00000027,0x0000000e,0x0004003d,0x00000006,0x0000002a,0x00000029,0x00050051,
    0x00000006,0x0000002b,0x00000024,0x00000000,0x00050051,0x00000006,0x0000002c,0x00000024,0x00000001,0x00070050,0x00000017,0x0000002d,0x0000002b,0x0000002c,0x0000002a,0x00000022,
    0x00050041,0x0000002e,0x0000002f,0x0000001d,0x0000001e,0x0003003e,0x0000002f,0x0000002d,0x000100fd,0x00010038};
static const uint32_t g_clr_fs[] = {
    0x07230203,0x00010000,0x0008000b,0x00000012,0x00000000,0x00020011,0x00000001,0x0006000b,0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0006000f,0x00000004,0x00000004,0x6e69616d,0x00000000,0x00000009,0x00030010,0x00000004,0x00000007,0x00040047,0x00000009,0x0000001e,0x00000000,0x00030047,0x0000000a,0x00000002,
    0x00050048,0x0000000a,0x00000000,0x00000023,0x00000000,0x00050048,0x0000000a,0x00000001,0x00000023,0x00000010,0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,
    0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,0x00000004,0x00040020,0x00000008,0x00000003,0x00000007,0x0004003b,0x00000008,0x00000009,0x00000003,0x0004001e,0x0000000a,
    0x00000007,0x00000006,0x00040020,0x0000000b,0x00000009,0x0000000a,0x0004003b,0x0000000b,0x0000000c,0x00000009,0x00040015,0x0000000d,0x00000020,0x00000001,0x0004002b,0x0000000d,
    0x0000000e,0x00000000,0x00040020,0x0000000f,0x00000009,0x00000007,0x00050036,0x00000002,0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x00050041,0x0000000f,0x00000010,
    0x0000000c,0x0000000e,0x0004003d,0x00000007,0x00000011,0x00000010,0x0003003e,0x00000009,0x00000011,0x000100fd,0x00010038};
static VkShaderModule g_clr_mod[2]; static VkPipelineLayout g_clr_pl;
typedef struct { uint32_t cpf, dpf, spf; int cmask, depth, stencil; } ckey;
static struct { ckey k; VkPipeline p; } g_clr[32]; static int g_nclr;
static VkPipeline clr_pipeline(int cmask, int depth, int stencil) {   // for the open pass's formats; cmask 0: no colour written
    ckey k = {P.cpf, P.dpf, P.spf, cmask, depth, stencil};
    for (int i = 0; i < g_nclr; i++) if (!memcmp(&g_clr[i].k, &k, sizeof k)) return g_clr[i].p;
    VkPipelineShaderStageCreateInfo stg[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, g_clr_mod[0], "main", NULL},
                                              {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, g_clr_mod[1], "main", NULL}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE,
                                                 VK_FRONT_FACE_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkStencilOpState so = {VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS, 0xff, 0, 0};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL, 0, (VkBool32)depth, (VkBool32)depth, VK_COMPARE_OP_ALWAYS, VK_FALSE,
                                                (VkBool32)stencil, so, so, 0, 1};
    VkPipelineColorBlendAttachmentState ba = {VK_FALSE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, (VkColorComponentFlags)cmask};
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, P.cpf ? 1u : 0u, &ba, {0, 0, 0, 0}};
    static const VkDynamicState dsv[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 4, dsv};
    VkFormat cf = P.cpf;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, P.cpf ? 1u : 0u, &cf, P.dpf, P.spf};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, cmask ? 2u : 1u, stg, &vi, &ia, NULL, &vp, &rs, &ms, &ds, &cbs, &dy, g_clr_pl,
                                       VK_NULL_HANDLE, 0, VK_NULL_HANDLE, -1};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(hvk.d, VK_NULL_HANDLE, 1, &ci, NULL, &p)) { RED1("the clear pipeline could not be made"); return VK_NULL_HANDLE; }
    if (g_nclr < 32) { g_clr[g_nclr].k = k; g_clr[g_nclr].p = p; g_nclr++; } else { RED1("more than 32 clear pipelines (one made per clear from then on)"); defer(D_PIPE, g_serial, H(p), 0, 0); }
    return p;
}
// A clear the whole framebuffer takes (no scissor, nothing masked) becomes the next pass's load action; a scissored one vkCmdClearAttachments; a masked one (colour channels or
// stencil bits) a triangle of the layer's clear pipeline, which keeps the scissor rectangle and the masks
GLB(glClear, void, GLbitfield mask) {
    uint32_t fw, fh; fb_size(C.fb, &fw, &fh);
    int full = !C.scissor || (C.sc[0] <= 0 && C.sc[1] <= 0 && C.sc[0] + C.sc[2] >= (int)fw && C.sc[1] + C.sc[3] >= (int)fh);
    int bits = (int)(mask & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)), masked = 0;
    if ((bits & GL_COLOR_BUFFER_BIT) && C.cmask != 0xf) masked = 1;
    if ((bits & GL_DEPTH_BUFFER_BIT) && !C.dmask) bits &= ~GL_DEPTH_BUFFER_BIT;
    if ((bits & GL_STENCIL_BUFFER_BIT) && (C.swmask & 0xff) != 0xff) masked = 1;
    if (!bits || !g_on) return;
    if (full && !masked) {
        if (g_pend_clear.bits && g_pend_clear.fb != C.fb) clears_flush();
        if (P.open) pass_end();   // a new pass for the draws after it
        g_pend_clear.fb = C.fb; g_pend_clear.bits |= bits; memcpy(g_pend_clear.c, C.clear_c, sizeof C.clear_c); g_pend_clear.d = C.clear_d; g_pend_clear.s = C.clear_s;
        return;
    }
    if (!pass_ensure()) return;
    VkCommandBuffer cb = cb_main();
    int sc[4] = {0, 0, (int)P.w, (int)P.h};
    if (C.scissor) {
        int x0 = C.sc[0] < 0 ? 0 : C.sc[0], y0 = C.sc[1] < 0 ? 0 : C.sc[1], x1 = C.sc[0] + C.sc[2], y1 = C.sc[1] + C.sc[3];
        if (x1 > (int)P.w) x1 = (int)P.w;
        if (y1 > (int)P.h) y1 = (int)P.h;
        sc[0] = x0 < (int)P.w ? x0 : (int)P.w; sc[1] = y0 < (int)P.h ? y0 : (int)P.h; sc[2] = x1 > sc[0] ? x1 - sc[0] : 0; sc[3] = y1 > sc[1] ? y1 - sc[1] : 0;
    }
    if (!sc[2] || !sc[3]) return;
    VkRect2D rect = {{sc[0], sc[1]}, {(uint32_t)sc[2], (uint32_t)sc[3]}};
    int col = (bits & GL_COLOR_BUFFER_BIT) && P.cpf, dep = (bits & GL_DEPTH_BUFFER_BIT) && P.dpf, stn = (bits & GL_STENCIL_BUFFER_BIT) && P.spf;
    if (!masked) {
        VkClearAttachment ca[2]; uint32_t n = 0;
        if (col) { ca[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ca[n].colorAttachment = 0; memcpy(ca[n].clearValue.color.float32, C.clear_c, 16); n++; }
        if (dep || stn) {
            ca[n].aspectMask = (dep ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stn ? VK_IMAGE_ASPECT_STENCIL_BIT : 0); ca[n].colorAttachment = 0;
            ca[n].clearValue.depthStencil.depth = (float)C.clear_d; ca[n].clearValue.depthStencil.stencil = (uint32_t)C.clear_s & 0xff; n++;
        }
        VkClearRect cr = {rect, 0, 1};
        if (n) { vkCmdClearAttachments(cb, n, ca, 1, &cr); g_st.clears_rect++; }
        return;
    }
    VkPipeline p = clr_pipeline(col ? C.cmask : 0, dep, stn);
    if (!p) return;
    struct { float c[4]; float z; } pc; memcpy(pc.c, C.clear_c, 16); pc.z = (float)C.clear_d;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdPushConstants(cb, g_clr_pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkViewport v = {0, 0, (float)P.w, (float)P.h, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &v); vkCmdSetScissor(cb, 0, 1, &rect);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, stn ? C.swmask & 0xff : 0); vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, (uint32_t)C.clear_s & 0xff);
    vkCmdDraw(cb, 3, 1, 0, 0);
    g_st.clears_draw++;
    enc_reset();   // the next draw sets its own state again
}
GL(glClearColor, void, GLfloat r, GLfloat g, GLfloat b, GLfloat a) { C.clear_c[0] = r; C.clear_c[1] = g; C.clear_c[2] = b; C.clear_c[3] = a; }
GL(glClearDepth, void, GLdouble d) { C.clear_d = d < 0 ? 0 : d > 1 ? 1 : d; }
GL(glClearDepthf, void, GLfloat d) { C.clear_d = d < 0 ? 0 : d > 1 ? 1 : d; }
GL(glClearStencil, void, GLint s) { C.clear_s = s; }
// One buffer cleared to a value of its own, the clear values left as they were (LÖVE clears a canvas so when it is given a colour per canvas); colour attachment 0 only
GLB(glClearBufferfv, void, GLenum buffer, GLint drawbuffer, const GLfloat *v) {
    if (buffer == GL_COLOR && drawbuffer == 0) { float keep[4]; memcpy(keep, C.clear_c, sizeof keep); memcpy(C.clear_c, v, sizeof keep); h_glClear(GL_COLOR_BUFFER_BIT); memcpy(C.clear_c, keep, sizeof keep); }
    else if (buffer == GL_DEPTH) { double keep = C.clear_d; C.clear_d = v[0] < 0 ? 0 : v[0] > 1 ? 1 : v[0]; h_glClear(GL_DEPTH_BUFFER_BIT); C.clear_d = keep; }
    else RED1("glClearBufferfv(%#x, %d) not implemented", buffer, drawbuffer);
}

// ---------- State ----------
static uint8_t *cap_of(GLenum cap) {
    switch (cap) { case GL_DEPTH_TEST: return &C.depth; case GL_BLEND: return &C.blend; case GL_CULL_FACE: return &C.cull; case GL_SCISSOR_TEST: return &C.scissor;
                   case GL_STENCIL_TEST: return &C.stencil; case GL_POLYGON_OFFSET_FILL: return &C.poffset; case GL_PRIMITIVE_RESTART_FIXED_INDEX: return &C.restart; }
    return NULL;
}
static void cap_set(GLenum cap, int on) {
    uint8_t *c = cap_of(cap);
    if (c) { *c = (uint8_t)on; return; }
    if (cap == GL_COLOR_TABLE) { C.color_table = on; return; }
    if (cap == GL_FRAMEBUFFER_SRGB) { C.srgb = (uint8_t)on; return; }   // an sRGB attachment's view: converting with it on, UNORM with it off (pass_begin)
    if (cap == GL_TEXTURE_2D || cap == 0x809d /* MULTISAMPLE */ || cap == 0x8642 /* PROGRAM_POINT_SIZE */ || cap == 0x884f /* TEXTURE_CUBE_MAP_SEAMLESS */
        || cap == 0x92e0 /* DEBUG_OUTPUT */ || cap == 0x8242 /* DEBUG_OUTPUT_SYNCHRONOUS */) return;
    static GLenum seen[32]; static int n;
    for (int i = 0; i < n; i++) if (seen[i] == cap) return;
    if (n < 32) { seen[n++] = cap; GLOG("[RED] gl%s(%#x) not implemented", on ? "Enable" : "Disable", cap); }
}
GL(glEnable, void, GLenum cap) { cap_set(cap, 1); }
GL(glDisable, void, GLenum cap) { cap_set(cap, 0); }
GL(glColorMask, void, uint32_t r, uint32_t g, uint32_t b, uint32_t a) { C.cmask = (uint8_t)((r & 0xff ? 1 : 0) | (g & 0xff ? 2 : 0) | (b & 0xff ? 4 : 0) | (a & 0xff ? 8 : 0)); }
GL(glDepthMask, void, uint32_t m) { C.dmask = (m & 0xff) != 0; }
GL(glDepthFunc, void, GLenum f) { if (f >= GL_NEVER && f <= GL_NEVER + 7) C.dfunc = f; }
GL(glBlendFunc, void, GLenum s, GLenum d) { C.bsrc = C.bsa = s; C.bdst = C.bda = d; }
GL(glBlendFuncSeparate, void, GLenum sc, GLenum dc, GLenum sa, GLenum da) { C.bsrc = sc; C.bdst = dc; C.bsa = sa; C.bda = da; }
GL(glBlendEquation, void, GLenum e) { C.eqc = C.eqa = e; }
GL(glBlendEquationSeparate, void, GLenum c, GLenum a) { C.eqc = c; C.eqa = a; }
GL(glCullFace, void, GLenum m) { C.cull_face = m; }   // GL_FRONT_AND_BACK too: Vulkan culls both
GL(glFrontFace, void, GLenum m) { if (m == GL_CW || m == GL_CCW) C.front = m; }
// Points are as big as the vertex shader's point size output says (glPointSize sets the fixed size, which a core profile's shaders do not read)
GL(glPointSize, void, GLfloat s) { C.psize = s; }
GL(glPolygonMode, void, GLenum face, GLenum mode) { (void)face; if (mode >= GL_POINT && mode <= GL_FILL) C.poly = mode; }   // one mode for both faces, as the core profile has it
GL(glHint, void, GLenum target, GLenum mode) { (void)target; (void)mode; }
GL(glStencilFunc, void, GLenum f, GLint ref, GLuint mask) { if (f >= GL_NEVER && f <= GL_NEVER + 7) C.sfunc = f; C.sref = ref; C.smask = mask; }
GL(glStencilMask, void, GLuint m) { C.swmask = m; }
GL(glStencilOp, void, GLenum sf, GLenum zf, GLenum zp) { C.sfail = sf; C.szfail = zf; C.spass = zp; }
GL(glPolygonOffset, void, GLfloat f, GLfloat u) { C.pfactor = f; C.punits = u; }
GL(glViewport, void, GLint x, GLint y, GLsizei w, GLsizei h) { C.vp[0] = x; C.vp[1] = y; C.vp[2] = w; C.vp[3] = h; }
GL(glScissor, void, GLint x, GLint y, GLsizei w, GLsizei h) { C.sc[0] = x; C.sc[1] = y; C.sc[2] = w; C.sc[3] = h; }
GL(glPixelStorei, void, GLenum p, GLint v) {
    if (p == GL_UNPACK_ALIGNMENT && (v == 1 || v == 2 || v == 4 || v == 8)) C.unpack_align = v;
    else if (p == GL_UNPACK_ROW_LENGTH) C.unpack_row = v;
    else if (p == GL_PACK_ALIGNMENT && (v == 1 || v == 2 || v == 4 || v == 8)) C.pack_align = v;
    else if (p >= 0xcf0 && p <= 0xcf4 && !v) {}   // SWAP_BYTES, LSB_FIRST, SKIP_ROWS, SKIP_PIXELS at their defaults
    else RED1("glPixelStorei(%#x, %d) not implemented", p, v);
}
// Colour-index images through a palette (GL's compatibility pixel transfer: glColorTable, GL_INDEX_SHIFT / GL_INDEX_OFFSET); the game's texture loader sets a palette for its
// paletted images. glTexEnvi is fixed-function texturing, which programs replace
GL(glPixelTransferi, void, GLenum p, GLint v) { if (p == GL_INDEX_SHIFT) C.index_shift = v; else if (p == GL_INDEX_OFFSET) C.index_offset = v; else RED1("glPixelTransferi(%#x) not implemented", p); }
GL(glColorTable, void, GLenum target, GLenum ifmt, GLsizei w, GLenum format, GLenum type, const void *table) {
    if (target != GL_COLOR_TABLE || type != GL_UNSIGNED_BYTE || (format != GL_RGBA && format != GL_RGB) || w <= 0 || !table) { RED1("glColorTable(%#x, %#x, %d, %#x, %#x) not implemented", target, ifmt, w, format, type); return; }
    C.npalette = w > 256 ? 256 : w; const uint8_t *s = table;
    for (int i = 0; i < C.npalette; i++) C.palette[i] = format == GL_RGBA ? (uint32_t)s[4 * i] | (uint32_t)s[4 * i + 1] << 8 | (uint32_t)s[4 * i + 2] << 16 | (uint32_t)s[4 * i + 3] << 24 :
                                                                      (uint32_t)s[3 * i] | (uint32_t)s[3 * i + 1] << 8 | (uint32_t)s[3 * i + 2] << 16 | 0xff000000u;
    RED1("paletted texture (glColorTable, %d entries): palette lookup at upload, the index shift is %d", w, C.index_shift);
}
GL(glTexEnvi, void, GLenum t, GLenum p, GLint v) { (void)t; (void)p; (void)v; }
GL(glGetError, GLenum, void) { return 0; }
// Everything recorded so far runs to its end (LÖVE when the window changes)
GLB(glFinish, void, void) {
    if (!g_on) return;
    if (g_pend_clear.bits) clears_flush();
    dm_gfx_enter(); uint64_t v = cb_commit(); dm_gfx_leave();
    g_st.early_commits++; serial_wait(v, UINT64_MAX);
}
// Debug output and labels (LÖVE's debug groups and its GL debug callback, used when the game asks for debug output): none here
GL(glDebugMessageCallback, void, void *cb, const void *user) { (void)cb; (void)user; }
GL(glDebugMessageControl, void, GLenum src, GLenum type, GLenum sev, GLsizei n, const GLuint *ids, uint32_t on) { (void)src; (void)type; (void)sev; (void)n; (void)ids; (void)on; }
GL(glPushDebugGroup, void, GLenum src, GLuint id, GLsizei len, const GLchar *msg) { (void)src; (void)id; (void)len; (void)msg; }
GL(glPopDebugGroup, void, void) {}
GL(glPushGroupMarkerEXT, void, GLsizei len, const GLchar *msg) { (void)len; (void)msg; }
GL(glPopGroupMarkerEXT, void, void) {}
GL(glObjectLabel, void, GLenum id, GLuint name, GLsizei len, const GLchar *label) { (void)id; (void)name; (void)len; (void)label; }

// ---------- Queries ----------
static const char *const g_exts[] = {"GL_ARB_ES3_2_compatibility", "GL_ARB_ES3_compatibility", "GL_ARB_texture_compression_bptc", "GL_ARB_texture_compression_rgtc",
    "GL_EXT_texture_compression_s3tc", "GL_EXT_texture_filter_anisotropic", "GL_ARB_vertex_array_object", "GL_ARB_direct_state_access", "GL_ARB_half_float_vertex",
    "GL_ARB_framebuffer_object", "GL_ARB_texture_float", "GL_EXT_texture_sRGB", "GL_KHR_texture_compression_astc_ldr", "GL_NV_shadow_samplers_cube"};
#define NEXTS (int)(sizeof g_exts / sizeof *g_exts)
GL(glGetString, const char *, GLenum n) {
    switch (n) {
        case GL_VENDOR: return "NVIDIA Corporation";
        case GL_RENDERER: return "NVIDIA Tegra X1 (nvgpu)/integrated (Vulkan)";
        case GL_VERSION: return "4.5.0 NVIDIA 349.00";
        case GL_SHADING_LANGUAGE_VERSION: return "4.50 NVIDIA";
        case GL_EXTENSIONS: {   // the compatibility profile's one string
            static char all[1024]; if (!all[0]) { size_t o = 0; for (int i = 0; i < NEXTS && o + 2 < sizeof all; i++) o += (size_t)snprintf(all + o, sizeof all - o, "%s ", g_exts[i]); }
            return all;
        }
    }
    RED1("glGetString(%#x) not implemented", n); return "";
}
GL(glGetStringi, const char *, GLenum n, GLuint i) { return n == GL_EXTENSIONS && i < (GLuint)NEXTS ? g_exts[i] : NULL; }
GL(glGetIntegerv, void, GLenum p, GLint *v) {
    switch (p) {
        case GL_NUM_EXTENSIONS: *v = NEXTS; return;
        case GL_MAJOR_VERSION: *v = 4; return;
        case GL_MINOR_VERSION: *v = 5; return;
        case GL_CONTEXT_PROFILE_MASK: *v = 1; return;   // CONTEXT_CORE_PROFILE_BIT: what the games asked for (Digimon a 4.5 core context); LÖVE takes its core paths (persistently mapped streams)
        case GL_CONTEXT_FLAGS: *v = 0; return;
        case GL_MAX_TEXTURE_SIZE: case GL_MAX_RENDERBUFFER_SIZE: case GL_MAX_CUBE_MAP_TEXTURE_SIZE: *v = 16384; return;
        case GL_MAX_3D_TEXTURE_SIZE: case GL_MAX_ARRAY_TEXTURE_LAYERS: *v = 2048; return;
        case GL_MAX_VIEWPORT_DIMS: v[0] = v[1] = 16384; return;
        case GL_MAX_COLOR_ATTACHMENTS: case GL_MAX_DRAW_BUFFERS: *v = 1; return;   // one colour attachment (gfb_t)
        case GL_MAX_VERTEX_ATTRIBS: *v = 16; return;
        case GL_MAX_TEXTURE_IMAGE_UNITS: *v = 32; return;
        case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: *v = NUNITS; return;
        case GL_MAX_VERTEX_UNIFORM_VECTORS: case GL_MAX_FRAGMENT_UNIFORM_VECTORS: *v = 1024; return;
        case GL_MAX_VARYING_VECTORS: *v = 31; return;
        case GL_MAX_SAMPLES: *v = 8; return;
        case GL_SAMPLE_BUFFERS: case GL_SAMPLES: *v = 0; return;   // the window surface is not multisampled
        case GL_VIEWPORT: memcpy(v, C.vp, sizeof C.vp); return;
        case GL_SCISSOR_BOX: memcpy(v, C.sc, sizeof C.sc); return;
        case GL_CULL_FACE_MODE: *v = (GLint)C.cull_face; return;
        case GL_FRONT_FACE: *v = (GLint)C.front; return;
        case GL_FRAMEBUFFER_BINDING: *v = (GLint)C.fb; return;   // and DRAW_FRAMEBUFFER_BINDING
        case GL_READ_FRAMEBUFFER_BINDING: *v = (GLint)C.read_fb; return;
        case GL_RENDERBUFFER_BINDING: *v = (GLint)C.rb; return;
        case GL_CURRENT_PROGRAM: *v = (GLint)C.prog; return;
        case 0x8894: *v = (GLint)C.array_buf; return;   // ARRAY_BUFFER_BINDING
        case 0x8895: *v = (GLint)cur_vao()->ebo; return;   // ELEMENT_ARRAY_BUFFER_BINDING
        case 0x85b5: *v = (GLint)C.vao; return;   // VERTEX_ARRAY_BINDING
        case 0x8069: *v = (GLint)C.units[C.unit].t2d; return;   // TEXTURE_BINDING_2D
        case GL_TEXTURE_BINDING_CUBE_MAP: *v = (GLint)C.units[C.unit].tcube; return;
        case GL_TEXTURE_BINDING_3D: *v = (GLint)C.units[C.unit].t3d; return;
        case GL_TEXTURE_BINDING_2D_ARRAY: *v = (GLint)C.units[C.unit].t2da; return;
        case 0x84e0: *v = GL_TEXTURE0 + C.unit; return;   // ACTIVE_TEXTURE
        case GL_UNPACK_ALIGNMENT: *v = C.unpack_align; return;
        case GL_PACK_ALIGNMENT: *v = C.pack_align; return;
    }
    static GLenum seen[32]; static int n;
    int known = 0; for (int i = 0; i < n; i++) known |= seen[i] == p;
    if (!known && n < 32) { seen[n++] = p; GLOG("[RED] glGetIntegerv(%#x) not implemented (0)", p); }
    *v = 0;
}
GL(glGetFloatv, void, GLenum p, GLfloat *v) {
    switch (p) {
        case GL_MAX_TEXTURE_LOD_BIAS: *v = g_on ? hvk.dev.props.limits.maxSamplerLodBias : 15; return;
        case GL_MAX_TEXTURE_MAX_ANISOTROPY: *v = g_on ? fminf(16, hvk.dev.props.limits.maxSamplerAnisotropy) : 16; return;
        case GL_VIEWPORT: for (int i = 0; i < 4; i++) v[i] = (float)C.vp[i]; return;
        case GL_POINT_SIZE: *v = C.psize; return;
        case GL_POINT_SIZE_RANGE: case GL_ALIASED_POINT_SIZE_RANGE: v[0] = 1; v[1] = g_on ? fminf(511, hvk.dev.props.limits.pointSizeRange[1]) : 511; return;
    }
    RED1("glGetFloatv(%#x) not implemented (0)", p); *v = 0;
}

// ---------- Buffers and vertex arrays ----------
GL(glGenBuffers, void, GLsizei n, GLuint *out) { for (GLsizei i = 0; i < n; i++) out[i] = name_new((void ***)&g_bufs, &g_nbufs, &g_capbufs, sizeof(gbuf_t)); }
GL(glDeleteBuffers, void, GLsizei n, const GLuint *b) {
    for (GLsizei i = 0; i < n; i++) {
        gbuf_t *x = buf_of(b[i]); if (!x) continue;
        bmem_release(&x->m, x->use); free(x); g_bufs[b[i]] = NULL;
        if (C.array_buf == b[i]) C.array_buf = 0;
        if (cur_vao()->ebo == b[i]) cur_vao()->ebo = 0;
    }
}
GL(glBindBuffer, void, GLenum target, GLuint b) {
    if (target == GL_ARRAY_BUFFER) C.array_buf = b;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) cur_vao()->ebo = b;
    else RED1("glBindBuffer target %#x not implemented", target);
}
static gbuf_t *bound_buf(GLenum target) { return buf_of(target == GL_ARRAY_BUFFER ? C.array_buf : target == GL_ELEMENT_ARRAY_BUFFER ? cur_vao()->ebo : 0); }
// Orphaning: data for a buffer a submission may still read goes into new memory, so draws already recorded keep the old contents
GLB(glBufferData, void, GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
    (void)usage;
    gbuf_t *b = bound_buf(target);
    if (!b) { RED1("glBufferData with no buffer bound to %#x", target); return; }
    if (b->immutable) { RED1("glBufferData on a buffer glBufferStorage made"); return; }
    if (size <= 0) { bmem_release(&b->m, b->use); b->size = 0; return; }
    if (!g_on) return;
    int busy = b->m.buf && b->use > gpu_done();
    if (!b->m.buf || b->m.cap < (VkDeviceSize)size || busy || b->m.cap > (VkDeviceSize)size * 2 + 4096) {
        if (busy) g_st.orphans++;
        bmem_release(&b->m, b->use);
        if (!bmem_alloc((VkDeviceSize)size, &b->m)) { b->size = 0; return; }
    }
    b->size = (size_t)size; b->use = 0;
    if (data) memcpy(b->m.map, data, (size_t)size); else memset(b->m.map, 0, (size_t)size);
}
// Immutable storage that stays mapped: the game writes into the buffer's own memory and fences what the GPU still reads (LÖVE's StreamBufferPersistentMapSync)
GLB(glBufferStorage, void, GLenum target, GLsizeiptr size, const void *data, GLbitfield flags) {
    (void)flags; gbuf_t *b = bound_buf(target);
    if (!b || size <= 0) { RED1("glBufferStorage(%#x, %ld) with no buffer bound", target, (long)size); return; }
    if (!g_on) return;
    bmem_release(&b->m, b->use);
    if (!bmem_alloc((VkDeviceSize)size, &b->m)) { b->size = 0; return; }
    b->size = (size_t)size; b->use = 0; b->immutable = 1;
    if (data) memcpy(b->m.map, data, (size_t)size);
}
// In command order: new bytes for a buffer a submission may still read go into a copy of it, which later draws use (the draws already recorded keep the old one)
GLB(glBufferSubData, void, GLenum target, GLintptr off, GLsizeiptr size, const void *data) {
    gbuf_t *b = bound_buf(target);
    if (!b || !b->m.buf || off < 0 || size < 0 || (size_t)(off + size) > b->size) { RED1("glBufferSubData(%#x, %ld, %ld) outside the bound buffer", target, (long)off, (long)size); return; }
    if (b->use > gpu_done()) {
        if (b->immutable) RED1("glBufferSubData into a buffer glBufferStorage made while the GPU may still read it: written in place");
        else {
            bmem_t nm; if (!bmem_alloc(b->m.cap, &nm)) return;
            memcpy(nm.map, b->m.map, b->size); bmem_release(&b->m, b->use); b->m = nm; b->use = 0; g_st.copies_on_write++;
        }
    }
    if (size) memcpy(b->m.map + off, data, (size_t)size);
}
// The mapping is the buffer's own memory: a persistent or unsynchronized mapping is the game's to fence; invalidating a whole buffer in flight orphans it, as glBufferData
// does; any other mapping of a buffer the GPU may still read waits for the GPU, as GL's synchronized map does
GLB(glMapBufferRange, void *, GLenum target, GLintptr off, GLsizeiptr len, GLbitfield access) {
    gbuf_t *b = bound_buf(target);
    if (!b || !b->m.buf || off < 0 || len <= 0 || (size_t)(off + len) > b->size) { RED1("glMapBufferRange(%#x, %ld, %ld) outside the bound buffer", target, (long)off, (long)len); return NULL; }
    if (b->use > gpu_done()) {
        if (!b->immutable && (access & GL_MAP_INVALIDATE_BUFFER_BIT)) {
            bmem_t nm; if (!bmem_alloc(b->m.cap, &nm)) return NULL;
            bmem_release(&b->m, b->use); b->m = nm; b->use = 0; g_st.orphans++;
        } else if (!(access & (GL_MAP_PERSISTENT_BIT | GL_MAP_UNSYNCHRONIZED_BIT))) {
            if (b->use >= g_serial) { if (g_pend_clear.bits) clears_flush(); dm_gfx_enter(); cb_commit(); dm_gfx_leave(); g_st.early_commits++; }
            serial_wait(b->use, UINT64_MAX); g_st.map_waits++;
        }
    }
    return b->m.map + off;
}
GL(glUnmapBuffer, uint32_t, GLenum target) { (void)target; return 1; }
GL(glFlushMappedBufferRange, void, GLenum target, GLintptr off, GLsizeiptr len) { (void)target; (void)off; (void)len; }   // coherent memory: nothing to flush

// ---------- Sync objects ----------
// A fence is the serial of the submission being recorded when it was made, signalled on the layer's timeline when that submission completes. LÖVE fences each third of its
// stream buffers at the end of a frame and waits for the fence three frames later, before it writes that third again
typedef struct { uint64_t serial; } gsync_t;
GL(glFenceSync, void *, GLenum cond, GLbitfield flags) { (void)cond; (void)flags; gsync_t *s = zalloc(sizeof *s); s->serial = g_serial; return s; }
GL(glDeleteSync, void, void *s) { free(s); }
GL(glWaitSync, void, void *s, GLbitfield flags, uint64_t timeout) { (void)s; (void)flags; (void)timeout; }   // a GPU-side wait: the submissions run in order on one queue
GLB(glClientWaitSync, GLenum, void *sy, GLbitfield flags, uint64_t timeout_ns) {
    gsync_t *s = sy;
    if (!s) return GL_WAIT_FAILED;
    if (!g_on || gpu_done() >= s->serial) return GL_ALREADY_SIGNALED;
    if (s->serial >= g_serial) {   // its submission is still being recorded: submit it now (as GL_SYNC_FLUSH_COMMANDS_BIT asks; without it the wait could never end)
        if (!timeout_ns && !(flags & GL_SYNC_FLUSH_COMMANDS_BIT)) return GL_TIMEOUT_EXPIRED;
        if (g_pend_clear.bits) clears_flush();
        dm_gfx_enter(); cb_commit(); dm_gfx_leave(); g_st.early_commits++;
    }
    if (!timeout_ns) return gpu_done() >= s->serial ? GL_ALREADY_SIGNALED : GL_TIMEOUT_EXPIRED;
    return serial_wait(s->serial, timeout_ns) ? GL_CONDITION_SATISFIED : GL_TIMEOUT_EXPIRED;
}
GL(glGenVertexArrays, void, GLsizei n, GLuint *out) { for (GLsizei i = 0; i < n; i++) out[i] = name_new((void ***)&g_vaos, &g_nvaos, &g_capvaos, sizeof(vao_t)); }
GL(glDeleteVertexArrays, void, GLsizei n, const GLuint *v) { for (GLsizei i = 0; i < n; i++) if (v[i] && v[i] < g_nvaos && g_vaos[v[i]]) { free(g_vaos[v[i]]); g_vaos[v[i]] = NULL; if (C.vao == v[i]) C.vao = 0; } }
GL(glBindVertexArray, void, GLuint v) { C.vao = v; }
GL(glEnableVertexAttribArray, void, GLuint i) { if (i < 16) cur_vao()->a[i].on = 1; }
GL(glDisableVertexAttribArray, void, GLuint i) { if (i < 16) cur_vao()->a[i].on = 0; }
GL(glEnableVertexArrayAttrib, void, GLuint vao, GLuint i) { vao_t *v = vao && vao < g_nvaos && g_vaos[vao] ? g_vaos[vao] : &g_vao0; if (i < 16) v->a[i].on = 1; }
GL(glVertexAttribPointer, void, GLuint i, GLint size, GLenum type, uint32_t norm, GLsizei stride, const void *ptr) {
    if (i >= 16) { RED1("vertex attribute %u over 15", i); return; }
    vattr_t *a = &cur_vao()->a[i]; a->bgra = size == GL_BGRA; a->size = (uint8_t)(a->bgra ? 4 : size); a->type = type; a->norm = (norm & 0xff) != 0; a->stride = stride; a->off = (uintptr_t)ptr; a->buf = C.array_buf;
}
GL(glVertexAttribDivisor, void, GLuint i, GLuint d) { if (i < 16) cur_vao()->a[i].div = d > 255 ? 255 : (uint8_t)d; }
GL(glVertexAttrib4f, void, GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { if (i < 16) { C.va[i][0] = x; C.va[i][1] = y; C.va[i][2] = z; C.va[i][3] = w; } }
GL(glVertexAttrib4fv, void, GLuint i, const GLfloat *v) { if (i < 16) memcpy(C.va[i], v, sizeof C.va[i]); }

// ---------- Textures ----------
GL(glGenTextures, void, GLsizei n, GLuint *out) { for (GLsizei i = 0; i < n; i++) out[i] = name_new((void ***)&g_texs, &g_ntexs, &g_captexs, sizeof(gtex_t)); }
GL(glDeleteTextures, void, GLsizei n, const GLuint *t) {
    for (GLsizei i = 0; i < n; i++) {
        gtex_t *x = tex_of(t[i]); if (!x) continue;
        tex_drop(x); free(x); g_texs[t[i]] = NULL;
        for (int u = 0; u < NUNITS; u++) {
            if (C.units[u].t2d == t[i]) C.units[u].t2d = 0;
            if (C.units[u].tcube == t[i]) C.units[u].tcube = 0;
            if (C.units[u].t3d == t[i]) C.units[u].t3d = 0;
            if (C.units[u].t2da == t[i]) C.units[u].t2da = 0;
        }
    }
}
GL(glActiveTexture, void, GLenum u) { if (u >= GL_TEXTURE0 && u < GL_TEXTURE0 + NUNITS) C.unit = (int)(u - GL_TEXTURE0); }
GL(glBindTexture, void, GLenum target, GLuint t) {
    GLuint *slot = target == GL_TEXTURE_2D || target == GL_TEXTURE_CUBE_MAP || target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY ? unit_slot(C.unit, target) : NULL;
    if (!slot) { RED1("texture target %#x not implemented", target); return; }
    gtex_t *x = tex_of(t);
    if (t && !x) {   // a name glGenTextures never gave: GL makes the texture on its first bind
        if (t > 1u << 20) { RED1("glBindTexture of name %u", t); return; }
        x = name_at((void ***)&g_texs, &g_ntexs, &g_captexs, t, sizeof(gtex_t));
    }
    if (x && !x->target) tex_init_params(x, target);
    *slot = t;
}
static void tex_param(GLenum target, GLenum p, float f, GLint i) {
    gtex_t *t = bound_tex(target);
    if (!t) return;
    switch (p) {
        case GL_TEXTURE_MIN_FILTER: t->minf = i; break; case GL_TEXTURE_MAG_FILTER: t->magf = i; break;
        case GL_TEXTURE_WRAP_S: t->ws = i; break; case GL_TEXTURE_WRAP_T: t->wt = i; break; case GL_TEXTURE_WRAP_R: t->wr = i; break;
        case GL_TEXTURE_MAX_LEVEL: t->maxlv = i; break; case GL_TEXTURE_MAX_ANISOTROPY: t->aniso = f; break;
        case GL_TEXTURE_COMPARE_MODE: t->cmp = i ? (t->cmp ? t->cmp : 0x203) : 0; break;   // COMPARE_REF_TO_TEXTURE, the function defaults to LEQUAL
        case GL_TEXTURE_COMPARE_FUNC: if (t->cmp) t->cmp = i; break;
        case GL_TEXTURE_LOD_BIAS: t->bias = f; break;   // LÖVE sets −(mipmap sharpness), 0 by default; Vulkan samplers take it
        case GL_TEXTURE_MIN_LOD: t->minlod = f; break;
        case GL_TEXTURE_MAX_LOD: t->maxlod = f; break;
        case GL_TEXTURE_BASE_LEVEL: if (i) RED1("GL_TEXTURE_BASE_LEVEL %d not implemented (level 0 stays the base)", i); break;
        case GL_TEXTURE_SWIZZLE_R: case GL_TEXTURE_SWIZZLE_R + 1: case GL_TEXTURE_SWIZZLE_R + 2: case GL_TEXTURE_SWIZZLE_A:   // the sampled view is made again
            if (t->swz[p - GL_TEXTURE_SWIZZLE_R] != i) t->view_dirty = 1;
            t->swz[p - GL_TEXTURE_SWIZZLE_R] = i; break;
        default: RED1("glTexParameter(%#x) not implemented", p); break;
    }
    t->smp_dirty = 1;
}
GL(glTexParameteri, void, GLenum target, GLenum p, GLint v) { tex_param(target, p, (float)v, v); }
GL(glTexParameterf, void, GLenum target, GLenum p, GLfloat v) { tex_param(target, p, v, (GLint)v); }
GL(glTexParameterfv, void, GLenum target, GLenum p, const GLfloat *v) {
    if (p == GL_TEXTURE_BORDER_COLOR) { gtex_t *t = bound_tex(target); if (t) { memcpy(t->border, v, sizeof t->border); t->smp_dirty = 1; } return; }
    tex_param(target, p, v[0], (GLint)v[0]);
}
// glTexImage2D, glTexSubImage2D, glCompressedTexImage2D (nine and eight arguments, the last on the stack: the host-call thunk reads it there)
static void teximg(GLenum target, GLint level, GLint ifmt, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *pixels, GLsizei size, int sub, int compressed) {
    if (sub) {
        gtex_t *t = bound_tex(target);
        int face = target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
        if (!t || t->fmt < 0 || w <= 0 || h <= 0) return;
        int f = t->fmt, cv = CV_COPY;   // the data is converted to the texture's format
        if (f == F_RGBA8) cv = type == GL_UNSIGNED_SHORT_5_6_5 ? CV_565 : type == GL_UNSIGNED_SHORT_4_4_4_4 ? CV_4444 : type == GL_UNSIGNED_SHORT_5_5_5_1 ? CV_5551 :
                               format == GL_RGB ? CV_RGB8 : format == GL_COLOR_INDEX ? CV_INDEX : CV_COPY;
        size_t pitch; void *conv = pixels_convert(pixels, (uint32_t)w, (uint32_t)h, format, type, f, cv, &pitch);
        if (!t->made) {   // into a level not made yet: patch the pending copy
            level_t *l = &t->pend[face][level < MAXLV ? level : 0];
            if (l->data && x + w <= (GLint)l->w && y + h <= (GLint)l->h && g_fmt[f].kind != K_BLOCK)
                for (GLsizei r = 0; r < h; r++) memcpy((uint8_t *)l->data + (size_t)(y + r) * l->pitch + (size_t)x * g_fmt[f].bytes, (uint8_t *)conv + (size_t)r * pitch, (size_t)w * g_fmt[f].bytes);
            else tex_make(t);
        }
        if (t->made && t->im.img && level < t->levels) tex_write(t, face, level, (uint32_t)x, (uint32_t)y, 0, (uint32_t)w, (uint32_t)h, conv, pitch);
        free(conv);
        return;
    }
    int cv, f = fmt_pick(ifmt, format, type, &cv);
    if (f < 0) { static GLint seen[16]; static int n; int k = 0; for (int i = 0; i < n; i++) k |= seen[i] == ifmt; if (!k && n < 16) { seen[n++] = ifmt; GLOG("[RED] texture internal format %#x (format %#x, type %#x) not mapped", ifmt, format, type); } return; }
    if (cv == CV_NONE && pixels) RED1("texture data of format %#x type %#x into %s not converted (zeros)", format, type, g_fmt[f].name);
    if (cv == CV_INDEX && !C.color_table) RED1("colour-index texture data with GL_COLOR_TABLE disabled");
    size_t pitch; void *conv;
    if (compressed) {
        size_t need = fmt_pitch(f, (uint32_t)w) * fmt_rows(f, (uint32_t)h);
        if (g_fmt[f].kind != K_BLOCK || (size_t)size < need) { RED1("glCompressedTexImage2D: %d bytes for %dx%d %s (%zu expected)", size, w, h, g_fmt[f].name, need); return; }
        if (is_astc(f) && g_on && !hvk.dev.astc_ldr) {   // a device without ASTC: the blocks decoded on the CPU (dm/gfx/tex/astc_decode.c), the texture RGBA8
            int srgb = f == F_ASTC4_SRGB; f = srgb ? F_SRGBA8 : F_RGBA8; pitch = (size_t)w * 4; conv = zalloc(pitch * (size_t)h + 1);
            astc_decode_image(pixels, fmt_pitch(srgb ? F_ASTC4_SRGB : F_ASTC4, (uint32_t)w), (uint32_t)w, (uint32_t)h, 4, 4, srgb, conv, pitch);
            RED1("ASTC textures decoded on the CPU into RGBA8: the device has no ASTC (textureCompressionASTC_LDR)"); g_st.astc_decoded++;
        } else conv = pixels_convert(pixels, (uint32_t)w, (uint32_t)h, 0, 0, f, CV_COPY, &pitch);
    } else conv = pixels_convert(cv == CV_NONE ? NULL : pixels, (uint32_t)w, (uint32_t)h, format, type, f, cv, &pitch);
    tex_image(target, level, f, (uint32_t)w, (uint32_t)h, pixels || compressed ? conv : (free(conv), (void *)NULL), pitch);
}
GL(glTexImage2D, void, GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void *pixels) {
    (void)border; teximg(target, level, ifmt, 0, 0, w, h, format, type, pixels, 0, 0, 0);
}
GL(glTexSubImage2D, void, GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *pixels) {
    teximg(target, level, 0, x, y, w, h, format, type, pixels, 0, 1, 0);
}
GL(glCompressedTexImage2D, void, GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h, GLint border, GLsizei size, const void *data) {
    (void)border; teximg(target, level, (GLint)ifmt, 0, 0, w, h, 0, 0, data, size, 0, 1);
}
// glTexStorage2D / 3D: the format, the size and the level count, before any data (LÖVE makes every texture so, the mipmapped ones with all their levels, which glGenerateMipmap
// fills). The image is made at its first upload, attachment or sampling
static void tex_storage(GLenum target, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h, GLsizei d) {
    gtex_t *t = bound_tex(target);
    int cv, f = fmt_pick((GLint)ifmt, 0, 0, &cv);
    if (!t || f < 0 || levels <= 0 || w <= 0 || h <= 0 || d <= 0) { RED1("glTexStorage(%#x, %d levels, %#x, %dx%dx%d) not implemented", target, levels, ifmt, w, h, d); return; }
    if (is_astc(f) && g_on && !hvk.dev.astc_ldr) f = f == F_ASTC4_SRGB ? F_SRGBA8 : F_RGBA8;
    if (t->made || t->defined) tex_drop(t);
    t->fmt = f; t->w = (uint32_t)w; t->h = (uint32_t)h; t->d = (uint32_t)d; t->slevels = levels; t->smp_dirty = 1;
}
GL(glTexStorage2D, void, GLenum target, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h) { tex_storage(target, levels, ifmt, w, h, 1); }
GL(glTexStorage3D, void, GLenum target, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h, GLsizei d) { tex_storage(target, levels, ifmt, w, h, d); }
// 3D and array textures (LÖVE makes a 1x1x1 one of each at start for its default textures): data through glTexSubImage3D, a layer (or a 3D slice) at a time; its last three
// arguments are on the stack (the host-call thunk reads them there)
GL(glTexSubImage3D, void, GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum format, GLenum type, const void *pixels) {
    gtex_t *t = bound_tex(target);
    if (!t || t->fmt < 0 || w <= 0 || h <= 0 || d <= 0 || !pixels) return;
    if (!t->made) tex_make(t);
    if (!t->im.img || level >= t->levels) return;
    int f = t->fmt, cv = f == F_RGBA8 && format == GL_RGB ? CV_RGB8 : CV_COPY;
    size_t pitch; void *conv = pixels_convert(pixels, (uint32_t)w, (uint32_t)(h * d), format, type, f, cv, &pitch);   // the layers one after the other
    size_t layer = pitch * fmt_rows(f, (uint32_t)h);
    for (GLsizei k = 0; k < d; k++) {
        if (t->target == GL_TEXTURE_3D) tex_write(t, 0, level, (uint32_t)x, (uint32_t)y, (uint32_t)(z + k), (uint32_t)w, (uint32_t)h, (uint8_t *)conv + (size_t)k * layer, pitch);
        else tex_write(t, z + k, level, (uint32_t)x, (uint32_t)y, 0, (uint32_t)w, (uint32_t)h, (uint8_t *)conv + (size_t)k * layer, pitch);
    }
    free(conv);
}
GL(glTexImage3D, void, GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLsizei d, GLint border, GLenum format, GLenum type, const void *pixels) {
    (void)border;
    if (level) { RED1("glTexImage3D of level %d not implemented", level); return; }
    tex_storage(target, 1, (GLenum)ifmt, w, h, d);
    if (pixels) h_glTexSubImage3D(target, 0, 0, 0, 0, w, h, d, format, type, pixels);
}
// Mipmaps from level 0, in command order (after the draws that rendered level 0 this frame), each level blitted from the one above with a linear filter: a texture of one level
// gets its full chain first, level 0 copied over
GL(glGenerateMipmap, void, GLenum target) {
    gtex_t *t = bound_tex(target);
    if (!t || (!t->made && !tex_make(t)) || !t->im.img) return;
    if (g_fmt[t->fmt].kind != K_COL) { RED1("glGenerateMipmap of a %s texture not implemented", g_fmt[t->fmt].name); return; }
    VkFormatFeatureFlags ff = fmt_feats(t->im.vf);
    if (!(ff & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(ff & VK_FORMAT_FEATURE_BLIT_DST_BIT)) { RED1("glGenerateMipmap: the device cannot blit format %d", t->im.vf); return; }
    VkCommandBuffer cb;
    if (t->use >= g_serial) { pass_end(); cb = cb_main(); } else cb = cb_pre();
    hvk_full_barrier(cb);
    uint32_t faces = tex_layers(t), h0 = t->target == GL_TEXTURE_CUBE_MAP ? t->w : t->h;
    if (t->levels == 1 && umax(t->w, h0) > 1) {
        gimg_t old = t->im; VkImageView oldv = t->view; uint64_t use = t->use;
        t->made = 0; t->im.img = VK_NULL_HANDLE; t->im.natt = 0; t->view = VK_NULL_HANDLE; t->defined = 0; t->slevels = MAXLV;
        if (!tex_make(t)) { t->im = old; t->view = oldv; t->made = 1; return; }
        if (cb == g_slot[g_cur].pre) hvk_full_barrier(cb);   // after the new image's first layout change, in the same command buffer
        VkImageCopy r = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, faces}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, faces}, {0, 0, 0}, {t->w, h0, 1}};
        vkCmdCopyImage(cb, old.img, VK_IMAGE_LAYOUT_GENERAL, t->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        if (oldv) defer(D_VIEW, g_serial, H(oldv), 0, 0);
        gimg_drop(&old, g_serial > use ? g_serial : use);
        t->gen++;
    }
    int lin = (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    for (int l = 1; l < t->levels; l++) {
        hvk_full_barrier(cb);
        int32_t sw = (int32_t)umax(1, t->w >> (l - 1)), sh = (int32_t)umax(1, h0 >> (l - 1)), dw = (int32_t)umax(1, t->w >> l), dh = (int32_t)umax(1, h0 >> l);
        int32_t sd = t->target == GL_TEXTURE_3D ? (int32_t)umax(1, t->d >> (l - 1)) : 1, dd = t->target == GL_TEXTURE_3D ? (int32_t)umax(1, t->d >> l) : 1;
        VkImageBlit b = {{VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l - 1, 0, faces}, {{0, 0, 0}, {sw, sh, sd}}, {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l, 0, faces}, {{0, 0, 0}, {dw, dh, dd}}};
        vkCmdBlitImage(cb, t->im.img, VK_IMAGE_LAYOUT_GENERAL, t->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    t->use = g_serial; g_st.mipmaps++;
}

// ---------- Framebuffers and renderbuffers ----------
GL(glGenFramebuffers, void, GLsizei n, GLuint *out) { for (GLsizei i = 0; i < n; i++) out[i] = name_new((void ***)&g_fbs, &g_nfbs, &g_capfbs, sizeof(gfb_t)); }
GLB(glDeleteFramebuffers, void, GLsizei n, const GLuint *f) {
    for (GLsizei i = 0; i < n; i++) {
        gfb_t *x = fb_of(f[i]); if (!x) continue;
        if (P.open && P.fb == f[i]) pass_end();
        if (g_pend_clear.fb == f[i]) g_pend_clear.bits = 0;
        free(x); g_fbs[f[i]] = NULL; if (C.fb == f[i]) C.fb = 0;
    }
}
GLB(glBindFramebuffer, void, GLenum target, GLuint f) {
    if (target != GL_DRAW_FRAMEBUFFER) C.read_fb = f;   // GL_FRAMEBUFFER binds both
    if (target == GL_READ_FRAMEBUFFER || f == C.fb) return;
    if (g_pend_clear.bits && g_pend_clear.fb == C.fb) clears_flush();
    C.fb = f;   // the pass ends at the next draw or clear into another framebuffer (pass_ensure)
}
GL(glCheckFramebufferStatus, GLenum, GLenum target) { (void)target; return GL_FRAMEBUFFER_COMPLETE; }
// One colour attachment: GL_NONE (LÖVE's depth-only canvases) and attachment 0 are the same draw buffer here
GL(glDrawBuffers, void, GLsizei n, const GLenum *b) { if (n > 1) RED1("glDrawBuffers of %d buffers (one colour attachment is implemented)", n); (void)b; }
GL(glDrawBuffer, void, GLenum b) { (void)b; }
GL(glReadBuffer, void, GLenum b) { (void)b; }
GL(glInvalidateFramebuffer, void, GLenum target, GLsizei n, const GLenum *att) { (void)target; (void)n; (void)att; }   // the contents are kept (stored)
GL(glDiscardFramebufferEXT, void, GLenum target, GLsizei n, const GLenum *att) { (void)target; (void)n; (void)att; }
GL(glGenRenderbuffers, void, GLsizei n, GLuint *out) { for (GLsizei i = 0; i < n; i++) out[i] = name_new((void ***)&g_rbs, &g_nrbs, &g_caprbs, sizeof(grb_t)); }
GL(glDeleteRenderbuffers, void, GLsizei n, const GLuint *r) { for (GLsizei i = 0; i < n; i++) { grb_t *x = rb_of(r[i]); if (!x) continue; gimg_drop(&x->im, x->use); free(x); g_rbs[r[i]] = NULL; } }
GL(glBindRenderbuffer, void, GLenum target, GLuint r) { (void)target; C.rb = r; }
GL(glGetRenderbufferParameteriv, void, GLenum target, GLenum p, GLint *v) {
    (void)target; grb_t *r = rb_of(C.rb);
    switch (p) {
        case GL_RENDERBUFFER_SAMPLES: *v = 0; return;
        case GL_RENDERBUFFER_WIDTH: *v = r ? (GLint)r->w : 0; return;
        case GL_RENDERBUFFER_HEIGHT: *v = r ? (GLint)r->h : 0; return;
    }
    RED1("glGetRenderbufferParameteriv(%#x) not implemented", p); *v = 0;
}
GLB(glRenderbufferStorage, void, GLenum target, GLenum ifmt, GLsizei w, GLsizei h) {
    (void)target; grb_t *r = rb_of(C.rb);
    int cv, f = fmt_pick((GLint)ifmt, GL_RGBA, GL_UNSIGNED_BYTE, &cv);
    if (!r || f < 0 || w <= 0 || h <= 0 || g_fmt[f].kind == K_BLOCK) { RED1("glRenderbufferStorage(%#x, %dx%d) not implemented", ifmt, w, h); return; }
    if (!g_on) return;
    gimg_drop(&r->im, r->use);
    VkFormat vf = fmt_vk(f); int ds = g_fmt[f].kind != K_COL;
    VkImageUsageFlags us = (ds ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (gimg_make(&r->im, cb_pre(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_TYPE_2D, vf, (uint32_t)w, (uint32_t)h, 1, 1, 1, us, unorm_of(vf) != vf ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0)) {
        r->fmt = f; r->w = (uint32_t)w; r->h = (uint32_t)h; r->gen++;
    }
    OLOG("renderbuffer %u: %dx%d %s", C.rb, w, h, g_fmt[f].name);
}
GLB(glRenderbufferStorageMultisample, void, GLenum target, GLsizei samples, GLenum ifmt, GLsizei w, GLsizei h) {
    if (samples > 1) RED1("multisampled renderbuffer (%d samples): made with one sample", samples);
    h_glRenderbufferStorage(target, ifmt, w, h);
}
static void fb_attach(GLenum target, GLenum attachment, int kind, GLuint name, int level, int face) {
    gfb_t *f = fb_of(target == GL_READ_FRAMEBUFFER ? C.read_fb : C.fb);
    if (!f) { RED1("attachment to framebuffer 0 or to one never generated"); return; }
    att_t a = {kind, name, level, face};
    if (!name) a.kind = 0;
    if (attachment == GL_COLOR_ATTACHMENT0) f->col = a;
    else if (attachment == GL_DEPTH_ATTACHMENT) f->depth = a;
    else if (attachment == GL_STENCIL_ATTACHMENT) f->stencil = a;
    else if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) f->depth = f->stencil = a;
    else { RED1("framebuffer attachment %#x not implemented", attachment); return; }
    f->gen++;
}
GL(glFramebufferTexture2D, void, GLenum target, GLenum att, GLenum textarget, GLuint t, GLint level) {
    int face = textarget >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && textarget <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? (int)(textarget - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
    gtex_t *x = tex_of(t); if (x && !x->target) tex_init_params(x, textarget == GL_TEXTURE_2D ? GL_TEXTURE_2D : GL_TEXTURE_CUBE_MAP);
    fb_attach(target, att, 1, t, level, face);
}
// A layer of an array texture or a slice of a 3D one
GL(glFramebufferTextureLayer, void, GLenum target, GLenum att, GLuint t, GLint level, GLint layer) { fb_attach(target, att, 1, t, level, layer); }
GL(glFramebufferTexture3D, void, GLenum target, GLenum att, GLenum textarget, GLuint t, GLint level, GLint z) { (void)textarget; fb_attach(target, att, 1, t, level, z); }
GL(glFramebufferRenderbuffer, void, GLenum target, GLenum att, GLenum rbt, GLuint r) { (void)rbt; fb_attach(target, att, 2, r, 0, 0); }
// A framebuffer's image for a transfer: its colour image (or depth / stencil), level and layer, made GENERAL for the frame when it is the window's
typedef struct { gimg_t *im; int level, layer; } fbimg_t;
static int fb_image(GLuint fb, int which, fbimg_t *o, VkCommandBuffer cb) {   // which: 0 colour, 1 depth / stencil
    memset(o, 0, sizeof *o);
    if (!fb) {
        win_ensure(); if (!g_win.made) return 0;
        if (which) { o->im = &g_win.ds; return 1; }
        win_general(cb); o->im = &g_win.col[g_win.cur]; return 1;
    }
    gfb_t *f = fb_of(fb); if (!f) return 0;
    const att_t *a = which ? (f->depth.kind ? &f->depth : &f->stencil) : &f->col;
    if (a->kind == 1) { gtex_t *t = tex_of(a->name); if (!t || (!t->made && !tex_make(t)) || !t->im.img) return 0; t->use = g_serial; o->im = &t->im; o->level = a->level; o->layer = a->face; }
    else if (a->kind == 2) { grb_t *r = rb_of(a->name); if (!r || !r->im.img) return 0; r->use = g_serial; o->im = &r->im; }
    return o->im != NULL;
}
// glBlitFramebuffer: a copy where nothing scales, flips or converts, else a blit (scaling and flipping by the rectangles, which are GL's rows; linear where GL_LINEAR and the
// format filters); depth and stencil as blits of one format, nearest. Its last two arguments are on the stack (the host-call thunk reads them there)
GL(glBlitFramebuffer, void, GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter) {
    if (!g_on || !(mask & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT))) return;
    if (g_pend_clear.bits) clears_flush();
    pass_end(); VkCommandBuffer cb = cb_main(); hvk_full_barrier(cb);
    for (int which = 0; which < 2; which++) {
        GLbitfield m = which ? mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT) : mask & GL_COLOR_BUFFER_BIT;
        if (!m) continue;
        fbimg_t s, d;
        if (!fb_image(C.read_fb, which, &s, cb) || !fb_image(C.fb, which, &d, cb)) { RED1("glBlitFramebuffer of a framebuffer without that image"); continue; }
        VkImageAspectFlags a = which ? ((m & GL_DEPTH_BUFFER_BIT) && has_depth(s.im->vf) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((m & GL_STENCIL_BUFFER_BIT) && has_stencil(s.im->vf) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0) : VK_IMAGE_ASPECT_COLOR_BIT;
        if (!a) continue;
        int32_t w = sx1 - sx0, h = sy1 - sy0;
        if (s.im->vf == d.im->vf && w > 0 && h > 0 && w == dx1 - dx0 && h == dy1 - dy0) {
            VkImageCopy r = {{a, (uint32_t)s.level, (uint32_t)s.layer, 1}, {sx0, sy0, 0}, {a, (uint32_t)d.level, (uint32_t)d.layer, 1}, {dx0, dy0, 0}, {(uint32_t)w, (uint32_t)h, 1}};
            vkCmdCopyImage(cb, s.im->img, VK_IMAGE_LAYOUT_GENERAL, d.im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        } else {
            if (which && s.im->vf != d.im->vf) { RED1("glBlitFramebuffer of depth / stencil between two formats not implemented"); continue; }
            if (!(fmt_feats(s.im->vf) & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(fmt_feats(d.im->vf) & VK_FORMAT_FEATURE_BLIT_DST_BIT)) { RED1("glBlitFramebuffer: the device cannot blit format %d to %d", s.im->vf, d.im->vf); continue; }
            int lin = !which && filter == GL_LINEAR && (fmt_feats(s.im->vf) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
            VkImageBlit b = {{a, (uint32_t)s.level, (uint32_t)s.layer, 1}, {{sx0, sy0, 0}, {sx1, sy1, 1}}, {a, (uint32_t)d.level, (uint32_t)d.layer, 1}, {{dx0, dy0, 0}, {dx1, dy1, 1}}};
            vkCmdBlitImage(cb, s.im->img, VK_IMAGE_LAYOUT_GENERAL, d.im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        }
        g_st.blits++;
    }
}
GL(glResolveMultisampleFramebufferAPPLE, void, void) { RED1("glResolveMultisampleFramebufferAPPLE not implemented"); }
// glReadPixels: the region copied out after everything recorded so far (that submission is made and waited for), rows as GL lays them, bottom row first, which is how this
// layer stores them. RGBA / unsigned byte from an RGBA8, sRGB RGBA8 or BGRA8 image (LÖVE's Canvas:newImageData and screenshots)
GL(glReadPixels, void, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *data) {
    if (w <= 0 || h <= 0 || !data || !g_on) return;
    if (g_pend_clear.bits) clears_flush();
    pass_end();
    VkCommandBuffer cb = cb_main(); fbimg_t s;
    if (!fb_image(C.read_fb, 0, &s, cb)) { RED1("glReadPixels of a framebuffer without a colour image"); return; }
    VkFormat f = s.im->vf; int bgra = f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
    if ((f != VK_FORMAT_R8G8B8A8_UNORM && f != VK_FORMAT_R8G8B8A8_SRGB && !bgra) || format != GL_RGBA || type != GL_UNSIGNED_BYTE) {
        RED1("glReadPixels(%#x, %#x) of a format-%d image not implemented", format, type, f); return;
    }
    size_t pitch = (size_t)w * 4; VkBuffer ob; VkDeviceMemory om; void *map = NULL;
    if (hvk_buffer_make(pitch * (size_t)h, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1, &ob, &om, &map)) { RED1("glReadPixels: no buffer for the copy"); return; }
    hvk_full_barrier(cb);
    VkBufferImageCopy r = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)s.level, (uint32_t)s.layer, 1}, {x, y, 0}, {(uint32_t)w, (uint32_t)h, 1}};
    vkCmdCopyImageToBuffer(cb, s.im->img, VK_IMAGE_LAYOUT_GENERAL, ob, 1, &r);
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb, 0, NULL, 0, NULL};
    vkCmdPipelineBarrier2(cb, &di);
    dm_gfx_enter(); uint64_t v = cb_commit(); dm_gfx_leave();
    g_st.early_commits++; serial_wait(v, UINT64_MAX);
    size_t dp = (pitch + (size_t)C.pack_align - 1) / (size_t)C.pack_align * (size_t)C.pack_align;
    for (GLsizei row = 0; row < h; row++) {
        uint8_t *d = (uint8_t *)data + (size_t)row * dp; const uint8_t *sp = (const uint8_t *)map + (size_t)row * pitch;
        if (!bgra) memcpy(d, sp, pitch);
        else for (GLsizei i = 0; i < w; i++) { d[4 * i] = sp[4 * i + 2]; d[4 * i + 1] = sp[4 * i + 1]; d[4 * i + 2] = sp[4 * i]; d[4 * i + 3] = sp[4 * i + 3]; }
    }
    hvk_buffer_free(ob, om);
    g_st.readpixels++;
}

// ---------- Shaders and programs ----------
static void sh_capture(const gsh_t *s) {   // DM_GL_CAPTURE=<dir>: whole files only, a capture run may be killed at any time
    static const char *dir; static int init;
    if (!init) { init = 1; dir = getenv("DM_GL_CAPTURE"); if (dir && !*dir) dir = NULL; if (dir) GLOG("shader capture → %s", dir); }
    if (!dir) return;
    char p[1024]; snprintf(p, sizeof p, "%s/%s.%s", dir, s->key, s->type == GL_VERTEX_SHADER ? "vert" : "frag");
    FILE *e = fopen(p, "rb"); if (e) { fclose(e); return; }
    char t[1100]; snprintf(t, sizeof t, "%s.%d.tmp", p, (int)getpid());
    FILE *f = fopen(t, "wb"); if (!f) { RED1("shader capture cannot write to %s", dir); return; }
    int ok = fwrite(s->src, 1, s->len, f) == s->len; ok &= fclose(f) == 0;
    if (ok && rename(t, p)) remove(t);   // another thread wrote it first (Windows refuses to rename over a file)
    else if (!ok) remove(t);
}
GL(glCreateShader, GLuint, GLenum type) {
    GLuint n = name_new((void ***)&g_sps, &g_nsps, &g_capsps, sizeof(sp_t)); g_sps[n]->s.type = type; return n;
}
GL(glCreateProgram, GLuint, void) {
    GLuint n = name_new((void ***)&g_sps, &g_nsps, &g_capsps, sizeof(sp_t)); g_sps[n]->is_prog = 1; return n;
}
GL(glShaderSource, void, GLuint sh, GLsizei count, const GLchar *const *str, const GLint *len) {
    gsh_t *s = shader_of(sh); if (!s) return;
    size_t n = 0;
    for (GLsizei i = 0; i < count; i++) n += len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
    free(s->src); s->src = zalloc(n + 1); s->len = 0;
    for (GLsizei i = 0; i < count; i++) { size_t k = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]); memcpy(s->src + s->len, str[i], k); s->len += k; }
    s->len = strnlen(s->src, s->len);   // the source ends at its first NUL (gl_shaders.py hashes the same text)
    s->compiled = 0;
}
GLB(glCompileShader, void, GLuint sh) {
    gsh_t *s = shader_of(sh); if (!s || !s->src) return;
    uint8_t h[32]; plat_sha256(s->src, s->len, h);
    for (int i = 0; i < 8; i++) snprintf(s->key + 2 * i, 3, "%02x", h[i]);
    sh_capture(s);
    int frag = s->type == GL_FRAGMENT_SHADER; size_t n = 0;
    s->si = (shinfo *)sh_find(s->key, frag); s->compiled = 1;
    if (!s->si || !sh_words(s->si, &n)) { static int k; if (k++ < 50) GLOG("[RED] %s shader %s (%zu bytes of GLSL) is not in the library %s (DM_GL_CAPTURE collects it)", frag ? "fragment" : "vertex", s->key, s->len, g_libdir); s->si = NULL; }
    else OLOG("CompileShader %s", s->key);
}
GL(glGetShaderiv, void, GLuint sh, GLenum p, GLint *v) {
    gsh_t *s = shader_of(sh); if (!s) { *v = 0; return; }
    switch (p) {
        case GL_COMPILE_STATUS: *v = 1; return;   // a shader the library lacks still compiles: its program's draws are skipped (and reported)
        case GL_INFO_LOG_LENGTH: *v = 0; return;
        case GL_SHADER_TYPE: *v = (GLint)s->type; return;
        case GL_SHADER_SOURCE_LENGTH: *v = (GLint)s->len + 1; return;
        case GL_DELETE_STATUS: *v = 0; return;
    }
    RED1("glGetShaderiv(%#x) not implemented", p); *v = 0;
}
GL(glGetShaderInfoLog, void, GLuint sh, GLsizei size, GLsizei *len, GLchar *log) { (void)sh; if (len) *len = 0; if (log && size > 0) log[0] = 0; }
GL(glDeleteShader, void, GLuint sh) { gsh_t *s = shader_of(sh); if (!s) return; free(s->src); free(g_sps[sh]); g_sps[sh] = NULL; }
GL(glAttachShader, void, GLuint pr, GLuint sh) {
    gprog_t *p = prog_of(pr); gsh_t *s = shader_of(sh); if (!p || !s) return;
    if (s->type == GL_VERTEX_SHADER) p->vs = sh; else p->fs = sh;
}
static void prog_clear(gprog_t *p) {   // its Vulkan objects released once the GPU is done with its draws
    for (int i = 0; i < p->npc; i++) defer(D_PIPE, p->use, H(p->pc[i].p), 0, 0);
    free(p->pc); p->pc = NULL; p->npc = p->cappc = 0;
    for (int st = 0; st < 2; st++) {
        defer(D_MOD, p->use, H(p->mod[st]), 0, 0); defer(D_DSL, p->use, H(p->dsl[st]), 0, 0);
        p->mod[st] = VK_NULL_HANDLE; p->dsl[st] = VK_NULL_HANDLE; free(p->ub[st]); p->ub[st] = NULL; free(p->sig[st]); p->sig[st] = NULL;
        p->set_serial[st] = p->ubm_serial[st] = 0; p->nres[st] = 0;
    }
    defer(D_PLAYOUT, p->use, H(p->pl), 0, 0); p->pl = VK_NULL_HANDLE;
    free(p->u); p->u = NULL; p->nu = 0; p->linked = 0;
}
static VkShaderModule spv_module(const uint32_t *w, size_t n) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, n * 4, w};
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(hvk.d, &ci, NULL, &m) ? VK_NULL_HANDLE : m;
}
// Uniforms are merged by name over both stages, as GL links them; a location is uniform index × 4096 + array element. The Vulkan side: a module per stage (the fragment one
// with its inputs at the vertex outputs' locations), a descriptor set layout per stage (set 0 vertex, set 1 fragment: the default uniform block as a dynamic uniform buffer, the
// combined samplers), the pipeline layout over both
GLB(glLinkProgram, void, GLuint pr) {
    static uint32_t serials;
    gprog_t *p = prog_of(pr); if (!p) return;
    prog_clear(p);
    gsh_t *vs = shader_of(p->vs), *fs = shader_of(p->fs);
    if (!vs || !fs || !vs->si || !fs->si) { RED1("program %u: a shader is missing from the library, its draws are skipped", pr); return; }
    if (!g_on) return;
    p->si[0] = vs->si; p->si[1] = fs->si;
    size_t nv = 0, nf = 0; const uint32_t *wv = sh_words(vs->si, &nv), *wf = sh_words(fs->si, &nf);
    int moved = 0; uint32_t *lf = spv_link(wv, nv, wf, nf, pr, &moved);
    p->mod[0] = spv_module(wv, nv); p->mod[1] = lf ? spv_module(lf, nf) : VK_NULL_HANDLE; free(lf);
    if (!p->mod[0] || !p->mod[1]) { LOG("[gl] [RED] program %u: the driver refuses its shaders (vs %s, fs %s): its draws are skipped", pr, vs->key, fs->key); prog_clear(p); return; }
    g_st.varyings_relocated += (uint64_t)moved;
    int cap = vs->si->nu + vs->si->nt + fs->si->nu + fs->si->nt; p->u = zalloc((size_t)(cap + 1) * sizeof *p->u);
    for (int st = 0; st < 2; st++) {
        const shinfo *si = p->si[st];
        p->ub[st] = zalloc((size_t)si->ub_size + 16);
        for (int i = 0; i < si->nu + si->nt; i++) {
            const char *nm = i < si->nu ? si->u[i].name : si->t[i - si->nu].name;
            puni_t *u = NULL;
            for (int j = 0; j < p->nu; j++) if (!strcmp(p->u[j].name, nm)) u = &p->u[j];
            if (!u) { u = &p->u[p->nu++]; snprintf(u->name, sizeof u->name, "%s", nm); u->off[0] = u->off[1] = -1; u->slot[0] = u->slot[1] = -1; }
            if (i < si->nu) { const shuni *m = &si->u[i]; u->type = m->type; u->count = m->count; u->off[st] = m->off; u->stride[st] = m->stride; }
            else { const shsamp *m = &si->t[i - si->nu]; u->type = m->type; u->slot[st] = m->slot; }
        }
        VkDescriptorSetLayoutBinding b[65]; uint32_t nb = 0; VkShaderStageFlags sf = st ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        if (si->ub_slot >= 0 && si->ub_size > 0) b[nb++] = (VkDescriptorSetLayoutBinding){(uint32_t)si->ub_slot, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, sf, NULL};
        for (int i = 0; i < si->nt && nb < 65; i++) b[nb++] = (VkDescriptorSetLayoutBinding){si->t[i].slot, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, sf, NULL};
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, nb, b};
        if (vkCreateDescriptorSetLayout(hvk.d, &li, NULL, &p->dsl[st])) { RED1("a descriptor set layout could not be made"); prog_clear(p); return; }
        p->nres[st] = (int)nb; p->sig[st] = zalloc((size_t)(1 + 2 * 64) * 8);
    }
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 2, p->dsl, 0, NULL};
    if (vkCreatePipelineLayout(hvk.d, &pli, NULL, &p->pl)) { RED1("a pipeline layout could not be made"); prog_clear(p); return; }
    // the game's attribute indices: glBindAttribLocation's for the inputs it names, the input's own location for the others (or the lowest index no binding took)
    const shinfo *vi = p->si[0]; uint32_t taken = 0;
    for (int s = 0; s < 16; s++) p->amap[s] = (int8_t)s;
    for (int i = 0; i < vi->na; i++) for (int j = 0; j < p->nbind; j++)
        if (!strcmp(p->bind[j].name, vi->a[i].name)) { p->amap[vi->a[i].slot] = (int8_t)p->bind[j].index; taken |= 1u << p->bind[j].index; }
    for (int i = 0; i < vi->na; i++) {
        int bound = 0; for (int j = 0; j < p->nbind; j++) bound |= !strcmp(p->bind[j].name, vi->a[i].name);
        if (bound) continue;
        int g = vi->a[i].slot; for (int n = 0; n < 16 && (taken >> g & 1); n++) g = (g + 1) % 16;
        p->amap[vi->a[i].slot] = (int8_t)g; taken |= 1u << g;
    }
    p->serial = ++serials; p->ub_gen[0]++; p->ub_gen[1]++; p->linked = 1;
    OLOG("LinkProgram %u: vs %s fs %s, %d uniforms, %d varyings moved", pr, vs->key, fs->key, p->nu, moved);
}
GL(glBindAttribLocation, void, GLuint pr, GLuint index, const GLchar *name) {   // takes effect at the next link
    gprog_t *p = prog_of(pr); if (!p || index >= 16 || !name) return;
    int j = 0; while (j < p->nbind && strcmp(p->bind[j].name, name)) j++;
    if (j == 16) { RED1("more than 16 attribute bindings in a program"); return; }
    if (j == p->nbind) { snprintf(p->bind[j].name, sizeof p->bind[j].name, "%s", name); p->nbind++; }
    p->bind[j].index = (int)index;
}
GL(glGetProgramiv, void, GLuint pr, GLenum p, GLint *v) {
    gprog_t *x = prog_of(pr); if (!x) { *v = 0; return; }
    switch (p) {
        case GL_LINK_STATUS: case GL_VALIDATE_STATUS: *v = 1; return;   // as glGetShaderiv's compile status
        case GL_INFO_LOG_LENGTH: *v = 0; return;
        case GL_ACTIVE_UNIFORMS: *v = x->nu; return;
        case GL_ACTIVE_UNIFORM_MAX_LENGTH: { int m = 0; for (int i = 0; i < x->nu; i++) m = (int)umax((uint32_t)m, (uint32_t)strlen(x->u[i].name) + 4); *v = m; return; }
        case GL_ATTACHED_SHADERS: *v = (x->vs != 0) + (x->fs != 0); return;
        case GL_ACTIVE_ATTRIBUTES: *v = x->si[0] ? x->si[0]->na : 0; return;
        case GL_DELETE_STATUS: *v = 0; return;
    }
    RED1("glGetProgramiv(%#x) not implemented", p); *v = 0;
}
GL(glDeleteProgram, void, GLuint pr) { gprog_t *p = prog_of(pr); if (!p) return; prog_clear(p); free(g_sps[pr]); g_sps[pr] = NULL; if (C.prog == pr) C.prog = 0; }
GL(glUseProgram, void, GLuint pr) { C.prog = pr; }
GL(glGetAttribLocation, GLint, GLuint pr, const GLchar *name) {
    gprog_t *p = prog_of(pr); if (!p || !p->linked) return -1;
    for (int i = 0; i < p->si[0]->na; i++) if (!strcmp(p->si[0]->a[i].name, name)) return p->amap[p->si[0]->a[i].slot];
    return -1;
}
GL(glGetProgramInfoLog, void, GLuint pr, GLsizei size, GLsizei *len, GLchar *log) { (void)pr; if (len) *len = 0; if (log && size > 0) log[0] = 0; }
// The uniforms in the order of the program's table (both stages merged, samplers among them): an array's name ends in [0], as GL reports it (LÖVE's mapActiveUniforms)
GL(glGetActiveUniform, void, GLuint pr, GLuint index, GLsizei size, GLsizei *len, GLint *count, GLenum *type, GLchar *name) {
    gprog_t *p = prog_of(pr);
    if (!p || !p->linked || index >= (GLuint)p->nu) { if (len) *len = 0; if (name && size > 0) name[0] = 0; return; }
    const puni_t *u = &p->u[index]; char nm[64];
    snprintf(nm, sizeof nm, "%s%s", u->name, u->count ? "[0]" : "");
    if (name && size > 0) snprintf(name, (size_t)size, "%s", nm);
    if (len) *len = name && size > 0 ? (GLsizei)umin((uint32_t)strlen(nm), (uint32_t)size - 1) : 0;
    if (count) *count = u->count ? u->count : 1;
    if (type) *type = g_tenums[u->type];
}
GL(glGetShaderPrecisionFormat, void, GLenum stage, GLenum prec, GLint *range, GLint *precision) {   // every precision is a 32-bit float or int
    (void)stage; int fl = prec <= 0x8df2;   // LOW / MEDIUM / HIGH_FLOAT, then the INTs
    if (range) { range[0] = fl ? 127 : 31; range[1] = fl ? 127 : 30; }
    if (precision) *precision = fl ? 23 : 0;
}
GL(glGetUniformLocation, GLint, GLuint pr, const GLchar *name) {
    gprog_t *p = prog_of(pr); if (!p || !p->linked) return -1;
    char nm[64]; snprintf(nm, sizeof nm, "%s", name); int elem = 0;
    char *br = strchr(nm, '[');
    if (br) { elem = atoi(br + 1); *br = 0; }
    for (int i = 0; i < p->nu; i++) if (!strcmp(p->u[i].name, nm)) return (elem < 0 || elem >= (p->u[i].count ? p->u[i].count : 1)) ? -1 : i * 4096 + elem;
    return -1;
}
// Write count elements from location loc of the current program: comps values each (a matrix: cols columns of comps / cols), into every stage that has the uniform.
// The default block is laid out as std140: a matrix column takes 16 bytes
static void uni_write(GLint loc, GLsizei count, const void *v, int comps, int is_int, int transpose, int cols) {
    gprog_t *p = prog_of(C.prog);
    if (!p || !p->linked || loc < 0 || count <= 0) return;
    int i = loc / 4096, el = loc % 4096;
    if (i >= p->nu) return;
    puni_t *u = &p->u[i];
    if (is_samp(u->type)) { if (is_int) u->unit = *(const GLint *)v; return; }
    int n = u->count ? u->count : 1, rows = cols ? comps / cols : comps, size = cols ? cols * 16 : 16;
    if (el + count > n) count = n - el;
    for (int st = 0; st < 2; st++) {
        if (u->off[st] < 0) continue;
        int stride = u->stride[st] ? u->stride[st] : size;
        for (GLsizei k = 0; k < count; k++) {
            uint8_t *d = p->ub[st] + u->off[st] + (size_t)(el + k) * (size_t)stride;
            if (d + size > p->ub[st] + p->si[st]->ub_size + 16) break;
            const float *s = (const float *)v + (size_t)k * (size_t)comps;
            if (cols) { for (int c = 0; c < cols; c++) for (int r = 0; r < rows; r++) ((float *)(d + c * 16))[r] = transpose ? s[r * cols + c] : s[c * rows + r]; }
            else memcpy(d, s, (size_t)umin((uint32_t)comps, (uint32_t)tcomps(u->type)) * 4);
        }
        p->ub_gen[st]++;
    }
}
// Integer values for a float uniform are converted (GL would reject them; NVIDIA's driver converts), as are float values for an integer one
static void uni_ints(GLint loc, GLsizei n, const GLint *v, int comps) {
    gprog_t *p = prog_of(C.prog);
    if (p && p->linked && loc >= 0 && loc / 4096 < p->nu && !is_samp(p->u[loc / 4096].type) && !is_intt(p->u[loc / 4096].type)) {
        float f[64]; for (GLsizei k = 0; k < n && (k + 1) * comps <= 64; k++) for (int c = 0; c < comps; c++) f[k * comps + c] = (float)v[k * comps + c];
        uni_write(loc, n > 64 / comps ? 64 / comps : n, f, comps, 0, 0, 0); return;
    }
    uni_write(loc, n, v, comps, 1, 0, 0);
}
static void uni_floats(GLint loc, GLsizei n, const GLfloat *v, int comps) {
    gprog_t *p = prog_of(C.prog);
    if (p && p->linked && loc >= 0 && loc / 4096 < p->nu && is_intt(p->u[loc / 4096].type)) {
        GLint iv[64]; for (GLsizei k = 0; k < n && (k + 1) * comps <= 64; k++) for (int c = 0; c < comps; c++) iv[k * comps + c] = (GLint)v[k * comps + c];
        uni_write(loc, n > 64 / comps ? 64 / comps : n, iv, comps, 1, 0, 0); return;
    }
    uni_write(loc, n, v, comps, 0, 0, 0);
}
GL(glUniform1fv, void, GLint loc, GLsizei n, const GLfloat *v) { uni_floats(loc, n, v, 1); }
GL(glUniform2fv, void, GLint loc, GLsizei n, const GLfloat *v) { uni_floats(loc, n, v, 2); }
GL(glUniform3fv, void, GLint loc, GLsizei n, const GLfloat *v) { uni_floats(loc, n, v, 3); }
GL(glUniform4fv, void, GLint loc, GLsizei n, const GLfloat *v) { uni_floats(loc, n, v, 4); }
GL(glUniform1f, void, GLint loc, GLfloat x) { uni_floats(loc, 1, &x, 1); }
GL(glUniform2f, void, GLint loc, GLfloat x, GLfloat y) { const GLfloat v[] = {x, y}; uni_floats(loc, 1, v, 2); }
GL(glUniform3f, void, GLint loc, GLfloat x, GLfloat y, GLfloat z) { const GLfloat v[] = {x, y, z}; uni_floats(loc, 1, v, 3); }
GL(glUniform4f, void, GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { const GLfloat v[] = {x, y, z, w}; uni_floats(loc, 1, v, 4); }
GL(glUniform1i, void, GLint loc, GLint v) { uni_ints(loc, 1, &v, 1); }
GL(glUniform1iv, void, GLint loc, GLsizei n, const GLint *v) { uni_ints(loc, n, v, 1); }
GL(glUniform2iv, void, GLint loc, GLsizei n, const GLint *v) { uni_ints(loc, n, v, 2); }
GL(glUniform3iv, void, GLint loc, GLsizei n, const GLint *v) { uni_ints(loc, n, v, 3); }
GL(glUniform4iv, void, GLint loc, GLsizei n, const GLint *v) { uni_ints(loc, n, v, 4); }
GL(glUniform1uiv, void, GLint loc, GLsizei n, const GLuint *v) { uni_write(loc, n, v, 1, 1, 0, 0); }
GL(glUniform2uiv, void, GLint loc, GLsizei n, const GLuint *v) { uni_write(loc, n, v, 2, 1, 0, 0); }
GL(glUniform3uiv, void, GLint loc, GLsizei n, const GLuint *v) { uni_write(loc, n, v, 3, 1, 0, 0); }
GL(glUniform4uiv, void, GLint loc, GLsizei n, const GLuint *v) { uni_write(loc, n, v, 4, 1, 0, 0); }
#define UMAT(name, c, r) GL(name, void, GLint loc, GLsizei n, uint32_t transpose, const GLfloat *v) { uni_write(loc, n, v, (c) * (r), 0, (transpose & 0xff) != 0, c); }
UMAT(glUniformMatrix2fv, 2, 2) UMAT(glUniformMatrix3fv, 3, 3) UMAT(glUniformMatrix4fv, 4, 4) UMAT(glUniformMatrix2x3fv, 2, 3) UMAT(glUniformMatrix2x4fv, 2, 4)
UMAT(glUniformMatrix3x2fv, 3, 2) UMAT(glUniformMatrix3x4fv, 3, 4) UMAT(glUniformMatrix4x2fv, 4, 2) UMAT(glUniformMatrix4x3fv, 4, 3)
// Values read back from the program's own copy (LÖVE keeps the values a shader initialises its uniforms with): the first stage that has the uniform
static void uni_read(GLuint pr, GLint loc, void *out, int as_int) {
    gprog_t *p = prog_of(pr);
    if (!p || !p->linked || loc < 0 || loc / 4096 >= p->nu) return;
    const puni_t *u = &p->u[loc / 4096]; int el = loc % 4096;
    if (is_samp(u->type)) { if (as_int) *(GLint *)out = u->unit; else *(GLfloat *)out = (GLfloat)u->unit; return; }
    int st = u->off[0] >= 0 ? 0 : 1; if (u->off[st] < 0) return;
    int cols = u->type == T_MAT4 ? 4 : u->type == T_MAT3 ? 3 : u->type == T_MAT2 ? 2 : 0, rows = cols ? cols : tcomps(u->type);
    const uint8_t *d = p->ub[st] + u->off[st] + (size_t)el * (size_t)(u->stride[st] ? u->stride[st] : cols ? cols * 16 : 16);
    for (int c = 0; c < (cols ? cols : 1); c++) for (int r = 0; r < rows; r++) {
        const uint8_t *s = d + c * 16 + r * 4; int k = c * rows + r;
        if (as_int == is_intt(u->type)) memcpy((uint8_t *)out + k * 4, s, 4);
        else if (as_int) { float f; memcpy(&f, s, 4); ((GLint *)out)[k] = (GLint)f; }
        else { GLint i; memcpy(&i, s, 4); ((GLfloat *)out)[k] = (GLfloat)i; }
    }
}
GL(glGetUniformfv, void, GLuint pr, GLint loc, GLfloat *v) { uni_read(pr, loc, v, 0); }
GL(glGetUniformiv, void, GLuint pr, GLint loc, GLint *v) { uni_read(pr, loc, v, 1); }
GL(glGetUniformuiv, void, GLuint pr, GLint loc, GLuint *v) { uni_read(pr, loc, v, 1); }

// ---------- The layer's start ----------
// The backend's device (it comes up at its first user, as the NVN backend's does), the layer's timeline, its command pools, the textures unbound samplers read, the clear
// pipeline's modules, the shader table; the present flips (GL's rows are stored bottom first)
static void dev_init_once(void) {
    g_log = getenv("DM_GL_LOG") && atoi(getenv("DM_GL_LOG"));
    lib_dir(); sh_load();
    if (!hvk_up()) { LOG("[gl] [RED] no Vulkan device for the GL layer (the backend has none: see its red line): GL calls keep their state, nothing is drawn"); return; }
    VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti, 0};
    if (vkCreateSemaphore(hvk.d, &si, NULL, &g_tl)) { LOG("[gl] [RED] the GL layer's timeline semaphore could not be made: nothing is drawn"); g_tl = VK_NULL_HANDLE; return; }
    for (int i = 0; i < NSLOT; i++) {
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, hvk.dev.queue_family};
        VkCommandBuffer cbs[2];
        if (vkCreateCommandPool(hvk.d, &pi, NULL, &g_slot[i].pool)) { LOG("[gl] [RED] the GL layer's command pools could not be made: nothing is drawn"); return; }
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_slot[i].pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
        if (vkAllocateCommandBuffers(hvk.d, &ai, cbs)) { LOG("[gl] [RED] the GL layer's command buffers could not be made: nothing is drawn"); return; }
        g_slot[i].pre = cbs[0]; g_slot[i].main = cbs[1];
    }
    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &pc};
    g_clr_mod[0] = spv_module(g_clr_vs, sizeof g_clr_vs / 4); g_clr_mod[1] = spv_module(g_clr_fs, sizeof g_clr_fs / 4);
    if (vkCreatePipelineLayout(hvk.d, &pli, NULL, &g_clr_pl) || !g_clr_mod[0] || !g_clr_mod[1]) LOG("[gl] [RED] the GL layer's clear pipeline cannot be made: masked clears are skipped");
    g_ubo_align = hvk.dev.props.limits.minUniformBufferOffsetAlignment > 16 ? hvk.dev.props.limits.minUniformBufferOffsetAlignment : 16;
    g_on = 1;
    dummies_make();
    be_set_origin_lower_left(1);   // the window image is stored bottom row first; the present flips it
    GLOG("on the backend's Vulkan device '%s' (%s); %d shader table entries in %s", hvk.dev.props.deviceName, hvk.dev.driver_name, g_nsh, g_libdir);
}
static void dev_init(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, dev_init_once); }
void dm_gl_drain(void) { if (g_on && g_last) serial_wait(g_last, UINT64_MAX); }   // dm_gfx_suspend (hle_nvn.c): the layer's submitted work done before the app is suspended

// ---------- EGL ----------
#define EGL_TRUE 1
static struct { int init; int surface; uint32_t w, h; } g_egl;
static uint64_t g_frames;
// Every 300 frames: draws, passes, uploads; every 1800: memory (the layer's images and buffers, its transient blocks), as the NVN layer's memory report
static void frame_end(void) {
    static guest_gl_stats p;
    uint64_t f = ++g_frames; g_st.frames = f;
    if (f <= 3 || f % 300 == 0) {
        GLOG("frame %llu: draws %llu, passes %llu, uploads %llu (%llu in command order), submissions %llu (since the last record, %llu frames); pipelines so far %llu",
             (unsigned long long)f, (unsigned long long)(g_st.draws - p.draws), (unsigned long long)(g_st.passes - p.passes),
             (unsigned long long)(g_st.uploads_pre + g_st.uploads_ordered - p.uploads_pre - p.uploads_ordered), (unsigned long long)(g_st.uploads_ordered - p.uploads_ordered),
             (unsigned long long)(g_st.submits - p.submits), (unsigned long long)(f <= 3 ? 1 : 300), (unsigned long long)g_st.pipelines);
        p = g_st;
    }
    if (f % 1800 == 0) {
        uint32_t nt = 0, nb = 0;
        for (uint32_t i = 1; i < g_ntexs; i++) nt += g_texs[i] && g_texs[i]->im.img;
        for (uint32_t i = 1; i < g_nbufs; i++) nb += g_bufs[i] && g_bufs[i]->m.buf;
        GLOG("memory at frame %llu: images %.0f MB (%u, %u textures), buffers %.0f MB (%u), %d transient blocks", (unsigned long long)f, g_mem_tex / 1048576.0, g_n_tex_img, nt,
             g_mem_buf / 1048576.0, nb, g_narena);
    }
    hvk_cache_tick(plat_now_ns());   // the disk cache is written 10 s after the last of a burst of new pipelines
    dm_guest_frame_tick();
}
EGL(eglGetDisplay, EGLDisplay, void *native) { (void)native; dev_init(); return (EGLDisplay)&g_egl; }
EGL(eglInitialize, EGLBoolean, EGLDisplay d, EGLint *major, EGLint *minor) { (void)d; dev_init(); g_egl.init = 1; if (major) *major = 1; if (minor) *minor = 5; return EGL_TRUE; }
EGL(eglTerminate, EGLBoolean, EGLDisplay d) { (void)d; return EGL_TRUE; }
EGL(eglReleaseThread, EGLBoolean, void) { return EGL_TRUE; }
// SDL binds 0 (its API before it has chosen one) when it makes the window again (Balatro's love.window.setMode): nothing to change
EGL(eglBindAPI, EGLBoolean, EGLenum api) { if (api && api != 0x30a2) RED1("eglBindAPI(%#x): only desktop OpenGL (0x30a2) is implemented", api); return EGL_TRUE; }
// One configuration: RGBA8, depth 24 (32-bit float here where the device has it), stencil 8, a window surface, desktop OpenGL; it matches the game's attributes
EGL(eglChooseConfig, EGLBoolean, EGLDisplay d, const EGLint *attr, EGLConfig *cfg, EGLint n, EGLint *num) {
    (void)d; (void)attr;
    if (cfg && n > 0) cfg[0] = (EGLConfig)(uintptr_t)1;
    if (num) *num = 1;
    return EGL_TRUE;
}
// The one configuration's attributes (SDL reads them back after eglChooseConfig): what eglChooseConfig says it is
EGL(eglGetConfigAttrib, EGLBoolean, EGLDisplay d, EGLConfig cfg, EGLint attr, EGLint *v) {
    (void)d; (void)cfg;
    switch (attr) {
        case 0x3020: *v = 32; break;   // BUFFER_SIZE
        case 0x3021: case 0x3022: case 0x3023: case 0x3024: *v = 8; break;   // ALPHA / BLUE / GREEN / RED_SIZE
        case 0x3025: *v = 24; break;   // DEPTH_SIZE
        case 0x3026: *v = 8; break;   // STENCIL_SIZE
        case 0x3028: *v = 1; break;   // CONFIG_ID
        case 0x3031: case 0x3032: *v = 0; break;   // SAMPLES, SAMPLE_BUFFERS
        case 0x302e: *v = 0; break;   // NATIVE_VISUAL_ID
        case 0x3033: *v = 0x5; break;   // SURFACE_TYPE: PBUFFER | WINDOW
        case 0x3040: case 0x3042: *v = 0x8; break;   // RENDERABLE_TYPE, CONFORMANT: OPENGL_BIT
        case 0x3027: *v = 0x3038; break;   // CONFIG_CAVEAT: NONE
        default: RED1("eglGetConfigAttrib(%#x) not implemented (0)", attr); *v = 0; break;
    }
    return EGL_TRUE;
}
EGL(eglQueryString, const char *, EGLDisplay d, EGLint name) {
    (void)d;
    switch (name) { case 0x3053: return "NVIDIA"; case 0x3054: return "1.5"; case 0x3055: return "EGL_KHR_create_context"; case 0x308d: return "OpenGL"; }   // VENDOR, VERSION, EXTENSIONS, CLIENT_APIS
    return NULL;
}
EGL(eglGetError, EGLint, void) { return 0x3000; }   // SUCCESS
EGL(eglQueryAPI, EGLenum, void) { return 0x30a2; }   // OPENGL_API
EGL(eglWaitNative, EGLBoolean, EGLint engine) { (void)engine; return EGL_TRUE; }
EGL(eglWaitGL, EGLBoolean, void) { return EGL_TRUE; }
EGL(eglDestroyContext, EGLBoolean, EGLDisplay d, EGLContext c) { (void)d; (void)c; return EGL_TRUE; }
EGL(eglCreatePbufferSurface, EGLSurface, EGLDisplay d, EGLConfig cfg, const EGLint *attr) { (void)d; (void)cfg; (void)attr; RED1("eglCreatePbufferSurface: drawing into it is not implemented"); return (EGLSurface)(uintptr_t)4; }
EGL(eglCreateContext, EGLContext, EGLDisplay d, EGLConfig cfg, EGLContext share, const EGLint *attr) {
    (void)d; (void)cfg;
    int major = 0, minor = 0; for (int i = 0; attr && attr[i] != 0x3038 && i < 32; i += 2) { if (attr[i] == 0x3098) major = attr[i + 1]; else if (attr[i] == 0x30fb) minor = attr[i + 1]; }
    if (share) RED1("shared EGL contexts are not implemented (one context)");
    GLOG("EGL context: OpenGL %d.%d", major, minor);
    return (EGLContext)(uintptr_t)2;
}
EGL(eglCreateWindowSurface, EGLSurface, EGLDisplay d, EGLConfig cfg, void *win, const EGLint *attr) {
    (void)d; (void)cfg; (void)win; (void)attr; dev_init();
    int w, h; win_size(&w, &h);
    if (P.open) pass_end();
    if (!g_win.made || g_win.w != (uint32_t)w || g_win.h != (uint32_t)h) win_make((uint32_t)w, (uint32_t)h);
    g_egl.surface = 1;
    return (EGLSurface)(uintptr_t)3;
}
EGL(eglDestroySurface, EGLBoolean, EGLDisplay d, EGLSurface s) { (void)d; (void)s; return EGL_TRUE; }
EGL(eglMakeCurrent, EGLBoolean, EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c) {
    (void)d; (void)rd;
    static pthread_t th; static int known, sized;
    if (c && dr) { pthread_t me = pthread_self(); if (!known || !pthread_equal(th, me)) { th = me; known = 1; OLOG("context current on a new thread"); } }
    // a context's viewport and scissor box start as its first drawable's size (LÖVE reads them back at start)
    if (c && dr && !sized && g_win.made) { sized = 1; if (!C.vp[2] && !C.vp[3]) { C.vp[2] = C.sc[2] = (GLint)g_win.w; C.vp[3] = C.sc[3] = (GLint)g_win.h; } }
    return EGL_TRUE;
}
EGL(eglSwapInterval, EGLBoolean, EGLDisplay d, EGLint n) { (void)d; if (n != C.swap_interval) { GLOG("swap interval %d", n); C.swap_interval = n; } return EGL_TRUE; }
static uint64_t g_next_swap;   // pacing without a vsync source: the earliest time of the next swap
EGL(eglSwapBuffers, EGLBoolean, EGLDisplay d, EGLSurface s) {
    (void)d; (void)s;
    if (g_on) {
        dm_gfx_enter();
        win_ensure();
        if (g_pend_clear.bits) clears_flush();
        int k = g_win.cur;
        if (g_win.made && g_win.general[k]) {   // the frame drew into it: back to the layout the presenter copies from
            pass_end(); img_barrier(cb_main(), g_win.col[k].img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, HVK_EXT_LAYOUT); g_win.general[k] = 0;
        }
        uint64_t v = cb_commit();
        int vs = C.swap_interval < 1 ? 1 : C.swap_interval > 4 ? 4 : C.swap_interval;
        if (g_win.made) {
            g_win.ticket[k] = be_present_ext(g_win.serial[k], vs / 60.0, (void *)g_tl, v, NULL);
            g_win.cur = (k + 1) % NWIN;
            if (g_win.ticket[g_win.cur]) be_present_wait(g_win.ticket[g_win.cur]);   // its previous present has stopped reading it, so drawing into it again is safe
        }
        // Paced by the vsync source when the backend has one, as the guest Vulkan layer's acquire: the swap interval in vsyncs; else a clock of its own
        uint64_t period = 1000000000ull * (uint64_t)vs / 60, now = now_ns();
        if (C.swap_interval > 0 && be_vsync_active() && be_vsync_wait(vs, 2 * period)) g_next_swap = now_ns() + period;
        else if (C.swap_interval > 0) {
            if (g_next_swap > now) plat_sleep_ns(g_next_swap - now);
            now = now_ns(); g_next_swap = g_next_swap && now < g_next_swap + period ? g_next_swap + period : now + period;
        }
        dm_gfx_leave();
    }
    frame_end();
    return EGL_TRUE;
}
// eglGetProcAddress: GL names from the same table as direct imports; anything else is null (the game's loader asks for every GL name it knows). It returns the host function,
// declared with EGL's function pointer type: the x86-64 engine's thunk of the entry hands the guest the function's guest-visible address, and a DM_GL_STUBS stub, which has
// no host-call thunk, becomes the engine's stub of that name (dm/host/tools/gen_hle_thunks.py, dm/host/aot.h dm_guest_fnptr_named), whose first call says UNIMPL <name>
static void *gl_lookup(const char *name) {
    size_t sz = 0; const hle_ent *t = plat_table_swhle(&sz);
    for (size_t i = 0; t && i < sz / sizeof *t; i++) if (t[i].name && !strcmp(t[i].name, name)) return t[i].fn;
    return NULL;
}
// DM_GL_STUBS=1 (bring-up): a GL name the layer does not implement gets a stub of its own instead of a null pointer, which reports its first call and returns 0; a game that calls
// such a function then says which one instead of jumping to address 0. Off by default: a loader may take a name's null pointer to mean "not there". The stubs are 4,096 C
// functions (hle_gl.m's are an assembly table), each knowing its index
#define NSTUB 4096
static const char *g_stub_name[NSTUB]; static _Atomic uint8_t g_stub_seen[NSTUB]; static int g_nstub;
static uint64_t gl_stub_c(int k, uint64_t x0, uint64_t x1, uint64_t x2) {
    if (k < NSTUB && !atomic_exchange(&g_stub_seen[k], 1)) GLOG("[RED] %s called: not implemented, returns 0 (x0=%#llx x1=%#llx x2=%#llx)", g_stub_name[k] ? g_stub_name[k] : "?",
                                                                (unsigned long long)x0, (unsigned long long)x1, (unsigned long long)x2);
    return 0;
}
#define STUB1(k) static uint64_t gl_stub_##k(uint64_t a, uint64_t b, uint64_t c) { return gl_stub_c(k, a, b, c); }
#define STUB16(p) STUB1(p##0) STUB1(p##1) STUB1(p##2) STUB1(p##3) STUB1(p##4) STUB1(p##5) STUB1(p##6) STUB1(p##7) STUB1(p##8) STUB1(p##9) STUB1(p##a) STUB1(p##b) STUB1(p##c) STUB1(p##d) STUB1(p##e) STUB1(p##f)
#define STUB256(p) STUB16(p##0) STUB16(p##1) STUB16(p##2) STUB16(p##3) STUB16(p##4) STUB16(p##5) STUB16(p##6) STUB16(p##7) STUB16(p##8) STUB16(p##9) STUB16(p##a) STUB16(p##b) STUB16(p##c) STUB16(p##d) STUB16(p##e) STUB16(p##f)
#define STUB4096(p) STUB256(p##0) STUB256(p##1) STUB256(p##2) STUB256(p##3) STUB256(p##4) STUB256(p##5) STUB256(p##6) STUB256(p##7) STUB256(p##8) STUB256(p##9) STUB256(p##a) STUB256(p##b) STUB256(p##c) STUB256(p##d) STUB256(p##e) STUB256(p##f)
STUB4096(0x)
#define STUBT1(k) (void *)gl_stub_##k,
#define STUBT16(p) STUBT1(p##0) STUBT1(p##1) STUBT1(p##2) STUBT1(p##3) STUBT1(p##4) STUBT1(p##5) STUBT1(p##6) STUBT1(p##7) STUBT1(p##8) STUBT1(p##9) STUBT1(p##a) STUBT1(p##b) STUBT1(p##c) STUBT1(p##d) STUBT1(p##e) STUBT1(p##f)
#define STUBT256(p) STUBT16(p##0) STUBT16(p##1) STUBT16(p##2) STUBT16(p##3) STUBT16(p##4) STUBT16(p##5) STUBT16(p##6) STUBT16(p##7) STUBT16(p##8) STUBT16(p##9) STUBT16(p##a) STUBT16(p##b) STUBT16(p##c) STUBT16(p##d) STUBT16(p##e) STUBT16(p##f)
#define STUBT4096(p) STUBT256(p##0) STUBT256(p##1) STUBT256(p##2) STUBT256(p##3) STUBT256(p##4) STUBT256(p##5) STUBT256(p##6) STUBT256(p##7) STUBT256(p##8) STUBT256(p##9) STUBT256(p##a) STUBT256(p##b) STUBT256(p##c) STUBT256(p##d) STUBT256(p##e) STUBT256(p##f)
static void *const g_stubs[NSTUB] = {STUBT4096(0x)};
static void *gl_stub(const char *name) {   // under eglGetProcAddress's lock
    static int on = -1; if (on < 0) on = getenv("DM_GL_STUBS") && atoi(getenv("DM_GL_STUBS"));
    if (!on || g_nstub == NSTUB) return NULL;
    g_stub_name[g_nstub] = name;   // the game's own string (its loader's name table)
    return g_stubs[g_nstub++];
}
EGL(eglGetProcAddress, __eglMustCastToProperFunctionPointerType, const char *name) {
    static int asked, found; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    void *fn = name && !strncmp(name, "gl", 2) ? gl_lookup(name) : NULL;
    pthread_mutex_lock(&mu);
    asked++; if (fn) found++;
    if (asked % 500 == 0 || (fn && g_log)) GLOG("eglGetProcAddress: %d names asked, %d implemented (last %s)", asked, found, name);
    if (!fn && name && !strncmp(name, "gl", 2)) fn = gl_stub(name);
    pthread_mutex_unlock(&mu);
    return (__eglMustCastToProperFunctionPointerType)fn;
}

// ---------- The layer's end (guest_gl.h) ----------
// Every Vulkan object of the layer released, after its last submission and present: the GL objects' images, buffers, programs and pipelines, the window surface, the transient
// and slab memory, the samplers, the clear pipelines, the dummies, the pools and the timeline. GL calls draw nothing afterwards (the state machine stays)
void guest_gl_shutdown(void) {
    if (!g_on) return;
    if (g_pend_clear.bits) clears_flush();
    dm_gfx_enter(); uint64_t v = cb_commit(); dm_gfx_leave();
    serial_wait(v, UINT64_MAX);
    for (int i = 0; i < NWIN; i++) if (g_win.made && g_win.ticket[i]) be_present_wait(g_win.ticket[i]);
    for (uint32_t i = 1; i < g_ntexs; i++) if (g_texs[i]) { tex_drop(g_texs[i]); free(g_texs[i]); g_texs[i] = NULL; }
    for (uint32_t i = 1; i < g_nrbs; i++) if (g_rbs[i]) { gimg_drop(&g_rbs[i]->im, 0); free(g_rbs[i]); g_rbs[i] = NULL; }
    for (uint32_t i = 1; i < g_nbufs; i++) if (g_bufs[i]) { bmem_release(&g_bufs[i]->m, 0); free(g_bufs[i]); g_bufs[i] = NULL; }
    for (uint32_t i = 1; i < g_nsps; i++) if (g_sps[i] && g_sps[i]->is_prog) prog_clear(&g_sps[i]->p);
    for (int i = 0; g_win.made && i < NWIN; i++) { hvk_ext_tex_drop(g_win.serial[i]); gimg_drop(&g_win.col[i], 0); }
    if (g_win.made) gimg_drop(&g_win.ds, 0);
    g_win.made = 0;
    for (int i = 0; i < 5; i++) { defer(D_VIEW, 0, H(g_dummy_view[i]), 0, 0); g_dummy_view[i] = VK_NULL_HANDLE; gimg_drop(&g_dummy[i], 0); }
    defer_run();
    for (int c = 0; c <= SLAB_MAX - SLAB_MIN; c++) for (slab_t *s = g_slabs[c], *nx; s; s = nx) { nx = s->next; hvk_buffer_free(s->buf, s->mem); free(s->fl); free(s); g_slabs[c] = NULL; }
    for (int i = 0; i < g_narena; i++) hvk_buffer_free(g_arena[i].buf, g_arena[i].mem);
    free(g_arena); g_arena = NULL; g_narena = g_caparena = 0; g_cur_arena = -1;
    for (int i = 0; i < g_nsmp; i++) vkDestroySampler(hvk.d, g_smp[i].s, NULL);
    g_nsmp = 0;
    vkDestroySampler(hvk.d, g_samp0, NULL); vkDestroySampler(hvk.d, g_samp0_cmp, NULL);
    for (int i = 0; i < g_nclr; i++) if (g_clr[i].p) vkDestroyPipeline(hvk.d, g_clr[i].p, NULL);
    g_nclr = 0;
    vkDestroyPipelineLayout(hvk.d, g_clr_pl, NULL); vkDestroyShaderModule(hvk.d, g_clr_mod[0], NULL); vkDestroyShaderModule(hvk.d, g_clr_mod[1], NULL);
    for (int i = 0; i < NSLOT; i++) {
        for (int k = 0; k < g_slot[i].ndp; k++) vkDestroyDescriptorPool(hvk.d, g_slot[i].dp[k], NULL);
        vkDestroyCommandPool(hvk.d, g_slot[i].pool, NULL);
    }
    memset(g_slot, 0, sizeof g_slot); g_cur = -1;
    vkDestroySemaphore(hvk.d, g_tl, NULL); g_tl = VK_NULL_HANDLE;
    g_on = 0;
}
