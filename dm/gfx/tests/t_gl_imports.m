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

// ---------- Vertex arrays: GL 4.3's separate format and binding (glVertexAttribFormat / glVertexAttribBinding / glBindVertexBuffer, Fire Emblem: Shadow
// Dragon), glVertexAttribPointer and glVertexAttribDivisor as GL's shorthand for them, and a binding's literal stride 0 ----------
static void (*gen_vao)(GLsizei, GLuint *), (*bind_vao)(GLuint), (*gen_buf)(GLsizei, GLuint *), (*bind_buf)(GLenum, GLuint), (*del_buf)(GLsizei, const GLuint *);
static void (*enable_attr)(GLuint), (*attr_ptr)(GLuint, GLint, GLenum, uint32_t, GLsizei, const void *), (*attr_div)(GLuint, GLuint);
static void (*attr_format)(GLuint, GLint, GLenum, uint32_t, GLuint), (*attr_binding)(GLuint, GLuint), (*bind_vb)(GLuint, GLuint, GLintptr, GLsizei), (*vb_div)(GLuint, GLuint);

// What a draw reads for attribute i: its binding's buffer, stride and divisor, at the binding's offset plus the attribute's relative offset
typedef struct { GLuint buf; uint64_t off; GLsizei stride; uint8_t div; } fetch_t;
static fetch_t fetch(GLuint i) { vao_t *v = cur_vao(); const vbind_t *b = vb_of(v, (int)i); return (fetch_t){b->buf, b->off + v->a[i].rel, b->stride, b->div}; }
#define FETCH(i, B, O, ST, D) do { fetch_t f_ = fetch(i); assert(f_.buf == (B) && f_.off == (O) && f_.stride == (ST) && f_.div == (D)); } while (0)
static const vattr_t *attr(GLuint i) { return &cur_vao()->a[i]; }

