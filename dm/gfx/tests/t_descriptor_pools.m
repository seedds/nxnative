// A recorded descriptor index is resolved against the execution pool, including an inherited pool and a pool changed after a bind. Synthetic NVN commands and 3D images only.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
#include <unistd.h>
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static void apply(NVNcommandHandle h) {
    size_t n; uint8_t *p = nvn_rec_linear(h, &n);
    for (size_t at = 0; at < n;) { op_hdr op; memcpy(&op, p + at, 8); if (op.op == OP_END) break; CHECK(state_op(op.op, p + at + 8), "recorded binding replays"); at += op.size; }
    free(p);
}
static unsigned pixel(tex_t *t) {
    id<MTLBuffer> b = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    CHECK(b != nil, "image readback allocated"); if (!b) return 0;
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:tex_mtl(t) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(1,1,1)
        toBuffer:b destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256];
    [bl endEncoding]; [cb commit]; [cb waitUntilCompleted]; CHECK(cb.status == MTLCommandBufferStatusCompleted, "image readback completes");
    return ((const uint8_t *)b.contents)[0];
}
int main(void) { @autoreleasepool {
    alarm(30); mtl_init();
    NVNtexturePool tp[2]; NVNsamplerPool sp[2];
    for (int i = 0; i < 2; i++) { h_nvnTexturePoolInitialize(&tp[i], NULL, 0, 256); h_nvnSamplerPoolInitialize(&sp[i], NULL, 0, 256); }
    NVNcommandBuffer cb; h_nvnCommandBufferInitialize(&cb, NULL);
    h_nvnCommandBufferBeginRecording(&cb);
    h_nvnCommandBufferBindImage(&cb, NVN_SHADER_STAGE_COMPUTE, 0, 3);
    h_nvnCommandBufferBindTexture(&cb, NVN_SHADER_STAGE_FRAGMENT, 0, 5u << 20 | 3u);
    NVNseparateSamplerHandle sam = {.value = 5u << 20}; h_nvnCommandBufferBindSeparateSamplers(&cb, NVN_SHADER_STAGE_FRAGMENT, 0, 1, &sam);
    uint64_t deferred = 5u << 20 | 3u; h_nvnCommandBufferBindTexturesDeferred(&cb, NVN_SHADER_STAGE_VERTEX, 0, 1, &deferred);
    NVNcommandHandle h = h_nvnCommandBufferEndRecording(&cb); deferred = 6u << 20 | 4u;
    state_reset(); p_set_pool pt = {.pool = (dpool_t *)&tp[1]}, ps = {.pool = (dpool_t *)&sp[1]};
    state_op(OP_SET_TEX_POOL, &pt); state_op(OP_SET_SAMP_POOL, &ps); apply(h);
    CHECK(S.img[5][0] == 3 && S.tex[1][0] == (5u << 20 | 3u), "binding state retains hardware indices");
    CHECK((bound_img(5, 0) & 0xfffff) == 259 && bound_tex(1, 0) == (261u << 20 | 259u), "a list inherits the execution pools");
    CHECK(bound_tex(0, 0) == (262u << 20 | 260u), "deferred handles read their new value in the inherited pools");
    nvn_rbind samp = {.kind = NVN_RB_SAMP, .reg = 0}; CHECK(bind_sid(1, &samp) == 261, "separate samplers use the execution pool");
    samp = (nvn_rbind){.kind = NVN_RB_SAMPI, .reg = 5}; CHECK(bind_sid(1, &samp) == 261, "constant sampler IDs use the execution pool");
    pt.pool = (dpool_t *)&tp[0]; ps.pool = (dpool_t *)&sp[0]; state_op(OP_SET_TEX_POOL, &pt); state_op(OP_SET_SAMP_POOL, &ps);
    CHECK(bound_tex(1, 0) == (5u << 20 | 3u) && (bound_img(5, 0) & 0xfffff) == 3, "changing pools resolves existing bindings again");
    CHECK(bound_handle(0) == 0 && bound_handle(0x100000003ull) == 3, "zero remains unbound and the GPU ignores the upper handle word");
    uint8_t *images = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(images != MAP_FAILED, "distinct image storage allocated"); if (images == MAP_FAILED) return 1; memset(images, 0, 65536); mtl_pool_add(images, 65536);
    tex_t tex[2];
    for (int i = 0; i < 2; i++) { tex[i] = (tex_t){.magic = MAGIC('T','E','X',0), .serial = nvn_new_serial(), .mem = images + i * 16384, .off = i * 16384};
        tex[i].d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_3D, .format = NVN_FORMAT_RGBA8, .flags = NVN_TEXTURE_FLAGS_IMAGE, .width = 4, .height = 4, .depth = 4, .levels = 1, .samples = 1};
        nvn_tex_layout(&tex[i].d, &tex[i].L); mtl_tex_init(&tex[i]); mtl_reg_image(3 + 256 * i, &tex[i], NULL); }
    NSError *error = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nstruct A { texture3d<float,access::write> t [[id(0)]]; };\nkernel void k(constant A&a [[buffer(0)]],constant float&v [[buffer(1)]]) { a.t.write(float4(v),uint3(0)); }" options:nil error:&error];
    CHECK(lib != nil, "image kernel compiles"); if (!lib) return 1;
    NvShader *sh = [NvShader new]; sh->stage = 5; strcpy(sh->key, "0123456789abcdef_b"); sh->fn = [lib newFunctionWithName:@"k"]; sh->ae = [sh->fn newArgumentEncoderWithBufferIndex:0]; sh->arg_len = sh->ae.encodedLength;
    char message[256]; CHECK(!nvn_refl_from_json(@{@"route":@"B", @"bindings":@[@{@"kind":@"img", @"reg":@0, @"arg_id":@0, @"msl_type":@"texture3d<float, access::write>"}]}, &sh->rf, message, sizeof message), "production image reflection converts");
    id<MTLComputePipelineState> pipeline = [g_dev newComputePipelineStateWithFunction:sh->fn error:&error]; CHECK(pipeline != nil, "image pipeline compiles"); if (!pipeline) return 1;
    NvProg *program = [NvProg new]; program->sh[5] = sh;
    id<MTLBuffer> values = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared]; ((float *)values.contents)[0] = 0.25f; ((float *)values.contents)[1] = 0.75f; res_add(values);
    mtl_sub_begin();
    for (int i = 0; i < 2; i++) { pt.pool = (dpool_t *)&tp[i]; state_op(OP_SET_TEX_POOL, &pt);
        image_prepare(program, 0x20); encode_args(nil, sh, 5, 0);
        id<MTL4ComputeCommandEncoder> en = dispatch_enc(); enc_c_pso(en, pipeline); CHECK(encode_args((id<MTLCommandEncoder>)en, sh, 5, 0), "production image binding encodes");
        enc_c_args(en, values.gpuAddress + 4 * i, 1); enc_c_dispatch(en, MTLSizeMake(1,1,1), MTLSizeMake(1,1,1)); }
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "image dispatches complete");
    unsigned a = pixel(&tex[0]), b = pixel(&tex[1]);
    if (a != 64 || b != 191) fprintf(stderr, "image pixels: %u, %u\n", a, b);
    CHECK(a == 64 && b == 191, "the same image slot writes distinct 3D images after a pool change");
    mtl_pool_del(images); h_nvnCommandBufferFinalize(&cb); printf("t_descriptor_pools: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
