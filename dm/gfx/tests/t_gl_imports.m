// Direct GL imports and eglGetProcAddress must reach the same implementations (issue #228), including big-stack entries.
#include "../gl/hle_gl.m"
#include "../nvn/nvn_protos.h"
#include <assert.h>
#include <unistd.h>

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
void guest_applet_wait(void) {}
void thr_svc_enter(uint64_t *r, uint64_t a) { (void)r; (void)a; }
void thr_svc_leave(void) {}
void dm_spin_wait(uint64_t r, uint64_t a) { (void)r; (void)a; }
uint64_t svc_dispatch(int n, uint64_t *r, uint64_t a) { (void)n; (void)r; (void)a; return 0; }
static _Atomic int g_gl_frame_ticks;
void dm_guest_frame_tick(void) { atomic_fetch_add(&g_gl_frame_ticks, 1); }
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
static fetch_t fetch(GLuint i) { vao_t *v = cur_vao(); const vbind_t *b = vb_of(v, (int)i); return (fetch_t){b->buf.name, b->off + v->a[i].rel, b->stride, b->div}; }
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
    bind_vb(7, 0, 64, 16); assert(!vb_of(cur_vao(), 2)->client && !vb_of(cur_vao(), 2)->buf.name && !vb_of(cur_vao(), 2)->buf.obj);
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
    void (*attr_iptr)(GLuint, GLint, GLenum, GLsizei, const void *) = import("glVertexAttribIPointer");
    attr_iptr(3, 2, GL_UNSIGNED_INT, 24, (const void *)16);
    assert(!attr(3)->norm && vfmt_of(attr(3)) == MTLVertexFormatUInt2); FETCH(3, b[1], 16, 24, 0);
    attr_ptr(3, 0x80e1, GL_UNSIGNED_BYTE, 1, 0, NULL);
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
        k.cpf[0] = MTLPixelFormatRGBA8Unorm; k.mask[0] = 15;
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
    assert(S.uniform_buf.name == b[0] && S.array_buf.name == b[1]);
    GLint cur = 0; h_glGetIntegerv(0x8a28, &cur); assert(cur == (GLint)b[0]);   // UNIFORM_BUFFER_BINDING
    const uint32_t data[4] = {1, 2, 3, 4};
    h_glBufferData(GL_UNIFORM_BUFFER, sizeof data, data, 0);   // the target's buffer takes the data
    assert(buf_of(b[0])->size == sizeof data && !memcmp(buf_of(b[0])->mb.contents, data, sizeof data) && !buf_of(b[1])->mb);
    range(GL_UNIFORM_BUFFER, 0, b[1], 0, 4); assert(S.uniform_buf.name == b[1] && S.array_buf.name == b[1]);
    base(GL_UNIFORM_BUFFER, 1, b[0]); assert(S.uniform_buf.name == b[0]);
    h_glBufferStorage(GL_UNIFORM_BUFFER, 64, NULL, 0); assert(buf_of(b[0])->immutable && buf_of(b[0])->size == 64);
    range(GL_ARRAY_BUFFER, 0, b[0], 0, 4); assert(S.array_buf.name == b[1] && S.uniform_buf.name == b[0]);   // a target the layer does not have: reported, nothing bound
    barrier(0x4000);   // CLIENT_MAPPED_BUFFER_BARRIER_BIT, Fire Emblem's every frame
    del_buf(1, &b[0]); assert(!S.uniform_buf.name && !S.uniform_buf.obj && S.array_buf.name == b[1]);
    del_buf(1, &b[1]); assert(!S.array_buf.name && !S.array_buf.obj);
}

// Compressed arrays keep each layer's block rows separate, including mip levels
// smaller than one block. Exercise the public entry points and read Metal's bytes.
static void test_compressed_arrays(void) {
    void (*image)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLsizei, GLint, GLsizei, const void *) = import("glCompressedTexImage3D");
    void (*sub)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, uint64_t, uint64_t, const void *) = import("glCompressedTexSubImage3D");
    assert(h_eglGetProcAddress("glCompressedTexImage3D") == (void *)image);
    assert(h_eglGetProcAddress("glCompressedTexSubImage3D") == (void *)sub);
    GLuint name; h_glGenTextures(1, &name); h_glBindTexture(GL_TEXTURE_2D_ARRAY, name);
    uint8_t base[128], mip[32], small[32], got[64];
    for (unsigned i = 0; i < sizeof base; i++) base[i] = (uint8_t)(i + 1);
    memset(mip, 0x35, 16); memset(mip + 16, 0x76, 16);
    memset(small, 0x93, 16); memset(small + 16, 0xb2, 16);
    image(GL_TEXTURE_2D_ARRAY, 0, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, 8, 8, 2, 0, sizeof base, base);
    image(GL_TEXTURE_2D_ARRAY, 1, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, 4, 4, 2, 0, sizeof mip, mip);
    image(GL_TEXTURE_2D_ARRAY, 2, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, 2, 2, 2, 0, sizeof small, small);
    tex_t *t = tex_of(name); assert(tex_make(t, 0));
    assert(t->tex.pixelFormat == MTLPixelFormatBC7_RGBAUnorm_sRGB && t->tex.arrayLength == 2 && t->tex.mipmapLevelCount == 3);
    for (unsigned lv = 0; lv < 3; lv++) for (unsigned layer = 0; layer < 2; layer++) {
        unsigned w = 8 >> lv, pitch = lv ? 16 : 32, bytes = lv ? 16 : 64;
        [t->tex getBytes:got bytesPerRow:pitch bytesPerImage:0 fromRegion:MTLRegionMake2D(0, 0, w, w) mipmapLevel:lv slice:layer];
        const uint8_t *want = lv == 0 ? base : lv == 1 ? mip : small;
        assert(!memcmp(got, want + layer * bytes, bytes));
    }
    uint8_t patch[16]; memset(patch, 0xdf, sizeof patch);
    sub(GL_TEXTURE_2D_ARRAY, 0, 4, 4, 1, 4, 4, 1, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, sizeof patch, patch);
    [t->tex getBytes:got bytesPerRow:32 bytesPerImage:0 fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 slice:1];
    assert(!memcmp(got, base + 64, 48) && !memcmp(got + 48, patch, 16));
    // An upload after a consumer was encoded must use an ordered blit, and
    // retain the copied bytes even when the caller immediately reuses its input.
    t->use = g_serial;
    memset(patch, 0x49, sizeof patch);
    sub(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 4, 4, 1, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, sizeof patch, patch);
    memset(patch, 0xef, sizeof patch);
    void (*finish)(void) = import("glFinish"); finish();
    [t->tex getBytes:got bytesPerRow:32 bytesPerImage:0 fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 slice:0];
    for (int i = 0; i < 16; i++) assert(got[i] == 0x49);
    assert(!memcmp(got + 16, base + 16, 48));
    // A partial last block is legal only at the texture's edge. Invalid uploads
    // leave all bytes intact and report the GL error instead of reaching Metal.
    sub(GL_TEXTURE_2D_ARRAY, 2, 0, 0, 0, 2, 2, 2, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, sizeof small, small);
    assert(h_glGetError() == GL_NO_ERROR);
    sub(GL_TEXTURE_2D_ARRAY, 0, 1, 0, 0, 4, 4, 1, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, 16, patch);
    assert(h_glGetError() == GL_INVALID_OPERATION);
    sub(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 2, 4, 4, 1, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, 16, patch);
    assert(h_glGetError() == GL_INVALID_VALUE);
    sub(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 4, 4, 1, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, 15, patch);
    assert(h_glGetError() == GL_INVALID_VALUE);
    // Immutable allocation uses the same upload path; NULL defines storage.
    h_glDeleteTextures(1, &name); h_glGenTextures(1, &name); h_glBindTexture(GL_TEXTURE_2D_ARRAY, name);
    h_glTexStorage3D(GL_TEXTURE_2D_ARRAY, 2, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, 4, 4, 2);
    sub(GL_TEXTURE_2D_ARRAY, 1, 0, 0, 0, 2, 2, 2, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, sizeof small, small);
    t = tex_of(name); assert(t->tex && t->tex.arrayLength == 2);
    [t->tex getBytes:got bytesPerRow:16 bytesPerImage:0 fromRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:1 slice:1];
    assert(!memcmp(got, small + 16, 16));
    image(GL_TEXTURE_2D_ARRAY, 0, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, 4, 4, 2, 0, sizeof small, small);
    assert(h_glGetError() == GL_INVALID_OPERATION);   // immutable storage cannot be redefined
    h_glDeleteTextures(1, &name);
    h_glGenTextures(1, &name); h_glBindTexture(GL_TEXTURE_2D_ARRAY, name);
    image(GL_TEXTURE_2D_ARRAY, 0, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, 4, 4, 2, 0, sizeof small, NULL);
    image(GL_TEXTURE_2D_ARRAY, 1, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, 2, 2, 2, 0, sizeof small, NULL);
    sub(GL_TEXTURE_2D_ARRAY, 1, 0, 0, 0, 2, 2, 2, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, sizeof small, small);
    t = tex_of(name); assert(!t->made && tex_make(t, 0) && t->tex.mipmapLevelCount == 2);   // the fill waited in the pending level
    [t->tex getBytes:got bytesPerRow:16 bytesPerImage:0 fromRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:1 slice:0];
    assert(!memcmp(got, small, 16));
    h_glDeleteTextures(1, &name);
    // Each level allocated with no data (imageSize 0) and filled before the next is given: the fills go into the pending levels, so the texture is made
    // with every level, not with the levels given at the first fill
    h_glGenTextures(1, &name); h_glBindTexture(GL_TEXTURE_2D_ARRAY, name);
    image(GL_TEXTURE_2D_ARRAY, 0, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, 8, 8, 2, 0, 0, NULL);
    sub(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 8, 8, 2, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, sizeof base, base);
    memset(patch, 0x5a, sizeof patch);
    sub(GL_TEXTURE_2D_ARRAY, 0, 4, 0, 1, 4, 4, 1, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, sizeof patch, patch);
    image(GL_TEXTURE_2D_ARRAY, 1, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, 4, 4, 2, 0, 0, NULL);
    sub(GL_TEXTURE_2D_ARRAY, 1, 0, 0, 0, 4, 4, 2, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, sizeof mip, mip);
    assert(h_glGetError() == GL_NO_ERROR);
    t = tex_of(name); assert(!t->made && tex_make(t, 0) && t->tex.mipmapLevelCount == 2);
    [t->tex getBytes:got bytesPerRow:32 bytesPerImage:0 fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 slice:1];
    assert(!memcmp(got, base + 64, 16) && !memcmp(got + 16, patch, 16) && !memcmp(got + 32, base + 96, 32));
    [t->tex getBytes:got bytesPerRow:16 bytesPerImage:0 fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:1 slice:1];
    assert(!memcmp(got, mip + 16, 16));
    h_glDeleteTextures(1, &name);
    // The host's gaps (a volume target, a compressed format the layer does not map) record no GL error; an uncompressed format is the caller's error
    h_glGenTextures(1, &name); h_glBindTexture(GL_TEXTURE_3D, name);
    image(GL_TEXTURE_3D, 0, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, 4, 4, 2, 0, sizeof small, small);
    assert(h_glGetError() == GL_NO_ERROR);
    h_glDeleteTextures(1, &name); h_glGenTextures(1, &name); h_glBindTexture(GL_TEXTURE_2D_ARRAY, name);
    image(GL_TEXTURE_2D_ARRAY, 0, 0x9274 /* GL_COMPRESSED_RGB8_ETC2 */, 4, 4, 1, 0, 8, small);
    assert(h_glGetError() == GL_NO_ERROR);
    image(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 4, 4, 1, 0, 64, small);
    assert(h_glGetError() == GL_INVALID_ENUM);
    h_glDeleteTextures(1, &name);
}

static void test_packed_uploads(void) {
    static const struct { GLenum internal, format, type; MTLPixelFormat metal; } cases[] = {
        {GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, MTLPixelFormatRGB10A2Unorm},
        {GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV, MTLPixelFormatRG11B10Float},
    };
    // Two one-pixel rows, with an unused packed word between them. Alignment 1 exposes a three-byte source layout.
    const uint8_t src[] = {0x12, 0x34, 0x56, 0x78, 0xee, 0xee, 0xee, 0xee, 0xaa, 0xbb, 0xcc, 0xdd};
    const uint8_t want[][8] = {
        {0x12, 0x34, 0x56, 0x78, 0xaa, 0xbb, 0xcc, 0xdd},
        {0x78, 0x56, 0x34, 0x12, 0xdd, 0xcc, 0xbb, 0xaa},
    };
    h_glPixelStorei(GL_UNPACK_ROW_LENGTH, 2);
    h_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    int failures = 0;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        GLuint name;
        h_glGenTextures(1, &name); h_glBindTexture(GL_TEXTURE_2D, name);
        for (int swap = 0; swap <= 1; swap++) {
            h_glPixelStorei(0xcf0, swap);   // GL_UNPACK_SWAP_BYTES
            h_glTexImage2D(GL_TEXTURE_2D, 0, cases[i].internal, 1, 2, 0, cases[i].format, cases[i].type, src);
            assert(h_glGetError() == GL_NO_ERROR);
            tex_t *t = tex_of(name);
            assert(t && tex_make(t, 0) && t->tex.pixelFormat == cases[i].metal);
            uint8_t got[sizeof want[0]];
            [t->tex getBytes:got bytesPerRow:4 fromRegion:MTLRegionMake2D(0, 0, 1, 2) mipmapLevel:0];
            if (memcmp(got, want[swap], sizeof got)) {
                fprintf(stderr, "GL packed upload type %#x, GL_UNPACK_SWAP_BYTES=%d: wrong bytes\n", cases[i].type, swap);
                failures++;
            }
        }
        h_glDeleteTextures(1, &name);
    }
    h_glPixelStorei(0xcf0, 0);
    h_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    h_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    assert(!failures);
    puts("GL packed uploads (RGB10_A2, R11F_G11F_B10F): OK");
}

// What DOOM 3's renderer (id Tech 4's BFG Edition) needs of the layer: its ARB names and extension check, named uniform blocks bound by range, two-sided stencil, frame copies into
// a texture, streamed compressed images, byte-swapped 16-bit images, programs with a vertex shader alone, sync queries and wireframe
static void test_doom3(void) {
    // An ARB name of a core function gets the core entry; ARB_fragment_program's own names have theirs; an ARB name of nothing stays missing
    assert(gl_lookup("glBindBufferARB") == gl_lookup("glBindBuffer") && gl_lookup("glBindBuffer"));
    assert(gl_lookup("glActiveTextureARB") == (void *)h_glActiveTexture_ctx);
    assert(gl_lookup("glCompressedTexSubImage2DARB") == import("glCompressedTexSubImage2D"));
    assert(gl_lookup("glGenProgramsARB") == import("glGenProgramsARB") && !gl_lookup("glNoSuchFunctionARB") && !gl_lookup("ARB"));
    static const char *const need[] = {"GL_ARB_multitexture", "GL_ARB_texture_compression", "GL_EXT_texture_compression_s3tc", "GL_ARB_vertex_buffer_object", "GL_ARB_map_buffer_range",
        "GL_ARB_draw_elements_base_vertex", "GL_ARB_vertex_array_object", "GL_ARB_uniform_buffer_object", "GL_ARB_buffer_storage", "GL_ATI_separate_stencil", "GL_ARB_fragment_program"};
    const char *ext = h_glGetString(GL_EXTENSIONS); GLint next = 0; h_glGetIntegerv(GL_NUM_EXTENSIONS, &next);
    for (size_t i = 0; i < sizeof need / sizeof *need; i++) {
        char word[64]; snprintf(word, sizeof word, " %s ", need[i]); char all[2048]; snprintf(all, sizeof all, " %s ", ext); assert(strstr(all, word));   // a whole name
        int listed = 0; for (GLint k = 0; k < next; k++) listed |= !strcmp(h_glGetStringi(GL_EXTENSIONS, (GLuint)k), need[i]);
        assert(listed);
    }
    GLint v = 0; h_glGetIntegerv(0x8a34, &v); assert(v == 256);   // UNIFORM_BUFFER_OFFSET_ALIGNMENT

    // A named block, merged over the stages by name: its index, its binding point, and the range a draw reads there (whole, or with zeros past a short range or the buffer's end)
    GLuint (*blkidx)(GLuint, const GLchar *) = import("glGetUniformBlockIndex"); void (*blkbind)(GLuint, GLuint, GLuint) = import("glUniformBlockBinding");
    void (*range)(GLenum, GLuint, GLuint, GLintptr, GLsizeiptr) = import("glBindBufferRange");
    static shblk vb = {"matrices_ubo", 1, 64}, fb = {"matrices_ubo", 2, 64};
    static shinfo vsi = {.ub_slot = -1, .nb = 1, .b = &vb}, fsi = {.frag = 1, .ub_slot = -1, .nb = 1, .b = &fb};
    GLuint pr = h_glCreateProgram(), vs = h_glCreateShader(GL_VERTEX_SHADER), fs = h_glCreateShader(GL_FRAGMENT_SHADER);
    id vfn = [NSObject new], ffn = [NSObject new];
    shader_of(vs)->si = &vsi; shader_of(vs)->fn = vfn; shader_of(fs)->si = &fsi; shader_of(fs)->fn = ffn;
    h_glAttachShader(pr, vs); h_glAttachShader(pr, fs); h_glLinkProgram(pr);
    prog_t *p = prog_of(pr); assert(p->linked && p->nblk == 1 && p->blk[0].slot[0] == 1 && p->blk[0].slot[1] == 2 && p->blk[0].size == 64 && !p->blk[0].binding);
    assert(blkidx(pr, "matrices_ubo") == 0 && blkidx(pr, "other") == 0xffffffffu);
    blkbind(pr, 0, 5); assert(p->blk[0].binding == 5);
    blkbind(pr, 1, 5); assert(h_glGetError() == GL_INVALID_VALUE);
    uint8_t data[256]; for (int i = 0; i < 256; i++) data[i] = (uint8_t)(i + 1);
    GLuint ub; gen_buf(1, &ub); bind_buf(GL_UNIFORM_BUFFER, ub); h_glBufferData(GL_UNIFORM_BUFFER, sizeof data, data, 0);
    size_t mo; id<MTLBuffer> mb;
    range(GL_UNIFORM_BUFFER, 5, ub, 128, 64); mb = blk_buffer(&p->blk[0], &mo); assert(mb == buf_of(ub)->mb && mo == 128);   // the range itself
    range(GL_UNIFORM_BUFFER, 5, ub, 192, 16); mb = blk_buffer(&p->blk[0], &mo); assert(mb != buf_of(ub)->mb);               // 16 of the block's 64 bytes: a copy
    const uint8_t *c = (const uint8_t *)mb.contents + mo; assert(!memcmp(c, data + 192, 16)); for (int i = 16; i < 64; i++) assert(!c[i]);
    h_glBindBufferBase(GL_UNIFORM_BUFFER, 5, ub); mb = blk_buffer(&p->blk[0], &mo); assert(mb == buf_of(ub)->mb && !mo);    // the whole buffer
    range(GL_UNIFORM_BUFFER, 5, ub, 224, 64); mb = blk_buffer(&p->blk[0], &mo); c = (const uint8_t *)mb.contents + mo;      // past the buffer's end
    assert(!memcmp(c, data + 224, 32)); for (int i = 32; i < 64; i++) assert(!c[i]);
    del_buf(1, &ub); assert(!S.ubo[5].buf.name && !S.ubo[5].buf.obj); mb = blk_buffer(&p->blk[0], &mo); c = (const uint8_t *)mb.contents + mo; for (int i = 0; i < 64; i++) assert(!c[i]);
    h_glDeleteProgram(pr); h_glDeleteShader(vs); h_glDeleteShader(fs);

    // A program with a vertex shader alone links, and its pipeline has no fragment function and writes no colour
    static const char *src = "#include <metal_stdlib>\n using namespace metal;\n struct VIn { float4 p [[attribute(0)]]; };\n"
        "vertex float4 vmain(VIn in [[stage_in]]) { return in.p; }\n";
    NSError *err = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(src) options:nil error:&err]; assert(lib);
    static shattr at = {.name = "p", .slot = 0}; static shinfo vonly = {.ub_slot = -1, .na = 1, .a = &at};
    pr = h_glCreateProgram(); vs = h_glCreateShader(GL_VERTEX_SHADER); shader_of(vs)->si = &vonly; shader_of(vs)->fn = [lib newFunctionWithName:@"vmain"];
    h_glAttachShader(pr, vs); h_glLinkProgram(pr); p = prog_of(pr);
    assert(p->linked && p->fn[0] && !p->fn[1] && p->si[1]->frag && !p->si[1]->nu && !p->si[1]->nt && !p->si[1]->nb && p->si[1]->ub_slot < 0);
    pkey_t k; memset(&k, 0, sizeof k); k.vf[0] = MTLVertexFormatFloat4; k.stride[0] = 16; k.mask[0] = 15;
    k.cpf[0] = MTLPixelFormatRGBA8Unorm; k.dpf = k.spf = MTLPixelFormatDepth32Float_Stencil8;
    assert(pso_get(p, &k));
    h_glDeleteProgram(pr); h_glDeleteShader(vs);

    // Two-sided stencil: each face its own function, reference, masks and operations; the one-sided calls set both
    void (*fsep)(GLenum, GLenum, GLint, GLuint) = import("glStencilFuncSeparate"), (*osep)(GLenum, GLenum, GLenum, GLenum) = import("glStencilOpSeparate");
    void (*fsep_ati)(GLenum, GLenum, GLint, GLuint) = import("glStencilFuncSeparateATI"), (*msep)(GLenum, GLuint) = import("glStencilMaskSeparate");
    osep(GL_BACK, GL_KEEP, GL_INCR_WRAP, GL_KEEP); osep(GL_FRONT, GL_KEEP, GL_DECR_WRAP, GL_KEEP);
    assert(S.st[0].zfail == GL_DECR_WRAP && S.st[1].zfail == GL_INCR_WRAP && S.st[0].pass == GL_KEEP);
    fsep(GL_FRONT_AND_BACK, GL_NEVER + 7, 128, 0xff); assert(S.st[0].func == GL_NEVER + 7 && S.st[1].ref == 128 && S.st[1].mask == 0xff);
    fsep_ati(GL_NEVER + 1, GL_NEVER + 3, 1, 0xf); assert(S.st[0].func == GL_NEVER + 1 && S.st[1].func == GL_NEVER + 3 && S.st[0].ref == 1);
    msep(GL_BACK, 0x0f); assert(S.st[1].wmask == 0x0f && S.st[0].wmask == 0xffffffff);
    S.stencil = 1; P.spf = MTLPixelFormatDepth32Float_Stencil8;
    id<MTLDepthStencilState> two = dss_get();
    osep(GL_BACK, GL_KEEP, GL_DECR_WRAP, GL_KEEP); assert(dss_get() != two);   // the back face's operations are its own
    h_glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP); h_glStencilFunc(GL_NEVER + 7, 0, 0xffffffff); h_glStencilMask(0xffffffff);
    assert(S.st[1].zfail == GL_KEEP && S.st[1].func == GL_NEVER + 7 && S.st[1].wmask == 0xffffffff);
    osep(0x1234, GL_KEEP, GL_KEEP, GL_KEEP); assert(h_glGetError() == GL_INVALID_ENUM);
    S.stencil = 0; P.spf = MTLPixelFormatInvalid;

    // glCopyTexImage2D / glCopyTexSubImage2D: the read framebuffer's rectangle into the texture, rows as they are, the part outside the framebuffer left alone
    uint32_t px[16]; for (int i = 0; i < 16; i++) px[i] = 0xff000000u | (uint32_t)(i * 9);
    GLuint a, b, fbo; h_glGenTextures(1, &a); h_glBindTexture(GL_TEXTURE_2D, a);
    h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    h_glGenFramebuffers(1, &fbo); h_glBindFramebuffer(GL_FRAMEBUFFER, fbo); h_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, a, 0);
    h_glGenTextures(1, &b); h_glBindTexture(GL_TEXTURE_2D, b);
    h_glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 2, 2, 0);
    h_glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 1, 0, 3, 3, 4, 4);   // from (3, 3): one pixel inside the framebuffer
    h_glFinish();
    tex_t *tb = tex_of(b); assert(tb->tex && tb->tex.width == 2 && tb->tex.height == 2);
    uint32_t got[4]; [tb->tex getBytes:got bytesPerRow:8 fromRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0];
    assert(got[0] == px[5] && got[1] == px[15] && got[2] == px[9] && got[3] == px[10]);
    h_glBindFramebuffer(GL_FRAMEBUFFER, 0); h_glDeleteFramebuffers(1, &fbo); h_glDeleteTextures(1, &a); h_glDeleteTextures(1, &b);

    // glCompressedTexSubImage2D into a level not made yet goes into its pending copy, next to the zeros of the allocation
    void (*sub2d)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *) = import("glCompressedTexSubImage2D");
    uint8_t blk[16], out[64]; memset(blk, 0x6c, sizeof blk);
    h_glGenTextures(1, &a); h_glBindTexture(GL_TEXTURE_2D, a);
    h_glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, 8, 8, 0, 64, NULL);
    sub2d(GL_TEXTURE_2D, 0, 4, 4, 4, 4, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, sizeof blk, blk); assert(h_glGetError() == GL_NO_ERROR);
    tex_t *ta = tex_of(a); assert(!ta->made && tex_make(ta, 0));
    [ta->tex getBytes:out bytesPerRow:32 fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0];
    for (int i = 0; i < 48; i++) assert(!out[i]);
    assert(!memcmp(out + 48, blk, 16));
    sub2d(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 4, 4, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, sizeof blk, blk); assert(h_glGetError() == GL_INVALID_ENUM);   // a 3D target in the 2D call
    h_glDeleteTextures(1, &a);

    // GL_UNPACK_SWAP_BYTES: RGB565 stored big-endian reads as the same colours stored little-endian
    const uint8_t be[4] = {0xf8, 0x00, 0x07, 0xe0};   // red, green
    h_glGenTextures(1, &a); h_glBindTexture(GL_TEXTURE_2D, a);
    h_glPixelStorei(0xcf0, 1); h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 1, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, be); h_glPixelStorei(0xcf0, 0);
    const uint8_t *d = tex_of(a)->pend[0][0].data; assert(d);
    assert(d[0] == 255 && d[1] == 0 && d[2] == 0 && d[3] == 255 && d[4] == 0 && d[5] == 255 && d[6] == 0 && d[7] == 255);
    h_glDeleteTextures(1, &a);

    // Sync queries, wireframe and its polygon offset enable
    void *sy = h_glFenceSync(0x9117, 0); assert(h_glIsSync(sy) && !h_glIsSync(NULL)); h_glDeleteSync(sy);
    h_glPolygonMode(GL_FRONT_AND_BACK, 0x1b01); assert(S.lines); h_glEnable(0x2a02); assert(S.poffset_line);
    h_glPolygonMode(GL_FRONT_AND_BACK, 0x1b02); h_glDisable(0x2a02); assert(!S.lines && !S.poffset_line);
}

