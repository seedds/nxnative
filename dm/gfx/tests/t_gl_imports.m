// Direct GL imports and eglGetProcAddress must reach the same implementations (issue #228), including big-stack entries.
#include "../gl/hle_gl.m"
#include <assert.h>

uint64_t resolve_import(module_t *m, const char *name);
int hle_implements(const char *name);
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
struct dm_flags g_flag;
pthread_key_t g_key18;   // written by hle.c's counter gate for mrs x18, cntpct_el0; in the host it is defined in loader.c (never created here: no gate runs)
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

    // Deleting an attached shader only flags it: it keeps its name and goes with its last attachment; a program deleted while current stays in use until the next glUseProgram
    GLuint (*createprog)(void) = import("glCreateProgram");
    void (*attach)(GLuint, GLuint) = import("glAttachShader"), (*detach)(GLuint, GLuint) = import("glDetachShader");
    void (*delprog)(GLuint) = import("glDeleteProgram"), (*use)(GLuint) = import("glUseProgram");
    void (*progiv)(GLuint, GLenum, GLint *) = import("glGetProgramiv");
    GLuint pr = createprog(), vs = create(GL_VERTEX_SHADER), fs = create(GL_FRAGMENT_SHADER);
    attach(pr, vs); attach(pr, fs); delete(vs);
    assert(shader_of(vs) && shader_of(vs)->type == GL_VERTEX_SHADER);
    getiv(vs, GL_DELETE_STATUS, &value); assert(value == 1);
    getiv(fs, GL_DELETE_STATUS, &value); assert(value == 0);
    GLuint other = create(GL_FRAGMENT_SHADER); assert(other != vs);   // the flagged shader's name is not handed out again
    assert(shader_of(prog_of(pr)->vs) == shader_of(vs));
    progiv(pr, GL_ATTACHED_SHADERS, &value); assert(value == 2);
    attach(pr, other); assert(prog_of(pr)->fs == other);   // a stage's new shader replaces the old one, which stays (it is not flagged)
    assert(shader_of(fs) && !shader_of(fs)->refs && shader_of(other)->refs == 1);
    detach(pr, vs); assert(!shader_of(vs) && !prog_of(pr)->vs);   // its last attachment: the flagged shader goes
    progiv(pr, GL_ATTACHED_SHADERS, &value); assert(value == 1);
    attach(pr, fs); delete(fs); delete(other); assert(shader_of(fs) && !shader_of(other));
    // a second program shares the flagged shader: it goes only when both have let go of it
    GLuint pr2 = createprog(); attach(pr2, fs); assert(shader_of(fs)->refs == 2);
    prog_of(pr)->linked = 1;   // as a link with the library would leave it
    use(pr); delprog(pr);
    assert(prog_of(pr) && prog_of(pr)->linked);   // still current: its draws go on
    progiv(pr, GL_DELETE_STATUS, &value); assert(value == 1);
    GLint cur = 0; h_glGetIntegerv(GL_CURRENT_PROGRAM, &cur); assert(cur == (GLint)pr);
    use(pr); assert(prog_of(pr));   // using it again does not release it
    use(pr2); assert(!prog_of(pr) && shader_of(fs) && shader_of(fs)->refs == 1);   // switched away: the program goes, and its attachment with it
    use(0); assert(prog_of(pr2));   // not flagged: it stays
    delprog(pr2); assert(!prog_of(pr2) && !shader_of(fs));   // not current: it goes at once, and the shader with its last attachment
    vs = create(GL_VERTEX_SHADER); assert(vs); delete(vs); assert(!shader_of(vs));   // the names are free again

    // A depth texture is private, so the level glTexImage2D gave goes in by a blit when the texture is made (it was freed unwritten)
    dev_init();
    GLuint dt = 0; h_glGenTextures(1, &dt); h_glBindTexture(GL_TEXTURE_2D, dt);
    float depth[16]; for (int i = 0; i < 16; i++) depth[i] = (float)i / 16;
    h_glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, 4, 4, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
    tex_t *dtex = tex_of(dt); assert(dtex && tex_make(dtex, 0) && dtex->tex.storageMode == MTLStorageModePrivate);
    id<MTLBuffer> rb = [g_dev newBufferWithLength:sizeof depth options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> rcb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> rbe = [rcb blitCommandEncoder];   // after the upload: one queue runs its command buffers in commit order
    [rbe copyFromTexture:dtex->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(4, 4, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:16 destinationBytesPerImage:64];
    [rbe endEncoding]; [rcb commit]; [rcb waitUntilCompleted];
    for (int i = 0; i < 16; i++) assert(((const float *)rb.contents)[i] == depth[i]);   // the values as given, each exactly
    // A real link: the linked program keeps the stages' code (si, fn) whatever its attachments do afterwards; a relink without a stage fails. The same shader attached twice
    // changes nothing (its count included)
    void (*link)(GLuint) = import("glLinkProgram");
    static shinfo none;   // no uniforms, samplers or attributes
    id vfn = [NSObject new], ffn = [NSObject new];
    GLuint pr3 = createprog(), vs3 = create(GL_VERTEX_SHADER), fs3 = create(GL_FRAGMENT_SHADER);
    shader_of(vs3)->si = &none; shader_of(vs3)->fn = vfn; shader_of(fs3)->si = &none; shader_of(fs3)->fn = ffn;
    attach(pr3, vs3); attach(pr3, fs3); attach(pr3, fs3); assert(shader_of(fs3)->refs == 1 && h_glGetError() == GL_INVALID_OPERATION);
    link(pr3); assert(prog_of(pr3)->linked && prog_of(pr3)->fn[0] == vfn && prog_of(pr3)->fn[1] == ffn);
    detach(pr3, vs3); delete(vs3); assert(!shader_of(vs3) && !prog_of(pr3)->vs);
    assert(prog_of(pr3)->linked && prog_of(pr3)->fn[0] == vfn && prog_of(pr3)->si[0] == &none);   // detached and deleted: the linked executable stays
    delete(fs3); assert(shader_of(fs3) && prog_of(pr3)->fn[1] == ffn);   // attached: only flagged
    link(pr3); assert(!prog_of(pr3)->linked);   // relinked without a vertex shader: the link fails
    delprog(pr3); assert(!prog_of(pr3) && !shader_of(fs3));

    // GL's error flag: the first error since the last glGetError, which reads and clears it; a second error while one is pending is not recorded
    GLenum (*geterr)(void) = import("glGetError");
    assert(geterr() == GL_NO_ERROR);
    GLuint pr4 = createprog(), vs4 = create(GL_VERTEX_SHADER);
    attach(pr4, vs4); assert(geterr() == GL_NO_ERROR);
    attach(pr4, vs4); assert(geterr() == GL_INVALID_OPERATION); assert(geterr() == GL_NO_ERROR);   // the same shader again; the read cleared it
    attach(pr4, 9999); assert(geterr() == GL_INVALID_VALUE);                                  // no such object
    attach(vs4, vs4); assert(geterr() == GL_INVALID_OPERATION);                               // a shader where a program goes
    GLuint fs4 = create(GL_FRAGMENT_SHADER);
    detach(pr4, fs4); assert(geterr() == GL_INVALID_OPERATION);                               // not attached to it
    use(9999); assert(geterr() == GL_INVALID_VALUE);
    use(pr4); use(vs4); GLint cur4 = 0; h_glGetIntegerv(GL_CURRENT_PROGRAM, &cur4); assert(geterr() == GL_INVALID_OPERATION && cur4 == (GLint)pr4);   // the error leaves the current program
    use(0); delete(0); delprog(0); assert(geterr() == GL_NO_ERROR);                           // 0 is silently ignored
    delete(9999); delprog(vs4); assert(geterr() == GL_INVALID_VALUE); assert(geterr() == GL_NO_ERROR);   // the first of two errors is kept, the second not recorded
    getiv(pr4, GL_SHADER_TYPE, &value); assert(geterr() == GL_INVALID_OPERATION && value == 0);
    progiv(vs4, GL_ATTACHED_SHADERS, &value); assert(geterr() == GL_INVALID_OPERATION);
    void (*bindattr)(GLuint, GLuint, const GLchar *) = import("glBindAttribLocation");
    bindattr(pr4, 16, "a"); assert(geterr() == GL_INVALID_VALUE);
    getiv(vs4, GL_SHADER_TYPE, &value); progiv(pr4, GL_ATTACHED_SHADERS, &value); assert(geterr() == GL_NO_ERROR);   // calls that succeed leave the flag clear
    delprog(pr4); delete(vs4); delete(fs4); assert(geterr() == GL_NO_ERROR);

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
    printf("GL imports: %u shared entries, shader handle/source, shader and program lifetimes, the error flag, a depth texture's data, wrappers and missing names (DM_GL_STUBS=%d): OK\n", count, stubs);
    return 0;
}
