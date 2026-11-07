// What Fire Emblem: Shadow Dragon's GL calls need of the host's GL layer: GL 4.3's separate vertex attribute format and binding (an attribute follows its binding's buffer,
// offset, stride and divisor; a binding's stride 0 is literal, every vertex reads the first element), glVertexAttribPointer as GL's shorthand for a format and its own binding's
// buffer, offset and stride (0 = the packed size), glVertexAttribDivisor as the attribute's return to its own binding with that binding's divisor (what the layer did before,
// for every game that uses them), glDetachShader and a shader's deferred deletion, and the GL_UNIFORM_BUFFER target. The calls go through the layer's import table by
// name, so a missing function fails here rather than at the link.
#include "../gl/hle_gl.m"
#include <assert.h>

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
void *hlecount_wrap(const char *kind, const char *name, void *fn) { (void)kind; (void)name; return fn; }

// The function the layer registers under a GL name: what a game's direct import or eglGetProcAddress gets (the functions that call into Metal are the big-stack entries, so
// those are called by their own name below)
static void *gl(const char *name) {
    unsigned long sz;
    const hle_ent *sec = (const hle_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swhle", &sz);
    for (unsigned long i = 0; i < sz / sizeof *sec; i++) if (!strcmp(sec[i].name, name)) return sec[i].fn;
    fprintf(stderr, "t_gl_vertex_binding: the layer does not implement %s\n", name);
    abort();
}

static void (*gen_vao)(GLsizei, GLuint *), (*bind_vao)(GLuint), (*gen_buf)(GLsizei, GLuint *), (*bind_buf)(GLenum, GLuint), (*del_buf)(GLsizei, const GLuint *);
static void (*enable_attr)(GLuint), (*attr_ptr)(GLuint, GLint, GLenum, uint32_t, GLsizei, const void *), (*attr_div)(GLuint, GLuint);
static void (*attr_format)(GLuint, GLint, GLenum, uint32_t, GLuint), (*attr_binding)(GLuint, GLuint), (*bind_vb)(GLuint, GLuint, GLintptr, GLsizei), (*vb_div)(GLuint, GLuint);

static const vattr_t *attr(GLuint i) { return &cur_vao()->a[i]; }

// An attribute reads what its binding holds, at its format's relative offset
static void test_separate_format_and_binding(void) {
    GLuint vao, b[2]; gen_vao(1, &vao); bind_vao(vao); gen_buf(2, b);
    attr_format(0, 2, GL_FLOAT, 0, 4);
    attr_binding(0, 3);
    bind_vb(3, b[0], 100, 24);
    const vattr_t *a = attr(0);
    assert(a->size == 2 && a->type == GL_FLOAT && !a->norm && a->buf == b[0] && a->off == 104 && a->stride == 24 && !a->div);
    // a second attribute on the same binding, at its own relative offset
    attr_format(1, 4, GL_UNSIGNED_BYTE, 1, 8);
    attr_binding(1, 3);
    assert(attr(1)->size == 4 && attr(1)->norm && attr(1)->buf == b[0] && attr(1)->off == 108 && attr(1)->stride == 24);
    // the binding's buffer, offset and stride change: every attribute on it follows, no matter whether it was formatted before or after
    bind_vb(3, b[1], 200, 32);
    assert(attr(0)->buf == b[1] && attr(0)->off == 204 && attr(0)->stride == 32);
    assert(attr(1)->buf == b[1] && attr(1)->off == 208 && attr(1)->stride == 32);
    vb_div(3, 1);
    assert(attr(0)->div == 1 && attr(1)->div == 1);
    // a new format moves the attribute on its binding; a new binding moves it off
    attr_format(1, 4, GL_UNSIGNED_BYTE, 1, 12);
    assert(attr(1)->off == 212 && attr(1)->buf == b[1]);
    attr_binding(0, 5);
    assert(attr(0)->buf == 0 && attr(0)->stride == 0 && !attr(0)->div);
    bind_vb(5, b[0], 16, 8);
    assert(attr(0)->buf == b[0] && attr(0)->off == 20 && attr(0)->stride == 8);
    assert(attr(1)->buf == b[1] && attr(1)->off == 212);   // the other binding's attribute stayed
    // stride 0 on a binding is literal, whatever the attributes' formats are: every vertex reads the first element (test_binding_stride)
    attr_format(2, 3, GL_FLOAT, 0, 0);
    attr_binding(2, 7);
    bind_vb(7, b[0], 0, 0);
    assert(attr(2)->stride == 0);
    bind_vao(0); del_buf(2, b);
}

// glVertexAttribPointer and glVertexAttribDivisor: format plus binding with the attribute's own index
static void test_shorthand(void) {
    GLuint vao, b[2]; gen_vao(1, &vao); bind_vao(vao); gen_buf(2, b);
    bind_buf(GL_ARRAY_BUFFER, b[0]);
    attr_ptr(2, 3, GL_FLOAT, 0, 0, (const void *)16);
    const vattr_t *a = attr(2);
    assert(a->size == 3 && a->type == GL_FLOAT && a->buf == b[0] && a->off == 16 && a->stride == 12 && !a->div);   // stride 0: the packed size
    // the array buffer binding at the time of the call is what counts, not the one of a later call
    bind_buf(GL_ARRAY_BUFFER, b[1]);
    assert(attr(2)->buf == b[0]);
    attr_ptr(3, 2, GL_UNSIGNED_SHORT, 0x100, 20, (const void *)4);   // the normalized flag is the low byte, as the Switch caller left it
    assert(attr(3)->buf == b[1] && attr(3)->off == 4 && attr(3)->stride == 20 && !attr(3)->norm);
    attr_ptr(3, 0x80e1, GL_UNSIGNED_BYTE, 1, 0, (const void *)0);   // GL_BGRA is four components
    assert(attr(3)->size == 4 && attr(3)->norm && attr(3)->stride == 4);
    attr_div(2, 4);
    assert(attr(2)->div == 4 && !attr(3)->div);
    attr_div(2, 1000);
    assert(attr(2)->div == 255);   // as far as the layer counts
    // pointer and divisor are independent halves: the divisor outlives a new pointer
    attr_ptr(2, 2, GL_FLOAT, 0, 8, (const void *)32);
    assert(attr(2)->div == 255 && attr(2)->off == 32 && attr(2)->stride == 8 && attr(2)->size == 2);
    // enabling is per attribute and untouched by either
    enable_attr(2); attr_ptr(2, 4, GL_FLOAT, 0, 0, (const void *)0); assert(attr(2)->on && attr(2)->stride == 16);
    // an attribute moved to another binding by glVertexAttribBinding goes back to its own with glVertexAttribPointer, and then no longer follows that binding
    attr_format(0, 2, GL_FLOAT, 0, 0); attr_binding(0, 9); bind_vb(9, b[0], 40, 16);
    assert(attr(0)->off == 40 && attr(0)->stride == 16);
    attr_ptr(0, 2, GL_FLOAT, 0, 0, (const void *)64);
    assert(attr(0)->off == 64 && attr(0)->stride == 8 && attr(0)->buf == b[1]);
    bind_vb(9, b[1], 80, 16);
    assert(attr(0)->off == 64 && attr(0)->stride == 8);
    // and the divisor of an attribute pointed at its own binding is that binding's alone
    vb_div(9, 3); attr_binding(1, 9);
    attr_div(0, 2);
    assert(attr(0)->div == 2 && attr(1)->div == 3);
    bind_vao(0); del_buf(2, b);
}

// Client memory (no buffer bound) is what the layer already drew from: the address stays the offset
static void test_client_memory(void) {
    GLuint vao; gen_vao(1, &vao); bind_vao(vao);
    bind_buf(GL_ARRAY_BUFFER, 0);
    static const float verts[8] = {0};
    attr_div(4, 1);
    attr_ptr(4, 2, GL_FLOAT, 0, 0, verts);
    const vattr_t *a = attr(4);
    assert(!a->buf && a->off == (uint64_t)(uintptr_t)verts && a->stride == 8 && a->div == 1);
    bind_vao(0);
}

// A pointer's stride 0 is the packed size of its own format, kept on the binding (ARB_vertex_attrib_binding: effectiveStride): attributes of other formats on the binding
// and a later format of the pointer's attribute leave it. A binding's own stride 0 stays 0
static void test_binding_stride(void) {
    GLuint vao, b; gen_vao(1, &vao); bind_vao(vao); gen_buf(1, &b);
    bind_buf(GL_ARRAY_BUFFER, b);
    attr_ptr(0, 2, GL_FLOAT, 0, 0, (const void *)0);
    assert(attr(0)->stride == 8);
    attr_format(1, 3, GL_FLOAT, 0, 8); attr_binding(1, 0);   // a vec3 on the vec2's binding advances by the binding's 8 as well
    assert(attr(1)->buf == b && attr(1)->off == 8 && attr(1)->stride == 8);
    attr_format(0, 4, GL_FLOAT, 0, 0);
    assert(attr(0)->stride == 8 && attr(1)->stride == 8);
    attr_format(1, 1, GL_UNSIGNED_BYTE, 0, 8);
    assert(attr(0)->stride == 8 && attr(1)->stride == 8);
    attr_ptr(0, 3, GL_UNSIGNED_BYTE, 0, 0, (const void *)0);   // a new pointer is a new stride for the binding, and attribute 1 follows
    assert(attr(0)->stride == 3 && attr(1)->stride == 3);
    bind_vb(5, b, 0, 0); attr_format(2, 3, GL_FLOAT, 0, 0); attr_binding(2, 5);
    assert(attr(2)->stride == 0);
    attr_format(2, 4, GL_FLOAT, 0, 0);
    assert(attr(2)->stride == 0);
    bind_vb(5, b, 0, 12);
    assert(attr(2)->stride == 12);
    bind_vao(0); del_buf(1, &b);
}

// A binding of stride 0 reaches the pipeline as a constant fetch: three points read three vec4 colours, and with stride 16 each reads its own, with stride 0 all the first
// (at the binding's offset), a divisor or not. The pipeline is made by the layer's own pso_get, the draw is encoded here
static void test_literal_stride_fetch(void) {
    static const char *src = "#include <metal_stdlib>\n using namespace metal;\n"
        "struct VIn { float4 c [[attribute(0)]]; };\n struct VOut { float4 pos [[position]]; float4 c; float ps [[point_size]]; };\n"
        "vertex VOut vmain(VIn in [[stage_in]], uint vid [[vertex_id]]) { VOut o; o.pos = float4(-0.75 + 0.5 * float(vid), 0, 0, 1); o.c = in.c; o.ps = 1; return o; }\n"
        "fragment float4 fmain(VOut in [[stage_in]]) { return in.c; }\n";
    NSError *err = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(src) options:nil error:&err];
    assert(lib);
    shattr at = {.name = "c", .slot = 0, .type = T_VEC4};
    shinfo si = {.na = 1, .a = &at};
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
        {0, 16, 1, {255, 255, 255}, {0, 0, 0}},              // a divisor of 1 on a stride: one instance, whose element all the vertices read
    };
    for (unsigned c = 0; c < sizeof cases / sizeof *cases; c++) {
        bind_vb(2, b, cases[c].off, cases[c].stride); vb_div(2, (GLuint)cases[c].div);
        pkey_t k; memset(&k, 0, sizeof k);
        vkey_set(&k, 0, attr(0));
        assert(k.stride[0] == cases[c].stride && (cases[c].stride ? k.div[0] == cases[c].div : !k.div[0]));   // a constant fetch has no divisor, so one pipeline serves both
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
        [e setVertexBuffer:buf_of(b)->mb offset:attr(0)->off atIndex:30];
        [e drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:3];
        [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
        assert(cb.status == MTLCommandBufferStatusCompleted);
        uint8_t px[16]; [tex getBytes:px bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 1) mipmapLevel:0];
        for (int i = 0; i < 3; i++) assert(px[4 * i] == cases[c].red[i] && px[4 * i + 1] == cases[c].green[i] && px[4 * i + 3] == 255);   // alpha 255: the point was drawn
        assert(px[12] == 0 && px[15] == 0);   // the fourth pixel no point covers
    }
    bind_vao(0); del_buf(1, &b);
}

