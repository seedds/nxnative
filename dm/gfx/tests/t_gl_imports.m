// Direct GL imports and eglGetProcAddress must reach the same implementations (issue #228), including big-stack entries.
#include "../gl/hle_gl.m"
#include <assert.h>

uint64_t resolve_import(module_t *m, const char *name);
int hle_implements(const char *name);
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
struct dm_flags g_flag;
void svc_thread_dump(void) {}
void dm_vi_layer_size(int *w, int *h) { *w = *h = 0; }
void dm_vi_layer_crop(int c[4]) { memset(c, 0, 4 * sizeof *c); }
int dm_opmode(void) { return 1; }
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
uint64_t guest_ns(void) { return 0; }
uint64_t guest_ticks(void) { return 0; }
void guest_clock_read_wait(void) {}
void thr_svc_enter(uint64_t *r, uint64_t a) { (void)r; (void)a; }
void thr_svc_leave(void) {}
void dm_spin_wait(uint64_t r, uint64_t a) { (void)r; (void)a; }
uint64_t svc_dispatch(int n, uint64_t *r, uint64_t a) { (void)n; (void)r; (void)a; return 0; }
uint64_t unimpl_stubs[4096];
static const char *wrap_kind;
void *hlecount_wrap(const char *kind, const char *name, void *fn) { (void)name; wrap_kind = kind; return fn; }

static void *import(const char *name) {
    assert(hle_implements(name));
    void *fn = (void *)resolve_import(NULL, name);
    assert(!strcmp(wrap_kind, "HLE"));
    return fn;
}

static void pixel_bytes(const void *src, unsigned w, unsigned h, GLenum format, GLenum type, int f, int cv, const void *want, size_t bytes) {
    size_t pitch;
    void *got = pixels_convert(src, w, h, format, type, f, cv, &pitch);
    assert(pitch * fmt_rows(f, h) == bytes);
    assert(!memcmp(got, want, bytes));
    free(got);
}

static void unpack_swap_bytes(void) {
    // DOOM 3 uploads big-endian RGB565 light maps. Padding is not a component, and the last row needs no trailing padding.
    const uint8_t rgb565[] = {0xf8,0, 7,0xe0, 0,0x1f, 0xaa,0xbb, 0xff,0xff, 0,0, 0xff,0xe0};
    const uint8_t rgba[] = {255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255, 0,0,0,255, 255,255,0,255};
    h_glPixelStorei(0xcf0, 1);   // GL_UNPACK_SWAP_BYTES
    pixel_bytes(rgb565, 3, 2, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, F_RGBA8, CV_565, rgba, sizeof rgba);
    const uint8_t packed[] = {0xf0,0x0f}, rgba4[] = {255,0,0,255}, rgb5a1[] = {246,0,57,255};
    pixel_bytes(packed, 1, 1, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, F_RGBA8, CV_4444, rgba4, sizeof rgba4);
    pixel_bytes(packed, 1, 1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, F_RGBA8, CV_5551, rgb5a1, sizeof rgb5a1);
    const uint8_t half[] = {0x3c,0, 0xc0,0}, half_le[] = {0,0x3c, 0,0xc0};
    pixel_bytes(half, 1, 1, GL_RG, GL_HALF_FLOAT, F_RG16F, CV_COPY, half_le, sizeof half_le);
    const uint8_t words[] = {0x12,0x34, 0xab,0xcd}, words_le[] = {0x34,0x12, 0xcd,0xab};
    pixel_bytes(words, 2, 1, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, F_D16, CV_COPY, words_le, sizeof words_le);
    const uint8_t floats[] = {0x3f,0x80,0,0, 0,0,0,0, 0xc0,0,0,0}, floats_le[] = {0,0,0x80,0x3f, 0,0,0,0xc0};
    h_glPixelStorei(GL_UNPACK_ROW_LENGTH, 2);
    pixel_bytes(floats, 1, 2, GL_RED, GL_FLOAT, F_R32F, CV_COPY, floats_le, sizeof floats_le);
    h_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    // Packed 32-bit pixels are one element, even when format names three or four components.
    const uint8_t packed32[] = {0x12,0x34,0x56,0x78, 0xaa,0xbb,0xcc,0xdd}, packed32_le[] = {0x78,0x56,0x34,0x12, 0xdd,0xcc,0xbb,0xaa};
    pixel_bytes(packed32, 1, 2, GL_RGBA, 0x8368, F_RGB10A2, CV_COPY, packed32_le, sizeof packed32_le);   // UNSIGNED_INT_2_10_10_10_REV
    pixel_bytes(packed32, 1, 2, GL_RGB, 0x8c3b, F_RG11B10F, CV_COPY, packed32_le, sizeof packed32_le);   // UNSIGNED_INT_10F_11F_11F_REV
    const uint8_t bytes[] = {1,2,3,4};
    pixel_bytes(bytes, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, F_RGBA8, CV_COPY, bytes, sizeof bytes);
    const uint8_t block[] = {1,2,3,4,5,6,7,8};
    pixel_bytes(block, 4, 4, 0, 0, F_BC1, CV_COPY, block, sizeof block);
    const uint8_t zeros[4] = {0};
    pixel_bytes(NULL, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, F_RGBA8, CV_COPY, zeros, sizeof zeros);
    GLint value = -1; h_glGetIntegerv(0xcf0, &value); assert(value == 1);
    h_glPixelStorei(0xcf0, 0);
    h_glGetIntegerv(0xcf0, &value); assert(value == 0);
    pixel_bytes(words, 2, 1, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, F_D16, CV_COPY, words, sizeof words);
    assert(rgb565[0] == 0xf8 && rgb565[1] == 0);   // caller memory is unchanged
    puts("GL unpack byte order: packed colours, half/float/depth, row layout and unchanged byte/block inputs: OK");
}

