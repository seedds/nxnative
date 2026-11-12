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

int main(int argc, char **argv) {
    (void)argv;
    int stubs = argc > 1;
    setenv("DM_GL_STUBS", stubs ? "1" : "0", 1);
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
    assert(!memcmp(rb.contents, depth, sizeof depth));
    // A real link: the linked program keeps the stages' code (si, fn) whatever its attachments do afterwards; a relink without a stage fails. The same shader attached twice
    // changes nothing (its count included)
    void (*link)(GLuint) = import("glLinkProgram");
    static shinfo none;   // no uniforms, samplers or attributes
    id vfn = [NSObject new], ffn = [NSObject new];
    GLuint pr3 = createprog(), vs3 = create(GL_VERTEX_SHADER), fs3 = create(GL_FRAGMENT_SHADER);
    shader_of(vs3)->si = &none; shader_of(vs3)->fn = vfn; shader_of(fs3)->si = &none; shader_of(fs3)->fn = ffn;
    attach(pr3, vs3); attach(pr3, fs3); attach(pr3, fs3); assert(shader_of(fs3)->refs == 1);
    link(pr3); assert(prog_of(pr3)->linked && prog_of(pr3)->fn[0] == vfn && prog_of(pr3)->fn[1] == ffn);
    detach(pr3, vs3); delete(vs3); assert(!shader_of(vs3) && !prog_of(pr3)->vs);
    assert(prog_of(pr3)->linked && prog_of(pr3)->fn[0] == vfn && prog_of(pr3)->si[0] == &none);   // detached and deleted: the linked executable stays
    delete(fs3); assert(shader_of(fs3) && prog_of(pr3)->fn[1] == ffn);   // attached: only flagged
    link(pr3); assert(!prog_of(pr3)->linked);   // relinked without a vertex shader: the link fails
    delprog(pr3); assert(!prog_of(pr3) && !shader_of(fs3));

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
    printf("GL imports: %u shared entries, shader handle/source, shader and program lifetimes, a depth texture's data, wrappers and missing names (DM_GL_STUBS=%d): OK\n", count, stubs);
    return 0;
}