// GL names a sync object by value. glIsSync answers for any value; glDeleteSync ignores 0 and, for a value that is no sync object, sets GL_INVALID_VALUE and touches nothing
// (OpenGL 4.6, section 4.1); glClientWaitSync on one returns WAIT_FAILED with that error (GitHub issue #2835)
static void test_sync_names(void) {
    static uint64_t not_a_sync[4] = {0x1111111111111111ull, 0x2222, 0x3333, 0x4444};   // static storage: no malloc block
    (void)h_glGetError();
    assert(!h_glIsSync(NULL) && !h_glIsSync(not_a_sync) && h_glGetError() == GL_NO_ERROR);
    h_glDeleteSync(NULL); assert(h_glGetError() == GL_NO_ERROR);
    h_glDeleteSync(not_a_sync);
    assert(not_a_sync[0] == 0x1111111111111111ull && h_glGetError() == GL_INVALID_VALUE);
    assert(h_glClientWaitSync((void *)(uintptr_t)0x10, 0, 0) == 0x911d && h_glGetError() == GL_INVALID_VALUE);
    assert(h_glClientWaitSync(NULL, 0, 0) == 0x911d && h_glGetError() == GL_INVALID_VALUE);
    void *sy = h_glFenceSync(0x9117, 0); assert(sy && h_glIsSync(sy));   // a live one, and a deleted one: its name answers false and a second delete is an error, not a second free
    h_glDeleteSync(sy); assert(h_glGetError() == GL_NO_ERROR && !h_glIsSync(sy));
    h_glDeleteSync(sy); assert(h_glGetError() == GL_INVALID_VALUE);
    printf("GL sync names: OK\n");
}

// GL_TEXTURE_LOD_BIAS goes to the texture's sampler: a 4x4 texture drawn 1:1 into a 4x4 target samples level 0 (red), and with a bias of 1 level 1 (green).
// Hardware check: sampler LOD-bias readback fails on CI's virtual Metal device.
static void test_lod_bias(void) {
    NSError *err = nil; GLuint a; tex_t *ta;
    static const char *bsrc = "#include <metal_stdlib>\n using namespace metal;\n struct V { float4 p [[position]]; float2 uv; };\n"
        "vertex V bv(uint i [[vertex_id]]) { float2 q = float2((i << 1) & 2, i & 2); V o; o.p = float4(q * 2 - 1, 0, 1); o.uv = q; return o; }\n"
        "fragment float4 bf(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { return t.sample(s, in.uv); }\n";
    id<MTLLibrary> blib = [g_dev newLibraryWithSource:@(bsrc) options:nil error:&err]; assert(blib);
    MTLRenderPipelineDescriptor *bpd = [MTLRenderPipelineDescriptor new];
    bpd.vertexFunction = [blib newFunctionWithName:@"bv"]; bpd.fragmentFunction = [blib newFunctionWithName:@"bf"]; bpd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> bpso = [g_dev newRenderPipelineStateWithDescriptor:bpd error:&err]; assert(bpso);
    uint32_t red[16], green[4]; for (int i = 0; i < 16; i++) red[i] = 0xff0000ffu; for (int i = 0; i < 4; i++) green[i] = 0xff00ff00u;
    h_glGenTextures(1, &a); h_glBindTexture(GL_TEXTURE_2D, a);
    h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, red); h_glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, green);
    h_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST); h_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    ta = tex_of(a); assert(tex_make(ta, 0) && ta->levels == 2);
    for (int bias = 0; bias <= 1; bias++) {
        h_glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, (float)bias);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
        id<MTLTexture> rt = [g_dev newTextureWithDescriptor:td];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:bpso]; [e setFragmentTexture:ta->tex atIndex:0]; [e setFragmentSamplerState:tex_sampler(ta, 0) atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
        uint32_t px[16]; [rt getBytes:px bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
        assert(px[5] == (bias ? green[0] : red[0]));
    }
    h_glDeleteTextures(1, &a);
}

// A 4x4 texture of two levels (red, then green) drawn 1:1 into a 4x4 target with the sampler a unit would use: the colour of a middle texel
static uint32_t draw_texel(tex_t *t, id<MTLSamplerState> s) {
    static id<MTLRenderPipelineState> pso;
    if (!pso) {
        static const char *src = "#include <metal_stdlib>\n using namespace metal;\n struct V { float4 p [[position]]; float2 uv; };\n"
            "vertex V sv(uint i [[vertex_id]]) { float2 q = float2((i << 1) & 2, i & 2); V o; o.p = float4(q * 2 - 1, 0, 1); o.uv = q; return o; }\n"
            "fragment float4 sf(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { return t.sample(s, in.uv); }\n";
        NSError *err = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(src) options:nil error:&err]; assert(lib);
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"sv"]; pd.fragmentFunction = [lib newFunctionWithName:@"sf"]; pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err]; assert(pso);
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> rt = [g_dev newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:pso]; [e setFragmentTexture:t->tex atIndex:0]; [e setFragmentSamplerState:s atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    uint32_t px[16]; [rt getBytes:px bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
    return px[5];
}

// GL sampler objects (Mega Man Legacy Collection sets its filters and wraps on one per unit, none on its textures), and uniform locations, of which none is 0
static void test_sampler_objects(int gpu) {
    const uint32_t red = 0xff0000ffu, green = 0xff00ff00u;
    uint32_t l0[16], l1[4]; for (int i = 0; i < 16; i++) l0[i] = red; for (int i = 0; i < 4; i++) l1[i] = green;
    GLuint t; h_glGenTextures(1, &t); h_glActiveTexture(GL_TEXTURE0 + 2); h_glBindTexture(GL_TEXTURE_2D, t);
    h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, l0); h_glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, l1);
    h_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); h_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    tex_t *ta = tex_of(t); assert(tex_make(ta, 0) && ta->levels == 2);
    GLuint s[2]; h_glGenSamplers(2, s); assert(s[0] && s[1] != s[0] && h_glIsSampler(s[0]) && !h_glIsSampler(s[1] + 100));
    so_t *o = so_of(s[0]); assert(o->sp.minf == GL_NEAREST_MIPMAP_LINEAR && o->sp.ws == GL_REPEAT && o->sp.aniso == 1);   // GL's defaults
    h_glSamplerParameteri(s[0], GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST); h_glSamplerParameteri(s[0], GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    h_glSamplerParameterf(s[0], GL_TEXTURE_LOD_BIAS, 1);
    h_glSamplerParameteri(s[0], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); h_glSamplerParameterf(s[0], GL_TEXTURE_MIN_LOD, -1000);
    const GLfloat border[4] = {0, 0, 0, 1}; h_glSamplerParameterfv(s[0], GL_TEXTURE_BORDER_COLOR, border);
    h_glSamplerParameteri(s[0], GL_TEXTURE_COMPARE_FUNC, 0x206); assert(o->sp.cfunc == 0x206 && !o->sp.cmode);   // a function set before the mode is kept
    h_glSamplerParameteri(s[0], GL_TEXTURE_COMPARE_MODE, 0); assert(o->sp.ws == GL_CLAMP_TO_EDGE && o->sp.border[3] == 1 && o->sp.lodbias == 1);
    // the texture's own parameters read level 0; the sampler bound to its unit (a mipmap filter, a bias of 1) level 1; unbound, level 0 again
    if (gpu) assert(draw_texel(ta, tex_sampler(ta, S.units[2].samp)) == red);
    h_glBindSampler(2, s[0]); assert(S.units[2].samp == s[0] && !S.units[0].samp);
    if (gpu) assert(draw_texel(ta, tex_sampler(ta, S.units[2].samp)) == green);
    if (gpu) assert(draw_texel(ta, tex_sampler(ta, S.units[0].samp)) == red);   // another unit keeps the texture's own
    h_glBindSampler(2, 0); if (gpu) assert(draw_texel(ta, tex_sampler(ta, S.units[2].samp)) == red);
    // a sampler's MAX_LOD below 1 keeps level 0 whatever the bias
    h_glSamplerParameterf(s[0], GL_TEXTURE_MAX_LOD, 0); h_glBindSampler(2, s[0]); if (gpu) assert(draw_texel(ta, tex_sampler(ta, S.units[2].samp)) == red);
    // deleting a bound sampler unbinds it
    h_glDeleteSamplers(1, &s[0]); assert(!so_of(s[0]) && !S.units[2].samp && !h_glIsSampler(s[0]));
    h_glDeleteSamplers(1, &s[1]); h_glDeleteTextures(1, &t); h_glActiveTexture(GL_TEXTURE0);

    // Uniform locations: (index + 1) x 4096 + element, never 0 (the game skips its glUniform1f of u_flip at location 0); a location below 4096 names nothing
    GLuint pr = h_glCreateProgram(); prog_t *p = prog_of(pr); assert(p);
    static puni_t u[2] = {{.name = "u_flip", .type = T_FLOAT, .off = {-1, -1}}, {.name = "T0", .type = T_SAMP2D, .off = {-1, -1}}};
    p->linked = 1; p->nu = 2; p->u = u; GLuint was = S.prog; S.prog = pr;
    GLint lf = h_glGetUniformLocation(pr, "u_flip"), lt = h_glGetUniformLocation(pr, "T0");
    assert(lf == 4096 && lt == 2 * 4096 && h_glGetUniformLocation(pr, "u_flip[1]") == -1 && h_glGetUniformLocation(pr, "none") == -1);
    h_glUniform1i(lt, 3); assert(u[1].unit == 3);
    h_glUniform1i(0, 5); h_glUniform1i(4095, 5); assert(u[1].unit == 3);
    GLint back = -1; h_glGetUniformiv(pr, lt, &back); assert(back == 3);
    S.prog = was; p->linked = 0; p->nu = 0; p->u = NULL; h_glDeleteProgram(pr);
}

// Extension names by gl_aliases.h (Fate/hollow ataraxia REMASTERED asks for its framebuffer functions by their EXT names, issue #2818), never by a stripped suffix; binds that
// make a name glGen* never gave; EGL's current context, display and surfaces, per thread
typedef struct { EGLContext context; EGLSurface draw, read; void *before, *after; EGLBoolean bound; EGLint error, next_error; } egl_attempt;
static void *other_thread(void *arg) {
    egl_attempt *seen = arg; seen->before = h_eglGetCurrentContext();
    seen->bound = h_eglMakeCurrent((EGLDisplay)&g_egl, seen->draw, seen->read, seen->context);
    seen->after = h_eglGetCurrentContext(); seen->error = h_eglGetError(); seen->next_error = h_eglGetError();
    return NULL;
}
static void test_extension_names_and_egl(void) {
    assert(gl_lookup("glGenFramebuffersEXT") == (void *)h_glGenFramebuffers_ctx && gl_lookup("glCheckFramebufferStatusEXT") == (void *)h_glCheckFramebufferStatus_ctx);
    assert(gl_lookup("glBindRenderbufferEXT") == (void *)h_glBindRenderbuffer_ctx && gl_lookup("glBindFramebufferEXT") == gl_lookup("glBindFramebuffer"));
    assert(gl_lookup("glGenFramebuffersOES") == (void *)h_glGenFramebuffers_ctx && gl_lookup("glBindVertexArrayOES") == (void *)h_glBindVertexArray_ctx);
    assert(!gl_lookup("glVertexAttribPointerNV") && !gl_lookup("glPolygonOffsetEXT") && !gl_lookup("glDrawArraysEXTRA"));   // another function under the stripped name
    assert(gl_lookup("glGetProgramivARB") == (void *)h_glGetProgramivARB_ctx && gl_lookup("glGetProgramivARB") != gl_lookup("glGetProgramiv"));
    GLint v = 0; h_glGetProgramivARB(0x8804, 0x88b6, &v); assert(v == 1);   // FRAGMENT_PROGRAM_ARB: within the native limits
    // a bind of a name never generated makes it (GL ES, the compatibility profile, EXT_framebuffer_object)
    assert(!rb_of(77) && !fb_of(78));
    h_glBindRenderbuffer(GL_RENDERBUFFER, 77); assert(rb_of(77) && S.rb == 77);
    h_glBindFramebuffer(GL_FRAMEBUFFER, 78); assert(fb_of(78) && S.fb == 78 && S.read_fb == 78);
    h_glBindFramebuffer(GL_FRAMEBUFFER, 0); h_glBindRenderbuffer(GL_RENDERBUFFER, 0);
    GLuint n = 78; h_glDeleteFramebuffers(1, &n); n = 77; h_glDeleteRenderbuffers(1, &n); assert(!rb_of(77) && !fb_of(78));
    // EGL names through eglGetProcAddress; the current binding per thread, read and draw apart, released by a make-current of no context or eglReleaseThread
    assert(h_eglGetProcAddress("eglGetCurrentDisplay") == (void *)h_eglGetCurrentDisplay_bs && h_eglGetProcAddress("eglMakeCurrent") == (void *)h_eglMakeCurrent_bs);
    assert(!h_eglGetCurrentContext() && !h_eglGetCurrentDisplay() && !h_eglGetCurrentSurface(0x3059));
    EGLDisplay d = (EGLDisplay)&g_egl;
    EGLContext context = h_eglCreateContext(d, NULL, NULL, NULL); assert(context);
    EGLSurface win = h_eglCreateWindowSurface(d, NULL, NULL, NULL);
    EGLSurface pbuf = h_eglCreatePbufferSurface(d, NULL, NULL);
    assert(win && pbuf && win != pbuf);
    assert(h_eglMakeCurrent(d, win, pbuf, context));
    assert(h_eglGetCurrentContext() == context && h_eglGetCurrentDisplay() == d);
    assert(h_eglGetCurrentSurface(0x3059) == win && h_eglGetCurrentSurface(0x305a) == pbuf && !h_eglGetCurrentSurface(0x3000));
    egl_attempt seen = {.context = context, .draw = win, .read = win}; pthread_t th; pthread_create(&th, NULL, other_thread, &seen); pthread_join(th, NULL);
    assert(!seen.before && !seen.bound && !seen.after && seen.error == 0x3002 && seen.next_error == 0x3000);   // BAD_ACCESS, cleared on read
    assert(h_eglGetCurrentContext() == context && h_eglGetCurrentSurface(0x305a) == pbuf && h_eglGetError() == 0x3000);
    assert(h_eglMakeCurrent(d, NULL, NULL, NULL)); assert(!h_eglGetCurrentContext() && !h_eglGetCurrentDisplay() && !h_eglGetCurrentSurface(0x3059));
    pthread_create(&th, NULL, other_thread, &seen); pthread_join(th, NULL);
    assert(!seen.before && seen.bound && seen.after == context && seen.error == 0x3000);   // released here, acquired there; thread exit releases it too
    assert(h_eglMakeCurrent(d, win, win, context)); h_eglReleaseThread(); assert(!h_eglGetCurrentContext());
    assert(h_eglDestroySurface(d, win) && h_eglDestroySurface(d, pbuf));
    assert(h_eglDestroyContext(d, context));
}

