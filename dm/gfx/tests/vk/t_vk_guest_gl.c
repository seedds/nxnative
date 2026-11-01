// t_vk_guest_gl: the guest OpenGL layer (dm/gfx/hostvk/guest_gl.c; docs/design/vulkan-backend.md 8.2, step P9b) driven as a game drives it: the EGL functions and
// eglGetProcAddress found by name in the import table (as the loader binds a game's imports), every GL function from eglGetProcAddress. On the backend's device (lavapipe under
// the validation layer with synchronization validation in CTest), over a 64x48 window surface, with the GLSL of dm/gfx/tests/vk/shaders/gl translated by gl_shaders.py --vk in
// the build (CMakeLists.txt): programs in Digimon's dialect (GLSL ES 3.20) and LÖVE's (#version 330 core, attributes bound with glBindAttribLocation, varyings declared in
// another order in each stage), a texture defined level by level and its levels picked by the size of the quads, glUniform* between draws, depth through the clip-space pass,
// culling by GL's winding, scissored and masked clears, gl_FragCoord in GL's orientation, a persistently mapped vertex stream rewritten after its fence, an index buffer
// orphaned while a draw reads it, a disabled attribute's current value, a triangle fan, uploads before and among a frame's draws, a framebuffer object sampled and blitted
// (straight, scaled and flipped), mipmaps generated (a texture of all its levels, and one of one level), glReadPixels (rows bottom first, as GL lays them out) and the
// presented picture read back from the backend's headless presenter (upright: it flips the bottom-up rows). Every validation message fails the test; without a Vulkan device
// it skips as the other GPU tests do (t_vk_harness.h), and without a shader library (no glslangValidator when the tree was configured, or a runner on another machine) it
// skips with a note. DM_GL_STUBS: a child process (hv_spawn) checks the reporting stub
#include "t_vk_harness.h"
#include "../../../host/hle.h"
#include "../../hostvk/guest_gl.h"
#include "../../hostvk/guest_vk.h"

PLAT_TABLE_DEFINE(swhle)   // the import table (dm/host/hle.c defines it in a host binary)
enum { WW = 64, WH = 48 };
Result h_vi_CreateLayerWH(void **out, void *disp, int w, int h);   // dm/gfx/hle_vi.c (linked): the game's vi::CreateLayer of a given size, which the window surface takes

typedef uint32_t GLenum, GLuint, GLbitfield; typedef int32_t GLint, GLsizei; typedef float GLfloat; typedef intptr_t GLsizeiptr, GLintptr; typedef char GLchar;
enum {
    GL_TRIANGLES = 4, GL_TRIANGLE_FAN = 6, GL_LESS = 0x201, GL_FRONT = 0x404, GL_BACK = 0x405, GL_CULL_FACE = 0xb44, GL_DEPTH_TEST = 0xb71, GL_VIEWPORT = 0xba2,
    GL_SCISSOR_TEST = 0xc11, GL_TEXTURE_2D = 0xde1, GL_UNSIGNED_BYTE = 0x1401, GL_UNSIGNED_SHORT = 0x1403, GL_FLOAT = 0x1406, GL_COLOR = 0x1800, GL_RGBA = 0x1908,
    GL_RENDERER = 0x1f01, GL_NEAREST = 0x2600, GL_NEAREST_MIPMAP_NEAREST = 0x2700, GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER = 0x2801, GL_RGBA8 = 0x8058,
    GL_TEXTURE0 = 0x84c0, GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER = 0x8893, GL_STATIC_DRAW = 0x88e4, GL_DEPTH24_STENCIL8 = 0x88f0, GL_FRAGMENT_SHADER = 0x8b30,
    GL_VERTEX_SHADER = 0x8b31, GL_COMPILE_STATUS = 0x8b81, GL_LINK_STATUS = 0x8b82, GL_ACTIVE_UNIFORMS = 0x8b86, GL_FLOAT_T = 0x1406, GL_BOOL = 0x8b56,
    GL_READ_FRAMEBUFFER = 0x8ca8, GL_DRAW_FRAMEBUFFER = 0x8ca9, GL_COLOR_ATTACHMENT0 = 0x8ce0, GL_FRAMEBUFFER = 0x8d40, GL_RENDERBUFFER = 0x8d41,
    GL_DEPTH_STENCIL_ATTACHMENT = 0x821a, GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117, GL_ALREADY_SIGNALED = 0x911a, GL_CONDITION_SATISFIED = 0x911c, GL_CONTEXT_PROFILE_MASK = 0x9126,
    GL_MAP_WRITE_BIT = 0x2, GL_MAP_PERSISTENT_BIT = 0x40, GL_MAP_COHERENT_BIT = 0x80, GL_SYNC_FLUSH_COMMANDS_BIT = 0x1,
    GL_COLOR_BUFFER_BIT = 0x4000, GL_DEPTH_BUFFER_BIT = 0x100,
};

