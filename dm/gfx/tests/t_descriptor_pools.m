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
static uint64_t sampler_argument(NvShader *sh) {
    CHECK(encode_args(nil, sh, 5, 0), "sampler argument encodes");
    return *(const uint64_t *)((const uint8_t *)sh->ab_buf.contents + sh->ab_off);
}
static void sampler_zero(const NVNsamplerPool sp[2], uint32_t *word) {
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct A { sampler s [[id(0)]]; };\n"
        "kernel void k(constant A&a [[buffer(0)]], texture2d<float> t [[texture(0)]], device float *out [[buffer(1)]]) {"
        "out[0] = t.sample(a.s, float2(0.5), level(0)).r;"
        "out[1] = t.sample(a.s, float2(-0.25, 0.5), level(0)).r; }" options:nil error:&error];
    CHECK(lib != nil, "sampler argument shader compiles"); if (!lib) return;
    NvShader *sh = [NvShader new]; sh->stage = 5; sh->fn = [lib newFunctionWithName:@"k"];
    sh->ae = [sh->fn newArgumentEncoderWithBufferIndex:0]; sh->arg_len = sh->ae.encodedLength;
    sh->rf.nb = 1; sh->rf.b[0] = (nvn_rbind){.kind = NVN_RB_SAMPT, .reg = 0, .aid = 0};
    uint64_t zero = 0, dummy = dummy_samp_rid();
    mtl_sub_begin();
    for (int i = 0; i < 2; i++) {
        p_set_pool p = {.pool = (const dpool_t *)&sp[i]};
        state_op(OP_SET_SAMP_POOL, &p);
        const uint32_t handles[] = {3, 0, 0xffffffffu, 255u << 20 | 3u};
        for (size_t k = 0; k < sizeof handles / sizeof *handles; k++) {
            p_bind_tex1 t = {.stage = 5, .slot = 0, .h = handles[k]};
            state_op(OP_BIND_TEX1, &t);
            uint64_t rid = sampler_argument(sh);
            if (!k) {
                CHECK(rid && rid != dummy, "slot zero on a populated handle has its own Metal sampler");
                CHECK(!i || rid == zero, "both pools provide the same slot-zero descriptor");
                zero = rid;
            } else CHECK(rid == dummy, "unbound and invalid handles keep the dummy");
        }
        __unsafe_unretained id<MTLBuffer> pool = nil;
        os_unfair_lock_lock(&g_rmu);
        pool_gpu(1, 0, 0, S.samp_base, &pool);
        os_unfair_lock_unlock(&g_rmu);
        const uint8_t *pbytes = pool.contents;
        const uint64_t *entries = (const uint64_t *)(pbytes + DMSH_POOL_HEAD);
        CHECK(entries[0] == zero, "both shader-indexed pools contain the reserved sampler");
        CHECK(entries[*(const uint32_t *)pbytes] == dummy, "pool table's out-of-range entry stays the dummy");
    }
    S.ubo[5][0] = (p_bind_buf){.addr = (uintptr_t)word, .size = sizeof *word};
    sh->rf.b[0].kind = NVN_RB_SAMPU;
    *word = 3;
    CHECK(sampler_argument(sh) == zero, "a combined uniform handle selects slot zero");
    *word = 0;
    CHECK(sampler_argument(sh) == dummy, "an empty uniform handle keeps the dummy");
    sh->rf.b[0].raw_id = 1;
    CHECK(sampler_argument(sh) == zero, "a readable sampler ID of zero selects slot zero");
    *word = 4096;
    CHECK(sampler_argument(sh) == dummy, "a raw sampler ID outside the handle field keeps the dummy");
    *word = 0;
    S.ubo[5][0].size = 0;
    CHECK(sampler_argument(sh) == dummy, "an unreadable sampler ID keeps the dummy");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "sampler argument submission completes");

    id<MTLComputePipelineState> pipe = [g_dev newComputePipelineStateWithFunction:sh->fn error:&error];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:2 height:1 mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [g_dev newTextureWithDescriptor:td];
    id<MTLBuffer> args = [g_dev newBufferWithLength:sh->arg_len options:MTLResourceStorageModeShared];
    id<MTLBuffer> out = [g_dev newBufferWithLength:2 * sizeof(float) options:MTLResourceStorageModeShared];
    CHECK(pipe && tex && args && out, "sampler readback resources allocated"); if (!pipe || !tex || !args || !out) return;
    const uint8_t pixels[2] = {0, 255};
    [tex replaceRegion:MTLRegionMake2D(0, 0, 2, 1) mipmapLevel:0 withBytes:pixels bytesPerRow:2];
    memcpy(args.contents, &zero, sizeof zero);
    id<MTLCommandBuffer> cb = q_cb();
    id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder];
    [en setComputePipelineState:pipe]; [en setTexture:tex atIndex:0];
    [en setBuffer:args offset:0 atIndex:0]; [en setBuffer:out offset:0 atIndex:1];
    [en dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK(cb.status == MTLCommandBufferStatusCompleted, "slot-zero sample completes on the GPU");
    const float *sample = out.contents;
    CHECK(fabsf(sample[0] - 0.5f) < 0.01f && sample[1] == 0, "slot zero interpolates texels and clamps outside the texture");
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
    sampler_zero(sp, (uint32_t *)(images + 32768));
    mtl_pool_del(images); h_nvnCommandBufferFinalize(&cb); printf("t_descriptor_pools: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