typedef struct {
    EGLDisplay dpy;
    EGLContext ctx;
    EGLSurface surf;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int ready, release;
    EGLBoolean bound;
    GLint vp[4];
} pbuffer_hold_t;
static void *pbuffer_hold_thread(void *arg) {
    pbuffer_hold_t *h = arg;
    EGLBoolean (*make_current)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = import("eglMakeCurrent");
    void (*get_integer)(GLenum, GLint *) = import("glGetIntegerv");
    EGLBoolean (*release_thread)(void) = import("eglReleaseThread");
    h->bound = make_current(h->dpy, h->surf, h->surf, h->ctx);
    if (h->bound) get_integer(GL_VIEWPORT, h->vp);
    pthread_mutex_lock(&h->mu);
    h->ready = 1;
    pthread_cond_signal(&h->cv);
    while (!h->release) pthread_cond_wait(&h->cv, &h->mu);
    pthread_mutex_unlock(&h->mu);
    assert(release_thread());
    return NULL;
}
static void test_egl_pbuffer_lifetime(void) {
    EGLDisplay d = (EGLDisplay)&g_egl;
    EGLSurface (*create_pbuffer)(EGLDisplay, EGLConfig, const EGLint *) = import("eglCreatePbufferSurface");
    EGLBoolean (*destroy_surface)(EGLDisplay, EGLSurface) = import("eglDestroySurface");
    EGLBoolean (*make_current)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = import("eglMakeCurrent");
    EGLSurface (*get_surface)(EGLint) = import("eglGetCurrentSurface");
    EGLContext (*get_context)(void) = import("eglGetCurrentContext");
    EGLint (*get_error)(void) = import("eglGetError");
    EGLContext (*create_context)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = import("eglCreateContext");
    EGLBoolean (*destroy_context)(EGLDisplay, EGLContext) = import("eglDestroyContext");
    void (*get_integer)(GLenum, GLint *) = import("glGetIntegerv");

    const EGLint bad_attr[] = {0x3057, -1, 0x3056, 4, 0x3038};
    assert(!create_pbuffer(d, NULL, bad_attr) && get_error() == 0x300c);   // EGL_BAD_PARAMETER
    const EGLint attr1[] = {0x3057, 16, 0x3056, 8, 0x3038};
    const EGLint attr2[] = {0x3057, 32, 0x3056, 16, 0x3038};
    EGLSurface pb1 = create_pbuffer(d, NULL, attr1);
    EGLSurface pb2 = create_pbuffer(d, NULL, attr2);
    assert(pb1 && pb2 && pb1 != pb2);
    EGLContext c1 = create_context(d, NULL, NULL, NULL);
    EGLContext c2 = create_context(d, NULL, c1, NULL);
    assert(c1 && c2);

    pbuffer_hold_t hold = {.dpy = d, .ctx = c1, .surf = pb1, .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};
    pthread_t th; assert(!pthread_create(&th, NULL, pbuffer_hold_thread, &hold));
    pthread_mutex_lock(&hold.mu);
    while (!hold.ready) pthread_cond_wait(&hold.cv, &hold.mu);
    pthread_mutex_unlock(&hold.mu);
    assert(hold.bound && hold.vp[2] == 16 && hold.vp[3] == 8);

    // Separate pbuffer binds simultaneously on this thread while thread 1 holds pb1.
    assert(make_current(d, pb2, pb2, c2));
    assert(get_context() == c2 && get_surface(0x3059) == pb2 && get_surface(0x305a) == pb2);
    GLint vp[4] = {0}; get_integer(GL_VIEWPORT, vp); assert(vp[2] == 32 && vp[3] == 16);

    // Competing for pb1 (still current on thread 1) fails with EGL_BAD_ACCESS and preserves c2/pb2.
    assert(!make_current(d, pb1, pb1, c2) && get_error() == 0x3002);
    assert(get_context() == c2 && get_surface(0x3059) == pb2 && get_surface(0x305a) == pb2);

    // Unknown surface handle fails with EGL_BAD_SURFACE.
    assert(!make_current(d, (EGLSurface)(uintptr_t)0xdead, (EGLSurface)(uintptr_t)0xdead, c2) && get_error() == 0x300d);

    // Destroying pb2 while current defers freeing until unbound, while invalidating its handle immediately.
    assert(destroy_surface(d, pb2));
    assert(get_surface(0x3059) == pb2);
    assert(!destroy_surface(d, pb2) && get_error() == 0x300d);
    assert(!make_current(d, pb2, pb2, c2) && get_error() == 0x300d);
    assert(make_current(d, NULL, NULL, NULL));

    pthread_mutex_lock(&hold.mu);
    hold.release = 1;
    pthread_cond_signal(&hold.cv);
    pthread_mutex_unlock(&hold.mu);
    assert(!pthread_join(th, NULL));
    pthread_cond_destroy(&hold.cv);
    pthread_mutex_destroy(&hold.mu);

    // Once thread 1 releases pb1, this thread can bind it, then destroy it.
    assert(make_current(d, pb1, pb1, c2));
    assert(make_current(d, NULL, NULL, NULL));
    assert(destroy_surface(d, pb1));
    assert(!make_current(d, pb1, pb1, c2) && get_error() == 0x300d);
    assert(destroy_context(d, c1) && destroy_context(d, c2));
    assert(!g_surfaces);
    puts("EGL pbuffers: distinct identities, concurrent threads, BAD_ACCESS and deferred destruction: OK");
}

static void test_egl_client_api(void) {
    EGLDisplay d = &g_egl;
    const EGLint renderable[] = {EGL_RENDERABLE_TYPE, 0x40, 0x3038};
    const EGLint requested[] = {0x3098, 3, 0x3038};
    EGLConfig cfg;
    EGLint n;
    assert(h_eglChooseConfig(d, renderable, &cfg, 1, &n) && n == 1);
    assert(h_eglGetConfigAttrib(d, cfg, EGL_RENDERABLE_TYPE, &n) && (n & 0x40));
    EGLContext es = h_eglCreateContext(d, cfg, NULL, requested);
    assert(es && h_eglMakeCurrent(d, NULL, NULL, es));
    const char *(*get_string)(GLenum) = import("glGetString");
    void (*get_integer)(GLenum, GLint *) = import("glGetIntegerv");
    int major, minor;
    assert(sscanf(get_string(GL_VERSION), "OpenGL ES %d.%d", &major, &minor) == 2);
    assert(major == 3 && minor == 0);
    get_integer(GL_MAJOR_VERSION, &n); assert(n == major);
    get_integer(GL_MINOR_VERSION, &n); assert(n == minor);
    get_integer(GL_MAX_TEXTURE_SIZE, &n); assert(n >= 2048);
    get_integer(0x8dfa, &n); assert(n == 1);   // GL_SHADER_COMPILER
    assert(!strcmp(get_string(GL_SHADING_LANGUAGE_VERSION), "OpenGL ES GLSL ES 3.00"));
    GLuint fbo;
    uint32_t (*is_framebuffer)(GLuint) = import("glIsFramebuffer");
    h_glGenFramebuffers(1, &fbo); assert(!is_framebuffer(fbo));
    h_glBindFramebuffer(GL_FRAMEBUFFER, fbo); assert(is_framebuffer(fbo));
    h_glDeleteFramebuffers(1, &fbo); assert(!is_framebuffer(fbo));
    assert(h_eglBindAPI(EGL_OPENGL_API) && h_eglQueryAPI() == EGL_OPENGL_API);
    EGLContext desktop = h_eglCreateContext(d, NULL, NULL, NULL);
    assert(desktop && h_eglMakeCurrent(d, NULL, NULL, desktop));
    assert(!strcmp(get_string(GL_VERSION), "4.5.0 NVIDIA 349.00"));
    assert(h_eglMakeCurrent(d, NULL, NULL, es));
    assert(!strcmp(get_string(GL_VERSION), "OpenGL ES 3.0"));
    GLuint rb;
    h_glGenRenderbuffers(1, &rb);
    h_glBindRenderbuffer(GL_RENDERBUFFER, rb);
    rb_of(rb)->fmt = F_D32FS8;
    rb_of(rb)->w = 1;
    h_glGetRenderbufferParameteriv(GL_RENDERBUFFER, 0x8d55, &n); assert(n == 8);
    h_glGenFramebuffers(1, &fbo); h_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    h_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
    get_integer(0xd56, &n); assert(n == 32);
    get_integer(0xd57, &n); assert(n == 8);
    assert(h_eglReleaseThread());
    assert(h_eglQueryAPI() == EGL_OPENGL_ES_API);
    assert(h_eglDestroyContext(d, desktop) && h_eglDestroyContext(d, es));
    puts("EGL client APIs: per-context OpenGL ES version and texture limits: OK");
}

typedef struct { EGLContext context; GLuint texture; void *sync; } shared_objects;
static void *shared_render_thread(void *arg) {
    const shared_objects *shared = arg;
    EGLBoolean (*make_current)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = import("eglMakeCurrent");
    assert(make_current(&g_egl, NULL, NULL, shared->context));
    GLint viewport[4];
    void (*get_integer)(GLenum, GLint *) = import("glGetIntegerv");
    get_integer(GL_VIEWPORT, viewport);
    assert(viewport[2] == 0 && viewport[3] == 0);
    get_integer(GL_FRAMEBUFFER_BINDING, viewport);
    assert(viewport[0] == 0);
    assert(((uint32_t (*)(void *))import("glIsSync"))(shared->sync));
    id<MTLTexture> image = (__bridge_transfer id<MTLTexture>)dm_gl_retain_texture_2d(shared->texture);
    assert(image.width == 4 && image.height == 2);
    ((void (*)(GLint, GLint, GLsizei, GLsizei))import("glViewport"))(0, 0, 7, 5);
    return NULL;   // the thread's exit releases its context
}
static void test_shared_contexts(void) {
    dev_init();
    EGLContext first = h_eglCreateContext(&g_egl, NULL, NULL, NULL);
    EGLContext shared = h_eglCreateContext(&g_egl, NULL, first, NULL);
    EGLContext isolated = h_eglCreateContext(&g_egl, NULL, NULL, NULL);
    assert(first && shared && isolated && first != shared && shared != isolated);
    assert(h_eglMakeCurrent(&g_egl, NULL, NULL, first));
    GLuint texture, fb;
    h_glGenTextures(1, &texture); h_glBindTexture(GL_TEXTURE_2D, texture);
    // Exercise the stack-argument wrapper as well as the context switch.
    void (*tex_image)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *) = import("glTexImage2D");
    uint8_t pixels[32] = {0x3c, 0x51, 0x60, 0xff};
    tex_image(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    h_glGenFramebuffers(1, &fb); h_glBindFramebuffer(GL_FRAMEBUFFER, fb);
    h_glViewport(0, 0, 4, 2);
    void *fence = h_glFenceSync(0x9117, 0);
    shared_objects work = {shared, texture, fence};
    pthread_t th; assert(!pthread_create(&th, NULL, shared_render_thread, &work));
    assert(!pthread_join(th, NULL));
    GLint viewport[4];
    ((void (*)(GLenum, GLint *))import("glGetIntegerv"))(GL_VIEWPORT, viewport);
    assert(viewport[2] == 4 && viewport[3] == 2);
    assert(h_eglDestroyContext(&g_egl, first));
    assert(h_eglMakeCurrent(&g_egl, NULL, NULL, shared));
    h_glGetIntegerv(GL_VIEWPORT, viewport);
    assert(viewport[2] == 7 && viewport[3] == 5);
    assert(!fb_of(fb) && h_glIsSync(fence) && tex_of(texture));
    assert(h_eglMakeCurrent(&g_egl, NULL, NULL, isolated));
    assert(!h_glIsSync(fence) && !tex_of(texture));
    assert(h_eglReleaseThread());
    assert(h_eglDestroyContext(&g_egl, shared));
    assert(h_eglDestroyContext(&g_egl, isolated));
    puts("EGL shared contexts: two threads, local bindings and share-group names: OK");
}

static void test_shared_buffer_lifetime(void) {
    dev_init();
    EGLBoolean (*make_current)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = import("eglMakeCurrent");
    void (*gen_buffers)(GLsizei, GLuint *) = import("glGenBuffers");
    void (*bind_buffer)(GLenum, GLuint) = import("glBindBuffer");
    void (*bind_buffer_range)(GLenum, GLuint, GLuint, GLintptr, GLsizeiptr) = import("glBindBufferRange");
    void (*bind_buffer_base)(GLenum, GLuint, GLuint) = import("glBindBufferBase");
    void (*delete_buffers)(GLsizei, const GLuint *) = import("glDeleteBuffers");
    void (*buffer_data)(GLenum, GLsizeiptr, const void *, GLenum) = import("glBufferData");
    void (*buffer_sub_data)(GLenum, GLintptr, GLsizeiptr, const void *) = import("glBufferSubData");
    void (*get_buffer_sub_data)(GLenum, GLintptr, GLsizeiptr, void *) = import("glGetBufferSubData");
    void (*get_integer)(GLenum, GLint *) = import("glGetIntegerv");
    void (*gen_vaos)(GLsizei, GLuint *) = import("glGenVertexArrays");
    void (*bind_vao_fn)(GLuint) = import("glBindVertexArray");
    void (*del_vaos)(GLsizei, const GLuint *) = import("glDeleteVertexArrays");
    void (*attrib_ptr)(GLuint, GLint, GLenum, uint32_t, GLsizei, const void *) = import("glVertexAttribPointer");
    GLenum (*get_err)(void) = import("glGetError");

    EGLContext ctx_a = h_eglCreateContext(&g_egl, NULL, NULL, NULL);
    EGLContext ctx_b = h_eglCreateContext(&g_egl, NULL, ctx_a, NULL);
    assert(ctx_a && ctx_b);

    // Context A creates buffer 1 and populates it.
    assert(make_current(&g_egl, NULL, NULL, ctx_a));
    GLuint buf_id = 0; gen_buffers(1, &buf_id); assert(buf_id == 1);
    const uint32_t initial[4] = {11, 22, 33, 44};
    bind_buffer(GL_ARRAY_BUFFER, buf_id);
    @autoreleasepool { buffer_data(GL_ARRAY_BUFFER, sizeof initial, initial, 0); }
    buf_t *orig = buf_of(buf_id);
    assert(orig && orig->mb && orig->refs == 2);   // name table + ctx_a ARRAY_BUFFER
    __weak id<MTLBuffer> weak_orig_mb = orig->mb;

    // Context B binds the same buffer to ARRAY_BUFFER, EBO, UBO and a VAO attribute.
    assert(make_current(&g_egl, NULL, NULL, ctx_b));
    GLuint vao_b = 0; gen_vaos(1, &vao_b); bind_vao_fn(vao_b);
    bind_buffer(GL_ARRAY_BUFFER, buf_id);
    bind_buffer(GL_ELEMENT_ARRAY_BUFFER, buf_id);
    bind_buffer_range(GL_UNIFORM_BUFFER, 3, buf_id, 0, sizeof initial);
    attrib_ptr(0, 4, GL_FLOAT, 0, 16, NULL);
    assert(orig->refs == 7);   // +ctx_b ARRAY_BUFFER, EBO, UNIFORM_BUFFER, ubo[3], vao_b b[0]

    // Context A deletes buffer 1 and immediately reuses numeric name 1 for a new buffer.
    assert(make_current(&g_egl, NULL, NULL, ctx_a));
    delete_buffers(1, &buf_id);
    assert(!buf_of(buf_id) && orig->refs == 5 && weak_orig_mb != nil);
    GLint bound = -1; get_integer(0x8894, &bound); assert(bound == 0);
    GLuint reused = 0; gen_buffers(1, &reused); assert(reused == buf_id);
    const uint32_t fresh[4] = {901, 902, 903, 904};
    bind_buffer(GL_ARRAY_BUFFER, reused);
    @autoreleasepool { buffer_data(GL_ARRAY_BUFFER, sizeof fresh, fresh, 0); }
    assert(buf_of(reused) && buf_of(reused) != orig);

    // Context B still sees its original buffer (reads, sub-writes, glBufferData, VAO capture), unaffected by name reuse.
    assert(make_current(&g_egl, NULL, NULL, ctx_b));
    get_integer(0x8894, &bound); assert(bound == (GLint)buf_id);
    get_integer(0x8895, &bound); assert(bound == (GLint)buf_id);
    get_integer(0x8a28, &bound); assert(bound == (GLint)buf_id);
    uint32_t got[4] = {0};
    get_buffer_sub_data(GL_ARRAY_BUFFER, 0, sizeof got, got);
    assert(!memcmp(got, initial, sizeof got));
    const uint32_t patch[2] = {55, 66};
    buffer_sub_data(GL_ARRAY_BUFFER, 4, sizeof patch, patch);
    get_buffer_sub_data(GL_ARRAY_BUFFER, 0, sizeof got, got);
    assert(got[0] == 11 && got[1] == 55 && got[2] == 66 && got[3] == 44);
    const uint32_t replaced[4] = {101, 102, 103, 104};
    @autoreleasepool { buffer_data(GL_ARRAY_BUFFER, sizeof replaced, replaced, 0); }
    get_buffer_sub_data(GL_ARRAY_BUFFER, 0, sizeof got, got);
    assert(!memcmp(got, replaced, sizeof got) && get_err() == GL_NO_ERROR);
    __weak id<MTLBuffer> weak_after_data = S.array_buf.obj->mb;
    assert(weak_after_data != nil);
    // Capturing ARRAY_BUFFER into another attribute while name 1 is reused still retains orig.
    attrib_ptr(1, 4, GL_FLOAT, 0, 16, NULL);
    assert(cur_vao()->b[1].buf.obj == orig && S.ubo[3].buf.obj == orig && cur_vao()->ebo.obj == orig && orig->refs == 6);

    // Context A's new buffer with the reused name is completely untouched.
    assert(make_current(&g_egl, NULL, NULL, ctx_a));
    get_buffer_sub_data(GL_ARRAY_BUFFER, 0, sizeof got, got);
    assert(!memcmp(got, fresh, sizeof got));

    // Deleting A's reused name from B must not unbind B's older retained buffer sharing the same numeric name.
    assert(make_current(&g_egl, NULL, NULL, ctx_b));
    delete_buffers(1, &reused);
    assert(!buf_of(reused) && S.array_buf.obj == orig && orig->refs == 6);
    get_buffer_sub_data(GL_ARRAY_BUFFER, 0, sizeof got, got);
    assert(!memcmp(got, replaced, sizeof got));

    // Releasing bindings keeps orig alive until the very last binding (the VAO) is released.
    bind_buffer(GL_ARRAY_BUFFER, 0);
    bind_buffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    bind_buffer_base(GL_UNIFORM_BUFFER, 3, 0);
    assert(orig->refs == 2 && weak_after_data != nil);   // still retained by vao_b's two vertex bindings
    del_vaos(1, &vao_b);
    assert(weak_after_data == nil);   // last binding gone: original buffer object freed

    // Destroying a context that still holds the last binding to a deleted buffer releases it cleanly.
    assert(make_current(&g_egl, NULL, NULL, ctx_a));
    GLuint id2 = 0; gen_buffers(1, &id2);
    bind_buffer(GL_ARRAY_BUFFER, id2);
    @autoreleasepool { buffer_data(GL_ARRAY_BUFFER, sizeof initial, initial, 0); }
    __weak id<MTLBuffer> weak_id2 = buf_of(id2)->mb;
    assert(make_current(&g_egl, NULL, NULL, ctx_b));
    bind_buffer(GL_ARRAY_BUFFER, id2);
    assert(make_current(&g_egl, NULL, NULL, ctx_a));
    delete_buffers(1, &id2);
    assert(!buf_of(id2) && weak_id2 != nil);
    assert(h_eglDestroyContext(&g_egl, ctx_b));
    assert(weak_id2 == nil);

    assert(h_eglReleaseThread());
    assert(h_eglDestroyContext(&g_egl, ctx_a));
    puts("GL shared buffer lifetime: delete in A, read/write in B across name reuse, free on last unbind: OK");
}

static PFNNVNDEVICEGETPROCADDRESSPROC nvn_loader(void) {
    unsigned long bytes;
    const hle_force_ent *hooks = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &bytes);
    for (unsigned long i = 0; i < bytes / sizeof *hooks; i++)
        if (!strcmp(hooks[i].name, "nvnBootstrapLoader"))
            return (PFNNVNDEVICEGETPROCADDRESSPROC)((PFNNVNBOOTSTRAPLOADERPROC)hooks[i].fn)("nvnDeviceGetProcAddress");
    return NULL;
}