static void test_detach_shader(void) {
    GLuint (*create_shader)(GLenum) = gl("glCreateShader"), (*create_program)(void) = gl("glCreateProgram");
    void (*attach)(GLuint, GLuint) = gl("glAttachShader"), (*detach)(GLuint, GLuint) = gl("glDetachShader"), (*del_shader)(GLuint) = gl("glDeleteShader");
    void (*del_program)(GLuint) = gl("glDeleteProgram"), (*shader_iv)(GLuint, GLenum, GLint *) = gl("glGetShaderiv"), (*program_iv)(GLuint, GLenum, GLint *) = gl("glGetProgramiv");
    GLint n;
    GLuint vs = create_shader(GL_VERTEX_SHADER), fs = create_shader(GL_FRAGMENT_SHADER), other = create_shader(GL_VERTEX_SHADER), pr = create_program();
    attach(pr, vs); attach(pr, fs);
    program_iv(pr, GL_ATTACHED_SHADERS, &n); assert(n == 2 && prog_of(pr)->vs == vs && prog_of(pr)->fs == fs);
    detach(pr, other);   // not attached: nothing changes
    program_iv(pr, GL_ATTACHED_SHADERS, &n); assert(n == 2 && prog_of(pr)->vs == vs);
    detach(pr, vs);
    program_iv(pr, GL_ATTACHED_SHADERS, &n); assert(n == 1 && !prog_of(pr)->vs && prog_of(pr)->fs == fs);
    assert(shader_of(vs));   // detached, not deleted
    shader_iv(vs, GL_DELETE_STATUS, &n); assert(!n);
    // a shader deleted while attached stays, marked, until it is detached
    del_shader(fs);
    assert(shader_of(fs));
    shader_iv(fs, GL_DELETE_STATUS, &n); assert(n == 1);
    program_iv(pr, GL_ATTACHED_SHADERS, &n); assert(n == 1 && prog_of(pr)->fs == fs);
    detach(pr, fs);
    assert(!shader_of(fs));
    program_iv(pr, GL_ATTACHED_SHADERS, &n); assert(!n);
    // one deleted while detached goes at once, and so does one attached to nothing
    del_shader(other); assert(!shader_of(other));
    attach(pr, vs); del_shader(vs); assert(shader_of(vs));
    // deleting the program detaches what it had, and the marked shader goes with it
    del_program(pr);
    assert(!prog_of(pr) && !shader_of(vs));
    // a shader two programs share stays for the second
    GLuint p1 = create_program(), p2 = create_program(), sh = create_shader(GL_FRAGMENT_SHADER);
    attach(p1, sh); attach(p2, sh); del_shader(sh);
    del_program(p1); assert(shader_of(sh));
    detach(p2, sh); assert(!shader_of(sh));
    del_program(p2);
    // attaching a shader of a stage that has one replaces it: the replaced one, deleted before, goes at once, and the program's teardown frees only what it holds
    GLuint pa = create_program(), a = create_shader(GL_VERTEX_SHADER), b = create_shader(GL_VERTEX_SHADER);
    attach(pa, a); del_shader(a); assert(shader_of(a));
    attach(pa, b); assert(!shader_of(a) && prog_of(pa)->vs == b);
    attach(pa, b); assert(shader_of(b) && prog_of(pa)->vs == b);   // the same shader again changes nothing
    program_iv(pa, GL_ATTACHED_SHADERS, &n); assert(n == 1);
    del_shader(b); assert(shader_of(b));
    del_program(pa); assert(!shader_of(b));
    // one replaced while it was not deleted stays, detached
    GLuint pb = create_program(), c = create_shader(GL_FRAGMENT_SHADER), d = create_shader(GL_FRAGMENT_SHADER);
    attach(pb, c); attach(pb, d); assert(shader_of(c) && prog_of(pb)->fs == d);
    del_shader(c); assert(!shader_of(c));
    del_shader(d); del_program(pb); assert(!shader_of(d));
}