static void test_vertex_binding(void) {
    gen_vao = import("glGenVertexArrays"); bind_vao = import("glBindVertexArray"); gen_buf = import("glGenBuffers"); bind_buf = import("glBindBuffer");
    del_buf = import("glDeleteBuffers"); enable_attr = import("glEnableVertexAttribArray"); attr_ptr = import("glVertexAttribPointer"); attr_div = import("glVertexAttribDivisor");
    attr_format = import("glVertexAttribFormat"); attr_binding = import("glVertexAttribBinding"); bind_vb = import("glBindVertexBuffer"); vb_div = import("glVertexBindingDivisor");
    GLuint vao, b[2]; gen_vao(1, &vao); bind_vao(vao); gen_buf(2, b);
    // An attribute reads what its binding holds, at its format's relative offset; a second attribute on the binding at its own
    attr_format(0, 2, GL_FLOAT, 0, 4); attr_binding(0, 3); bind_vb(3, b[0], 100, 24);
    assert(attr(0)->size == 2 && attr(0)->type == GL_FLOAT && !attr(0)->norm); FETCH(0, b[0], 104, 24, 0);
    attr_format(1, 4, GL_UNSIGNED_BYTE, 1, 8); attr_binding(1, 3);
    assert(attr(1)->size == 4 && attr(1)->norm); FETCH(1, b[0], 108, 24, 0);
    // the binding changes: every attribute on it follows, whether formatted before or after
    bind_vb(3, b[1], 200, 32); FETCH(0, b[1], 204, 32, 0); FETCH(1, b[1], 208, 32, 0);
    vb_div(3, 1); FETCH(0, b[1], 204, 32, 1); FETCH(1, b[1], 208, 32, 1);
    // a new format moves the attribute within its binding; a new binding moves it off
    attr_format(1, 4, GL_UNSIGNED_BYTE, 1, 12); FETCH(1, b[1], 212, 32, 1);
    attr_binding(0, 5); FETCH(0, 0, 4, 0, 0);
    bind_vb(5, b[0], 16, 8); FETCH(0, b[0], 20, 8, 0); FETCH(1, b[1], 212, 32, 1);
    // a binding's stride 0 is literal, whatever the formats on it
    attr_format(2, 3, GL_FLOAT, 0, 0); attr_binding(2, 7); bind_vb(7, b[0], 0, 0); FETCH(2, b[0], 0, 0, 0);
    attr_format(2, 4, GL_FLOAT, 0, 0); FETCH(2, b[0], 0, 0, 0);
    // a binding given no buffer is not client memory: a draw skips the attribute (draw_setup)
    bind_vb(7, 0, 64, 16); assert(!vb_of(cur_vao(), 2)->client && !vb_of(cur_vao(), 2)->buf);
    bind_vao(0);

    // glVertexAttribPointer: a format, binding i, and that binding's buffer (the array buffer at the call), offset and stride (0 = the packed size)
    gen_vao(1, &vao); bind_vao(vao);
    bind_buf(GL_ARRAY_BUFFER, b[0]);
    attr_ptr(2, 3, GL_FLOAT, 0, 0, (const void *)16);
    assert(attr(2)->size == 3 && attr(2)->type == GL_FLOAT); FETCH(2, b[0], 16, 12, 0);
    bind_buf(GL_ARRAY_BUFFER, b[1]); FETCH(2, b[0], 16, 12, 0);   // a later array buffer binding changes nothing
    attr_ptr(3, 2, GL_UNSIGNED_SHORT, 0x100, 20, (const void *)4);   // the normalized flag is the low byte, as the Switch caller left it
    assert(!attr(3)->norm); FETCH(3, b[1], 4, 20, 0);
    attr_ptr(3, 0x80e1, GL_UNSIGNED_BYTE, 1, 0, (const void *)0);   // GL_BGRA is four components
    assert(attr(3)->size == 4 && attr(3)->norm); FETCH(3, b[1], 0, 4, 0);
    // glVertexAttribDivisor: binding i's divisor, which a new pointer keeps
    attr_div(2, 4); FETCH(2, b[0], 16, 12, 4); FETCH(3, b[1], 0, 4, 0);
    attr_div(2, 1000); FETCH(2, b[0], 16, 12, 255);   // as far as the layer counts
    attr_ptr(2, 2, GL_FLOAT, 0, 8, (const void *)32); FETCH(2, b[1], 32, 8, 255);
    enable_attr(2); attr_ptr(2, 4, GL_FLOAT, 0, 0, (const void *)0); assert(attr(2)->on); FETCH(2, b[1], 0, 16, 255);
    // a pointer's stride 0 stays the packed size of its own format: an attribute of another format on the binding, or a later format, keeps it
    attr_ptr(0, 2, GL_FLOAT, 0, 0, (const void *)0); FETCH(0, b[1], 0, 8, 0);
    attr_format(1, 3, GL_FLOAT, 0, 8); attr_binding(1, 0); FETCH(1, b[1], 8, 8, 0);
    attr_format(0, 4, GL_FLOAT, 0, 0); FETCH(0, b[1], 0, 8, 0); FETCH(1, b[1], 8, 8, 0);
    attr_ptr(0, 3, GL_UNSIGNED_BYTE, 0, 0, (const void *)0); FETCH(0, b[1], 0, 3, 0); FETCH(1, b[1], 8, 3, 0);   // a new pointer is a new stride, and attribute 1 follows
    // an attribute moved off by glVertexAttribBinding comes back to its own binding with a pointer, and no longer follows the other
    attr_format(4, 2, GL_FLOAT, 0, 0); attr_binding(4, 9); bind_vb(9, b[0], 40, 16); FETCH(4, b[0], 40, 16, 0);
    attr_ptr(4, 2, GL_FLOAT, 0, 0, (const void *)64); FETCH(4, b[1], 64, 8, 0);
    bind_vb(9, b[0], 80, 16); FETCH(4, b[1], 64, 8, 0);
    // and glVertexAttribDivisor sets its own binding's divisor alone, taking the attribute back to it
    vb_div(9, 3); attr_binding(5, 9); attr_binding(6, 9); attr_div(6, 2);
    FETCH(5, b[0], 80, 16, 3); assert(!attr(6)->bind && vb_of(cur_vao(), 6)->div == 2);
    // client memory: no array buffer, the pointer is the address
    static const float verts[8];
    bind_buf(GL_ARRAY_BUFFER, 0); attr_div(7, 1); attr_ptr(7, 2, GL_FLOAT, 0, 0, verts);
    assert(vb_of(cur_vao(), 7)->client); FETCH(7, 0, (uint64_t)(uintptr_t)verts, 8, 1);
    // a deleted buffer leaves the current vertex array's bindings
    bind_buf(GL_ARRAY_BUFFER, b[0]); attr_ptr(8, 4, GL_FLOAT, 0, 0, (const void *)0); FETCH(8, b[0], 0, 16, 0);

    // GL's errors and limits
    GLenum (*geterr)(void) = import("glGetError");
    assert(geterr() == GL_NO_ERROR);
    attr_format(16, 2, GL_FLOAT, 0, 0); assert(geterr() == GL_INVALID_VALUE);
    attr_format(0, 2, GL_FLOAT, 0, 2048); assert(geterr() == GL_INVALID_VALUE && attr(0)->rel == 0);   // past MAX_VERTEX_ATTRIB_RELATIVE_OFFSET: nothing changes
    attr_binding(0, 16); assert(geterr() == GL_INVALID_VALUE && !attr(0)->bind);
    bind_vb(16, b[0], 0, 0); assert(geterr() == GL_INVALID_VALUE);
    bind_vb(1, b[0], -4, 0); assert(geterr() == GL_INVALID_VALUE);
    attr_ptr(16, 2, GL_FLOAT, 0, 0, NULL); assert(geterr() == GL_INVALID_VALUE);
    attr_div(16, 1); vb_div(16, 1); assert(geterr() == GL_INVALID_VALUE); assert(geterr() == GL_NO_ERROR);
    GLint lim = 0; h_glGetIntegerv(0x82da, &lim); assert(lim == 16); h_glGetIntegerv(0x82d9, &lim); assert(lim == 2047);   // MAX_VERTEX_ATTRIB_BINDINGS, _RELATIVE_OFFSET
    del_buf(1, &b[0]); FETCH(8, 0, 0, 16, 0); FETCH(2, b[1], 0, 16, 255);
    bind_vao(0); del_buf(1, &b[1]);
}