static void test_nvn_interop(void) {
    EGLContext ctx = h_eglCreateContext(&g_egl, NULL, NULL, NULL);
    assert(h_eglMakeCurrent(&g_egl, NULL, NULL, ctx));
    PFNNVNDEVICEGETPROCADDRESSPROC nvn = nvn_loader();
    assert(nvn);
#define NV_CALL(name) ((NVNT_##name)nvn(NULL, #name))
    GLuint name;
    h_glGenTextures(1, &name); h_glBindTexture(GL_TEXTURE_2D, name);
    h_glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 4, 2);
    NVNtextureBuilder builder = {0};
    NVNtexture texture = {0};
    NV_CALL(nvnTextureBuilderSetDefaults)(&builder);
    NV_CALL(nvnTextureBuilderSetGLTextureName)(&builder, name);
    assert(NV_CALL(nvnTextureBuilderGetGLTextureName)(&builder) == name);
    assert(NV_CALL(nvnTextureInitialize)(&texture, &builder));
    assert(NV_CALL(nvnTextureGetWidth)(&texture) == 4 && NV_CALL(nvnTextureGetHeight)(&texture) == 2);
    assert(NV_CALL(nvnTextureGetFormat)(&texture) == NVN_FORMAT_RGBA8);
    h_glDeleteTextures(1, &name);
    NV_CALL(nvnTextureFinalize)(&texture);

    NVNdevice device = {0};
    NVNdeviceBuilder db = {0};
    NV_CALL(nvnDeviceBuilderSetDefaults)(&db); assert(NV_CALL(nvnDeviceInitialize)(&device, &db));
    NVNqueueBuilder qb = {0};
    NVNqueue queue = {0};
    NV_CALL(nvnQueueBuilderSetDefaults)(&qb); NV_CALL(nvnQueueBuilderSetDevice)(&qb, &device);
    assert(NV_CALL(nvnQueueInitialize)(&queue, &qb));
    // Hold the initial GL work on the GPU to distinguish queue waits from
    // immediate success and to exercise a deleted GL name's imported fence.
    id<MTLSharedEvent> gate = [g_dev newSharedEvent];
    assert(gate);
    [cb_get() encodeWaitForEvent:gate value:1];
    void *first = h_glFenceSync(0x9117, 0);
    NVNsync imported = {0}, nv_done = {0};
    int status = -1;
    typedef NVNboolean (*import_sync)(NVNsync *, NVNdevice *, const void *, int *);
    typedef int (*export_sync)(const NVNsync *, void **);
    assert(((import_sync)nvn(NULL, "nvnSyncInitializeFromFencedGLSync"))(&imported, &device, first, &status) && !status);
    h_glDeleteSync(first);
    assert(NV_CALL(nvnSyncWait)(&imported, 0) == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
    assert(NV_CALL(nvnQueueWaitSync)(&queue, &imported));
    NV_CALL(nvnSyncFinalize)(&imported);
    assert(NV_CALL(nvnSyncInitialize)(&nv_done, &device));
    NV_CALL(nvnQueueFenceSync)(&queue, &nv_done, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    void *converted = NULL;
    assert(!((export_sync)nvn(NULL, "nvnSyncCreateGLSync"))(&nv_done, &converted) && converted);
    h_glWaitSync(converted, 0, UINT64_MAX);
    h_glDeleteSync(converted);
    void *last = h_glFenceSync(0x9117, 0);
    h_glFlush();
    GLint fence_status = 0;
    GLsizei count = 0;
    h_glGetSynciv(last, GL_SYNC_STATUS, 1, &count, &fence_status);
    assert(count == 1 && fence_status == GL_UNSIGNALED);
    assert(h_glClientWaitSync(last, 0, 0) == GL_TIMEOUT_EXPIRED);
    gate.signaledValue = 1;
    GLenum waited = h_glClientWaitSync(last, 0, 5000000000);
    assert(waited == GL_CONDITION_SATISFIED || waited == GL_ALREADY_SIGNALED);
    count = 0;
    h_glGetSynciv(last, GL_SYNC_STATUS, 1, &count, &fence_status);
    assert(count == 1 && fence_status == GL_SIGNALED);
    h_glDeleteSync(last);
    assert(NV_CALL(nvnSyncWait)(&nv_done, 5000000000) <= NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED);
    NV_CALL(nvnSyncFinalize)(&nv_done);
    NV_CALL(nvnQueueFinalize)(&queue);
    NV_CALL(nvnDeviceFinalize)(&device);
    assert(h_eglReleaseThread());
    assert(h_eglDestroyContext(&g_egl, ctx));
#undef NV_CALL
    puts("OpenGL–NVN interop: shared texture and round-trip GPU fence: OK");
}

// The other calls Fate/hollow ataraxia REMASTERED makes that the layer lacked (issue #2818): glValidateProgram after each link, glLineWidth, glClearTexImage on its new textures
static void test_program_validation(void) {
    static shsamp samplers[] = {{.name = "flat", .slot = 0, .type = T_SAMP2D, .unit = -1},
                                {.name = "cube", .slot = 1, .type = T_SAMPCUBE, .unit = -1}};
    static shinfo vsi = {.ub_slot = -1}, fsi = {.frag = 1, .ub_slot = -1, .nt = 2, .t = samplers};
    GLuint pr = h_glCreateProgram(), vs = h_glCreateShader(GL_VERTEX_SHADER), fs = h_glCreateShader(GL_FRAGMENT_SHADER), was = S.prog;
    assert(h_glIsProgram(pr) && !h_glIsProgram(vs) && !h_glIsProgram(0));
    shader_of(vs)->si = &vsi; shader_of(vs)->fn = (id)[NSObject new]; shader_of(fs)->si = &fsi; shader_of(fs)->fn = (id)[NSObject new];
    h_glAttachShader(pr, vs); h_glAttachShader(pr, fs); h_glLinkProgram(pr); h_glUseProgram(pr);
    GLint v; h_glGetProgramiv(pr, GL_VALIDATE_STATUS, &v); assert(v == 0);   // no validation yet
    h_glValidateProgram(pr); h_glGetProgramiv(pr, GL_VALIDATE_STATUS, &v); assert(v == 0);   // both types start at unit 0
    char log[160] = {0}; GLsizei len = 0; h_glGetProgramInfoLog(pr, sizeof log, &len, log); assert(strstr(log, "different types"));
    h_glGetProgramiv(pr, GL_INFO_LOG_LENGTH, &v); assert(v == len + 1);
    GLint cube = h_glGetUniformLocation(pr, "cube"); h_glUniform1i(cube, 1);
    h_glValidateProgram(pr); h_glGetProgramiv(pr, GL_VALIDATE_STATUS, &v); assert(v == 1);
    h_glUniform1i(cube, NUNITS); h_glGetProgramiv(pr, GL_VALIDATE_STATUS, &v); assert(v == 1);   // the previous validation's snapshot
    h_glValidateProgram(pr); h_glGetProgramiv(pr, GL_VALIDATE_STATUS, &v); assert(v == 0);
    h_glGetProgramInfoLog(pr, sizeof log, &len, log); assert(strstr(log, "outside"));
    h_glUniform1i(cube, -1); h_glValidateProgram(pr); h_glGetProgramiv(pr, GL_VALIDATE_STATUS, &v); assert(v == 0);
    h_glUniform1i(cube, 1); h_glValidateProgram(pr); h_glGetProgramiv(pr, GL_VALIDATE_STATUS, &v); assert(v == 1);
    h_glGetProgramInfoLog(pr, sizeof log, &len, log); assert(len == 0 && !log[0]);
    h_glLinkProgram(pr); h_glGetProgramiv(pr, GL_VALIDATE_STATUS, &v); assert(v == 0);   // relinking invalidates validation
    h_glUseProgram(was); h_glDeleteProgram(pr); h_glDeleteShader(vs); h_glDeleteShader(fs); assert(h_glGetError() == GL_NO_ERROR);

    // Explicit layout(binding = N) on samplers initializes u->unit at link time; unqualified samplers stay at unit 0.
    char row_ok[] = "0123456789abcdef\tfrag\tr:0:4 t:u_y=0:1:sampler2D t:u_uv=2:3:sampler2D:1\tmain#0";
    shinfo parsed = {0};
    assert(sh_read_row(row_ok, &parsed) && parsed.nt == 2 && parsed.t[0].unit == -1 && parsed.t[1].unit == 1);
    sh_free(&parsed);
    char row_bad[] = "0123456789abcdef\tfrag\tr:0:2 t:u_uv=0:1:sampler2D:999\tmain#0";
    assert(!sh_read_row(row_bad, &parsed));
    char row_junk[] = "0123456789abcdef\tfrag\tr:0:2 t:u_uv=0:1:sampler2D:bad\tmain#0";
    assert(sh_read_row(row_junk, &parsed) && parsed.nt == 0);
    sh_free(&parsed);

    // GL_TEXTURE_SWIZZLE_RGBA sets all four channels via glTexParameteriv and glTexParameterfv.
    GLuint swz_tex = 0;
    h_glGenTextures(1, &swz_tex);
    h_glBindTexture(GL_TEXTURE_2D, swz_tex);
    const GLint swz_i[4] = {GL_BLUE, GL_GREEN, GL_RED, GL_ONE};
    h_glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swz_i);
    tex_t *st = tex_of(swz_tex);
    assert(st && st->swz[0] == GL_BLUE && st->swz[1] == GL_GREEN && st->swz[2] == GL_RED && st->swz[3] == GL_ONE);
    const GLfloat swz_f[4] = {GL_ALPHA, GL_ZERO, GL_RED, GL_GREEN};
    h_glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swz_f);
    assert(st->swz[0] == GL_ALPHA && st->swz[1] == GL_ZERO && st->swz[2] == GL_RED && st->swz[3] == GL_GREEN);
    h_glDeleteTextures(1, &swz_tex);
    assert(h_glGetError() == GL_NO_ERROR);
}

static void test_fixed_function_caps(void) {
    static const GLenum unimplemented[] = {
        0x4000, 0x4001, 0x4002, 0x4003, 0x4004, 0x4005, 0x4006, 0x4007,  // LIGHT0..7
        0x0c60, 0x0c61, 0x0c62, 0x0c63,  // TEXTURE_GEN_S / T / R / Q
        0x0b57 /* COLOR_MATERIAL */, 0x0ba1 /* NORMALIZE */,
    };
    const uint8_t blend = S.blend, atest = F.atest;
    const uint8_t depth = S.depth, cull = S.cull, scissor = S.scissor, stencil = S.stencil;
    const uint8_t poffset = S.poffset, poffset_line = S.poffset_line;
    h_glEnable(GL_BLEND);
    h_glEnable(GL_ALPHA_TEST);
    assert(S.blend && F.atest);

    // Disabling an unsupported cap is silent and leaves its first enable free to warn.
    FILE *log = tmpfile();
    int saved_stderr = dup(STDERR_FILENO);
    assert(log && saved_stderr >= 0);
    assert(fflush(stderr) == 0);
    assert(dup2(fileno(log), STDERR_FILENO) == STDERR_FILENO);
    for (size_t i = 0; i < sizeof unimplemented / sizeof *unimplemented; i++) h_glDisable(unimplemented[i]);
    assert(fflush(stderr) == 0);
    off_t disable_bytes = lseek(fileno(log), 0, SEEK_CUR);
    for (size_t i = 0; i < sizeof unimplemented / sizeof *unimplemented; i++) h_glEnable(unimplemented[i]);
    assert(fflush(stderr) == 0);
    assert(dup2(saved_stderr, STDERR_FILENO) == STDERR_FILENO);
    close(saved_stderr);

    assert(disable_bytes == 0 && "disabling unsupported fixed-function caps should be silent");
    assert(fseek(log, 0, SEEK_SET) == 0);
    for (size_t i = 0; i < sizeof unimplemented / sizeof *unimplemented; i++) {
        char line[80], expected[80];
        snprintf(expected, sizeof expected, "[dm] [gl] [RED] glEnable(%#x) not implemented\n", unimplemented[i]);
        assert(fgets(line, sizeof line, log) && "enabling an unsupported fixed-function cap should warn");
        assert(!strcmp(line, expected));
    }
    assert(fgetc(log) == EOF && !ferror(log));
    fclose(log);

    assert(S.blend && F.atest && S.depth == depth && S.cull == cull && S.scissor == scissor && S.stencil == stencil);
    assert(S.poffset == poffset && S.poffset_line == poffset_line);
    assert(h_glGetError() == GL_NO_ERROR);
    h_glDisable(GL_BLEND);
    h_glDisable(GL_ALPHA_TEST);
    assert(!S.blend && !F.atest);
    S.blend = blend;
    F.atest = atest;
}

static void test_active_attributes(void) {
    void (*active)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *) = import("glGetActiveAttrib");
    assert(active && h_eglGetProcAddress("glGetActiveAttrib") == (void *)active);
    assert(h_eglGetProcAddress("glGetActiveAttribARB") == (void *)active);
    static shattr attrs[] = {{.name = "position", .slot = 0, .type = T_VEC3}, {.name = "uv", .slot = 2, .type = T_VEC2}};
    static shinfo vsi = {.ub_slot = -1, .na = 2, .a = attrs};
    GLuint pr = h_glCreateProgram(), vs = h_glCreateShader(GL_VERTEX_SHADER);
    GLint value;
    h_glGetProgramiv(pr, GL_ACTIVE_ATTRIBUTE_MAX_LENGTH, &value); assert(value == 0);
    shader_of(vs)->si = &vsi; shader_of(vs)->fn = (id)[NSObject new];
    h_glAttachShader(pr, vs); h_glBindAttribLocation(pr, 7, "position"); h_glLinkProgram(pr);
    h_glGetProgramiv(pr, GL_ACTIVE_ATTRIBUTES, &value); assert(value == 2);
    h_glGetProgramiv(pr, GL_ACTIVE_ATTRIBUTE_MAX_LENGTH, &value); assert(value == sizeof "position");
    assert(h_glGetAttribLocation(pr, "position") == 7);

    GLsizei length; GLint count; GLenum type; char name[16];
    active(pr, 0, sizeof name, &length, &count, &type, name);
    assert(length == 8 && count == 1 && type == GL_FLOAT_VEC3 && !strcmp(name, "position"));
    active(pr, 1, 2, &length, &count, &type, name);
    assert(length == 1 && count == 1 && type == GL_FLOAT_VEC2 && !strcmp(name, "u"));
    active(pr, 0, 0, &length, NULL, NULL, name);
    assert(length == 0 && !strcmp(name, "u") && h_glGetError() == GL_NO_ERROR);

    length = 23; count = 42; type = GL_INT;
    active(pr, 2, sizeof name, &length, &count, &type, name);
    assert(h_glGetError() == GL_INVALID_VALUE);
    active(pr, 0, -1, &length, &count, &type, name);
    assert(h_glGetError() == GL_INVALID_VALUE);
    active(vs, 0, sizeof name, &length, &count, &type, name);
    assert(h_glGetError() == GL_INVALID_OPERATION);
    assert(length == 23 && count == 42 && type == GL_INT && !strcmp(name, "u"));

    h_glDetachShader(pr, vs); h_glLinkProgram(pr);
    h_glGetProgramiv(pr, GL_ACTIVE_ATTRIBUTES, &value); assert(value == 0);
    h_glGetProgramiv(pr, GL_ACTIVE_ATTRIBUTE_MAX_LENGTH, &value); assert(value == 0);
    h_glDeleteProgram(pr); h_glDeleteShader(vs);
    puts("GL attribute reflection: count, maximum length, enumeration, truncation and errors: OK");
}