static void test_uniform_buffer(void) {
    void (*buffer_range)(GLenum, GLuint, GLuint, GLintptr, GLsizeiptr) = gl("glBindBufferRange"), (*buffer_base)(GLenum, GLuint, GLuint) = gl("glBindBufferBase");
    void (*barrier)(GLbitfield) = gl("glMemoryBarrier");
    // (the device is made in main)
    GLuint b[2]; gen_buf(2, b);
    bind_buf(GL_ARRAY_BUFFER, b[1]);
    bind_buf(GL_UNIFORM_BUFFER, b[0]);
    assert(S.uniform_buf == b[0] && S.array_buf == b[1]);   // a target of its own
    const uint32_t data[4] = {1, 2, 3, 4};
    h_glBufferData(GL_UNIFORM_BUFFER, sizeof data, data, 0);   // the target's buffer takes the data: Fire Emblem allocates its block this way
    assert(buf_of(b[0])->size == sizeof data && !memcmp(buf_of(b[0])->mb.contents, data, sizeof data) && !buf_of(b[1])->mb);
    buffer_range(GL_UNIFORM_BUFFER, 0, b[1], 0, 4);
    assert(S.uniform_buf == b[1] && S.array_buf == b[1]);
    buffer_base(GL_UNIFORM_BUFFER, 1, b[0]);
    assert(S.uniform_buf == b[0]);
    h_glBufferStorage(GL_UNIFORM_BUFFER, 64, NULL, 0);
    assert(buf_of(b[0])->immutable && buf_of(b[0])->size == 64);
    buffer_range(GL_ARRAY_BUFFER, 0, b[0], 0, 4);   // not a target the layer has: it is reported and binds nothing
    assert(S.array_buf == b[1] && S.uniform_buf == b[0]);
    barrier(0x4000 /* CLIENT_MAPPED_BUFFER_BARRIER_BIT */);
    del_buf(1, &b[0]);
    assert(!S.uniform_buf && S.array_buf == b[1]);
    del_buf(1, &b[1]);
    assert(!S.array_buf);
}