int main(int argc, char **argv) {
    (void)argv;
    int stubs = argc > 1;
    setenv("DM_GL_STUBS", stubs ? "1" : "0", 1);
    unpack_swap_bytes();
    // The calls from the issue must bind without using a GL loader first.
    GLuint (*create)(GLenum) = import("glCreateShader");
    assert((void *)create == (void *)h_glCreateShader);
    GLuint sh = create(GL_VERTEX_SHADER); assert(sh);
    void (*source)(GLuint, GLsizei, const GLchar *const *, const GLint *) = import("glShaderSource");
    const GLchar *text = "void main() {}";
    source(sh, 1, &text, NULL);
    void (*getiv)(GLuint, GLenum, GLint *) = import("glGetShaderiv");
    GLint value = 0; getiv(sh, GL_SHADER_TYPE, &value); assert(value == GL_VERTEX_SHADER);
    getiv(sh, GL_SHADER_SOURCE_LENGTH, &value); assert(value == strlen(text) + 1);
    // The import and loader paths must both keep the wrapper, rather than exposing the Metal implementation's small-stack entry.
    assert(import("glCompileShader") == (void *)h_glCompileShader_bs);
    assert(import("glClear") == (void *)h_glClear_bs);
    void (*clearcolor)(GLfloat, GLfloat, GLfloat, GLfloat) = import("glClearColor");
    clearcolor(0.25f, 0.5f, 0.75f, 1); assert(S.clear_c[0] == 0.25f && S.clear_c[3] == 1);
    void (*info)(GLuint, GLsizei, GLsizei *, GLchar *) = import("glGetShaderInfoLog");
    GLsizei len = -1; GLchar log[2] = {'?', 0}; info(sh, sizeof log, &len, log); assert(!len && !log[0]);
    void (*delete)(GLuint) = import("glDeleteShader"); delete(sh); assert(!shader_of(sh));

    unsigned long sz;
    const hle_ent *sec = (const hle_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swhle", &sz);
    unsigned count = 0;
    for (unsigned long i = 0; i < sz / sizeof *sec; i++)
        if (!strncmp(sec[i].name, "gl", 2)) {
            assert(import(sec[i].name) == sec[i].fn);
            assert(h_eglGetProcAddress(sec[i].name) == sec[i].fn); count++;
        }
    assert(count > 100);
    assert((void *)resolve_import(NULL, "eglGetProcAddress") == (void *)h_eglGetProcAddress_bs);
    assert(!h_eglGetProcAddress(NULL));
    assert(!h_eglGetProcAddress("not_a_GL_function"));
    const char *missing = "glMissingFunctionForTest";
    assert(!hle_implements(missing));
    assert((h_eglGetProcAddress(missing) != NULL) == stubs);
    assert(resolve_import(NULL, missing) == (uint64_t)&unimpl_stubs[0]);
    assert(!strcmp(wrap_kind, "STUB"));
    printf("GL imports: %u shared entries, shader handle/source, wrappers and missing names (DM_GL_STUBS=%d): OK\n", count, stubs);
    return 0;
}