static void test_uniform_block_reflection(void) {
    void (*uniformsiv)(GLuint, GLsizei, const GLuint *, GLenum, GLint *) = import("glGetActiveUniformsiv");
    assert(uniformsiv && h_eglGetProcAddress("glGetActiveUniformsiv") == (void *)uniformsiv);

    // Reject malformed member rows (out-of-range block slot or invalid row_major flag)
    char bad_slot[] = "aaaaaaaaaaaaaaaa\tvert\tr:0:1 n:B=0:16 m:x=1:0:vec4:0:0:0:0\ttest";
    char bad_rm[] = "bbbbbbbbbbbbbbbb\tvert\tr:0:1 n:B=0:16 m:x=0:0:mat4:0:0:16:2\ttest";
    shinfo bad = {0};
    assert(!sh_read_row(bad_slot, &bad));
    assert(!sh_read_row(bad_rm, &bad));

    // Vertex + fragment tables: shared block across stages (deduplicated), two distinct uninstanced blocks with a colliding
    // bare member name (`common`), two instanced blocks (`InstA.m` / `InstB.m`), reflected matrix strides (8 and 16) and row_major
    char vrow[] = "1111111111111111\tvert\tr:0:4 b:0:64 u:scale=0:float:0:0 u:weights=16:vec4:3:16 "
                  "n:SharedCB=1:128 m:mtxPVW=1:0:mat4:0:0:16:0 m:rm3=1:64:mat3:0:0:16:1 "
                  "n:VertCB=2:64 m:common=2:0:mat2:0:0:8:0 "
                  "n:InstA=3:64 m:InstA.m=3:0:mat3x4:0:0:16:1\ttest";
    char frow[] = "2222222222222222\tfrag\tr:0:3 "
                  "n:SharedCB=0:128 m:mtxPVW=0:0:mat4:0:0:16:0 "
                  "n:FragCB=1:64 m:common=1:16:vec4:2:16:0:0 "
                  "n:InstB=2:64 m:InstB.m=2:0:mat4:0:0:16:0\ttest";
    shinfo vsi = {0}, fsi = {0};
    assert(sh_read_row(vrow, &vsi) && sh_read_row(frow, &fsi));
    assert(vsi.nb == 3 && vsi.nm == 4 && fsi.nb == 3 && fsi.nm == 3);
    vsi.nargs = fsi.nargs = 0;   // synthetic link without Metal argument encoder

    GLuint pr = h_glCreateProgram(), vs = h_glCreateShader(GL_VERTEX_SHADER), fs = h_glCreateShader(GL_FRAGMENT_SHADER);
    shader_of(vs)->si = &vsi; shader_of(vs)->fn = (id)[NSObject new];
    shader_of(fs)->si = &fsi; shader_of(fs)->fn = (id)[NSObject new];
    h_glAttachShader(pr, vs); h_glAttachShader(pr, fs); h_glLinkProgram(pr);

    GLint val = -1;
    h_glGetProgramiv(pr, 0x8a36 /* GL_ACTIVE_UNIFORM_BLOCKS */, &val); assert(val == 5);
    h_glGetProgramiv(pr, 0x8a35 /* GL_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH */, &val); assert(val == (GLint)sizeof "SharedCB");
    h_glGetProgramiv(pr, GL_ACTIVE_UNIFORMS, &val); assert(val == 8);   // 2 default + 6 block members
    h_glGetProgramiv(pr, GL_ACTIVE_UNIFORM_MAX_LENGTH, &val); assert(val == (GLint)sizeof "weights[0]");

    // Default uniforms have locations; named block members never do
    assert(h_glGetUniformLocation(pr, "scale") > 0 && h_glGetUniformLocation(pr, "weights") > 0);
    assert(h_glGetUniformLocation(pr, "mtxPVW") == -1 && h_glGetUniformLocation(pr, "common") == -1 && h_glGetUniformLocation(pr, "InstA.m") == -1);

    // Enumerate every active uniform and check glGetActiveUniform + glGetActiveUniformsiv
    static const struct {
        const char *name; GLenum type; GLint size, blk, off, astride, mstride, rm;
    } want[8] = {
        {"scale",      GL_FLOAT,        1, -1, -1, -1, -1, 0},
        {"weights[0]", GL_FLOAT_VEC4,   3, -1, -1, -1, -1, 0},
        {"mtxPVW",     GL_FLOAT_MAT4,   1,  0,  0,  0, 16, 0},
        {"rm3",        GL_FLOAT_MAT3,   1,  0, 64,  0, 16, 1},
        {"common",     GL_FLOAT_MAT2,   1,  1,  0,  0,  8, 0},
        {"InstA.m",    GL_FLOAT_MAT3x4, 1,  2,  0,  0, 16, 1},
        {"common[0]",  GL_FLOAT_VEC4,   2,  3, 16, 16,  0, 0},
        {"InstB.m",    GL_FLOAT_MAT4,   1,  4,  0,  0, 16, 0},
    };
    GLuint idxs[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    GLint types[8], sizes[8], nlens[8], blks[8], offs[8], astrides[8], mstrides[8], rms[8];
    uniformsiv(pr, 8, idxs, 0x8a37, types);
    uniformsiv(pr, 8, idxs, 0x8a38, sizes);
    uniformsiv(pr, 8, idxs, 0x8a39, nlens);
    uniformsiv(pr, 8, idxs, 0x8a3a, blks);
    uniformsiv(pr, 8, idxs, 0x8a3b, offs);
    uniformsiv(pr, 8, idxs, 0x8a3c, astrides);
    uniformsiv(pr, 8, idxs, 0x8a3d, mstrides);
    uniformsiv(pr, 8, idxs, 0x8a3e, rms);
    assert(h_glGetError() == GL_NO_ERROR);
    for (int i = 0; i < 8; i++) {
        char buf[64] = {0}; GLsizei len = -1; GLint cnt = -1; GLenum ty = 0;
        h_glGetActiveUniform(pr, (GLuint)i, sizeof buf, &len, &cnt, &ty, buf);
        assert(!strcmp(buf, want[i].name) && len == (GLsizei)strlen(want[i].name) && cnt == want[i].size && ty == want[i].type);
        assert(types[i] == (GLint)want[i].type && sizes[i] == want[i].size && nlens[i] == (GLint)strlen(want[i].name) + 1);
        assert(blks[i] == want[i].blk && offs[i] == want[i].off && astrides[i] == want[i].astride && mstrides[i] == want[i].mstride && rms[i] == want[i].rm);
    }

    // glGetActiveUniformBlockiv: name length, active uniforms/indices, stage references
    GLint bval = -1, bindices[4] = {-1, -1, -1, -1};
    h_glGetActiveUniformBlockiv(pr, 0, 0x8a41, &bval); assert(bval == (GLint)sizeof "SharedCB");
    h_glGetActiveUniformBlockiv(pr, 0, 0x8a42, &bval); assert(bval == 2);
    h_glGetActiveUniformBlockiv(pr, 0, 0x8a43, bindices); assert(bindices[0] == 2 && bindices[1] == 3);
    h_glGetActiveUniformBlockiv(pr, 0, 0x8a44, &bval); assert(bval == 1);
    h_glGetActiveUniformBlockiv(pr, 0, 0x8a46, &bval); assert(bval == 1);
    h_glGetActiveUniformBlockiv(pr, 1, 0x8a44, &bval); assert(bval == 1);
    h_glGetActiveUniformBlockiv(pr, 1, 0x8a46, &bval); assert(bval == 0);
    h_glGetActiveUniformBlockiv(pr, 3, 0x8a42, &bval); assert(bval == 1);
    h_glGetActiveUniformBlockiv(pr, 3, 0x8a43, bindices); assert(bindices[0] == 6);
    h_glGetActiveUniformBlockiv(pr, 3, 0x8a44, &bval); assert(bval == 0);
    h_glGetActiveUniformBlockiv(pr, 3, 0x8a46, &bval); assert(bval == 1);
    assert(h_glGetError() == GL_NO_ERROR);

    // Error handling: negative count, out-of-range index leaves output untouched, wrong object, invalid pname even with n == 0
    GLint out[2] = {77, 88};
    const GLuint bad_idx[2] = {0, 8};
    uniformsiv(pr, -1, idxs, 0x8a3a, out); assert(h_glGetError() == GL_INVALID_VALUE);
    uniformsiv(pr, 2, bad_idx, 0x8a3a, out); assert(h_glGetError() == GL_INVALID_VALUE && out[0] == 77 && out[1] == 88);
    uniformsiv(vs, 1, idxs, 0x8a3a, out); assert(h_glGetError() == GL_INVALID_OPERATION);
    uniformsiv(pr, 0, idxs, 0x1234, out); assert(h_glGetError() == GL_INVALID_ENUM);

    // Failed relink clears blocks and members
    h_glDetachShader(pr, vs); h_glLinkProgram(pr);
    h_glGetProgramiv(pr, 0x8a36, &val); assert(val == 0);
    h_glGetProgramiv(pr, GL_ACTIVE_UNIFORMS, &val); assert(val == 0);

    // More than 8 uniform blocks link cleanly
    char many[] = "3333333333333333\tvert\tr:0:10 n:B0=0:16 n:B1=1:16 n:B2=2:16 n:B3=3:16 n:B4=4:16 n:B5=5:16 n:B6=6:16 n:B7=7:16 n:B8=8:16 n:B9=9:16 "
                  "m:m9=9:0:vec4:0:0:0:0\ttest";
    shinfo msi = {0}; assert(sh_read_row(many, &msi)); msi.nargs = 0;
    shader_of(vs)->si = &msi;
    h_glDetachShader(pr, fs); h_glAttachShader(pr, vs); h_glLinkProgram(pr);
    h_glGetProgramiv(pr, 0x8a36, &val); assert(val == 10);
    GLuint idx0 = 0; GLint blk9 = -1;
    uniformsiv(pr, 1, &idx0, 0x8a3a, &blk9); assert(blk9 == 9 && h_glGetError() == GL_NO_ERROR);

    h_glDeleteProgram(pr); h_glDeleteShader(vs); h_glDeleteShader(fs);
    sh_free(&vsi); sh_free(&fsi); sh_free(&msi);
    puts("GL uniform block reflection: layout, collisions, glGetActiveUniformsiv and errors: OK");
}

static void test_fate_calls(void) {
    assert(import("glValidateProgram") == (void *)h_glValidateProgram_ctx && import("glLineWidth") == (void *)h_glLineWidth_ctx);
    assert(import("glClearTexImage") == (void *)h_glClearTexImage_bs);
    GLuint pr = h_glCreateProgram(); h_glValidateProgram(pr); GLint v = 0; h_glGetProgramiv(pr, GL_VALIDATE_STATUS, &v); assert(v == 0 && h_glGetError() == GL_NO_ERROR);
    char log[160]; GLsizei len; h_glGetProgramInfoLog(pr, sizeof log, &len, log); assert(len > 0 && strstr(log, "linked"));
    h_glDeleteProgram(pr); h_glLineWidth(1);
    // a 4x2 RGBA8 texture of two levels: level 0 cleared to one RGBA value, level 1 to one RGB value (alpha 1), then to zeros (no data)
    GLuint t; h_glGenTextures(1, &t); h_glBindTexture(GL_TEXTURE_2D, t); h_glTexStorage2D(GL_TEXTURE_2D, 2, GL_RGBA8, 4, 2);
    const uint8_t rgba[4] = {1, 2, 3, 4}, rgb[3] = {9, 8, 7};
    h_glClearTexImage(t, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba); h_glClearTexImage(t, 1, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    tex_t *x = tex_of(t); assert(x && x->made && x->levels == 2 && h_glGetError() == GL_NO_ERROR);
    uint32_t px[8]; [x->tex getBytes:px bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 2) mipmapLevel:0];
    for (int i = 0; i < 8; i++) assert(px[i] == 0x04030201u);
    [x->tex getBytes:px bytesPerRow:8 fromRegion:MTLRegionMake2D(0, 0, 2, 1) mipmapLevel:1]; assert(px[0] == 0xff070809u && px[1] == 0xff070809u);
    h_glClearTexImage(t, 1, GL_RGBA, GL_UNSIGNED_BYTE, NULL); [x->tex getBytes:px bytesPerRow:8 fromRegion:MTLRegionMake2D(0, 0, 2, 1) mipmapLevel:1]; assert(!px[0] && !px[1]);
    h_glClearTexImage(t, 2, GL_RGBA, GL_UNSIGNED_BYTE, rgba); assert(h_glGetError() == GL_INVALID_OPERATION);   // a level the texture does not have
    h_glClearTexImage(t + 100, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba); assert(h_glGetError() == GL_INVALID_OPERATION);   // no such texture
    const float red[4] = {1, 0, 0, 1};
    h_glPixelStorei(0xcf0, 1); h_glPixelStorei(GL_UNPACK_ROW_LENGTH, 99);   // clear values have no unpack state
    h_glClearTexImage(t, 0, GL_RGBA, GL_FLOAT, red);
    assert(S.unpack_swap == 1 && S.unpack_row == 99); h_glPixelStorei(0xcf0, 0); h_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    [x->tex getBytes:px bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 2) mipmapLevel:0];
    for (int i = 0; i < 8; i++) assert(px[i] == 0xff0000ffu);
    const uint8_t bgra[4] = {255, 128, 0, 255}; h_glClearTexImage(t, 0, GL_BGRA, GL_UNSIGNED_BYTE, bgra);
    [x->tex getBytes:px bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 2) mipmapLevel:0];
    for (int i = 0; i < 8; i++) assert(px[i] == 0xffff8000u);
    uint16_t half_red = 32768; h_glClearTexImage(t, 0, GL_RED, GL_UNSIGNED_SHORT, &half_red);
    [x->tex getBytes:px bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 2) mipmapLevel:0];
    for (int i = 0; i < 8; i++) assert(px[i] == 0xff000080u);
    h_glClearTexImage(t, 0, GL_DEPTH_COMPONENT, GL_FLOAT, red); assert(h_glGetError() == GL_INVALID_OPERATION);
    GLuint fb; h_glGenFramebuffers(1, &fb); h_glBindFramebuffer(GL_FRAMEBUFFER, fb);
    h_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    h_glClearColor(1, 0, 0, 1); h_glClear(GL_COLOR_BUFFER_BIT);
    const uint8_t blue[4] = {0, 0, 255, 255}; h_glClearTexImage(t, 0, GL_RGBA, GL_UNSIGNED_BYTE, blue); h_glFinish();
    [x->tex getBytes:px bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 2) mipmapLevel:0];
    for (int i = 0; i < 8; i++) assert(px[i] == 0xffff0000u);   // the later texture clear wins
    h_glBindFramebuffer(GL_FRAMEBUFFER, 0); h_glDeleteFramebuffers(1, &fb);
    h_glDeleteTextures(1, &t);
    h_glGenTextures(1, &t); h_glBindTexture(GL_TEXTURE_2D, t); h_glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, 1, 1);
    h_glClearTexImage(t, 0, GL_BGRA, GL_UNSIGNED_BYTE, bgra);
    _Float16 fp[4]; [tex_of(t)->tex getBytes:fp bytesPerRow:8 fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
    assert(fp[0] == 0 && fabs((double)fp[1] - 128.0 / 255) < 0.001 && fp[2] == 1 && fp[3] == 1);
    h_glDeleteTextures(1, &t);
}

// Generated GLSL, reflected table and metallib through the real compile/link/draw path. Seventeen distinct sampler states, vertex attribute 15, a texture shared by both
// stages and a named uniform block: three draws in one command buffer must retain their own uniforms, sampler units and block ranges.
static void test_argument_buffers(const char *dir) {
    __typeof__(S) was = S;
    GLuint pr = h_glCreateProgram(), shaders[2];
    for (int st = 0; st < 2; st++) {
        char path[1024]; snprintf(path, sizeof path, "%s/resource.%s", dir, st ? "frag" : "vert");
        FILE *f = fopen(path, "rb"); assert(f); assert(fseek(f, 0, SEEK_END) == 0);
        long len = ftell(f); assert(len >= 0); assert(fseek(f, 0, SEEK_SET) == 0);
        char *source = calloc((size_t)len + 1, 1); assert(source); assert(fread(source, 1, (size_t)len, f) == (size_t)len); fclose(f);
        shaders[st] = h_glCreateShader(st ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
        const char *s = source; h_glShaderSource(shaders[st], 1, &s, NULL); free(source); h_glCompileShader(shaders[st]);
        assert(shader_of(shaders[st])->fn); h_glAttachShader(pr, shaders[st]);
    }
    h_glLinkProgram(pr); assert(prog_of(pr)->linked && prog_of(pr)->ae[0] && prog_of(pr)->ae[1]); h_glUseProgram(pr);
    // Named block member reflection end-to-end through gl_shaders.py -> gl_shaders.tsv -> host
    GLint nblk = 0, nuni = 0, fidx = -1;
    h_glGetProgramiv(pr, 0x8a36, &nblk); assert(nblk == 1);
    h_glGetProgramiv(pr, GL_ACTIVE_UNIFORMS, &nuni); assert(nuni >= 1);
    for (GLint i = 0; i < nuni; i++) {
        char nm[64]; GLsizei l = 0; GLint c = 0; GLenum t = 0;
        h_glGetActiveUniform(pr, (GLuint)i, sizeof nm, &l, &c, &t, nm);
        if (!strcmp(nm, "factors")) { assert(c == 1 && t == GL_FLOAT_VEC4); fidx = i; }
    }
    assert(fidx >= 0 && h_glGetUniformLocation(pr, "factors") == -1);
    GLuint fu = (GLuint)fidx; GLint fblk = -1, foff = -1, fastr = -1, fmstr = -1, frow = -1;
    h_glGetActiveUniformsiv(pr, 1, &fu, 0x8a3a, &fblk); h_glGetActiveUniformsiv(pr, 1, &fu, 0x8a3b, &foff);
    h_glGetActiveUniformsiv(pr, 1, &fu, 0x8a3c, &fastr); h_glGetActiveUniformsiv(pr, 1, &fu, 0x8a3d, &fmstr);
    h_glGetActiveUniformsiv(pr, 1, &fu, 0x8a3e, &frow);
    assert(fblk == 0 && foff == 0 && fastr == 0 && fmstr == 0 && frow == 0);
    GLuint textures[17], samplers[17], vao, vb, ub, target, fb;
    h_glGenTextures(17, textures); h_glGenSamplers(17, samplers);
    for (int i = 0; i < 17; i++) {
        uint8_t pixels[8] = {(uint8_t)(32 + i * 4), i ? 0 : 64, 0, 255, (uint8_t)(128 + i * 4), i ? 0 : 64, 0, 255};
        h_glActiveTexture(0x84c0 + i); h_glBindTexture(GL_TEXTURE_2D, textures[i]);
        h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        h_glSamplerParameteri(samplers[i], GL_TEXTURE_MIN_FILTER, GL_NEAREST); h_glSamplerParameteri(samplers[i], GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        h_glSamplerParameteri(samplers[i], GL_TEXTURE_WRAP_S, i & 1 ? GL_CLAMP_TO_EDGE : GL_REPEAT);
        h_glSamplerParameterf(samplers[i], GL_TEXTURE_LOD_BIAS, (float)i / 16);   // distinct even when filter and wrap match
        h_glBindSampler(i, samplers[i]);
        char name[32]; snprintf(name, sizeof name, "tex%d", i); GLint loc = h_glGetUniformLocation(pr, name); assert(loc >= 0); h_glUniform1i(loc, i);
    }
    h_glGenVertexArrays(1, &vao); h_glBindVertexArray(vao); h_glGenBuffers(1, &vb); h_glBindBuffer(GL_ARRAY_BUFFER, vb);
    const float vertices[] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    h_glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, 0); h_glVertexAttribPointer(15, 4, GL_FLOAT, 0, 16, NULL); h_glEnableVertexAttribArray(15);
    float blocks[128] = {0}; blocks[0] = blocks[64] = -0.25f; blocks[1] = 1; blocks[65] = 0.5f; blocks[2] = blocks[66] = 0.125f;
    h_glGenBuffers(1, &ub); h_glBindBuffer(GL_UNIFORM_BUFFER, ub); h_glBufferData(GL_UNIFORM_BUFFER, sizeof blocks, blocks, 0);
    h_glUniformBlockBinding(pr, h_glGetUniformBlockIndex(pr, "Block"), 5); h_glBindBufferRange(GL_UNIFORM_BUFFER, 5, ub, 0, 16);
    h_glUniform4f(h_glGetUniformLocation(pr, "shift"), 0.25f, 0.25f, 0, 0);
    GLint weight = h_glGetUniformLocation(pr, "weight"), last = h_glGetUniformLocation(pr, "tex16"); assert(weight >= 0 && last >= 0);
    h_glActiveTexture(0x84c0 + 31); h_glGenTextures(1, &target); h_glBindTexture(GL_TEXTURE_2D, target);
    h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 51, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    h_glGenFramebuffers(1, &fb); h_glBindFramebuffer(GL_FRAMEBUFFER, fb); h_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    h_glDisable(GL_BLEND); h_glDisable(GL_DEPTH_TEST); h_glDisable(GL_STENCIL_TEST); h_glDisable(GL_CULL_FACE); h_glDisable(GL_SCISSOR_TEST); h_glColorMask(1, 1, 1, 1);
    for (int draw = 0; draw < 3; draw++) {
        h_glViewport(17 * draw, 0, 17, 1); h_glUniform4f(weight, draw ? 0.5f : 1, 1, 1, 1);
        if (draw == 1) h_glUniform1i(last, 0);
        if (draw == 2) h_glBindBufferRange(GL_UNIFORM_BUFFER, 5, ub, 256, 16);
        h_glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    uint8_t got[51 * 4]; h_glReadPixels(0, 0, 51, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);
    for (int draw = 0; draw < 3; draw++) for (int i = 0; i < 17; i++) {
        int unit = draw && i == 16 ? 0 : i, expected = (unit & 1 ? 128 : 32) + unit * 4;
        expected /= draw == 2 ? 4 : draw == 1 ? 2 : 1;
        assert(abs((int)got[4 * (17 * draw + i)] - expected) <= 1);
        assert(abs((int)got[4 * (17 * draw + i) + 1] - (draw == 2 ? 48 : 96)) <= 1);   // vertex default and named uniforms both contribute
    }
    assert(h_glGetError() == GL_NO_ERROR);
    h_glUseProgram(0); h_glDeleteProgram(pr); h_glDeleteShader(shaders[0]); h_glDeleteShader(shaders[1]);
    h_glDeleteFramebuffers(1, &fb); h_glDeleteTextures(1, &target); h_glDeleteTextures(17, textures); h_glDeleteSamplers(17, samplers);
    h_glDeleteBuffers(1, &vb); h_glDeleteBuffers(1, &ub); h_glDeleteVertexArrays(1, &vao); S = was;
    puts("GL argument buffers: 17 samplers, both stages, uniforms and block ranges: OK");
}

static void test_rg_texture_uploads(void) {
    GLuint name;
    h_glGenTextures(1, &name);
    h_glBindTexture(GL_TEXTURE_2D, name);
    h_glPixelStorei(GL_UNPACK_ROW_LENGTH, 3);
    h_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    // GL_RGB storage, as in Layton. Each row has two pixels, a skipped third pixel and two alignment bytes.
    const uint8_t rg[] = {10, 20, 30, 40, 91, 92, 93, 94, 50, 60, 70, 80};
    const uint8_t rgba[] = {10, 20, 0, 255, 30, 40, 0, 255, 50, 60, 0, 255, 70, 80, 0, 255};
    h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 2, 0, GL_RG, GL_UNSIGNED_BYTE, rg);
    const level_t *level = &tex_of(name)->pend[0][0];
    assert(level->pitch == 8 && !memcmp(level->data, rgba, sizeof rgba));

    // Replace one column with big-endian 16-bit red. Input row pitch is eight bytes, and alpha stays opaque.
    const uint8_t red[] = {0x40, 0, 9, 10, 11, 12, 13, 14, 0x80, 0};
    const uint8_t patched[] = {10, 20, 0, 255, 64, 0, 0, 255, 50, 60, 0, 255, 128, 0, 0, 255};
    h_glPixelStorei(0xcf0, 1);
    h_glTexSubImage2D(GL_TEXTURE_2D, 0, 1, 0, 1, 2, GL_RED, GL_UNSIGNED_SHORT, red);
    assert(!memcmp(level->data, patched, sizeof patched));
    h_glPixelStorei(0xcf0, 0);
    h_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    h_glDeleteTextures(1, &name);

    const float float_rg[] = {0.25f, 0.75f};
    const uint8_t rgba8[] = {64, 191, 0, 255};
    const float rgba32f[] = {0.25f, 0.75f, 0, 1};
    h_glGenTextures(1, &name);
    h_glBindTexture(GL_TEXTURE_2D, name);
    h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RG, GL_FLOAT, float_rg);
    level = &tex_of(name)->pend[0][0];
    assert(level->pitch == 4 && !memcmp(level->data, rgba8, sizeof rgba8));
    h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RG, GL_FLOAT, float_rg);
    float uploaded[4];
    assert(level->pitch == sizeof uploaded);
    memcpy(uploaded, level->data, sizeof uploaded);
    for (int i = 0; i < 4; i++) assert(uploaded[i] == rgba32f[i]);
    h_glDeleteTextures(1, &name);
    puts("GL texture uploads: RED/RG expansion, typed components, row length and byte swapping: OK");
}

