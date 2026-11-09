// A draw that writes the depth attachment while a fragment texture slot names that depth draws into a copy (g_dswap in nvn_mtl.m): fb_prepare asks for it, the pass the draw
// opens attaches the copy, and the pass end copies it back, so the depth the draw wrote reaches the attachment. A draw that writes depth with no read of it, and one that reads it
// with no depth write, keep the attachment. With DM_NVN_DEPTH_SWAP=0 no draw takes the copy, and the depth is written all the same.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static id<MTLTexture> make(tex_t *t, uint64_t serial, int format) {
    *t = (tex_t){.magic=MAGIC('T','E','X',0), .serial=serial};
    t->d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=format, .width=4, .height=4, .depth=1, .levels=1, .samples=1};
    MTLTextureDescriptor *d = tex_desc(t, 0);
    id<MTLTexture> texture = [g_dev newTextureWithDescriptor:d];
    CHECK(texture != nil, "texture created");
    res_add(texture); mtl_tex_init(t); serial_put(serial, texture);
    return texture;
}
// The depth of every texel, read back after the submission
static int depth_is(id<MTLTexture> depth, float want, const char *label) {
    id<MTLBuffer> b = [g_dev newBufferWithLength:1024 options:MTLResourceStorageModeShared]; res_add(b);
    enc_b_copy_texture_buffer(blit(), depth, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(4, 4, 1), b, 0, 256, 1024);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "GPU submission completed without error");
    int ok = 1; for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) ok &= ((const float *)((const uint8_t *)b.contents + y * 256))[x] == want;
    CHECK(ok, label);
    mtl_sub_begin();
    return ok;
}

int main(void) { @autoreleasepool {
    setenv("DM_NVN_TBDR", "0", 1); setenv("DM_NVN_GPU_TS", "segment", 1);
    const char *e = getenv("DM_NVN_DEPTH_SWAP"); int swap = !(e && !strcmp(e, "0"));
    mtl_init(); NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 p[[position]]; };\n"
        "vertex V v(uint i[[vertex_id]]) { V o; o.p = float4(i == 1 ? 3.0 : -1.0, i == 2 ? 3.0 : -1.0, 0.25, 1); return o; }\n"
        "fragment float4 f() { return float4(0, 1, 0, 1); }\n" options:nil error:&error];
    CHECK(lib != nil, "synthetic shader compiles");
    if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    MTLRenderPipelineDescriptor *pdesc = [MTLRenderPipelineDescriptor new];
    pdesc.vertexFunction = [lib newFunctionWithName:@"v"]; pdesc.fragmentFunction = [lib newFunctionWithName:@"f"];
    pdesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm; pdesc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    id<MTLRenderPipelineState> pipeline = [g_dev newRenderPipelineStateWithDescriptor:pdesc error:&error];
    CHECK(pipeline != nil, "depth-writing pipeline compiles");
    if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new]; dd.depthCompareFunction = MTLCompareFunctionLessEqual; dd.depthWriteEnabled = YES;
    id<MTLDepthStencilState> lequal = [g_dev newDepthStencilStateWithDescriptor:dd];

    NvProg *pr = [NvProg new]; NvShader *fs = [NvShader new]; pr->sh[1] = fs;
    fs->stage = 1; strcpy(fs->key, "0123456789abcdef");
    fs->rf.nb = 1; fs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_TEX, .reg=0, .aid=0, .dtt=MTLTextureType2D};
    tex_t color, depth; make(&color, 99901, NVN_FORMAT_RGBA8); id<MTLTexture> dtex = make(&depth, 99902, NVN_FORMAT_DEPTH32F);
    const int tid = 700; mtl_reg_tex(tid, &depth, NULL);

    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &color; R.rt.depth = &depth; S.chmask.mask = 0xffffffffu;
    R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(0, 0, 0, 1); R.clear_d = 1; R.cd = 1.0;
    CHECK(rp_begin() != nil, "the clearing pass opens"); rp_end();
    depth_is(dtex, 1.0f, "the depth starts cleared to 1");

    // Written and read through a texture slot: a pass of its own on a copy, and the depth comes back at its end
    S.ds.dt = 1; S.ds.dw = 1; S.ds.df = NVN_DEPTH_FUNC_LEQUAL; S.tex[1][0] = tid;
    fb_prepare(pr);
    CHECK(g_dswap_want == swap, swap ? "a draw reading the depth it writes asks for the copy" : "DM_NVN_DEPTH_SWAP=0: no copy asked for");
    CHECK(rp_begin() != nil, "the draw's pass opens");
    CHECK(g_dswap_on == swap && (!swap || (g_dswap_src == dtex && g_dswap_tex && g_dswap_tex != dtex)), swap ? "its pass attaches a copy of the depth" : "its pass attaches the depth itself");
    enc_r_pso(g_enc, pipeline); enc_r_dss(g_enc, lequal); enc_r_viewport(g_enc, (MTLViewport){0, 0, 4, 4, 0, 1});
    enc_r_draw(g_enc, MTLPrimitiveTypeTriangle, 0, 3);
    rp_end();
    CHECK(!g_dswap_on && !g_dswap_src, "the pass end hands the copy back");
    depth_is(dtex, 0.25f, "the depth the draw wrote reaches the attachment");

    // Written with no read of it, and read with no write: the attachment as it is
    S.tex[1][0] = 0; fb_prepare(pr);
    CHECK(!g_dswap_want, "a draw writing depth it does not read asks for no copy");
    S.tex[1][0] = tid; S.ds.dw = 0; fb_prepare(pr);
    CHECK(!g_dswap_want, "a draw reading depth it does not write asks for no copy");
    CHECK(rp_begin() != nil && !g_dswap_on, "its pass attaches the depth itself");
    rp_end();

    // A request no pass with a depth attachment takes is not left for a later pass
    S.ds.dw = 1; fb_prepare(pr); R.rt.depth = NULL;
    CHECK(rp_begin() != nil, "a pass without depth opens"); rp_end();
    R.rt.depth = &depth; S.tex[1][0] = 0; S.ds.dw = 0;
    CHECK(rp_begin() != nil && !g_dswap_on, "a later pass with the depth does not take the copy asked for before");
    rp_end();
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "last submission completed without error");
    mtl_tex_release(&color); mtl_tex_release(&depth);
    printf("depth swap (%s): %d checks, %d failed\n", swap ? "on" : "DM_NVN_DEPTH_SWAP=0", checks, failed);
    return failed != 0;
} }
