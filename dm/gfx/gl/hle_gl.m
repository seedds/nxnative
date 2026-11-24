// OpenGL → Metal layer for games that render with OpenGL through EGL (Digimon Story Cyber Sleuth: Complete Edition: Media.Vision's MVGL engine on NVIDIA's desktop GL 4.5 core
// driver, subsdk0, which the host does not load; Balatro: LÖVE 11.4 on SDL, the same driver). main imports the EGL functions, which bind to the ones here through the host's import
// table (hle.h swhle). GL functions use that same table for direct imports and eglGetProcAddress (a glad loader asks for 2,956 names; Digimon's code calls 79 of them,
// LÖVE's about 150, counted with dm/tools/loader_census.py). Only those are implemented; every other loader name gets a null pointer (DM_GL_STUBS=1: a stub that reports its first call).
// LÖVE takes the core profile's paths (GL_CONTEXT_PROFILE_MASK): its vertex streams are persistently mapped buffers (glBufferStorage, glMapBufferRange) that fences
// (glFenceSync / glClientWaitSync) keep the CPU from rewriting while a frame reads them; with a compatibility profile it would draw from client memory, which Metal cannot.
// Model:
//   · one context, used from the thread that made it current; GL calls are encoded into Metal as they come: one MTLCommandBuffer per frame on this layer's own queue,
//     committed at eglSwapBuffers (signalling g_ev with its serial for the present path), a render encoder per stretch of draws into one framebuffer;
//   · rendering keeps GL's orientation: images are stored bottom row first, viewports are flipped (negative height), the winding is reversed, and the present flips the
//     window image (the NVN layer's lower-left origin, mtl_set_origin_lower_left); scissor rectangles and gl_FragCoord then need no change;
//   · the window surface is three textures registered with the NVN layer's present path (mtl_ext_tex), as the Vulkan layer's swapchain, plus a depth / stencil texture;
//   · buffers are shared MTLBuffers; glBufferData on a buffer the GPU may still read gets a new one (orphaning, as GL drivers do);
//   · textures are created on first use from the levels given so far (the game defines a texture level by level, and Metal needs the level count up front); an upload into a
//     texture the GPU may still read goes through a blit in command order, any other is a CPU write;
//   · shaders come from the offline library built by dm/gfx/tools/gl_shaders.py (<DM_MSL_DIR>/gl/): a shader is found by the hash of its source and its stage; its table row lists the
//     vertex inputs, the members of its default uniform block (glUniform* writes CPU copies, snapshotted into transient buffers) and its samplers. Each stage reads all its
//     resources through one argument buffer at buffer(0), independent of GL's binding numbers and Metal's 16 direct sampler slots. Mac capture uses the same compiler for missing stages.
//     glGetAttribLocation returns the Metal attribute index of the input, so the game's attribute index is the Metal one; vertex buffers are at slot 30 − index.
// Environment: DM_GL_LOG=1 logs object creation and the deletion of shaders and programs; DM_GL_CAPTURE=<dir> writes every shader source the game compiles (<key>.vert / <key>.frag, read by gl_shaders.py).
#include "../../host/hle.h"
#include "../nvn/nvn_mtl.h"
#include "../nvn/host_msl.h"
#include "gl_aliases.h"
#import <Metal/Metal.h>
#include <CommonCrypto/CommonDigest.h>
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <math.h>
#include <stdatomic.h>
#include <time.h>
#if TARGET_OS_OSX
#include <crt_externs.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#endif

int mtl_fps_target(void);
int mtl_link_active(void); int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns);   // nvn_mtl.m
void dm_vi_layer_size(int *w, int *h);   // hle_vi.c: the size of the last sized vi::CreateLayer

#define GLOG(...) LOG("[gl] " __VA_ARGS__)
#define RED1(...) do { static int once_; if (!once_) { once_ = 1; LOG("[gl] [RED] " __VA_ARGS__); } } while (0)
static int g_log;
#define OLOG(...) do { if (g_log) GLOG(__VA_ARGS__); } while (0)

typedef uint32_t GLenum, GLuint, GLbitfield; typedef int32_t GLint, GLsizei; typedef float GLfloat; typedef double GLdouble; typedef char GLchar; typedef intptr_t GLsizeiptr, GLintptr;
typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface; typedef int32_t EGLint; typedef uint32_t EGLBoolean, EGLenum;

// GL: one GL function, registered in the host's import table for both direct imports and eglGetProcAddress. GLB: the same, run on the thread's big host stack
// (hle_bigstack.c, register arguments only) for the functions that call into Metal: a game thread's stack can be small. EGL uses the same import table and big-stack entry.
// GLboolean and other arguments narrower than 32 bits are taken as uint32_t and truncated here (hle.h: Switch callers do not extend them)
#define GL(name, ret, ...)                                                                       \
    static ret h_##name(__VA_ARGS__);                                                            \
    DM_REGISTER("swhle", hle_ent, e_##name, {#name, (void *)h_##name}); \
    static ret h_##name(__VA_ARGS__)
#define BIGSTUB(fn)                                                                              \
    extern const char fn##_bs[];                                                                 \
    __asm__(".text\n.p2align 2\n.globl _" #fn "_bs\n.private_extern _" #fn "_bs\n_" #fn "_bs:\n" \
            "    adrp x16, _" #fn "@PAGE\n    add x16, x16, _" #fn "@PAGEOFF\n    b _dm_bigstack_enter\n")
#define GLB(name, ret, ...)                                                                      \
    static ret h_##name(__VA_ARGS__) __attribute__((used));                                      \
    BIGSTUB(h_##name);                                                                           \
    DM_REGISTER("swhle", hle_ent, e_##name, {#name, (void *)h_##name##_bs}); \
    static ret h_##name(__VA_ARGS__)
#define EGL(name, ret, ...)                                                                      \
    static ret h_##name(__VA_ARGS__) __attribute__((used));                                      \
    BIGSTUB(h_##name);                                                                           \
    DM_REGISTER("swhle", hle_ent, e_##name, {#name, (void *)h_##name##_bs}); \
    static ret h_##name(__VA_ARGS__)

// ---------- GL enums ----------
enum {
    GL_NO_ERROR = 0, GL_INVALID_ENUM = 0x500, GL_INVALID_VALUE, GL_INVALID_OPERATION, GL_OUT_OF_MEMORY = 0x505,
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
    GL_KEEP = 0x1e00, GL_REPLACE, GL_INCR, GL_DECR, GL_VENDOR = 0x1f00, GL_RENDERER, GL_VERSION, GL_EXTENSIONS,
    GL_NEAREST = 0x2600, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST = 0x2700, GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR,
    GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER, GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_CLAMP = 0x2900, GL_REPEAT,
    GL_FUNC_ADD = 0x8006, GL_MIN, GL_MAX, GL_FUNC_SUBTRACT = 0x800a, GL_FUNC_REVERSE_SUBTRACT,
    GL_UNSIGNED_SHORT_4_4_4_4 = 0x8033, GL_UNSIGNED_SHORT_5_5_5_1, GL_POLYGON_OFFSET_FILL = 0x8037,
    GL_RGB8 = 0x8051, GL_RGBA4 = 0x8056, GL_RGB5_A1, GL_RGBA8, GL_RGB10_A2, GL_RGBA16 = 0x805b,
    GL_TEXTURE_WRAP_R = 0x8072, GL_COLOR_TABLE = 0x80d0, GL_BGRA = 0x80e1,
    GL_CLAMP_TO_BORDER = 0x812d, GL_CLAMP_TO_EDGE = 0x812f, GL_TEXTURE_MIN_LOD = 0x813a, GL_TEXTURE_MAX_LOD, GL_TEXTURE_MAX_LEVEL = 0x813d,
    GL_DEPTH_COMPONENT16 = 0x81a5, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT32,
    GL_MAJOR_VERSION = 0x821b, GL_MINOR_VERSION, GL_NUM_EXTENSIONS, GL_RG = 0x8227, GL_RG_INTEGER, GL_R8, GL_RG8 = 0x822b, GL_R16F = 0x822d, GL_R32F, GL_RG16F, GL_RG32F,
    GL_R16UI = 0x8234, GL_RG32UI = 0x823c, GL_RGBA32UI = 0x8d70, GL_RED_INTEGER = 0x8d94, GL_RGBA_INTEGER = 0x8d99,
    GL_UNSIGNED_SHORT_5_6_5 = 0x8363, GL_UNSIGNED_INT_2_10_10_10_REV = 0x8368, GL_MIRRORED_REPEAT = 0x8370,
    GL_COMPRESSED_RGB_S3TC_DXT1 = 0x83f0, GL_COMPRESSED_RGBA_S3TC_DXT1, GL_COMPRESSED_RGBA_S3TC_DXT3, GL_COMPRESSED_RGBA_S3TC_DXT5,
    GL_TEXTURE0 = 0x84c0, GL_MAX_RENDERBUFFER_SIZE = 0x84e8, GL_DEPTH_STENCIL = 0x84f9, GL_UNSIGNED_INT_24_8 = 0x84fa, GL_MAX_TEXTURE_LOD_BIAS = 0x84fd,
    GL_TEXTURE_MAX_ANISOTROPY = 0x84fe, GL_MAX_TEXTURE_MAX_ANISOTROPY, GL_TEXTURE_LOD_BIAS = 0x8501,
    GL_INCR_WRAP = 0x8507, GL_DECR_WRAP, GL_TEXTURE_CUBE_MAP = 0x8513, GL_TEXTURE_CUBE_MAP_POSITIVE_X = 0x8515, GL_TEXTURE_CUBE_MAP_NEGATIVE_Z = 0x851a,
    GL_MAX_VERTEX_ATTRIBS = 0x8869, GL_MAX_TEXTURE_IMAGE_UNITS = 0x8872, GL_RGBA32F = 0x8814, GL_RGB32F, GL_RGBA16F = 0x881a, GL_RGB16F,
    GL_TEXTURE_COMPARE_MODE = 0x884c, GL_TEXTURE_COMPARE_FUNC,
    GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER, GL_DEPTH24_STENCIL8 = 0x88f0, GL_UNIFORM_BUFFER = 0x8a11,
    GL_FRAGMENT_SHADER = 0x8b30, GL_VERTEX_SHADER, GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS = 0x8b4d, GL_SHADER_TYPE = 0x8b4f, GL_DELETE_STATUS = 0x8b80, GL_COMPILE_STATUS,
    GL_LINK_STATUS, GL_VALIDATE_STATUS, GL_INFO_LOG_LENGTH, GL_ATTACHED_SHADERS, GL_ACTIVE_UNIFORMS, GL_ACTIVE_UNIFORM_MAX_LENGTH, GL_SHADER_SOURCE_LENGTH,
    GL_ACTIVE_ATTRIBUTES, GL_ACTIVE_ATTRIBUTE_MAX_LENGTH, GL_SHADING_LANGUAGE_VERSION = 0x8b8c, GL_CURRENT_PROGRAM,
    GL_R11F_G11F_B10F = 0x8c3a, GL_UNSIGNED_INT_10F_11F_11F_REV = 0x8c3b, GL_SRGB8_ALPHA8 = 0x8c43,
    GL_TEXTURE_BUFFER = 0x8c2a, GL_MAX_TEXTURE_BUFFER_SIZE, GL_TEXTURE_BINDING_BUFFER,
    GL_SAMPLER_BUFFER = 0x8dc2, GL_UNSIGNED_INT_SAMPLER_2D = 0x8dd2,
    GL_COMPRESSED_SRGB_S3TC_DXT1 = 0x8c4c, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3,
    GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5, GL_DEPTH_COMPONENT32F = 0x8cac, GL_DEPTH32F_STENCIL8,
    GL_FRAMEBUFFER_BINDING = 0x8ca6, GL_RENDERBUFFER_BINDING, GL_COLOR_ATTACHMENT0 = 0x8ce0, GL_DEPTH_ATTACHMENT = 0x8d00, GL_STENCIL_ATTACHMENT = 0x8d20,
    GL_FRAMEBUFFER = 0x8d40, GL_RENDERBUFFER, GL_STENCIL_INDEX8 = 0x8d48, GL_RGB565 = 0x8d62, GL_MAX_SAMPLES = 0x8d57,
    GL_COMPRESSED_RED_RGTC1 = 0x8dbb, GL_COMPRESSED_SIGNED_RED_RGTC1, GL_COMPRESSED_RG_RGTC2, GL_COMPRESSED_SIGNED_RG_RGTC2,
    GL_MAX_VERTEX_UNIFORM_VECTORS = 0x8dfb, GL_MAX_VARYING_VECTORS, GL_MAX_FRAGMENT_UNIFORM_VECTORS,
    GL_COMPRESSED_RGBA_BPTC_UNORM = 0x8e8c, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT, GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT,
    GL_COMPRESSED_RGBA_ASTC_4x4 = 0x93b0, GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4 = 0x93d0,
    GL_DEPTH_STENCIL_ATTACHMENT = 0x821a,
    GL_BLEND_COLOR = 0x8005, GL_SAMPLE_MASK = 0x8e51, GL_MAX_SAMPLE_MASK_WORDS = 0x8e59,
    GL_QUERY_RESULT = 0x8866, GL_QUERY_RESULT_AVAILABLE, GL_SAMPLES_PASSED = 0x8914, GL_TIMESTAMP = 0x8e28,
    GL_ANY_SAMPLES_PASSED = 0x8c2f, GL_ANY_SAMPLES_PASSED_CONSERVATIVE = 0x8d6a,
    GL_CW = 0x900, GL_CCW, GL_POINT_SIZE = 0xb11, GL_POINT_SIZE_RANGE, GL_CULL_FACE_MODE = 0xb45, GL_FRONT_FACE, GL_SCISSOR_BOX = 0xc10, GL_PACK_ALIGNMENT = 0xd05,
    GL_MAX_VIEWPORT_DIMS = 0xd3a, GL_COLOR = 0x1800, GL_DEPTH, GL_STENCIL, GL_ZERO = 0, GL_ONE = 1,
    GL_TEXTURE_BINDING_3D = 0x806a, GL_TEXTURE_3D = 0x806f, GL_MAX_3D_TEXTURE_SIZE = 0x8073, GL_SAMPLE_BUFFERS = 0x80a8, GL_SAMPLES, GL_CONTEXT_FLAGS = 0x821e,
    GL_ALIASED_POINT_SIZE_RANGE = 0x846d, GL_TEXTURE_BINDING_CUBE_MAP = 0x8514, GL_MAX_CUBE_MAP_TEXTURE_SIZE = 0x851c, GL_MAX_DRAW_BUFFERS = 0x8824, GL_MAX_ARRAY_TEXTURE_LAYERS = 0x88ff,
    GL_FLOAT_VEC2 = 0x8b50, GL_FLOAT_VEC3, GL_FLOAT_VEC4, GL_INT_VEC2, GL_INT_VEC3, GL_INT_VEC4, GL_BOOL, GL_FLOAT_MAT2 = 0x8b5a, GL_FLOAT_MAT3, GL_FLOAT_MAT4, GL_SAMPLER_2D = 0x8b5e,
    GL_SAMPLER_3D, GL_SAMPLER_CUBE, GL_SAMPLER_2D_SHADOW = 0x8b62, GL_TEXTURE_2D_ARRAY = 0x8c1a, GL_TEXTURE_BINDING_2D_ARRAY = 0x8c1d,
    GL_READ_FRAMEBUFFER = 0x8ca8, GL_DRAW_FRAMEBUFFER, GL_READ_FRAMEBUFFER_BINDING, GL_RENDERBUFFER_SAMPLES, GL_FRAMEBUFFER_COMPLETE = 0x8cd5, GL_MAX_COLOR_ATTACHMENTS = 0x8cdf,
    GL_RENDERBUFFER_WIDTH = 0x8d42, GL_RENDERBUFFER_HEIGHT, GL_RENDERBUFFER_INTERNAL_FORMAT, GL_FRAMEBUFFER_SRGB = 0x8db9, GL_SAMPLER_2D_ARRAY = 0x8dc1,
    GL_UNSIGNED_INT_VEC2 = 0x8dc6, GL_UNSIGNED_INT_VEC3, GL_UNSIGNED_INT_VEC4, GL_TEXTURE_SWIZZLE_R = 0x8e42, GL_TEXTURE_SWIZZLE_A = 0x8e45, GL_TEXTURE_SWIZZLE_RGBA = 0x8e46,
    GL_ALREADY_SIGNALED = 0x911a, GL_TIMEOUT_EXPIRED, GL_CONDITION_SATISFIED, GL_CONTEXT_PROFILE_MASK = 0x9126,
};
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x8
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x1
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x100
#define GL_STENCIL_BUFFER_BIT 0x400
#define NGL_COLORS 8
#define NUNITS 96

static uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t umax(uint32_t a, uint32_t b) { return a > b ? a : b; }
static void *zalloc(size_t n) { void *p = calloc(1, n); if (!p) die("[gl] out of memory %zu", n); return p; }

// ---------- Device-wide state ----------
static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_q;
static id<MTLSharedEvent> g_ev;                  // signalled with a command buffer's serial at its end
static id<MTLTexture> g_tex0, g_cube0, g_tex3d0, g_arr0, g_uint0, g_tbuf0;   // opaque black for incomplete textures
static id<MTLSamplerState> g_samp0;
static id<MTLLibrary> g_lib, g_util;             // the game's shaders; the layer's own (clears)
static const char *g_capture_dir, *g_capture_compiler;
static uint64_t g_serial = 1;                    // serial of the command buffer being recorded (g_cb); committed ones have lower serials
static _Atomic uint64_t g_done;                  // highest serial whose command buffer has completed
static id<MTLCommandBuffer> g_cb;
static NSMutableSet<id<MTLSamplerState>> *g_cb_samplers;   // argument buffers do not retain the sampler states they reference
static id<MTLRenderCommandEncoder> g_enc;
static _Atomic uint64_t g_n_draw, g_n_pass, g_n_up, g_n_blit, g_n_pso, g_n_cb;
static _Atomic uint64_t g_sk_prog, g_sk_pass, g_sk_attr, g_sk_pso;   // draws dropped in draw_setup, by reason

// ---------- Shader table (<DM_MSL_DIR>/gl/gl_shaders.tsv) ----------
enum { T_FLOAT, T_VEC2, T_VEC3, T_VEC4, T_INT, T_IVEC2, T_IVEC3, T_IVEC4, T_UINT, T_UVEC2, T_UVEC3, T_UVEC4, T_BOOL, T_MAT2, T_MAT3, T_MAT4,
       T_SAMP2D, T_SAMPCUBE, T_SAMP3D, T_SAMP2DARR, T_SAMP2DSH, T_SAMPBUF, T_USAMP2D, T_OTHER };
static const char *const g_tnames[] = {"float", "vec2", "vec3", "vec4", "int", "ivec2", "ivec3", "ivec4", "uint", "uvec2", "uvec3", "uvec4", "bool", "mat2", "mat3", "mat4",
                                       "sampler2D", "samplerCube", "sampler3D", "sampler2DArray", "sampler2DShadow", "samplerBuffer", "usampler2D"};
static const GLenum g_tenums[] = {GL_FLOAT, GL_FLOAT_VEC2, GL_FLOAT_VEC3, GL_FLOAT_VEC4, GL_INT, GL_INT_VEC2, GL_INT_VEC3, GL_INT_VEC4, GL_UNSIGNED_INT, GL_UNSIGNED_INT_VEC2,
                                  GL_UNSIGNED_INT_VEC3, GL_UNSIGNED_INT_VEC4, GL_BOOL, GL_FLOAT_MAT2, GL_FLOAT_MAT3, GL_FLOAT_MAT4, GL_SAMPLER_2D, GL_SAMPLER_CUBE, GL_SAMPLER_3D,
                                  GL_SAMPLER_2D_ARRAY, GL_SAMPLER_2D_SHADOW, GL_SAMPLER_BUFFER, GL_UNSIGNED_INT_SAMPLER_2D, GL_FLOAT};   // glGetActiveUniform's types
static int tcode(const char *s) { for (int i = 0; i < T_OTHER; i++) if (!strcmp(s, g_tnames[i])) return i; return T_OTHER; }
static int tcomps(int t) { static const int c[] = {1, 2, 3, 4, 1, 2, 3, 4, 1, 2, 3, 4, 1, 4, 9, 16}; return t < T_SAMP2D ? c[t] : 1; }
static int is_samp(int t) { return t >= T_SAMP2D && t < T_OTHER; }
static int is_intt(int t) { return t >= T_INT && t <= T_BOOL; }
typedef struct { char name[48]; uint8_t slot, type; } shattr;                         // a vertex input
typedef struct { char name[48]; uint16_t off, count, stride; uint8_t type; } shuni;   // a default block member (count 0 = not an array)
typedef struct { char name[48]; uint16_t slot; uint8_t type; int sampler; uint8_t unit; } shsamp;   // a combined sampler: texture/sampler argument IDs and optional layout(binding) unit
typedef struct { char name[48]; uint16_t slot; uint32_t size; } shblk;               // a named uniform block: its argument ID and size
typedef struct { char key[17]; int frag, ub_slot, ub_size, arg_slot, nargs, na, nu, nt, nb; shattr *a; shuni *u; shsamp *t; shblk *b; } shinfo;
static shinfo *g_sh; static int g_nsh;
static int sh_cmp(const void *a, const void *b) { int c = memcmp(((const shinfo *)a)->key, ((const shinfo *)b)->key, 16); return c ? c : ((const shinfo *)a)->frag - ((const shinfo *)b)->frag; }
static int sh_read_abi(FILE *f) {
    const char abi[] = "# GL shader ABI 2\n";
    char line[sizeof abi];
    return fgets(line, sizeof line, f) && !strcmp(line, abi);
}
static void sh_free(shinfo *s) { free(s->a); free(s->u); free(s->t); free(s->b); }
static int sh_read_row(char *line, shinfo *s) {
    char *rest = line, *key = strsep(&rest, "\t"), *stage = strsep(&rest, "\t"), *res = strsep(&rest, "\t\n");
    if (!key || !stage || strlen(key) != 16 || (strcmp(stage, "vert") && strcmp(stage, "frag"))) return 0;
    *s = (shinfo){.frag = !strcmp(stage, "frag"), .ub_slot = -1, .arg_slot = -1, .nargs = -1};
    memcpy(s->key, key, 16);
    if (!res || sscanf(res, "r:%d:%d", &s->arg_slot, &s->nargs) != 2 || s->nargs < 0 || s->nargs > 512 || (s->nargs ? s->arg_slot != 0 : s->arg_slot != -1)) {
        GLOG("[RED] shader %s: invalid argument buffer in the GL shader table", s->key);
        return 0;
    }
    int na = 0, nu = 0, nt = 0, nb = 0;
    for (const char *c = res; *c; c++) if (c == res || c[-1] == ' ') { na += *c == 'a'; nu += *c == 'u'; nt += *c == 't'; nb += *c == 'n'; }
    s->a = zalloc((na + 1) * sizeof *s->a); s->u = zalloc((nu + 1) * sizeof *s->u); s->t = zalloc((nt + 1) * sizeof *s->t); s->b = zalloc((nb + 1) * sizeof *s->b);
    int bad = 0;
    char *save;
    for (char *t = strtok_r(res, " ", &save); t; t = strtok_r(NULL, " ", &save)) {
        char nm[64], ty[32]; unsigned a1, a2, a3; int sa;
        if (t[0] == 'a' && (unsigned)s->na < (unsigned)na && sscanf(t, "a:%63[^=]=%u:%31s", nm, &a1, ty) == 3 && a1 < 16) {
            shattr *x = &s->a[s->na++]; strlcpy(x->name, nm, sizeof x->name); x->slot = a1; x->type = tcode(ty);
        } else if (t[0] == 'u' && (unsigned)s->nu < (unsigned)nu && sscanf(t, "u:%63[^=]=%u:%31[^:]:%u:%u", nm, &a1, ty, &a2, &a3) == 5) {
            shuni *x = &s->u[s->nu++]; strlcpy(x->name, nm, sizeof x->name); x->off = a1; x->type = tcode(ty); x->count = a2; x->stride = a3;
        } else if (t[0] == 't' && (unsigned)s->nt < (unsigned)nt) {
            unsigned unit = 0; int end4 = 0, end5 = 0;
            int n = sscanf(t, "t:%63[^=]=%u:%d:%31[^:]%n:%u%n", nm, &a1, &sa, ty, &end4, &unit, &end5);
            if ((n == 4 && !t[end4]) || (n == 5 && !t[end5])) {
                if (a1 >= (unsigned)s->nargs || sa < -1 || sa >= s->nargs || unit >= NUNITS) { bad = 1; continue; }
                shsamp *x = &s->t[s->nt++]; strlcpy(x->name, nm, sizeof x->name); x->slot = a1; x->sampler = sa; x->type = tcode(ty); x->unit = (uint8_t)unit;
            }
        } else if (t[0] == 'b' && sscanf(t, "b:%u:%u", &a1, &a2) == 2) {
            if (a1 >= (unsigned)s->nargs) { bad = 1; continue; } s->ub_slot = a1; s->ub_size = a2;
        } else if (t[0] == 'n' && (unsigned)s->nb < (unsigned)nb && sscanf(t, "n:%63[^=]=%u:%u", nm, &a1, &a2) == 3) {
            if (a1 >= (unsigned)s->nargs) { bad = 1; continue; }
            shblk *x = &s->b[s->nb++]; strlcpy(x->name, nm, sizeof x->name); x->slot = a1; x->size = a2;
        }
    }
    if (bad) { GLOG("[RED] shader %s: invalid argument IDs in the GL shader table", s->key); sh_free(s); }
    return !bad;
}
static id<MTLLibrary> sh_library(const char *path) {
    NSError *err = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:&err];
    if (!lib) GLOG("[RED] shader library failed to load %s: %s", path, err.localizedDescription.UTF8String);
    return lib;
}
static id<MTLFunction> sh_function(id<MTLLibrary> lib, const shinfo *s) {
    return [lib newFunctionWithName:[NSString stringWithFormat:@"gl_%s_%c", s->key, s->frag ? 'f' : 'v']];
}
static void sh_load(void) {
    const char *d = getenv("DM_MSL_DIR"); char p[1024];
    if (!d) { GLOG("[RED] DM_MSL_DIR not set: the GL shader library cannot be found"); return; }
    snprintf(p, sizeof p, "%s/gl/gl_shaders.tsv", d);
    FILE *f = fopen(p, "r");
    if (!f) {
        if (!g_capture_compiler || errno != ENOENT) GLOG("[RED] cannot open %s (run dm/gfx/tools/gl_shaders.py first)", p);
        return;
    }
    static char line[65536]; int cap = 0;
    if (!sh_read_abi(f)) {
        GLOG("[RED] GL shader table has an incompatible ABI; rebuild with dm/gfx/tools/gl_shaders.py"); fclose(f); return;
    }
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        shinfo s; if (!sh_read_row(line, &s)) continue;
        if (g_nsh == cap) { cap = cap ? cap * 2 : 1024; g_sh = realloc(g_sh, cap * sizeof *g_sh); }
        g_sh[g_nsh++] = s;
    }
    fclose(f);
    qsort(g_sh, g_nsh, sizeof *g_sh, sh_cmp);
    snprintf(p, sizeof p, "%s/gl/gl_shaders.metallib", d);
    g_lib = sh_library(p);
}
static const shinfo *sh_find(const char *key, int frag) { shinfo k; memcpy(k.key, key, 16); k.frag = frag; return g_nsh ? bsearch(&k, g_sh, g_nsh, sizeof *g_sh, sh_cmp) : NULL; }


static void dev_init_once(void) {
    @autoreleasepool {
        g_log = getenv("DM_GL_LOG") && atoi(getenv("DM_GL_LOG"));
        g_capture_dir = getenv("DM_GL_CAPTURE");
#if TARGET_OS_OSX
        if (g_capture_dir) g_capture_compiler = getenv("DM_GL_CAPTURE_COMPILER");
#endif
        if (g_capture_dir) GLOG("shader capture → %s", g_capture_dir);
        g_dev = (__bridge id<MTLDevice>)mtl_device_classic();
        g_q = [g_dev newCommandQueueWithMaxCommandBufferCount:64]; g_q.label = @"gl";
        g_ev = [g_dev newSharedEvent];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
        const uint32_t black = 0xff000000u, blk6[6] = {black, black, black, black, black, black};
        g_tex0 = [g_dev newTextureWithDescriptor:td];
        [g_tex0 replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&black bytesPerRow:4];
        td = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm size:1 mipmapped:NO];
        g_cube0 = [g_dev newTextureWithDescriptor:td];
        for (int f = 0; f < 6; f++) [g_cube0 replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:f withBytes:&blk6[f] bytesPerRow:4 bytesPerImage:4];
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
        td.textureType = MTLTextureType3D; g_tex3d0 = [g_dev newTextureWithDescriptor:td];
        [g_tex3d0 replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:0 withBytes:&black bytesPerRow:4 bytesPerImage:4];
        td.textureType = MTLTextureType2DArray; g_arr0 = [g_dev newTextureWithDescriptor:td];
        [g_arr0 replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:0 withBytes:&black bytesPerRow:4 bytesPerImage:0];
        td.textureType = MTLTextureType2D; td.pixelFormat = MTLPixelFormatRGBA32Uint;
        g_uint0 = [g_dev newTextureWithDescriptor:td];
        const uint32_t black_uint[] = {0, 0, 0, 1};
        [g_uint0 replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:black_uint bytesPerRow:sizeof black_uint];
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.supportArgumentBuffers = YES;
        g_samp0 = [g_dev newSamplerStateWithDescriptor:sd];
        NSError *err = nil;
        g_util = host_msl_lib(g_dev, nil, "gl_util", &err);   // dm/gfx/msl/gl_util.metal: a quad that clears what a scissored or masked glClear covers (a render pass can only clear its attachments whole)
        if (!g_util) GLOG("[RED] the layer's own shaders do not compile: %s", err.localizedDescription.UTF8String);
        sh_load();
        mtl_set_origin_lower_left(1);   // the window image is stored bottom row first; the present flips it
        GLOG("Metal device %s; %d shader table entries", g_dev.name.UTF8String, g_nsh);
    }
}
static void dev_init(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, dev_init_once); }

// ---------- Command buffers ----------
// Transient memory (converted indices, staging for blits, large uniform blocks): bump allocation in 4 MB shared buffers, each reused once the command buffer that last used it completed
typedef struct { __strong id<MTLBuffer> buf; size_t used; uint64_t serial; } arena_t;
static arena_t g_arena[64]; static int g_narena, g_cur_arena = -1;
static id<MTLBuffer> arena_alloc(size_t n, size_t *off) {
    n = (n + 255) & ~(size_t)255;
    if (g_cur_arena >= 0) { arena_t *a = &g_arena[g_cur_arena]; if (a->used + n <= a->buf.length) { *off = a->used; a->used += n; a->serial = g_serial; return a->buf; } }
    size_t sz = umax(4u << 20, (uint32_t)n);
    for (int i = 0; i < g_narena; i++) {
        arena_t *a = &g_arena[i];
        if (i != g_cur_arena && a->serial <= atomic_load(&g_done) && a->buf.length >= sz) { g_cur_arena = i; a->used = n; a->serial = g_serial; *off = 0; return a->buf; }
    }
    int k = -1;
    if (g_narena < 64) k = g_narena++;
    else for (int i = 0; i < 64; i++) if (g_arena[i].serial <= atomic_load(&g_done)) { k = i; break; }
    if (k < 0) { RED1("transient memory: all 64 blocks in flight"); k = (g_cur_arena + 1) % 64; }
    g_arena[k].buf = [g_dev newBufferWithLength:sz options:MTLResourceStorageModeShared]; g_arena[k].used = n; g_arena[k].serial = g_serial;
    g_cur_arena = k; *off = 0; return g_arena[k].buf;
}
// The last command buffer committed (frame or upload): the GPU runs them in order, so waiting for it waits for all (dm_gl_drain, when the app goes to the background)
static id<MTLCommandBuffer> g_last_cb; static pthread_mutex_t g_last_mu = PTHREAD_MUTEX_INITIALIZER;
static void cb_commit_one(id<MTLCommandBuffer> cb) {
    if (cb == g_cb && g_cb_samplers.count) {
        NSSet *samplers = g_cb_samplers; g_cb_samplers = nil;
        [cb addCompletedHandler:^(id<MTLCommandBuffer> done) { (void)done; (void)samplers.count; }];
    }
    pthread_mutex_lock(&g_last_mu); [cb commit]; g_last_cb = cb; pthread_mutex_unlock(&g_last_mu); g_n_cb++;
}
void dm_gl_drain(void) {   // dm_gfx_suspend (hle_nvn.c)
    pthread_mutex_lock(&g_last_mu); id<MTLCommandBuffer> cb = g_last_cb; pthread_mutex_unlock(&g_last_mu);
    [cb waitUntilCompleted];
}
static id<MTLCommandBuffer> cb_get(void) {
    if (!g_cb) { g_cb = [g_q commandBuffer]; g_cb.label = @"gl frame"; }
    return g_cb;
}
static void pass_end(void);
// Commit the frame's command buffer: the event and g_done reach its serial when it completes
static uint64_t cb_commit(void) {
    pass_end();
    id<MTLCommandBuffer> cb = cb_get();
    uint64_t s = g_serial++;
    [cb encodeSignalEvent:g_ev value:s];
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
        if (c.status == MTLCommandBufferStatusError) { static int n; if (n++ < 10) GLOG("[RED] command buffer %llu failed: %s", (unsigned long long)s, c.error.localizedDescription.UTF8String); }
        uint64_t d = atomic_load(&g_done); while (d < s && !atomic_compare_exchange_weak(&g_done, &d, s)) {}
    }];
    cb_commit_one(cb);
    g_cb = nil;
    return s;
}

// ---------- Objects ----------
// GL names index plain tables; a name is handed out by glGen* / glCreate* and its slot freed by glDelete*
#define NAMES(T, tab, n, cap) static T **tab; static uint32_t n, cap;
static uint32_t name_new(void ***tab, uint32_t *n, uint32_t *cap, size_t sz) {
    for (uint32_t i = 1; i < *n; i++) if (!(*tab)[i]) { (*tab)[i] = zalloc(sz); return i; }
    if (*n == 0) *n = 1;
    if (*n >= *cap) { *cap = *cap ? *cap * 2 : 256; *tab = realloc(*tab, *cap * sizeof **tab); memset(*tab + *n, 0, (*cap - *n) * sizeof **tab); }
    (*tab)[*n] = zalloc(sz); return (*n)++;
}
static void *name_at(void ***tab, uint32_t *n, uint32_t *cap, uint32_t k, size_t sz) {   // the object of name k, made if it is not there
    if (k >= *cap) { uint32_t c = *cap ? *cap : 256; while (c <= k) c *= 2; *tab = realloc(*tab, c * sizeof **tab); memset(*tab + *cap, 0, (c - *cap) * sizeof **tab); *cap = c; }
    if (*n <= k) *n = k + 1;
    if (!(*tab)[k]) (*tab)[k] = zalloc(sz);
    return (*tab)[k];
}

// use: serial of the last command buffer that read it; immutable: made by glBufferStorage (LÖVE's persistently mapped streams), never orphaned, the game keeps a pointer into it
typedef struct { __strong id<MTLBuffer> mb; size_t size; uint64_t use; int immutable, deleted; unsigned texrefs; } buf_t;
NAMES(buf_t, g_bufs, g_nbufs, g_capbufs)
static buf_t *buf_of(GLuint n) { return n && n < g_nbufs ? g_bufs[n] : NULL; }

// The sampling parameters GL keeps both in a texture and in a sampler object (GL 3.3): filters, wraps, the LOD range and bias, the depth comparison, anisotropy and border colour.
// A sampler object bound to a unit replaces all of them for the texture that unit samples; the texture's own levels (GL_TEXTURE_MAX_LEVEL, the levels it has) still bound the LOD.
// cmode: GL_TEXTURE_COMPARE_MODE on, cfunc: GL_TEXTURE_COMPARE_FUNC (kept apart, so a function set before the mode is not lost); lodbias: GL_TEXTURE_LOD_BIAS
typedef struct { GLint minf, magf, ws, wt, wr, cmode, cfunc; float aniso, lodbias, minlod, maxlod, border[4]; } samp_p;
static const samp_p SAMP_P0 = {.minf = GL_NEAREST_MIPMAP_LINEAR, .magf = GL_LINEAR, .ws = GL_REPEAT, .wt = GL_REPEAT, .wr = GL_REPEAT, .cfunc = 0x203 /* LEQUAL */,
                               .aniso = 1, .minlod = -1000, .maxlod = 1000};
// A texture: target, format, levels given so far (pend[face][level], until the Metal texture is made), its sampling parameters. d: depth (3D) or layers (2D array);
// slevels: the level count glTexStorage gave (LÖVE allocates every level up front); swz: GL_TEXTURE_SWIZZLE_R..A (LÖVE's LA8 is RG8 read as R, R, R, G in a core profile)
#define MAXLV 16
typedef struct { void *data; size_t pitch, size; uint32_t w, h; } level_t;
typedef struct {
    GLenum target; int made, fmt, levels, slevels; uint32_t w, h, d; uint64_t use, gen;
    level_t pend[6][MAXLV]; int defined;   // defined: bit per level whose face(s) were given
    __strong id<MTLTexture> tex, linear_rt; __strong id<MTLSamplerState> smp;   // smp: the Metal sampler of its own parameters, made again when smp_dirty
    samp_p sp; GLint maxlv, swz[4]; int smp_dirty;
    GLint ifmt;   // GL base image format, including RGB images expanded to RGBA in Metal
    buf_t *buffer;   // for GL_TEXTURE_BUFFER, the object survives until the texture is detached
} tex_t;
NAMES(tex_t, g_texs, g_ntexs, g_captexs)
static tex_t *tex_of(GLuint n) { return n && n < g_ntexs ? g_texs[n] : NULL; }
// A sampler object (glGenSamplers): its parameters only; glBindSampler puts it on a unit (Mega Man Legacy Collection sets its filters and wraps on one per unit, none on its textures)
typedef struct { samp_p sp; } so_t;
NAMES(so_t, g_sos, g_nsos, g_capsos)
static so_t *so_of(GLuint n) { return n && n < g_nsos ? g_sos[n] : NULL; }

typedef struct { int fmt; uint32_t w, h; __strong id<MTLTexture> tex, linear_rt; uint64_t gen; } rb_t;
NAMES(rb_t, g_rbs, g_nrbs, g_caprbs)
static rb_t *rb_of(GLuint n) { return n && n < g_nrbs ? g_rbs[n] : NULL; }

typedef struct { int kind; GLuint name; int level, face; } att_t;   // kind 0 none, 1 texture, 2 renderbuffer; face: a cube face, an array layer or a 3D slice
// The draw table maps output locations to attachments (+1, with 0 for GL_NONE).
typedef struct { att_t col[NGL_COLORS], depth, stencil; uint64_t gen; uint8_t draw[NGL_COLORS], read; } fb_t;
NAMES(fb_t, g_fbs, g_nfbs, g_capfbs)
static fb_t *fb_of(GLuint n) { return n && n < g_nfbs ? g_fbs[n] : NULL; }

// A vertex array as GL 4.3 keeps it: 16 attributes and the 16 buffer bindings they read (glVertexAttribFormat, glVertexAttribBinding, glBindVertexBuffer).
// vattr_t: on (the array enabled), the format (size, type, norm), rel: the attribute's offset in its binding's element, bind: its binding + 1, 0 = its own
//   (attribute i reads binding i, GL's default).
// vbind_t: buf and off: the buffer and where its first element starts; client: no buffer, off points into the game's memory (glVertexAttribPointer with no
//   array buffer bound). stride: bytes between elements, 0 = every vertex reads the first (glBindVertexBuffer's 0 is literal); div: an element per div
//   instances, 0 = per vertex.
// glVertexAttribPointer is GL's shorthand for a format, binding i and that binding's buffer, offset and stride; glVertexAttribDivisor for binding i and its
// divisor. A draw reads an attribute through its binding (vb_of), so every attribute on a binding follows a change to it.
typedef struct { uint8_t on, size, norm, bind; GLenum type; uint32_t rel; } vattr_t;
typedef struct { GLuint buf; GLsizei stride; uint8_t div, client; uint64_t off; } vbind_t;
typedef struct { vattr_t a[16]; vbind_t b[16]; GLuint ebo; } vao_t;
static vbind_t *vb_of(vao_t *v, int i) { int b = v->a[i].bind; return &v->b[b ? b - 1 : i]; }
NAMES(vao_t, g_vaos, g_nvaos, g_capvaos)
static vao_t g_vao0;   // the default vertex array (GL core has none, the compatibility profile does)

// refs: the programs it is attached to; deleted: glDeleteShader flagged it while attached (it keeps its name and goes with its last attachment, as in GL)
typedef struct { GLenum type; char *src; size_t len; char key[17]; const shinfo *si; __strong id<MTLFunction> fn; int compiled, refs, deleted; } shader_t;
typedef struct { char name[48]; uint8_t type; uint16_t count; int32_t off[2]; uint16_t stride[2]; int slot[2], sampler[2]; int unit; } puni_t;   // [0] vertex, [1] fragment
typedef struct {
    uint32_t vf[16]; uint16_t stride[16]; uint8_t div[16];
    uint32_t cpf[NGL_COLORS], dpf, spf;
    uint8_t mask[NGL_COLORS], blend, bsrc, bdst, bsa, bda, eqc, eqa, reserved;
} pkey_t;
typedef struct { pkey_t k; void *pso; } pent_t;   // pso: a retained id<MTLRenderPipelineState>, NULL = failed
// amap: the game's attribute index of each Metal vertex input (glBindAttribLocation before the link, else the input's own number, which glGetAttribLocation returns);
// LÖVE binds VertexPosition, VertexTexCoord, VertexColor and ConstantColor to 0–3 in every program
typedef struct { char name[48]; int index; } abind_t;
// A named uniform block of a program: its slot in each stage (−1 = not in that stage), its size, and the binding point glUniformBlockBinding gave it (0 by default)
typedef struct { char name[48]; int slot[2]; uint32_t size, binding; } pblk_t;
// vs, fs: the attached shaders, each counted in its refs; deleted: glDeleteProgram flagged it while current (it goes at the next glUseProgram of another);
// what the link took from them (si, fn, the uniform table) is the program's own, so attaching, detaching or deleting a shader later leaves the linked program as it is
typedef struct {
    GLuint vs, fs; int linked, deleted, validated; char validation_log[160]; const shinfo *si[2]; __strong id<MTLFunction> fn[2];
    int nu; puni_t *u; uint8_t *ub[2]; uint64_t ub_gen[2]; uint32_t serial;
    __strong id<MTLArgumentEncoder> ae[2]; __strong id<MTLBuffer> ub_buf[2]; size_t ub_off[2]; uint64_t ub_serial[2], ub_written_gen[2];
    pent_t *pc; int npc, cappc;
    abind_t bind[16]; int nbind; int8_t amap[16];
    pblk_t blk[8]; int nblk;
} prog_t;
// Shaders and programs share one name space, as in GL
typedef struct { int is_prog; union { shader_t s; prog_t p; }; } sp_t;
NAMES(sp_t, g_sps, g_nsps, g_capsps)
static shader_t *shader_of(GLuint n) { return n && n < g_nsps && g_sps[n] && !g_sps[n]->is_prog ? &g_sps[n]->s : NULL; }
static prog_t *prog_of(GLuint n) { return n && n < g_nsps && g_sps[n] && g_sps[n]->is_prog ? &g_sps[n]->p : NULL; }
// GL's error flag: the first error since the last glGetError, which returns it and clears it; another error is not recorded while one is (GL lets an implementation keep
// one flag). Recorded only where GL requires an error and the host does what GL does: a call or a value the host does not implement says so in a red line instead and
// records nothing. A link the offline library cannot serve still reports success and its draws are skipped. The capture compiler reports the actual compile and link outcomes.
// The first 8 errors are logged with the call that recorded them (a game's misuse, not a host fault: no red line)
static GLenum g_gl_error;
static void gl_error_at(GLenum e, const char *fn) {
    if (!g_gl_error) g_gl_error = e;
    static int n; if (n < 8) { n++; GLOG("GL error %#x recorded by %s (glGetError returns it once)%s", e, fn, n == 8 ? "; no more of these lines" : ""); }
}
#define gl_error(e) gl_error_at(e, __func__)
// A shader or program name, as a call that takes one checks it: 1 for an object of that kind; else GL_INVALID_VALUE (no such object) or GL_INVALID_OPERATION (the other kind)
static int sp_check(GLuint n, int prog, const char *fn) {
    if (!n || n >= g_nsps || !g_sps[n]) { gl_error_at(GL_INVALID_VALUE, fn); return 0; }
    if (!g_sps[n]->is_prog != !prog) { gl_error_at(GL_INVALID_OPERATION, fn); return 0; }
    return 1;
}
#define SP_CHECK(n, prog) sp_check(n, prog, __func__)

// ---------- Context state ----------
// units: as many as GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS says (LÖVE binds a texture of each type to every one at start); fb / read_fb: the draw and read framebuffers;
// va: each attribute's current value (glVertexAttrib4f), what a vertex input reads when its array is disabled: LÖVE's VertexColor and ConstantColor
// A face's stencil state: GL keeps one for front faces and one for back faces (glStencilFuncSeparate, glStencilOpSeparate, glStencilMaskSeparate); glStencilFunc / Op / Mask set both
typedef struct { GLenum func, fail, zfail, pass; GLint ref; GLuint mask, wmask; } stface_t;
#define STFACE0 {0x207 /* ALWAYS */, GL_KEEP, GL_KEEP, GL_KEEP, 0, 0xffffffff, 0xffffffff}
#define NUBO 72   // uniform buffer binding points, GL 4.5's minimum GL_MAX_UNIFORM_BUFFER_BINDINGS
static struct {
    GLuint array_buf, uniform_buf, texture_buf, rb, fb, read_fb, prog, vao; int unit;
    struct { GLuint buf; GLintptr off; GLsizeiptr size; } ubo[NUBO];   // glBindBufferRange / Base (size 0: the whole buffer)
    struct { GLuint t2d, tcube, t3d, t2da, tbuf, samp; } units[NUNITS];   // samp: the sampler object bound to the unit (glBindSampler), 0 for the texture's own parameters
    float clear_c[4], blend_c[4]; double clear_d; int clear_s;
    uint8_t cmask[NGL_COLORS], default_draw, default_read;
    uint8_t dmask, depth, blend, cull, scissor, stencil, poffset, poffset_line, lines, srgb;   // lines: glPolygonMode GL_LINE
    GLenum dfunc, cull_face, front, bsrc, bdst, bsa, bda, eqc, eqa;
    uint8_t sample_mask_on; GLbitfield sample_mask;
    stface_t st[2];   // [0] front faces, [1] back faces
    GLint vp[4], sc[4]; float pfactor, punits, psize;
    float va[16][4];
    int unpack_align, unpack_row, pack_align, unpack_swap;
    uint32_t palette[256]; int npalette, color_table, index_shift, index_offset;
    int swap_interval;
} S = {.cmask = {0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf, 0xf}, .default_draw = 1, .default_read = 1,
       .dmask = 1, .dfunc = 0x201, .cull_face = GL_BACK, .front = GL_CCW, .bsrc = 1, .bdst = 0, .bsa = 1, .bda = 0, .eqc = GL_FUNC_ADD, .eqa = GL_FUNC_ADD,
       .st = {STFACE0, STFACE0}, .clear_d = 1, .unpack_align = 4, .pack_align = 4,
       .swap_interval = 1, .psize = 1, .sample_mask = UINT32_MAX,
       .va = {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1},
              {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}}};
static vao_t *cur_vao(void) { return S.vao && S.vao < g_nvaos && g_vaos[S.vao] ? g_vaos[S.vao] : &g_vao0; }
static GLuint *unit_slot(int unit, GLenum target) {   // a unit's binding of a texture target (a cube face names the cube); NULL for a target this layer does not have
    if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) target = GL_TEXTURE_CUBE_MAP;
    if (unit < 0 || unit >= NUNITS) return NULL;
    switch (target) {
        case GL_TEXTURE_2D: return &S.units[unit].t2d; case GL_TEXTURE_CUBE_MAP: return &S.units[unit].tcube;
        case GL_TEXTURE_3D: return &S.units[unit].t3d; case GL_TEXTURE_2D_ARRAY: return &S.units[unit].t2da;
        case GL_TEXTURE_BUFFER: return &S.units[unit].tbuf;
    }
    return NULL;
}
static tex_t *bound_tex(GLenum target) { GLuint *s = unit_slot(S.unit, target); return s ? tex_of(*s) : NULL; }

// ---------- The window surface ----------
#define NWIN 3
enum { WIN_COLOR_BITS = 8, WIN_DEPTH_BITS = 24, WIN_STENCIL_BITS = 8 };
static struct { int made; uint32_t w, h; __strong id<MTLTexture> col[NWIN], srgb_rt[NWIN], ds; uint64_t serial[NWIN], ticket[NWIN]; int cur; } g_win;
static void win_make(uint32_t w, uint32_t h) {
    @autoreleasepool {
        for (int i = 0; i < NWIN; i++) if (g_win.ticket[i]) mtl_present_wait(g_win.ticket[i]);
        for (int i = 0; i < NWIN; i++) if (g_win.serial[i]) mtl_ext_tex_release(g_win.serial[i]);   // the surface it replaces (another size): let go once its presents complete
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
        for (int i = 0; i < NWIN; i++) {
            g_win.srgb_rt[i] = nil;
            g_win.col[i] = [g_dev newTextureWithDescriptor:td];
            g_win.serial[i] = mtl_ext_tex((__bridge void *)g_win.col[i]); g_win.ticket[i] = 0;
        }
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
        g_win.ds = [g_dev newTextureWithDescriptor:td];
        g_win.w = w; g_win.h = h; g_win.cur = 0; g_win.made = 1;
    }
    GLOG("window surface %ux%u (RGBA8, depth 32F + stencil 8, %d images)", w, h, NWIN);
}
// The Switch's desktop GL window can encode sRGB when GL_FRAMEBUFFER_SRGB is enabled.
// Present and read back through the raw view so the stored display values are used directly.
static id<MTLTexture> win_color_attachment(void) {
    int i = g_win.cur;
    if (!S.srgb) return g_win.col[i];
    if (!g_win.srgb_rt[i]) {
        g_win.srgb_rt[i] = [g_win.col[i] newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB];
        if (!g_win.srgb_rt[i]) die("[gl] cannot make the sRGB view of the window colour attachment");
    }
    return g_win.srgb_rt[i];
}

// ---------- Formats ----------
// fmt: index into g_fmt. A swizzle only where Metal's default differs from GL's: Metal reads a channel a format lacks as 0 (alpha 1), as GL does, so only the luminance /
// alpha formats and DXT1 without alpha have one (Metal refuses a swizzle on a render target, and GL cannot render into those). Conversions on upload: CV_COPY (the bytes as given), CV_RGB8 (RGB → RGBA), CV_EXPAND_RG (RED/RG → RGBA), CV_565 / CV_4444 / CV_5551 (16-bit packed → RGBA8), CV_INDEX (8-bit colour
// indices through the glColorTable palette). bw / bh: block size of a compressed format, bytes: per texel or per block
enum { CV_COPY, CV_RGB8, CV_565, CV_4444, CV_5551, CV_INDEX, CV_NONE, CV_EXPAND_RG };
enum { K_COL, K_DEPTH, K_DS, K_BLOCK };
typedef struct { MTLPixelFormat pf; uint8_t bytes, bw, bh, kind; MTLTextureSwizzleChannels swz; const char *name; } fmt_t;
#define SWZ(r, g, b, a) {MTLTextureSwizzle##r, MTLTextureSwizzle##g, MTLTextureSwizzle##b, MTLTextureSwizzle##a}
#define SW0 SWZ(Red, Green, Blue, Alpha)
enum { F_RGBA8, F_BGRA8, F_SRGBA8, F_R8, F_RG8, F_L8, F_A8, F_LA8, F_RGBA16F, F_RGBA32F, F_R16F, F_RG16F, F_R32F, F_RG32F, F_RG11B10F, F_RGB10A2, F_RGBA16,
       F_D16, F_D32F, F_D32FS8, F_S8, F_BC1_RGB, F_BC1, F_BC2, F_BC3, F_BC1_SRGB, F_BC2_SRGB, F_BC3_SRGB, F_BC4, F_BC4S, F_BC5, F_BC5S, F_BC7, F_BC7_SRGB, F_BC6S, F_BC6U,
       F_ASTC4, F_ASTC4_SRGB, F_R16UI, F_RG32UI, F_RGBA32UI, F_N };
static const fmt_t g_fmt[F_N] = {
    [F_RGBA8] = {MTLPixelFormatRGBA8Unorm, 4, 1, 1, K_COL, SW0, "RGBA8"}, [F_BGRA8] = {MTLPixelFormatBGRA8Unorm, 4, 1, 1, K_COL, SW0, "BGRA8"},
    [F_SRGBA8] = {MTLPixelFormatRGBA8Unorm_sRGB, 4, 1, 1, K_COL, SW0, "SRGB8_ALPHA8"},
    [F_R8] = {MTLPixelFormatR8Unorm, 1, 1, 1, K_COL, SW0, "R8"}, [F_RG8] = {MTLPixelFormatRG8Unorm, 2, 1, 1, K_COL, SW0, "RG8"},
    [F_L8] = {MTLPixelFormatR8Unorm, 1, 1, 1, K_COL, SWZ(Red, Red, Red, One), "LUMINANCE"}, [F_A8] = {MTLPixelFormatR8Unorm, 1, 1, 1, K_COL, SWZ(Zero, Zero, Zero, Red), "ALPHA"},
    [F_LA8] = {MTLPixelFormatRG8Unorm, 2, 1, 1, K_COL, SWZ(Red, Red, Red, Green), "LUMINANCE_ALPHA"},
    [F_RGBA16F] = {MTLPixelFormatRGBA16Float, 8, 1, 1, K_COL, SW0, "RGBA16F"}, [F_RGBA32F] = {MTLPixelFormatRGBA32Float, 16, 1, 1, K_COL, SW0, "RGBA32F"},
    [F_R16F] = {MTLPixelFormatR16Float, 2, 1, 1, K_COL, SW0, "R16F"}, [F_RG16F] = {MTLPixelFormatRG16Float, 4, 1, 1, K_COL, SW0, "RG16F"},
    [F_R32F] = {MTLPixelFormatR32Float, 4, 1, 1, K_COL, SW0, "R32F"}, [F_RG32F] = {MTLPixelFormatRG32Float, 8, 1, 1, K_COL, SW0, "RG32F"},
    [F_RG11B10F] = {MTLPixelFormatRG11B10Float, 4, 1, 1, K_COL, SW0, "R11F_G11F_B10F"},
    [F_RGB10A2] = {MTLPixelFormatRGB10A2Unorm, 4, 1, 1, K_COL, SW0, "RGB10_A2"}, [F_RGBA16] = {MTLPixelFormatRGBA16Unorm, 8, 1, 1, K_COL, SW0, "RGBA16"},
    [F_D16] = {MTLPixelFormatDepth16Unorm, 2, 1, 1, K_DEPTH, SW0, "DEPTH16"}, [F_D32F] = {MTLPixelFormatDepth32Float, 4, 1, 1, K_DEPTH, SW0, "DEPTH24/32"},
    [F_D32FS8] = {MTLPixelFormatDepth32Float_Stencil8, 8, 1, 1, K_DS, SW0, "DEPTH24_STENCIL8"}, [F_S8] = {MTLPixelFormatStencil8, 1, 1, 1, K_DS, SW0, "STENCIL8"},
    [F_BC1_RGB] = {MTLPixelFormatBC1_RGBA, 8, 4, 4, K_BLOCK, SWZ(Red, Green, Blue, One), "DXT1 RGB"}, [F_BC1] = {MTLPixelFormatBC1_RGBA, 8, 4, 4, K_BLOCK, SW0, "DXT1"},
    [F_BC2] = {MTLPixelFormatBC2_RGBA, 16, 4, 4, K_BLOCK, SW0, "DXT3"}, [F_BC3] = {MTLPixelFormatBC3_RGBA, 16, 4, 4, K_BLOCK, SW0, "DXT5"},
    [F_BC1_SRGB] = {MTLPixelFormatBC1_RGBA_sRGB, 8, 4, 4, K_BLOCK, SW0, "DXT1 sRGB"}, [F_BC2_SRGB] = {MTLPixelFormatBC2_RGBA_sRGB, 16, 4, 4, K_BLOCK, SW0, "DXT3 sRGB"},
    [F_BC3_SRGB] = {MTLPixelFormatBC3_RGBA_sRGB, 16, 4, 4, K_BLOCK, SW0, "DXT5 sRGB"},
    [F_BC4] = {MTLPixelFormatBC4_RUnorm, 8, 4, 4, K_BLOCK, SW0, "RGTC1"}, [F_BC4S] = {MTLPixelFormatBC4_RSnorm, 8, 4, 4, K_BLOCK, SW0, "RGTC1 signed"},
    [F_BC5] = {MTLPixelFormatBC5_RGUnorm, 16, 4, 4, K_BLOCK, SW0, "RGTC2"}, [F_BC5S] = {MTLPixelFormatBC5_RGSnorm, 16, 4, 4, K_BLOCK, SW0, "RGTC2 signed"},
    [F_BC7] = {MTLPixelFormatBC7_RGBAUnorm, 16, 4, 4, K_BLOCK, SW0, "BPTC"}, [F_BC7_SRGB] = {MTLPixelFormatBC7_RGBAUnorm_sRGB, 16, 4, 4, K_BLOCK, SW0, "BPTC sRGB"},
    [F_BC6S] = {MTLPixelFormatBC6H_RGBFloat, 16, 4, 4, K_BLOCK, SW0, "BPTC signed float"},
    [F_BC6U] = {MTLPixelFormatBC6H_RGBUfloat, 16, 4, 4, K_BLOCK, SW0, "BPTC unsigned float"},
    [F_ASTC4] = {MTLPixelFormatASTC_4x4_LDR, 16, 4, 4, K_BLOCK, SW0, "ASTC 4x4"}, [F_ASTC4_SRGB] = {MTLPixelFormatASTC_4x4_sRGB, 16, 4, 4, K_BLOCK, SW0, "ASTC 4x4 sRGB"},
    [F_RG32UI] = {MTLPixelFormatRG32Uint, 8, 1, 1, K_COL, SW0, "RG32UI"}, [F_RGBA32UI] = {MTLPixelFormatRGBA32Uint, 16, 1, 1, K_COL, SW0, "RGBA32UI"},
    [F_R16UI] = {MTLPixelFormatR16Uint, 2, 1, 1, K_COL, SW0, "R16UI"},
};
// internal format (+ format / type for unsized ones) → fmt and the conversion of the given data; -1 when not mapped
static int fmt_pick(GLint ifmt, GLenum format, GLenum type, int *cv) {
    *cv = CV_COPY;
    if (format == GL_COLOR_INDEX) { *cv = type == GL_UNSIGNED_BYTE ? CV_INDEX : CV_NONE; return F_RGBA8; }
    if (type == GL_UNSIGNED_SHORT_5_6_5) { *cv = CV_565; return F_RGBA8; }
    if (type == GL_UNSIGNED_SHORT_4_4_4_4) { *cv = CV_4444; return F_RGBA8; }
    if (type == GL_UNSIGNED_SHORT_5_5_5_1) { *cv = CV_5551; return F_RGBA8; }
    int half = type == GL_HALF_FLOAT, fl = type == GL_FLOAT;
    int rg = format == GL_RED || format == GL_RG;
    switch (ifmt) {
        case GL_RGBA: case GL_RGBA8: case 4:
            if (rg && ifmt == GL_RGBA8) return F_RGBA8;
            if (fl) return F_RGBA32F;
            if (half) return F_RGBA16F;
            if (format == GL_BGRA) return F_BGRA8;
            if (format == GL_RGB) { *cv = CV_RGB8; return F_RGBA8; }
            return F_RGBA8;
        case GL_RGB: case GL_RGB8: case 3:
            if (rg && ifmt == GL_RGB8) return F_RGBA8;
            if (fl || half) { if (!rg) *cv = CV_NONE; return F_RGBA16F; }
            if (format == GL_RGBA || rg) return F_RGBA8;
            *cv = CV_RGB8; return F_RGBA8;
        case GL_BGRA: return F_BGRA8;
        case GL_RGBA4: case GL_RGB5_A1: case GL_RGB565: return F_RGBA8;   // glTexStorage2D's sized formats (no data: the uploads are converted by their type)
        case GL_SRGB8_ALPHA8: return F_SRGBA8;
        case GL_RED: case GL_R8: return fl ? F_R32F : half ? F_R16F : F_R8;
        case GL_RG: case GL_RG8: return fl ? F_RG32F : half ? F_RG16F : F_RG8;
        case GL_LUMINANCE: case 1: return F_L8;
        case GL_ALPHA: return F_A8;
        case GL_LUMINANCE_ALPHA: case 2: return F_LA8;
        case GL_RGBA16F: if (fl && !rg) { *cv = CV_NONE; } return F_RGBA16F;
        case GL_RGBA32F: return F_RGBA32F;
        case GL_R16F: return F_R16F;
        case GL_RG16F: return F_RG16F;
        case GL_R32F: return F_R32F;
        case GL_RG32F: return F_RG32F;
        case GL_R16UI: if (format != GL_RED_INTEGER || type != GL_UNSIGNED_SHORT) *cv = CV_NONE; return F_R16UI;
        case GL_RG32UI: if (format != GL_RG_INTEGER || type != GL_UNSIGNED_INT) *cv = CV_NONE; return F_RG32UI;
        case GL_RGBA32UI: if (format != GL_RGBA_INTEGER || type != GL_UNSIGNED_INT) *cv = CV_NONE; return F_RGBA32UI;
        case GL_R11F_G11F_B10F: if (type != GL_UNSIGNED_INT_10F_11F_11F_REV) *cv = CV_NONE; return F_RG11B10F;
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
        case GL_COMPRESSED_SRGB_S3TC_DXT1: case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1: return F_BC1_SRGB;
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

static GLenum convert_pixel(int f, GLenum format, GLenum type, const void *data, uint8_t *out);

// The given pixels (GL unpack rules: rows of unpack_row or w pixels, each row aligned to unpack_align) → the Metal texture's bytes, rows packed. NULL data → zeros
static void *pixels_convert(const void *src, uint32_t w, uint32_t h, GLenum format, GLenum type, int f, int cv, size_t *pitch_out) {
    size_t dp = fmt_pitch(f, w), rows = fmt_rows(f, h);
    uint8_t *dst = zalloc(dp * rows + 1); *pitch_out = dp;
    if (!src) return dst;
    if (g_fmt[f].kind == K_BLOCK) { memcpy(dst, src, dp * rows); return dst; }   // compressed data has no unpack rows
    static const int comps_of[] = {[GL_RED - GL_RED] = 1, [GL_GREEN - GL_RED] = 1, [GL_BLUE - GL_RED] = 1, [GL_ALPHA - GL_RED] = 1, [GL_RGB - GL_RED] = 3, [GL_RGBA - GL_RED] = 4,
                                   [GL_LUMINANCE - GL_RED] = 1, [GL_LUMINANCE_ALPHA - GL_RED] = 2};
    int comps = format == GL_BGRA || format == GL_RGBA_INTEGER ? 4 : format == GL_RG || format == GL_RG_INTEGER ? 2 :
                format == GL_COLOR_INDEX || format == GL_DEPTH_COMPONENT || format == GL_RED_INTEGER ? 1 :
                format >= GL_RED && format <= GL_LUMINANCE_ALPHA ? comps_of[format - GL_RED] : 4;
    int tbytes = type == GL_FLOAT || type == GL_UNSIGNED_INT || type == GL_INT ? 4 : type == GL_HALF_FLOAT || type == GL_UNSIGNED_SHORT || type == GL_SHORT ? 2 : 1;
    if (cv == CV_565 || cv == CV_4444 || cv == CV_5551) { comps = 1; tbytes = 2; }
    else if (type == GL_UNSIGNED_INT_2_10_10_10_REV || type == GL_UNSIGNED_INT_10F_11F_11F_REV) { comps = 1; tbytes = 4; }
    if (cv == CV_COPY && (format == GL_RED || format == GL_RG) &&
        ((type >= GL_BYTE && type <= GL_FLOAT) || type == GL_HALF_FLOAT) &&
        (f == F_RGBA8 || f == F_SRGBA8 || f == F_RGBA16 || f == F_RGBA16F || f == F_RGBA32F))
        cv = CV_EXPAND_RG;
    size_t sp = (size_t)(S.unpack_row ? S.unpack_row : w) * comps * tbytes; sp = (sp + S.unpack_align - 1) / S.unpack_align * S.unpack_align;
    const uint8_t *source = src;
    // GL_UNPACK_SWAP_BYTES: each 2- or 4-byte element of the source in the other byte order (DOOM 3's light images are RGB565 stored big-endian), swapped into a copy
    // of the rows as given, each row without its alignment padding
    uint8_t *swapped = NULL;
    if (S.unpack_swap && tbytes > 1 && h) {
        size_t row = (size_t)w * comps * tbytes; swapped = malloc(sp * (h - 1) + row);
        for (uint32_t y = 0; y < h; y++) for (size_t i = 0; i < row; i += tbytes) for (int b = 0; b < tbytes; b++) swapped[y * sp + i + b] = source[y * sp + i + tbytes - 1 - b];
        source = swapped;
    }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = source + y * sp;
        uint8_t *d = dst + y * dp;
        switch (cv) {
            case CV_COPY: memcpy(d, s, umin((uint32_t)dp, (uint32_t)((size_t)w * comps * tbytes))); break;
            case CV_RGB8: for (uint32_t x = 0; x < w; x++) { d[4 * x] = s[3 * x]; d[4 * x + 1] = s[3 * x + 1]; d[4 * x + 2] = s[3 * x + 2]; d[4 * x + 3] = 255; } break;
            case CV_EXPAND_RG:
                if (type == GL_UNSIGNED_BYTE && (f == F_RGBA8 || f == F_SRGBA8)) {
                    for (uint32_t x = 0; x < w; x++) {
                        d[4 * x] = s[comps * x];
                        if (comps == 2) d[4 * x + 1] = s[comps * x + 1];
                        d[4 * x + 3] = 255;
                    }
                } else {
                    for (uint32_t x = 0; x < w; x++)
                        convert_pixel(f, format, type, s + (size_t)x * comps * tbytes, d + (size_t)x * g_fmt[f].bytes);
                }
                break;
            case CV_565: for (uint32_t x = 0; x < w; x++) { uint16_t v; memcpy(&v, s + 2 * x, 2); d[4 * x] = (v >> 11) * 255 / 31; d[4 * x + 1] = (v >> 5 & 63) * 255 / 63; d[4 * x + 2] = (v & 31) * 255 / 31; d[4 * x + 3] = 255; } break;
            case CV_4444: for (uint32_t x = 0; x < w; x++) { uint16_t v; memcpy(&v, s + 2 * x, 2); d[4 * x] = (v >> 12) * 17; d[4 * x + 1] = (v >> 8 & 15) * 17; d[4 * x + 2] = (v >> 4 & 15) * 17; d[4 * x + 3] = (v & 15) * 17; } break;
            case CV_5551: for (uint32_t x = 0; x < w; x++) { uint16_t v; memcpy(&v, s + 2 * x, 2); d[4 * x] = (v >> 11) * 255 / 31; d[4 * x + 1] = (v >> 6 & 31) * 255 / 31; d[4 * x + 2] = (v >> 1 & 31) * 255 / 31; d[4 * x + 3] = v & 1 ? 255 : 0; } break;
            case CV_INDEX:
                for (uint32_t x = 0; x < w; x++) {
                    int i = ((int)s[x] << S.index_shift) + S.index_offset; uint32_t c = S.npalette ? S.palette[i < 0 ? 0 : i >= S.npalette ? S.npalette - 1 : i] : 0xff000000u | s[x] * 0x10101u;
                    memcpy(d + 4 * x, &c, 4);
                }
                break;
        }
    }
    free(swapped);
    return dst;
}

// ---------- Textures ----------
static MTLSamplerAddressMode wrap_of(GLint w) {
    switch (w) { case GL_CLAMP_TO_EDGE: case GL_CLAMP: return MTLSamplerAddressModeClampToEdge; case GL_MIRRORED_REPEAT: return MTLSamplerAddressModeMirrorRepeat;
                 case GL_CLAMP_TO_BORDER: return MTLSamplerAddressModeClampToBorderColor; default: return MTLSamplerAddressModeRepeat; }
}
static void tex_init_params(tex_t *t, GLenum target) {
    t->target = target; t->sp = SAMP_P0; t->maxlv = 1000; t->smp_dirty = 1; t->fmt = -1;
    for (int c = 0; c < 4; c++) t->swz[c] = GL_RED + c;
}
static int tex_faces(const tex_t *t) { return t->target == GL_TEXTURE_CUBE_MAP ? 6 : 1; }
// The swizzle a texture is made with: its format's own (only where Metal's reading differs from GL's), then GL_TEXTURE_SWIZZLE_R..A over it
static MTLTextureSwizzleChannels tex_swz(const tex_t *t) {
    MTLTextureSwizzleChannels f = g_fmt[t->fmt].swz, o;
    const MTLTextureSwizzle src[4] = {f.red, f.green, f.blue, f.alpha};
    MTLTextureSwizzle out[4];
    for (int c = 0; c < 4; c++) { GLint p = t->swz[c]; out[c] = p >= GL_RED && p <= GL_ALPHA ? src[p - GL_RED] : p == GL_ONE ? MTLTextureSwizzleOne : MTLTextureSwizzleZero; }
    o.red = out[0]; o.green = out[1]; o.blue = out[2]; o.alpha = out[3];
    return o;
}
// The Metal sampler for parameters p sampling texture t: sampler states are shared, one per distinct set (the texture's level count and target are part of it)
static id<MTLSamplerState> samp_state(const samp_p *p, const tex_t *t) {
    typedef struct { GLint minf, magf, ws, wt, wr, cmp; float lo, hi, aniso, b, bias; } skey;
    static struct { skey k; __strong id<MTLSamplerState> s; } cache[256]; static int n;
    int mips = t->levels > 1;
    float top = mips ? (float)umin((uint32_t)t->maxlv, (uint32_t)t->levels - 1) : 0;
    skey k = {p->minf, p->magf, p->ws, p->wt, t->target == GL_TEXTURE_CUBE_MAP ? GL_CLAMP_TO_EDGE : p->wr, p->cmode ? p->cfunc : 0,
              mips ? fmaxf(0, fminf(top, p->minlod)) : 0, mips ? fmaxf(0, fminf(top, p->maxlod)) : 0, p->aniso,
              p->border[3] > 0.5f ? (p->border[0] > 0.5f ? 2.f : 1.f) : 0.f, fmaxf(-15, fminf(15, p->lodbias))};   // GL clamps the bias to GL_MAX_TEXTURE_LOD_BIAS
    for (int i = 0; i < n; i++) if (!memcmp(&cache[i].k, &k, sizeof k)) return cache[i].s;
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.supportArgumentBuffers = YES;
    d.magFilter = k.magf == GL_NEAREST ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    d.minFilter = k.minf == GL_NEAREST || k.minf == GL_NEAREST_MIPMAP_NEAREST || k.minf == GL_NEAREST_MIPMAP_LINEAR ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    // a texture of one level samples level 0 whatever its minifying filter (GL would call it incomplete with a mipmap filter and sample black)
    d.mipFilter = !mips || k.minf == GL_NEAREST || k.minf == GL_LINEAR ? MTLSamplerMipFilterNotMipmapped :
                  k.minf == GL_NEAREST_MIPMAP_NEAREST || k.minf == GL_LINEAR_MIPMAP_NEAREST ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    d.lodMinClamp = k.lo; d.lodMaxClamp = k.hi;   // GL_TEXTURE_MIN_LOD / MAX_LOD within the levels the texture has
    d.sAddressMode = wrap_of(k.ws); d.tAddressMode = wrap_of(k.wt); d.rAddressMode = wrap_of(k.wr);
    d.borderColor = k.b == 2.f ? MTLSamplerBorderColorOpaqueWhite : k.b == 1.f ? MTLSamplerBorderColorOpaqueBlack : MTLSamplerBorderColorTransparentBlack;
    d.maxAnisotropy = (NSUInteger)fmaxf(1, fminf(16, k.aniso));
    d.lodBias = k.bias;   // added to the level of detail a sample computes, as GL adds the texture's (Metal 4: macOS and iOS 26, the host's minimum)
    if (k.cmp) d.compareFunction = (MTLCompareFunction)(k.cmp - GL_NEVER);
    id<MTLSamplerState> s = [g_dev newSamplerStateWithDescriptor:d];
    if (n < 256) { cache[n].k = k; cache[n].s = s; n++; } else RED1("more than 256 distinct sampler parameter sets");
    return s;
}
// The Metal sampler a draw uses for texture t on a unit: the bound sampler object's parameters when there is one, else the texture's own (kept on the texture until they change)
static id<MTLSamplerState> tex_sampler(tex_t *t, GLuint samp) {
    so_t *so = so_of(samp);
    if (so) return samp_state(&so->sp, t);
    if (t->smp_dirty || !t->smp) { t->smp = samp_state(&t->sp, t); t->smp_dirty = 0; }
    return t->smp;
}
static void lv_free(level_t *l) { free(l->data); memset(l, 0, sizeof *l); }
static void tex_drop(tex_t *t) { for (int f = 0; f < 6; f++) for (int l = 0; l < MAXLV; l++) lv_free(&t->pend[f][l]); t->tex = t->linear_rt = nil; t->made = 0; t->defined = 0; t->levels = 0; t->gen++; }
static void buf_free(buf_t *b) { b->mb = nil; free(b); }
static void texture_buffer_attach(tex_t *t, buf_t *b) {
    if (b != t->buffer) {
        buf_t *old = t->buffer;
        if (old && !--old->texrefs && old->deleted) buf_free(old);
        t->buffer = b; if (b) b->texrefs++;
    }
    t->tex = nil;
}
static NSUInteger texture_buffer_limit(void) {
    NSUInteger limit = g_dev.maxBufferLength / 16;
    return limit < (1u << 28) ? limit : (1u << 28);
}
static id<MTLTexture> texture_buffer_view(id<MTLBuffer> b, MTLPixelFormat pf, size_t width, size_t elem) {
    NSUInteger alignment = [g_dev minimumTextureBufferAlignmentForPixelFormat:pf];
    size_t pitch = (width * elem + alignment - 1) / alignment * alignment;
    if (pitch > b.length) { RED1("buffer texture: %zu bytes of aligned storage required, has %zu", pitch, (size_t)b.length); return nil; }
    MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:pf width:width resourceOptions:b.resourceOptions usage:MTLTextureUsageShaderRead];
    return [b newTextureWithDescriptor:d offset:0 bytesPerRow:pitch];
}
static id<MTLTexture> texture_buffer(tex_t *t) {
    buf_t *b = t ? t->buffer : NULL;
    if (b && b->mb) {
        const fmt_t *f = &g_fmt[t->fmt];
        size_t width = b->size / f->bytes, limit = texture_buffer_limit();
        if (width > limit) width = limit;
        if (width) {
            // A storage replacement or a size change needs a new view. Already encoded draws keep the old one.
            if (!t->tex || t->tex.buffer != b->mb || t->tex.width != width) t->tex = texture_buffer_view(b->mb, f->pf, width, f->bytes);
            if (t->tex) { b->use = g_serial; return t->tex; }
        }
    }
    if (!g_tbuf0) {
        NSUInteger align = [g_dev minimumTextureBufferAlignmentForPixelFormat:MTLPixelFormatRGBA32Float];
        id<MTLBuffer> black = [g_dev newBufferWithLength:(16 + align - 1) / align * align options:MTLResourceStorageModeShared];
        memset(black.contents, 0, black.length);
        ((float *)black.contents)[3] = 1;
        g_tbuf0 = texture_buffer_view(black, MTLPixelFormatRGBA32Float, 1, 16);
    }
    return g_tbuf0;
}
static void tex_write(tex_t *t, int face, int lv, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, const void *data, size_t pitch);
// Make the Metal texture from the levels given so far: as many levels as are given in a row from level 0 (and fit the size), or as glTexStorage said. rt: made for a framebuffer attachment
static int tex_make(tex_t *t, int rt) {
    if (t->made) return t->tex != nil;
    if (t->fmt < 0 || !t->w || !t->h) return 0;
    int faces = tex_faces(t), lv = 0, vol = t->target == GL_TEXTURE_3D, arr = t->target == GL_TEXTURE_2D_ARRAY;
    if (arr && !t->d) return 0;
    uint32_t full = 1; for (uint32_t m = umax(umax(t->w, t->h), vol ? t->d : 1); m > 1; m >>= 1) full++;
    if (t->slevels) lv = (int)umin(umin((uint32_t)t->slevels, full), MAXLV);
    else { while (lv < MAXLV && lv < (int)full && (t->defined >> lv & 1)) lv++; if (!lv) lv = 1; }
    const fmt_t *m = &g_fmt[t->fmt];
    @autoreleasepool {
        MTLTextureDescriptor *td = [MTLTextureDescriptor new];
        td.textureType = faces == 6 ? MTLTextureTypeCube : vol ? MTLTextureType3D : arr ? MTLTextureType2DArray : MTLTextureType2D;
        td.pixelFormat = m->pf; td.width = t->w; td.height = faces == 6 ? t->w : t->h;
        if (vol) td.depth = umax(1, t->d); else if (arr) td.arrayLength = umax(1, t->d);
        td.mipmapLevelCount = lv;
        MTLTextureSwizzleChannels sw = tex_swz(t);
        int swz = sw.red != MTLTextureSwizzleRed || sw.green != MTLTextureSwizzleGreen || sw.blue != MTLTextureSwizzleBlue || sw.alpha != MTLTextureSwizzleAlpha;
        int data = 0; for (int f = 0; f < faces; f++) for (int l = 0; l < lv; l++) data |= t->pend[f][l].data != NULL;
        td.usage = MTLTextureUsageShaderRead | (m->kind != K_BLOCK && !swz ? MTLTextureUsageRenderTarget : 0);
        // a render target (first made as an attachment, with no data given): private, so Metal can compress it, and its uploads are blits (tex_write). Textures the CPU fills stay
        // shared: the movie planes, made with no data and filled by glTexSubImage2D every frame, are written in place once the GPU is done with them
        td.storageMode = m->kind == K_DEPTH || m->kind == K_DS || (rt && !data) ? MTLStorageModePrivate : MTLStorageModeShared;
        if (swz) td.swizzle = sw;
        if (swz && rt) RED1("texture %ux%u %s with a swizzle is a framebuffer attachment (Metal renders into no swizzled texture)", t->w, t->h, m->name);
        t->linear_rt = nil; t->tex = [g_dev newTextureWithDescriptor:td];
        if (!t->tex) { GLOG("[RED] texture %ux%u %s (%d levels) could not be made", t->w, t->h, m->name, lv); t->made = 1; return 0; }
        // The levels given so far go in through tex_write: a CPU write into a shared texture, a blit into a private one. Depth textures are always private, and a
        // glTexImage2D(GL_DEPTH_COMPONENT, ..., pixels) level was once freed here unwritten (only shared textures were filled). No command buffer has the new texture yet
        t->use = 0;
        for (int f = 0; f < faces; f++) for (int l = 0; l < lv; l++) {
            level_t *p = &t->pend[f][l];
            if (p->data) {
                size_t layer = p->pitch * fmt_rows(t->fmt, p->h);
                for (uint32_t k = 0; k < (arr ? t->d : 1); k++)
                    tex_write(t, arr ? k : f, l, 0, 0, 0, p->w, p->h, (uint8_t *)p->data + k * layer, p->pitch);
            }
        }
    }
    for (int f = 0; f < 6; f++) for (int l = 0; l < MAXLV; l++) lv_free(&t->pend[f][l]);
    t->levels = lv; t->made = 1; t->smp_dirty = 1;
    OLOG("texture %ux%u %s, %d levels%s", t->w, t->h, m->name, lv, faces == 6 ? ", cube" : "");
    return 1;
}
// Write one level's (or a part's) bytes into a made texture, in command order: a CPU write when no command buffer that may still read it is pending,
// else a blit from transient memory (in this frame's command buffer, after ending its render pass, when this frame used it; in a command buffer of its own otherwise).
// face: the cube face or array layer; z: the slice of a 3D texture
static void tex_write(tex_t *t, int face, int lv, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, const void *data, size_t pitch) {
    g_n_up++;
    size_t rows = fmt_rows(t->fmt, h);
    if (t->use <= atomic_load(&g_done) && t->tex.storageMode == MTLStorageModeShared) {
        [t->tex replaceRegion:MTLRegionMake3D(x, y, z, w, h, 1) mipmapLevel:lv slice:face withBytes:data bytesPerRow:pitch bytesPerImage:t->target == GL_TEXTURE_3D ? pitch * rows : 0];
        return;
    }
    size_t off; id<MTLBuffer> st = arena_alloc(pitch * rows, &off);
    memcpy((uint8_t *)st.contents + off, data, pitch * rows);
    id<MTLCommandBuffer> cb;
    if (t->use >= g_serial) { pass_end(); cb = cb_get(); }
    else { cb = [g_q commandBuffer]; cb.label = @"gl upload"; }
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromBuffer:st sourceOffset:off sourceBytesPerRow:pitch sourceBytesPerImage:pitch * rows sourceSize:MTLSizeMake(w, h, 1) toTexture:t->tex
       destinationSlice:face destinationLevel:lv destinationOrigin:MTLOriginMake(x, y, z)];
    [b endEncoding]; g_n_blit++;
    if (cb != g_cb) { dm_gfx_enter(); cb_commit_one(cb); dm_gfx_leave(); }   // no GPU work goes out while the app is in the background (dm_gfx_suspend)
}
// glTexImage2D / glCompressedTexImage2D: the level's bytes, converted
static void tex_image(GLenum target, GLint level, GLint ifmt, int f, int cv, uint32_t w, uint32_t h, uint32_t depth, void *conv, size_t pitch) {
    tex_t *t = target == GL_TEXTURE_2D || target == GL_TEXTURE_2D_ARRAY || (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) ? bound_tex(target) : NULL;
    int face = target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
    if (!t || level < 0 || level >= MAXLV) { RED1("glTexImage2D target %#x level %d with no texture bound", target, level); free(conv); return; }
    (void)cv;
    uint32_t w0 = level ? w << level : w, h0 = level ? h << level : h;
    if (level == 0 && (t->fmt != f || t->w != w || t->h != h || t->d != depth) && (t->made || t->defined)) tex_drop(t);   // level 0 respecified: a new texture
    if (!t->made && !t->defined) { t->fmt = f; t->w = level ? umax(w0, 1) : w; t->h = level ? umax(h0, 1) : h; t->d = depth; }
    if (t->fmt != f) { RED1("texture level %d in format %s, its level 0 in %s", level, g_fmt[f].name, g_fmt[t->fmt].name); free(conv); return; }
    if (!level) t->ifmt = ifmt;
    if (t->made && level < t->levels && t->tex) {
        if (conv) for (uint32_t k = 0; k < depth; k++)
            tex_write(t, target == GL_TEXTURE_2D_ARRAY ? k : face, level, 0, 0, 0, w, h, (uint8_t *)conv + k * pitch * fmt_rows(f, h), pitch);
        free(conv); return;
    }
    if (t->made) { RED1("texture level %d given after the texture was made with %d levels: ignored", level, t->levels); free(conv); return; }
    level_t *l = &t->pend[face][level]; lv_free(l);
    *l = (level_t){conv, pitch, pitch * fmt_rows(f, h) * depth, w, h};
    int faces = t->target == GL_TEXTURE_CUBE_MAP ? 6 : 1, all = 1;
    for (int k = 0; k < faces; k++) all &= faces == 1 || t->pend[k][level].data != NULL || (level == 0 && !conv);
    if (all) t->defined |= 1 << level;
}

// ---------- Render passes ----------
// The pass being encoded: framebuffer name, its attachments' generation (a changed attachment ends the pass), clears waiting for the pass to begin
static struct { GLuint fb; uint64_t gen; int open, visibility; uint32_t w, h; MTLPixelFormat cpf[NGL_COLORS], dpf, spf; } P;
static struct { int bits; uint8_t colors; float c[NGL_COLORS][4]; double d; int s; GLuint fb; } g_pend_clear;
static struct { void *pso; void *dss; int cull, wind, fill, vp[4], sc[4], sref[2]; float pf, pu, blend_c[4]; } E;   // encoder state set so far (NULL / -1 = unset)

// One visibility result per render pass. Queries sharing the pass retain the same sample count and combine their passes after completion.
typedef struct {
    GLenum target; int deleted; uint64_t serial, result;
    __strong NSMutableArray<id<MTLBuffer>> *passes;
    __strong id<MTLCommandBuffer> timestamp;
} gl_query_t;
NAMES(gl_query_t, g_queries, g_nqueries, g_capqueries)
static gl_query_t *g_active_query[3];
static id<MTLBuffer> query_buffer(void) {
    if (!g_active_query[0] && !g_active_query[1] && !g_active_query[2]) return nil;
    id<MTLBuffer> b = [g_dev newBufferWithLength:sizeof(uint64_t) options:MTLResourceStorageModeShared];
    if (!b) die("[gl] cannot allocate a visibility result");
    *(uint64_t *)b.contents = 0;
    for (int i = 0; i < 3; i++) if (g_active_query[i]) {
        [g_active_query[i]->passes addObject:b];
        g_active_query[i]->serial = g_serial;
    }
    return b;
}

static void query_count(int enabled) {
    if (P.visibility) [g_enc setVisibilityResultMode:enabled ? MTLVisibilityResultModeCounting : MTLVisibilityResultModeDisabled offset:0];
}

static id<MTLTexture> att_tex(const att_t *a, uint64_t *gen) {
    if (a->kind == 1) { tex_t *t = tex_of(a->name); if (!t) return nil; if (!t->made) tex_make(t, 1); *gen += t->gen; t->use = g_serial; return t->tex; }
    if (a->kind == 2) { rb_t *r = rb_of(a->name); if (!r) return nil; *gen += r->gen; return r->tex; }
    return nil;
}
// Sampling keeps the texture's sRGB format. Framebuffer writes and destination blending use
// that format only while GL_FRAMEBUFFER_SRGB is enabled; otherwise the same bytes are linear.
static id<MTLTexture> color_attachment(const att_t *a, id<MTLTexture> image) {
    if (S.srgb || image.pixelFormat != MTLPixelFormatRGBA8Unorm_sRGB) return image;
    tex_t *t = a->kind == 1 ? tex_of(a->name) : NULL;
    rb_t *r = a->kind == 2 ? rb_of(a->name) : NULL;
    id<MTLTexture> view = t ? t->linear_rt : r ? r->linear_rt : nil;
    if (!view) {
        view = [image newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
        if (!view) die("[gl] cannot make the linear view of an sRGB colour attachment");
        if (t) t->linear_rt = view; else if (r) r->linear_rt = view;
    }
    return view;
}
static uint64_t fb_gen(GLuint fb) {
    fb_t *f = fb_of(fb); if (!f) return 0;
    uint64_t g = f->gen;
    for (int i = 0; i < NGL_COLORS; i++) if (f->draw[i]) att_tex(&f->col[f->draw[i] - 1], &g);
    att_tex(&f->depth, &g); att_tex(&f->stencil, &g);
    return g;
}
// The size of a framebuffer (its first attachment's level), 0 when it has none
static void fb_size(GLuint fb, uint32_t *w, uint32_t *h) {
    *w = *h = 0;
    if (!fb) { *w = g_win.w; *h = g_win.h; return; }
    fb_t *f = fb_of(fb); if (!f) return;
    const att_t *a = f->depth.kind ? &f->depth : &f->stencil;
    for (int i = 0; i < NGL_COLORS; i++) if (f->col[i].kind) { a = &f->col[i]; break; }
    uint64_t g = 0;
    id<MTLTexture> t = att_tex(a, &g); if (t) { *w = umax(1, (uint32_t)t.width >> a->level); *h = umax(1, (uint32_t)t.height >> a->level); }
}
static void pass_end(void) {
    if (g_enc) { [g_enc endEncoding]; g_enc = nil; }
    P.open = P.visibility = 0;
}
static void enc_reset(void) { memset(&E, 0, sizeof E); E.cull = E.wind = E.fill = E.sref[0] = E.sref[1] = -1; E.vp[2] = E.sc[2] = -1; E.pf = E.pu = E.blend_c[0] = NAN; }
// A partial clear can bind one draw buffer at output 0 for the clear shader.
// Ordinary draws use the framebuffer's full draw table (only == -1).
static void pass_begin(int only) {
    @autoreleasepool {
        id<MTLCommandBuffer> cb = cb_get();
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        id<MTLTexture> d = nil, s = nil; int dl = 0, sl = 0;
        uint64_t gen = 0;
        fb_t *f = fb_of(S.fb);
        if (!S.fb) {
            if (!g_win.made) { int w, h; dm_vi_layer_size(&w, &h); win_make(w > 0 ? w : 1280, h > 0 ? h : 720); }
            d = s = g_win.ds;
        } else {
            if (f) {
                d = att_tex(&f->depth, &gen); s = att_tex(&f->stencil, &gen);
                dl = f->depth.level; sl = f->stencil.level;
                gen += f->gen;
            }
            if (d && d.pixelFormat != MTLPixelFormatDepth32Float && d.pixelFormat != MTLPixelFormatDepth16Unorm && d.pixelFormat != MTLPixelFormatDepth32Float_Stencil8) d = nil;
            if (s && s.pixelFormat != MTLPixelFormatDepth32Float_Stencil8 && s.pixelFormat != MTLPixelFormatStencil8) s = nil;
            // a depth / stencil texture attached as GL's depth attachment alone (or stencil alone) is both of Metal's: its pixel format has both, and the pipelines say so
            if (d && !s && d.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) { s = d; sl = dl; }
            else if (s && !d && s.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) { d = s; dl = sl; }
        }
        int pc = g_pend_clear.fb == S.fb ? g_pend_clear.bits : 0;
        id<MTLTexture> first = nil; int first_lv = 0;
        memset(P.cpf, 0, sizeof P.cpf);
        for (int i = 0; i < NGL_COLORS; i++) {
            if (only >= 0 && only != i) continue;
            const att_t *att = f && f->draw[i] ? &f->col[f->draw[i] - 1] : NULL;
            id<MTLTexture> c = att ? color_attachment(att, att_tex(att, &gen)) : !S.fb && !i && S.default_draw ? win_color_attachment() : nil;
            if (!c) continue;
            int loc = only < 0 ? i : 0, cl = att ? att->level : 0, cs = att ? att->face : 0;
            MTLRenderPassColorAttachmentDescriptor *a = rp.colorAttachments[loc]; a.texture = c; a.level = cl; a.storeAction = MTLStoreActionStore;
            if (c.textureType == MTLTextureType3D) a.depthPlane = cs; else a.slice = cs;
            if ((pc & GL_COLOR_BUFFER_BIT) && (g_pend_clear.colors & (1u << i))) {
                const float *v = g_pend_clear.c[i]; a.loadAction = MTLLoadActionClear; a.clearColor = MTLClearColorMake(v[0], v[1], v[2], v[3]);
            }
            else a.loadAction = MTLLoadActionLoad;
            P.cpf[loc] = c.pixelFormat;
            if (!first) { first = c; first_lv = cl; }
        }
        if (d) {
            rp.depthAttachment.texture = d; rp.depthAttachment.level = dl; rp.depthAttachment.storeAction = MTLStoreActionStore;
            if (pc & GL_DEPTH_BUFFER_BIT) { rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.clearDepth = g_pend_clear.d; }
            else rp.depthAttachment.loadAction = MTLLoadActionLoad;
        }
        if (s) {
            rp.stencilAttachment.texture = s; rp.stencilAttachment.level = sl; rp.stencilAttachment.storeAction = MTLStoreActionStore;
            if (pc & GL_STENCIL_BUFFER_BIT) { rp.stencilAttachment.loadAction = MTLLoadActionClear; rp.stencilAttachment.clearStencil = g_pend_clear.s; }
            else rp.stencilAttachment.loadAction = MTLLoadActionLoad;
        }
        if (g_pend_clear.fb == S.fb) { g_pend_clear.bits = 0; g_pend_clear.colors = 0; }
        if (!first && !d && !s) { RED1("framebuffer %u has no usable attachment", S.fb); return; }
        id<MTLTexture> any = first ? first : d ? d : s; int lv = first ? first_lv : d ? dl : sl;
        P.w = umax(1, (uint32_t)any.width >> lv); P.h = umax(1, (uint32_t)any.height >> lv);
        if (!first) { rp.renderTargetWidth = P.w; rp.renderTargetHeight = P.h; }
        P.dpf = d ? d.pixelFormat : MTLPixelFormatInvalid; P.spf = s ? s.pixelFormat : MTLPixelFormatInvalid;
        id<MTLBuffer> visibility = query_buffer();
        if (visibility) rp.visibilityResultBuffer = visibility;
        g_enc = [cb renderCommandEncoderWithDescriptor:rp];
        P.fb = S.fb; P.gen = gen; P.open = 1; g_n_pass++;
        P.visibility = visibility != nil;
        query_count(1);
        enc_reset();
    }
}
// The encoder for the bound framebuffer (a new pass when the binding or an attachment changed)
static int pass_ensure(void) {
    if (P.open && P.fb == S.fb && (!S.fb || fb_gen(S.fb) == P.gen)) return 1;
    pass_end(); pass_begin(-1);
    return P.open;
}
// Clears no draw followed: a pass that only clears
static void clears_flush(void) {
    if (g_pend_clear.bits) { GLuint keep = S.fb; S.fb = g_pend_clear.fb; pass_end(); pass_begin(-1); pass_end(); S.fb = keep; }
}

// ---------- Pipelines ----------
static MTLVertexFormat vfmt_of(const vattr_t *a) {
    int n = a->size < 1 ? 1 : a->size > 4 ? 4 : a->size, no = a->norm;
    static const MTLVertexFormat fl[] = {MTLVertexFormatFloat, MTLVertexFormatFloat2, MTLVertexFormatFloat3, MTLVertexFormatFloat4},
        hf[] = {MTLVertexFormatHalf, MTLVertexFormatHalf2, MTLVertexFormatHalf3, MTLVertexFormatHalf4},
        ub[] = {MTLVertexFormatUChar, MTLVertexFormatUChar2, MTLVertexFormatUChar3, MTLVertexFormatUChar4},
        ubn[] = {MTLVertexFormatUCharNormalized, MTLVertexFormatUChar2Normalized, MTLVertexFormatUChar3Normalized, MTLVertexFormatUChar4Normalized},
        sb[] = {MTLVertexFormatChar, MTLVertexFormatChar2, MTLVertexFormatChar3, MTLVertexFormatChar4},
        sbn[] = {MTLVertexFormatCharNormalized, MTLVertexFormatChar2Normalized, MTLVertexFormatChar3Normalized, MTLVertexFormatChar4Normalized},
        us[] = {MTLVertexFormatUShort, MTLVertexFormatUShort2, MTLVertexFormatUShort3, MTLVertexFormatUShort4},
        usn[] = {MTLVertexFormatUShortNormalized, MTLVertexFormatUShort2Normalized, MTLVertexFormatUShort3Normalized, MTLVertexFormatUShort4Normalized},
        ss[] = {MTLVertexFormatShort, MTLVertexFormatShort2, MTLVertexFormatShort3, MTLVertexFormatShort4},
        ssn[] = {MTLVertexFormatShortNormalized, MTLVertexFormatShort2Normalized, MTLVertexFormatShort3Normalized, MTLVertexFormatShort4Normalized},
        ui[] = {MTLVertexFormatUInt, MTLVertexFormatUInt2, MTLVertexFormatUInt3, MTLVertexFormatUInt4},
        si[] = {MTLVertexFormatInt, MTLVertexFormatInt2, MTLVertexFormatInt3, MTLVertexFormatInt4};
    switch (a->type) {
        case GL_FLOAT: return fl[n - 1];
        case GL_HALF_FLOAT: return hf[n - 1];
        case GL_UNSIGNED_BYTE: return no ? ubn[n - 1] : ub[n - 1];
        case GL_BYTE: return no ? sbn[n - 1] : sb[n - 1];
        case GL_UNSIGNED_SHORT: return no ? usn[n - 1] : us[n - 1];
        case GL_SHORT: return no ? ssn[n - 1] : ss[n - 1];
        case GL_UNSIGNED_INT: return ui[n - 1];
        case GL_INT: return si[n - 1];
    }
    return MTLVertexFormatInvalid;
}
static int vtype_bytes(GLenum t) { return t == GL_FLOAT || t == GL_INT || t == GL_UNSIGNED_INT ? 4 : t == GL_HALF_FLOAT || t == GL_SHORT || t == GL_UNSIGNED_SHORT ? 2 : 1; }
// What a draw's pipeline takes of an attribute (vertex slot s): its format, and its binding's stride and divisor. Stride 0 is a constant fetch (pso_get),
// which has no divisor, so one pipeline serves either
static void vkey_set(pkey_t *k, int s, const vattr_t *a, const vbind_t *b) {
    k->vf[s] = (uint32_t)vfmt_of(a); k->stride[s] = (uint16_t)b->stride; k->div[s] = b->stride ? b->div : 0;
}
static MTLBlendFactor bf_of(GLenum f) {
    switch (f) {
        case 0: return MTLBlendFactorZero; case 1: return MTLBlendFactorOne;
        case GL_SRC_COLOR: return MTLBlendFactorSourceColor; case GL_ONE_MINUS_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
        case GL_SRC_ALPHA: return MTLBlendFactorSourceAlpha; case GL_ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
        case GL_DST_ALPHA: return MTLBlendFactorDestinationAlpha; case GL_ONE_MINUS_DST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
        case GL_DST_COLOR: return MTLBlendFactorDestinationColor; case GL_ONE_MINUS_DST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
        case GL_SRC_ALPHA_SATURATE: return MTLBlendFactorSourceAlphaSaturated;
        case 0x8001: return MTLBlendFactorBlendColor; case 0x8002: return MTLBlendFactorOneMinusBlendColor; case 0x8003: return MTLBlendFactorBlendAlpha; case 0x8004: return MTLBlendFactorOneMinusBlendAlpha;
    }
    RED1("blend factor %#x not mapped", f); return MTLBlendFactorOne;
}
static MTLBlendOperation beq_of(GLenum e) {
    switch (e) { case GL_FUNC_SUBTRACT: return MTLBlendOperationSubtract; case GL_FUNC_REVERSE_SUBTRACT: return MTLBlendOperationReverseSubtract;
                 case GL_MIN: return MTLBlendOperationMin; case GL_MAX: return MTLBlendOperationMax; default: return MTLBlendOperationAdd; }
}
static uint8_t bcode(GLenum f) { return f < 2 ? (uint8_t)f : f >= GL_SRC_COLOR && f <= GL_SRC_ALPHA_SATURATE ? (uint8_t)(f - GL_SRC_COLOR + 2) : f >= 0x8001 && f <= 0x8004 ? (uint8_t)(f - 0x8001 + 11) : 15; }
static GLenum bdecode(uint8_t c) { return c < 2 ? c : c < 11 ? GL_SRC_COLOR + c - 2 : c < 15 ? 0x8001 + c - 11 : 1; }
static MTLColorWriteMask mtl_color_mask(unsigned m) {
    return (m & 1 ? MTLColorWriteMaskRed : 0) | (m & 2 ? MTLColorWriteMaskGreen : 0) |
           (m & 4 ? MTLColorWriteMaskBlue : 0) | (m & 8 ? MTLColorWriteMaskAlpha : 0);
}
static int color_is_uint(MTLPixelFormat pf) { return pf == MTLPixelFormatR16Uint || pf == MTLPixelFormatRG32Uint || pf == MTLPixelFormatRGBA32Uint; }

static id<MTLRenderPipelineState> pso_get(prog_t *p, const pkey_t *k) {
    for (int i = 0; i < p->npc; i++) if (!memcmp(&p->pc[i].k, k, sizeof *k)) return (__bridge id<MTLRenderPipelineState>)p->pc[i].pso;
    id<MTLRenderPipelineState> pso = nil;
    @autoreleasepool {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = p->fn[0]; pd.fragmentFunction = p->fn[1];
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        const shinfo *vs = p->si[0];
        for (int i = 0; i < vs->na; i++) {
            int s = vs->a[i].slot;
            if (k->vf[s]) {
                vd.attributes[s].format = (MTLVertexFormat)k->vf[s]; vd.attributes[s].offset = 0; vd.attributes[s].bufferIndex = 30 - s;
                if (!k->stride[s]) {   // a binding of stride 0: every vertex and instance reads its first element, as a disabled array's value is read
                    vd.layouts[30 - s].stride = 16; vd.layouts[30 - s].stepFunction = MTLVertexStepFunctionConstant; vd.layouts[30 - s].stepRate = 0;
                } else {
                    vd.layouts[30 - s].stride = k->stride[s];
                    if (k->div[s]) { vd.layouts[30 - s].stepFunction = MTLVertexStepFunctionPerInstance; vd.layouts[30 - s].stepRate = k->div[s]; }
                    else vd.layouts[30 - s].stepFunction = MTLVertexStepFunctionPerVertex;
                }
            } else {   // a disabled array: the attribute's current value (glVertexAttrib4f; (0, 0, 0, 1) by default)
                vd.attributes[s].format = MTLVertexFormatFloat4; vd.attributes[s].offset = 0; vd.attributes[s].bufferIndex = 30 - s;
                vd.layouts[30 - s].stride = 16; vd.layouts[30 - s].stepFunction = MTLVertexStepFunctionConstant; vd.layouts[30 - s].stepRate = 0;
            }
        }
        pd.vertexDescriptor = vd;
        for (int i = 0; i < NGL_COLORS; i++) if (k->cpf[i]) {
            MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[i];
            ca.pixelFormat = (MTLPixelFormat)k->cpf[i];
            ca.writeMask = p->fn[1] ? mtl_color_mask(k->mask[i]) : MTLColorWriteMaskNone;
            if (k->blend && !color_is_uint(ca.pixelFormat)) {
                ca.blendingEnabled = YES;
                ca.sourceRGBBlendFactor = bf_of(bdecode(k->bsrc)); ca.destinationRGBBlendFactor = bf_of(bdecode(k->bdst));
                ca.sourceAlphaBlendFactor = bf_of(bdecode(k->bsa)); ca.destinationAlphaBlendFactor = bf_of(bdecode(k->bda));
                ca.rgbBlendOperation = (MTLBlendOperation)k->eqc; ca.alphaBlendOperation = (MTLBlendOperation)k->eqa;
            }
        }
        pd.depthAttachmentPixelFormat = (MTLPixelFormat)k->dpf; pd.stencilAttachmentPixelFormat = (MTLPixelFormat)k->spf;
        NSError *err = nil;
        pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!pso) { static int n; if (n++ < 20) GLOG("[RED] cannot build the pipeline (vs %s, fs %s): %s", p->si[0]->key, p->si[1]->key, err.localizedDescription.UTF8String); }
        else g_n_pso++;
    }
    if (p->npc == p->cappc) { p->cappc = p->cappc ? p->cappc * 2 : 4; p->pc = realloc(p->pc, p->cappc * sizeof *p->pc); }
    p->pc[p->npc].k = *k; p->pc[p->npc].pso = pso ? (__bridge_retained void *)pso : NULL; p->npc++;
    return pso;
}
static id<MTLDepthStencilState> dss_get(void) {
    typedef struct { GLenum sf, s1, s2, s3; GLuint rm, wm; } dface;
    typedef struct { uint8_t dt, dm, st; GLenum df; dface f[2]; } dkey;
    static struct { dkey k; __strong id<MTLDepthStencilState> s; } cache[128]; static int n;
    dkey k; memset(&k, 0, sizeof k);   // padding too: the cache compares whole keys
    k.dt = S.depth && P.dpf; k.dm = S.dmask && S.depth && P.dpf; k.st = S.stencil && P.spf; k.df = S.depth ? S.dfunc : 0;
    if (k.st) for (int i = 0; i < 2; i++) { const stface_t *f = &S.st[i]; k.f[i] = (dface){f->func, f->fail, f->zfail, f->pass, f->mask & 0xff, f->wmask & 0xff}; }
    for (int i = 0; i < n; i++) if (!memcmp(&cache[i].k, &k, sizeof k)) return cache[i].s;
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    if (k.dt) { d.depthCompareFunction = (MTLCompareFunction)(k.df - GL_NEVER); d.depthWriteEnabled = k.dm; }
    if (k.st) for (int fc = 0; fc < 2; fc++) {   // GL's front faces are Metal's (enc_state reverses the winding with the y flip)
        const dface *f = &k.f[fc];
        MTLStencilDescriptor *s = [MTLStencilDescriptor new];
        static const struct { GLenum gl; MTLStencilOperation m; } ops[] = {{GL_KEEP, MTLStencilOperationKeep}, {0, MTLStencilOperationZero}, {GL_REPLACE, MTLStencilOperationReplace},
            {GL_INCR, MTLStencilOperationIncrementClamp}, {GL_DECR, MTLStencilOperationDecrementClamp}, {GL_INVERT, MTLStencilOperationInvert},
            {GL_INCR_WRAP, MTLStencilOperationIncrementWrap}, {GL_DECR_WRAP, MTLStencilOperationDecrementWrap}};
        MTLStencilOperation o[3] = {MTLStencilOperationKeep, MTLStencilOperationKeep, MTLStencilOperationKeep}; GLenum g[3] = {f->s1, f->s2, f->s3};
        for (int j = 0; j < 3; j++) for (size_t i = 0; i < sizeof ops / sizeof *ops; i++) if (ops[i].gl == g[j]) o[j] = ops[i].m;
        s.stencilCompareFunction = (MTLCompareFunction)(f->sf - GL_NEVER); s.stencilFailureOperation = o[0]; s.depthFailureOperation = o[1]; s.depthStencilPassOperation = o[2];
        s.readMask = f->rm; s.writeMask = f->wm;
        if (fc) d.backFaceStencil = s; else d.frontFaceStencil = s;
    }
    id<MTLDepthStencilState> st = [g_dev newDepthStencilStateWithDescriptor:d];
    if (n < 128) { cache[n].k = k; cache[n].s = st; n++; } else RED1("more than 128 depth / stencil states");
    return st;
}

// ---------- Fixed function ----------
// OpenGL 1.x draws with no program current. UnMetal (Unepic Games' engine, SDK 14.3) does nothing else: its loader asks eglGetProcAddress for 26 GL names, and its code calls
// glVertexPointer / glColorPointer / glTexCoordPointer on client arrays, glMatrixMode / glLoadIdentity / glRotatef, glTexEnvi / glTexEnvf and glDrawArrays, and never
// glCreateShader (dm/tools/loader_census.py). The arrays are attributes of the vertex array at NVIDIA's aliased numbers (0 the vertex, 2 the normal, 3 the colour, 8 + n texture
// unit n's coordinates), as its compatibility driver keeps them, so a draw reads them as it reads any attribute (client memory copied per draw, a buffer bound as it is), and a
// disabled one reads its current value (glColor*: white at start; texture coordinates (0, 0, 0, 1)). The program is the layer's own (dm/gfx/msl/gl_ff.metal), its two uniform
// blocks written before each draw from the matrix stacks, the texture environment and the alpha test. Implemented: texture unit 0, no lighting, no fog, no texture coordinate
// generation; a game that turns one of those on gets a red line
enum { GL_MODELVIEW = 0x1700, GL_PROJECTION, GL_TEXTURE, GL_VERTEX_ARRAY = 0x8074, GL_NORMAL_ARRAY, GL_COLOR_ARRAY, GL_INDEX_ARRAY, GL_TEXTURE_COORD_ARRAY,
       GL_ALPHA_TEST = 0xbc0, GL_LIGHTING = 0xb50, GL_FOG = 0xb60, GL_TEXTURE_ENV = 0x2300, GL_TEXTURE_ENV_MODE = 0x2200, GL_TEXTURE_ENV_COLOR = 0x2201,
       GL_MODULATE = 0x2100, GL_DECAL = 0x2101, GL_ADD = 0x104, GL_DOUBLE = 0x140a };
enum { FF_DEPTH = 32, FF_UNITS = 8 };   // stack depth (GL's minimum for modelview), texture units with an environment and coordinates
typedef struct { float m[FF_DEPTH][16]; int top; } ff_stack;
static struct {
    ff_stack modelview, projection, texture[FF_UNITS]; GLenum mode;
    float color[4];                               // the current colour (glColor*)
    int client_unit;                              // glClientActiveTexture
    uint32_t tex2d;                               // GL_TEXTURE_2D, a bit per texture unit
    GLenum env[FF_UNITS]; float envc[FF_UNITS][4];
    uint8_t atest; GLenum afunc; float aref;      // GL_ALPHA_TEST, glAlphaFunc
} F;
static void m_ident(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }
static void ff_init(void) {
    memset(&F, 0, sizeof F);
    F.mode = GL_MODELVIEW; F.afunc = GL_NEVER + 7;
    for (int k = 0; k < 4; k++) F.color[k] = 1;
    m_ident(F.modelview.m[0]); m_ident(F.projection.m[0]);
    for (int u = 0; u < FF_UNITS; u++) { m_ident(F.texture[u].m[0]); F.env[u] = GL_MODULATE; }
}
static ff_stack *ff_matrix(void) {
    if (F.mode == GL_MODELVIEW) return &F.modelview;
    if (F.mode == GL_PROJECTION) return &F.projection;
    if (S.unit < FF_UNITS) return &F.texture[S.unit];
    RED1("fixed-function texture matrix on unsupported unit %d", S.unit);
    return NULL;
}
static void m_mul(float *o, const float *a, const float *b) {   // o = a × b (column-major; o may be a or b)
    float r[16];
    for (int c = 0; c < 4; c++) for (int k = 0; k < 4; k++) r[c * 4 + k] = a[k] * b[c * 4] + a[4 + k] * b[c * 4 + 1] + a[8 + k] * b[c * 4 + 2] + a[12 + k] * b[c * 4 + 3];
    memcpy(o, r, sizeof r);
}
static void m_apply(const float *b) { ff_stack *s = ff_matrix(); if (s) m_mul(s->m[s->top], s->m[s->top], b); }
// The layer's program: two uniform blocks (gl_ff.metal ff_vu / ff_fu) and one sampler, texture unit 0's GL_TEXTURE_2D
typedef struct { float mvp[16], tm[16], point_size, pad[3]; } ff_vu;
typedef struct { int32_t tex, env, has_c, has_a, atest, afunc; float aref, pad; float envc[4]; } ff_fu;
static prog_t g_ffp; static shinfo g_ffsi[2];
static shattr g_ffa[3] = {{"gl_Vertex", 0, T_VEC4}, {"gl_Color", 3, T_VEC4}, {"gl_MultiTexCoord0", 8, T_VEC4}};
static int g_ctx_fixed, g_ctx_profile = 1;
static const float g_ff_va0[4] = {0, 0, 0, 1};
static const float *ff_current(int attr) { return attr == 3 ? F.color : g_ff_va0; }   // what a disabled array reads
static int ff_texture_complete(const tex_t *t, GLint minf) {
    if (!t || t->fmt < 0 || !t->w || !t->h || (!(t->defined & 1) && !t->slevels)) return 0;
    if (minf == GL_LINEAR || minf == GL_NEAREST) return 1;
    unsigned needed = 1;
    for (uint32_t size = umax(t->w, t->h); size > 1 && needed < (unsigned)t->maxlv + 1 && needed < MAXLV; size >>= 1) needed++;
    unsigned mask = (1u << needed) - 1;
    return t->made ? t->levels >= (int)needed : t->slevels ? t->slevels >= (int)needed : (t->defined & mask) == mask;
}
static void ff_bind(void) {
    id<MTLRenderCommandEncoder> e = g_enc;
    ff_vu v = {.point_size = S.psize};
    m_mul(v.mvp, F.projection.m[F.projection.top], F.modelview.m[F.modelview.top]);
    memcpy(v.tm, F.texture[0].m[F.texture[0].top], sizeof v.tm);
    ff_fu f = {0};
    id<MTLTexture> texture = g_tex0;
    id<MTLSamplerState> sampler = g_samp0;
    tex_t *t = tex_of(S.units[0].t2d);
    so_t *so = so_of(S.units[0].samp);
    if ((F.tex2d & 1) && t && ff_texture_complete(t, so ? so->sp.minf : t->sp.minf) && t->tex) {
        GLenum mode = F.env[0];
        f.tex = 1; f.env = mode == GL_REPLACE ? 1 : mode == GL_DECAL ? 2 : mode == GL_BLEND ? 3 : mode == GL_ADD ? 4 : 0;
        f.has_c = t->fmt != F_A8;
        f.has_a = t->fmt != F_L8 && t->fmt != F_BC1_RGB && t->ifmt != GL_RGB && t->ifmt != GL_RGB8 && t->ifmt != GL_RGB565 && t->ifmt != 3;
        memcpy(f.envc, F.envc[0], sizeof f.envc);
        texture = t->tex; sampler = tex_sampler(t, S.units[0].samp); t->use = g_serial;
    }
    if (F.tex2d & ~1u) RED1("GL_TEXTURE_2D enabled on texture unit %d: the fixed pipeline textures from unit 0 only", __builtin_ctz(F.tex2d & ~1u));
    if (F.atest) { f.atest = 1; f.afunc = (int32_t)(F.afunc - GL_NEVER); f.aref = F.aref; }
    [e setVertexBytes:&v length:sizeof v atIndex:0];
    [e setFragmentBytes:&f length:sizeof f atIndex:0];
    [e setFragmentTexture:texture atIndex:0];
    [e setFragmentSamplerState:sampler atIndex:0];
}
static prog_t *ff_prog(void) {
    if (!g_ffp.linked) {
        static int failed; if (failed) return NULL;
        NSError *err = nil;
        id<MTLLibrary> lib = host_msl_lib(g_dev, nil, "gl_ff", &err);
        id<MTLFunction> vs = [lib newFunctionWithName:@"ff_vs"], fs = [lib newFunctionWithName:@"ff_fs"];
        if (!vs || !fs) { failed = 1; GLOG("[RED] the fixed pipeline's shaders (gl_ff) do not load: %s", err.localizedDescription.UTF8String); return NULL; }
        for (int st = 0; st < 2; st++) { strlcpy(g_ffsi[st].key, "fixed-function", sizeof g_ffsi[st].key); g_ffsi[st].frag = st; g_ffsi[st].ub_slot = 0; }
        g_ffsi[0].na = 3; g_ffsi[0].a = g_ffa;
        g_ffp.si[0] = &g_ffsi[0]; g_ffp.si[1] = &g_ffsi[1]; g_ffp.fn[0] = vs; g_ffp.fn[1] = fs;
        for (int i = 0; i < 16; i++) g_ffp.amap[i] = (int8_t)i;
        g_ffp.serial = 0xffffffffu;   // programs count up from 1 (glLinkProgram)
        g_ffp.linked = 1;
        GLOG("fixed pipeline: the first draw with no program current");
    }
    tex_t *t = tex_of(S.units[0].t2d);
    so_t *so = so_of(S.units[0].samp);
    if ((F.tex2d & 1) && t && ff_texture_complete(t, so ? so->sp.minf : t->sp.minf) && !t->made) tex_make(t, 0);
    return &g_ffp;
}

// ---------- Drawing ----------
static void enc_state(void) {
    id<MTLRenderCommandEncoder> e = g_enc;
    // GL viewport (lower-left origin) → flipped Metal viewport (see the file header); the scissor rectangle is the same rows
    int vp[4] = {S.vp[0], S.vp[1], S.vp[2], S.vp[3]};
    if (!vp[2] && !vp[3]) { vp[2] = P.w; vp[3] = P.h; }
    if (memcmp(vp, E.vp, sizeof vp)) { memcpy(E.vp, vp, sizeof vp); [e setViewport:(MTLViewport){vp[0], vp[1] + vp[3], vp[2], -vp[3], 0, 1}]; }
    int sc[4] = {0, 0, (int)P.w, (int)P.h};
    if (S.scissor) {
        int x0 = S.sc[0] < 0 ? 0 : S.sc[0], y0 = S.sc[1] < 0 ? 0 : S.sc[1], x1 = S.sc[0] + S.sc[2], y1 = S.sc[1] + S.sc[3];
        if (x1 > (int)P.w) x1 = P.w; if (y1 > (int)P.h) y1 = P.h;
        sc[0] = x0 < (int)P.w ? x0 : (int)P.w; sc[1] = y0 < (int)P.h ? y0 : (int)P.h; sc[2] = x1 > sc[0] ? x1 - sc[0] : 0; sc[3] = y1 > sc[1] ? y1 - sc[1] : 0;
    }
    if (memcmp(sc, E.sc, sizeof sc)) { memcpy(E.sc, sc, sizeof sc); [e setScissorRect:(MTLScissorRect){sc[0], sc[1], sc[2], sc[3]}]; }
    int cull = !S.cull ? MTLCullModeNone : S.cull_face == GL_FRONT ? MTLCullModeFront : S.cull_face == GL_BACK ? MTLCullModeBack : MTLCullModeNone;
    if (S.cull && S.cull_face == GL_FRONT_AND_BACK) RED1("culling front and back faces not implemented");
    if (cull != E.cull) { E.cull = cull; [e setCullMode:(MTLCullMode)cull]; }
    int wind = S.front == GL_CW ? MTLWindingCounterClockwise : MTLWindingClockwise;   // GL's winding, reversed by the y flip
    if (E.wind != wind) { E.wind = wind; [e setFrontFacingWinding:(MTLWinding)wind]; }
    if (E.fill != S.lines) { E.fill = S.lines; [e setTriangleFillMode:S.lines ? MTLTriangleFillModeLines : MTLTriangleFillModeFill]; }
    int off = S.lines ? S.poffset_line : S.poffset;   // GL offsets a polygon by the offset enable of the mode it is drawn in
    float pf = off ? S.pfactor : 0, pu = off ? S.punits : 0;
    if (pf != E.pf || pu != E.pu) { E.pf = pf; E.pu = pu; [e setDepthBias:pu slopeScale:pf clamp:0]; }
    if (S.stencil && ((int)(S.st[0].ref & 0xff) != E.sref[0] || (int)(S.st[1].ref & 0xff) != E.sref[1])) {
        E.sref[0] = S.st[0].ref & 0xff; E.sref[1] = S.st[1].ref & 0xff; [e setStencilFrontReferenceValue:E.sref[0] backReferenceValue:E.sref[1]];
    }
    if (S.blend && (bcode(S.bsrc) >= 11 || bcode(S.bdst) >= 11 || bcode(S.bsa) >= 11 || bcode(S.bda) >= 11)) {
        const float *c = S.blend_c;
        if (c[0] != E.blend_c[0] || c[1] != E.blend_c[1] || c[2] != E.blend_c[2] || c[3] != E.blend_c[3]) {
            memcpy(E.blend_c, c, sizeof E.blend_c);
            [e setBlendColorRed:c[0] green:c[1] blue:c[2] alpha:c[3]];
        }
    }
    id<MTLDepthStencilState> ds = dss_get();
    if ((__bridge void *)ds != E.dss) { E.dss = (__bridge void *)ds; [e setDepthStencilState:ds]; }
}
// A named uniform block's buffer and offset: the range at its binding point. A range shorter than the block (DOOM 3 binds only a skinned model's joints out of its 6,528-byte
// matrices_ubo), or a buffer that ends before the block does, is copied with zeros after it, so the shader reads no memory outside the buffer
static id<MTLBuffer> blk_buffer(const pblk_t *b, size_t *mo) {
    buf_t *x = buf_of(S.ubo[b->binding].buf);
    size_t off = (size_t)S.ubo[b->binding].off, len = 0;
    if (x && x->mb && off < x->size) len = S.ubo[b->binding].size ? ((size_t)S.ubo[b->binding].size < x->size - off ? (size_t)S.ubo[b->binding].size : x->size - off) : x->size - off;
    if (x) x->use = g_serial;
    if (x && x->mb && len >= b->size) { *mo = off; return x->mb; }
    if (!x || !x->mb) RED1("uniform block %s: no buffer at binding point %u (zeros)", b->name, b->binding);
    id<MTLBuffer> mb = arena_alloc(b->size, mo); memset((uint8_t *)mb.contents + *mo, 0, b->size);
    if (len) memcpy((uint8_t *)mb.contents + *mo, (const uint8_t *)x->mb.contents + off, len);
    return mb;
}
static void bind_uniforms_textures(prog_t *p) {
    id<MTLRenderCommandEncoder> e = g_enc;
    for (int st = 0; st < 2; st++) {
        const shinfo *si = p->si[st];
        id<MTLArgumentEncoder> ae = p->ae[st];
        if (!ae) continue;
        size_t off; id<MTLBuffer> ab = arena_alloc(ae.encodedLength, &off);
        memset((uint8_t *)ab.contents + off, 0, ae.encodedLength);
        [ae setArgumentBuffer:ab offset:off];
        if (st) [e setFragmentBuffer:ab offset:off atIndex:si->arg_slot]; else [e setVertexBuffer:ab offset:off atIndex:si->arg_slot];
        if (si->ub_slot >= 0 && si->ub_size > 0) {
            if (p->ub_serial[st] != g_serial || p->ub_written_gen[st] != p->ub_gen[st]) {
                p->ub_buf[st] = arena_alloc(si->ub_size, &p->ub_off[st]);
                memcpy((uint8_t *)p->ub_buf[st].contents + p->ub_off[st], p->ub[st], si->ub_size);
                p->ub_serial[st] = g_serial; p->ub_written_gen[st] = p->ub_gen[st];
            }
            [ae setBuffer:p->ub_buf[st] offset:p->ub_off[st] atIndex:si->ub_slot];
            [e useResource:p->ub_buf[st] usage:MTLResourceUsageRead stages:st ? MTLRenderStageFragment : MTLRenderStageVertex];
        }
    }
    for (int i = 0; i < p->nblk; i++) {
        const pblk_t *b = &p->blk[i]; size_t mo; id<MTLBuffer> mb = blk_buffer(b, &mo);
        for (int st = 0; st < 2; st++) if (b->slot[st] >= 0) {
            [p->ae[st] setBuffer:mb offset:mo atIndex:b->slot[st]];
            [e useResource:mb usage:MTLResourceUsageRead stages:st ? MTLRenderStageFragment : MTLRenderStageVertex];
        }
    }
    for (int i = 0; i < p->nu; i++) {
        puni_t *u = &p->u[i];
        if (!is_samp(u->type)) continue;
        int unit = u->unit < 0 || u->unit >= NUNITS ? 0 : u->unit;
        GLenum target = u->type == T_SAMPCUBE ? GL_TEXTURE_CUBE_MAP : u->type == T_SAMP3D ? GL_TEXTURE_3D :
                        u->type == T_SAMP2DARR ? GL_TEXTURE_2D_ARRAY : u->type == T_SAMPBUF ? GL_TEXTURE_BUFFER : GL_TEXTURE_2D;
        tex_t *t = tex_of(*unit_slot(unit, target));
        id<MTLTexture> tx = nil; id<MTLSamplerState> sm = g_samp0;
        if (target == GL_TEXTURE_BUFFER) tx = texture_buffer(t);
        else if (t && (t->made || tex_make(t, 0)) && t->tex) { tx = t->tex; sm = tex_sampler(t, S.units[unit].samp); t->use = g_serial; }
        if (!tx) tx = u->type == T_SAMPCUBE ? g_cube0 : u->type == T_SAMP3D ? g_tex3d0 :
                      u->type == T_SAMP2DARR ? g_arr0 : u->type == T_USAMP2D ? g_uint0 : g_tex0;
        static int dl = -1; if (dl < 0) dl = getenv("DM_GL_DRAWLOG") != NULL;
        if (dl && tx == g_tex0 && t == NULL) {   // a sampler with no texture: the layer draws the 1x1 black placeholder
            static uint32_t seen_p[64]; static int seen_u[64], nseen;
            int dup = 0; for (int k = 0; k < nseen; k++) if (seen_p[k] == p->serial && seen_u[k] == unit) dup = 1;
            if (!dup && nseen < 64) { seen_p[nseen] = p->serial; seen_u[nseen++] = unit;
                GLOG("program %u sampler %s (unit %d) has no texture: drawing the 1x1 black placeholder", p->serial, u->name, unit); } }
        for (int st = 0; st < 2; st++) {
            if (u->slot[st] < 0) continue;
            [p->ae[st] setTexture:tx atIndex:u->slot[st]];
            if (u->sampler[st] >= 0 && sm) {
                [p->ae[st] setSamplerState:sm atIndex:u->sampler[st]];
                if (!g_cb_samplers) g_cb_samplers = [NSMutableSet new];
                [g_cb_samplers addObject:sm];
            }
            [e useResource:tx usage:MTLResourceUsageRead stages:st ? MTLRenderStageFragment : MTLRenderStageVertex];
        }
    }
}
static int g_dl_mode, g_dl_n, g_dl_inst, g_dl_elems;   // the draw call being set up, for the DM_GL_DRAWLOG line
// The vertex range a draw touches, which a client-memory array is copied by: draw_arrays' first..first+count, or the range of draw_elements' indices (plus its base vertex),
// scanned only when a draw has such an array (draw_span), since the indices of a draw from vertex buffers are never read on the CPU
static int64_t g_dl_first, g_dl_vend; static const uint8_t *g_dl_idx; static int g_dl_isz, g_dl_spanned;
static void draw_span(void) {
    if (g_dl_spanned || !g_dl_idx) return;
    g_dl_spanned = 1;
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int i = 0; i < g_dl_n; i++) {
        uint64_t x = g_dl_isz == 4 ? ((const uint32_t *)g_dl_idx)[i] : g_dl_isz == 2 ? ((const uint16_t *)g_dl_idx)[i] : g_dl_idx[i];
        if (x < lo) lo = x; if (x > hi) hi = x;
    }
    if (lo == UINT64_MAX) lo = hi = 0;
    g_dl_first += (int64_t)lo; g_dl_vend = g_dl_first - (int64_t)lo + (int64_t)hi + 1;   // g_dl_first held the base vertex
}
// Everything a draw needs; 0 = skip it
static id<MTLTexture> fb_color(GLuint fb, int draw, int *level, int *slice);
static int draw_setup(void) {
    prog_t *p = S.prog ? prog_of(S.prog) : g_ctx_fixed ? ff_prog() : NULL;   // no program current: OpenGL 1.x's fixed pipeline (Fixed function), where the context has it
    if (!p || !p->linked) { g_sk_prog++; RED1("draw with no usable program (%u)", S.prog); return 0; }
    if (!pass_ensure()) { g_sk_pass++; return 0; }
    pkey_t k; memset(&k, 0, sizeof k);
    vao_t *v = cur_vao(); const shinfo *vs = p->si[0];
    id<MTLBuffer> cbuf[16] = {nil}; size_t coff[16] = {0};
    for (int i = 0; i < vs->na; i++) {
        int s = vs->a[i].slot; vattr_t *a = &v->a[p->amap[s]]; const vbind_t *vb = vb_of(v, p->amap[s]);
        if (!a->on) continue;
        vkey_set(&k, s, a, vb);
        if (p == &g_ffp && vb->client && vb->stride > 0 && vb->stride <= UINT16_MAX - 3)
            k.stride[s] = (uint16_t)((vb->stride + 3) & ~3);
        if (!k.vf[s] || (k.stride[s] & 3)) { RED1("vertex attribute %d: type %#x size %d stride %d not mapped", p->amap[s], a->type, a->size, vb->stride); return 0; }
        buf_t *b = buf_of(vb->buf);
        if (b && b->mb) continue;   // a vertex buffer: bound as it is
        if (vb->buf) { g_sk_attr++; RED1("vertex attribute %d reads a buffer with no data (%u)", p->amap[s], vb->buf); return 0; }
        if (!vb->client) { g_sk_attr++; RED1("vertex attribute %d reads a binding with no buffer", p->amap[s]); return 0; }   // GL leaves such a read undefined
        // Client memory: GL lets the game keep the vertices in its own array, Metal needs a buffer, so copy the span this draw touches into a
        // transient block (GameMaker's runner draws every sprite and the screen composite through such arrays). The block keeps vertex i at i * stride from
        // its start, as the array does, so the draw's own first vertex, indices and base vertex read the same vertices; only lo..hi is copied. A per-instance
        // attribute (divisor d) is read by instance: elements 0 .. ceil(instances / d)
        {   draw_span();
            int64_t lo = vb->div ? 0 : g_dl_first, hi = vb->div ? (g_dl_inst + (int64_t)vb->div - 1) / vb->div : g_dl_vend; if (hi - lo < 1) hi = lo + 1;
            size_t stride = k.stride[s], esz = (size_t)a->size * (size_t)vtype_bytes(a->type);
            size_t len = lo < 0 ? 0 : (size_t)lo * stride + (size_t)(hi - lo - 1) * stride + esz;
            if (!esz || lo < 0 || len > (128u << 20)) { g_sk_attr++; RED1("vertex attribute %d: client array vertices %lld..%lld (%zu bytes) out of range", p->amap[s], (long long)lo, (long long)hi, len); return 0; }
            size_t off; id<MTLBuffer> mb = arena_alloc(len, &off);
            uint8_t *dst = (uint8_t *)mb.contents + off;
            const uint8_t *src = (const uint8_t *)(uintptr_t)(vb->off + a->rel);
            if (stride == (size_t)vb->stride) memcpy(dst + (size_t)lo * stride, src + (size_t)lo * stride, len - (size_t)lo * stride);
            else for (size_t i = (size_t)lo; i < (size_t)hi; i++) memcpy(dst + i * stride, src + i * (size_t)vb->stride, esz);
            cbuf[s] = mb; coff[s] = off;   // bound below
        }
    }
    for (int i = 0; i < NGL_COLORS; i++) if (P.cpf[i]) {
        k.cpf[i] = (uint32_t)P.cpf[i]; k.mask[i] = S.cmask[i]; k.blend = S.blend;
    }
    k.dpf = (uint32_t)P.dpf; k.spf = (uint32_t)P.spf;
    if (k.blend) { k.bsrc = bcode(S.bsrc); k.bdst = bcode(S.bdst); k.bsa = bcode(S.bsa); k.bda = bcode(S.bda); k.eqc = beq_of(S.eqc); k.eqa = beq_of(S.eqa); }
    id<MTLRenderPipelineState> pso = pso_get(p, &k);
    if (!pso) { g_sk_pso++; return 0; }
    id<MTLRenderCommandEncoder> e = g_enc;
    if ((__bridge void *)pso != E.pso) { E.pso = (__bridge void *)pso; [e setRenderPipelineState:pso]; }
    enc_state();
    for (int i = 0; i < vs->na; i++) {
        int s = vs->a[i].slot, gi = p->amap[s]; vattr_t *a = &v->a[gi];
        if (a->on && cbuf[s]) [e setVertexBuffer:cbuf[s] offset:coff[s] atIndex:30 - s];
        else if (a->on) { const vbind_t *vb = vb_of(v, gi); buf_t *b = buf_of(vb->buf); b->use = g_serial; [e setVertexBuffer:b->mb offset:vb->off + a->rel atIndex:30 - s]; }
        else [e setVertexBytes:p == &g_ffp ? ff_current(gi) : S.va[gi] length:16 atIndex:30 - s];
    }
    if (p == &g_ffp) ff_bind(); else bind_uniforms_textures(p);
    g_n_draw++;
    // DM_GL_DRAWLOG=<n>[:<seconds>]: the first n draws after <seconds> of run time (default 0) with their target, viewport, scissor box and program
    // (a port's first question is where its draws go: a picture that stays black while the game keeps drawing is usually a target or a viewport, not a shader)
    { static long lim = -1, start_at; static double t0; static long n;
      if (lim < 0) { const char *e = getenv("DM_GL_DRAWLOG"); lim = e ? atol(e) : 0; if (e && strchr(e, ':')) start_at = atol(strchr(e, ':') + 1); t0 = now_ns() / 1e9; }
      if (n < lim && now_ns() / 1e9 - t0 >= (double)start_at) { int lv = 0, sl = 0; id<MTLTexture> t = fb_color(S.fb, 0, &lv, &sl); n++;
          const shinfo *vsi = p->si[0];
          fb_t *fbt = fb_of(S.fb);
          GLOG("draw %ld: framebuffer %u (colour tex %u) target %ux%u viewport %d,%d %dx%d scissor %d,%d %dx%d program %u blend %d mode %#x %s %d x%d",
               (long)n, S.fb, fbt && fbt->draw[0] ? fbt->col[fbt->draw[0] - 1].name : 0, t ? (unsigned)t.width : 0, t ? (unsigned)t.height : 0,
               S.vp[0], S.vp[1], S.vp[2], S.vp[3], S.sc[0], S.sc[1], S.sc[2], S.sc[3],
               S.prog, S.blend, (unsigned)g_dl_mode, g_dl_elems ? "indexed" : "array", g_dl_n, g_dl_inst);
          { static uint32_t dumped[32]; static int ndumped; int seen = 0; for (int k = 0; k < ndumped; k++) if (dumped[k] == p->serial) seen = 1;
            if (!seen && ndumped < 32) { dumped[ndumped++] = p->serial;
                GLOG("program %u blocks: vertex slot %d size %u, fragment slot %d size %u; %d uniforms:", p->serial, p->si[0]->ub_slot, p->si[0]->ub_size, p->si[1]->ub_slot, p->si[1]->ub_size, p->nu);
                for (int k = 0; k < p->nu; k++) GLOG("   uniform %s type %u count %u unit %d (offset vertex %d fragment %d)", p->u[k].name, p->u[k].type, p->u[k].count, p->u[k].unit, p->u[k].off[0], p->u[k].off[1]); } }
          // The textures the samplers point at: a full-screen composite of a 1280x720 surface must show that size in unit 0
          { char line[512]; size_t o = 0;
            for (int i = 0; i < 16; i++) { GLuint tn = S.units[i].t2d; tex_t *t = tex_of(tn); if (!t || !t->tex) continue;
              o += snprintf(line + o, sizeof line - o, "%s%d: tex %u %ux%u fmt %lu", o ? ", " : "", i, tn, (unsigned)t->tex.width, (unsigned)t->tex.height, (unsigned long)t->tex.pixelFormat); }
            GLOG("   samplers %s", line); }
          // The first three vertices of every active attribute: where a black frame's quads actually are and which part of a texture they read
          { vao_t *va = cur_vao();
            for (int i = 0; i < vsi->na && i < 8; i++) { int s = vsi->a[i].slot, gi = p->amap[s]; vattr_t *a = &va->a[gi]; const vbind_t *vb = vb_of(va, gi);
              if (!a->on || a->size < 1 || a->size > 4) continue;
              buf_t *b = buf_of(vb->buf); uint64_t at = vb->off + a->rel;
              const uint8_t *base = b && b->mb ? (const uint8_t *)b->mb.contents + at : vb->client ? (const uint8_t *)(uintptr_t)at : NULL;
              if (!base) { GLOG("   attribute %d: no data", gi); continue; }
              size_t st = (size_t)vb->stride;
              char v[3][64]; for (int k = 0; k < 3; k++) { const float *f = (const float *)(base + k * st); char *o = v[k]; o += snprintf(o, 64, "(");
                for (int c = 0; c < a->size && c < 4; c++) o += snprintf(o, 64 - (size_t)(o - v[k]), "%s%.4f", c ? " " : "", f[c]);
                snprintf(o, 64 - (size_t)(o - v[k]), ")"); }
              GLOG("   attribute %d: size %d type %#x stride %zu %s, first 3 vertices %s %s %s", gi, a->size, a->type, st, vb->buf ? "buffer" : "client memory", v[0], v[1], v[2]); } } } }

    return 1;
}
static MTLPrimitiveType prim_of(GLenum m, int *conv) {
    *conv = 0;
    switch (m) {
        case GL_POINTS: return MTLPrimitiveTypePoint; case GL_LINES: return MTLPrimitiveTypeLine; case GL_LINE_STRIP: return MTLPrimitiveTypeLineStrip;
        case GL_TRIANGLES: return MTLPrimitiveTypeTriangle; case GL_TRIANGLE_STRIP: return MTLPrimitiveTypeTriangleStrip;
        case GL_LINE_LOOP: *conv = 1; return MTLPrimitiveTypeLineStrip;
        case GL_TRIANGLE_FAN: *conv = 2; return MTLPrimitiveTypeTriangle;
        case GL_QUADS: *conv = 3; return MTLPrimitiveTypeTriangle;
    }
    RED1("primitive mode %#x not implemented", m); *conv = -1; return MTLPrimitiveTypeTriangle;
}
// Indices of a mode Metal does not have (line loop, triangle fan, quads) as a list, from the given indices (or 0 … n−1); returns the new count
static uint32_t idx_convert(int conv, const uint32_t *in, uint32_t n, uint32_t base, uint32_t *out) {
    uint32_t m = 0;
#define IX(i) (in ? in[i] : base + (i))
    if (conv == 1) { for (uint32_t i = 0; i < n; i++) out[m++] = IX(i); if (n) out[m++] = IX(0); }
    else if (conv == 2) for (uint32_t i = 2; i < n; i++) { out[m++] = IX(0); out[m++] = IX(i - 1); out[m++] = IX(i); }
    else if (conv == 3) for (uint32_t i = 0; i + 3 < n; i += 4) { out[m++] = IX(i); out[m++] = IX(i + 1); out[m++] = IX(i + 2); out[m++] = IX(i); out[m++] = IX(i + 2); out[m++] = IX(i + 3); }
#undef IX
    return m;
}
static void draw_arrays(GLenum mode, GLint first, GLsizei count, GLsizei inst) {
    if (count <= 0 || inst <= 0) return;
    g_dl_mode = mode; g_dl_n = count; g_dl_inst = inst; g_dl_elems = 0; g_dl_first = first; g_dl_vend = (int64_t)first + count; g_dl_idx = NULL;
    @autoreleasepool {
        int conv; MTLPrimitiveType pt = prim_of(mode, &conv);
        if (conv < 0 || !draw_setup()) return;
        if (!conv) { [g_enc drawPrimitives:pt vertexStart:first vertexCount:count instanceCount:inst]; return; }
        size_t off; id<MTLBuffer> b = arena_alloc((size_t)count * 6 * 4 + 16, &off);
        uint32_t n = idx_convert(conv, NULL, count, first, (uint32_t *)((uint8_t *)b.contents + off));
        if (n) [g_enc drawIndexedPrimitives:pt indexCount:n indexType:MTLIndexTypeUInt32 indexBuffer:b indexBufferOffset:off instanceCount:inst];
    }
}
// base: added to every index (glDrawElementsBaseVertex: LÖVE draws its batched quads with one shared index buffer so)
static void draw_elements(GLenum mode, GLsizei count, GLenum type, const void *indices, GLint base, GLsizei inst) {
    if (count <= 0 || inst <= 0) return;
    @autoreleasepool {
        int conv; MTLPrimitiveType pt = prim_of(mode, &conv);
        if (conv < 0) return;
        buf_t *eb = buf_of(cur_vao()->ebo);
        uintptr_t io = (uintptr_t)indices; int isz = type == GL_UNSIGNED_INT ? 4 : type == GL_UNSIGNED_SHORT ? 2 : 1;
        const uint8_t *src = eb ? (eb->mb ? (const uint8_t *)eb->mb.contents + io : NULL) : (const uint8_t *)indices;
        if (!src || (eb && io + (size_t)count * isz > eb->size)) { RED1("glDrawElements: indices outside their buffer"); return; }
        g_dl_mode = mode; g_dl_n = count; g_dl_inst = inst; g_dl_elems = 1; g_dl_first = base; g_dl_idx = src; g_dl_isz = isz; g_dl_spanned = 0;   // draw_span reads them
        if (!draw_setup()) return;
        if (!conv && isz > 1 && eb && io % 4 == 0) {   // the game's own index buffer
            eb->use = g_serial;
            [g_enc drawIndexedPrimitives:pt indexCount:count indexType:isz == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16 indexBuffer:eb->mb indexBufferOffset:io
                           instanceCount:inst baseVertex:base baseInstance:0];
            return;
        }
        // client indices, 8-bit indices, an unaligned offset or a converted mode: 32-bit indices in transient memory
        size_t off; id<MTLBuffer> b = arena_alloc((size_t)count * 4 * (conv ? 6 : 1) + 16, &off);
        uint32_t *tmp = conv ? malloc((size_t)count * 4) : (uint32_t *)((uint8_t *)b.contents + off);
        for (GLsizei i = 0; i < count; i++) tmp[i] = isz == 4 ? ((const uint32_t *)src)[i] : isz == 2 ? ((const uint16_t *)src)[i] : src[i];
        uint32_t n = count;
        if (conv) { n = idx_convert(conv, tmp, count, 0, (uint32_t *)((uint8_t *)b.contents + off)); free(tmp); }
        if (n) [g_enc drawIndexedPrimitives:pt indexCount:n indexType:MTLIndexTypeUInt32 indexBuffer:b indexBufferOffset:off instanceCount:inst baseVertex:base baseInstance:0];
    }
}
GLB(glDrawArrays, void, GLenum mode, GLint first, GLsizei count) { draw_arrays(mode, first, count, 1); }
GLB(glDrawArraysInstanced, void, GLenum mode, GLint first, GLsizei count, GLsizei inst) { draw_arrays(mode, first, count, inst); }
GLB(glDrawElements, void, GLenum mode, GLsizei count, GLenum type, const void *indices) { draw_elements(mode, count, type, indices, 0, 1); }
// The range is the caller's bound on the indices. Client arrays already use the actual index span in draw_elements.
GLB(glDrawRangeElements, void, GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices) {
    if (end < start || count < 0) { gl_error(GL_INVALID_VALUE); return; }
    draw_elements(mode, count, type, indices, 0, 1);
}
GLB(glDrawRangeElementsBaseVertex, void, GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices, GLint base) {
    if (end < start || count < 0) { gl_error(GL_INVALID_VALUE); return; }
    draw_elements(mode, count, type, indices, base, 1);
}
GLB(glDrawElementsBaseVertex, void, GLenum mode, GLsizei count, GLenum type, const void *indices, GLint base) { draw_elements(mode, count, type, indices, base, 1); }
GLB(glDrawElementsInstanced, void, GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei inst) { draw_elements(mode, count, type, indices, 0, inst); }
GLB(glDrawElementsInstancedBaseVertex, void, GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei inst, GLint base) { draw_elements(mode, count, type, indices, base, inst); }

// ---------- Clears ----------
static uint8_t draw_colors(void) {
    if (!S.fb) return S.default_draw;
    fb_t *f = fb_of(S.fb);
    uint8_t active = 0;
    if (f) for (int i = 0; i < NGL_COLORS; i++) if (f->draw[i] && f->col[f->draw[i] - 1].kind) active |= 1u << i;
    return active;
}
static void clear_quad(int bits, const GLfloat *color, unsigned cmask) {
    @autoreleasepool {
        static __strong id<MTLRenderPipelineState> cache[16]; static uint64_t ck[16]; static int nc;
        MTLPixelFormat pf = P.cpf[0];
        uint64_t key = (uint64_t)pf | (uint64_t)P.dpf << 16 | (uint64_t)P.spf << 32 | (uint64_t)cmask << 48;
        id<MTLRenderPipelineState> pso = nil;
        for (int i = 0; i < nc; i++) if (ck[i] == key) pso = cache[i];
        if (!pso && g_util) {
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = [g_util newFunctionWithName:@"clr_vs"];
            pd.fragmentFunction = [g_util newFunctionWithName:pf ? (color_is_uint(pf) ? @"clr_fs_uint" : @"clr_fs") : @"clr_fs_none"];
            pd.colorAttachments[0].pixelFormat = pf;
            pd.colorAttachments[0].writeMask = mtl_color_mask(cmask);
            pd.depthAttachmentPixelFormat = P.dpf; pd.stencilAttachmentPixelFormat = P.spf;
            NSError *err = nil; pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
            if (!pso) { RED1("clear pipeline: %s", err.localizedDescription.UTF8String); return; }
            if (nc < 16) { ck[nc] = key; cache[nc++] = pso; }
        }
        if (!pso) return;
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
        if ((bits & GL_DEPTH_BUFFER_BIT) && P.dpf) { dd.depthCompareFunction = MTLCompareFunctionAlways; dd.depthWriteEnabled = YES; }
        if ((bits & GL_STENCIL_BUFFER_BIT) && P.spf) {
            MTLStencilDescriptor *s = [MTLStencilDescriptor new]; s.stencilCompareFunction = MTLCompareFunctionAlways; s.depthStencilPassOperation = MTLStencilOperationReplace;
            s.writeMask = S.st[0].wmask & 0xff; dd.frontFaceStencil = s; dd.backFaceStencil = s;
        }
        id<MTLRenderCommandEncoder> e = g_enc;
        [e setRenderPipelineState:pso]; [e setDepthStencilState:[g_dev newDepthStencilStateWithDescriptor:dd]];
        [e setCullMode:MTLCullModeNone]; [e setTriangleFillMode:MTLTriangleFillModeFill]; [e setStencilReferenceValue:S.clear_s & 0xff];
        [e setViewport:(MTLViewport){0, 0, P.w, P.h, 0, 1}];
        int sc[4] = {0, 0, (int)P.w, (int)P.h};
        if (S.scissor) {
            int x0 = S.sc[0] < 0 ? 0 : S.sc[0], y0 = S.sc[1] < 0 ? 0 : S.sc[1], x1 = S.sc[0] + S.sc[2], y1 = S.sc[1] + S.sc[3];
            if (x1 > (int)P.w) x1 = P.w; if (y1 > (int)P.h) y1 = P.h;
            sc[0] = x0 < (int)P.w ? x0 : (int)P.w; sc[1] = y0 < (int)P.h ? y0 : (int)P.h; sc[2] = x1 > sc[0] ? x1 - sc[0] : 0; sc[3] = y1 > sc[1] ? y1 - sc[1] : 0;
        }
        [e setScissorRect:(MTLScissorRect){sc[0], sc[1], sc[2], sc[3]}];
        float z = (float)S.clear_d; [e setVertexBytes:&z length:4 atIndex:0];
        if (color_is_uint(pf)) {
            uint32_t c[4]; for (int i = 0; i < 4; i++) c[i] = (uint32_t)fmax(0, fmin(UINT32_MAX, color[i]));
            [e setFragmentBytes:c length:sizeof c atIndex:0];
        } else if (pf) [e setFragmentBytes:color length:16 atIndex:0];
        query_count(0);   // this internal clear quad does not belong to a GL occlusion query
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        query_count(1);
        enc_reset();   // the next draw sets its own state again
    }
}
// Full clears become load actions. A partial colour clear uses a quad at one output at a time.
static void clear_buffers(GLbitfield mask, uint8_t colors, const GLfloat *color) {
    uint32_t fw, fh; fb_size(S.fb, &fw, &fh);
    int full = !S.scissor || (S.sc[0] <= 0 && S.sc[1] <= 0 && S.sc[0] + S.sc[2] >= (int)fw && S.sc[1] + S.sc[3] >= (int)fh);
    int bits = mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    if (!S.dmask) bits &= ~GL_DEPTH_BUFFER_BIT;
    if ((bits & GL_STENCIL_BUFFER_BIT) && (S.st[0].wmask & 0xff) != 0xff) full = 0;
    uint8_t active = draw_colors(); colors &= active;
    for (int i = 0; i < NGL_COLORS; i++) if (colors & (1u << i)) {
        if (!S.cmask[i]) colors &= ~(1u << i);
        else if (S.cmask[i] != 0xf) full = 0;
    }
    if (!bits && !colors) return;
    if (full) {
        if (g_pend_clear.bits && g_pend_clear.fb != S.fb) clears_flush();
        if (P.open) pass_end();
        g_pend_clear.fb = S.fb; g_pend_clear.bits |= bits; g_pend_clear.colors |= colors;
        if (bits & GL_DEPTH_BUFFER_BIT) g_pend_clear.d = S.clear_d;
        if (bits & GL_STENCIL_BUFFER_BIT) g_pend_clear.s = S.clear_s;
        if (colors) g_pend_clear.bits |= GL_COLOR_BUFFER_BIT;
        for (int i = 0; i < NGL_COLORS; i++) if (colors & (1u << i)) memcpy(g_pend_clear.c[i], color, sizeof g_pend_clear.c[i]);
        return;
    }
    if (active <= 1) {
        if (pass_ensure()) clear_quad(bits, color, colors ? S.cmask[0] : 0);
        return;
    }
    clears_flush(); pass_end();
    for (int i = 0; i < NGL_COLORS; i++) if (colors & (1u << i)) {
        pass_begin(i);
        if (P.open) { clear_quad(bits, color, S.cmask[i]); bits = 0; }
        pass_end();
    }
    if (bits) {
        pass_begin(NGL_COLORS);
        if (P.open) clear_quad(bits, color, 0);
        pass_end();
    }
}
GLB(glClear, void, GLbitfield mask) { clear_buffers(mask, mask & GL_COLOR_BUFFER_BIT ? UINT8_MAX : 0, S.clear_c); }
GL(glClearColor, void, GLfloat r, GLfloat g, GLfloat b, GLfloat a) { S.clear_c[0] = r; S.clear_c[1] = g; S.clear_c[2] = b; S.clear_c[3] = a; }
GL(glClearDepth, void, GLdouble d) { S.clear_d = d < 0 ? 0 : d > 1 ? 1 : d; }
GL(glClearDepthf, void, GLfloat d) { S.clear_d = d < 0 ? 0 : d > 1 ? 1 : d; }
GL(glClearStencil, void, GLint s) { S.clear_s = s; }
// The depth range is [0,1] unless a game narrows it; the layer's viewport always maps the whole range (GameMaker's runner sets 0..1)
GL(glDepthRange, void, GLdouble n, GLdouble f) { if (n != 0 || f != 1) RED1("glDepthRange(%g, %g): only 0..1 is mapped", (double)n, (double)f); }
GL(glDepthRangef, void, GLfloat n, GLfloat f) { if (n != 0 || f != 1) RED1("glDepthRangef(%g, %g): only 0..1 is mapped", (double)n, (double)f); }
GLB(glClearBufferfv, void, GLenum buffer, GLint drawbuffer, const GLfloat *v) {
    if (buffer == GL_COLOR && drawbuffer >= 0 && drawbuffer < NGL_COLORS) clear_buffers(0, 1u << drawbuffer, v);
    else if (buffer == GL_DEPTH) { double keep = S.clear_d; S.clear_d = v[0] < 0 ? 0 : v[0] > 1 ? 1 : v[0]; h_glClear(GL_DEPTH_BUFFER_BIT); S.clear_d = keep; }
    else RED1("glClearBufferfv(%#x, %d) not implemented", buffer, drawbuffer);
}

// ---------- State ----------
static uint8_t *cap_of(GLenum cap) {
    switch (cap) { case GL_DEPTH_TEST: return &S.depth; case GL_BLEND: return &S.blend; case GL_CULL_FACE: return &S.cull; case GL_SCISSOR_TEST: return &S.scissor;
                   case GL_STENCIL_TEST: return &S.stencil; case GL_SAMPLE_MASK: return &S.sample_mask_on;
                   case GL_POLYGON_OFFSET_FILL: return &S.poffset; case 0x2a02 /* POLYGON_OFFSET_LINE */: return &S.poffset_line; }
    return NULL;
}
static void cap_set(GLenum cap, int on) {
    uint8_t *c = cap_of(cap);
    if (c) { *c = on; return; }
    if (cap == GL_COLOR_TABLE) { S.color_table = on; return; }
    // the fixed pipeline's (Fixed function): texturing per texture unit, the alpha test; lighting and fog are not implemented
    if (cap == GL_TEXTURE_2D) { if (S.unit < 32) F.tex2d = on ? F.tex2d | 1u << S.unit : F.tex2d & ~(1u << S.unit); return; }
    if (cap == GL_ALPHA_TEST) { F.atest = (uint8_t)on; return; }
    if ((cap == GL_LIGHTING || cap == GL_FOG) && !on) return;
    // The fixed pipeline implements texture unit 0 and the alpha test; a light, the texture-coordinate generators, colour material and normal normalization are not
    // implemented, so switching one of them on is reported and switching it off, which cannot change a draw, is not (as for the lighting and the fog above)
    if (!on && ((cap >= 0x4000 && cap <= 0x4007) /* LIGHT0..7 */ || (cap >= 0x0c60 && cap <= 0x0c63) /* TEXTURE_GEN_S / T / R / Q */
                || cap == 0x0b57 /* COLOR_MATERIAL */ || cap == 0x0ba1 /* NORMALIZE */)) return;
    if (cap == GL_FRAMEBUFFER_SRGB) {
        if (S.srgb != on) { clears_flush(); pass_end(); S.srgb = on; }
        return;
    }
    if (cap == 0x809d /* MULTISAMPLE */ || cap == 0x8642 /* PROGRAM_POINT_SIZE */ || cap == 0x8d69 /* PRIMITIVE_RESTART_FIXED_INDEX */ || cap == 0x884f /* TEXTURE_CUBE_MAP_SEAMLESS */
        || cap == 0x864f /* DEPTH_CLAMP: Metal clamps to the depth range already */
        || cap == 0x92e0 /* DEBUG_OUTPUT */ || cap == 0x8242 /* DEBUG_OUTPUT_SYNCHRONOUS */) return;
    static GLenum seen[32]; static int n;
    for (int i = 0; i < n; i++) if (seen[i] == cap) return;
    if (n < 32) { seen[n++] = cap; GLOG("[RED] gl%s(%#x) not implemented", on ? "Enable" : "Disable", cap); }
}
GLB(glEnable, void, GLenum cap) { cap_set(cap, 1); }
GLB(glDisable, void, GLenum cap) { cap_set(cap, 0); }
static uint8_t color_mask(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return (r & 0xff ? 1 : 0) | (g & 0xff ? 2 : 0) | (b & 0xff ? 4 : 0) | (a & 0xff ? 8 : 0);
}
GL(glColorMask, void, uint32_t r, uint32_t g, uint32_t b, uint32_t a) { memset(S.cmask, color_mask(r, g, b, a), sizeof S.cmask); }
GL(glColorMaski, void, GLuint buf, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (buf >= NGL_COLORS) { gl_error(GL_INVALID_VALUE); return; }
    S.cmask[buf] = color_mask(r, g, b, a);
}
GL(glDepthMask, void, uint32_t m) { S.dmask = (m & 0xff) != 0; }
GL(glDepthFunc, void, GLenum f) { if (f >= GL_NEVER && f <= GL_NEVER + 7) S.dfunc = f; }
GL(glBlendFunc, void, GLenum s, GLenum d) { S.bsrc = S.bsa = s; S.bdst = S.bda = d; }
GL(glBlendFuncSeparate, void, GLenum sc, GLenum dc, GLenum sa, GLenum da) { S.bsrc = sc; S.bdst = dc; S.bsa = sa; S.bda = da; }
GL(glBlendEquation, void, GLenum e) { S.eqc = S.eqa = e; }
GL(glBlendEquationSeparate, void, GLenum c, GLenum a) { S.eqc = c; S.eqa = a; }
GL(glBlendColor, void, GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    S.blend_c[0] = r; S.blend_c[1] = g; S.blend_c[2] = b; S.blend_c[3] = a;
}
// This state affects multisample fragments. The host's attachments currently have no sample buffers.
GL(glSampleMaski, void, GLuint index, GLbitfield mask) {
    if (index) { gl_error(GL_INVALID_VALUE); return; }
    S.sample_mask = mask;
}
GL(glCullFace, void, GLenum m) { S.cull_face = m; }
GL(glFrontFace, void, GLenum m) { if (m == GL_CW || m == GL_CCW) S.front = m; }
// Points are as big as the vertex shader's point size output says (Metal has no fixed size). Wireframe is Metal's triangle fill mode, for both faces (DOOM 3 sets it once while it
// puts its state in order); points for polygons and a mode for one face only are not implemented
GL(glPointSize, void, GLfloat s) { S.psize = s; }
// Metal draws lines one pixel wide; Fate/hollow ataraxia REMASTERED sets 1 before its draws
GL(glLineWidth, void, GLfloat w) { if (w != 1) RED1("glLineWidth(%g): lines are drawn one pixel wide", w); }
GL(glPolygonMode, void, GLenum face, GLenum mode) {
    if (mode != 0x1b02 /* FILL */ && mode != 0x1b01 /* LINE */) { RED1("glPolygonMode(%#x) not implemented (filled)", mode); return; }
    if (face != GL_FRONT_AND_BACK) RED1("glPolygonMode of one face (%#x): both faces take it", face);
    S.lines = mode == 0x1b01;
}
GL(glHint, void, GLenum target, GLenum mode) { (void)target; (void)mode; }
// The compatibility profile's shading model applies to the fixed pipeline's colours; GLSL programs interpolate smoothly unless a varying says flat (DOOM 3 sets GL_SMOOTH)
GL(glShadeModel, void, GLenum mode) { if (mode != 0x1d01 /* SMOOTH */) RED1("glShadeModel(%#x): only GL_SMOOTH is implemented", mode); }
// Two-sided stencil: DOOM 3's shadow volumes count front and back faces the other way in one draw (GL_ATI_separate_stencil, GL 2.0's glStencilOpSeparate)
static int st_faces(GLenum face, const char *fn) {   // bit 0 front, bit 1 back
    if (face == GL_FRONT) return 1; if (face == GL_BACK) return 2; if (face == GL_FRONT_AND_BACK) return 3;
    gl_error_at(GL_INVALID_ENUM, fn); return 0;
}
static void st_func(int faces, GLenum f, GLint ref, GLuint mask) {
    if (f < GL_NEVER || f > GL_NEVER + 7) { gl_error_at(GL_INVALID_ENUM, "glStencilFunc"); return; }
    for (int i = 0; i < 2; i++) if (faces >> i & 1) { S.st[i].func = f; S.st[i].ref = ref; S.st[i].mask = mask; }
}
static void st_op(int faces, GLenum sf, GLenum zf, GLenum zp) { for (int i = 0; i < 2; i++) if (faces >> i & 1) { S.st[i].fail = sf; S.st[i].zfail = zf; S.st[i].pass = zp; } }
GL(glStencilFunc, void, GLenum f, GLint ref, GLuint mask) { st_func(3, f, ref, mask); }
GL(glStencilMask, void, GLuint m) { S.st[0].wmask = S.st[1].wmask = m; }
GL(glStencilOp, void, GLenum sf, GLenum zf, GLenum zp) { st_op(3, sf, zf, zp); }
GL(glStencilFuncSeparate, void, GLenum face, GLenum f, GLint ref, GLuint mask) { st_func(st_faces(face, __func__), f, ref, mask); }
GL(glStencilOpSeparate, void, GLenum face, GLenum sf, GLenum zf, GLenum zp) { st_op(st_faces(face, __func__), sf, zf, zp); }
GL(glStencilMaskSeparate, void, GLenum face, GLuint m) { int fs = st_faces(face, __func__); for (int i = 0; i < 2; i++) if (fs >> i & 1) S.st[i].wmask = m; }
GL(glStencilOpSeparateATI, void, GLenum face, GLenum sf, GLenum zf, GLenum zp) { st_op(st_faces(face, __func__), sf, zf, zp); }
GL(glStencilFuncSeparateATI, void, GLenum ff, GLenum bf, GLint ref, GLuint mask) { st_func(1, ff, ref, mask); st_func(2, bf, ref, mask); }
GL(glPolygonOffset, void, GLfloat f, GLfloat u) { S.pfactor = f; S.punits = u; }
GL(glViewport, void, GLint x, GLint y, GLsizei w, GLsizei h) { S.vp[0] = x; S.vp[1] = y; S.vp[2] = w; S.vp[3] = h; }
GL(glScissor, void, GLint x, GLint y, GLsizei w, GLsizei h) { S.sc[0] = x; S.sc[1] = y; S.sc[2] = w; S.sc[3] = h; }
GL(glPixelStorei, void, GLenum p, GLint v) {
    if (p == GL_UNPACK_ALIGNMENT && (v == 1 || v == 2 || v == 4 || v == 8)) S.unpack_align = v;
    else if (p == GL_UNPACK_ROW_LENGTH) S.unpack_row = v;
    else if (p == GL_PACK_ALIGNMENT && (v == 1 || v == 2 || v == 4 || v == 8)) S.pack_align = v;
    else if (p == 0xcf0) S.unpack_swap = v != 0;   // UNPACK_SWAP_BYTES (pixels_convert)
    else if (p >= 0xcf1 && p <= 0xcf4 && !v) {}   // LSB_FIRST, SKIP_ROWS, SKIP_PIXELS at their defaults
    else RED1("glPixelStorei(%#x, %d) not implemented", p, v);
}
// Colour-index images through a palette (GL's compatibility pixel transfer: glColorTable, GL_INDEX_SHIFT / GL_INDEX_OFFSET); the game's texture loader sets a palette for its
// paletted images
GL(glPixelTransferi, void, GLenum p, GLint v) { if (p == GL_INDEX_SHIFT) S.index_shift = v; else if (p == GL_INDEX_OFFSET) S.index_offset = v; else RED1("glPixelTransferi(%#x) not implemented", p); }
GL(glColorTable, void, GLenum target, GLenum ifmt, GLsizei w, GLenum format, GLenum type, const void *table) {
    if (target != GL_COLOR_TABLE || type != GL_UNSIGNED_BYTE || (format != GL_RGBA && format != GL_RGB) || w <= 0 || !table) { RED1("glColorTable(%#x, %#x, %d, %#x, %#x) not implemented", target, ifmt, w, format, type); return; }
    S.npalette = w > 256 ? 256 : w; const uint8_t *s = table;
    for (int i = 0; i < S.npalette; i++) S.palette[i] = format == GL_RGBA ? (uint32_t)s[4 * i] | s[4 * i + 1] << 8 | s[4 * i + 2] << 16 | (uint32_t)s[4 * i + 3] << 24 : (uint32_t)s[3 * i] | s[3 * i + 1] << 8 | s[3 * i + 2] << 16 | 0xff000000u;
    RED1("paletted texture (glColorTable, %d entries): palette lookup at upload, the index shift is %d", w, S.index_shift);
}
GL(glGetError, GLenum, void) { GLenum e = g_gl_error; g_gl_error = GL_NO_ERROR; return e; }   // the flag above, read and cleared
// Everything recorded so far runs to its end (LÖVE when the window changes)
// glFlush: hand the commands encoded so far to the GPU without waiting. The layer commits at the frame's end, so a game that flushes in the middle of a frame
// (GameMaker's runner does, before a menu or a surface read) would otherwise see nothing but the pointer: eglGetProcAddress returned null for a name it only
// asks for, the guest called it and jumped to address 0 (Well Dweller's `CRASH sig=11 addr=0 pc=0 lr=main+0x3409a78`, at the pause menu).
GLB(glFlush, void, void) { if (g_pend_clear.bits) clears_flush(); dm_gfx_enter(); cb_commit(); dm_gfx_leave(); }
GLB(glFinish, void, void) { if (g_pend_clear.bits) clears_flush(); dm_gfx_enter(); id<MTLCommandBuffer> cb = cb_get(); cb_commit(); [cb waitUntilCompleted]; dm_gfx_leave(); }

// These three occlusion targets can share Metal's sample count. Closing a pass at a query boundary keeps each pass in exactly the queries that covered it.
static int query_target(GLenum target) {
    switch (target) {
        case GL_SAMPLES_PASSED: return 0;
        case GL_ANY_SAMPLES_PASSED: return 1;
        case GL_ANY_SAMPLES_PASSED_CONSERVATIVE: return 2;
    }
    RED1("query target %#x: only occlusion queries are implemented", target);
    return -1;
}
static gl_query_t *query_of(GLuint name) { return name && name < g_nqueries ? g_queries[name] : NULL; }
static int query_active(const gl_query_t *q) {
    return q == g_active_query[0] || q == g_active_query[1] || q == g_active_query[2];
}
static void query_free(gl_query_t *q) { q->passes = nil; q->timestamp = nil; free(q); }
GL(glGenQueries, void, GLsizei n, GLuint *ids) {
    if (n < 0) { gl_error(GL_INVALID_VALUE); return; }
    for (GLsizei i = 0; i < n; i++) ids[i] = name_new((void ***)&g_queries, &g_nqueries, &g_capqueries, sizeof(gl_query_t));
}
GL(glDeleteQueries, void, GLsizei n, const GLuint *ids) {
    if (n < 0) { gl_error(GL_INVALID_VALUE); return; }
    for (GLsizei i = 0; i < n; i++) {
        gl_query_t *q = query_of(ids[i]);
        if (!q) continue;
        g_queries[ids[i]] = NULL;
        if (query_active(q)) q->deleted = 1;
        else query_free(q);
    }
}
GLB(glBeginQuery, void, GLenum target, GLuint name) {
    int index = query_target(target);
    if (index < 0) return;
    gl_query_t *q = query_of(name);
    if (!q || g_active_query[index] || query_active(q) || q->target == GL_TIMESTAMP ||
        (q->target && (q->target == GL_SAMPLES_PASSED) != (target == GL_SAMPLES_PASSED))) {
        gl_error(GL_INVALID_OPERATION); return;
    }
    pass_end();
    q->target = target; q->serial = q->result = 0;
    q->passes = [NSMutableArray new];
    g_active_query[index] = q;
}
GLB(glEndQuery, void, GLenum target) {
    int index = query_target(target);
    if (index < 0) return;
    gl_query_t *q = g_active_query[index];
    if (!q) { gl_error(GL_INVALID_OPERATION); return; }
    pass_end();
    g_active_query[index] = NULL;
    if (q->deleted) query_free(q);
}
GLB(glQueryCounter, void, GLuint name, GLenum target) {
    if (target != GL_TIMESTAMP) { gl_error(GL_INVALID_ENUM); return; }
    gl_query_t *q = query_of(name);
    if (!q || (q->target && q->target != GL_TIMESTAMP)) { gl_error(GL_INVALID_OPERATION); return; }
    // Place the timestamp after all work issued so far, including a deferred clear.
    // Metal reports GPU completion time for this command-buffer boundary.
    dm_gfx_enter();
    if (g_pend_clear.bits) clears_flush();
    q->target = target;
    q->timestamp = cb_get();
    q->serial = cb_commit();
    dm_gfx_leave();
}
static int query_result(GLuint name, GLenum p, uint64_t *v) {
    gl_query_t *q = query_of(name);
    if (!q || !q->target || query_active(q)) { gl_error(GL_INVALID_OPERATION); return 0; }
    if (p == GL_QUERY_RESULT_AVAILABLE) { *v = q->serial <= atomic_load(&g_done); return 1; }
    if (p != GL_QUERY_RESULT) { gl_error(GL_INVALID_ENUM); return 0; }
    if (q->target == GL_TIMESTAMP) {
        if (q->timestamp) {
            [q->timestamp waitUntilCompleted];
            q->result = (uint64_t)(q->timestamp.GPUEndTime * 1e9);
            q->timestamp = nil;
        }
        *v = q->result;
        return 1;
    }
    if (q->serial > atomic_load(&g_done)) {
        if (q->serial == g_serial) { dm_gfx_enter(); cb_commit(); dm_gfx_leave(); }
        dm_gl_drain();
    }
    @autoreleasepool {
        for (id<MTLBuffer> b in q->passes) q->result += *(const uint64_t *)b.contents;
        q->passes = nil;
    }
    *v = q->target == GL_SAMPLES_PASSED ? q->result : q->result != 0;
    return 1;
}
GLB(glGetQueryObjectuiv, void, GLuint name, GLenum p, GLuint *v) {
    uint64_t result;
    if (query_result(name, p, &result)) *v = result > UINT32_MAX ? UINT32_MAX : (GLuint)result;
}
GLB(glGetQueryObjecti64v, void, GLuint name, GLenum p, int64_t *v) {
    uint64_t result;
    if (query_result(name, p, &result)) *v = result > INT64_MAX ? INT64_MAX : (int64_t)result;
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
    "GL_ARB_framebuffer_object", "GL_ARB_texture_float", "GL_EXT_texture_sRGB", "GL_KHR_texture_compression_astc_ldr", "GL_NV_shadow_samplers_cube",
    // core in GL 4.5, named by a renderer that checks for them (DOOM 3 stops with "<name> not available" without the ones it needs); their functions are the core ones (gl_lookup)
    "GL_ARB_multitexture", "GL_ARB_texture_compression", "GL_ARB_vertex_buffer_object", "GL_ARB_map_buffer_range", "GL_ARB_draw_elements_base_vertex",
    "GL_ARB_uniform_buffer_object", "GL_ARB_buffer_storage", "GL_ARB_sync", "GL_ARB_seamless_cube_map", "GL_ARB_copy_image",
    "GL_ATI_separate_stencil", "GL_ARB_fragment_program"};
#define NEXTS (int)(sizeof g_exts / sizeof *g_exts)
GL(glGetString, const char *, GLenum n) {
    switch (n) {
        case GL_VENDOR: return "NVIDIA Corporation";
        case GL_RENDERER: return "NVIDIA Tegra X1 (nvgpu)/integrated (Metal)";
        case GL_VERSION: return "4.5.0 NVIDIA 349.00";
        case GL_SHADING_LANGUAGE_VERSION: return "4.50 NVIDIA";
        case GL_EXTENSIONS: {   // the compatibility profile's one string
            static char all[1024]; if (!all[0]) for (int i = 0; i < NEXTS; i++) { strlcat(all, g_exts[i], sizeof all); strlcat(all, " ", sizeof all); }
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
        case GL_CONTEXT_PROFILE_MASK: *v = g_ctx_profile; return;
        case GL_CONTEXT_FLAGS: *v = 0; return;
        case GL_MAX_TEXTURE_SIZE: case GL_MAX_RENDERBUFFER_SIZE: case GL_MAX_CUBE_MAP_TEXTURE_SIZE: *v = 16384; return;
        case GL_MAX_3D_TEXTURE_SIZE: case GL_MAX_ARRAY_TEXTURE_LAYERS: *v = 2048; return;
        case GL_MAX_TEXTURE_BUFFER_SIZE: *v = (GLint)texture_buffer_limit(); return;
        case GL_TEXTURE_BINDING_BUFFER: *v = (GLint)S.units[S.unit].tbuf; return;
        case GL_MAX_VIEWPORT_DIMS: v[0] = v[1] = 16384; return;
        case GL_MAX_COLOR_ATTACHMENTS: case GL_MAX_DRAW_BUFFERS: *v = NGL_COLORS; return;
        case GL_MAX_VERTEX_ATTRIBS: case 0x82da: *v = 16; return;   // and MAX_VERTEX_ATTRIB_BINDINGS (vao_t)
        case 0x82d9: *v = 2047; return;   // MAX_VERTEX_ATTRIB_RELATIVE_OFFSET, GL's minimum
        case GL_MAX_TEXTURE_IMAGE_UNITS: *v = 32; return;
        case 0x8b4c: *v = 16; return;   // MAX_VERTEX_TEXTURE_IMAGE_UNITS: the slots a translated shader's table can name (gl_shaders.py)
        case 0x8871: *v = 8; return;   // MAX_TEXTURE_COORDS: the compatibility profile's texture coordinate sets
        case 0x8a2f: *v = NUBO; return;   // MAX_UNIFORM_BUFFER_BINDINGS
        case 0x8a30: *v = 65536; return;   // MAX_UNIFORM_BLOCK_SIZE
        case 0x8a34: *v = 256; return;   // UNIFORM_BUFFER_OFFSET_ALIGNMENT, as NVIDIA's driver reports it
        case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: *v = NUNITS; return;
        case GL_MAX_VERTEX_UNIFORM_VECTORS: case GL_MAX_FRAGMENT_UNIFORM_VECTORS: *v = 1024; return;
        case GL_MAX_VARYING_VECTORS: *v = 31; return;
        case GL_MAX_SAMPLES: *v = 8; return;
        case GL_MAX_SAMPLE_MASK_WORDS: *v = 1; return;
        case GL_SAMPLE_BUFFERS: case GL_SAMPLES: *v = 0; return;   // the window surface is not multisampled
        case GL_VIEWPORT: memcpy(v, S.vp, sizeof S.vp); return;
        case GL_SCISSOR_BOX: memcpy(v, S.sc, sizeof S.sc); return;
        case GL_CULL_FACE_MODE: *v = S.cull_face; return;
        case GL_FRONT_FACE: *v = S.front; return;
        case GL_FRAMEBUFFER_BINDING: *v = S.fb; return;   // and DRAW_FRAMEBUFFER_BINDING
        case GL_READ_FRAMEBUFFER_BINDING: *v = S.read_fb; return;
        case GL_RENDERBUFFER_BINDING: *v = S.rb; return;
        case GL_CURRENT_PROGRAM: *v = S.prog; return;
        case 0x8894: *v = S.array_buf; return;   // ARRAY_BUFFER_BINDING
        case 0x8895: *v = cur_vao()->ebo; return;   // ELEMENT_ARRAY_BUFFER_BINDING
        case 0x8a28: *v = S.uniform_buf; return;   // UNIFORM_BUFFER_BINDING
        case 0x85b5: *v = S.vao; return;   // VERTEX_ARRAY_BINDING
        case 0x8069: *v = S.units[S.unit].t2d; return;   // TEXTURE_BINDING_2D
        case GL_TEXTURE_BINDING_CUBE_MAP: *v = S.units[S.unit].tcube; return;
        case GL_TEXTURE_BINDING_3D: *v = S.units[S.unit].t3d; return;
        case GL_TEXTURE_BINDING_2D_ARRAY: *v = S.units[S.unit].t2da; return;
        case 0x84e0: *v = GL_TEXTURE0 + S.unit; return;   // ACTIVE_TEXTURE
        case GL_MAX_TEXTURE_MAX_ANISOTROPY: *v = 16; return;
        case GL_UNPACK_ALIGNMENT: *v = S.unpack_align; return;
        case GL_PACK_ALIGNMENT: *v = S.pack_align; return;
    }
    static GLenum seen[32]; static int n;
    int known = 0; for (int i = 0; i < n; i++) known |= seen[i] == p;
    if (!known && n < 32) { seen[n++] = p; GLOG("[RED] glGetIntegerv(%#x) not implemented (0)", p); }
    *v = 0;
}
GL(glGetFloatv, void, GLenum p, GLfloat *v) {
    switch (p) {
        case GL_MAX_TEXTURE_LOD_BIAS: *v = 15; return;
        case GL_MAX_TEXTURE_MAX_ANISOTROPY: *v = 16; return;
        case GL_VIEWPORT: for (int i = 0; i < 4; i++) v[i] = (float)S.vp[i]; return;
        case GL_POINT_SIZE: *v = S.psize; return;
        case GL_BLEND_COLOR: memcpy(v, S.blend_c, sizeof S.blend_c); return;
        case GL_POINT_SIZE_RANGE: case GL_ALIASED_POINT_SIZE_RANGE: v[0] = 1; v[1] = 511; return;   // Metal's largest point
    }
    RED1("glGetFloatv(%#x) not implemented (0)", p); *v = 0;
}

// ---------- Buffers and vertex arrays ----------
GL(glGenBuffers, void, GLsizei n, GLuint *out) { for (GLsizei i = 0; i < n; i++) out[i] = name_new((void ***)&g_bufs, &g_nbufs, &g_capbufs, sizeof(buf_t)); }
GL(glDeleteBuffers, void, GLsizei n, const GLuint *b) {
    // GL unbinds a deleted buffer from the context and the current vertex array (another vertex array keeps its name)
    for (GLsizei i = 0; i < n; i++) {
        buf_t *x = buf_of(b[i]); if (!x) continue;
        g_bufs[b[i]] = NULL;
        for (int j = 0; j < NUNITS; j++) {
            tex_t *t = tex_of(S.units[j].tbuf);
            if (t && t->buffer == x) texture_buffer_attach(t, NULL);
        }
        x->deleted = 1; if (!x->texrefs) buf_free(x);
        vao_t *v = cur_vao();
        if (S.array_buf == b[i]) S.array_buf = 0; if (S.uniform_buf == b[i]) S.uniform_buf = 0; if (v->ebo == b[i]) v->ebo = 0;
        if (S.texture_buf == b[i]) S.texture_buf = 0;
        for (int j = 0; j < NUBO; j++) if (S.ubo[j].buf == b[i]) S.ubo[j].buf = 0;
        for (int j = 0; j < 16; j++) if (v->b[j].buf == b[i]) v->b[j].buf = 0;
    }
}
GL(glBindBuffer, void, GLenum target, GLuint b) {
    if (target == GL_ARRAY_BUFFER) S.array_buf = b;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) cur_vao()->ebo = b;
    else if (target == GL_UNIFORM_BUFFER) S.uniform_buf = b;
    else if (target == GL_TEXTURE_BUFFER) S.texture_buf = b;
    else RED1("glBindBuffer target %#x not implemented", target);
}
// A uniform buffer binding point: the buffer and the range a named uniform block of a program bound to that point reads (bind_blocks); both calls also bind the generic target
static void ubo_bind(GLenum target, GLuint index, GLuint b, GLintptr off, GLsizeiptr size, const char *fn) {
    if (target != GL_UNIFORM_BUFFER) { RED1("%s target %#x not implemented", fn, target); return; }
    if (index >= NUBO || off < 0 || size < 0) { gl_error_at(GL_INVALID_VALUE, fn); return; }
    S.uniform_buf = b; S.ubo[index].buf = b; S.ubo[index].off = b ? off : 0; S.ubo[index].size = b ? size : 0;
}
GL(glBindBufferRange, void, GLenum target, GLuint index, GLuint b, GLintptr off, GLsizeiptr size) { ubo_bind(target, index, b, off, size, __func__); }
GL(glBindBufferBase, void, GLenum target, GLuint index, GLuint b) { ubo_bind(target, index, b, 0, 0, __func__); }
static buf_t *bound_buf(GLenum target) {
    return buf_of(target == GL_ARRAY_BUFFER ? S.array_buf : target == GL_ELEMENT_ARRAY_BUFFER ? cur_vao()->ebo :
                  target == GL_UNIFORM_BUFFER ? S.uniform_buf : target == GL_TEXTURE_BUFFER ? S.texture_buf : 0);
}
// Orphaning: data for a buffer a pending command buffer may still read goes into a new MTLBuffer, so draws already encoded keep the old contents
GLB(glBufferData, void, GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
    (void)usage;
    buf_t *b = bound_buf(target);
    if (!b) { RED1("glBufferData with no buffer bound to %#x", target); return; }
    if (b->immutable) { RED1("glBufferData on a buffer glBufferStorage made"); return; }
    if (size <= 0) { b->mb = nil; b->size = 0; return; }
    @autoreleasepool {
        if (!b->mb || b->mb.length < (NSUInteger)size || b->use > atomic_load(&g_done) || b->mb.length > (NSUInteger)size * 2 + 4096)
            b->mb = [g_dev newBufferWithLength:(size + 15) & ~(GLsizeiptr)15 options:MTLResourceStorageModeShared];
        b->size = size; b->use = 0;
        if (data) memcpy(b->mb.contents, data, size); else memset(b->mb.contents, 0, size);
    }
}
// Immutable storage that stays mapped: the game writes into the MTLBuffer's own memory and fences what the GPU still reads (LÖVE's StreamBufferPersistentMapSync)
GLB(glBufferStorage, void, GLenum target, GLsizeiptr size, const void *data, GLbitfield flags) {
    (void)flags; buf_t *b = bound_buf(target);
    if (!b || size <= 0) { RED1("glBufferStorage(%#x, %ld) with no buffer bound", target, (long)size); return; }
    @autoreleasepool { b->mb = [g_dev newBufferWithLength:(size + 15) & ~(GLsizeiptr)15 options:MTLResourceStorageModeShared]; }
    b->size = size; b->use = 0; b->immutable = 1;
    if (data) memcpy(b->mb.contents, data, size);
}
// In command order: new bytes for a buffer a pending command buffer may still read go into a copy of it, which later draws use (the draws already encoded keep the old one)
GLB(glBufferSubData, void, GLenum target, GLintptr off, GLsizeiptr size, const void *data) {
    buf_t *b = bound_buf(target);
    if (!b || !b->mb || off < 0 || size < 0 || (size_t)(off + size) > b->size) { RED1("glBufferSubData(%#x, %ld, %ld) outside the bound buffer", target, (long)off, (long)size); return; }
    if (b->use > atomic_load(&g_done)) {
        if (b->immutable) RED1("glBufferSubData into a buffer glBufferStorage made while the GPU may still read it");
        else { @autoreleasepool { id<MTLBuffer> nb = [g_dev newBufferWithLength:b->mb.length options:MTLResourceStorageModeShared]; memcpy(nb.contents, b->mb.contents, b->size); b->mb = nb; } b->use = 0; }
    }
    if (size) memcpy((uint8_t *)b->mb.contents + off, data, size);
}
// The mapping is the MTLBuffer's own memory: a persistent or unsynchronized mapping is the game's to fence; invalidating a whole buffer in flight orphans it, as glBufferData does
GLB(glMapBufferRange, void *, GLenum target, GLintptr off, GLsizeiptr len, GLbitfield access) {
    buf_t *b = bound_buf(target);
    if (!b || !b->mb || off < 0 || len <= 0 || (size_t)(off + len) > b->size) { RED1("glMapBufferRange(%#x, %ld, %ld) outside the bound buffer", target, (long)off, (long)len); return NULL; }
    if (b->use > atomic_load(&g_done) && !b->immutable && (access & GL_MAP_INVALIDATE_BUFFER_BIT)) {
        @autoreleasepool { b->mb = [g_dev newBufferWithLength:b->mb.length options:MTLResourceStorageModeShared]; } b->use = 0;
    } else if (b->use > atomic_load(&g_done) && !(access & (0x40 /* PERSISTENT */ | 0x20 /* UNSYNCHRONIZED */)))
        RED1("glMapBufferRange of a buffer the GPU may still read: not waited for");
    return (uint8_t *)b->mb.contents + off;
}
GL(glUnmapBuffer, uint32_t, GLenum target) { (void)target; return 1; }
GL(glGetBufferSubData, void, GLenum target, GLintptr off, GLsizeiptr size, void *data) {
    buf_t *b = bound_buf(target);
    if (!b || !b->mb) { gl_error(GL_INVALID_OPERATION); return; }
    if (off < 0 || size < 0 || (size_t)off > b->size || (size_t)size > b->size - (size_t)off) {
        gl_error(GL_INVALID_VALUE); return;
    }
    if (size) memcpy(data, (const uint8_t *)b->mb.contents + off, size);
}
GL(glFlushMappedBufferRange, void, GLenum target, GLintptr off, GLsizeiptr len) { (void)target; (void)off; (void)len; }   // shared memory: nothing to flush

// ---------- Sync objects ----------
// A fence is the serial of the command buffer being recorded when it was made, signalled when that command buffer completes (g_ev). LÖVE fences each third of its stream
// buffers at the end of a frame and waits for the fence three frames later, before it writes that third again
// A sync is a name, as in GL: a value this layer handed out and has not deleted, which the set of live syncs answers without reading through the value (glIsSync; DOOM 3 asks before it
// waits on or deletes one). A value that never was a sync, or one deleted already, is GL_INVALID_VALUE and is neither written nor freed (OpenGL 4.6, section 4.1). Read through, a
// GLsync that NVN's nvnSyncCreateGLSync (not implemented) left unwritten was freed as if it were a block of ours: libmalloc's abort at boot (GitHub issue #2835)
typedef struct { uint64_t serial; } sync_t;
static CFMutableSetRef g_syncs;   // the live syncs, by address (no retain callbacks)
static int sync_live(const void *s) { return s && g_syncs && CFSetContainsValue(g_syncs, s); }
GL(glFenceSync, void *, GLenum cond, GLbitfield flags) {
    (void)cond; (void)flags; sync_t *s = zalloc(sizeof *s); s->serial = g_serial;
    if (!g_syncs) g_syncs = CFSetCreateMutable(NULL, 0, NULL);
    CFSetAddValue(g_syncs, s); return s;
}
GL(glDeleteSync, void, void *s) {   // frees the set's own entry, a block glFenceSync made, never what the caller passed
    const void *own = NULL; if (!s) return;
    if (!g_syncs || !CFSetGetValueIfPresent(g_syncs, s, &own)) { gl_error(GL_INVALID_VALUE); return; }
    CFSetRemoveValue(g_syncs, own); free((void *)own);
}
GL(glIsSync, uint32_t, void *s) { return sync_live(s); }
GL(glWaitSync, void, void *s, GLbitfield flags, uint64_t timeout) { (void)s; (void)flags; (void)timeout; }   // a GPU-side wait: the command buffers run in order
// glMemoryBarrier orders shader writes (images, storage buffers, transform feedback) and the GPU's writes to persistently mapped buffers against what reads
// them next. Nothing here writes from a shader, the buffers are shared memory, and the command buffers run in order: there is nothing to wait for
GL(glMemoryBarrier, void, GLbitfield barriers) { (void)barriers; }
GLB(glClientWaitSync, GLenum, void *sy, GLbitfield flags, uint64_t timeout_ns) {
    sync_t *s = sy;
    if (!sync_live(s)) { gl_error(GL_INVALID_VALUE); return 0x911d; }   // WAIT_FAILED: no sync object
    if (g_ev.signaledValue >= s->serial) return GL_ALREADY_SIGNALED;
    if (s->serial >= g_serial) {   // its command buffer is still being recorded: commit it now (as GL_SYNC_FLUSH_COMMANDS_BIT asks; without it the wait could never end)
        if (!timeout_ns && !(flags & GL_SYNC_FLUSH_COMMANDS_BIT)) return GL_TIMEOUT_EXPIRED;
        dm_gfx_enter(); cb_commit(); dm_gfx_leave();
    }
    if (!timeout_ns) return GL_TIMEOUT_EXPIRED;
    uint64_t ms = timeout_ns / 1000000;
    return [g_ev waitUntilSignaledValue:s->serial timeoutMS:ms ? ms : 1] ? GL_CONDITION_SATISFIED : GL_TIMEOUT_EXPIRED;
}
GL(glGenVertexArrays, void, GLsizei n, GLuint *out) { for (GLsizei i = 0; i < n; i++) out[i] = name_new((void ***)&g_vaos, &g_nvaos, &g_capvaos, sizeof(vao_t)); }
GL(glDeleteVertexArrays, void, GLsizei n, const GLuint *v) { for (GLsizei i = 0; i < n; i++) if (v[i] && v[i] < g_nvaos && g_vaos[v[i]]) { free(g_vaos[v[i]]); g_vaos[v[i]] = NULL; if (S.vao == v[i]) S.vao = 0; } }
GL(glBindVertexArray, void, GLuint v) { S.vao = v; }
GL(glEnableVertexAttribArray, void, GLuint i) { if (i < 16) cur_vao()->a[i].on = 1; }
GL(glDisableVertexAttribArray, void, GLuint i) { if (i < 16) cur_vao()->a[i].on = 0; }
GL(glEnableVertexArrayAttrib, void, GLuint vao, GLuint i) { vao_t *v = vao && vao < g_nvaos && g_vaos[vao] ? g_vaos[vao] : &g_vao0; if (i < 16) v->a[i].on = 1; }
static void va_format(vattr_t *a, GLint size, GLenum type, uint32_t norm, GLuint rel) { a->size = size == 0x80e1 ? 4 : (uint8_t)size; a->type = type; a->norm = (norm & 0xff) != 0; a->rel = rel; }
static uint8_t va_div(GLuint d) { return d > 255 ? 255 : (uint8_t)d; }
static void va_pointer(GLuint i, GLint size, GLenum type, uint32_t norm, GLsizei stride, const void *ptr) {
    if (i >= 16 || stride < 0) { gl_error(GL_INVALID_VALUE); return; }   // GL_MAX_VERTEX_ATTRIBS
    vao_t *v = cur_vao(); vattr_t *a = &v->a[i]; vbind_t *b = &v->b[i];
    va_format(a, size, type, norm, 0); a->bind = 0;
    // Stride 0 is tightly packed: GL makes it the format's size here, on the binding (its effective stride), so another attribute on it keeps it
    b->buf = S.array_buf; b->client = !S.array_buf; b->off = (uintptr_t)ptr; b->stride = stride ? stride : a->size * vtype_bytes(type);
}
GL(glVertexAttribPointer, void, GLuint i, GLint size, GLenum type, uint32_t norm, GLsizei stride, const void *ptr) {
    va_pointer(i, size, type, norm, stride, ptr);
}
GL(glVertexAttribIPointer, void, GLuint i, GLint size, GLenum type, GLsizei stride, const void *ptr) {
    if (size < 1 || size > 4) { gl_error(GL_INVALID_VALUE); return; }
    if (type < GL_BYTE || type > GL_UNSIGNED_INT) { gl_error(GL_INVALID_ENUM); return; }
    // An integer shader input uses the existing unnormalized Metal integer formats.
    va_pointer(i, size, type, 0, stride, ptr);
}
GL(glVertexAttribDivisor, void, GLuint i, GLuint d) {
    if (i >= 16) { gl_error(GL_INVALID_VALUE); return; }
    vao_t *v = cur_vao(); v->a[i].bind = 0; v->b[i].div = va_div(d);
}
GL(glVertexAttribFormat, void, GLuint i, GLint size, GLenum type, uint32_t norm, GLuint rel) {
    if (i >= 16 || rel > 2047) { gl_error(GL_INVALID_VALUE); return; }   // MAX_VERTEX_ATTRIB_RELATIVE_OFFSET
    va_format(&cur_vao()->a[i], size, type, norm, rel);
}
GL(glVertexAttribBinding, void, GLuint i, GLuint bi) {
    if (i >= 16 || bi >= 16) { gl_error(GL_INVALID_VALUE); return; }   // MAX_VERTEX_ATTRIB_BINDINGS
    cur_vao()->a[i].bind = (uint8_t)(bi + 1);
}
GL(glBindVertexBuffer, void, GLuint bi, GLuint buf, GLintptr off, GLsizei stride) {
    if (bi >= 16 || off < 0 || stride < 0) { gl_error(GL_INVALID_VALUE); return; }
    vbind_t *b = &cur_vao()->b[bi]; b->buf = buf; b->client = 0; b->off = (uint64_t)off; b->stride = stride;   // stride 0 is literal (vbind_t)
}
GL(glVertexBindingDivisor, void, GLuint bi, GLuint d) {
    if (bi >= 16) { gl_error(GL_INVALID_VALUE); return; }
    cur_vao()->b[bi].div = va_div(d);
}
GL(glVertexAttrib4f, void, GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { if (i < 16) { S.va[i][0] = x; S.va[i][1] = y; S.va[i][2] = z; S.va[i][3] = w; } }
GL(glVertexAttrib4fv, void, GLuint i, const GLfloat *v) { if (i < 16) memcpy(S.va[i], v, sizeof S.va[i]); }

// ---------- Fixed function: arrays, the current colour, matrices, the texture environment ----------
// An array of the fixed pipeline: the vertex array's attribute at its aliased number (Fixed function), set as glVertexAttribPointer sets one; norm: integer colours and normals
// are normalized, vertices and texture coordinates are not
static void ff_pointer(int i, GLint size, GLenum type, GLsizei stride, const void *ptr, int norm, const char *fn) {
    if (stride < 0 || size < 1 || (size > 4 && size != 0x80e1)) { gl_error_at(GL_INVALID_VALUE, fn); return; }
    if (size == 0x80e1) RED1("%s with GL_BGRA components: read as RGBA", fn);
    if (type == GL_DOUBLE) RED1("%s of doubles: Metal reads no double vertex attribute", fn);
    vao_t *v = cur_vao(); vattr_t *a = &v->a[i]; vbind_t *b = &v->b[i];
    va_format(a, size, type, (uint32_t)norm, 0); a->bind = 0;
    b->buf = S.array_buf; b->client = !S.array_buf; b->off = (uintptr_t)ptr; b->stride = stride ? stride : a->size * vtype_bytes(type);
}
GL(glVertexPointer, void, GLint size, GLenum type, GLsizei stride, const void *ptr) { ff_pointer(0, size, type, stride, ptr, 0, __func__); }
GL(glColorPointer, void, GLint size, GLenum type, GLsizei stride, const void *ptr) { ff_pointer(3, size, type, stride, ptr, type != GL_FLOAT && type != GL_HALF_FLOAT && type != GL_DOUBLE, __func__); }
GL(glNormalPointer, void, GLenum type, GLsizei stride, const void *ptr) { ff_pointer(2, 3, type, stride, ptr, type != GL_FLOAT && type != GL_HALF_FLOAT && type != GL_DOUBLE, __func__); }
GL(glTexCoordPointer, void, GLint size, GLenum type, GLsizei stride, const void *ptr) {
    if (F.client_unit >= FF_UNITS) { RED1("glTexCoordPointer for texture unit %d", F.client_unit); return; }
    ff_pointer(8 + F.client_unit, size, type, stride, ptr, 0, __func__);
}
GL(glClientActiveTexture, void, GLenum u) { if (u >= GL_TEXTURE0 && u < GL_TEXTURE0 + FF_UNITS) F.client_unit = (int)(u - GL_TEXTURE0); else gl_error(GL_INVALID_ENUM); }
static void client_state(GLenum a, int on) {
    int i = a == GL_VERTEX_ARRAY ? 0 : a == GL_NORMAL_ARRAY ? 2 : a == GL_COLOR_ARRAY ? 3 : a == GL_TEXTURE_COORD_ARRAY ? 8 + F.client_unit : -1;
    if (i < 0 || i >= 16) { RED1("gl%sClientState(%#x) not implemented", on ? "Enable" : "Disable", a); return; }
    cur_vao()->a[i].on = (uint8_t)on;
}
GL(glEnableClientState, void, GLenum a) { client_state(a, 1); }
GL(glDisableClientState, void, GLenum a) { client_state(a, 0); }
GL(glColor4f, void, GLfloat r, GLfloat g, GLfloat b, GLfloat a) { F.color[0] = r; F.color[1] = g; F.color[2] = b; F.color[3] = a; }
GL(glColor3f, void, GLfloat r, GLfloat g, GLfloat b) { F.color[0] = r; F.color[1] = g; F.color[2] = b; F.color[3] = 1; }
GL(glColor4fv, void, const GLfloat *v) { memcpy(F.color, v, sizeof F.color); }
GL(glColor4ub, void, uint32_t r, uint32_t g, uint32_t b, uint32_t a) { F.color[0] = (r & 0xff) / 255.f; F.color[1] = (g & 0xff) / 255.f; F.color[2] = (b & 0xff) / 255.f; F.color[3] = (a & 0xff) / 255.f; }
GL(glMatrixMode, void, GLenum m) { if (m >= GL_MODELVIEW && m <= GL_TEXTURE) F.mode = m; else gl_error(GL_INVALID_ENUM); }
GL(glLoadIdentity, void, void) { ff_stack *s = ff_matrix(); if (s) m_ident(s->m[s->top]); }
GL(glLoadMatrixf, void, const GLfloat *m) { ff_stack *s = ff_matrix(); if (s) memcpy(s->m[s->top], m, sizeof s->m[0]); }
GL(glMultMatrixf, void, const GLfloat *m) { m_apply(m); }
GL(glPushMatrix, void, void) {
    ff_stack *s = ff_matrix();
    if (!s) return;
    if (s->top + 1 >= FF_DEPTH) { gl_error(0x503); return; }   // STACK_OVERFLOW
    memcpy(s->m[s->top + 1], s->m[s->top], sizeof s->m[0]); s->top++;
}
GL(glPopMatrix, void, void) { ff_stack *s = ff_matrix(); if (s) { if (s->top) s->top--; else gl_error(0x504); } }   // STACK_UNDERFLOW
static void m_rotate(float deg, float x, float y, float z) {   // glRotate: deg degrees about (x, y, z), counter-clockwise looking down the axis
    float l = sqrtf(x * x + y * y + z * z); if (l == 0) return;
    x /= l; y /= l; z /= l;
    float c = cosf(deg * (float)M_PI / 180), s = sinf(deg * (float)M_PI / 180), k = 1 - c;
    float r[16] = {x * x * k + c, y * x * k + z * s, x * z * k - y * s, 0,   x * y * k - z * s, y * y * k + c, y * z * k + x * s, 0,
                   x * z * k + y * s, y * z * k - x * s, z * z * k + c, 0,   0, 0, 0, 1};
    m_apply(r);
}
GL(glRotatef, void, GLfloat a, GLfloat x, GLfloat y, GLfloat z) { m_rotate(a, x, y, z); }
GL(glTranslatef, void, GLfloat x, GLfloat y, GLfloat z) { float t[16]; m_ident(t); t[12] = x; t[13] = y; t[14] = z; m_apply(t); }
GL(glScalef, void, GLfloat x, GLfloat y, GLfloat z) { float t[16]; m_ident(t); t[0] = x; t[5] = y; t[10] = z; m_apply(t); }
static void m_ortho(double l, double r, double b, double t, double n, double f) {
    if (l == r || b == t || n == f) { gl_error(GL_INVALID_VALUE); return; }
    float o[16]; m_ident(o);
    o[0] = (float)(2 / (r - l)); o[5] = (float)(2 / (t - b)); o[10] = (float)(-2 / (f - n));
    o[12] = (float)(-(r + l) / (r - l)); o[13] = (float)(-(t + b) / (t - b)); o[14] = (float)(-(f + n) / (f - n));
    m_apply(o);
}
GL(glOrtho, void, GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) { m_ortho(l, r, b, t, n, f); }
GL(glOrthof, void, GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) { m_ortho(l, r, b, t, n, f); }
GL(glFrustum, void, GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    if (n <= 0 || f <= 0 || l == r || b == t || n == f) { gl_error(GL_INVALID_VALUE); return; }
    float o[16]; memset(o, 0, sizeof o);
    o[0] = (float)(2 * n / (r - l)); o[5] = (float)(2 * n / (t - b)); o[8] = (float)((r + l) / (r - l)); o[9] = (float)((t + b) / (t - b));
    o[10] = (float)(-(f + n) / (f - n)); o[11] = -1; o[14] = (float)(-2 * f * n / (f - n));
    m_apply(o);
}
// glTexEnv: the active texture unit's GL_TEXTURE_ENV_MODE and GL_TEXTURE_ENV_COLOR (GL_COMBINE and point sprites are not implemented)
static int tex_env_params(GLenum target, GLenum pname, int vector) {
    if (target != GL_TEXTURE_ENV || (pname != GL_TEXTURE_ENV_MODE && (!vector || pname != GL_TEXTURE_ENV_COLOR))) {
        gl_error(GL_INVALID_ENUM); return 0;
    }
    if (S.unit >= FF_UNITS) { RED1("glTexEnv on texture unit %d", S.unit); return 0; }
    return 1;
}
static void tex_env_mode(double mode) {
    if (mode == 0x8570) { RED1("GL_COMBINE is not implemented"); return; }
    if (mode != GL_MODULATE && mode != GL_REPLACE && mode != GL_DECAL && mode != GL_BLEND && mode != GL_ADD) { gl_error(GL_INVALID_ENUM); return; }
    F.env[S.unit] = (GLenum)mode;
}
GL(glTexEnvf, void, GLenum t, GLenum p, GLfloat v) { if (tex_env_params(t, p, 0)) tex_env_mode(v); }
GL(glTexEnvi, void, GLenum t, GLenum p, GLint v) { if (tex_env_params(t, p, 0)) tex_env_mode(v); }
GL(glTexEnvfv, void, GLenum t, GLenum p, const GLfloat *v) {
    if (!tex_env_params(t, p, 1)) return;
    if (p == GL_TEXTURE_ENV_MODE) tex_env_mode(v[0]);
    else for (int k = 0; k < 4; k++) F.envc[S.unit][k] = fminf(1, fmaxf(0, v[k]));
}
GL(glTexEnviv, void, GLenum t, GLenum p, const GLint *v) {
    if (!tex_env_params(t, p, 1)) return;
    if (p == GL_TEXTURE_ENV_MODE) tex_env_mode(v[0]);
    else for (int k = 0; k < 4; k++) F.envc[S.unit][k] = (float)fmax(0, (2.0 * v[k] + 1) / UINT32_MAX);
}
GL(glAlphaFunc, void, GLenum f, GLfloat ref) { if (f >= GL_NEVER && f <= GL_NEVER + 7) { F.afunc = f; F.aref = ref < 0 ? 0 : ref > 1 ? 1 : ref; } else gl_error(GL_INVALID_ENUM); }

// ---------- Textures ----------
GL(glGenTextures, void, GLsizei n, GLuint *out) { for (GLsizei i = 0; i < n; i++) out[i] = name_new((void ***)&g_texs, &g_ntexs, &g_captexs, sizeof(tex_t)); }
GL(glDeleteTextures, void, GLsizei n, const GLuint *t) {
    for (GLsizei i = 0; i < n; i++) {
        tex_t *x = tex_of(t[i]); if (!x) continue;
        texture_buffer_attach(x, NULL); tex_drop(x); x->smp = nil; free(x); g_texs[t[i]] = NULL;
        for (int u = 0; u < NUNITS; u++) {
            if (S.units[u].t2d == t[i]) S.units[u].t2d = 0; if (S.units[u].tcube == t[i]) S.units[u].tcube = 0;
            if (S.units[u].t3d == t[i]) S.units[u].t3d = 0; if (S.units[u].t2da == t[i]) S.units[u].t2da = 0;
            if (S.units[u].tbuf == t[i]) S.units[u].tbuf = 0;
        }
    }
}
GL(glActiveTexture, void, GLenum u) { if (u >= GL_TEXTURE0 && u < GL_TEXTURE0 + NUNITS) S.unit = u - GL_TEXTURE0; }
GL(glBindTexture, void, GLenum target, GLuint t) {
    GLuint *slot = target == GL_TEXTURE_2D || target == GL_TEXTURE_CUBE_MAP || target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY || target == GL_TEXTURE_BUFFER ? unit_slot(S.unit, target) : NULL;
    if (!slot) { RED1("texture target %#x not implemented", target); return; }
    tex_t *x = tex_of(t);
    if (t && !x) {   // a name glGenTextures never gave: GL makes the texture on its first bind
        if (t > 1u << 20) { RED1("glBindTexture of name %u", t); return; }
        x = name_at((void ***)&g_texs, &g_ntexs, &g_captexs, t, sizeof(tex_t));
    }
    if (x && !x->target) tex_init_params(x, target);
    *slot = t;
}
GL(glTexBuffer, void, GLenum target, GLenum internal_format, GLuint name) {
    if (target != GL_TEXTURE_BUFFER) { gl_error(GL_INVALID_ENUM); return; }
    tex_t *t = bound_tex(target); buf_t *b = buf_of(name);
    if (!t || (name && !b)) { gl_error(GL_INVALID_OPERATION); return; }
    int f;
    switch (internal_format) {
        case GL_R32F: f = F_R32F; break;
        case GL_RG32F: f = F_RG32F; break;
        case GL_RGBA32F: f = F_RGBA32F; break;
        default: gl_error(GL_INVALID_ENUM); return;
    }
    texture_buffer_attach(t, b); t->fmt = f; t->ifmt = (GLint)internal_format;
}
// A sampling parameter (samp_p) from glTexParameter* or glSamplerParameter*; v: the value as floats (four for the border colour). 0 for a parameter that is not one of them
static int samp_param(samp_p *s, GLenum p, const float *v) {
    float f = v[0]; GLint i = (GLint)f;
    switch (p) {
        case GL_TEXTURE_MIN_FILTER: s->minf = i; return 1; case GL_TEXTURE_MAG_FILTER: s->magf = i; return 1;
        case GL_TEXTURE_WRAP_S: s->ws = i; return 1; case GL_TEXTURE_WRAP_T: s->wt = i; return 1; case GL_TEXTURE_WRAP_R: s->wr = i; return 1;
        case GL_TEXTURE_MAX_ANISOTROPY: s->aniso = f; return 1;
        case GL_TEXTURE_COMPARE_MODE: s->cmode = i != 0; return 1;   // COMPARE_REF_TO_TEXTURE or NONE
        case GL_TEXTURE_COMPARE_FUNC: s->cfunc = i; return 1;
        case GL_TEXTURE_LOD_BIAS: s->lodbias = f; return 1;   // LÖVE sets −(mipmap sharpness), 0 by default; DOOM 3 0.5 on some images
        case GL_TEXTURE_MIN_LOD: s->minlod = f; return 1; case GL_TEXTURE_MAX_LOD: s->maxlod = f; return 1;
        case GL_TEXTURE_BORDER_COLOR: memcpy(s->border, v, sizeof s->border); return 1;
    }
    return 0;
}
static void tex_set_swz(tex_t *t, int c, GLint s) {   // fixed once the texture is made (tex_make)
    if (t->made && t->swz[c] != s) RED1("texture swizzle changed after the texture was made: ignored");
    t->swz[c] = s;
}
// glTexParameter*: v as floats, from an integer or a float call alike (GL converts between them the same way)
static void tex_param(GLenum target, GLenum p, const float *v) {
    tex_t *t = bound_tex(target);
    if (!t) return;
    GLint i = (GLint)v[0];
    if (samp_param(&t->sp, p, v)) { t->smp_dirty = 1; return; }
    switch (p) {
        case GL_TEXTURE_MAX_LEVEL: t->maxlv = i; t->smp_dirty = 1; break;
        case 0x813c: break;   // BASE_LEVEL
        case GL_TEXTURE_SWIZZLE_R: case GL_TEXTURE_SWIZZLE_R + 1: case GL_TEXTURE_SWIZZLE_R + 2: case GL_TEXTURE_SWIZZLE_A:
            tex_set_swz(t, p - GL_TEXTURE_SWIZZLE_R, i); break;
        default: RED1("glTexParameter(%#x) not implemented", p); break;
    }
}
GL(glTexParameteri, void, GLenum target, GLenum p, GLint v) { float f[4] = {(float)v}; tex_param(target, p, f); }
GL(glTexParameterf, void, GLenum target, GLenum p, GLfloat v) { float f[4] = {v}; tex_param(target, p, f); }
GL(glTexParameterfv, void, GLenum target, GLenum p, const GLfloat *v) {
    if (p == GL_TEXTURE_SWIZZLE_RGBA) { tex_t *t = bound_tex(target); if (t) for (int c = 0; c < 4; c++) tex_set_swz(t, c, (GLint)v[c]); return; }
    float f[4] = {v[0]}; if (p == GL_TEXTURE_BORDER_COLOR) memcpy(f, v, sizeof f); tex_param(target, p, f);
}
GL(glTexParameteriv, void, GLenum target, GLenum p, const GLint *v) {   // an integer border colour is normalized as GL does: the full int range to −1..1
    if (p == GL_TEXTURE_SWIZZLE_RGBA) { tex_t *t = bound_tex(target); if (t) for (int c = 0; c < 4; c++) tex_set_swz(t, c, v[c]); return; }
    float f[4] = {(float)v[0]};
    if (p == GL_TEXTURE_BORDER_COLOR) for (int c = 0; c < 4; c++) f[c] = (float)((double)v[c] / 2147483647.0);
    tex_param(target, p, f);
}

// ---------- Sampler objects ----------
GL(glGenSamplers, void, GLsizei n, GLuint *out) {
    for (GLsizei i = 0; i < n; i++) { out[i] = name_new((void ***)&g_sos, &g_nsos, &g_capsos, sizeof(so_t)); g_sos[out[i]]->sp = SAMP_P0; }
}
GL(glDeleteSamplers, void, GLsizei n, const GLuint *s) {   // a deleted sampler is unbound from every unit it was on
    for (GLsizei i = 0; i < n; i++) {
        if (!so_of(s[i])) continue;
        free(g_sos[s[i]]); g_sos[s[i]] = NULL;
        for (int u = 0; u < NUNITS; u++) if (S.units[u].samp == s[i]) S.units[u].samp = 0;
    }
}
GL(glIsSampler, uint32_t, GLuint s) { return so_of(s) != NULL; }
GL(glBindSampler, void, GLuint unit, GLuint s) {
    if (unit >= NUNITS) { RED1("glBindSampler on unit %u: the layer has %d", unit, NUNITS); return; }
    if (s && !so_of(s)) { RED1("glBindSampler of sampler %u, which glGenSamplers did not give", s); return; }
    S.units[unit].samp = s;
}
static void so_param(GLuint s, GLenum p, const float *v) {
    so_t *o = so_of(s);
    if (!o) { RED1("glSamplerParameter on sampler %u, which glGenSamplers did not give", s); return; }
    if (!samp_param(&o->sp, p, v)) RED1("glSamplerParameter(%#x) not implemented", p);
}
GL(glSamplerParameteri, void, GLuint s, GLenum p, GLint v) { float f[4] = {(float)v}; so_param(s, p, f); }
GL(glSamplerParameterf, void, GLuint s, GLenum p, GLfloat v) { float f[4] = {v}; so_param(s, p, f); }
GL(glSamplerParameterfv, void, GLuint s, GLenum p, const GLfloat *v) { float f[4] = {v[0]}; if (p == GL_TEXTURE_BORDER_COLOR) memcpy(f, v, sizeof f); so_param(s, p, f); }
GL(glSamplerParameteriv, void, GLuint s, GLenum p, const GLint *v) {
    float f[4] = {(float)v[0]};
    if (p == GL_TEXTURE_BORDER_COLOR) for (int c = 0; c < 4; c++) f[c] = (float)((double)v[c] / 2147483647.0);
    so_param(s, p, f);
}
// The conversion of data given in format and type into a texture's own format f (glTexSubImage2D, glClearTexImage)
static int sub_cv(int f, GLenum format, GLenum type) {
    if (f != F_RGBA8) return CV_COPY;
    return type == GL_UNSIGNED_SHORT_5_6_5 ? CV_565 : type == GL_UNSIGNED_SHORT_4_4_4_4 ? CV_4444 : type == GL_UNSIGNED_SHORT_5_5_5_1 ? CV_5551 :
           format == GL_RGB ? CV_RGB8 : format == GL_COLOR_INDEX ? CV_INDEX : CV_COPY;
}
// glTexImage2D and glTexSubImage2D take nine arguments, the last on the stack, which the big-stack entry does not carry: they pack them here, on the game's stack, and enter there
typedef struct { GLenum target; GLint level, ifmt, x, y; GLsizei w, h; GLenum format, type; const void *pixels; GLsizei size; int sub, compressed; } teximg_args;
static void teximg_impl(const teximg_args *a) __attribute__((used));
BIGSTUB(teximg_impl);
static void teximg_impl(const teximg_args *a) {
    @autoreleasepool {
        if (a->sub) {
            tex_t *t = bound_tex(a->target);
            int face = a->target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && a->target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? (int)(a->target - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
            if (!t || t->fmt < 0 || a->w <= 0 || a->h <= 0) return;
            int f = t->fmt, cv = sub_cv(f, a->format, a->type);   // the data is converted to the texture's format
            size_t pitch; void *conv = pixels_convert(a->pixels, a->w, a->h, a->format, a->type, f, cv, &pitch);
            if (!t->made) {   // into a level not made yet: patch the pending copy
                level_t *l = &t->pend[face][a->level < MAXLV ? a->level : 0];
                if (l->data && a->x + a->w <= (GLint)l->w && a->y + a->h <= (GLint)l->h && g_fmt[f].kind != K_BLOCK)
                    for (GLsizei r = 0; r < a->h; r++) memcpy((uint8_t *)l->data + (a->y + r) * l->pitch + (size_t)a->x * g_fmt[f].bytes, (uint8_t *)conv + r * pitch, (size_t)a->w * g_fmt[f].bytes);
                else tex_make(t, 0);
            }
            if (t->made && t->tex && a->level < t->levels) tex_write(t, face, a->level, a->x, a->y, 0, a->w, a->h, conv, pitch);
            free(conv);
            return;
        }
        int cv, f = fmt_pick(a->ifmt, a->format, a->type, &cv);
        if (f < 0) { static GLint seen[16]; static int n; int k = 0; for (int i = 0; i < n; i++) k |= seen[i] == a->ifmt; if (!k && n < 16) { seen[n++] = a->ifmt; GLOG("[RED] texture internal format %#x (format %#x, type %#x) not mapped", a->ifmt, a->format, a->type); } return; }
        if (cv == CV_NONE && a->pixels) { RED1("texture data of format %#x type %#x into %s not converted (zeros)", a->format, a->type, g_fmt[f].name); }
        if (cv == CV_INDEX && !S.color_table) RED1("colour-index texture data with GL_COLOR_TABLE disabled");
        size_t pitch; void *conv;
        if (a->compressed) {
            size_t need = fmt_pitch(f, a->w) * fmt_rows(f, a->h);
            if (g_fmt[f].kind != K_BLOCK || (size_t)a->size < need) { RED1("glCompressedTexImage2D: %d bytes for %dx%d %s (%zu expected)", a->size, a->w, a->h, g_fmt[f].name, need); return; }
            conv = pixels_convert(a->pixels, a->w, a->h, 0, 0, f, CV_COPY, &pitch);
        } else conv = pixels_convert(cv == CV_NONE ? NULL : a->pixels, a->w, a->h, a->format, a->type, f, cv, &pitch);
        tex_image(a->target, a->level, a->ifmt, f, cv, a->w, a->h, 1, a->pixels || a->compressed ? conv : (free(conv), (void *)NULL), pitch);
    }
}
GL(glTexImage2D, void, GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void *pixels) {
    (void)border; teximg_args a = {target, level, ifmt, 0, 0, w, h, format, type, pixels, 0, 0, 0}; ((void (*)(const teximg_args *))(const void *)teximg_impl_bs)(&a);
}
GL(glTexSubImage2D, void, GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *pixels) {
    teximg_args a = {target, level, 0, x, y, w, h, format, type, pixels, 0, 1, 0}; ((void (*)(const teximg_args *))(const void *)teximg_impl_bs)(&a);
}
GL(glCompressedTexImage2D, void, GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h, GLint border, GLsizei size, const void *data) {
    (void)border; teximg_args a = {target, level, (GLint)ifmt, 0, 0, w, h, 0, 0, data, size, 0, 1}; ((void (*)(const teximg_args *))(const void *)teximg_impl_bs)(&a);
}
// glTexStorage2D / 3D: the format, the size and the level count, before any data (LÖVE makes every texture so, the mipmapped ones with all their levels, which glGenerateMipmap
// fills). The Metal texture is made at its first upload, attachment or sampling, as one glTexImage2D gives (tex_make: shared, or private for a render target)
static void tex_storage(GLenum target, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h, GLsizei d) {
    tex_t *t = bound_tex(target);
    int cv, f = fmt_pick((GLint)ifmt, 0, 0, &cv);
    if (!t || f < 0 || levels <= 0 || w <= 0 || h <= 0 || d <= 0) { RED1("glTexStorage(%#x, %d levels, %#x, %dx%dx%d) not implemented", target, levels, ifmt, w, h, d); return; }
    if (t->made || t->defined) tex_drop(t);
    t->fmt = f; t->ifmt = ifmt; t->w = w; t->h = h; t->d = d; t->slevels = levels; t->smp_dirty = 1;
}
// One scalar of pixel-transfer data. Integer colour/depth components are normalized; stencil indices are integers. memcpy permits unaligned guest data.
static double pixel_scalar(const uint8_t *p, GLenum type, int normalized) {
    switch (type) {
        case GL_UNSIGNED_BYTE: return normalized ? *p / 255.0 : *p;
        case GL_BYTE: { int8_t v; memcpy(&v, p, 1); return normalized ? fmax(-1, v / 127.0) : v; }
        case GL_UNSIGNED_SHORT: { uint16_t v; memcpy(&v, p, 2); return normalized ? v / 65535.0 : v; }
        case GL_SHORT: { int16_t v; memcpy(&v, p, 2); return normalized ? fmax(-1, v / 32767.0) : v; }
        case GL_UNSIGNED_INT: { uint32_t v; memcpy(&v, p, 4); return normalized ? v / 4294967295.0 : v; }
        case GL_INT: { int32_t v; memcpy(&v, p, 4); return normalized ? fmax(-1, v / 2147483647.0) : v; }
        case GL_HALF_FLOAT: { _Float16 v; memcpy(&v, p, 2); return (double)v; }
        case GL_FLOAT: { float v; memcpy(&v, p, 4); return v; }
    }
    return 0;
}
static uint32_t pixel_unorm(double v, uint32_t max) { return (uint32_t)llround(fmax(0, fmin(1, v)) * max); }
// An unsigned floating component (R11F_G11F_B10F): five exponent bits, six or five fraction bits, including subnormal and non-finite values.
static uint32_t pixel_ufloat(double v, int bits) {
    if (isnan(v)) return (31u << bits) | 1;
    if (v <= 0) return 0;
    if (isinf(v)) return 31u << bits;
    int exp; double frac = frexp(v, &exp); exp += 14;
    if (exp <= 0) return (uint32_t)llrint(ldexp(v, bits + 14));
    uint32_t m = (uint32_t)llrint(ldexp(frac, bits + 1));
    if (m == 2u << bits) { m >>= 1; exp++; }
    return exp >= 31 ? 31u << bits : ((uint32_t)exp << bits) | (m - (1u << bits));
}
// Convert one pixel into the actual texture format. The caller handles any unpack state and provides room for a destination texel.
static GLenum convert_pixel(int f, GLenum format, GLenum type, const void *data, uint8_t *out) {
    int n;
    switch (format) {
        case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA: case GL_DEPTH_COMPONENT: case GL_STENCIL_INDEX: n = 1; break;
        case GL_RG: case GL_DEPTH_STENCIL: n = 2; break;
        case GL_RGB: case 0x80e0 /* BGR */: n = 3; break;
        case GL_RGBA: case GL_BGRA: n = 4; break;
        default: return GL_INVALID_ENUM;
    }
    if ((g_fmt[f].kind == K_DEPTH) != (format == GL_DEPTH_COMPONENT) || (f == F_D32FS8) != (format == GL_DEPTH_STENCIL) ||
        (f == F_S8) != (format == GL_STENCIL_INDEX)) return GL_INVALID_OPERATION;
    static const struct { GLenum type; uint8_t n, bytes, bits[4], rev; } packed[] = {
        {0x8032, 3, 1, {3, 3, 2}, 0}, {0x8362, 3, 1, {3, 3, 2}, 1},
        {GL_UNSIGNED_SHORT_5_6_5, 3, 2, {5, 6, 5}, 0}, {0x8364, 3, 2, {5, 6, 5}, 1},
        {GL_UNSIGNED_SHORT_4_4_4_4, 4, 2, {4, 4, 4, 4}, 0}, {0x8365, 4, 2, {4, 4, 4, 4}, 1},
        {GL_UNSIGNED_SHORT_5_5_5_1, 4, 2, {5, 5, 5, 1}, 0}, {0x8366, 4, 2, {5, 5, 5, 1}, 1},
        {0x8035, 4, 4, {8, 8, 8, 8}, 0}, {0x8367, 4, 4, {8, 8, 8, 8}, 1},
        {0x8036, 4, 4, {10, 10, 10, 2}, 0}, {GL_UNSIGNED_INT_2_10_10_10_REV, 4, 4, {10, 10, 10, 2}, 1},
    };
    int pk = -1, bytes = 0;
    for (unsigned i = 0; i < sizeof packed / sizeof *packed; i++) if (packed[i].type == type) pk = (int)i;
    if (pk >= 0) { if (n != packed[pk].n || format == GL_DEPTH_STENCIL) return GL_INVALID_OPERATION; }
    else {
        switch (type) {
            case GL_BYTE: case GL_UNSIGNED_BYTE: bytes = 1; break;
            case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: bytes = 2; break;
            case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: bytes = 4; break;
            case GL_UNSIGNED_INT_24_8: case 0x8dad /* FLOAT_32_UNSIGNED_INT_24_8_REV */: if (format != GL_DEPTH_STENCIL) return GL_INVALID_OPERATION; break;
            case GL_UNSIGNED_INT_10F_11F_11F_REV: case 0x8c3e /* UNSIGNED_INT_5_9_9_9_REV */: if (n != 3) return GL_INVALID_OPERATION; break;
            default: return GL_INVALID_ENUM;
        }
    }
    if (format == GL_DEPTH_STENCIL && type != GL_UNSIGNED_INT_24_8 && type != 0x8dad) return GL_INVALID_OPERATION;
    memset(out, 0, g_fmt[f].bytes); if (!data) return GL_NO_ERROR;
    double c[4] = {0, 0, 0, 1}; uint32_t stencil = 0;
    if (format == GL_DEPTH_STENCIL) {
        uint32_t v; memcpy(&v, data, 4);
        if (type == GL_UNSIGNED_INT_24_8) { c[0] = (v >> 8) / 16777215.0; stencil = v & 255; }
        else { float d; memcpy(&d, data, 4); c[0] = d; memcpy(&stencil, (const uint8_t *)data + 4, 4); stencil &= 255; }
    } else if (pk >= 0) {
        uint32_t v = 0; memcpy(&v, data, packed[pk].bytes); int off = packed[pk].rev ? 0 : packed[pk].bytes * 8;
        for (int i = 0; i < n; i++) { int b = packed[pk].bits[i]; if (!packed[pk].rev) off -= b; c[i] = (double)(v >> off & ((1u << b) - 1)) / ((1u << b) - 1); if (packed[pk].rev) off += b; }
    } else if (type == GL_UNSIGNED_INT_10F_11F_11F_REV || type == 0x8c3e) {
        uint32_t v; memcpy(&v, data, 4);
        if (type == 0x8c3e) for (int i = 0; i < 3; i++) c[i] = ldexp(v >> (i * 9) & 511, (int)(v >> 27) - 24);
        else for (int i = 0; i < 3; i++) { int b = i == 2 ? 10 : 11; uint16_t h = (uint16_t)((v >> (i * 11) & ((1u << b) - 1)) << (15 - b)); _Float16 a; memcpy(&a, &h, 2); c[i] = (double)a; }
    } else for (int i = 0; i < n; i++) c[i] = pixel_scalar((const uint8_t *)data + i * bytes, type, format != GL_STENCIL_INDEX);
    if (format == GL_STENCIL_INDEX) stencil = (uint32_t)(int64_t)c[0] & 255;
    if (format == GL_GREEN) { c[1] = c[0]; c[0] = 0; } else if (format == GL_BLUE) { c[2] = c[0]; c[0] = 0; }
    else if (format == GL_ALPHA) { c[3] = c[0]; c[0] = 0; }
    else if (format == GL_BGRA || format == 0x80e0) { double keep = c[0]; c[0] = c[2]; c[2] = keep; }
    if (f == F_BGRA8) { double keep = c[0]; c[0] = c[2]; c[2] = keep; }
    switch (f) {
        case F_RGBA8: case F_BGRA8: case F_SRGBA8: for (int i = 0; i < 4; i++) out[i] = (uint8_t)pixel_unorm(c[i], 255); break;
        case F_R8: case F_L8: out[0] = (uint8_t)pixel_unorm(c[0], 255); break;
        case F_A8: out[0] = (uint8_t)pixel_unorm(c[3], 255); break;
        case F_RG8: case F_LA8: out[0] = (uint8_t)pixel_unorm(c[0], 255); out[1] = (uint8_t)pixel_unorm(c[f == F_LA8 ? 3 : 1], 255); break;
        case F_RGBA16F: case F_R16F: case F_RG16F:
            for (int i = 0; i < g_fmt[f].bytes / 2; i++) { _Float16 v = (_Float16)c[i]; memcpy(out + 2 * i, &v, 2); } break;
        case F_RGBA32F: case F_R32F: case F_RG32F:
            for (int i = 0; i < g_fmt[f].bytes / 4; i++) { float v = (float)c[i]; memcpy(out + 4 * i, &v, 4); } break;
        case F_RGBA16: for (int i = 0; i < 4; i++) { uint16_t v = (uint16_t)pixel_unorm(c[i], 65535); memcpy(out + 2 * i, &v, 2); } break;
        case F_RGB10A2: { uint32_t v = pixel_unorm(c[0], 1023) | pixel_unorm(c[1], 1023) << 10 | pixel_unorm(c[2], 1023) << 20 | pixel_unorm(c[3], 3) << 30; memcpy(out, &v, 4); break; }
        case F_RG11B10F: { uint32_t v = pixel_ufloat(c[0], 6) | pixel_ufloat(c[1], 6) << 11 | pixel_ufloat(c[2], 5) << 22; memcpy(out, &v, 4); break; }
        case F_D16: { uint16_t v = (uint16_t)pixel_unorm(c[0], 65535); memcpy(out, &v, 2); break; }
        case F_D32F: case F_D32FS8: { float v = (float)fmax(0, fmin(1, c[0])); memcpy(out, &v, 4); if (f == F_D32FS8) memcpy(out + 4, &stencil, 4); break; }
        case F_S8: out[0] = (uint8_t)stencil; break;
        default: return GL_INVALID_OPERATION;
    }
    return GL_NO_ERROR;
}
// glClearTexImage (GL 4.4): convert one value, then fill every face, layer and slice in command order. NULL data means storage zeroes, including alpha.
GLB(glClearTexImage, void, GLuint name, GLint level, GLenum format, GLenum type, const void *data) {
    tex_t *t = tex_of(name);
    if (!t || level < 0 || level >= MAXLV) { gl_error(GL_INVALID_OPERATION); return; }
    if (t->fmt < 0 || g_fmt[t->fmt].kind == K_BLOCK) { gl_error(GL_INVALID_OPERATION); return; }
    uint8_t one[16]; GLenum error = convert_pixel(t->fmt, format, type, data, one);
    if (error) { gl_error(error); return; }
    @autoreleasepool {
        if (g_pend_clear.bits) clears_flush();
        if (!t->made) tex_make(t, 0);
        if (!t->tex || level >= t->levels) { gl_error(GL_INVALID_OPERATION); return; }
        int f = t->fmt; size_t bytes = g_fmt[f].bytes;
        uint32_t w = umax(1, t->w >> level), h = umax(1, (t->target == GL_TEXTURE_CUBE_MAP ? t->w : t->h) >> level);
        uint32_t slices = t->target == GL_TEXTURE_3D ? umax(1, t->d >> level) : 1, layers = t->target == GL_TEXTURE_2D_ARRAY ? t->d : (uint32_t)tex_faces(t);
        size_t lp = fmt_pitch(f, w); uint8_t *img = zalloc(lp * h);
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) memcpy(img + y * lp + x * bytes, one, bytes);
        for (uint32_t k = 0; k < layers; k++) for (uint32_t z = 0; z < slices; z++) tex_write(t, (int)k, level, 0, 0, z, w, h, img, lp);
        free(img);
    }
}
GL(glTexStorage2D, void, GLenum target, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h) { tex_storage(target, levels, ifmt, w, h, 1); }
GL(glTexStorage3D, void, GLenum target, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h, GLsizei d) { tex_storage(target, levels, ifmt, w, h, d); }
// 3D and array textures (LÖVE makes a 1x1x1 one of each at start for its default textures): data through glTexSubImage3D, a layer (or a 3D slice) at a time.
// glTexSubImage3D's last three arguments are on the stack: 64-bit here, so the Apple and AAPCS64 stack layouts agree
typedef struct { GLenum target; GLint level, x, y, z; GLsizei w, h, d; GLenum format, type; const void *pixels; } tex3d_args;
static void tex3d_impl(const tex3d_args *a) __attribute__((used));
BIGSTUB(tex3d_impl);
static void tex3d_impl(const tex3d_args *a) {
    @autoreleasepool {
        tex_t *t = bound_tex(a->target);
        if (!t || t->fmt < 0 || a->w <= 0 || a->h <= 0 || a->d <= 0 || !a->pixels) return;
        if (!t->made) tex_make(t, 0);
        if (!t->tex || a->level >= t->levels) return;
        int f = t->fmt, cv = f == F_RGBA8 && a->format == GL_RGB ? CV_RGB8 : CV_COPY;
        size_t pitch; void *conv = pixels_convert(a->pixels, a->w, a->h * a->d, a->format, a->type, f, cv, &pitch);   // the layers one after the other
        size_t layer = pitch * fmt_rows(f, a->h);
        for (GLsizei k = 0; k < a->d; k++) {
            if (t->target == GL_TEXTURE_3D) tex_write(t, 0, a->level, a->x, a->y, a->z + k, a->w, a->h, (uint8_t *)conv + k * layer, pitch);
            else tex_write(t, a->z + k, a->level, a->x, a->y, 0, a->w, a->h, (uint8_t *)conv + k * layer, pitch);
        }
        free(conv);
    }
}
GL(glTexSubImage3D, void, GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, uint64_t format, uint64_t type, const void *pixels) {
    tex3d_args a = {target, level, x, y, z, w, h, d, (GLenum)format, (GLenum)type, pixels}; ((void (*)(const tex3d_args *))(const void *)tex3d_impl_bs)(&a);
}
GL(glTexImage3D, void, GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLsizei d, GLint border, GLenum format, uint64_t type, const void *pixels) {
    (void)border;
    if (level) { RED1("glTexImage3D of level %d not implemented", level); return; }
    tex_storage(target, 1, (GLenum)ifmt, w, h, d);
    if (pixels) h_glTexSubImage3D(target, 0, 0, 0, 0, w, h, d, format, type, pixels);
}
// glCompressedTexImage3D / glCompressedTexSubImage3D into 2D arrays, and glCompressedTexSubImage2D into a 2D texture or a cube face (its image calls go through teximg_impl):
// each layer has its own rounded block rows, even when a mip is smaller than one block. Their last argument(s) are on the stack (64-bit here, as glTexSubImage3D's), so they pack
// them before the big-stack entry. Other 3D targets (3D, cube-map arrays) and compressed formats the layer does not map are the host's gaps: a red line, no GL error (see g_gl_error)
typedef struct { GLenum target, format; GLint level, x, y, z, border; GLsizei w, h, d, size; const void *data; int sub, dims; } compressed_args;
static void compressed_impl(const compressed_args *a) __attribute__((used));
BIGSTUB(compressed_impl);
static void compressed_impl(const compressed_args *a) {
    @autoreleasepool {
        const char *sub = a->sub ? "Sub" : "";
        int arr = a->target == GL_TEXTURE_2D_ARRAY, cube = a->target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && a->target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
        if (a->dims == 3 && !arr) { RED1("glCompressedTex%sImage3D target %#x not implemented (2D arrays only)", sub, a->target); return; }
        if (a->dims == 2 && a->target != GL_TEXTURE_2D && !cube) { gl_error(GL_INVALID_ENUM); return; }
        int face = cube ? (int)(a->target - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;   // the pending level's face (an array keeps all its layers in face 0)
        if (a->level < 0 || a->level >= MAXLV || a->w < 0 || a->h < 0 || a->d < 0 || a->size < 0 || a->border ||
            (uint32_t)a->w > (16384u >> a->level) || (uint32_t)a->h > (16384u >> a->level) || a->d > 2048) { gl_error(GL_INVALID_VALUE); return; }
        int cv, f = fmt_pick(a->format, 0, 0, &cv);
        if (f < 0) { RED1("glCompressedTex%sImage%dD format %#x not mapped", sub, a->dims, a->format); return; }
        if (g_fmt[f].kind != K_BLOCK) { gl_error(GL_INVALID_ENUM); return; }   // not a compressed format
        size_t pitch = fmt_pitch(f, a->w), rows = fmt_rows(f, a->h);
        if (rows && pitch > SIZE_MAX / rows) { gl_error(GL_INVALID_VALUE); return; }
        size_t layer = pitch * rows;
        if (a->d && layer > SIZE_MAX / (uint32_t)a->d) { gl_error(GL_INVALID_VALUE); return; }
        // the byte count must match the data given; an allocation with no data has no bytes to count, whatever size it names
        if (a->data && layer * a->d != (size_t)a->size) { gl_error(GL_INVALID_VALUE); return; }
        tex_t *t = bound_tex(a->target);
        if (!t) { gl_error(GL_INVALID_OPERATION); return; }
        if (!a->sub) {
            if (t->slevels) { gl_error(GL_INVALID_OPERATION); return; }
            // All levels in a complete array have the same layer count.
            if (a->level && (t->made || t->defined) && (t->d != (uint32_t)a->d || t->fmt != f ||
                umax(1, t->w >> a->level) != (uint32_t)a->w || umax(1, t->h >> a->level) != (uint32_t)a->h)) { gl_error(GL_INVALID_OPERATION); return; }
            void *copy = NULL;
            if (a->data && a->size) { copy = zalloc((size_t)a->size); memcpy(copy, a->data, (size_t)a->size); }
            tex_image(a->target, a->level, a->format, f, CV_COPY, a->w, a->h, a->d, copy, pitch);
            return;
        }
        level_t *l = &t->pend[face][a->level];
        int have = t->slevels ? a->level < t->slevels : (t->defined >> a->level & 1) || l->w || (t->made && a->level < t->levels);   // a cube face may be given before the others
        if (t->fmt != f || !have) { gl_error(GL_INVALID_OPERATION); return; }
        uint32_t w = umax(1, t->w >> a->level), h = umax(1, t->h >> a->level);
        if (a->x < 0 || a->y < 0 || a->z < 0 || (uint64_t)a->x + a->w > w || (uint64_t)a->y + a->h > h || (uint64_t)a->z + a->d > (arr ? t->d : 1)) { gl_error(GL_INVALID_VALUE); return; }
        const fmt_t *m = &g_fmt[f];
        if (a->x % m->bw || a->y % m->bh || (a->w % m->bw && a->x + a->w != w) || (a->h % m->bh && a->y + a->h != h)) { gl_error(GL_INVALID_OPERATION); return; }
        if (!a->w || !a->h || !a->d) return;
        if (!a->data) { gl_error(GL_INVALID_VALUE); return; }
        // Before the Metal texture is made, the bytes go into the level's pending copy: making it here would fix its level count at the levels given so far, and a
        // level given afterwards (allocate and fill level 0, then level 1, ...) would be dropped. glTexStorage storage has all its levels: made now
        if (!t->made && !t->slevels && l->w == w && l->h == h) {
            if (!l->data) l->data = zalloc(l->size);
            size_t ll = l->pitch * fmt_rows(f, h), off = (size_t)(a->y / m->bh) * l->pitch + (size_t)(a->x / m->bw) * m->bytes;
            for (GLsizei k = 0; k < a->d; k++) for (size_t r = 0; r < rows; r++)
                memcpy((uint8_t *)l->data + (size_t)(a->z + k) * ll + off + r * l->pitch, (const uint8_t *)a->data + k * layer + r * pitch, pitch);
            return;
        }
        if (!t->made && !tex_make(t, 0)) return;
        if (!t->tex || a->level >= t->levels) { gl_error(GL_INVALID_OPERATION); return; }
        for (GLsizei k = 0; k < a->d; k++)
            tex_write(t, arr ? a->z + k : face, a->level, a->x, a->y, 0, a->w, a->h, (const uint8_t *)a->data + k * layer, pitch);
    }
}
static void compressed_call(compressed_args a) { ((void (*)(const compressed_args *))(const void *)compressed_impl_bs)(&a); }
GL(glCompressedTexImage3D, void, GLenum target, GLint level, GLenum format, GLsizei w, GLsizei h, GLsizei d, GLint border, GLsizei size, const void *data) {
    compressed_call((compressed_args){target, format, level, 0, 0, 0, border, w, h, d, size, data, 0, 3});
}
GL(glCompressedTexSubImage3D, void, GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, uint64_t format, uint64_t size, const void *data) {
    compressed_call((compressed_args){target, (GLenum)format, level, x, y, z, 0, w, h, d, (GLsizei)size, data, 1, 3});
}
// DOOM 3 streams its DXT images a part at a time (glCompressedTexSubImage2DARB)
GL(glCompressedTexSubImage2D, void, GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLsizei size, const void *data) {
    compressed_call((compressed_args){target, format, level, x, y, 0, 0, w, h, 1, size, data, 1, 2});
}
// Mipmaps from level 0, in command order (after the draws that rendered level 0 this frame): a texture of one level gets its full chain first, level 0 copied over
static void mip_impl(GLenum target) __attribute__((used));
BIGSTUB(mip_impl);
static void mip_impl(GLenum target) {
    @autoreleasepool {
        tex_t *t = bound_tex(target);
        if (!t || (!t->made && !tex_make(t, 0)) || !t->tex) return;
        if (g_fmt[t->fmt].kind != K_COL) { RED1("glGenerateMipmap of a %s texture not implemented", g_fmt[t->fmt].name); return; }
        id<MTLCommandBuffer> cb;
        if (t->use >= g_serial) { pass_end(); cb = cb_get(); } else { cb = [g_q commandBuffer]; cb.label = @"gl mipmaps"; }
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        if (t->levels == 1 && umax(t->w, t->h) > 1) {
            id<MTLTexture> old = t->tex; t->made = 0; t->tex = nil; t->defined = 0; t->slevels = MAXLV;
            tex_make(t, old.storageMode == MTLStorageModePrivate);
            if (!t->tex) { [b endEncoding]; return; }
            for (int f = 0; f < tex_faces(t); f++) [b copyFromTexture:old sourceSlice:f sourceLevel:0 toTexture:t->tex destinationSlice:f destinationLevel:0 sliceCount:1 levelCount:1];
            t->gen++;
        }
        if (t->levels > 1) [b generateMipmapsForTexture:t->tex];
        [b endEncoding]; g_n_blit++; t->use = g_serial;
        if (cb != g_cb) { dm_gfx_enter(); cb_commit_one(cb); dm_gfx_leave(); }
    }
}
GL(glGenerateMipmap, void, GLenum target) { ((void (*)(GLenum))(const void *)mip_impl_bs)(target); }

// ---------- Framebuffers and renderbuffers ----------
static void fb_init(GLuint name) { fb_t *f = fb_of(name); f->draw[0] = f->read = 1; }
GL(glGenFramebuffers, void, GLsizei n, GLuint *out) {
    for (GLsizei i = 0; i < n; i++) { out[i] = name_new((void ***)&g_fbs, &g_nfbs, &g_capfbs, sizeof(fb_t)); fb_init(out[i]); }
}
GLB(glDeleteFramebuffers, void, GLsizei n, const GLuint *f) {
    for (GLsizei i = 0; i < n; i++) {
        fb_t *x = fb_of(f[i]); if (!x) continue;
        if (P.open && P.fb == f[i]) pass_end();
        if (g_pend_clear.fb == f[i]) { g_pend_clear.bits = 0; g_pend_clear.colors = 0; }
        free(x); g_fbs[f[i]] = NULL;
        if (S.fb == f[i]) S.fb = 0;
        if (S.read_fb == f[i]) S.read_fb = 0;
    }
}
GLB(glBindFramebuffer, void, GLenum target, GLuint f) {
    if (f && !fb_of(f)) {   // a name glGenFramebuffers never gave: made at its first bind, as GL ES and the compatibility profile do (EXT_framebuffer_object, OES_framebuffer_object)
        if (f > 1u << 20) { RED1("glBindFramebuffer of name %u", f); return; }
        name_at((void ***)&g_fbs, &g_nfbs, &g_capfbs, f, sizeof(fb_t));
        fb_init(f);
    }
    if (target != GL_DRAW_FRAMEBUFFER) S.read_fb = f;   // GL_FRAMEBUFFER binds both
    if (target == GL_READ_FRAMEBUFFER || f == S.fb) return;
    { static int on = -1; if (on < 0) on = getenv("DM_GL_FBLOG") != NULL; if (on) { uint32_t bw, bh; fb_size(f, &bw, &bh); GLOG("bind framebuffer %u %ux%u (was %u) after %llu draws", f, bw, bh, S.fb, (unsigned long long)g_n_draw); } }
    if (g_pend_clear.bits && g_pend_clear.fb == S.fb) clears_flush();
    S.fb = f;   // the pass ends at the next draw or clear into another framebuffer (pass_ensure)
}
GL(glCheckFramebufferStatus, GLenum, GLenum target) { (void)target; return GL_FRAMEBUFFER_COMPLETE; }
static int color_index(GLuint fb, GLenum buffer) {
    if (!buffer) return 0;   // GL_NONE
    if (fb) return buffer >= GL_COLOR_ATTACHMENT0 && buffer < GL_COLOR_ATTACHMENT0 + NGL_COLORS ? (int)(buffer - GL_COLOR_ATTACHMENT0 + 1) : -1;
    return buffer == GL_BACK || buffer == GL_FRONT || buffer == 0x400 || buffer == 0x402 ? 1 : -1;
}
GLB(glDrawBuffers, void, GLsizei n, const GLenum *b) {
    if (n < 0 || n > NGL_COLORS) { gl_error(GL_INVALID_VALUE); return; }
    uint8_t draw[NGL_COLORS] = {0}; unsigned used = 0;
    for (GLsizei i = 0; i < n; i++) {
        int index = color_index(S.fb, b[i]);
        if (index < 0 || (index && ((!S.fb && i) || (used & (1u << index))))) { gl_error(GL_INVALID_OPERATION); return; }
        draw[i] = (uint8_t)index; used |= 1u << index;
    }
    fb_t *f = fb_of(S.fb);
    if (f ? !memcmp(f->draw, draw, sizeof draw) : S.default_draw == draw[0]) return;
    if (g_pend_clear.fb == S.fb) clears_flush();
    if (P.open && P.fb == S.fb) pass_end();
    if (f) { memcpy(f->draw, draw, sizeof draw); f->gen++; }
    else S.default_draw = draw[0];
}
GLB(glDrawBuffer, void, GLenum b) { h_glDrawBuffers(1, &b); }
GL(glReadBuffer, void, GLenum b) {
    int index = color_index(S.read_fb, b);
    if (index < 0) { gl_error(GL_INVALID_OPERATION); return; }
    fb_t *f = fb_of(S.read_fb);
    if (f) f->read = (uint8_t)index;
    else S.default_read = (uint8_t)index;
}
GL(glInvalidateFramebuffer, void, GLenum target, GLsizei n, const GLenum *att) { (void)target; (void)n; (void)att; }   // the contents are kept (a store action)
GL(glDiscardFramebufferEXT, void, GLenum target, GLsizei n, const GLenum *att) { (void)target; (void)n; (void)att; }
GL(glGenRenderbuffers, void, GLsizei n, GLuint *out) { for (GLsizei i = 0; i < n; i++) out[i] = name_new((void ***)&g_rbs, &g_nrbs, &g_caprbs, sizeof(rb_t)); }
GL(glDeleteRenderbuffers, void, GLsizei n, const GLuint *r) { for (GLsizei i = 0; i < n; i++) { rb_t *x = rb_of(r[i]); if (!x) continue; x->tex = x->linear_rt = nil; free(x); g_rbs[r[i]] = NULL; } }
GL(glBindRenderbuffer, void, GLenum target, GLuint r) {   // a name glGenRenderbuffers never gave is made at its first bind, as glBindFramebuffer's
    (void)target;
    if (r && !rb_of(r)) { if (r > 1u << 20) { RED1("glBindRenderbuffer of name %u", r); return; } name_at((void ***)&g_rbs, &g_nrbs, &g_caprbs, r, sizeof(rb_t)); }
    S.rb = r;
}
static void h_glRenderbufferStorage(GLenum target, GLenum ifmt, GLsizei w, GLsizei h);
GLB(glRenderbufferStorageMultisample, void, GLenum target, GLsizei samples, GLenum ifmt, GLsizei w, GLsizei h) {
    if (samples > 1) RED1("multisampled renderbuffer (%d samples): made with one sample", samples);
    h_glRenderbufferStorage(target, ifmt, w, h);
}
GL(glGetRenderbufferParameteriv, void, GLenum target, GLenum p, GLint *v) {
    (void)target; rb_t *r = rb_of(S.rb);
    switch (p) {
        case GL_RENDERBUFFER_SAMPLES: *v = 0; return;
        case GL_RENDERBUFFER_WIDTH: *v = r ? (GLint)r->w : 0; return;
        case GL_RENDERBUFFER_HEIGHT: *v = r ? (GLint)r->h : 0; return;
    }
    RED1("glGetRenderbufferParameteriv(%#x) not implemented", p); *v = 0;
}
GLB(glRenderbufferStorage, void, GLenum target, GLenum ifmt, GLsizei w, GLsizei h) {
    (void)target; rb_t *r = rb_of(S.rb);
    int cv, f = fmt_pick((GLint)ifmt, GL_RGBA, GL_UNSIGNED_BYTE, &cv);
    if (!r || f < 0 || w <= 0 || h <= 0) { RED1("glRenderbufferStorage(%#x, %dx%d) not implemented", ifmt, w, h); return; }
    @autoreleasepool {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:g_fmt[f].pf width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
        r->linear_rt = nil; r->tex = [g_dev newTextureWithDescriptor:td]; r->fmt = f; r->w = w; r->h = h; r->gen++;
    }
    OLOG("renderbuffer %u: %dx%d %s", S.rb, w, h, g_fmt[f].name);
}
static int att_same(att_t a, att_t b) { return a.kind == b.kind && a.name == b.name && a.level == b.level && a.face == b.face; }
static void fb_attach(GLenum target, GLenum attachment, int kind, GLuint name, int level, int face) {
    GLuint fb = target == GL_READ_FRAMEBUFFER ? S.read_fb : S.fb;
    fb_t *f = fb_of(fb);
    if (!f) { RED1("attachment to framebuffer 0 or to one never generated"); return; }
    int both = attachment == GL_DEPTH_STENCIL_ATTACHMENT;
    int col = color_index(fb, attachment);
    att_t *slot = col > 0 ? &f->col[col - 1] : attachment == GL_STENCIL_ATTACHMENT ? &f->stencil :
                  attachment == GL_DEPTH_ATTACHMENT || both ? &f->depth : NULL;
    if (!slot) { RED1("framebuffer attachment %#x not implemented", attachment); return; }
    att_t a = name ? (att_t){kind, name, level, face} : (att_t){0};
    if (att_same(*slot, a) && (!both || att_same(f->stencil, a))) return;
    // A pending clear addresses the images that were attached at glClear, even when the framebuffer name stays bound.
    if (g_pend_clear.bits && g_pend_clear.fb == fb) clears_flush();
    *slot = a;
    if (both) f->stencil = a;
    f->gen++;
}
GLB(glFramebufferTexture2D, void, GLenum target, GLenum att, GLenum textarget, GLuint t, GLint level) {
    int face = textarget >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && textarget <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? (int)(textarget - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
    tex_t *x = tex_of(t); if (x && !x->target) tex_init_params(x, textarget == GL_TEXTURE_2D ? GL_TEXTURE_2D : GL_TEXTURE_CUBE_MAP);
    fb_attach(target, att, 1, t, level, face);
}
// A layer of an array texture or a slice of a 3D one
GLB(glFramebufferTextureLayer, void, GLenum target, GLenum att, GLuint t, GLint level, GLint layer) { fb_attach(target, att, 1, t, level, layer); }
GLB(glFramebufferTexture3D, void, GLenum target, GLenum att, GLenum textarget, GLuint t, GLint level, GLint z) { (void)textarget; fb_attach(target, att, 1, t, level, z); }
GLB(glFramebufferRenderbuffer, void, GLenum target, GLenum att, GLenum rbt, GLuint r) { (void)rbt; fb_attach(target, att, 2, r, 0, 0); }

GL(glGetFramebufferAttachmentParameteriv, void, GLenum target, GLenum attachment, GLenum p, GLint *v) {
    if (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER && target != GL_READ_FRAMEBUFFER) { gl_error(GL_INVALID_ENUM); return; }
    GLuint fb = target == GL_READ_FRAMEBUFFER ? S.read_fb : S.fb;
    int format = -1, kind = 0, stencil = attachment == GL_STENCIL || attachment == GL_STENCIL_ATTACHMENT;
    const att_t *a = NULL;
    if (!fb) {
        kind = 0x8218;   // FRAMEBUFFER_DEFAULT
        if (attachment == GL_DEPTH || attachment == GL_STENCIL) format = F_D32FS8;
        else if (attachment == GL_FRONT || attachment == GL_BACK || attachment == 0x400 || attachment == 0x402) format = F_SRGBA8;
        else { gl_error(GL_INVALID_ENUM); return; }
    } else {
        fb_t *f = fb_of(fb);
        if (!f) { gl_error(GL_INVALID_OPERATION); return; }
        int col = color_index(fb, attachment);
        if (col > 0) a = &f->col[col - 1];
        else if (attachment == GL_DEPTH_ATTACHMENT) a = &f->depth;
        else if (attachment == GL_STENCIL_ATTACHMENT) a = &f->stencil;
        else if (attachment == GL_DEPTH_STENCIL_ATTACHMENT && att_same(f->depth, f->stencil)) a = &f->depth;
        else { gl_error(attachment == GL_DEPTH_STENCIL_ATTACHMENT ? GL_INVALID_OPERATION : GL_INVALID_ENUM); return; }
        if (a->kind == 1) { tex_t *t = tex_of(a->name); if (t) { kind = 0x1702; format = t->fmt; } }   // TEXTURE
        else if (a->kind == 2) { rb_t *r = rb_of(a->name); if (r) { kind = GL_RENDERBUFFER; format = r->fmt; } }
    }
    static const uint8_t bits[F_N][6] = {
        [F_RGBA8] = {8, 8, 8, 8}, [F_BGRA8] = {8, 8, 8, 8}, [F_SRGBA8] = {8, 8, 8, 8}, [F_R8] = {8}, [F_RG8] = {8, 8},
        [F_RGBA16F] = {16, 16, 16, 16}, [F_RGBA32F] = {32, 32, 32, 32}, [F_R16F] = {16}, [F_RG16F] = {16, 16},
        [F_R32F] = {32}, [F_RG32F] = {32, 32}, [F_RG11B10F] = {11, 11, 10}, [F_RGB10A2] = {10, 10, 10, 2}, [F_RGBA16] = {16, 16, 16, 16},
        [F_D16] = {0, 0, 0, 0, 16}, [F_D32F] = {0, 0, 0, 0, 32}, [F_D32FS8] = {0, 0, 0, 0, 32, 8}, [F_S8] = {0, 0, 0, 0, 0, 8},
        [F_R16UI] = {16}, [F_RG32UI] = {32, 32}, [F_RGBA32UI] = {32, 32, 32, 32},
    };
    if (p == 0x8cd0) { *v = kind; return; }   // OBJECT_TYPE
    if (p == 0x8cd1 && a) { *v = kind ? (GLint)a->name : 0; return; }   // OBJECT_NAME
    if (p == 0x8cd2 && kind == 0x1702) { *v = a->level; return; }   // TEXTURE_LEVEL
    if (format < 0) { gl_error(GL_INVALID_OPERATION); return; }
    if (p >= 0x8212 && p <= 0x8217) {   // RED_SIZE through STENCIL_SIZE
        *v = !fb && p == 0x8216 && attachment == GL_DEPTH ? WIN_DEPTH_BITS : bits[format][p - 0x8212];
        return;
    }
    if (p == 0x8210) { *v = format == F_SRGBA8 ? 0x8c40 : GL_LINEAR; return; }   // COLOR_ENCODING, SRGB
    if (p == 0x8211) {   // COMPONENT_TYPE
        int floating = (format >= F_RGBA16F && format <= F_RG11B10F) || format == F_D32F || (fb && format == F_D32FS8);
        *v = stencil || color_is_uint(g_fmt[format].pf) ? GL_UNSIGNED_INT : floating ? GL_FLOAT : 0x8c17;   // UNSIGNED_NORMALIZED
        return;
    }
    gl_error(GL_INVALID_ENUM);
}
// A framebuffer's colour image: the texture, its level and its slice (a cube face or an array layer) or 3D slice
static id<MTLTexture> fb_color(GLuint fb, int draw, int *level, int *slice) {
    *level = *slice = 0;
    if (!fb) {
        if (draw < 0 ? !S.default_read : draw || !S.default_draw) return nil;
        if (!g_win.made) { int w, h; dm_vi_layer_size(&w, &h); win_make(w > 0 ? w : 1280, h > 0 ? h : 720); }
        return g_win.col[g_win.cur];
    }
    fb_t *f = fb_of(fb); uint64_t g = 0;
    if (!f) return nil;
    int index = draw < 0 ? f->read : f->draw[draw];
    if (!index) return nil;
    const att_t *a = &f->col[index - 1];
    *level = a->level; *slice = a->face;
    return att_tex(a, &g);
}
// glBlitFramebuffer of colour without scaling, flipping or a format change (LÖVE resolves multisampled canvases so); its last two arguments are on the stack, 64-bit here
typedef struct { GLint s[4], d[4]; GLbitfield mask; } blit_args;
static void blit_impl(const blit_args *a) __attribute__((used));
BIGSTUB(blit_impl);
static void blit_impl(const blit_args *a) {
    @autoreleasepool {
        if (a->mask & ~GL_COLOR_BUFFER_BIT) RED1("glBlitFramebuffer of depth or stencil not implemented");
        if (!(a->mask & GL_COLOR_BUFFER_BIT)) return;
        if (g_pend_clear.bits) clears_flush();
        int sl, ss; id<MTLTexture> src = fb_color(S.read_fb, -1, &sl, &ss);
        int w = a->s[2] - a->s[0], h = a->s[3] - a->s[1];
        if (!src || w <= 0 || h <= 0 || w != a->d[2] - a->d[0] || h != a->d[3] - a->d[1]) {
            RED1("glBlitFramebuffer with scaling, flipping or a format change not implemented"); return;
        }
        id<MTLTexture> dst[NGL_COLORS]; int dl[NGL_COLORS], ds[NGL_COLORS];
        for (int i = 0; i < NGL_COLORS; i++) {
            dst[i] = fb_color(S.fb, i, &dl[i], &ds[i]);
            if (dst[i] && (src.pixelFormat != dst[i].pixelFormat || src.sampleCount != dst[i].sampleCount)) {
                RED1("glBlitFramebuffer with a format or sample count change not implemented"); return;
            }
        }
        pass_end(); id<MTLBlitCommandEncoder> b = [cb_get() blitCommandEncoder];
        for (int i = 0; i < NGL_COLORS; i++) if (dst[i]) {
            [b copyFromTexture:src sourceSlice:ss sourceLevel:sl sourceOrigin:MTLOriginMake(a->s[0], a->s[1], 0) sourceSize:MTLSizeMake(w, h, 1)
                     toTexture:dst[i] destinationSlice:ds[i] destinationLevel:dl[i] destinationOrigin:MTLOriginMake(a->d[0], a->d[1], 0)];
        }
        [b endEncoding]; g_n_blit++;
    }
}
GL(glBlitFramebuffer, void, GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, uint64_t mask, uint64_t filter) {
    (void)filter; blit_args a = {{sx0, sy0, sx1, sy1}, {dx0, dy0, dx1, dy1}, (GLbitfield)mask}; ((void (*)(const blit_args *))(const void *)blit_impl_bs)(&a);
}
GL(glResolveMultisampleFramebufferAPPLE, void, void) { RED1("glResolveMultisampleFramebufferAPPLE not implemented"); }

// Image copies name the source and destination directly, with no framebuffer or texture binding.
typedef struct { GLuint name; GLenum target; GLint level, x, y, z; } copy_image_t;
typedef struct { copy_image_t src, dst; GLsizei w, h, d; } copy_image_args;
static id<MTLTexture> copy_image_tex(const copy_image_t *a, const fmt_t **fmt) {
    id<MTLTexture> image;
    if (a->target == GL_RENDERBUFFER) {
        rb_t *rb = rb_of(a->name);
        if (rb) *fmt = &g_fmt[rb->fmt];
        image = rb ? rb->tex : nil;
    } else {
        tex_t *t = tex_of(a->name);
        if (!t || t->target != a->target || (!t->made && !tex_make(t, 0))) return nil;
        t->use = g_serial;
        *fmt = &g_fmt[t->fmt];
        image = t->tex;
    }
    if (!image || a->level < 0 || (NSUInteger)a->level >= image.mipmapLevelCount ||
        a->x < 0 || a->y < 0 || a->z < 0) return nil;
    return image;
}
static MTLSize copy_image_extent(const copy_image_t *a, id<MTLTexture> t, uint64_t w, uint64_t h, unsigned d, const fmt_t *f) {
    uint32_t tw = umax(1, (uint32_t)t.width >> a->level), th = umax(1, (uint32_t)t.height >> a->level);
    uint64_t depth = a->target == GL_TEXTURE_3D ? umax(1, (uint32_t)t.depth >> a->level) :
                     a->target == GL_TEXTURE_CUBE_MAP ? 6 : t.arrayLength;
    if (a->x % f->bw || a->y % f->bh || a->x >= tw || a->y >= th || (uint64_t)a->z + d > depth) return MTLSizeMake(0, 0, 0);
    uint32_t rw = tw - (uint32_t)a->x, rh = th - (uint32_t)a->y;
    // The last compressed block can cover the edge padding outside the logical level.
    if (w > rw + (f->bw - rw % f->bw) % f->bw || h > rh + (f->bh - rh % f->bh) % f->bh) return MTLSizeMake(0, 0, 0);
    w = w > rw ? rw : w; h = h > rh ? rh : h;
    if ((w % f->bw && w != rw) || (h % f->bh && h != rh)) return MTLSizeMake(0, 0, 0);
    return MTLSizeMake(w, h, 1);
}
static void copy_image_impl(const copy_image_args *a) __attribute__((used));
BIGSTUB(copy_image_impl);
static void copy_image_impl(const copy_image_args *a) {
    @autoreleasepool {
        if (a->w < 0 || a->h < 0 || a->d < 0) { gl_error_at(GL_INVALID_VALUE, "glCopyImageSubData"); return; }
        if (!a->w || !a->h || !a->d) return;
        if (g_pend_clear.bits) clears_flush();
        const fmt_t *sf = NULL, *df = NULL;
        id<MTLTexture> src = copy_image_tex(&a->src, &sf), dst = copy_image_tex(&a->dst, &df);
        if (!src || !dst) { gl_error_at(GL_INVALID_VALUE, "glCopyImageSubData"); return; }
        int through_buffer = src.pixelFormat != dst.pixelFormat;
        int block_copy = (sf->kind == K_BLOCK && df->kind == K_COL) || (sf->kind == K_COL && df->kind == K_BLOCK);
        if ((through_buffer && (!block_copy || sf->bytes != df->bytes)) || src.sampleCount != dst.sampleCount) {
            RED1("glCopyImageSubData between Metal formats %lu and %lu not implemented", (unsigned long)src.pixelFormat, (unsigned long)dst.pixelFormat);
            gl_error_at(GL_INVALID_OPERATION, "glCopyImageSubData"); return;
        }
        uint64_t bw = ((uint64_t)a->w + sf->bw - 1) / sf->bw, bh = ((uint64_t)a->h + sf->bh - 1) / sf->bh;
        MTLSize ss = copy_image_extent(&a->src, src, a->w, a->h, a->d, sf);
        MTLSize ds = copy_image_extent(&a->dst, dst, block_copy ? bw * df->bw : a->w, block_copy ? bh * df->bh : a->h, a->d, df);
        if (!ss.width || !ds.width || (!through_buffer && (ss.width != ds.width || ss.height != ds.height))) {
            gl_error_at(GL_INVALID_VALUE, "glCopyImageSubData"); return;
        }
        int svol = a->src.target == GL_TEXTURE_3D, dvol = a->dst.target == GL_TEXTURE_3D;
        int layers = !through_buffer && svol && dvol ? 1 : a->d;
        ss.depth = a->d / layers;
        size_t pitch = bw * sf->bytes, image_bytes = pitch * bh, off = 0;
        id<MTLBuffer> staging = through_buffer ? arena_alloc(image_bytes, &off) : nil;
        if (through_buffer && !staging) { gl_error_at(GL_OUT_OF_MEMORY, "glCopyImageSubData"); return; }
        pass_end();
        id<MTLBlitCommandEncoder> b = [cb_get() blitCommandEncoder];
        for (int i = 0; i < layers; i++) {
            int sz = a->src.z + i, dz = a->dst.z + i;
            MTLOrigin so = MTLOriginMake(a->src.x, a->src.y, svol ? sz : 0), dest = MTLOriginMake(a->dst.x, a->dst.y, dvol ? dz : 0);
            if (staging) {
                // A GL texel in the uncompressed image stores the bytes of one compressed block.
                [b copyFromTexture:src sourceSlice:svol ? 0 : sz sourceLevel:a->src.level sourceOrigin:so sourceSize:ss
                          toBuffer:staging destinationOffset:off destinationBytesPerRow:pitch destinationBytesPerImage:image_bytes];
                [b copyFromBuffer:staging sourceOffset:off sourceBytesPerRow:pitch sourceBytesPerImage:image_bytes sourceSize:ds
                         toTexture:dst destinationSlice:dvol ? 0 : dz destinationLevel:a->dst.level destinationOrigin:dest];
            } else {
                [b copyFromTexture:src sourceSlice:svol ? 0 : sz sourceLevel:a->src.level sourceOrigin:so sourceSize:ss
                         toTexture:dst destinationSlice:dvol ? 0 : dz destinationLevel:a->dst.level destinationOrigin:dest];
            }
        }
        [b endEncoding]; g_n_blit++;
    }
}
GL(glCopyImageSubData, void, GLuint sn, GLenum st, GLint sl, GLint sx, GLint sy, GLint sz, GLuint dn, GLenum dt,
   int64_t dl, int64_t dx, int64_t dy, int64_t dz, int64_t w, int64_t h, int64_t d) {
    // The guest ABI uses 8-byte slots for integer arguments on the stack.
    copy_image_args a = {{sn, st, sl, sx, sy, sz}, {dn, dt, (GLint)dl, (GLint)dx, (GLint)dy, (GLint)dz}, (GLsizei)w, (GLsizei)h, (GLsizei)d};
    ((void (*)(const copy_image_args *))(const void *)copy_image_impl_bs)(&a);
}
// glCopyTexSubImage2D: a rectangle of the read framebuffer's colour image into a texture level, in command order (a blit after the draws so far). Both are stored bottom row
// first, so the rows go across as they are; the part outside the framebuffer is left as it was (GL leaves it undefined). DOOM 3 copies each frame's lit picture into its
// _currentRender texture this way and draws it back for the post effects, which cover the screen in black without the copy
static void tex_copy(GLenum target, GLint level, GLint xo, GLint yo, GLint zo, GLint x, GLint y, GLsizei w, GLsizei h) {
    tex_t *t = bound_tex(target);
    int face = target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X) :
               target == GL_TEXTURE_2D_ARRAY ? zo : 0;
    const char *name = target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY ? "glCopyTexSubImage3D" : "glCopyTexSubImage2D";
    if (!t || level < 0 || level >= MAXLV || xo < 0 || yo < 0 || zo < 0 || w < 0 || h < 0) { gl_error_at(GL_INVALID_VALUE, name); return; }
    if (g_pend_clear.bits) clears_flush();
    int sl, ss; id<MTLTexture> src = fb_color(S.read_fb, -1, &sl, &ss);
    if (!t->made && !tex_make(t, 0)) return;
    if (!src || !t->tex || level >= t->levels) { RED1("%s with no colour image to read or no texture level to write", name); return; }
    uint32_t d = target == GL_TEXTURE_3D ? umax(1, t->d >> level) : target == GL_TEXTURE_2D_ARRAY ? t->d : 1;
    if ((uint32_t)zo >= d) { gl_error_at(GL_INVALID_VALUE, name); return; }
    uint32_t sw = umax(1, (uint32_t)src.width >> sl), sh = umax(1, (uint32_t)src.height >> sl), tw = umax(1, t->w >> level), th = umax(1, t->h >> level);
    if (x < 0) { xo -= x; w += x; x = 0; } if (y < 0) { yo -= y; h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    w = (GLsizei)umin((uint32_t)w, umin(sw - umin((uint32_t)x, sw), tw - umin((uint32_t)xo, tw))); h = (GLsizei)umin((uint32_t)h, umin(sh - umin((uint32_t)y, sh), th - umin((uint32_t)yo, th)));
    if (w <= 0 || h <= 0) return;
    if (src.pixelFormat != t->tex.pixelFormat || src.sampleCount != t->tex.sampleCount) {
        RED1("%s from a %lu image into a %lu texture: only the same format is copied", name, (unsigned long)src.pixelFormat, (unsigned long)t->tex.pixelFormat); return;
    }
    pass_end();
    id<MTLBlitCommandEncoder> b = [cb_get() blitCommandEncoder];
    int volume = src.textureType == MTLTextureType3D;
    [b copyFromTexture:src sourceSlice:volume ? 0 : ss sourceLevel:sl sourceOrigin:MTLOriginMake(x, y, volume ? ss : 0) sourceSize:MTLSizeMake(w, h, 1)
             toTexture:t->tex destinationSlice:face destinationLevel:level destinationOrigin:MTLOriginMake(xo, yo, target == GL_TEXTURE_3D ? zo : 0)];
    [b endEncoding]; g_n_blit++; t->use = g_serial;
}
GLB(glCopyTexSubImage2D, void, GLenum target, GLint level, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h) {
    @autoreleasepool { tex_copy(target, level, xo, yo, 0, x, y, w, h); }
}
typedef struct { GLenum target; GLint lv, xo, yo, zo, x, y; GLsizei w, h; } copy_slice_args;
static void copy_slice_impl(const copy_slice_args *a) __attribute__((used));
BIGSTUB(copy_slice_impl);
static void copy_slice_impl(const copy_slice_args *a) {
    @autoreleasepool { tex_copy(a->target, a->lv, a->xo, a->yo, a->zo, a->x, a->y, a->w, a->h); }
}
GL(glCopyTexSubImage3D, void, GLenum target, GLint lv, GLint xo, GLint yo, GLint zo, GLint x, GLint y, GLsizei w, GLsizei h) {
    if (target != GL_TEXTURE_3D && target != GL_TEXTURE_2D_ARRAY) { gl_error(GL_INVALID_ENUM); return; }
    copy_slice_args a = {target, lv, xo, yo, zo, x, y, w, h};
    ((void (*)(const copy_slice_args *))(const void *)copy_slice_impl_bs)(&a);
}
// glCopyTexImage2D: the level (re)defined at the copy's size, in the read image's format when the internal format names the same one (a texture the copy keeps
// remaking at one size keeps its Metal texture, tex_image), then filled as glCopyTexSubImage2D does
GLB(glCopyTexImage2D, void, GLenum target, GLint level, GLenum ifmt, GLint x, GLint y, GLsizei w, GLsizei h, GLint border) {
    @autoreleasepool {
        int cv, f = fmt_pick((GLint)ifmt, GL_RGBA, GL_UNSIGNED_BYTE, &cv);
        if (f < 0 || g_fmt[f].kind == K_BLOCK) { RED1("glCopyTexImage2D into internal format %#x not implemented", ifmt); return; }
        if (w <= 0 || h <= 0 || border) { gl_error(GL_INVALID_VALUE); return; }
        tex_image(target, level, ifmt, f, cv, (uint32_t)w, (uint32_t)h, 1, NULL, fmt_pitch(f, (uint32_t)w));
        tex_copy(target, level, 0, 0, 0, x, y, w, h);
    }
}
// glReadPixels: everything recorded so far runs first (the frame's command buffer is committed and waited for), then the rows are copied out as GL lays them, bottom row first,
// which is how this layer stores them. RGBA / unsigned byte from an RGBA8 or BGRA8 image (LÖVE's Canvas:newImageData and screenshots)
typedef struct { GLint x, y; GLsizei w, h; GLenum format, type; void *data; GLenum target; GLint level; } readpix_args;
static void readpix_impl(const readpix_args *a) __attribute__((used));
BIGSTUB(readpix_impl);
static void readpix_impl(const readpix_args *a) {
    @autoreleasepool {
        if (!a->data || (!a->target && (a->w <= 0 || a->h <= 0))) return;
        int lv, sl;
        id<MTLTexture> src;
        readpix_args tex = *a;
        if (!a->target) src = fb_color(S.read_fb, -1, &lv, &sl);
        else {
            int cube = a->target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && a->target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
            if (a->target != GL_TEXTURE_2D && !cube) { RED1("glGetTexImage target %#x not implemented", a->target); return; }
            if (g_pend_clear.bits) clears_flush();
            tex_t *t = bound_tex(a->target);
            if (!t || (!t->made && !tex_make(t, 0))) return;
            lv = a->level; sl = cube ? (int)(a->target - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
            if (lv < 0 || lv >= t->levels) { gl_error(GL_INVALID_VALUE); return; }
            src = t->tex;
            tex.w = (GLsizei)umax(1, t->w >> lv); tex.h = (GLsizei)umax(1, t->h >> lv);
            a = &tex;
        }
        MTLPixelFormat pf = src.pixelFormat;
        int bgra = pf == MTLPixelFormatBGRA8Unorm, bytes = 0;
        int decompress = src.textureType == MTLTextureType2D &&
            (pf == MTLPixelFormatBC1_RGBA || pf == MTLPixelFormatBC2_RGBA || pf == MTLPixelFormatBC3_RGBA || pf == MTLPixelFormatBC4_RUnorm);
        decompress &= a->format == GL_RGBA && a->type == GL_UNSIGNED_BYTE;
        if (a->format == GL_RGBA && a->type == GL_UNSIGNED_BYTE && (pf == MTLPixelFormatRGBA8Unorm || pf == MTLPixelFormatRGBA8Unorm_sRGB || bgra)) bytes = 4;
        if (decompress) bytes = 4;
        if (a->format == GL_RED_INTEGER && a->type == GL_UNSIGNED_SHORT && pf == MTLPixelFormatR16Uint) bytes = 2;
        if (a->type == GL_UNSIGNED_INT) {
            if (a->format == GL_RG_INTEGER && pf == MTLPixelFormatRG32Uint) bytes = 8;
            if (a->format == GL_RGBA_INTEGER && pf == MTLPixelFormatRGBA32Uint) bytes = 16;
        }
        if (!bytes) {
            RED1("%s(%#x, %#x) of a %lu image not implemented", a->target ? "glGetTexImage" : "glReadPixels",
                 a->format, a->type, src ? (unsigned long)src.pixelFormat : 0ul); return;
        }
        if (g_pend_clear.bits) clears_flush();
        size_t pitch = (size_t)a->w * bytes; id<MTLBuffer> out = [g_dev newBufferWithLength:pitch * a->h options:MTLResourceStorageModeShared];
        if (!out) { gl_error_at(GL_OUT_OF_MEMORY, a->target ? "glGetTexImage" : "glReadPixels"); return; }
        static id<MTLComputePipelineState> decode;
        if (decompress && !decode) {
            id<MTLFunction> fn = [g_util newFunctionWithName:@"read_bc_rgba8"];
            if (!fn) { RED1("compressed texture readback kernel is missing"); return; }
            NSError *error = nil;
            decode = [g_dev newComputePipelineStateWithFunction:fn error:&error];
            if (!decode) { RED1("compressed texture readback: %s", error.localizedDescription.UTF8String); return; }
        }
        dm_gfx_enter();
        pass_end();
        id<MTLCommandBuffer> cb = cb_get();
        if (decompress) {
            id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
            uint32_t region[] = {(uint32_t)a->x, (uint32_t)a->y, (uint32_t)a->w, (uint32_t)a->h}, level = (uint32_t)lv;
            [c setComputePipelineState:decode]; [c setTexture:src atIndex:0]; [c setBuffer:out offset:0 atIndex:0];
            [c setBytes:region length:sizeof region atIndex:1]; [c setBytes:&level length:sizeof level atIndex:2];
            [c dispatchThreads:MTLSizeMake(a->w, a->h, 1) threadsPerThreadgroup:MTLSizeMake(16, 8, 1)];
            [c endEncoding];
        } else {
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromTexture:src sourceSlice:sl sourceLevel:lv sourceOrigin:MTLOriginMake(a->x, a->y, 0) sourceSize:MTLSizeMake(a->w, a->h, 1) toBuffer:out destinationOffset:0
                destinationBytesPerRow:pitch destinationBytesPerImage:pitch * a->h];
            [b endEncoding];
        }
        cb_commit(); [cb waitUntilCompleted];
        dm_gfx_leave();
        if (cb.status != MTLCommandBufferStatusCompleted) return;
        size_t dp = (pitch + S.pack_align - 1) / S.pack_align * S.pack_align;
        for (GLsizei r = 0; r < a->h; r++) {
            uint8_t *d = (uint8_t *)a->data + r * dp; const uint8_t *s = (const uint8_t *)out.contents + r * pitch;
            if (!bgra) memcpy(d, s, pitch);
            else for (GLsizei x = 0; x < a->w; x++) { d[4 * x] = s[4 * x + 2]; d[4 * x + 1] = s[4 * x + 1]; d[4 * x + 2] = s[4 * x]; d[4 * x + 3] = s[4 * x + 3]; }
        }
    }
}
GL(glReadPixels, void, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *data) {
    readpix_args a = {x, y, w, h, format, type, data, 0, 0}; ((void (*)(const readpix_args *))(const void *)readpix_impl_bs)(&a);
}
GL(glGetTexLevelParameteriv, void, GLenum target, GLint level, GLenum p, GLint *value) {
    tex_t *t = bound_tex(target);
    if (level < 0 || level >= MAXLV) { gl_error(GL_INVALID_VALUE); return; }
    int defined = t && t->fmt >= 0 && (level < t->levels || level < t->slevels || (t->defined & (1u << level)));
    uint32_t w = defined ? umax(1, t->w >> level) : 0, h = defined ? umax(1, t->h >> level) : 0;
    uint32_t d = !defined ? 0 : target == GL_TEXTURE_3D ? umax(1, t->d >> level) : target == GL_TEXTURE_2D_ARRAY ? t->d : 1;
    int compressed = defined && g_fmt[t->fmt].kind == K_BLOCK;
    switch (p) {
        case 0x1000: *value = (GLint)w; return;   // TEXTURE_WIDTH
        case 0x1001: *value = (GLint)h; return;   // TEXTURE_HEIGHT
        case 0x8071: *value = (GLint)d; return;   // TEXTURE_DEPTH
        case 0x1003: *value = defined ? t->ifmt : GL_RGBA; return;   // TEXTURE_INTERNAL_FORMAT
        case 0x86a1: *value = compressed; return;   // TEXTURE_COMPRESSED
        case 0x86a0:   // TEXTURE_COMPRESSED_IMAGE_SIZE
            if (!compressed) { gl_error(GL_INVALID_OPERATION); return; }
            *value = (GLint)(fmt_pitch(t->fmt, w) * fmt_rows(t->fmt, h) * d); return;
    }
    gl_error(GL_INVALID_ENUM);
}
GLB(glGetCompressedTexImage, void, GLenum target, GLint level, void *data) {
    tex_t *t = bound_tex(target);
    if (!t || (!t->made && !tex_make(t, 0)) || !t->tex || g_fmt[t->fmt].kind != K_BLOCK) { gl_error(GL_INVALID_OPERATION); return; }
    if (level < 0 || level >= t->levels) { gl_error(GL_INVALID_VALUE); return; }
    if (!data) return;
    @autoreleasepool {
        uint32_t w = umax(1, t->w >> level), h = umax(1, t->h >> level);
        size_t pitch = fmt_pitch(t->fmt, w), image = pitch * fmt_rows(t->fmt, h);
        int cube = target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
        NSUInteger face = cube ? target - GL_TEXTURE_CUBE_MAP_POSITIVE_X : 0;
        NSUInteger layers = target == GL_TEXTURE_2D_ARRAY ? t->d : 1;
        id<MTLBuffer> out = [g_dev newBufferWithLength:image * layers options:MTLResourceStorageModeShared];
        if (!out) { gl_error(GL_OUT_OF_MEMORY); return; }
        if (g_pend_clear.bits) clears_flush();
        dm_gfx_enter();
        pass_end();
        id<MTLCommandBuffer> cb = cb_get();
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        for (NSUInteger i = 0; i < layers; i++) {
            [b copyFromTexture:t->tex sourceSlice:face + i sourceLevel:level sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                      toBuffer:out destinationOffset:i * image destinationBytesPerRow:pitch destinationBytesPerImage:image];
        }
        [b endEncoding];
        cb_commit(); [cb waitUntilCompleted];
        dm_gfx_leave();
        if (cb.status == MTLCommandBufferStatusCompleted) memcpy(data, out.contents, image * layers);
    }
}
GLB(glGetTexImage, void, GLenum target, GLint level, GLenum format, GLenum type, void *data) {
    if (!target) { gl_error(GL_INVALID_ENUM); return; }
    readpix_args a = {.format = format, .type = type, .data = data, .target = target, .level = level};
    readpix_impl(&a);
}

// ---------- Shaders and programs ----------
static int g_compiles;   // glCompileShader calls so far (renderprogs_job runs after the game's first)
#if TARGET_OS_OSX
// A program holds its stage's reflection pointer after the shader is deleted or recompiled.
// Each captured stage has a stable address and is shared by every later use of the same key and stage.
typedef struct capture_stage {
    shinfo info;
    __strong id<MTLFunction> fn;
    struct capture_stage *next;
} capture_stage;
static capture_stage *g_capture_stages;

static int sh_run_compiler(const char *stage, const char *src, const char *out) {
    const char *python = getenv("DM_GL_CAPTURE_PYTHON");
    if (!python) python = "python3";
    char *argv[] = {(char *)python, (char *)g_capture_compiler, "--capture-shader", (char *)stage, (char *)src, (char *)out, NULL};
    // The host handles SIGTERM through GCD and ignores its ordinary disposition. The compiler needs the default to stop with the capture.
    sigset_t defaults;
    sigemptyset(&defaults); sigaddset(&defaults, SIGTERM);
    posix_spawnattr_t attr;
    int error = posix_spawnattr_init(&attr);
    pid_t child;
    if (!error) {
        error = posix_spawnattr_setsigdefault(&attr, &defaults);
        if (!error) error = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF);
        if (!error) error = posix_spawnp(&child, argv[0], NULL, &attr, argv, *_NSGetEnviron());
        posix_spawnattr_destroy(&attr);
    }
    if (error) { GLOG("[RED] cannot start the GL capture compiler: %s", strerror(error)); return 0; }
    int status;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    return waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void sh_compile_capture(shader_t *s, const char *stage, const char *source) {
    int frag = s->type == GL_FRAGMENT_SHADER;
    for (capture_stage *c = g_capture_stages; c; c = c->next) {
        if (c->info.frag == frag && !strcmp(c->info.key, s->key)) { s->si = &c->info; s->fn = c->fn; return; }
    }
    const char *tmp = getenv("DM_TEMP");
    if (!tmp || !tmp[0]) tmp = NSTemporaryDirectory().fileSystemRepresentation;
    char work[PATH_MAX];
    if (snprintf(work, sizeof work, "%s/gl-shader.XXXXXX", tmp) >= (int)sizeof work || !mkdtemp(work)) {
        GLOG("[RED] cannot create GL capture compiler directory in %s", tmp);
        return;
    }
    char table[sizeof work + 32], library[sizeof work + 32];
    snprintf(table, sizeof table, "%s/gl_shaders.tsv", work);
    snprintf(library, sizeof library, "%s/gl_shaders.metallib", work);
    if (sh_run_compiler(stage, source, work)) {
        FILE *f = fopen(table, "r");
        static char line[65536];
        shinfo info;
        if (f && sh_read_abi(f) && fgets(line, sizeof line, f) && sh_read_row(line, &info)) {
            id<MTLFunction> fn = info.frag == frag && !strcmp(info.key, s->key) ? sh_function(sh_library(library), &info) : nil;
            if (fn) {
                capture_stage *c = zalloc(sizeof *c);
                c->info = info; c->fn = fn; c->next = g_capture_stages; g_capture_stages = c;
                s->si = &c->info; s->fn = c->fn;
                GLOG("compiled capture shader %s.%s", s->key, stage);
            } else sh_free(&info);
        }
        if (f) fclose(f);
    }
    if (!s->fn) GLOG("[RED] failed to compile capture shader %s.%s", s->key, stage);
    unlink(table); unlink(library); rmdir(work);
}
#endif

static void sh_capture(shader_t *s) {
    const char *stage = s->type == GL_VERTEX_SHADER ? "vert" : "frag";
    char path[PATH_MAX], temp[sizeof path + 32];
    if (snprintf(path, sizeof path, "%s/%s.%s", g_capture_dir, s->key, stage) >= (int)sizeof path) {
        RED1("shader capture path too long: %s", g_capture_dir);
        return;
    }
    int fresh = access(path, F_OK) != 0;
    if (fresh) {
        snprintf(temp, sizeof temp, "%s.%d.tmp", path, getpid());
        FILE *f = fopen(temp, "wb");
        if (!f) { RED1("shader capture cannot write to %s", g_capture_dir); return; }
        int ok = fwrite(s->src, 1, s->len, f) == s->len;
        if (fclose(f) != 0 || !ok) { unlink(temp); RED1("shader capture cannot write to %s", g_capture_dir); return; }
    }
#if TARGET_OS_OSX
    if (!s->fn && g_capture_compiler) sh_compile_capture(s, stage, fresh ? temp : path);
#endif
    // The packer can stop the run once it sees the last complete file.
    if (fresh && rename(temp, path) != 0) { unlink(temp); RED1("shader capture cannot rename %s", path); }
}
GL(glCreateShader, GLuint, GLenum type) {
    GLuint n = name_new((void ***)&g_sps, &g_nsps, &g_capsps, sizeof(sp_t)); g_sps[n]->s.type = type; return n;
}
GL(glCreateProgram, GLuint, void) {
    GLuint n = name_new((void ***)&g_sps, &g_nsps, &g_capsps, sizeof(sp_t)); g_sps[n]->is_prog = 1; return n;
}
GL(glShaderSource, void, GLuint sh, GLsizei count, const GLchar *const *str, const GLint *len) {
    if (!SP_CHECK(sh, 0)) return;
    if (count < 0) { gl_error(GL_INVALID_VALUE); return; }
    shader_t *s = shader_of(sh);
    size_t n = 0;
    for (GLsizei i = 0; i < count; i++) n += len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
    free(s->src); s->src = zalloc(n + 1); s->len = 0;
    for (GLsizei i = 0; i < count; i++) { size_t k = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]); memcpy(s->src + s->len, str[i], k); s->len += k; }
    s->len = strnlen(s->src, s->len);   // the source ends at its first NUL (gl_shaders.py hashes the same text)
    s->compiled = 0;
}
GLB(glCompileShader, void, GLuint sh) {
    if (!SP_CHECK(sh, 0)) return;
    shader_t *s = shader_of(sh); if (!s->src) return;
    g_compiles++;
    uint8_t h[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(s->src, (CC_LONG)s->len, h);
    for (int i = 0; i < 8; i++) snprintf(s->key + 2 * i, 3, "%02x", h[i]);
    int frag = s->type == GL_FRAGMENT_SHADER;
    s->si = sh_find(s->key, frag);
    @autoreleasepool {
        s->fn = s->si && g_lib ? sh_function(g_lib, s->si) : nil;
        if (g_capture_dir) sh_capture(s);
    }
    s->compiled = s->fn != nil;
    if (s->fn) OLOG("CompileShader %s", s->key);
    else if (!g_capture_compiler) { static int n; if (n++ < 50) GLOG("[RED] %s shader %s (%zu bytes of GLSL) is not in the library (DM_GL_CAPTURE collects it)", s->type == GL_VERTEX_SHADER ? "vertex" : "fragment", s->key, s->len); }
}
GL(glGetShaderiv, void, GLuint sh, GLenum p, GLint *v) {
    if (!SP_CHECK(sh, 0)) { *v = 0; return; }
    shader_t *s = shader_of(sh);
    switch (p) {
        case GL_COMPILE_STATUS: *v = g_capture_compiler ? s->compiled : 1; return;   // normal runs report missing library entries without aborting the game
        case GL_INFO_LOG_LENGTH: *v = 0; return;
        case GL_SHADER_TYPE: *v = s->type; return;
        case GL_SHADER_SOURCE_LENGTH: *v = s->len ? (GLint)s->len + 1 : 0; return;
        case GL_DELETE_STATUS: *v = s->deleted; return;
    }
    RED1("glGetShaderiv(%#x) not implemented", p); *v = 0;
}
GL(glGetShaderInfoLog, void, GLuint sh, GLsizei size, GLsizei *len, GLchar *log) { (void)sh; if (len) *len = 0; if (log && size > 0) log[0] = 0; }
GL(glGetShaderSource, void, GLuint sh, GLsizei size, GLsizei *len, GLchar *src) {   // the source glShaderSource kept (Fate/hollow ataraxia REMASTERED reads it back after compiling)
    if (!SP_CHECK(sh, 0)) return;
    if (size < 0) { gl_error(GL_INVALID_VALUE); return; }
    shader_t *s = shader_of(sh);
    if (len) *len = 0;
    if (!src || size == 0) return;
    if (!s->src) { src[0] = 0; return; }
    size_t c = s->len < (size_t)size - 1 ? s->len : (size_t)size - 1;
    memcpy(src, s->src, c); src[c] = 0;
    if (len) *len = (GLsizei)c;
}
static void shader_free(GLuint sh) { shader_t *s = &g_sps[sh]->s; free(s->src); s->fn = nil; free(g_sps[sh]); g_sps[sh] = NULL; }
static void shader_release(GLuint *slot) {   // a program lets go of the shader in one of its stages
    GLuint sh = *slot; shader_t *s = shader_of(sh); *slot = 0;
    if (s && --s->refs <= 0 && s->deleted) shader_free(sh);
}
GL(glDeleteShader, void, GLuint sh) {
    if (!sh || !SP_CHECK(sh, 0)) return;   // 0 is silently ignored
    shader_t *s = shader_of(sh);
    OLOG("DeleteShader %u%s", sh, s->refs ? " (attached: flagged)" : "");
    if (s->refs) s->deleted = 1; else shader_free(sh);
}
// One shader per stage: attaching another replaces it (GLES refuses a second one; the shader tables have one stage each), said once: desktop GL would link both. The same
// shader again is GL_INVALID_OPERATION, and nothing changes, its count included
GL(glAttachShader, void, GLuint pr, GLuint sh) {
    if (!SP_CHECK(pr, 1) || !SP_CHECK(sh, 0)) return;
    prog_t *p = prog_of(pr); shader_t *s = shader_of(sh);
    GLuint *slot = s->type == GL_VERTEX_SHADER ? &p->vs : &p->fs;
    if (*slot == sh) { gl_error(GL_INVALID_OPERATION); return; }
    if (*slot) RED1("program %u: a second %s shader (%u) replaces the first (%u): desktop GL links both, the shader tables have one per stage", pr, s->type == GL_VERTEX_SHADER ? "vertex" : "fragment", sh, *slot);
    shader_release(slot); *slot = sh; s->refs++;
}
GL(glDetachShader, void, GLuint pr, GLuint sh) {
    if (!SP_CHECK(pr, 1) || !SP_CHECK(sh, 0)) return;
    prog_t *p = prog_of(pr);
    OLOG("DetachShader %u from %u", sh, pr);
    if (p->vs == sh) shader_release(&p->vs); else if (p->fs == sh) shader_release(&p->fs); else gl_error(GL_INVALID_OPERATION);   // not attached to it
}
static void prog_clear(prog_t *p) {
    p->validated = 0; p->validation_log[0] = 0;
    for (int i = 0; i < p->npc; i++) if (p->pc[i].pso) CFRelease(p->pc[i].pso);
    free(p->pc); p->pc = NULL; p->npc = p->cappc = 0;
    free(p->u); p->u = NULL; p->nu = 0; free(p->ub[0]); free(p->ub[1]); p->ub[0] = p->ub[1] = NULL; p->fn[0] = p->fn[1] = nil; p->linked = 0;
    for (int st = 0; st < 2; st++) { p->ae[st] = nil; p->ub_buf[st] = nil; p->ub_serial[st] = p->ub_written_gen[st] = 0; }
}
// Uniforms are merged by name over both stages, as GL links them; a location is (uniform index + 1) × 4096 + array element (uni_loc, uni_of)
GLB(glLinkProgram, void, GLuint pr) {
    static uint32_t serials;
    if (!SP_CHECK(pr, 1)) return;
    prog_t *p = prog_of(pr);
    prog_clear(p);
    shader_t *vs = shader_of(p->vs), *fs = shader_of(p->fs);
    // A program with a vertex shader alone links in desktop GL: its draws rasterize and write depth and stencil, their colour is undefined (DOOM 3's shadow volumes,
    // drawn with colour writes off). Its fragment stage is the empty one here, and its pipelines have no fragment function and write no colour (pso_get)
    if (!vs || !vs->fn || (fs && !fs->fn)) {
        RED1("program %u: a shader is missing from the library, its draws are skipped", pr);
        return;
    }
    static const shinfo nofrag = {.key = "none", .frag = 1, .ub_slot = -1, .arg_slot = -1};
    p->si[0] = vs->si; p->si[1] = fs ? fs->si : &nofrag; p->fn[0] = vs->fn; p->fn[1] = fs ? fs->fn : nil;
    int cap = p->si[0]->nu + p->si[0]->nt + p->si[1]->nu + p->si[1]->nt; p->u = zalloc((cap + 1) * sizeof *p->u);
    for (int st = 0; st < 2; st++) {
        const shinfo *si = p->si[st];
        if (si->nargs) {
            p->ae[st] = [p->fn[st] newArgumentEncoderWithBufferIndex:si->arg_slot];
            if (!p->ae[st].encodedLength || p->ae[st].alignment > 256) { RED1("shader %s: invalid argument buffer layout", si->key); return; }
        }
        p->ub[st] = zalloc(si->ub_size + 16);
        for (int i = 0; i < si->nu + si->nt; i++) {
            const char *nm = i < si->nu ? si->u[i].name : si->t[i - si->nu].name;
            puni_t *u = NULL;
            for (int j = 0; j < p->nu; j++) if (!strcmp(p->u[j].name, nm)) u = &p->u[j];
            if (!u) { u = &p->u[p->nu++]; strlcpy(u->name, nm, sizeof u->name); u->off[0] = u->off[1] = -1; u->slot[0] = u->slot[1] = -1; u->sampler[0] = u->sampler[1] = -1; }
            if (i < si->nu) { const shuni *m = &si->u[i]; u->type = m->type; u->count = m->count; u->off[st] = m->off; u->stride[st] = m->stride; }
            else { const shsamp *m = &si->t[i - si->nu]; u->type = m->type; u->slot[st] = m->slot; u->sampler[st] = m->sampler; if (m->unit) u->unit = m->unit; }
        }
    }
    // the game's attribute indices: glBindAttribLocation's for the inputs it names, the input's own number for the others (or the lowest index no binding took)
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
    // named uniform blocks, merged by name over both stages as GL links them; each starts at binding point 0
    p->nblk = 0;
    for (int st = 0; st < 2; st++) for (int i = 0; i < p->si[st]->nb; i++) {
        const shblk *b = &p->si[st]->b[i]; pblk_t *x = NULL;
        for (int j = 0; j < p->nblk; j++) if (!strcmp(p->blk[j].name, b->name)) x = &p->blk[j];
        if (!x) { if (p->nblk == 8) { RED1("program %u: more than 8 uniform blocks", pr); continue; } x = &p->blk[p->nblk++]; memset(x, 0, sizeof *x); strlcpy(x->name, b->name, sizeof x->name); x->slot[0] = x->slot[1] = -1; }
        x->slot[st] = b->slot; x->size = umax(x->size, b->size);
    }
    p->serial = ++serials; p->ub_gen[0]++; p->ub_gen[1]++; p->linked = 1;
    OLOG("LinkProgram %u: vs %s fs %s, %d uniforms", pr, vs->key, fs ? fs->key : "none", p->nu);
}
GL(glBindAttribLocation, void, GLuint pr, GLuint index, const GLchar *name) {   // takes effect at the next link
    if (!SP_CHECK(pr, 1)) return;
    if (index >= 16) { gl_error(GL_INVALID_VALUE); return; }   // GL_MAX_VERTEX_ATTRIBS
    prog_t *p = prog_of(pr); if (!name) return;
    int j = 0; while (j < p->nbind && strcmp(p->bind[j].name, name)) j++;
    if (j == 16) { RED1("more than 16 attribute bindings in a program"); return; }
    if (j == p->nbind) { strlcpy(p->bind[j].name, name, sizeof p->bind[j].name); p->nbind++; }
    p->bind[j].index = (int)index;
}
// Validation is a snapshot of the linked executable and its active samplers' current units. Different sampler types cannot share a unit, and each stage must fit its limits.
GL(glValidateProgram, void, GLuint pr) {
    if (!SP_CHECK(pr, 1)) return;
    prog_t *p = prog_of(pr); p->validated = 0; p->validation_log[0] = 0;
    const char *why = p->linked ? NULL : "program has no linked executable";
    uint8_t types[NUNITS] = {0}; int counts[2] = {0};
    for (int i = 0; !why && i < p->nu; i++) {
        const puni_t *u = &p->u[i]; if (!is_samp(u->type)) continue;
        if (u->unit < 0 || u->unit >= NUNITS) why = "sampler unit is outside GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS";
        else if (types[u->unit] && types[u->unit] != u->type) why = "samplers of different types use the same texture unit";
        else { types[u->unit] = u->type; for (int st = 0; st < 2; st++) counts[st] += u->slot[st] >= 0; }
    }
    if (!why && (counts[0] > 16 || counts[1] > 32 || counts[0] + counts[1] > NUNITS)) why = "active samplers exceed the texture image unit limits";
    if (why) strlcpy(p->validation_log, why, sizeof p->validation_log); else p->validated = 1;
}
GL(glGetProgramiv, void, GLuint pr, GLenum p, GLint *v) {
    if (!SP_CHECK(pr, 1)) { *v = 0; return; }
    prog_t *x = prog_of(pr);
    switch (p) {
        case GL_LINK_STATUS: *v = g_capture_compiler ? x->linked : 1; return;
        case GL_VALIDATE_STATUS: *v = x->validated; return;
        case GL_INFO_LOG_LENGTH: *v = x->validation_log[0] ? (GLint)strlen(x->validation_log) + 1 : 0; return;
        case GL_ACTIVE_UNIFORMS: *v = x->nu; return;
        case GL_ACTIVE_UNIFORM_MAX_LENGTH: { int m = 0; for (int i = 0; i < x->nu; i++) m = (int)umax(m, (uint32_t)strlen(x->u[i].name) + 4); *v = m; return; }
        case GL_ATTACHED_SHADERS: *v = (x->vs != 0) + (x->fs != 0); return;
        case GL_ACTIVE_ATTRIBUTES:
        case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH: {
            const shinfo *vs = x->linked ? x->si[0] : NULL;
            int n = vs ? vs->na : 0;
            if (p == GL_ACTIVE_ATTRIBUTES) { *v = n; return; }
            *v = 0;
            for (int i = 0; i < n; i++) *v = (GLint)umax((uint32_t)*v, (uint32_t)strlen(vs->a[i].name) + 1);
            return;
        }
        case GL_DELETE_STATUS: *v = x->deleted; return;
    }
    RED1("glGetProgramiv(%#x) not implemented", p); *v = 0;
}
static void prog_free(GLuint pr) {   // the program goes, and its attachments with it
    prog_t *p = &g_sps[pr]->p; prog_clear(p); shader_release(&p->vs); shader_release(&p->fs); free(g_sps[pr]); g_sps[pr] = NULL;
}
// As in GL, the current program deleted stays in use, its draws going on, until another is made current
GL(glDeleteProgram, void, GLuint pr) {
    if (!pr || !SP_CHECK(pr, 1)) return;   // 0 is silently ignored
    prog_t *p = prog_of(pr);
    OLOG("DeleteProgram %u%s", pr, S.prog == pr ? " (current: flagged)" : "");
    if (S.prog == pr) p->deleted = 1; else prog_free(pr);
}
GL(glUseProgram, void, GLuint pr) {
    if (pr && !SP_CHECK(pr, 1)) return;   // an error leaves the current program as it is
    GLuint old = S.prog; S.prog = pr;
    prog_t *o = prog_of(old); if (old != pr && o && o->deleted) prog_free(old);
}
GL(glGetAttribLocation, GLint, GLuint pr, const GLchar *name) {
    if (!SP_CHECK(pr, 1)) return -1;
    prog_t *p = prog_of(pr); if (!p->linked) return -1;
    for (int i = 0; i < p->si[0]->na; i++) if (!strcmp(p->si[0]->a[i].name, name)) return p->amap[p->si[0]->a[i].slot];
    return -1;
}
// The enumeration index is the attribute's row in the linked vertex table, independent of its bound location.
GL(glGetActiveAttrib, void, GLuint pr, GLuint index, GLsizei size, GLsizei *len, GLint *count, GLenum *type, GLchar *name) {
    if (!SP_CHECK(pr, 1)) return;
    prog_t *p = prog_of(pr);
    const shinfo *vs = p->linked ? p->si[0] : NULL;
    if (size < 0 || !vs || index >= (GLuint)vs->na) { gl_error(GL_INVALID_VALUE); return; }
    const shattr *a = &vs->a[index];
    size_t copied = 0;
    if (name && size > 0) copied = umin((uint32_t)strlcpy(name, a->name, (size_t)size), (uint32_t)size - 1);
    if (len) *len = (GLsizei)copied;
    if (count) *count = 1;
    if (type) *type = g_tenums[a->type];
}
GL(glGetProgramInfoLog, void, GLuint pr, GLsizei size, GLsizei *len, GLchar *log) {
    if (len) *len = 0;
    prog_t *p = prog_of(pr); if (!p) { (void)SP_CHECK(pr, 1); return; }
    if (size < 0) { gl_error(GL_INVALID_VALUE); return; }
    if (log && size > 0) { strlcpy(log, p->validation_log, (size_t)size); if (len) *len = (GLsizei)strlen(log); }
}
// The uniforms in the order of the program's table (both stages merged, samplers among them): an array's name ends in [0], as GL reports it (LÖVE's mapActiveUniforms)
GL(glGetActiveUniform, void, GLuint pr, GLuint index, GLsizei size, GLsizei *len, GLint *count, GLenum *type, GLchar *name) {
    prog_t *p = prog_of(pr);
    if (!p || !p->linked || index >= (GLuint)p->nu) { if (len) *len = 0; if (name && size > 0) name[0] = 0; return; }
    const puni_t *u = &p->u[index]; char nm[64];
    snprintf(nm, sizeof nm, "%s%s", u->name, u->count ? "[0]" : "");
    if (name && size > 0) strlcpy(name, nm, (size_t)size);
    if (len) *len = name && size > 0 ? (GLsizei)umin((uint32_t)strlen(nm), (uint32_t)size - 1) : 0;
    if (count) *count = u->count ? u->count : 1;
    if (type) *type = g_tenums[u->type];
}
GL(glGetShaderPrecisionFormat, void, GLenum stage, GLenum prec, GLint *range, GLint *precision) {   // every precision is a 32-bit float or int
    (void)stage; int fl = prec <= 0x8df2;   // LOW / MEDIUM / HIGH_FLOAT, then the INTs
    if (range) { range[0] = fl ? 127 : 31; range[1] = fl ? 127 : 30; }
    if (precision) *precision = fl ? 23 : 0;
}
// No uniform has location 0, though GL allows it: a game may take 0 for "not found" (Mega Man Legacy Collection loads u_flip's location and skips its glUniform1f when it is 0,
// main+0x77b28, so its pictures stayed upside down while u_flip was a program's first uniform)
static GLint uni_loc(int i, int el) { return (i + 1) * 4096 + el; }
static int uni_of(const prog_t *p, GLint loc, int *el) {   // the uniform index of loc in p, or -1; el: its array element
    if (!p || !p->linked || loc < 4096 || loc / 4096 - 1 >= p->nu) return -1;
    if (el) *el = loc % 4096;
    return loc / 4096 - 1;
}
GL(glGetUniformLocation, GLint, GLuint pr, const GLchar *name) {
    prog_t *p = prog_of(pr); if (!p || !p->linked) return -1;
    char nm[64]; strlcpy(nm, name, sizeof nm); int elem = 0;
    char *br = strchr(nm, '[');
    if (br) { elem = atoi(br + 1); *br = 0; }
    for (int i = 0; i < p->nu; i++) if (!strcmp(p->u[i].name, nm)) return (elem < 0 || elem >= (p->u[i].count ? p->u[i].count : 1)) ? -1 : uni_loc(i, elem);
    return -1;
}
// Named uniform blocks: an index is the block's place in the program's list (prog_t.blk), GL_INVALID_INDEX for a name it has not
GL(glGetUniformBlockIndex, GLuint, GLuint pr, const GLchar *name) {
    if (!SP_CHECK(pr, 1)) return 0xffffffffu;
    prog_t *p = prog_of(pr);
    if (p->linked && name) for (int i = 0; i < p->nblk; i++) if (!strcmp(p->blk[i].name, name)) return (GLuint)i;
    return 0xffffffffu;
}
GL(glUniformBlockBinding, void, GLuint pr, GLuint index, GLuint binding) {
    if (!SP_CHECK(pr, 1)) return;
    prog_t *p = prog_of(pr);
    if (index >= (GLuint)p->nblk || binding >= NUBO) { gl_error(GL_INVALID_VALUE); return; }
    p->blk[index].binding = binding;
}
GL(glGetActiveUniformBlockiv, void, GLuint pr, GLuint index, GLenum pname, GLint *v) {
    if (!SP_CHECK(pr, 1)) return;
    prog_t *p = prog_of(pr);
    if (index >= (GLuint)p->nblk) { gl_error(GL_INVALID_VALUE); return; }
    switch (pname) {
        case 0x8a3f: *v = (GLint)p->blk[index].binding; return;   // UNIFORM_BLOCK_BINDING
        case 0x8a40: *v = (GLint)p->blk[index].size; return;      // UNIFORM_BLOCK_DATA_SIZE
    }
    RED1("glGetActiveUniformBlockiv(%#x) not implemented", pname);
}
// ARB assembly programs (GL_ARB_fragment_program, reported because DOOM 3 stops without it): DOOM 3 draws with GLSL and never calls these; a game that does is told in a red line,
// its programs get names but no code, and its draws go on with the current GLSL program
GL(glGenProgramsARB, void, GLsizei n, GLuint *out) { static GLuint next = 1; for (GLsizei i = 0; i < n; i++) out[i] = next++; }
GL(glDeleteProgramsARB, void, GLsizei n, const GLuint *v) { (void)n; (void)v; }
GL(glBindProgramARB, void, GLenum target, GLuint pr) { if (pr) RED1("glBindProgramARB(%#x, %u): ARB assembly programs are not implemented", target, pr); }
GL(glProgramStringARB, void, GLenum target, GLenum format, GLsizei len, const void *str) { (void)format; (void)str; RED1("glProgramStringARB(%#x, %d bytes): ARB assembly programs are not implemented", target, len); }
GL(glProgramEnvParameter4fvARB, void, GLenum target, GLuint index, const GLfloat *v) { (void)target; (void)index; (void)v; }
GL(glProgramLocalParameter4fvARB, void, GLenum target, GLuint index, const GLfloat *v) { (void)target; (void)index; (void)v; }
// The current assembly program's properties, asked by its target (not glGetProgramiv, which takes a GLSL program): within the native limits (0x88b6), the rest 0
GL(glGetProgramivARB, void, GLenum target, GLenum pname, GLint *v) { (void)target; *v = pname == 0x88b6; }
// Write count elements from location loc of the current program: comps values each (a matrix: cols columns of comps / cols), into every stage that has the uniform.
// The default block is laid out as std140: a matrix column takes 16 bytes
static void uni_write(GLint loc, GLsizei count, const void *v, int comps, int is_int, int transpose, int cols) {
    prog_t *p = prog_of(S.prog);
    int el, i = uni_of(p, loc, &el);
    if (i < 0 || count <= 0) return;
    puni_t *u = &p->u[i];
    if (is_samp(u->type)) { if (is_int) u->unit = *(const GLint *)v; return; }
    int n = u->count ? u->count : 1, rows = cols ? comps / cols : comps, size = cols ? cols * 16 : 16;
    if (el + count > n) count = n - el;
    for (int st = 0; st < 2; st++) {
        if (u->off[st] < 0) continue;
        int stride = u->stride[st] ? u->stride[st] : size;
        for (GLsizei k = 0; k < count; k++) {
            uint8_t *d = p->ub[st] + u->off[st] + (size_t)(el + k) * stride;
            if (d + size > p->ub[st] + p->si[st]->ub_size + 16) break;
            const float *s = (const float *)v + (size_t)k * comps;
            if (cols) { for (int c = 0; c < cols; c++) for (int r = 0; r < rows; r++) ((float *)(d + c * 16))[r] = transpose ? s[r * cols + c] : s[c * rows + r]; }
            // Conversion callers initialize the number of elements passed in count. The uniform's array bound can only reduce it.
            else memcpy(d, s, (size_t)umin(comps, tcomps(u->type)) * 4);   // NOLINT(clang-analyzer-unix.cstring.UninitializedRead)
        }
        p->ub_gen[st]++;
    }
}
// Integer values for a float uniform are converted (GL would reject them; NVIDIA's driver converts), as are float values for an integer one
static void uni_ints(GLint loc, GLsizei n, const GLint *v, int comps) {
    prog_t *p = prog_of(S.prog);
    int i = uni_of(p, loc, NULL);
    if (i >= 0 && !is_samp(p->u[i].type) && !is_intt(p->u[i].type)) {
        GLsizei len = n > 64 / comps ? 64 / comps : n;
        float f[64]; for (GLsizei k = 0; k < len; k++) for (int c = 0; c < comps; c++) f[k * comps + c] = (float)v[k * comps + c];
        uni_write(loc, len, f, comps, 0, 0, 0); return;
    }
    uni_write(loc, n, v, comps, 1, 0, 0);
}
static void uni_floats(GLint loc, GLsizei n, const GLfloat *v, int comps) {
    prog_t *p = prog_of(S.prog);
    int i = uni_of(p, loc, NULL);
    if (i >= 0 && is_intt(p->u[i].type)) {
        GLsizei len = n > 64 / comps ? 64 / comps : n;
        GLint iv[64]; for (GLsizei k = 0; k < len; k++) for (int c = 0; c < comps; c++) iv[k * comps + c] = (GLint)v[k * comps + c];
        uni_write(loc, len, iv, comps, 1, 0, 0); return;
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
    prog_t *p = prog_of(pr);
    int el, i = uni_of(p, loc, &el);
    if (i < 0) return;
    const puni_t *u = &p->u[i];
    if (is_samp(u->type)) { if (as_int) *(GLint *)out = u->unit; else *(GLfloat *)out = (GLfloat)u->unit; return; }
    int st = u->off[0] >= 0 ? 0 : 1; if (u->off[st] < 0) return;
    int cols = u->type == T_MAT4 ? 4 : u->type == T_MAT3 ? 3 : u->type == T_MAT2 ? 2 : 0, rows = cols ? cols : tcomps(u->type);
    const uint8_t *d = p->ub[st] + u->off[st] + (size_t)el * (u->stride[st] ? u->stride[st] : cols ? cols * 16 : 16);
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

// ---------- EGL ----------
#define EGL_TRUE 1
#define EGL_FALSE 0
static struct { int init; int surface; uint32_t w, h; } g_egl;
static uint64_t g_frames;
// Every 300 frames: draws, passes, uploads; every 1800: memory (the process footprint, Metal's allocations, the GL objects alive), as the NVN layer's memory report
static void frame_end(void) {
    static uint64_t pd, pp, pu, pb, pc;
    uint64_t f = ++g_frames, dr = g_n_draw, pa = g_n_pass, up = g_n_up, bl = g_n_blit, cb = g_n_cb;
    uint64_t sp = g_sk_prog, sq = g_sk_pass, sa = g_sk_attr, so = g_sk_pso;
    static uint64_t pd2, pq, pa2, po;
    if (f <= 3 || f % 300 == 0) {
        GLOG("frame %llu: draws %llu, passes %llu, uploads %llu (%llu by blit), command buffers %llu (since the last record, %llu frames); pipelines so far %llu",
             (unsigned long long)f, (unsigned long long)(dr - pd), (unsigned long long)(pa - pp), (unsigned long long)(up - pu), (unsigned long long)(bl - pb),
             (unsigned long long)(cb - pc), (unsigned long long)(f <= 3 ? 1 : 300), (unsigned long long)g_n_pso);
        pd = dr; pp = pa; pu = up; pb = bl; pc = cb;
        if (sp || sq || sa || so) { GLOG("frame %llu: dropped draws since the last record: %llu no program, %llu no pass, %llu no attribute buffer, %llu no pipeline",
             (unsigned long long)f, (unsigned long long)(sp - pd2), (unsigned long long)(sq - pq), (unsigned long long)(sa - pa2), (unsigned long long)(so - po));
            pd2 = sp; pq = sq; pa2 = sa; po = so; }
    }
    if (f % 1800 == 0) {
        task_vm_info_data_t vi; mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
        uint64_t foot = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n) == KERN_SUCCESS ? vi.phys_footprint : 0;
        uint32_t nt = 0, nb = 0; uint64_t bb = 0;
        for (uint32_t i = 1; i < g_ntexs; i++) nt += g_texs[i] && g_texs[i]->tex != nil;
        for (uint32_t i = 1; i < g_nbufs; i++) if (g_bufs[i] && g_bufs[i]->mb) { nb++; bb += g_bufs[i]->mb.length; }
        GLOG("memory at frame %llu: footprint %.0f MB, Metal allocated %.0f MB; %u textures, %u buffers (%.0f MB), %d transient blocks",
             (unsigned long long)f, foot / 1048576.0, g_dev.currentAllocatedSize / 1048576.0, nt, nb, bb / 1048576.0, g_narena);
    }
    dm_guest_frame_tick();
}
EGL(eglGetDisplay, EGLDisplay, void *native) { (void)native; dev_init(); return (EGLDisplay)&g_egl; }
EGL(eglInitialize, EGLBoolean, EGLDisplay d, EGLint *major, EGLint *minor) { (void)d; dev_init(); g_egl.init = 1; if (major) *major = 1; if (minor) *minor = 5; return EGL_TRUE; }
EGL(eglTerminate, EGLBoolean, EGLDisplay d) { (void)d; return EGL_TRUE; }
// SDL binds 0 (its API before it has chosen one) when it makes the window again (Balatro's love.window.setMode): nothing to change
EGL(eglBindAPI, EGLBoolean, EGLenum api) { if (api && api != 0x30a2) RED1("eglBindAPI(%#x): only desktop OpenGL (0x30a2) is implemented", api); return EGL_TRUE; }
// One configuration: RGBA8, depth 24 (32F here), stencil 8, a window surface, desktop OpenGL; it matches the game's attributes
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
        case 0x3020: *v = 4 * WIN_COLOR_BITS; break;   // BUFFER_SIZE
        case 0x3021: case 0x3022: case 0x3023: case 0x3024: *v = WIN_COLOR_BITS; break;   // ALPHA / BLUE / GREEN / RED_SIZE
        case 0x3025: *v = WIN_DEPTH_BITS; break;   // DEPTH_SIZE
        case 0x3026: *v = WIN_STENCIL_BITS; break;   // STENCIL_SIZE
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
static __thread EGLint t_egl_error = 0x3000;   // SUCCESS: EGL errors belong to the calling thread and clear when read
EGL(eglGetError, EGLint, void) { EGLint e = t_egl_error; t_egl_error = 0x3000; return e; }
EGL(eglQueryAPI, EGLenum, void) { return 0x30a2; }   // OPENGL_API
EGL(eglWaitNative, EGLBoolean, EGLint engine) { (void)engine; return EGL_TRUE; }
EGL(eglWaitGL, EGLBoolean, void) { return EGL_TRUE; }
EGL(eglDestroyContext, EGLBoolean, EGLDisplay d, EGLContext c) { (void)d; (void)c; return EGL_TRUE; }
EGL(eglCreatePbufferSurface, EGLSurface, EGLDisplay d, EGLConfig cfg, const EGLint *attr) { (void)d; (void)cfg; (void)attr; RED1("eglCreatePbufferSurface: drawing into it is not implemented"); return (EGLSurface)(uintptr_t)4; }
EGL(eglCreateContext, EGLContext, EGLDisplay d, EGLConfig cfg, EGLContext share, const EGLint *attr) {
    (void)d; (void)cfg;
    int major = 1, minor = 0, profile = 1, flags = 0;
    for (int i = 0; attr && i < 32 && attr[i] != 0x3038; i += 2) {
        if (attr[i] == 0x3098) major = attr[i + 1];
        else if (attr[i] == 0x30fb) minor = attr[i + 1];
        else if (attr[i] == 0x30fc) flags = attr[i + 1];
        else if (attr[i] == 0x30fd) profile = attr[i + 1];
    }
    if (share) RED1("shared EGL contexts are not implemented (one context)");
    // 3.1 removed fixed function. A forward-compatible 3.0 context also omits it; profile selection starts at 3.2.
    int compat = (major > 3 || (major == 3 && minor >= 2)) && profile == 2;
    g_ctx_fixed = major < 3 || (major == 3 && minor == 0 && !(flags & 2)) || compat;
    g_ctx_profile = g_ctx_fixed ? 2 : 1;
    if (g_ctx_fixed) ff_init();
    GLOG("EGL context: OpenGL %d.%d%s", major, minor, g_ctx_fixed ? " (with fixed function)" : "");
    return (EGLContext)(uintptr_t)2;
}
EGL(eglCreateWindowSurface, EGLSurface, EGLDisplay d, EGLConfig cfg, void *win, const EGLint *attr) {
    (void)d; (void)cfg; (void)win; (void)attr; dev_init();
    int w, h; dm_vi_layer_size(&w, &h); if (w <= 0 || h <= 0) { w = 1280; h = 720; }
    if (P.open) pass_end();
    if (g_win.made && (g_win.w != (uint32_t)w || g_win.h != (uint32_t)h)) g_win.made = 0;
    if (!g_win.made) win_make(w, h);
    g_egl.surface = 1;
    return (EGLSurface)(uintptr_t)3;
}
EGL(eglDestroySurface, EGLBoolean, EGLDisplay d, EGLSurface s) { (void)d; (void)s; return EGL_TRUE; }
// The one context has one owner. A conflicting bind fails without changing either thread's binding; an explicit release or thread exit permits a transfer.
static __thread struct { EGLDisplay dpy; EGLContext ctx; EGLSurface draw, read; } t_cur;
static pthread_mutex_t g_cur_mu = PTHREAD_MUTEX_INITIALIZER;
static void *g_cur_owner;
static pthread_key_t g_cur_key;
static pthread_once_t g_cur_once = PTHREAD_ONCE_INIT;
static int g_cur_key_ok;
static void cur_release(void *binding) { pthread_mutex_lock(&g_cur_mu); if (g_cur_owner == binding) g_cur_owner = NULL; pthread_mutex_unlock(&g_cur_mu); }
static void cur_key_init(void) { g_cur_key_ok = pthread_key_create(&g_cur_key, cur_release) == 0; }
EGL(eglMakeCurrent, EGLBoolean, EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c) {
    static pthread_t th; static int known, sized;
    pthread_once(&g_cur_once, cur_key_init);
    if (!g_cur_key_ok) { t_egl_error = 0x3003; return EGL_FALSE; }   // BAD_ALLOC
    pthread_mutex_lock(&g_cur_mu);
    if (c && g_cur_owner && g_cur_owner != &t_cur) { pthread_mutex_unlock(&g_cur_mu); t_egl_error = 0x3002; return EGL_FALSE; }   // BAD_ACCESS
    if (c) {
        if (pthread_setspecific(g_cur_key, &t_cur)) { pthread_mutex_unlock(&g_cur_mu); t_egl_error = 0x3003; return EGL_FALSE; }
        g_cur_owner = &t_cur; t_cur.dpy = d; t_cur.ctx = c; t_cur.draw = dr; t_cur.read = rd;
    } else { if (g_cur_owner == &t_cur) g_cur_owner = NULL; pthread_setspecific(g_cur_key, NULL); memset(&t_cur, 0, sizeof t_cur); }
    if (c && dr) { pthread_t me = pthread_self(); if (!known || !pthread_equal(th, me)) { th = me; known = 1; OLOG("context current on a new thread"); } }
    // a context's viewport and scissor box start as its first drawable's size (LÖVE reads them back at start)
    if (c && dr && !sized && g_win.made) { sized = 1; if (!S.vp[2] && !S.vp[3]) { S.vp[2] = S.sc[2] = g_win.w; S.vp[3] = S.sc[3] = g_win.h; } }
    pthread_mutex_unlock(&g_cur_mu);
    return EGL_TRUE;
}
EGL(eglReleaseThread, EGLBoolean, void) { if (t_cur.ctx) h_eglMakeCurrent(NULL, NULL, NULL, NULL); t_egl_error = 0x3000; return EGL_TRUE; }
EGL(eglGetCurrentContext, EGLContext, void) { return t_cur.ctx; }
EGL(eglGetCurrentDisplay, EGLDisplay, void) { return t_cur.dpy; }
EGL(eglGetCurrentSurface, EGLSurface, EGLint which) { return which == 0x3059 ? t_cur.draw : which == 0x305a ? t_cur.read : NULL; }   // DRAW, READ
EGL(eglSwapInterval, EGLBoolean, EGLDisplay d, EGLint n) { (void)d; if (n != S.swap_interval) { GLOG("swap interval %d", n); S.swap_interval = n; } return EGL_TRUE; }
// DM_GL_RENDERPROGS=<job file> (the NSP script's shader capture, dm/gfx/tools/idtech4_programs.py): id Tech 4's BFG renderer converts a render program to GLSL only when it first uses
// it, so a capture run that only plays the game gets the ones it drew with. The job names the renderer's own FindVertexShader / FindFragmentShader (this, name) in main, the slot
// of its manager, and every render program of the game's containers; each is called once per program at the first swap after the game has compiled shaders of its own (its
// manager compiles its built-in programs while the renderer starts, before the first frame), at that frame boundary on the thread that renders, and the game converts and
// compiles them all as it would on first use: the capture gets its own text. The calls come back into this layer (glCreateShader …), which a big-stack entry takes on the host
// stack it already runs on
static void renderprogs_job(void) {
    static int done; const char *path = getenv("DM_GL_RENDERPROGS");
    if (done || !path || !g_compiles) return;
    done = 1;
    const module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    FILE *f = fopen(path, "r"); char line[512]; unsigned long long fv = 0, ff = 0, slot = 0;
    if (!m || !f || !fgets(line, sizeof line, f) || sscanf(line, "renderprogs %llx %llx %llx", &fv, &ff, &slot) != 3 || slot + 8 > m->size) {
        GLOG("[RED] DM_GL_RENDERPROGS %s: no job (main %s)", path, m ? "found" : "missing"); if (f) fclose(f); return;
    }
    void *mgr = *(void **)(m->base + slot);
    int (*find_v)(void *, const char *) = (int (*)(void *, const char *))(void *)(m->base + fv), (*find_f)(void *, const char *) = (int (*)(void *, const char *))(void *)(m->base + ff);
    int nv = 0, nf = 0;
    while (mgr && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "v ", 2)) { find_v(mgr, line + 2); nv++; } else if (!strncmp(line, "f ", 2)) { find_f(mgr, line + 2); nf++; }
    }
    fclose(f);
    GLOG("render programs: the game converted %d vertex and %d fragment programs (DM_GL_RENDERPROGS, manager %p)", nv, nf, mgr);
}
static uint64_t g_next_swap;   // pacing without the display link: the earliest time of the next swap
EGL(eglSwapBuffers, EGLBoolean, EGLDisplay d, EGLSurface s) {
    (void)d; (void)s;
    dm_gfx_enter();
    @autoreleasepool {
        if (!g_win.made) { int w, h; dm_vi_layer_size(&w, &h); win_make(w > 0 ? w : 1280, h > 0 ? h : 720); }
        if (g_pend_clear.bits) clears_flush();
        uint64_t v = cb_commit();
        int vs = S.swap_interval < 1 ? 1 : S.swap_interval > 4 ? 4 : S.swap_interval;
        int k = g_win.cur;
        g_win.ticket[k] = mtl_present_ext(g_win.serial[k], vs / 60.0, (__bridge void *)g_ev, v, NULL);
        g_win.cur = (k + 1) % NWIN;
        if (g_win.ticket[g_win.cur]) mtl_present_wait(g_win.ticket[g_win.cur]);   // its previous present has been shown, so drawing into it again is safe
        // Paced by the display link when it runs, as the Vulkan layer's acquire: the swap interval in vsyncs
        uint64_t period = 1000000000ull * vs / 60, now = now_ns();
        if (S.swap_interval > 0 && mtl_link_active() && mtl_link_wait_tick(vs, 2 * period)) g_next_swap = now_ns() + period;
        else if (S.swap_interval > 0) {
            if (g_next_swap > now) { uint64_t w = g_next_swap - now; struct timespec ts = {(time_t)(w / 1000000000ull), (long)(w % 1000000000ull)}; nanosleep(&ts, NULL); }
            now = now_ns(); g_next_swap = g_next_swap && now < g_next_swap + period ? g_next_swap + period : now + period;
        }
    }
    dm_gfx_leave();
    frame_end();
    renderprogs_job();
    return EGL_TRUE;
}
// eglGetProcAddress: GL and EGL names from the same table as direct imports; anything else is null (the game's loader asks for every name it knows).
// An extension's name of a function GL already has gets that function's entry when the registry says the two are the same (gl_aliases.h, written by
// dm/gfx/tools/gl_aliases.py from Khronos gl.xml and a reviewed GL ES list): DOOM 3's renderer asks for its buffer, multitexture and vertex attribute functions only by their
// ARB names (glBindBufferARB), Fate/hollow ataraxia REMASTERED for its framebuffer functions by their EXT names (glGenFramebuffersEXT). A suffix is not stripped on its own:
// glVertexAttribPointerNV takes other arguments than glVertexAttribPointer, glGetProgramivARB asks about an ARB assembly program
static void *gl_entry(const char *name) {
    static const hle_ent *sec; static unsigned long sz;
    if (!sec) sec = (const hle_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swhle", &sz);
    for (unsigned long i = 0; i < sz / sizeof *sec; i++) if (!strcmp(sec[i].name, name)) return sec[i].fn;
    return NULL;
}
static void *gl_lookup(const char *name) {
    void *fn = gl_entry(name);
    for (size_t lo = 0, hi = sizeof gl_aliases / sizeof *gl_aliases; !fn && lo < hi;) {
        size_t mid = (lo + hi) / 2; int c = strcmp(name, gl_aliases[mid].name);
        if (!c) { fn = gl_entry(gl_aliases[mid].same); break; }
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return fn;
}
// DM_GL_STUBS=1 (bring-up): a GL name the layer does not implement gets a stub of its own instead of a null pointer, which reports its first call and returns 0; a game that calls
// such a function then says which one instead of jumping to address 0. Off by default: a loader may take a name's null pointer to mean "not there"
#define NSTUB 4096
static const char *g_stub_name[NSTUB]; static uint8_t g_stub_seen[NSTUB]; static int g_nstub;
uint64_t gl_stub_c(uint64_t k, uint64_t x0, uint64_t x1, uint64_t x2) {
    if (k < NSTUB && !g_stub_seen[k]++) GLOG("[RED] %s called: not implemented, returns 0 (x0=%#llx x1=%#llx x2=%#llx)", g_stub_name[k], x0, x1, x2);
    return 0;
}
__asm__(".text\n.p2align 2\ngl_stub_entry:\n    mov x3, x2\n    mov x2, x1\n    mov x1, x0\n    mov x0, x9\n    b _gl_stub_c\n"
        ".p2align 3\n.globl _gl_stubs\n.private_extern _gl_stubs\n_gl_stubs:\n.set gl_stub_k, 0\n.rept 4096\n    mov x9, #gl_stub_k\n    b gl_stub_entry\n.set gl_stub_k, gl_stub_k + 1\n.endr\n");
extern const char gl_stubs[];   // 8 bytes a stub
static void *gl_stub(const char *name) {
    static int on = -1; if (on < 0) on = getenv("DM_GL_STUBS") && atoi(getenv("DM_GL_STUBS"));
    if (!on || g_nstub == NSTUB) return NULL;
    g_stub_name[g_nstub] = strdup(name);   // a copy: a loader may put each name together in one buffer (Fate/hollow ataraxia REMASTERED), which the next name overwrites
    return (void *)(gl_stubs + 8 * g_nstub++);
}
// A name the loader asks for and the layer does not implement gets a null pointer, which is the documented meaning of "not there" (many loaders ask for the
// whole GL 4.6 list and only call a few). The cost is that calling one jumps to address 0 with nothing in the log to say which: `[RED] CRASH sig=11 addr=0
// pc=0 lr=<the blr's return address>` is that mistake. DM_GL_MISSES=1 prints every missing name once, and DM_GL_STUBS=1 replaces them with stubs that report
// their first call, which names the one the game actually calls (GameMaker's runner asks for 215 names it does not implement and calls glFlush).
static const char *g_miss_name[512]; static int g_nmiss;
static void note_miss(const char *name) {
    for (int i = 0; i < g_nmiss; i++) if (!strcmp(g_miss_name[i], name)) return;   // by text: a loader may put every name together in one buffer
    if (g_nmiss < 512) g_miss_name[g_nmiss++] = strdup(name);
    if (getenv("DM_GL_MISSES")) GLOG("[RED] eglGetProcAddress(%s): not implemented, the game gets a null pointer", name);
}
EGL(eglGetProcAddress, void *, const char *name) {
    static int asked, found; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    int ours = name && (!strncmp(name, "gl", 2) || !strncmp(name, "egl", 3));   // EGL 1.5 answers its own functions too (a glad loader asks for eglGetCurrentDisplay)
    void *fn = ours ? gl_lookup(name) : NULL;
    pthread_mutex_lock(&mu);
    asked++; if (fn) found++;
    if (!fn && ours) note_miss(name);
    if (asked % 500 == 0 || (fn && g_log)) GLOG("eglGetProcAddress: %d names asked, %d implemented, %d not implemented (last %s)", asked, found, g_nmiss, name);
    if (!fn && ours) fn = gl_stub(name);
    pthread_mutex_unlock(&mu);
    return fn;
}