static void test_fixed_function(int gpu) {
    const EGLint core[] = {0x3098, 3, 0x30fb, 2, 0x3038};
    const EGLint compat[] = {0x3098, 3, 0x30fb, 2, 0x30fd, 2, 0x3038};
    const EGLint gl31[] = {0x3098, 3, 0x30fb, 1, 0x3038};
    const EGLint forward[] = {0x3098, 3, 0x30fc, 2, 0x3038};
    const EGLint *profiles[] = {core, gl31, forward, compat};
    for (unsigned i = 0; i < sizeof profiles / sizeof *profiles; i++) {
        EGLContext ctx = h_eglCreateContext(NULL, NULL, NULL, profiles[i]); assert(ctx);
        assert(h_eglMakeCurrent(NULL, NULL, NULL, ctx));
        GLint profile; h_glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
        assert(g_ctx_fixed == (i == 3) && profile == (i == 3 ? 2 : 1));
        assert(h_eglDestroyContext(NULL, ctx));
        assert(h_eglReleaseThread());
    }
    EGLContext ctx = h_eglCreateContext(NULL, NULL, NULL, NULL); assert(ctx);
    assert(h_eglMakeCurrent(NULL, NULL, NULL, ctx) && g_ctx_fixed);

    __typeof__(S) saved = S;
    h_glActiveTexture(GL_TEXTURE0);
    const GLfloat color[] = {-1, 0.25f, 0.5f, 2};
    h_glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color);
    assert(F.envc[0][0] == 0 && F.envc[0][1] == 0.25f && F.envc[0][2] == 0.5f && F.envc[0][3] == 1);
    h_glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, 0.5f); assert(h_glGetError() == GL_INVALID_ENUM);
    h_glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, 1); assert(h_glGetError() == GL_INVALID_ENUM);
    assert(F.envc[0][1] == 0.25f && F.envc[0][3] == 1);
    h_glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, NAN); assert(h_glGetError() == GL_INVALID_ENUM && F.env[0] == GL_MODULATE);
    h_glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE); assert(F.env[0] == GL_REPLACE);
    h_glMatrixMode(GL_TEXTURE);
    h_glActiveTexture(GL_TEXTURE0 + 1);
    h_glTranslatef(0.5f, 0, 0);
    assert(F.texture[0].m[0][12] == 0 && F.texture[1].m[0][12] == 0.5f);
    h_glMatrixMode(GL_MODELVIEW);

    if (gpu) {
        GLuint tex[2], fb, vao;
        h_glGenTextures(2, tex);
        h_glBindTexture(GL_TEXTURE_2D, tex[0]);
        h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        h_glGenFramebuffers(1, &fb); h_glBindFramebuffer(GL_FRAMEBUFFER, fb);
        h_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[0], 0);
        h_glActiveTexture(GL_TEXTURE0); h_glBindTexture(GL_TEXTURE_2D, tex[1]);
        const uint8_t green[8] = {0, 255, 0, 255, 0, 255, 0, 255};
        h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, green);
        h_glEnable(GL_TEXTURE_2D);

        h_glUseProgram(0); h_glGenVertexArrays(1, &vao); h_glBindVertexArray(vao); h_glBindBuffer(GL_ARRAY_BUFFER, 0);
        const GLfloat vertices[] = {-1, -1, 3, -1, -1, 3};
        const uint8_t red[9] = {255, 0, 0, 255, 0, 0, 255, 0, 0};
        h_glVertexPointer(2, GL_FLOAT, 0, vertices); h_glEnableClientState(GL_VERTEX_ARRAY);
        h_glColorPointer(3, GL_UNSIGNED_BYTE, 0, red); h_glEnableClientState(GL_COLOR_ARRAY);
        h_glDisable(GL_BLEND); h_glDisable(GL_DEPTH_TEST); h_glDisable(GL_STENCIL_TEST); h_glDisable(GL_CULL_FACE); h_glDisable(GL_SCISSOR_TEST);
        h_glColorMask(1, 1, 1, 1);
        h_glViewport(0, 0, 1, 1); h_glDrawArrays(GL_TRIANGLES, 0, 3);   // default mip filter, incomplete image: use vertex color
        h_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        h_glViewport(1, 0, 1, 1); h_glDrawArrays(GL_TRIANGLES, 0, 3);   // complete image: GL_REPLACE uses the sampled color
        uint8_t pixels[8];
        h_glReadPixels(0, 0, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        assert(pixels[0] == 255 && pixels[1] == 0 && pixels[3] == 255);
        assert(pixels[4] == 0 && pixels[5] == 255 && pixels[7] == 255);

        h_glBlendFunc(0x8001 /* CONSTANT_COLOR */, GL_ZERO);
        h_glBlendColor(0, 0.5f, 0, 1);
        h_glEnable(GL_BLEND);
        h_glDrawArrays(GL_TRIANGLES, 0, 3);
        h_glReadPixels(1, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        assert(pixels[1] >= 127 && pixels[1] <= 128 && pixels[3] == 255);
        h_glDisable(GL_BLEND);

        GLuint q;
        h_glGenQueries(1, &q);
        h_glBeginQuery(GL_SAMPLES_PASSED, q);
        // More passes than the original fixed segment array could track. Each draw covers exactly one pixel.
        for (int i = 0; i < 40; i++) {
            h_glClear(GL_COLOR_BUFFER_BIT);
            h_glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        h_glEndQuery(GL_SAMPLES_PASSED);
        GLuint result;
        h_glGetQueryObjectuiv(q, GL_QUERY_RESULT_AVAILABLE, &result); assert(result == 0);
        h_glGetQueryObjectuiv(q, GL_QUERY_RESULT, &result); assert(result == 40);
        h_glGetQueryObjectuiv(q, GL_QUERY_RESULT_AVAILABLE, &result); assert(result == 1);
        h_glDeleteQueries(1, &q);

        h_glClearColor(0, 0, 1, 1); h_glClear(GL_COLOR_BUFFER_BIT);
        h_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[1], 0);
        h_glReadPixels(0, 0, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        assert(!memcmp(pixels, green, sizeof green));
        h_glBindTexture(GL_TEXTURE_2D, tex[0]);
        const uint8_t blue[8] = {0, 0, 255, 255, 0, 0, 255, 255};
        h_glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        assert(!memcmp(pixels, blue, sizeof blue));

        h_glDeleteVertexArrays(1, &vao); h_glDeleteFramebuffers(1, &fb); h_glDeleteTextures(2, tex);
    }
    assert(h_glGetError() == GL_NO_ERROR);
    S = saved;
    assert(h_eglReleaseThread());
    assert(h_eglDestroyContext(NULL, ctx));
    puts("GL fixed-function context, texture environment and draw: OK");
}

static int test_capture_shaders(void) {
    dev_init();
    assert(g_capture_dir && g_capture_compiler && !g_lib);
    const GLchar *vert = "#version 120\nattribute vec2 position; uniform float vpFlip;\n"
                         "void main() { gl_Position = vec4(position.x, position.y * vpFlip, 0.0, 1.0); }";
    const GLchar *frag = "#version 120\nuniform vec4 color;\nvoid main() { gl_FragColor = color; }";
    const GLchar *invalid = "#version 120\nuniform float guess;\nvoid main() { invalid; }";
    GLuint vs = h_glCreateShader(GL_VERTEX_SHADER), fs = h_glCreateShader(GL_FRAGMENT_SHADER), pr = h_glCreateProgram();
    GLint status;
    h_glShaderSource(vs, 1, &invalid, NULL); h_glCompileShader(vs); h_glAttachShader(pr, vs); h_glLinkProgram(pr);
    h_glGetShaderiv(vs, GL_COMPILE_STATUS, &status); assert(!status);
    h_glGetProgramiv(pr, GL_LINK_STATUS, &status); assert(!status);
    assert(h_glGetUniformLocation(pr, "guess") == -1);

    h_glShaderSource(vs, 1, &vert, NULL); h_glCompileShader(vs);
    const shinfo *info = shader_of(vs)->si;
    h_glGetShaderiv(vs, GL_COMPILE_STATUS, &status); assert(status && info);
    h_glCompileShader(vs); assert(shader_of(vs)->si == info);
    h_glShaderSource(fs, 1, &frag, NULL); h_glCompileShader(fs); h_glAttachShader(pr, fs);
    h_glLinkProgram(pr);
    h_glGetProgramiv(pr, GL_LINK_STATUS, &status); assert(status);
    GLint flip = h_glGetUniformLocation(pr, "vpFlip"), color = h_glGetUniformLocation(pr, "color");
    assert(flip > 0 && color > 0);
    h_glDetachShader(pr, vs); h_glDetachShader(pr, fs); h_glDeleteShader(vs); h_glDeleteShader(fs);
    h_glUseProgram(pr); h_glUniform1f(flip, 1); h_glUniform4f(color, 0, 1, 0, 1);
    GLuint vao, vb, fb, target;
    h_glGenVertexArrays(1, &vao); h_glBindVertexArray(vao); h_glGenBuffers(1, &vb); h_glBindBuffer(GL_ARRAY_BUFFER, vb);
    const float vertices[] = {-1, -1, 3, -1, -1, 3};
    GLuint pos = h_glGetAttribLocation(pr, "position"); assert(pos < 16);
    h_glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, 0);
    h_glVertexAttribPointer(pos, 2, GL_FLOAT, 0, 0, NULL); h_glEnableVertexAttribArray(pos);
    h_glGenTextures(1, &target); h_glBindTexture(GL_TEXTURE_2D, target);
    h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    h_glGenFramebuffers(1, &fb); h_glBindFramebuffer(GL_FRAMEBUFFER, fb);
    h_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    h_glViewport(0, 0, 1, 1); h_glDrawArrays(GL_TRIANGLES, 0, 3);
    uint8_t pixel[4], green[] = {0, 255, 0, 255};
    h_glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    assert(!memcmp(pixel, green, sizeof green) && h_glGetError() == GL_NO_ERROR);
    h_glUseProgram(0); h_glDeleteProgram(pr);
    h_glDeleteFramebuffers(1, &fb); h_glDeleteTextures(1, &target); h_glDeleteBuffers(1, &vb); h_glDeleteVertexArrays(1, &vao);

    // Desktop GLSL 1.20 with implicit int->float conversion, sampler3D + texture3D, and GL_TEXTURE_SWIZZLE_RGBA on a 3D LUT.
    const GLchar *vert3d = "#version 120\nattribute vec2 position;\nvoid main() { gl_Position = vec4(position, 0, 1); }";
    const GLchar *frag3d = "#version 120\nuniform sampler3D lut;\nvoid main() { float scale = 1; gl_FragColor = texture3D(lut, vec3(0.5)) * scale; }";
    GLuint vs3 = h_glCreateShader(GL_VERTEX_SHADER), fs3 = h_glCreateShader(GL_FRAGMENT_SHADER), pr3 = h_glCreateProgram();
    h_glShaderSource(vs3, 1, &vert3d, NULL); h_glCompileShader(vs3);
    h_glShaderSource(fs3, 1, &frag3d, NULL); h_glCompileShader(fs3);
    h_glAttachShader(pr3, vs3); h_glAttachShader(pr3, fs3); h_glLinkProgram(pr3);
    h_glGetProgramiv(pr3, GL_LINK_STATUS, &status); assert(status);
    GLuint vao2, vb2, fb2, rt2, lut3d;
    h_glGenVertexArrays(1, &vao2); h_glBindVertexArray(vao2); h_glGenBuffers(1, &vb2); h_glBindBuffer(GL_ARRAY_BUFFER, vb2);
    h_glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, 0);
    GLuint pos3 = h_glGetAttribLocation(pr3, "position"); assert(pos3 < 16);
    h_glVertexAttribPointer(pos3, 2, GL_FLOAT, 0, 0, NULL); h_glEnableVertexAttribArray(pos3);
    h_glGenTextures(1, &rt2); h_glBindTexture(GL_TEXTURE_2D, rt2);
    h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    h_glGenFramebuffers(1, &fb2); h_glBindFramebuffer(GL_FRAMEBUFFER, fb2);
    h_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt2, 0);
    h_glActiveTexture(GL_TEXTURE0);
    h_glGenTextures(1, &lut3d); h_glBindTexture(GL_TEXTURE_3D, lut3d);
    const GLint swz[4] = {GL_BLUE, GL_RED, GL_GREEN, GL_ONE};
    h_glTexParameteriv(GL_TEXTURE_3D, GL_TEXTURE_SWIZZLE_RGBA, swz);
    h_glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    h_glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    const uint8_t lut_px[4] = {10, 20, 200, 50};
    h_glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, lut_px);
    h_glUseProgram(pr3); h_glViewport(0, 0, 1, 1); h_glDrawArrays(GL_TRIANGLES, 0, 3);
    const uint8_t want_swz[4] = {200, 10, 20, 255};
    h_glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    assert(!memcmp(pixel, want_swz, sizeof want_swz) && h_glGetError() == GL_NO_ERROR);

    // Explicit layout(binding = 1) sampler reads from texture unit 1 without glUniform1i.
    const GLchar *vertb = "#version 450 core\nlayout(location = 0) in vec2 position;\nvoid main() { gl_Position = vec4(position, 0.0, 1.0); }";
    const GLchar *fragb = "#version 450 core\nlayout(binding = 0) uniform sampler2D t0;\nlayout(binding = 1) uniform sampler2D t1;\n"
                          "out vec4 outColor;\nvoid main() { outColor = vec4(texture(t0, vec2(0.5)).r, texture(t1, vec2(0.5)).g, 0.0, 1.0); }";
    GLuint vsb = h_glCreateShader(GL_VERTEX_SHADER), fsb = h_glCreateShader(GL_FRAGMENT_SHADER), prb = h_glCreateProgram();
    h_glShaderSource(vsb, 1, &vertb, NULL); h_glCompileShader(vsb);
    h_glShaderSource(fsb, 1, &fragb, NULL); h_glCompileShader(fsb);
    h_glAttachShader(prb, vsb); h_glAttachShader(prb, fsb); h_glLinkProgram(prb);
    h_glGetProgramiv(prb, GL_LINK_STATUS, &status); assert(status);
    GLint u0 = h_glGetUniformLocation(prb, "t0"), u1 = h_glGetUniformLocation(prb, "t1");
    GLint v0[16] = {-1}, v1[16] = {-1};   // allow a full matrix if reflection reports the wrong type
    assert(u0 > 0 && u1 > 0);
    h_glGetUniformiv(prb, u0, v0); h_glGetUniformiv(prb, u1, v1);
    assert(v0[0] == 0 && v1[0] == 1);
    GLuint bt[2]; h_glGenTextures(2, bt);
    const uint8_t red_px[4] = {220, 0, 0, 255}, grn_px[4] = {0, 180, 0, 255};
    h_glActiveTexture(GL_TEXTURE0); h_glBindTexture(GL_TEXTURE_2D, bt[0]);
    h_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, red_px);
    h_glActiveTexture(GL_TEXTURE0 + 1); h_glBindTexture(GL_TEXTURE_2D, bt[1]);
    h_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, grn_px);
    h_glActiveTexture(GL_TEXTURE0);
    h_glUseProgram(prb); h_glDrawArrays(GL_TRIANGLES, 0, 3);
    const uint8_t want_bind[4] = {220, 180, 0, 255};
    h_glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    assert(!memcmp(pixel, want_bind, sizeof want_bind) && h_glGetError() == GL_NO_ERROR);

    const GLchar *frag_array = "#version 300 es\nprecision highp float;\nuniform sampler2D tex[2];\n"
                               "out vec4 color;\nvoid main() { color = vec4(texture(tex[0], vec2(0.5)).r, texture(tex[1], vec2(0.5)).g, 0.0, 1.0); }";
    GLuint fsa = h_glCreateShader(GL_FRAGMENT_SHADER), pra = h_glCreateProgram();
    h_glShaderSource(fsa, 1, &frag_array, NULL); h_glCompileShader(fsa);
    h_glAttachShader(pra, vsb); h_glAttachShader(pra, fsa); h_glLinkProgram(pra);
    h_glGetProgramiv(pra, GL_LINK_STATUS, &status); assert(status);
    GLint a0 = h_glGetUniformLocation(pra, "tex"), a1 = h_glGetUniformLocation(pra, "tex[1]");
    assert(a0 > 0 && a1 > a0 && h_glGetUniformLocation(pra, "tex[2]") == -1);
    h_glGetUniformiv(pra, a1, v1); assert(v1[0] == 0);
    h_glUseProgram(pra);
    const GLint units[] = {0, 1}; h_glUniform1iv(a0, 2, units);
    h_glGetUniformiv(pra, a1, v1); assert(v1[0] == 1);
    h_glDrawArrays(GL_TRIANGLES, 0, 3);
    h_glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    assert(!memcmp(pixel, want_bind, sizeof want_bind) && h_glGetError() == GL_NO_ERROR);
    h_glUseProgram(0); h_glDeleteProgram(pra); h_glDeleteShader(fsa);

    h_glUseProgram(0); h_glDeleteProgram(prb); h_glDeleteShader(vsb); h_glDeleteShader(fsb);
    h_glDeleteProgram(pr3); h_glDeleteShader(vs3); h_glDeleteShader(fs3);
    h_glDeleteTextures(2, bt); h_glDeleteTextures(1, &lut3d);
    h_glDeleteFramebuffers(1, &fb2); h_glDeleteTextures(1, &rt2); h_glDeleteBuffers(1, &vb2); h_glDeleteVertexArrays(1, &vao2);
    puts("GL fresh capture: compile failure, legacy link, cache reuse, uniform writes and GPU readback: OK");
    return 0;
}

// Read stored framebuffer bytes: RGB alone is encoded, alpha is always linear.
static int srgb_pixel(int x, const float color[4], int encode, const char *label) {
    uint8_t pixel[4]; h_glReadPixels(x, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    for (int i = 0; i < 4; i++) {
        float v = color[i];
        if (encode && i < 3) v = v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1.f / 2.4f) - 0.055f;
        int want = (int)lroundf(v * 255);
        if (abs(pixel[i] - want) > 1) {
            fprintf(stderr, "%s: pixel %d channel %d got %u, expected %d\n", label, x, i, pixel[i], want);
            return 1;
        }
    }
    return 0;
}

// GL_FRAMEBUFFER_SRGB chooses framebuffer conversion and destination blending; sampling
// still decodes an sRGB texture. No guest shader or game data is used.
static int test_framebuffer_srgb(void) {
    dev_init();
    EGLContext ctx = h_eglCreateContext(NULL, NULL, NULL, NULL); assert(ctx);
    assert(h_eglMakeCurrent(NULL, NULL, NULL, ctx));
    assert(import("glEnable") == (void *)h_glEnable_bs);
    assert(import("glDisable") == (void *)h_glDisable_bs);
    __typeof__(S) saved = S; __typeof__(F) fsaved = F;
    h_glUseProgram(0); h_glActiveTexture(GL_TEXTURE0); h_glDisable(GL_TEXTURE_2D);
    h_glDisable(GL_BLEND); h_glDisable(GL_DEPTH_TEST); h_glDisable(GL_STENCIL_TEST); h_glDisable(GL_CULL_FACE);
    h_glDisable(GL_ALPHA_TEST); h_glColorMask(1, 1, 1, 1);
    h_glMatrixMode(GL_MODELVIEW); h_glLoadIdentity(); h_glMatrixMode(GL_PROJECTION); h_glLoadIdentity();
    GLuint vao; h_glGenVertexArrays(1, &vao); h_glBindVertexArray(vao); h_glBindBuffer(GL_ARRAY_BUFFER, 0);
    const GLfloat vertices[] = {-1, -1, 3, -1, -1, 3};
    h_glVertexPointer(2, GL_FLOAT, 0, vertices); h_glEnableClientState(GL_VERTEX_ARRAY); h_glDisableClientState(GL_COLOR_ARRAY);
    const float color[4] = {0.25f, 0.5f, 0.75f, 0.5f};
    const float blend[4] = {0.375f, 0.375f, 0.375f, 0.5f};
    int checks = 0;
    for (int kind = 0; kind < 7; kind++) {
        int srgb = kind != 1 && kind != 3, rb = kind == 2 || kind == 3;
        GLenum fmt = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
        GLuint tex = 0, renderbuffer = 0, fb = 0;
        EGLSurface win_surf = NULL;
        if (kind != 6) h_glGenFramebuffers(1, &fb);
        h_glBindFramebuffer(GL_FRAMEBUFFER, fb);
        if (!fb) {
            win_surf = h_eglCreateWindowSurface(NULL, NULL, NULL, NULL);
            assert(win_surf);
        } else if (rb) {
            h_glGenRenderbuffers(1, &renderbuffer); h_glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
            h_glRenderbufferStorage(GL_RENDERBUFFER, fmt, 2, 1);
            h_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
        } else {
            GLenum target = kind == 4 ? GL_TEXTURE_2D_ARRAY : kind == 5 ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
            h_glGenTextures(1, &tex); h_glBindTexture(target, tex);
            if (kind == 4) {
                h_glTexStorage3D(target, 2, fmt, 4, 2, 2);
                h_glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 1, 1);
            } else if (kind == 5) {
                h_glTexStorage2D(target, 2, fmt, 4, 4);
                h_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + 2, tex, 1);
            } else {
                h_glTexImage2D(target, 0, fmt, 2, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                h_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, tex, 0);
            }
        }
        // A deferred whole-image clear keeps the state at issue, even if the toggle changes before any draw.
        for (int on = 0; on < 2; on++) {
            cap_set(GL_FRAMEBUFFER_SRGB, on); h_glDisable(GL_SCISSOR_TEST);
            h_glClearColor(color[0], color[1], color[2], color[3]); h_glClear(GL_COLOR_BUFFER_BIT);
            cap_set(GL_FRAMEBUFFER_SRGB, !on);
            if (srgb_pixel(0, color, on && srgb, "pending clear before toggle")) return 1;
            checks++;
        }
        // Partial clears go through a draw; a toggle must finish its existing encoder too.
        h_glDisable(GL_SCISSOR_TEST); h_glClearColor(0, 0, 0, 0); h_glClear(GL_COLOR_BUFFER_BIT);
        h_glEnable(GL_SCISSOR_TEST); h_glClearColor(color[0], color[1], color[2], color[3]);
        h_glDisable(GL_FRAMEBUFFER_SRGB); h_glScissor(0, 0, 1, 1); h_glClear(GL_COLOR_BUFFER_BIT);
        h_glEnable(GL_FRAMEBUFFER_SRGB); h_glScissor(1, 0, 1, 1); h_glClear(GL_COLOR_BUFFER_BIT);
        if (srgb_pixel(0, color, 0, "linear partial clear") || srgb_pixel(1, color, srgb, "encoded partial clear")) return 1;
        checks += 2;
        h_glDisable(GL_SCISSOR_TEST);
        // Ordinary GL draws into the same attachment, without an intervening readback or bind.
        h_glColor4fv(color); h_glDisable(GL_FRAMEBUFFER_SRGB); h_glViewport(0, 0, 1, 1); h_glDrawArrays(GL_TRIANGLES, 0, 3);
        h_glEnable(GL_FRAMEBUFFER_SRGB); h_glViewport(1, 0, 1, 1); h_glDrawArrays(GL_TRIANGLES, 0, 3);
        if (srgb_pixel(0, color, 0, "linear draw") || srgb_pixel(1, color, srgb, "encoded draw")) return 1;
        checks += 2;
        for (int on = 0; on < 2; on++) {
            cap_set(GL_FRAMEBUFFER_SRGB, on); h_glClearColor(0.25f, 0.25f, 0.25f, 0.5f); h_glClear(GL_COLOR_BUFFER_BIT);
            h_glColor4f(0.5f, 0.5f, 0.5f, 0.5f); h_glBlendFunc(0x302 /* SRC_ALPHA */, 0x303 /* ONE_MINUS_SRC_ALPHA */); h_glEnable(GL_BLEND);
            h_glViewport(0, 0, 2, 1); h_glDrawArrays(GL_TRIANGLES, 0, 3); h_glDisable(GL_BLEND);
            if (srgb_pixel(0, blend, on && srgb, "destination blend")) return 1;
            checks++;
        }
        // A nonzero clear distinguishes writes to the new storage from writes to a stale view.
        if (rb || kind < 2) {
            h_glDisable(GL_FRAMEBUFFER_SRGB);
            if (rb) h_glRenderbufferStorage(GL_RENDERBUFFER, fmt, 2, 1);
            else h_glTexImage2D(GL_TEXTURE_2D, 0, fmt, 4, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            h_glClearColor(color[0], color[1], color[2], color[3]); h_glClear(GL_COLOR_BUFFER_BIT);
            if (srgb_pixel(0, color, 0, rb ? "replacement renderbuffer storage" : "replacement texture storage")) return 1;
            checks++;
        }
        if (fb) {
            h_glBindFramebuffer(GL_FRAMEBUFFER, 0); h_glDeleteFramebuffers(1, &fb);
            if (rb) h_glDeleteRenderbuffers(1, &renderbuffer); else h_glDeleteTextures(1, &tex);
        } else {
            GLint encoding = 0;
            h_glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_BACK, 0x8210 /* COLOR_ENCODING */, &encoding);
            assert(encoding == 0x8c40 /* SRGB */);
            assert(h_eglDestroySurface(NULL, win_surf));
        }
    }
    // Write sRGB bytes through the linear render view, then sample the same texture.
    GLuint sample[2], sample_fb; h_glGenTextures(2, sample);
    h_glBindTexture(GL_TEXTURE_2D, sample[0]);
    h_glTexStorage2D(GL_TEXTURE_2D, 1, GL_SRGB8_ALPHA8, 1, 1);
    h_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    h_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    h_glBindTexture(GL_TEXTURE_2D, sample[1]); h_glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1);
    h_glGenFramebuffers(1, &sample_fb); h_glBindFramebuffer(GL_FRAMEBUFFER, sample_fb);
    h_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sample[0], 0);
    h_glDisable(GL_FRAMEBUFFER_SRGB);
    const float encoded[4] = {137.f / 255, 188.f / 255, 225.f / 255, 128.f / 255};
    h_glClearColor(encoded[0], encoded[1], encoded[2], encoded[3]); h_glClear(GL_COLOR_BUFFER_BIT);
    if (srgb_pixel(0, encoded, 0, "sRGB sampling source through linear view")) return 1;
    checks++;
    h_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sample[1], 0);
    h_glBindTexture(GL_TEXTURE_2D, sample[0]); h_glEnable(GL_TEXTURE_2D); h_glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    h_glMatrixMode(GL_TEXTURE); h_glLoadIdentity(); h_glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    h_glViewport(0, 0, 1, 1); h_glDrawArrays(GL_TRIANGLES, 0, 3);
    if (srgb_pixel(0, color, 0, "sRGB sampling to linear target")) return 1;
    checks++;
    h_glBindFramebuffer(GL_FRAMEBUFFER, 0); h_glDeleteFramebuffers(1, &sample_fb); h_glDeleteTextures(2, sample);
    h_glBindVertexArray(0); h_glDeleteVertexArrays(1, &vao); S = saved; F = fsaved;
    assert(h_eglReleaseThread());
    assert(h_eglDestroyContext(NULL, ctx));
    printf("GL framebuffer sRGB: %d pixel checks passed\n", checks); return 0;
}

