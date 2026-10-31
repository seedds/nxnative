// The ASTC decoder (dm/gfx/tex/astc_decode.c) against an independent one: Mesa's, through desktop OpenGL on llvmpipe (EGL without a surface; Mesa decodes ASTC itself when the
// driver has no ASTC sampling, its util/texcompress_astc.cpp, into 8-bit texels that glGetTexImage returns). Lavapipe's Vulkan device reports no textureCompressionASTC_LDR
// (Mesa 25.2.8), so this is the check the GPU test t_vk_astc cannot make there. For every block size, linear and sRGB, 256 synthetic blocks of every kind (tests/astc_synth.h:
// normal blocks of random partitions, dual planes, grids, ranges and LDR endpoint modes; void-extent blocks; illegal blocks; blocks with an HDR partition) are decoded by both,
// and every texel must be equal. Linux only and CTest only (dm/host/CMakeLists.txt builds it where EGL and GL are found: the nxnative-build image); it skips (exit 77) where no
// GL context or no GL_KHR_texture_compression_astc_ldr is available.
// Checks: one per format (28), the extension present
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../astc_decode.h"
#include "astc_synth.h"

static int fail, total;
static void t(const char *name, int ok) { total++; fail += !ok; printf("%-100s %s\n", name, ok ? "ok" : "FAIL"); }
static const int BS[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
static int skip(const char *why) { printf("note: %s: skipped\n== t_astc_mesa: 0 checks, 0 failed (skipped) ==\n", why); return 77; }

int main(void) {
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 0); setenv("GALLIUM_DRIVER", "llvmpipe", 0);   // Mesa's software decode, also on a machine whose GPU has ASTC
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay d = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, NULL, NULL)) return skip("no surfaceless EGL display");
    eglBindAPI(EGL_OPENGL_API);
    EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE}; EGLConfig cfg; EGLint n = 0;
    eglChooseConfig(d, ca, &cfg, 1, &n);
    EGLint cx[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT, EGL_NONE};
    EGLContext c = eglCreateContext(d, n ? cfg : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, cx);
    if (c == EGL_NO_CONTEXT || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) return skip("no OpenGL 4.5 context");
    printf("GL %s, renderer %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    int has = 0; GLint ne = 0; glGetIntegerv(GL_NUM_EXTENSIONS, &ne);
    for (GLint i = 0; i < ne; i++) if (!strcmp((const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i), "GL_KHR_texture_compression_astc_ldr")) has = 1;
    if (!has) return skip("no GL_KHR_texture_compression_astc_ldr");
    t("GL_KHR_texture_compression_astc_ldr", has);
    uint64_t rng = 0xA57C; long all = 0, diff = 0, dk[4] = {0}, tk[4] = {0};
    char name[200];
    for (int k = 0; k < 14; k++)
        for (int srgb = 0; srgb < 2; srgb++) {
            int bw = BS[k][0], bh = BS[k][1], nbx = 16, nby = 16, W = nbx * bw, H = nby * bh;
            uint8_t *blk = malloc((size_t)nbx * nby * 16), *ref = malloc((size_t)W * H * 4), *got = malloc((size_t)W * H * 4), *kind = malloc((size_t)nbx * nby);
            if (!blk || !ref || !got || !kind) return 2;
            for (int i = 0; i < nbx * nby; i++) {
                int kk = astc_synth_random(&rng, bw, bh, i % 16, blk + (size_t)i * 16, NULL);
                if (kk < 0) return 2;
                kind[i] = (uint8_t)kk;
            }
            astc_decode_image(blk, (size_t)nbx * 16, (uint32_t)W, (uint32_t)H, bw, bh, srgb, ref, (size_t)W * 4);
            memset(got, 0x5a, (size_t)W * H * 4);
            GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
            GLenum fmt = (srgb ? GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR : GL_COMPRESSED_RGBA_ASTC_4x4_KHR) + (GLenum)k;   // the GL enums follow the block sizes in this order
            glCompressedTexImage2D(GL_TEXTURE_2D, 0, fmt, W, H, 0, nbx * nby * 16, blk);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, got);
            GLenum e = glGetError();
            long fd = 0;
            for (int i = 0; i < W * H; i++) {
                int x = i % W, y = i / W, kk = kind[(y / bh) * nbx + x / bw];
                tk[kk]++;
                if (memcmp(got + 4 * i, ref + 4 * i, 4)) {
                    if (!fd) printf("  %dx%d %s: first difference at %d,%d (block kind %d): Mesa %d %d %d %d, astc_decode.c %d %d %d %d\n", bw, bh, srgb ? "sRGB" : "linear", x, y, kk,
                                    got[4 * i], got[4 * i + 1], got[4 * i + 2], got[4 * i + 3], ref[4 * i], ref[4 * i + 1], ref[4 * i + 2], ref[4 * i + 3]);
                    fd++; dk[kk]++;
                }
            }
            snprintf(name, sizeof name, "ASTC %dx%d %s: 256 blocks, %d texels: Mesa's decode = astc_decode.c", bw, bh, srgb ? "sRGB" : "linear", W * H);
            t(name, e == GL_NO_ERROR && fd == 0);
            all += W * H; diff += fd;
            glDeleteTextures(1, &tex); free(blk); free(ref); free(got); free(kind);
        }
    printf("Mesa's ASTC decode against astc_decode.c: %ld of %ld texels differ; by block kind: normal %ld of %ld, void-extent %ld of %ld, illegal %ld of %ld, HDR partition %ld of %ld\n",
           diff, all, dk[0], tk[0], dk[1], tk[1], dk[2], tk[2], dk[3], tk[3]);
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); eglDestroyContext(d, c); eglTerminate(d);
    printf("== t_astc_mesa: %d checks, %d failed ==\n", total, fail);
    return fail != 0;
}