// A binding of stride 0 reaches the pipeline as a constant fetch: three points read three vec4 colours. With stride 16 each reads its own, with stride 0 all the
// first (at the binding's offset), with a divisor or not. The pipeline is the layer's (vkey_set, pso_get); the draw is encoded here
static void test_literal_stride_fetch(void) {
    static const char *src = "#include <metal_stdlib>\n using namespace metal;\n"
        "struct VIn { float4 c [[attribute(0)]]; };\n struct VOut { float4 pos [[position]]; float4 c; float ps [[point_size]]; };\n"
        "vertex VOut vmain(VIn in [[stage_in]], uint vid [[vertex_id]]) { VOut o; o.pos = float4(-0.75 + 0.5 * float(vid), 0, 0, 1); o.c = in.c; o.ps = 1; return o; }\n"
        "fragment float4 fmain(VOut in [[stage_in]]) { return in.c; }\n";
    NSError *err = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(src) options:nil error:&err];
    assert(lib);
    static shattr at = {.name = "c", .slot = 0};
    static shinfo si = {.na = 1, .a = &at};
    static prog_t p;
    p.si[0] = &si; p.fn[0] = [lib newFunctionWithName:@"vmain"]; p.fn[1] = [lib newFunctionWithName:@"fmain"];
    const float colours[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
    GLuint vao, b; gen_vao(1, &vao); bind_vao(vao); gen_buf(1, &b);
    bind_buf(GL_ARRAY_BUFFER, b);
    h_glBufferData(GL_ARRAY_BUFFER, sizeof colours, colours, 0);
    attr_format(0, 4, GL_FLOAT, 0, 0); attr_binding(0, 2); enable_attr(0);
    static const struct { GLintptr off; GLsizei stride, div; uint8_t red[3], green[3]; } cases[] = {   // the colour each of the three points read, by channel
        {0, 16, 0, {255, 0, 0}, {0, 255, 0}},                // per vertex: the three colours
        {0, 0, 0, {255, 255, 255}, {0, 0, 0}},               // stride 0: the first for all
        {16, 0, 0, {0, 0, 0}, {255, 255, 255}},              // at the binding's offset
        {0, 0, 1, {255, 255, 255}, {0, 0, 0}},               // a divisor changes nothing
        {0, 16, 1, {255, 255, 255}, {0, 0, 0}},              // a divisor of 1 on a stride: one instance, whose element every vertex reads
    };
    for (unsigned c = 0; c < sizeof cases / sizeof *cases; c++) {
        bind_vb(2, b, cases[c].off, cases[c].stride); vb_div(2, (GLuint)cases[c].div);
        pkey_t k; memset(&k, 0, sizeof k);
        vkey_set(&k, 0, attr(0), vb_of(cur_vao(), 0));
        assert(k.stride[0] == cases[c].stride && k.div[0] == (cases[c].stride ? cases[c].div : 0));   // a constant fetch has no divisor, so one pipeline serves both
        k.cpf = MTLPixelFormatRGBA8Unorm; k.mask = 15;
        id<MTLRenderPipelineState> pso = pso_get(&p, &k);
        assert(pso);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:1 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [g_dev newTextureWithDescriptor:td];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = tex; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        id<MTLCommandBuffer> cb = [g_q commandBuffer];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:pso];
        [e setVertexBuffer:buf_of(b)->mb offset:fetch(0).off atIndex:30];
        [e drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:3];
        [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
        assert(cb.status == MTLCommandBufferStatusCompleted);
        uint8_t px[16]; [tex getBytes:px bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 1) mipmapLevel:0];
        for (int i = 0; i < 3; i++) assert(px[4 * i] == cases[c].red[i] && px[4 * i + 1] == cases[c].green[i] && px[4 * i + 3] == 255);   // alpha 255: the point was drawn
        assert(px[12] == 0 && px[15] == 0);   // the fourth pixel, which no point covers
    }
    bind_vao(0); del_buf(1, &b);
}