static void test_image_copies(void) {
    const uint8_t block[16] = {255, 255, 0, 0, 0, 0, 0, 0, 0, 0xf8};   // one red BC3 block
    GLuint tex[3]; h_glGenTextures(3, tex);
    h_glBindTexture(GL_TEXTURE_2D, tex[0]);
    h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI, 1, 1, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, block);
    h_glBindTexture(GL_TEXTURE_2D, tex[1]);
    h_glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5, 4, 4, 0, sizeof block, NULL);
    h_glCopyImageSubData(tex[0], GL_TEXTURE_2D, 0, 0, 0, 0, tex[1], GL_TEXTURE_2D, 0, 0, 0, 0, 1, 1, 1);
    GLint size = 0; h_glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, 0x86a0, &size); assert(size == sizeof block);
    uint8_t got[16] = {0}; h_glGetCompressedTexImage(GL_TEXTURE_2D, 0, got); assert(!memcmp(block, got, sizeof got));
    uint32_t decoded[16] = {0}; h_glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, decoded);
    for (unsigned i = 0; i < 16; i++) assert(decoded[i] == 0xff0000ff);
    h_glBindTexture(GL_TEXTURE_2D, tex[2]);
    h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32UI, 1, 1, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, NULL);
    h_glCopyImageSubData(tex[1], GL_TEXTURE_2D, 0, 0, 0, 0, tex[2], GL_TEXTURE_2D, 0, 0, 0, 0, 4, 4, 1);
    memset(got, 0, sizeof got); h_glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, got);
    assert(!memcmp(block, got, sizeof got) && h_glGetError() == GL_NO_ERROR);
    h_glDeleteTextures(3, tex);
}

static void test_multiple_draw_buffers(void) {
    typeof(S) saved = S;
    h_glColorMask(1, 1, 1, 1); h_glDisable(GL_SCISSOR_TEST);
    GLuint tex[2], fb; h_glGenTextures(2, tex); h_glGenFramebuffers(1, &fb); h_glBindFramebuffer(GL_FRAMEBUFFER, fb);
    for (int i = 0; i < 2; i++) {
        h_glBindTexture(GL_TEXTURE_2D, tex[i]);
        h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        h_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, tex[i], 0);
    }
    const GLenum reverse[] = {GL_COLOR_ATTACHMENT0 + 1, GL_COLOR_ATTACHMENT0};
    const GLfloat red[] = {1, 0, 0, 1}, green[] = {0, 1, 0, 1};
    h_glDrawBuffers(2, reverse);
    h_glClearBufferfv(GL_COLOR, 0, red); h_glClearBufferfv(GL_COLOR, 1, green);
    h_glColorMaski(0, 0, 1, 0, 0); h_glColorMaski(1, 0, 0, 0, 0);
    h_glEnable(GL_SCISSOR_TEST); h_glScissor(0, 0, 1, 1);
    h_glClearColor(0, 1, 1, 0); h_glClear(GL_COLOR_BUFFER_BIT);
    uint32_t pixels[4] = {0};
    h_glReadBuffer(GL_COLOR_ATTACHMENT0 + 1); h_glReadPixels(0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    assert(pixels[0] == 0xff00ffff && pixels[1] == 0xff0000ff && pixels[2] == 0xff0000ff && pixels[3] == 0xff0000ff);
    h_glReadBuffer(GL_COLOR_ATTACHMENT0); h_glReadPixels(0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    for (unsigned i = 0; i < 4; i++) assert(pixels[i] == 0xff00ff00);
    assert(h_glGetError() == GL_NO_ERROR);
    h_glDeleteFramebuffers(1, &fb); h_glDeleteTextures(2, tex); S = saved;
}

static void test_texture_buffer_storage(void) {
    GLuint buf, tex; h_glGenBuffers(1, &buf); h_glGenTextures(1, &tex);
    const float data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    h_glBindBuffer(GL_TEXTURE_BUFFER, buf); h_glBufferData(GL_TEXTURE_BUFFER, sizeof data, data, 0);
    h_glBindTexture(GL_TEXTURE_BUFFER, tex); h_glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, buf);
    id<MTLTexture> before = texture_buffer(tex_of(tex));
    assert(before.buffer == buf_of(buf)->mb && before.width == 2);
    h_glBufferData(GL_TEXTURE_BUFFER, sizeof data / 2, data, 0);
    id<MTLTexture> after = texture_buffer(tex_of(tex));
    assert(after.buffer == buf_of(buf)->mb && after.width == 1 && before != after);
    h_glDeleteBuffers(1, &buf); assert(texture_buffer(tex_of(tex)) == g_tbuf0);
    h_glDeleteTextures(1, &tex); assert(h_glGetError() == GL_NO_ERROR);
}

static void test_timestamps(void) {
    GLuint query[2]; h_glGenQueries(2, query);
    h_glQueryCounter(query[0], GL_TIMESTAMP); h_glQueryCounter(query[1], GL_TIMESTAMP); h_glFinish();
    int64_t t0 = 0, t1 = 0, available = 0;
    h_glGetQueryObjecti64v(query[1], GL_QUERY_RESULT_AVAILABLE, &available); assert(available == 1);
    h_glGetQueryObjecti64v(query[0], GL_QUERY_RESULT, &t0); h_glGetQueryObjecti64v(query[1], GL_QUERY_RESULT, &t1);
    assert(t0 > 0 && t1 >= t0 && h_glGetError() == GL_NO_ERROR);
    h_glDeleteQueries(2, query);
}

static void test_swizzle_and_layout_binding(void) {
    typeof(S) saved = S;
    const char *msl =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct A2 { texture2d<float> t0 [[id(0)]]; sampler s0 [[id(1)]]; texture2d<float> t1 [[id(2)]]; sampler s1 [[id(3)]]; };\n"
        "struct A3 { texture3d<float> lut [[id(0)]]; sampler s0 [[id(1)]]; };\n"
        "vertex float4 vs(uint vid [[vertex_id]]) { float2 p[3] = {{-1,-1},{3,-1},{-1,3}}; return float4(p[vid], 0, 1); }\n"
        "fragment float4 fs_bind(constant A2 &a [[buffer(0)]]) { return float4(a.t0.sample(a.s0, float2(0.5)).r, a.t1.sample(a.s1, float2(0.5)).g, 0, 1); }\n"
        "fragment float4 fs_lut(constant A3 &a [[buffer(0)]]) { return a.lut.sample(a.s0, float3(0.5)); }\n";
    NSError *err = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(msl) options:nil error:&err]; assert(lib);
    id<MTLFunction> fvs = [lib newFunctionWithName:@"vs"], fbind = [lib newFunctionWithName:@"fs_bind"], flut = [lib newFunctionWithName:@"fs_lut"];
    assert(fvs && fbind && flut);

    char vrow[] = "1111111111111111\tvert\tr:-1:0\tmain#0";
    char brow[] = "2222222222222222\tfrag\tr:0:4 t:t0=0:1:sampler2D t:t1=2:3:sampler2D:1\tmain#0";
    char lrow[] = "3333333333333333\tfrag\tr:0:2 t:lut=0:1:sampler3D\tmain#0";
    shinfo vsi = {0}, bsi = {0}, lsi = {0};
    assert(sh_read_row(vrow, &vsi) && sh_read_row(brow, &bsi) && sh_read_row(lrow, &lsi));

    GLuint vs = h_glCreateShader(GL_VERTEX_SHADER), fsb = h_glCreateShader(GL_FRAGMENT_SHADER), fsl = h_glCreateShader(GL_FRAGMENT_SHADER);
    shader_of(vs)->si = &vsi; shader_of(vs)->fn = fvs;
    shader_of(fsb)->si = &bsi; shader_of(fsb)->fn = fbind;
    shader_of(fsl)->si = &lsi; shader_of(fsl)->fn = flut;
    GLuint prb = h_glCreateProgram(), prl = h_glCreateProgram();
    h_glAttachShader(prb, vs); h_glAttachShader(prb, fsb); h_glLinkProgram(prb);
    h_glAttachShader(prl, vs); h_glAttachShader(prl, fsl); h_glLinkProgram(prl);
    assert(prog_of(prb)->linked && prog_of(prl)->linked);
    GLint u0 = h_glGetUniformLocation(prb, "t0"), u1 = h_glGetUniformLocation(prb, "t1");
    GLint v0[16] = {-1}, v1[16] = {-1};   // allow a full matrix if reflection reports the wrong type
    h_glGetUniformiv(prb, u0, v0); h_glGetUniformiv(prb, u1, v1);
    assert(v0[0] == 0 && v1[0] == 1);

    GLuint rt, fb, vao, bt[2], lut;
    h_glGenFramebuffers(1, &fb); h_glBindFramebuffer(GL_FRAMEBUFFER, fb);
    h_glGenTextures(1, &rt); h_glBindTexture(GL_TEXTURE_2D, rt);
    h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    h_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
    h_glGenVertexArrays(1, &vao); h_glBindVertexArray(vao);
    h_glDisable(GL_BLEND); h_glDisable(GL_DEPTH_TEST); h_glDisable(GL_STENCIL_TEST); h_glDisable(GL_CULL_FACE); h_glDisable(GL_SCISSOR_TEST);
    h_glColorMask(1, 1, 1, 1); h_glViewport(0, 0, 1, 1);

    // Explicit layout(binding = 1) samples unit 1 without glUniform1i.
    h_glGenTextures(2, bt);
    const uint8_t red[4] = {220, 0, 0, 255}, green[4] = {0, 180, 0, 255};
    h_glActiveTexture(GL_TEXTURE0); h_glBindTexture(GL_TEXTURE_2D, bt[0]);
    h_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, red);
    h_glActiveTexture(GL_TEXTURE0 + 1); h_glBindTexture(GL_TEXTURE_2D, bt[1]);
    h_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    h_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, green);
    h_glActiveTexture(GL_TEXTURE0);
    h_glUseProgram(prb); h_glDrawArrays(GL_TRIANGLES, 0, 3);
    uint8_t px[4] = {0}; const uint8_t want_bind[4] = {220, 180, 0, 255};
    h_glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    assert(!memcmp(px, want_bind, sizeof want_bind));

    // GL_TEXTURE_SWIZZLE_RGBA on a 3D LUT texture swaps channels on GPU sampling.
    h_glGenTextures(1, &lut); h_glBindTexture(GL_TEXTURE_3D, lut);
    const GLint swz[4] = {GL_BLUE, GL_RED, GL_GREEN, GL_ONE};
    h_glTexParameteriv(GL_TEXTURE_3D, GL_TEXTURE_SWIZZLE_RGBA, swz);
    h_glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    h_glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    const uint8_t lut_px[4] = {10, 20, 200, 50};
    h_glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, 1, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, lut_px);
    h_glUseProgram(prl); h_glDrawArrays(GL_TRIANGLES, 0, 3);
    const uint8_t want_swz[4] = {200, 10, 20, 255};
    h_glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    assert(!memcmp(px, want_swz, sizeof want_swz) && h_glGetError() == GL_NO_ERROR);

    h_glUseProgram(0); h_glDeleteProgram(prb); h_glDeleteProgram(prl);
    h_glDeleteShader(vs); h_glDeleteShader(fsb); h_glDeleteShader(fsl);
    h_glDeleteTextures(2, bt); h_glDeleteTextures(1, &lut); h_glDeleteTextures(1, &rt);
    h_glDeleteFramebuffers(1, &fb); h_glDeleteVertexArrays(1, &vao);
    sh_free(&vsi); sh_free(&bsi); sh_free(&lsi);
    S = saved;
    puts("GL layout(binding) and GL_TEXTURE_SWIZZLE_RGBA GPU readback: OK");
}