// glDepthBoundsEXT is not implemented: the game calls it only when the driver lists GL_EXT_depth_bounds_test (the byte its extension loop sets), and Metal has no such test
static void test_depth_bounds_unadvertised(void) {
    for (GLuint i = 0; i < (GLuint)NEXTS; i++) assert(strcmp(g_exts[i], "GL_EXT_depth_bounds_test"));
    assert(!strstr(h_glGetString(GL_EXTENSIONS), "depth_bounds"));
    assert(!hle_implements("glDepthBoundsEXT"));
}

int main(void) {
    setenv("DM_GL_STUBS", "0", 1);
    dev_init();
    gen_vao = gl("glGenVertexArrays"); bind_vao = gl("glBindVertexArray"); gen_buf = gl("glGenBuffers"); bind_buf = gl("glBindBuffer"); del_buf = gl("glDeleteBuffers");
    enable_attr = gl("glEnableVertexAttribArray"); attr_ptr = gl("glVertexAttribPointer"); attr_div = gl("glVertexAttribDivisor");
    attr_format = gl("glVertexAttribFormat"); attr_binding = gl("glVertexAttribBinding"); bind_vb = gl("glBindVertexBuffer"); vb_div = gl("glVertexBindingDivisor");
    test_separate_format_and_binding();
    test_shorthand();
    test_binding_stride();
    test_literal_stride_fetch();
    test_client_memory();
    test_detach_shader();
    test_uniform_buffer();
    test_depth_bounds_unadvertised();
    printf("GL vertex binding: separate format and binding, binding stride, pointer / divisor shorthand, client memory, glDetachShader, uniform buffer target: OK\n");
    return 0;
}