// ---------- The game's function pointers ----------
#define EGLFNS(X) X(eglGetDisplay, void *, (void *)) X(eglInitialize, uint32_t, (void *, int32_t *, int32_t *)) X(eglBindAPI, uint32_t, (uint32_t)) \
    X(eglChooseConfig, uint32_t, (void *, const int32_t *, void **, int32_t, int32_t *)) X(eglGetConfigAttrib, uint32_t, (void *, void *, int32_t, int32_t *)) \
    X(eglCreateContext, void *, (void *, void *, void *, const int32_t *)) X(eglCreateWindowSurface, void *, (void *, void *, void *, const int32_t *)) \
    X(eglMakeCurrent, uint32_t, (void *, void *, void *, void *)) X(eglSwapInterval, uint32_t, (void *, int32_t)) X(eglSwapBuffers, uint32_t, (void *, void *)) \
    X(eglGetProcAddress, void *, (const char *))
#define GLFNS(X) \
    X(glGenBuffers, void, (GLsizei, GLuint *)) X(glBindBuffer, void, (GLenum, GLuint)) X(glBufferData, void, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(glBufferStorage, void, (GLenum, GLsizeiptr, const void *, GLbitfield)) X(glMapBufferRange, void *, (GLenum, GLintptr, GLsizeiptr, GLbitfield)) \
    X(glGenVertexArrays, void, (GLsizei, GLuint *)) X(glBindVertexArray, void, (GLuint)) X(glEnableVertexAttribArray, void, (GLuint)) X(glDisableVertexAttribArray, void, (GLuint)) \
    X(glVertexAttribPointer, void, (GLuint, GLint, GLenum, uint32_t, GLsizei, const void *)) X(glVertexAttrib4f, void, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(glCreateShader, GLuint, (GLenum)) X(glShaderSource, void, (GLuint, GLsizei, const GLchar *const *, const GLint *)) X(glCompileShader, void, (GLuint)) \
    X(glGetShaderiv, void, (GLuint, GLenum, GLint *)) X(glCreateProgram, GLuint, (void)) X(glAttachShader, void, (GLuint, GLuint)) \
    X(glBindAttribLocation, void, (GLuint, GLuint, const GLchar *)) X(glLinkProgram, void, (GLuint)) X(glGetProgramiv, void, (GLuint, GLenum, GLint *)) \
    X(glUseProgram, void, (GLuint)) X(glGetAttribLocation, GLint, (GLuint, const GLchar *)) X(glGetUniformLocation, GLint, (GLuint, const GLchar *)) \
    X(glUniform1i, void, (GLint, GLint)) X(glUniform1f, void, (GLint, GLfloat)) X(glUniform2f, void, (GLint, GLfloat, GLfloat)) \
    X(glUniform4f, void, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) X(glUniform1fv, void, (GLint, GLsizei, const GLfloat *)) \
    X(glUniformMatrix4fv, void, (GLint, GLsizei, uint32_t, const GLfloat *)) X(glGetActiveUniform, void, (GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *)) \
    X(glGetUniformfv, void, (GLuint, GLint, GLfloat *)) X(glGenTextures, void, (GLsizei, GLuint *)) X(glBindTexture, void, (GLenum, GLuint)) X(glActiveTexture, void, (GLenum)) \
    X(glTexImage2D, void, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(glTexSubImage2D, void, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) X(glTexParameteri, void, (GLenum, GLenum, GLint)) \
    X(glTexStorage2D, void, (GLenum, GLsizei, GLenum, GLsizei, GLsizei)) X(glGenerateMipmap, void, (GLenum)) X(glGenFramebuffers, void, (GLsizei, GLuint *)) \
    X(glBindFramebuffer, void, (GLenum, GLuint)) X(glFramebufferTexture2D, void, (GLenum, GLenum, GLenum, GLuint, GLint)) X(glGenRenderbuffers, void, (GLsizei, GLuint *)) \
    X(glBindRenderbuffer, void, (GLenum, GLuint)) X(glRenderbufferStorage, void, (GLenum, GLenum, GLsizei, GLsizei)) \
    X(glFramebufferRenderbuffer, void, (GLenum, GLenum, GLenum, GLuint)) X(glBlitFramebuffer, void, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(glClearBufferfv, void, (GLenum, GLint, const GLfloat *)) X(glViewport, void, (GLint, GLint, GLsizei, GLsizei)) X(glScissor, void, (GLint, GLint, GLsizei, GLsizei)) \
    X(glEnable, void, (GLenum)) X(glDisable, void, (GLenum)) X(glCullFace, void, (GLenum)) X(glClearColor, void, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(glClearDepthf, void, (GLfloat)) X(glClear, void, (GLbitfield)) X(glColorMask, void, (uint32_t, uint32_t, uint32_t, uint32_t)) X(glDepthFunc, void, (GLenum)) \
    X(glDrawArrays, void, (GLenum, GLint, GLsizei)) X(glDrawElements, void, (GLenum, GLsizei, GLenum, const void *)) \
    X(glDrawElementsBaseVertex, void, (GLenum, GLsizei, GLenum, const void *, GLint)) X(glFenceSync, void *, (GLenum, GLbitfield)) \
    X(glClientWaitSync, GLenum, (void *, GLbitfield, uint64_t)) X(glDeleteSync, void, (void *)) X(glReadPixels, void, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    X(glGetIntegerv, void, (GLenum, GLint *)) X(glGetString, const char *, (GLenum)) X(glFinish, void, (void))
static struct {
#define DECL(n, r, a) r (*n) a;
    EGLFNS(DECL) GLFNS(DECL)
#undef DECL
} G;
static void *import(const char *name) {   // a game import, bound by name through the import table as the loader binds it
    size_t sz = 0; const hle_ent *e = plat_table_swhle(&sz);
    for (size_t i = 0; e && i < sz / sizeof *e; i++) if (e[i].name && !strcmp(e[i].name, name)) return e[i].fn;
    return NULL;
}
static int g_missing;
#define LOADE(n, r, a) if (!(G.n = (r (*) a)import(#n))) { g_missing++; printf("missing %s\n", #n); }
#define LOADG(n, r, a) if (!(G.n = (r (*) a)G.eglGetProcAddress(#n))) { g_missing++; printf("missing %s\n", #n); }

static int near(int a, int b, int tol) { return a - b <= tol && b - a <= tol; }
static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = calloc(1, (size_t)n + 1); if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f); return b;
}
static GLuint shader(GLenum type, const char *file) {   // the source text as a game hands it to glShaderSource (its hash finds the module)
    char p[800]; snprintf(p, sizeof p, "%s/%s", T_GL_SHADERS, file);
    char *src = slurp(p); if (!src) { printf("cannot read %s\n", p); return 0; }
    GLuint s = G.glCreateShader(type); const GLchar *s0 = src;
    G.glShaderSource(s, 1, &s0, NULL); G.glCompileShader(s); free(src);
    GLint ok = 0; G.glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    return ok ? s : 0;
}
static GLuint program(GLuint vs, GLuint fs, int love) {
    GLuint p = G.glCreateProgram(); G.glAttachShader(p, vs); G.glAttachShader(p, fs);
    if (love) { G.glBindAttribLocation(p, 2, "VertexColor"); G.glBindAttribLocation(p, 0, "VertexPosition"); }   // LÖVE's indices, bound before the link
    G.glLinkProgram(p);
    return p;
}
// A rectangle of pixels [x0, x1) x [y0, y1) (GL's window coordinates: y from the bottom row) of a w x h target, as quad.vert's uRect (offset xy, scale zw in NDC)
static void rect(GLint loc, int x0, int y0, int x1, int y1, int w, int h) {
    G.glUniform4f(loc, -1 + 2.0f * x0 / w, -1 + 2.0f * y0 / h, 2.0f * (x1 - x0) / w, 2.0f * (y1 - y0) / h);
}
// A quad of the stream (VertexPosition 2 floats, VertexColor 4 bytes: 12 bytes a vertex), corners counter-clockwise from the bottom left
static void stream_quad(uint8_t *v, int x0, int y0, int x1, int y1, const uint8_t c[4]) {
    const float px[4] = {-1 + 2.0f * x0 / WW, -1 + 2.0f * x1 / WW, -1 + 2.0f * x1 / WW, -1 + 2.0f * x0 / WW}, py[4] = {-1 + 2.0f * y0 / WH, -1 + 2.0f * y0 / WH, -1 + 2.0f * y1 / WH, -1 + 2.0f * y1 / WH};
    for (int i = 0; i < 4; i++) { memcpy(v + 12 * i, &px[i], 4); memcpy(v + 12 * i + 4, &py[i], 4); memcpy(v + 12 * i + 8, c, 4); }
}
static uint8_t g_rp[WW * WH * 4], g_shot[WW * WH * 4];
static const uint8_t *rp(int x, int y) { return g_rp + ((size_t)y * WW + (size_t)x) * 4; }   // glReadPixels: GL's rows, the bottom one first
static const uint8_t *shot(int x, int y) { return g_shot + ((size_t)(WH - 1 - y) * WW + (size_t)x) * 4; }   // the presented picture, upright: GL row y is row WH - 1 - y
static int is(const uint8_t *p, int r, int g, int b, int tol) { return near(p[0], r, tol) && near(p[1], g, tol) && near(p[2], b, tol) && p[3] == 255; }
static void px(const char *what, const uint8_t *p, int r, int g, int b, int tol) {
    char name[300]; snprintf(name, sizeof name, "%s: %u %u %u %u (%d %d %d 255)", what, p[0], p[1], p[2], p[3], r, g, b);
    t(name, is(p, r, g, b, tol));
}
static int grad(int i, int n) { return (int)((i + 0.5) / n * 255 + 0.5); }   // coord.frag's channel for pixel i of n (gl_FragCoord is the pixel's centre)

// The child of the DM_GL_STUBS check: a name the layer does not implement gets a stub that returns 0 (and says so once) instead of a null pointer
static int stubs_child(void) {
    setenv("DM_GL_STUBS", "1", 1);
    void *(*gpa)(const char *) = (void *(*)(const char *))import("eglGetProcAddress");
    uint64_t (*f)(uint64_t, uint64_t, uint64_t) = gpa ? (uint64_t (*)(uint64_t, uint64_t, uint64_t))gpa("glNotAFunctionOfTheLayer") : NULL;
    uint64_t r = f ? f(1, 2, 3) : 1;
    printf("DM_GL_STUBS=1: stub %p, returns %llu\n", (void *)f, (unsigned long long)r);
    return f && r == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "stubs")) return stubs_child();
    hv_up("t_vk_guest_gl");
    char tsv[700]; snprintf(tsv, sizeof tsv, "%s/gl-vk/gl_shaders.tsv", T_GL_LIB);
#if defined(T_GL_NO_GLSLANG)
    printf("note: glslangValidator was not found when the tree was configured: no GL shader library, skipped\n");
    return hv_finish();
#endif
    FILE *lf = fopen(tsv, "r");
    if (!lf) {
        if (!vk_required()) { printf("note: no GL shader library at %s (gl_shaders.py --vk in the build): skipped\n", tsv); return hv_finish(); }
        t("the GL shader library is built (gl_shaders.py --vk)", 0); return hv_finish();
    }
    fclose(lf);
    setenv("DM_MSL_DIR", T_GL_LIB, 1);   // the layer finds <DM_MSL_DIR>/gl-vk at its start (the backend has already found its own library)
    void *layer = NULL; h_vi_CreateLayerWH(&layer, NULL, WW, WH);   // the layer's size, as the game sets it before its window surface (hle_vi.c's state, which the layer reads)
    guest_gl_stats st0; guest_gl_stats_get(&st0);   // a reference to the layer's object links it, with its entries, from its static library

    // ---------- EGL, as SDL and MVGL bring it up ----------
    EGLFNS(LOADE)
    if (g_missing) { t("the EGL functions are entries of the import table", 0); return hv_finish(); }
    void *dpy = G.eglGetDisplay(NULL); int32_t maj = 0, min = 0;
    t("eglInitialize: EGL 1.5", G.eglInitialize(dpy, &maj, &min) == 1 && maj == 1 && min == 5);
    t("eglBindAPI(EGL_OPENGL_API)", G.eglBindAPI(0x30a2) == 1);
    static const int32_t cattr[] = {0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3025, 24, 0x3026, 8, 0x3038};
    void *cfg = NULL; int32_t ncfg = 0, depth = 0;
    G.eglChooseConfig(dpy, cattr, &cfg, 1, &ncfg); G.eglGetConfigAttrib(dpy, cfg, 0x3025, &depth);
    t("eglChooseConfig: one configuration, depth 24", ncfg == 1 && cfg && depth == 24);
    static const int32_t xattr[] = {0x3098, 4, 0x30fb, 5, 0x3038};
    void *ctx = G.eglCreateContext(dpy, cfg, NULL, xattr), *surf = G.eglCreateWindowSurface(dpy, cfg, (void *)(uintptr_t)0x1000, NULL);
    t("a 4.5 context, a window surface, made current", ctx && surf && G.eglMakeCurrent(dpy, surf, surf, ctx) == 1 && G.eglSwapInterval(dpy, 1) == 1);
    GLFNS(LOADG)
    char name[300]; snprintf(name, sizeof name, "eglGetProcAddress: every GL function the test calls (%d missing); null for a name the layer lacks", g_missing);
    t(name, g_missing == 0 && G.eglGetProcAddress("glNotAFunctionOfTheLayer") == NULL && G.eglGetProcAddress("vkCreateInstance") == NULL);
    if (g_missing) return hv_finish();
    GLint vp[4] = {0}, prof = 0; G.glGetIntegerv(GL_VIEWPORT, vp); G.glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &prof);
    const char *rend = G.glGetString(GL_RENDERER);
    snprintf(name, sizeof name, "the context: core profile, viewport %d %d %d %d (the surface's 64x48), renderer '%s'", vp[0], vp[1], vp[2], vp[3], rend ? rend : "");
    t(name, prof == 1 && vp[0] == 0 && vp[1] == 0 && vp[2] == WW && vp[3] == WH && rend && strstr(rend, "Vulkan"));
    int status = hv_spawn(argv[0], "stubs");
    snprintf(name, sizeof name, "DM_GL_STUBS=1 (a child process): a name the layer lacks gets a stub that returns 0 (exit status %d)", status);
    t(name, status == 0);

    // ---------- Programs ----------
    GLuint vq = shader(GL_VERTEX_SHADER, "quad.vert"), fq = shader(GL_FRAGMENT_SHADER, "quad.frag"), fc = shader(GL_FRAGMENT_SHADER, "coord.frag");
    GLuint vs = shader(GL_VERTEX_SHADER, "stream.vert"), fs = shader(GL_FRAGMENT_SHADER, "stream.frag");
    t("five shaders compile (found in the library by the hash of their source)", vq && fq && fc && vs && fs);
    GLuint P1 = program(vq, fq, 0), P3 = program(vq, fc, 0), P2 = program(vs, fs, 1);
    GLint l1 = 0, l2 = 0, l3 = 0; G.glGetProgramiv(P1, GL_LINK_STATUS, &l1); G.glGetProgramiv(P2, GL_LINK_STATUS, &l2); G.glGetProgramiv(P3, GL_LINK_STATUS, &l3);
    GLint aPos = G.glGetAttribLocation(P1, "aPos"), aUV = G.glGetAttribLocation(P1, "aUV"), aVP = G.glGetAttribLocation(P2, "VertexPosition"), aVC = G.glGetAttribLocation(P2, "VertexColor");
    snprintf(name, sizeof name, "glGetAttribLocation: aPos %d aUV %d (their locations); VertexPosition %d VertexColor %d (glBindAttribLocation's)", aPos, aUV, aVP, aVC);
    t(name, l1 && l2 && l3 && aPos == 0 && aUV == 1 && aVP == 0 && aVC == 2);
    GLint nu = 0; G.glGetProgramiv(P2, GL_ACTIVE_UNIFORMS, &nu);
    int found = 0;
    for (GLint i = 0; i < nu; i++) {
        char un[64]; GLsizei len = 0; GLint cnt = 0; GLenum ty = 0; G.glGetActiveUniform(P2, (GLuint)i, sizeof un, &len, &cnt, &ty, un);
        found += !strcmp(un, "uScale[0]") && cnt == 2 && ty == GL_FLOAT_T; found += !strcmp(un, "uInvert") && ty == GL_BOOL && cnt == 1;
    }
    snprintf(name, sizeof name, "glGetActiveUniform: %d uniforms; uScale[0] (2 floats) and uInvert (a GLSL bool) as declared", nu);
    t(name, nu == 3 && found == 2);
    GLint uRect = G.glGetUniformLocation(P1, "uRect"), uDepth = G.glGetUniformLocation(P1, "uDepth"), uColor = G.glGetUniformLocation(P1, "uColor"), uTex = G.glGetUniformLocation(P1, "uTex");
    GLint cRect = G.glGetUniformLocation(P3, "uRect"), cDepth = G.glGetUniformLocation(P3, "uDepth"), cSize = G.glGetUniformLocation(P3, "uSize");
    GLint sTr = G.glGetUniformLocation(P2, "uTransform"), sScale = G.glGetUniformLocation(P2, "uScale"), sScale1 = G.glGetUniformLocation(P2, "uScale[1]"), sInv = G.glGetUniformLocation(P2, "uInvert");
    t("glGetUniformLocation: every uniform; uScale[1] one past uScale", uRect >= 0 && uDepth >= 0 && uColor >= 0 && uTex >= 0 && cRect >= 0 && cDepth >= 0 && cSize >= 0 && sTr >= 0 &&
      sScale >= 0 && sInv >= 0 && sScale1 == sScale + 1);

    // ---------- Vertex data, the texture defined level by level ----------
    static const float quad[6][4] = {{0, 0, 0, 0}, {1, 0, 1, 0}, {1, 1, 1, 1}, {0, 0, 0, 0}, {1, 1, 1, 1}, {0, 1, 0, 1}};   // position, uv: two counter-clockwise triangles
    GLuint vao1, vao2, vbo, ibo, sbo; G.glGenVertexArrays(1, &vao1); G.glGenVertexArrays(1, &vao2);
    G.glGenBuffers(1, &vbo); G.glGenBuffers(1, &ibo); G.glGenBuffers(1, &sbo);
    G.glBindVertexArray(vao1); G.glBindBuffer(GL_ARRAY_BUFFER, vbo); G.glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    G.glEnableVertexAttribArray((GLuint)aPos); G.glVertexAttribPointer((GLuint)aPos, 2, GL_FLOAT, 0, 16, (const void *)0);
    G.glEnableVertexAttribArray((GLuint)aUV); G.glVertexAttribPointer((GLuint)aUV, 2, GL_FLOAT, 0, 16, (const void *)8);
    GLuint T; G.glGenTextures(1, &T); G.glActiveTexture(GL_TEXTURE0); G.glBindTexture(GL_TEXTURE_2D, T);
    G.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST); G.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    uint8_t lv0[16 * 4], lv1[4 * 4], lv2[4];
    for (int i = 0; i < 16; i++) memcpy(lv0 + 4 * i, (const uint8_t[]){255, 0, 0, 255}, 4);
    for (int i = 0; i < 4; i++) memcpy(lv1 + 4 * i, (const uint8_t[]){0, 255, 0, 255}, 4);
    memcpy(lv2, (const uint8_t[]){0, 0, 255, 255}, 4);
    G.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, lv0);   // as Digimon's loader: one level at a time, the texture made at its first use
    G.glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, lv1);
    G.glTexImage2D(GL_TEXTURE_2D, 2, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, lv2);
    // the stream: immutable storage mapped for good (LÖVE's StreamBufferPersistentMapSync), an index buffer, the attributes at glBindAttribLocation's indices
    G.glBindVertexArray(vao2); G.glBindBuffer(GL_ARRAY_BUFFER, sbo);
    G.glBufferStorage(GL_ARRAY_BUFFER, 4096, NULL, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
    uint8_t *map = G.glMapBufferRange(GL_ARRAY_BUFFER, 0, 4096, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
    t("glBufferStorage + glMapBufferRange: a persistent mapping", map != NULL);
    if (!map) return hv_finish();
    G.glEnableVertexAttribArray((GLuint)aVP); G.glVertexAttribPointer((GLuint)aVP, 2, GL_FLOAT, 0, 12, (const void *)0);
    G.glEnableVertexAttribArray((GLuint)aVC); G.glVertexAttribPointer((GLuint)aVC, 4, GL_UNSIGNED_BYTE, 1, 12, (const void *)8);
    static const uint16_t quad_idx[6] = {0, 1, 2, 0, 2, 3}, tri_idx[6] = {0, 1, 2, 0, 1, 2};
    G.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo); G.glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof quad_idx, quad_idx, GL_STATIC_DRAW);
    static const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    // ---------- Frame 1 ----------
    G.glViewport(0, 0, WW, WH); G.glClearColor(0, 0, 0, 1); G.glClearDepthf(1); G.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    G.glEnable(GL_DEPTH_TEST); G.glDepthFunc(GL_LESS);
    G.glBindVertexArray(vao1); G.glUseProgram(P1); G.glUniform1i(uTex, 0); G.glBindTexture(GL_TEXTURE_2D, T);
    G.glUniform4f(uColor, 1, 1, 1, 1); G.glUniform1f(uDepth, -0.5f);   // window depth 0.25 through the clip-space pass; Vulkan would clip it away without it
    G.glEnable(GL_CULL_FACE); G.glCullFace(GL_BACK);
    rect(uRect, 0, 0, 8, 8, WW, WH); G.glDrawArrays(GL_TRIANGLES, 0, 6);     // A: 8 pixels for 4 texels, level 0 (red); counter-clockwise, so not culled
    G.glCullFace(GL_FRONT);
    rect(uRect, 0, 8, 8, 16, WW, WH); G.glDrawArrays(GL_TRIANGLES, 0, 6);    // culled: GL's front faces
    G.glDisable(GL_CULL_FACE);
    rect(uRect, 8, 0, 10, 2, WW, WH); G.glDrawArrays(GL_TRIANGLES, 0, 6);    // B: 2 pixels for 4 texels, level 1 (green)
    rect(uRect, 10, 0, 11, 1, WW, WH); G.glDrawArrays(GL_TRIANGLES, 0, 6);   // C: 1 pixel, level 2 (blue)
    G.glUniform4f(uColor, 0, 0, 1, 1); G.glUniform1f(uDepth, 0.5f);        // D: over A at depth 0.75, which fails GL_LESS
    rect(uRect, 0, 0, 8, 8, WW, WH); G.glDrawArrays(GL_TRIANGLES, 0, 6);
    G.glUniform4f(uColor, 0.5f, 1, 1, 1); G.glUniform1f(uDepth, -0.5f);     // E: glUniform between draws, red halved
    rect(uRect, 12, 0, 20, 8, WW, WH); G.glDrawArrays(GL_TRIANGLES, 0, 6);
    G.glDisable(GL_DEPTH_TEST);
    G.glUseProgram(P3); G.glUniform2f(cSize, WW, WH); G.glUniform1f(cDepth, 0); rect(cRect, 0, 0, WW, WH, WW, WH);
    G.glEnable(GL_SCISSOR_TEST); G.glScissor(32, 24, 32, 24); G.glDrawArrays(GL_TRIANGLES, 0, 6);   // gl_FragCoord over the upper right quarter (GL's y up)
    G.glScissor(0, 40, 8, 8); G.glClearColor(1, 1, 0, 1); G.glClear(GL_COLOR_BUFFER_BIT);               // a scissored clear: yellow, top left
    G.glScissor(8, 40, 8, 8); G.glColorMask(0, 1, 0, 1); G.glClearColor(1, 1, 1, 1); G.glClear(GL_COLOR_BUFFER_BIT);   // a masked clear: green only
    G.glColorMask(1, 1, 1, 1); G.glDisable(GL_SCISSOR_TEST);
    G.glBindVertexArray(vao2); G.glUseProgram(P2);
    G.glUniformMatrix4fv(sTr, 1, 0, ident); const float sc1[2] = {1, 1}; G.glUniform1fv(sScale, 2, sc1); G.glUniform1i(sInv, 0);
    stream_quad(map, 16, 16, 24, 24, (const uint8_t[]){0, 0, 255, 255});
    G.glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const void *)0);   // blue: varyings matched by name, attributes at the bound indices
    void *fence = G.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    G.glReadPixels(0, 0, WW, WH, GL_RGBA, GL_UNSIGNED_BYTE, g_rp);
    px("glReadPixels (row 0 at the bottom), A: level 0 at depth 0.25, culling GL's back faces", rp(2, 2), 255, 0, 0, 0);
    px("the quad drawn with GL's front faces culled is not there", rp(2, 12), 0, 0, 0, 0);
    px("B: a 2-pixel quad samples level 1", rp(9, 1), 0, 255, 0, 0);
    px("C: a 1-pixel quad samples level 2", rp(10, 0), 0, 0, 255, 0);
    px("E: uColor changed between draws", rp(15, 4), 128, 0, 0, 1);
    px("gl_FragCoord at GL (40, 30): red across, green up", rp(40, 30), grad(40, WW), grad(30, WH), 0, 1);
    px("gl_FragCoord at GL (63, 47), the top right pixel", rp(63, 47), grad(63, WW), grad(47, WH), 0, 1);
    px("the scissor kept the gradient out of the lower right quarter", rp(40, 10), 0, 0, 0, 0);
    px("a scissored clear at the top left (GL y 40 to 47)", rp(3, 44), 255, 255, 0, 0);
    px("a masked clear: green only", rp(12, 44), 0, 255, 0, 0);
    px("the stream: blue, its varyings matched by name", rp(20, 20), 0, 0, 255, 0);
    G.glEnable(GL_SCISSOR_TEST); G.glScissor(30, 0, 2, 2); G.glClearColor(1, 0, 1, 1); G.glClear(GL_COLOR_BUFFER_BIT); G.glDisable(GL_SCISSOR_TEST);   // after the readback, in the same frame
    G.eglSwapBuffers(dpy, surf);
    hvk_present_info info; memset(&info, 0, sizeof info);
    int got = hvk_present_last(g_shot, sizeof g_shot, &info);
    snprintf(name, sizeof name, "present 1: %ux%u format %d, %llu presents", info.width, info.height, info.format, (unsigned long long)info.frames);
    t(name, got && info.width == WW && info.height == WH && info.format == VK_FORMAT_R8G8B8A8_UNORM && info.frames == 1);
    px("present 1, upright: yellow at the top left", g_shot + ((size_t)2 * WW + 3) * 4, 255, 255, 0, 0);
    px("present 1, upright: red at the bottom left", g_shot + ((size_t)(WH - 3) * WW + 2) * 4, 255, 0, 0, 0);
    px("present 1: the clear recorded after glReadPixels (magenta, GL (30, 0))", shot(30, 0), 255, 0, 255, 0);
    px("present 1: the gradient's top right pixel is the picture's first row", g_shot + ((size_t)0 * WW + 63) * 4, grad(63, WW), grad(47, WH), 0, 1);

    // ---------- Frame 2 ----------
    GLenum w = G.glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
    t("glClientWaitSync on frame 1's fence: signalled", w == GL_ALREADY_SIGNALED || w == GL_CONDITION_SATISFIED);
    G.glDeleteSync(fence);
    G.glClearColor(0, 0, 0, 1); G.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    stream_quad(map, 24, 16, 32, 24, (const uint8_t[]){255, 0, 255, 255});        // the same memory, rewritten after the fence
    stream_quad(map + 48, 40, 16, 48, 24, (const uint8_t[]){255, 255, 0, 255});
    stream_quad(map + 96, 48, 0, 56, 8, (const uint8_t[]){255, 255, 255, 255});
    G.glBindVertexArray(vao2); G.glUseProgram(P2);
    const float sc2[2] = {1, 0.5f}; G.glUniform1fv(sScale, 2, sc2);
    float back = 0; G.glGetUniformfv(P2, sScale1, &back);
    t("glGetUniformfv reads uScale[1] back from the program's copy", back == 0.5f);
    G.glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const void *)0);                     // Q1: the whole quad, halved
    G.glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof tri_idx, tri_idx, GL_STATIC_DRAW);          // orphaned: Q1's draw keeps its indices
    G.glDrawElementsBaseVertex(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (const void *)0, 4);       // Q2: one triangle, the lower right half
    G.glDisableVertexAttribArray((GLuint)aVC); G.glVertexAttrib4f((GLuint)aVC, 0, 1, 0, 1);   // LÖVE's VertexColor: a disabled array, its current value
    G.glDrawArrays(GL_TRIANGLE_FAN, 8, 4);                                                    // S3: a fan, green halved
    G.glEnableVertexAttribArray((GLuint)aVC);
    G.glBindVertexArray(vao1); G.glUseProgram(P1); G.glBindTexture(GL_TEXTURE_2D, T); G.glUniform4f(uColor, 1, 1, 1, 1); G.glUniform1f(uDepth, 0);
    for (int i = 0; i < 16; i++) memcpy(lv0 + 4 * i, (const uint8_t[]){0, 255, 255, 255}, 4);
    G.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, lv0);         // before the frame reads it: before the frame's commands
    rect(uRect, 0, 24, 8, 32, WW, WH); G.glDrawArrays(GL_TRIANGLES, 0, 6);                   // F: cyan
    for (int i = 0; i < 16; i++) memcpy(lv0 + 4 * i, (const uint8_t[]){255, 255, 255, 255}, 4);
    G.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, lv0);         // after F read it: among the frame's commands
    rect(uRect, 8, 24, 16, 32, WW, WH); G.glDrawArrays(GL_TRIANGLES, 0, 6);                  // G: white
    // a framebuffer object: a 16x16 colour texture and a depth-stencil renderbuffer, cleared with glClearBufferfv, gl_FragCoord drawn into it, then sampled and blitted
    GLuint CT, RB, FB; G.glGenTextures(1, &CT); G.glBindTexture(GL_TEXTURE_2D, CT);
    G.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    G.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); G.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    G.glGenRenderbuffers(1, &RB); G.glBindRenderbuffer(GL_RENDERBUFFER, RB); G.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 16, 16);
    G.glGenFramebuffers(1, &FB); G.glBindFramebuffer(GL_FRAMEBUFFER, FB);
    G.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, CT, 0); G.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, RB);
    G.glViewport(0, 0, 16, 16); const float blue[4] = {0, 0, 1, 1}; G.glClearBufferfv(GL_COLOR, 0, blue);
    G.glUseProgram(P3); G.glUniform2f(cSize, 16, 16); G.glUniform1f(cDepth, 0); rect(cRect, 0, 0, 16, 16, 16, 16); G.glDrawArrays(GL_TRIANGLES, 0, 6);
    G.glBindFramebuffer(GL_FRAMEBUFFER, 0); G.glViewport(0, 0, WW, WH);
    G.glUseProgram(P1); G.glBindTexture(GL_TEXTURE_2D, CT); rect(uRect, 48, 24, 64, 40, WW, WH); G.glDrawArrays(GL_TRIANGLES, 0, 6);   // sampled at GL's texture coordinates
    G.glBindFramebuffer(GL_READ_FRAMEBUFFER, FB); G.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    G.glBlitFramebuffer(0, 0, 16, 16, 48, 8, 64, 24, GL_COLOR_BUFFER_BIT, GL_NEAREST);       // a copy
    G.glBlitFramebuffer(0, 0, 16, 16, 32, 40, 40, 32, GL_COLOR_BUFFER_BIT, GL_NEAREST);      // scaled down and flipped
    G.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // mipmaps: a texture of all its levels (glTexStorage2D, LÖVE's), and one of one level, which gets its chain
    GLuint M, M1; uint8_t m0[16 * 4], n0[16 * 4];
    for (int i = 0; i < 16; i++) { memcpy(m0 + 4 * i, (const uint8_t[]){200, 100, 50, 255}, 4); memcpy(n0 + 4 * i, (const uint8_t[]){10, 20, 30, 255}, 4); }
    G.glGenTextures(1, &M); G.glBindTexture(GL_TEXTURE_2D, M); G.glTexStorage2D(GL_TEXTURE_2D, 3, GL_RGBA8, 4, 4);
    G.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, m0); G.glGenerateMipmap(GL_TEXTURE_2D);
    G.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST); G.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    rect(uRect, 60, 0, 61, 1, WW, WH); G.glDrawArrays(GL_TRIANGLES, 0, 6);
    G.glGenTextures(1, &M1); G.glBindTexture(GL_TEXTURE_2D, M1); G.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, n0);
    G.glGenerateMipmap(GL_TEXTURE_2D);
    G.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST); G.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    rect(uRect, 62, 0, 63, 1, WW, WH); G.glDrawArrays(GL_TRIANGLES, 0, 6);
    G.glReadPixels(0, 0, WW, WH, GL_RGBA, GL_UNSIGNED_BYTE, g_rp);
    px("frame 2, Q1: the stream rewritten after its fence, faded by uScale[1]", rp(25, 22), 128, 0, 128, 1);
    px("Q2: the orphaned index buffer's new indices (the lower right triangle)", rp(46, 17), 128, 128, 0, 1);
    px("Q2: its upper left corner stays clear", rp(41, 22), 0, 0, 0, 0);
    px("S3: a triangle fan, its colour the disabled attribute's current value", rp(50, 5), 0, 128, 0, 1);
    px("F: the upload before the frame's first read of the texture", rp(4, 28), 0, 255, 255, 0);
    px("G: the upload after it, in command order", rp(12, 28), 255, 255, 255, 0);
    px("the framebuffer object sampled: its bottom row at the bottom", rp(50, 24), grad(2, 16), grad(0, 16), 0, 1);
    px("the framebuffer object sampled: its top row at the top", rp(50, 39), grad(2, 16), grad(15, 16), 0, 1);
    px("glBlitFramebuffer, a copy: GL (50, 9) is the object's (2, 1)", rp(50, 9), grad(2, 16), grad(1, 16), 0, 1);
    snprintf(name, sizeof name, "glBlitFramebuffer, scaled and flipped: green %u at the region's bottom row, %u at its top", rp(34, 32)[1], rp(34, 39)[1]);
    t(name, rp(34, 32)[1] > 200 && rp(34, 39)[1] < 60 && rp(34, 32)[2] == 0);
    px("glGenerateMipmap of a texture of 3 levels: level 2 at a 1-pixel quad", rp(60, 0), 200, 100, 50, 1);
    px("glGenerateMipmap of a texture of 1 level: its chain made, level 2", rp(62, 0), 10, 20, 30, 1);
    G.eglSwapBuffers(dpy, surf);
    got = hvk_present_last(g_shot, sizeof g_shot, &info);
    t("present 2", got && info.frames == 2);
    px("present 2, upright: the sampled object's top row high in the picture", shot(50, 39), grad(2, 16), grad(15, 16), 0, 1);
    px("present 2: G (white) at GL (12, 28)", shot(12, 28), 255, 255, 255, 0);

    // ---------- What the layer did ----------
    guest_gl_stats s; guest_gl_stats_get(&s);
    snprintf(name, sizeof name, "paths: %llu passes, %llu draws, %llu pipelines, uploads %llu before / %llu among the frame's commands, %llu orphans, clears %llu load / %llu rect / %llu draw",
             (unsigned long long)s.passes, (unsigned long long)s.draws, (unsigned long long)s.pipelines, (unsigned long long)s.uploads_pre, (unsigned long long)s.uploads_ordered,
             (unsigned long long)s.orphans, (unsigned long long)s.clears_load, (unsigned long long)s.clears_rect, (unsigned long long)s.clears_draw);
    t(name, s.passes > 2 && s.draws >= 17 && s.pipelines >= 3 && s.uploads_pre >= 2 && s.uploads_ordered >= 1 && s.orphans >= 1 && s.clears_load >= 2 && s.clears_rect >= 2 && s.clears_draw >= 1);
    snprintf(name, sizeof name, "paths: %llu varyings moved, %llu blits, %llu mipmaps, %llu glReadPixels, %llu early submissions of %llu, %llu frames",
             (unsigned long long)s.varyings_relocated, (unsigned long long)s.blits, (unsigned long long)s.mipmaps, (unsigned long long)s.readpixels, (unsigned long long)s.early_commits,
             (unsigned long long)s.submits, (unsigned long long)s.frames);
    t(name, s.varyings_relocated == 2 && s.blits == 2 && s.mipmaps == 2 && s.readpixels == 2 && s.early_commits >= 2 && s.frames == 2);
    G.glFinish();
    guest_gl_shutdown();   // every object of the layer released before the backend's device goes
    return hv_finish();
}