// OpenGL 4.5 direct state access (issue #3420): object creation, direct operations, GL validation, and binding preservation.
static void test_dsa(void) {
    void (*create_bufs)(GLsizei, GLuint *) = import("glCreateBuffers");
    void (*named_buf_data)(GLuint, GLsizeiptr, const void *, GLenum) = import("glNamedBufferData");
    void (*create_texs)(GLenum, GLsizei, GLuint *) = import("glCreateTextures");
    void (*tex_storage2d)(GLuint, GLsizei, GLenum, GLsizei, GLsizei) = import("glTextureStorage2D");
    void (*tex_sub2d)(GLuint, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *) = import("glTextureSubImage2D");
    void (*comp_sub2d)(GLuint, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *) = import("glCompressedTextureSubImage2D");
    void (*tex_parami)(GLuint, GLenum, GLint) = import("glTextureParameteri");
    void (*tex_paramfv)(GLuint, GLenum, const GLfloat *) = import("glTextureParameterfv");
    void (*create_fbs)(GLsizei, GLuint *) = import("glCreateFramebuffers");
    void (*named_fb_tex)(GLuint, GLenum, GLuint, GLint) = import("glNamedFramebufferTexture");
    GLenum (*check_named_fb)(GLuint, GLenum) = import("glCheckNamedFramebufferStatus");
    assert(h_eglGetProcAddress("glCreateBuffers") == (void *)create_bufs);
    assert(h_eglGetProcAddress("glNamedBufferData") == (void *)named_buf_data);
    assert(h_eglGetProcAddress("glCreateTextures") == (void *)create_texs);
    assert(h_eglGetProcAddress("glTextureStorage2D") == (void *)tex_storage2d);
    assert(h_eglGetProcAddress("glTextureSubImage2D") == (void *)tex_sub2d);
    assert(h_eglGetProcAddress("glCompressedTextureSubImage2D") == (void *)comp_sub2d);
    assert(h_eglGetProcAddress("glTextureParameteri") == (void *)tex_parami);
    assert(h_eglGetProcAddress("glTextureParameterfv") == (void *)tex_paramfv);
    assert(h_eglGetProcAddress("glCreateFramebuffers") == (void *)create_fbs);
    assert(h_eglGetProcAddress("glNamedFramebufferTexture") == (void *)named_fb_tex);
    assert(h_eglGetProcAddress("glCheckNamedFramebufferStatus") == (void *)check_named_fb);
    assert(h_glGetError() == GL_NO_ERROR);

    // Sentinel bindings across buffers, textures (non-zero active unit), and draw/read framebuffers
    GLuint vao = 0; h_glGenVertexArrays(1, &vao); h_glBindVertexArray(vao);
    GLuint sb[4] = {0}; h_glGenBuffers(4, sb);
    h_glBindBuffer(GL_ARRAY_BUFFER, sb[0]);
    h_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sb[1]);
    h_glBindBuffer(GL_UNIFORM_BUFFER, sb[2]);
    h_glBindBuffer(GL_TEXTURE_BUFFER, sb[3]);
    GLuint st[5] = {0}; h_glGenTextures(5, st);
    h_glActiveTexture(GL_TEXTURE0 + 3);
    h_glBindTexture(GL_TEXTURE_2D, st[0]);
    h_glBindTexture(GL_TEXTURE_CUBE_MAP, st[1]);
    h_glBindTexture(GL_TEXTURE_3D, st[2]);
    h_glBindTexture(GL_TEXTURE_2D_ARRAY, st[3]);
    h_glBindTexture(GL_TEXTURE_BUFFER, st[4]);
    GLuint sfb[2] = {0}; h_glGenFramebuffers(2, sfb);
    h_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sfb[0]);
    h_glBindFramebuffer(GL_READ_FRAMEBUFFER, sfb[1]);

    // Buffers: glCreateBuffers + glNamedBufferData
    GLuint b = 0, gen_b = 0, imm_b = 0;
    create_bufs(1, &b);
    assert(b && buf_of(b) && buf_of(b)->created && h_glGetError() == GL_NO_ERROR);
    const uint32_t bdata[4] = {0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u};
    named_buf_data(b, sizeof bdata, bdata, 0x88e8 /* GL_DYNAMIC_DRAW */);
    assert(h_glGetError() == GL_NO_ERROR && buf_of(b)->size == sizeof bdata && !memcmp(buf_of(b)->mb.contents, bdata, sizeof bdata));
    // Negative size leaves existing storage intact and records GL_INVALID_VALUE
    named_buf_data(b, -4, bdata, 0x88e8);
    assert(h_glGetError() == GL_INVALID_VALUE && buf_of(b)->size == sizeof bdata && !memcmp(buf_of(b)->mb.contents, bdata, sizeof bdata));
    // Invalid usage enum
    named_buf_data(b, 4, bdata, 0x1234);
    assert(h_glGetError() == GL_INVALID_ENUM);
    // Size 0 clears storage; NULL data zeroes storage
    named_buf_data(b, 0, NULL, 0x88e4 /* GL_STATIC_DRAW */);
    assert(h_glGetError() == GL_NO_ERROR && !buf_of(b)->mb && !buf_of(b)->size);
    named_buf_data(b, 16, NULL, 0x88e0 /* GL_STREAM_DRAW */);
    assert(h_glGetError() == GL_NO_ERROR && buf_of(b)->size == 16);
    for (int i = 0; i < 16; i++) assert(((const uint8_t *)buf_of(b)->mb.contents)[i] == 0);
    // Uncreated / invalid buffer names and negative count
    create_bufs(-1, &gen_b); assert(h_glGetError() == GL_INVALID_VALUE);
    named_buf_data(0, 16, bdata, 0x88e8); assert(h_glGetError() == GL_INVALID_OPERATION);
    named_buf_data(999999, 16, bdata, 0x88e8); assert(h_glGetError() == GL_INVALID_OPERATION);
    h_glGenBuffers(1, &gen_b);
    named_buf_data(gen_b, sizeof bdata, bdata, 0x88e8); assert(h_glGetError() == GL_INVALID_OPERATION);
    h_glBindBuffer(GL_ARRAY_BUFFER, gen_b); h_glBindBuffer(GL_ARRAY_BUFFER, sb[0]);
    named_buf_data(gen_b, sizeof bdata, bdata, 0x88e8); assert(h_glGetError() == GL_NO_ERROR && buf_of(gen_b)->size == sizeof bdata);
    // Immutable buffer rejects glNamedBufferData with GL_INVALID_OPERATION
    h_glGenBuffers(1, &imm_b);
    h_glBindBuffer(GL_ARRAY_BUFFER, imm_b); h_glBufferStorage(GL_ARRAY_BUFFER, 32, NULL, 0); h_glBindBuffer(GL_ARRAY_BUFFER, sb[0]);
    named_buf_data(imm_b, 16, bdata, 0x88e8); assert(h_glGetError() == GL_INVALID_OPERATION && buf_of(imm_b)->size == 32);

    // Textures: glCreateTextures, glTextureStorage2D, glTextureSubImage2D, glCompressedTextureSubImage2D, glTextureParameteri/fv
    GLuint t2d = 0, tbc = 0, tcube = 0, tcube2 = 0, t3d = 0, tbuf = 0, gen_t = 0;
    create_texs(GL_TEXTURE_2D, 1, &t2d);
    create_texs(GL_TEXTURE_2D, 1, &tbc);
    create_texs(GL_TEXTURE_CUBE_MAP, 1, &tcube);
    create_texs(GL_TEXTURE_CUBE_MAP, 1, &tcube2);
    create_texs(GL_TEXTURE_3D, 1, &t3d);
    create_texs(GL_TEXTURE_BUFFER, 1, &tbuf);
    assert(h_glGetError() == GL_NO_ERROR);
    assert(tex_of(t2d)->target == GL_TEXTURE_2D && tex_of(tcube)->target == GL_TEXTURE_CUBE_MAP && tex_of(t3d)->target == GL_TEXTURE_3D && tex_of(tbuf)->target == GL_TEXTURE_BUFFER);

    // Texture parameters and swizzle without touching bound textures
    const GLfloat border[4] = {1, 0, 0, 1}, swz[4] = {GL_BLUE, GL_GREEN, GL_RED, GL_ONE};
    tex_parami(t2d, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    tex_parami(t2d, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    tex_paramfv(t2d, GL_TEXTURE_BORDER_COLOR, border);
    tex_paramfv(t2d, GL_TEXTURE_SWIZZLE_RGBA, swz);
    assert(h_glGetError() == GL_NO_ERROR);
    assert(tex_of(t2d)->sp.minf == GL_NEAREST && tex_of(t2d)->sp.ws == GL_CLAMP_TO_EDGE && tex_of(t2d)->sp.border[0] == 1 && tex_of(t2d)->swz[0] == GL_BLUE && tex_of(t2d)->swz[3] == GL_ONE);
    assert(tex_of(st[0])->sp.minf == GL_NEAREST_MIPMAP_LINEAR && tex_of(st[0])->sp.ws == GL_REPEAT && tex_of(st[0])->swz[0] == GL_RED);

    // 2D immutable storage + sub-image uploads across mip levels
    tex_storage2d(t2d, 2, GL_RGBA8, 4, 4);
    assert(h_glGetError() == GL_NO_ERROR && tex_of(t2d)->slevels == 2 && tex_of(t2d)->w == 4 && tex_of(t2d)->h == 4);
    const uint8_t sub2[16] = {1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255};
    const uint8_t mip1[16] = {21, 22, 23, 255, 24, 25, 26, 255, 27, 28, 29, 255, 30, 31, 32, 255};
    tex_sub2d(t2d, 0, 1, 1, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, sub2);
    tex_sub2d(t2d, 1, 0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, mip1);
    tex_sub2d(t2d, 0, 0, 0, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);   // empty rect is a valid no-op
    assert(h_glGetError() == GL_NO_ERROR && tex_of(t2d)->made && tex_of(t2d)->tex.mipmapLevelCount == 2);
    uint8_t got[64];
    [tex_of(t2d)->tex getBytes:got bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
    assert(!memcmp(got + 1 * 16 + 4, sub2, 8) && !memcmp(got + 2 * 16 + 4, sub2 + 8, 8));
    [tex_of(t2d)->tex getBytes:got bytesPerRow:8 fromRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:1];
    assert(!memcmp(got, mip1, sizeof mip1));

    // Compressed 2D immutable storage + glCompressedTextureSubImage2D (including partial edge mip)
    tex_storage2d(tbc, 3, GL_COMPRESSED_RGBA_BPTC_UNORM, 8, 8);
    assert(h_glGetError() == GL_NO_ERROR);
    uint8_t blk[16]; memset(blk, 0x6c, sizeof blk);
    uint8_t small_blk[16]; memset(small_blk, 0x93, sizeof small_blk);
    comp_sub2d(tbc, 0, 4, 0, 4, 4, GL_COMPRESSED_RGBA_BPTC_UNORM, sizeof blk, blk);
    comp_sub2d(tbc, 2, 0, 0, 2, 2, GL_COMPRESSED_RGBA_BPTC_UNORM, sizeof small_blk, small_blk);
    assert(h_glGetError() == GL_NO_ERROR && tex_of(tbc)->made && tex_of(tbc)->tex.mipmapLevelCount == 3);
    [tex_of(tbc)->tex getBytes:got bytesPerRow:32 fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0];
    assert(!memcmp(got + 16, blk, sizeof blk));
    [tex_of(tbc)->tex getBytes:got bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:2];
    assert(!memcmp(got, small_blk, sizeof small_blk));

    // Cube map storage works for square dimensions and rejects non-square
    tex_storage2d(tcube, 2, GL_RGBA8, 4, 4);
    assert(h_glGetError() == GL_NO_ERROR && tex_of(tcube)->slevels == 2);
    tex_storage2d(tcube2, 1, GL_RGBA8, 4, 2);
    assert(h_glGetError() == GL_INVALID_VALUE);

    // Texture DSA validation
    GLuint bad = 0;
    create_texs(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 1, &bad); assert(h_glGetError() == GL_INVALID_ENUM);
    create_texs(0x1234, 1, &bad); assert(h_glGetError() == GL_INVALID_ENUM);
    create_texs(GL_TEXTURE_2D, -1, &bad); assert(h_glGetError() == GL_INVALID_VALUE);
    create_texs(0x84f5 /* GL_TEXTURE_RECTANGLE */, 1, &bad); assert(h_glGetError() == GL_NO_ERROR);   // host gap: RED1, no GL error
    h_glGenTextures(1, &gen_t);
    tex_storage2d(gen_t, 1, GL_RGBA8, 4, 4); assert(h_glGetError() == GL_INVALID_OPERATION);
    tex_sub2d(gen_t, 0, 0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, sub2); assert(h_glGetError() == GL_INVALID_OPERATION);
    comp_sub2d(gen_t, 0, 0, 0, 4, 4, GL_COMPRESSED_RGBA_BPTC_UNORM, sizeof blk, blk); assert(h_glGetError() == GL_INVALID_OPERATION);
    tex_parami(gen_t, GL_TEXTURE_MIN_FILTER, GL_NEAREST); assert(h_glGetError() == GL_INVALID_OPERATION);
    tex_paramfv(gen_t, GL_TEXTURE_BORDER_COLOR, border); assert(h_glGetError() == GL_INVALID_OPERATION);
    tex_storage2d(t3d, 1, GL_RGBA8, 4, 4); assert(h_glGetError() == GL_INVALID_OPERATION);
    tex_storage2d(tbuf, 1, GL_RGBA8, 4, 4); assert(h_glGetError() == GL_INVALID_OPERATION);
    tex_storage2d(t2d, 1, GL_RGBA8, 4, 4); assert(h_glGetError() == GL_INVALID_OPERATION);   // already immutable
    tex_storage2d(tcube2, 0, GL_RGBA8, 4, 4); assert(h_glGetError() == GL_INVALID_VALUE);
    tex_storage2d(tcube2, 1, GL_RGBA8, 0, 4); assert(h_glGetError() == GL_INVALID_VALUE);
    tex_storage2d(tcube2, 4, GL_RGBA8, 4, 4); assert(h_glGetError() == GL_INVALID_OPERATION);   // 4x4 has at most 3 levels
    tex_storage2d(tcube2, 1, GL_RGBA, 4, 4); assert(h_glGetError() == GL_INVALID_ENUM);         // unsized internalformat
    tex_sub2d(0, 0, 0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, sub2); assert(h_glGetError() == GL_INVALID_OPERATION);
    tex_sub2d(tcube, 0, 0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, sub2); assert(h_glGetError() == GL_INVALID_OPERATION);
    tex_sub2d(t3d, 0, 0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, sub2); assert(h_glGetError() == GL_INVALID_OPERATION);
    tex_sub2d(tbc, 0, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, sub2); assert(h_glGetError() == GL_INVALID_OPERATION);   // compressed texture
    tex_sub2d(t2d, -1, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sub2); assert(h_glGetError() == GL_INVALID_VALUE);
    tex_sub2d(t2d, 2, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sub2); assert(h_glGetError() == GL_INVALID_OPERATION);
    tex_sub2d(t2d, 0, 3, 0, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, sub2); assert(h_glGetError() == GL_INVALID_VALUE);
    comp_sub2d(0, 0, 0, 0, 4, 4, GL_COMPRESSED_RGBA_BPTC_UNORM, sizeof blk, blk); assert(h_glGetError() == GL_INVALID_OPERATION);
    comp_sub2d(tcube, 0, 0, 0, 4, 4, GL_COMPRESSED_RGBA_BPTC_UNORM, sizeof blk, blk); assert(h_glGetError() == GL_INVALID_OPERATION);
    comp_sub2d(t3d, 0, 0, 0, 4, 4, GL_COMPRESSED_RGBA_BPTC_UNORM, sizeof blk, blk); assert(h_glGetError() == GL_INVALID_OPERATION);
    comp_sub2d(tbc, 0, 0, 0, 4, 4, GL_RGBA8, 64, got); assert(h_glGetError() == GL_INVALID_ENUM);
    comp_sub2d(tbc, 0, 0, 0, 4, 4, GL_COMPRESSED_RGBA_BPTC_UNORM, 15, blk); assert(h_glGetError() == GL_INVALID_VALUE);
    comp_sub2d(tbc, 0, 1, 0, 4, 4, GL_COMPRESSED_RGBA_BPTC_UNORM, sizeof blk, blk); assert(h_glGetError() == GL_INVALID_OPERATION);
    tex_parami(tbuf, GL_TEXTURE_MIN_FILTER, GL_NEAREST); assert(h_glGetError() == GL_INVALID_OPERATION);
    tex_paramfv(tbuf, GL_TEXTURE_BORDER_COLOR, border); assert(h_glGetError() == GL_INVALID_OPERATION);

    // Framebuffers: glCreateFramebuffers, glNamedFramebufferTexture, glCheckNamedFramebufferStatus
    GLuint fb = 0, gen_fb = 0;
    create_fbs(1, &fb);
    assert(fb && h_glIsFramebuffer(fb) && h_glGetError() == GL_NO_ERROR);
    named_fb_tex(fb, GL_COLOR_ATTACHMENT0, t2d, 1);
    assert(h_glGetError() == GL_NO_ERROR && fb_of(fb)->col[0].kind == 1 && fb_of(fb)->col[0].name == t2d && fb_of(fb)->col[0].level == 1);
    assert(check_named_fb(fb, GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    assert(check_named_fb(fb, GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    assert(check_named_fb(fb, GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    assert(check_named_fb(0, GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    assert(h_glGetError() == GL_NO_ERROR);
    named_fb_tex(fb, GL_COLOR_ATTACHMENT0, 0, -99);   // detaching ignores level
    assert(h_glGetError() == GL_NO_ERROR && !fb_of(fb)->col[0].kind && !fb_of(fb)->col[0].name);
    // Framebuffer DSA validation
    create_fbs(-1, &gen_fb); assert(h_glGetError() == GL_INVALID_VALUE);
    h_glGenFramebuffers(1, &gen_fb);
    assert(!h_glIsFramebuffer(gen_fb));
    named_fb_tex(gen_fb, GL_COLOR_ATTACHMENT0, t2d, 0); assert(h_glGetError() == GL_INVALID_OPERATION);
    assert(check_named_fb(gen_fb, GL_FRAMEBUFFER) == 0 && h_glGetError() == GL_INVALID_OPERATION);
    named_fb_tex(0, GL_COLOR_ATTACHMENT0, t2d, 0); assert(h_glGetError() == GL_INVALID_OPERATION);
    named_fb_tex(fb, GL_COLOR_ATTACHMENT0, 999999, 0); assert(h_glGetError() == GL_INVALID_VALUE);
    named_fb_tex(fb, GL_COLOR_ATTACHMENT0, gen_t, 0); assert(h_glGetError() == GL_INVALID_VALUE);
    named_fb_tex(fb, GL_COLOR_ATTACHMENT0, tbuf, 0); assert(h_glGetError() == GL_INVALID_OPERATION);
    named_fb_tex(fb, GL_COLOR_ATTACHMENT0, t2d, -1); assert(h_glGetError() == GL_INVALID_VALUE);
    named_fb_tex(fb, 0x1234, t2d, 0); assert(h_glGetError() == GL_INVALID_ENUM);
    assert(check_named_fb(fb, 0x1234) == 0 && h_glGetError() == GL_INVALID_ENUM);
    assert(check_named_fb(999999, GL_FRAMEBUFFER) == 0 && h_glGetError() == GL_INVALID_OPERATION);

    // Every pre-existing context binding stayed untouched across all DSA calls
    GLint cur = 0;
    h_glGetIntegerv(0x8894, &cur); assert(cur == (GLint)sb[0]);
    h_glGetIntegerv(0x8895, &cur); assert(cur == (GLint)sb[1]);
    h_glGetIntegerv(0x8a28, &cur); assert(cur == (GLint)sb[2]);
    assert(S.texture_buf.name == sb[3]);
    h_glGetIntegerv(0x84e0, &cur); assert(cur == GL_TEXTURE0 + 3);
    h_glGetIntegerv(0x8069, &cur); assert(cur == (GLint)st[0]);
    h_glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &cur); assert(cur == (GLint)st[1]);
    h_glGetIntegerv(GL_TEXTURE_BINDING_3D, &cur); assert(cur == (GLint)st[2]);
    h_glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &cur); assert(cur == (GLint)st[3]);
    h_glGetIntegerv(GL_TEXTURE_BINDING_BUFFER, &cur); assert(cur == (GLint)st[4]);
    h_glGetIntegerv(GL_FRAMEBUFFER_BINDING, &cur); assert(cur == (GLint)sfb[0]);
    h_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &cur); assert(cur == (GLint)sfb[1]);
    assert(h_glGetError() == GL_NO_ERROR);

    // Cleanup
    h_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    h_glDeleteFramebuffers(2, sfb); h_glDeleteFramebuffers(1, &fb); h_glDeleteFramebuffers(1, &gen_fb);
    h_glDeleteTextures(5, st); h_glDeleteTextures(1, &t2d); h_glDeleteTextures(1, &tbc); h_glDeleteTextures(1, &tcube);
    h_glDeleteTextures(1, &tcube2); h_glDeleteTextures(1, &t3d); h_glDeleteTextures(1, &tbuf); h_glDeleteTextures(1, &gen_t);
    h_glActiveTexture(GL_TEXTURE0);
    h_glDeleteBuffers(4, sb); h_glDeleteBuffers(1, &b); h_glDeleteBuffers(1, &gen_b); h_glDeleteBuffers(1, &imm_b);
    h_glBindVertexArray(0); h_glDeleteVertexArrays(1, &vao);
    puts("GL 4.5 direct state access: OK");
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--srgb")) return test_framebuffer_srgb();
    if (argc == 2 && !strcmp(argv[1], "--capture")) return test_capture_shaders();
    int stubs = 0, cpu = 0; const char *fixture = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--stubs")) stubs = 1;
        else if (!strcmp(argv[i], "--cpu")) cpu = 1;
        else if (!strcmp(argv[i], "--fixture") && i + 1 < argc) fixture = argv[++i];
        else { fprintf(stderr, "usage: %s [--cpu] [--stubs] [--fixture DIR] or %s --capture\n", argv[0], argv[0]); return 2; }
    }
    if (fixture) setenv("DM_MSL_DIR", fixture, 1);
    setenv("DM_GL_STUBS", stubs ? "1" : "0", 1);
    test_fixed_function_caps();
    test_rg_texture_uploads();
    test_sync_names();
    // The calls from the issue must bind without using a GL loader first.
    GLuint (*create)(GLenum) = import("glCreateShader");
    assert((void *)create == (void *)h_glCreateShader_ctx);
    GLuint sh = create(GL_VERTEX_SHADER); assert(sh);
    void (*source)(GLuint, GLsizei, const GLchar *const *, const GLint *) = import("glShaderSource");
    const GLchar *text = "void main() {}";
    source(sh, 1, &text, NULL);
    void (*getiv)(GLuint, GLenum, GLint *) = import("glGetShaderiv");
    GLint value = 0; getiv(sh, GL_SHADER_TYPE, &value); assert(value == GL_VERTEX_SHADER);
    getiv(sh, GL_SHADER_SOURCE_LENGTH, &value); assert(value == strlen(text) + 1);
    void (*getsrc)(GLuint, GLsizei, GLsizei *, GLchar *) = import("glGetShaderSource");
    GLsizei slen = -1; GLchar sbuf[32] = {0};
    getsrc(sh, sizeof sbuf, &slen, sbuf); assert(slen == (GLsizei)strlen(text) && !strcmp(sbuf, text));
    GLchar small[5] = {'x', 'x', 'x', 'x', 'x'}; slen = -1;
    getsrc(sh, sizeof small, &slen, small); assert(slen == 4 && !strcmp(small, "void"));
    getsrc(sh, sizeof small, NULL, small); assert(!strcmp(small, "void"));
    slen = 99; getsrc(sh, sizeof sbuf, &slen, NULL); assert(slen == 0);
    slen = 99; small[0] = 'z'; getsrc(sh, 0, &slen, small); assert(slen == 0 && small[0] == 'z');
    GLuint empty = create(GL_FRAGMENT_SHADER);
    getiv(empty, GL_SHADER_SOURCE_LENGTH, &value); assert(value == 0);
    slen = 99; sbuf[0] = 'z'; getsrc(empty, sizeof sbuf, &slen, sbuf); assert(slen == 0 && sbuf[0] == 0);
    h_glDeleteShader(empty);
    assert(h_glGetError() == GL_NO_ERROR);
    slen = 7; getsrc(sh, -1, &slen, sbuf); assert(h_glGetError() == GL_INVALID_VALUE && slen == 7);
    getsrc(999999, sizeof sbuf, &slen, sbuf); assert(h_glGetError() == GL_INVALID_VALUE && slen == 7);
    GLuint badpr = h_glCreateProgram();
    getsrc(badpr, sizeof sbuf, &slen, sbuf); assert(h_glGetError() == GL_INVALID_OPERATION && slen == 7);
    h_glDeleteProgram(badpr);
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
    if (!cpu) test_literal_stride_fetch();
    test_uniform_buffer();
    test_compressed_arrays();
    test_dsa();
    test_doom3();
    test_packed_uploads();
    if (!cpu && test_framebuffer_srgb()) return 1;
    if (!cpu) test_lod_bias();
    test_sampler_objects(!cpu);
    test_extension_names_and_egl();
    test_egl_pbuffer_lifetime();
    test_egl_client_api();
    test_shared_buffer_lifetime();
    if (!cpu) { test_shared_contexts(); test_nvn_interop(); }
    test_program_validation();
    test_active_attributes();
    test_uniform_block_reflection();
    test_fate_calls();
    test_fixed_function(!cpu);
    if (!cpu) {
        test_image_copies(); test_multiple_draw_buffers(); test_texture_buffer_storage(); test_timestamps(); test_swizzle_and_layout_binding();
        int gl_ticks0 = atomic_load(&g_gl_frame_ticks);
        assert(h_eglSwapBuffers(NULL, NULL) == EGL_TRUE && h_eglSwapBuffers(NULL, NULL) == EGL_TRUE);
        assert(atomic_load(&g_gl_frame_ticks) == gl_ticks0 + 2);
    }
    if (fixture) test_argument_buffers(fixture);

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
    assert((h_eglGetProcAddress("eglNoSuchFunctionForTest") != NULL) == stubs);   // an EGL name the layer lacks: as a GL one
    const char *missing = "glMissingFunctionForTest";
    assert(!hle_implements(missing));
    assert((h_eglGetProcAddress(missing) != NULL) == stubs);
    assert(resolve_import(NULL, missing) == (uint64_t)&unimpl_stubs[0]);
    assert(!strcmp(wrap_kind, "STUB"));
    printf("GL imports: %u shared entries, shader handle/source, shader and program lifetimes, the error flag, a depth texture's data, vertex formats and bindings, the uniform buffer target, compressed 2D arrays, DOOM 3's calls (ARB names, extensions, named blocks, two-sided stencil, frame copies, compressed and byte-swapped images, vertex-only programs), sampler objects and uniform locations, extension names and EGL's current binding, Fate's program validation, line width and texture clears, fixed-function capability diagnostics, wrappers and missing names (DM_GL_STUBS=%d): OK\n", count, stubs);
    printf("GL rendering (stride 0, texture LOD bias, sampler objects): %s\n", cpu ? "not run (--cpu)" : "OK");
    return 0;
}