// GL_UNIFORM_BUFFER is a generic target of its own, which glBindBufferRange and glBindBufferBase also set; glMemoryBarrier has nothing to wait for
static void test_uniform_buffer(void) {
    void (*range)(GLenum, GLuint, GLuint, GLintptr, GLsizeiptr) = import("glBindBufferRange"), (*base)(GLenum, GLuint, GLuint) = import("glBindBufferBase");
    void (*barrier)(GLbitfield) = import("glMemoryBarrier");
    GLuint b[2]; gen_buf(2, b);
    bind_buf(GL_ARRAY_BUFFER, b[1]); bind_buf(GL_UNIFORM_BUFFER, b[0]);
    assert(S.uniform_buf == b[0] && S.array_buf == b[1]);
    GLint cur = 0; h_glGetIntegerv(0x8a28, &cur); assert(cur == (GLint)b[0]);   // UNIFORM_BUFFER_BINDING
    const uint32_t data[4] = {1, 2, 3, 4};
    h_glBufferData(GL_UNIFORM_BUFFER, sizeof data, data, 0);   // the target's buffer takes the data
    assert(buf_of(b[0])->size == sizeof data && !memcmp(buf_of(b[0])->mb.contents, data, sizeof data) && !buf_of(b[1])->mb);
    range(GL_UNIFORM_BUFFER, 0, b[1], 0, 4); assert(S.uniform_buf == b[1] && S.array_buf == b[1]);
    base(GL_UNIFORM_BUFFER, 1, b[0]); assert(S.uniform_buf == b[0]);
    h_glBufferStorage(GL_UNIFORM_BUFFER, 64, NULL, 0); assert(buf_of(b[0])->immutable && buf_of(b[0])->size == 64);
    range(GL_ARRAY_BUFFER, 0, b[0], 0, 4); assert(S.array_buf == b[1] && S.uniform_buf == b[0]);   // a target the layer does not have: reported, nothing bound
    barrier(0x4000);   // CLIENT_MAPPED_BUFFER_BARRIER_BIT, Fire Emblem's every frame
    del_buf(1, &b[0]); assert(!S.uniform_buf && S.array_buf == b[1]);
    del_buf(1, &b[1]); assert(!S.array_buf);
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

    test_vertex_binding();
    test_literal_stride_fetch();
    test_uniform_buffer();

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
    printf("GL imports: %u shared entries, shader handle/source, shader and program lifetimes, the error flag, a depth texture's data, vertex formats and bindings, a binding's stride 0 drawn, the uniform buffer target, wrappers and missing names (DM_GL_STUBS=%d): OK\n", count, stubs);
    return 0;
}
