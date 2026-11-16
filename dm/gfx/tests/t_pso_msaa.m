// A pipeline drawn in a multisampled pass carries the pass's sample count, through the production pass setup, pipeline lookup and Metal 4 base specialization.
// newRenderPipelineDescriptorForSpecialization returns its fixed state at the defaults (1 sample, alpha to coverage off, no input primitive class, amplification count 1): specialized from it as it came, the pipeline
// reads as 1-sample, and Metal's API validation (MTL_DEBUG_LAYER=1, which the Makefile check sets) stops at its bind in a 4x pass. PRAGMATA's 4x depth-only passes did.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static tex_t texture(int format, int samples) {
    static uint64_t serial = 990000;
    serial++;
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = serial, .off = (int64_t)serial << 20};
    t.d = (nvn_texdesc){.target = samples > 1 ? NVN_TEXTURE_TARGET_2D_MULTISAMPLE : NVN_TEXTURE_TARGET_2D, .format = format, .width = 8, .height = 8, .depth = 1, .levels = 1, .samples = samples};
    return t;
}
static NvProg *program(id<MTLLibrary> lib) {
    NvProg *p = [NvProg new];
    p->sh[0] = [NvShader new]; p->sh[1] = [NvShader new];
    strcpy(p->sh[0]->key, "msaa-vs"); strcpy(p->sh[1]->key, "msaa-fs"); p->sh[1]->stage = 1;
    p->sh[0]->fn = fn4_note([lib newFunctionWithName:@"v"], lib, @"v", nil);
    p->sh[1]->fn = fn4_note([lib newFunctionWithName:@"f"], lib, @"f", nil);
    p->sh[1]->rf.color_known = 1; p->sh[1]->rf.color_mask = 1;
    return p;
}
// What the pipeline says of itself. Only Metal's debug layer answers (its pipeline wrapper keeps the descriptor the pipeline was made from); 0 without it
static uint32_t pso_samples(id ps) { @try { return [[ps valueForKey:@"rasterSampleCount"] unsignedIntValue]; } @catch (id x) { return 0; } }
static long pso_a2c(id ps) {
    @try { id d = [ps valueForKey:@"mtl4Descriptor"]; return [d isKindOfClass:MTL4RenderPipelineDescriptor.class] ? (long)((MTL4RenderPipelineDescriptor *)d).alphaToCoverageState : -1; }
    @catch (id x) { return -1; }
}

static void pass(NvProg *p, int samples, int color, int a2c, const char *what) {
    tex_t c = texture(NVN_FORMAT_RGBA8, samples), ds = texture(NVN_FORMAT_DEPTH32F, samples);
    state_reset(); memset(&R, 0, sizeof R);
    S.ds = (ds_st){.dt = 1, .dw = 1, .df = NVN_DEPTH_FUNC_ALWAYS, .sf_f = NVN_STENCIL_FUNC_ALWAYS, .sf_b = NVN_STENCIL_FUNC_ALWAYS};
    S.ms.a2c = (uint8_t)a2c;
    R.has_rt = R.has_sc = 1; R.sc = (p_scissor){0,0,8,8}; R.rt.n = color; R.rt.depth = &ds; R.clear_d = 1; R.cd = 1;
    if (color) { R.rt.color[0] = &c; R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(0,0,0,1); }
    mtl_sub_begin(); id<MTL4RenderCommandEncoder> e = rp_begin_ex(0);
    CHECK(e != nil, what);
    CHECK(g_pass.samples == (uint32_t)samples, "the pass takes its attachments' sample count");
    uint64_t spec = g_pb_spec;
    id<MTLRenderPipelineState> ps = pso_get_(p, nil);
    CHECK(ps != nil, "the pipeline builds");
    if (g_comp4) CHECK(g_pb_spec == spec + 1, "the pipeline is specialized from a Metal 4 base (the path under test)");
    uint32_t n = pso_samples(ps);
    if (!n) printf("SKIP: %s: the pipeline does not say its sample count (no debug layer: run with MTL_DEBUG_LAYER=1)\n", what);
    if (n) CHECK(n == (uint32_t)samples, "the pipeline reads as the pass's sample count");
    long a = pso_a2c(ps);
    if (a < 0 && g_comp4) printf("SKIP: %s: the pipeline does not say its alpha to coverage (no debug layer, or its descriptor key is gone)\n", what);
    if (a >= 0) CHECK(a == (a2c ? MTL4AlphaToCoverageStateEnabled : MTL4AlphaToCoverageStateDisabled), "the pipeline keeps the draw's alpha to coverage");
    // NOLINTBEGIN(clang-analyzer-osx.cocoa.RetainCount): dss_get retains the state in g_dss through hm_put_id's CFBridgingRetain
    if (e && ps) {   // with MTL_DEBUG_LAYER=1 a sample count mismatch aborts here
        enc_r_pso(e, ps); enc_r_dss(e, dss_get(g_pass.hd, g_pass.hs));
        enc_r_viewport(e, (MTLViewport){0,0,8,8,0,1}); enc_r_scissor(e, (MTLScissorRect){0,0,8,8});
        enc_r_cull(e, MTLCullModeNone); enc_r_draw(e, MTLPrimitiveTypeTriangle, 0, 3);
    }
    // NOLINTEND(clang-analyzer-osx.cocoa.RetainCount)
    mtl_sub_commit(); mtl_drain(); CHECK(!mtl_last_commit_error(), "the draw completes without Metal errors");
    mtl_tex_release(&ds); if (color) mtl_tex_release(&c);
}

// The rest of the base's fixed state: a layered pass's input primitive class and a slice-merged pass's amplification count (AMP_MARK), as pso_desc sets them, through pso_build
static void fixed_state(NvProg *p) {
    if (!g_comp4) return;   // the MTLDevice builds pipelines whole: nothing is specialized
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = p->sh[0]->fn; d.fragmentFunction = p->sh[1]->fn;
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm; d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    d.rasterSampleCount = 4; d.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle; d.maxVertexAmplificationCount = 2;
    uint64_t spec = g_pb_spec; NSError *e = nil;
    id<MTLRenderPipelineState> ps = pso_build(d, 0, &e);
    CHECK(ps != nil, "a layered, amplified 4x pipeline builds");
    CHECK(g_pb_spec == spec + 1, "the layered, amplified pipeline is specialized from a base");
    id m = nil; @try { m = [(id)ps valueForKey:@"mtl4Descriptor"]; } @catch (id x) { m = nil; }
    if (![m isKindOfClass:MTL4RenderPipelineDescriptor.class]) { printf("SKIP: the layered, amplified pipeline does not say its descriptor (no debug layer: run with MTL_DEBUG_LAYER=1)\n"); return; }
    MTL4RenderPipelineDescriptor *s = m;
    CHECK(s.rasterSampleCount == 4, "the specialization keeps its base's sample count");
    CHECK(s.inputPrimitiveTopology == MTLPrimitiveTopologyClassTriangle, "the specialization keeps its base's input primitive class");
    CHECK(s.maxVertexAmplificationCount == 2, "the specialization keeps its base's amplification count");
}

int main(void) { @autoreleasepool {
    setenv("DM_PSO_ARCHIVE", "off", 1); setenv("DM_PSO_MANIFEST", "off", 1); setenv("DM_PSO_USER", "/dev/null", 1);
    setenv("DM_PSO_PREBUILD", "0", 1); setenv("DM_PSO_HYBRID", "0", 1); setenv("DM_NVN_PREDICT", "0", 1);
    mtl_init(); NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint n[[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[n],.25,1); }\n"
        "fragment float4 f() { return float4(1,0,0,.5); }\n" options:nil error:&error];
    CHECK(lib != nil, "synthetic shaders compile"); if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    NvProg *p = program(lib);
    pass(p, 4, 0, 0, "a 4x depth-only pass opens (PRAGMATA's case)");
    pass(p, 4, 1, 0, "a 4x colour + depth pass opens");
    pass(p, 4, 1, 1, "a 4x colour + depth pass with alpha to coverage opens");
    pass(p, 1, 1, 0, "a 1x pass opens");
    fixed_state(p);
    int debug = getenv("MTL_DEBUG_LAYER") && atoi(getenv("MTL_DEBUG_LAYER"));
    printf("multisampled pipelines: %d checks, %d failed (%s%s)\n", checks, failed, g_comp4 ? "Metal 4" : "MTLDevice", debug ? ", API validation" : ", no API validation: the binds are not checked");
    return failed != 0;
} }
