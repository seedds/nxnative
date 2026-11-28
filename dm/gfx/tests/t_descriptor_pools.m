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
static void texture_zero(const NVNtexturePool tp[2], const NVNsamplerPool sp[2], uint32_t *word) {
    uint8_t *zmem = mmap(NULL, 32768, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(zmem != MAP_FAILED, "texture_zero backing allocated"); if (zmem == MAP_FAILED) return;
    memset(zmem, 0, 32768); mtl_pool_add(zmem, 32768);
    pool_t zp = {.magic = MAGIC('P','O','O','L'), .mem = zmem, .size = 32768, .flags = NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_reg_tex(0, NULL, NULL); mtl_reg_tex(256, NULL, NULL);
    state_reset(); p_set_pool pt = {.pool = (dpool_t *)&tp[0]}; state_op(OP_SET_TEX_POOL, &pt);
    CHECK(bound_tex_id(0) < 0 && ubo_tex_id(0) < 0, "handle 0 remains unbound when slot 0 is empty");
    CHECK(bound_tex_id(5u << 20 | 0u) < 0 && ubo_tex_id(0x3f800000u) < 0, "unregistered texture slot 0 remains unbound");
    pt.pool = (dpool_t *)&tp[1]; state_op(OP_SET_TEX_POOL, &pt);
    CHECK(bound_tex_id(5u << 20 | 0u) < 0 && ubo_tex_id(0x3f800000u) < 0, "unregistered texture slot 0 in pool 1 remains unbound");
    CHECK(h_ser(0) == 0 && h_ser(bound_handle(5u << 20 | 0u)) == 0, "unregistered slot 0 resolves to serial 0 through h_ser");
    p_set_pool ps1 = {.pool = (dpool_t *)&sp[1]}; state_op(OP_SET_SAMP_POOL, &ps1);
    uint64_t clamped = bound_handle(0xffffffffu);
    CHECK((clamped & 0xfffffu) == 0xfffffu && bound_samp_id(clamped) == 4095 + 256, "bound_handle clamps low 20 bits at 0xfffff without carrying into sampler bits");
    CHECK(bound_handle(1ull << 32) == 0 && bound_tex_id(bound_handle(1ull << 32)) < 0, "upper-word-only handle stays unbound");
    mtl_sub_begin(); __unsafe_unretained id<MTLBuffer> pb = nil;
    os_unfair_lock_lock(&g_rmu);
    uint64_t a0 = pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &pb); id<MTLBuffer> pb0 = pb;
    uint64_t a1 = pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 256, &pb); id<MTLBuffer> pb1 = pb;
    uint64_t a_oob = pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, g_ntreg + 256, &pb);
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "unregistered slot 0 pool tables commit");
    CHECK(a0 && a1 && a_oob && pb0 && pb1, "unregistered slot 0 and empty-tail pool tables exist");
    if (!pb0 || !pb1) { mtl_pool_del(zmem); munmap(zmem, 32768); return; }
    uint64_t d0 = ((const uint64_t *)((const uint8_t *)pb0.contents + DMSH_POOL_HEAD))[*(const uint32_t *)pb0.contents];
    uint64_t e0 = ((const uint64_t *)((const uint8_t *)pb0.contents + DMSH_POOL_HEAD))[0];
    uint64_t d1 = ((const uint64_t *)((const uint8_t *)pb1.contents + DMSH_POOL_HEAD))[*(const uint32_t *)pb1.contents];
    uint64_t e1 = ((const uint64_t *)((const uint8_t *)pb1.contents + DMSH_POOL_HEAD))[0];
    CHECK(e0 == d0 && e1 == d1, "unregistered pool slot 0 holds dummy resource");
    // Unregistered slot 0 in handle-table and RT-hazard consumers: null and sampler-only stay unbound.
    pt.pool = (dpool_t *)&tp[0]; state_op(OP_SET_TEX_POOL, &pt);
    memset(word, 0, 16); word[0] = 0; word[2] = 5u << 20 | 0u;
    S.ubo[5][0] = (p_bind_buf){.addr = (uint64_t)word, .size = 16};
    nvn_rbind htab_b = {.kind = NVN_RB_TEXH, .reg = 0, .off = 0, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT};
    mtl_sub_begin(); pb = nil;
    os_unfair_lock_lock(&g_rmu);
    uint64_t ha = htab_gpu(5, &htab_b, &pb); id<MTLBuffer> hb0 = pb;
    uint64_t hser0 = g_htab_last ? g_htab_last->ser[0] : 1, hser1 = g_htab_last ? g_htab_last->ser[1] : 1;
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "unregistered slot 0 htab commits");
    CHECK(ha && hb0 && hser0 == 0 && hser1 == 0, "unregistered slot 0 leaves null and sampler-only htab entries unbound");
    tex_t zt[2];
    for (int i = 0; i < 2; i++) {
        zt[i] = (tex_t){.magic = MAGIC('T','E','X',0), .has_layout = 1, .serial = nvn_new_serial(), .pool = &zp, .mem = zmem + 16384 * i, .off = 16384 * i};
        zt[i].d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = 4, .height = 4, .depth = 1, .levels = 1, .samples = 1};
        nvn_tex_layout(&zt[i].d, &zt[i].L);
        uint8_t *px = (uint8_t *)zt[i].mem;
        for (size_t k = 0; k + 3 < zt[i].L.size; k += 4) { px[k] = i ? 191 : 64; px[k + 1] = 0; px[k + 2] = 0; px[k + 3] = 255; }
        mtl_tex_init(&zt[i]); mtl_reg_tex(256 * i, &zt[i], NULL);
    }
    pt.pool = (dpool_t *)&tp[0]; state_op(OP_SET_TEX_POOL, &pt);
    CHECK(bound_tex_id(0) < 0 && ubo_tex_id(0) < 0 && bound_tex_id(bound_handle(1ull << 32)) < 0, "handle 0 and upper-word-only stay unbound after slot 0 is registered");
    CHECK(bound_tex_id(5u << 20 | 0u) == 0 && ubo_tex_id(0x3f800000u) == 0 && tid_ser(0) == zt[0].serial && h_ser(bound_handle(5u << 20 | 0u)) == zt[0].serial && h_ser(0) == 0, "pool 0 resolves registered slot 0");
    pt.pool = (dpool_t *)&tp[1]; state_op(OP_SET_TEX_POOL, &pt);
    CHECK(bound_tex_id(0) < 0 && ubo_tex_id(0) < 0, "handle 0 stays unbound in pool 1 after slot 0 is registered");
    CHECK(bound_tex_id(5u << 20 | 0u) == 256 && ubo_tex_id(0x3f800000u) == 256 && tid_ser(256) == zt[1].serial && h_ser(bound_handle(5u << 20 | 0u)) == zt[1].serial, "pool 1 resolves registered slot 0");
    mtl_sub_begin(); pb = nil;
    os_unfair_lock_lock(&g_rmu);
    pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &pb); pb0 = pb;
    pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 256, &pb); pb1 = pb;
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "registered slot 0 pool tables commit");
    CHECK(pb0 && pb1, "registered slot 0 pool tables exist");
    if (!pb0 || !pb1) { mtl_reg_tex(0, NULL, NULL); mtl_reg_tex(256, NULL, NULL); mtl_pool_del(zmem); munmap(zmem, 32768); return; }
    e0 = ((const uint64_t *)((const uint8_t *)pb0.contents + DMSH_POOL_HEAD))[0];
    e1 = ((const uint64_t *)((const uint8_t *)pb1.contents + DMSH_POOL_HEAD))[0];
    CHECK(e0 && e1 && e0 != d0 && e1 != d1 && e0 != e1, "registered pool slot 0 entries are distinct real textures");
    // Registered slot 0 in htab_gpu, draw_samples_rt, img_pool_gpu, and treg_res_check.
    pt.pool = (dpool_t *)&tp[0]; state_op(OP_SET_TEX_POOL, &pt);
    mtl_sub_begin(); pb = nil;
    os_unfair_lock_lock(&g_rmu);
    ha = htab_gpu(5, &htab_b, &pb); id<MTLBuffer> hb1 = pb;
    hser0 = g_htab_last ? g_htab_last->ser[0] : 1; hser1 = g_htab_last ? g_htab_last->ser[1] : 0;
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "registered slot 0 htab commits");
    CHECK(ha && hb1 && hser0 == 0 && hser1 == zt[0].serial, "htab_gpu keeps null entry 0 unbound and resolves slot-0 entry 1");
    NvShader *fsh = [NvShader new]; fsh->stage = 1; fsh->rf.nb = 1;
    fsh->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEX, .reg = 0, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT};
    NvProg *fprog = [NvProg new]; fprog->sh[1] = fsh;
    R.rt = (p_set_rt){.n = 1, .color = {&zt[0]}};
    S.tex[1][0] = 0; CHECK(!draw_samples_rt(fprog), "draw_samples_rt keeps null handle unbound when slot 0 is registered");
    S.tex[1][0] = 5u << 20 | 0u; CHECK(draw_samples_rt(fprog), "draw_samples_rt detects RT hazard on registered texture slot 0");
    S.tex[1][0] = 0; R.rt = (p_set_rt){0};
    { int prev_rc = g_res_check_on; g_res_check_on = 1; treg_res_check(); g_res_check_on = prev_rc; }
    mtl_reg_image(0, &zt[0], NULL);
    nvn_rbind ip_b = {.kind = NVN_RB_IMGP, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT, .rw = 0};
    mtl_sub_begin(); pb = nil;
    os_unfair_lock_lock(&g_rmu);
    uint64_t ipa = img_pool_gpu(&ip_b, 0, &pb); id<MTLBuffer> ipb = pb;
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "registered slot 0 img_pool commits");
    if (ipa && ipb) {
        uint64_t id0 = ((const uint64_t *)((const uint8_t *)ipb.contents + DMSH_POOL_HEAD))[*(const uint32_t *)ipb.contents];
        uint64_t ie0 = ((const uint64_t *)((const uint8_t *)ipb.contents + DMSH_POOL_HEAD))[0];
        CHECK(ie0 && ie0 != id0, "img_pool_gpu populates registered slot 0");
    } else CHECK(0, "img_pool_gpu returns a buffer for registered slot 0");
    mtl_reg_tex(0, &zt[0], NULL);
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct spvPool_tp { uint n; ulong seen; array<texture2d<float>, 1> e; };\n"
        "struct A { texture2d<float> t [[id(0)]]; texture2d<float> tu [[id(1)]]; constant spvPool_tp *tp [[id(2)]]; };\n"
        "kernel void k0(constant A &a [[buffer(0)]], device uint4 *out [[buffer(1)]]) {\n"
        "    uint r_t  = (uint)(a.t.read(uint2(0)).x * 255.0f + 0.5f);\n"
        "    uint r_tu = (uint)(a.tu.read(uint2(0)).x * 255.0f + 0.5f);\n"
        "    uint r_tp = (uint)(a.tp->e[0].read(uint2(0)).x * 255.0f + 0.5f);\n"
        "    out[0] = uint4(r_t, r_tu, r_tp, 0);\n"
        "}\n" options:nil error:&error];
    CHECK(lib != nil, "texture slot 0 kernel compiles"); if (!lib) { mtl_reg_tex(0, NULL, NULL); mtl_reg_tex(256, NULL, NULL); mtl_pool_del(zmem); return; }
    NvShader *sh = [NvShader new]; sh->stage = 5; strcpy(sh->key, "0123456789abcde0_b");
    sh->fn = [lib newFunctionWithName:@"k0"]; sh->ae = [sh->fn newArgumentEncoderWithBufferIndex:0]; sh->arg_len = sh->ae.encodedLength;
    char message[256];
    CHECK(!nvn_refl_from_json(@{@"route": @"B", @"bindings": @[
        @{@"kind": @"tex", @"reg": @0, @"arg_id": @0, @"msl_type": @"texture2d<float>"},
        @{@"kind": @"tex_ubo", @"reg": @0, @"off": @0, @"arg_id": @1, @"msl_type": @"texture2d<float>"},
        @{@"kind": @"tex_pool", @"arg_id": @2, @"elem": @"texture2d<float>"}
    ]}, &sh->rf, message, sizeof message), "texture slot 0 reflection converts");
    id<MTLComputePipelineState> pipeline = [g_dev newComputePipelineStateWithFunction:sh->fn error:&error];
    CHECK(pipeline != nil, "texture slot 0 pipeline compiles"); if (!pipeline) { mtl_reg_tex(0, NULL, NULL); mtl_reg_tex(256, NULL, NULL); mtl_pool_del(zmem); return; }
    id<MTLBuffer> out = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared]; res_add(out);
    memset(word, 0, 16); S.ubo[5][0] = (p_bind_buf){.addr = (uint64_t)word, .size = 16};
    const uint64_t tex_h[3] = {0, 5u << 20 | 0u, (1ull << 32) | (5u << 20 | 0u)};
    const uint32_t ubo_h[3] = {0, 5u << 20 | 0u, 0x3f800000u};
    const uint32_t exp_t[3] = {0, 64, 191}, exp_tp[3] = {64, 64, 191};
    for (int i = 0; i < 3; i++) {
        pt.pool = (dpool_t *)&tp[i == 2 ? 1 : 0]; state_op(OP_SET_TEX_POOL, &pt);
        S.tex[5][0] = tex_h[i]; word[0] = ubo_h[i];
        mtl_sub_begin(); encode_args(nil, sh, 5, 0);
        id<MTL4ComputeCommandEncoder> en = dispatch_enc(); enc_c_pso(en, pipeline);
        CHECK(encode_args((id<MTLCommandEncoder>)en, sh, 5, 0), "texture slot 0 binding encodes");
        enc_c_args(en, out.gpuAddress + 16 * i, 1); enc_c_dispatch(en, MTLSizeMake(1,1,1), MTLSizeMake(1,1,1));
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "texture slot 0 dispatch completes");
    }
    const uint32_t *r = (const uint32_t *)out.contents;
    for (int i = 0; i < 3; i++) {
        uint32_t got_t = r[4 * i], got_tu = r[4 * i + 1], got_tp = r[4 * i + 2];
        if (got_t != exp_t[i] || got_tu != exp_t[i] || got_tp != exp_tp[i])
            fprintf(stderr, "texture_zero case %d: got (%u, %u, %u), want (%u, %u, %u)\n", i, got_t, got_tu, got_tp, exp_t[i], exp_t[i], exp_tp[i]);
        CHECK(got_t == exp_t[i] && got_tu == exp_t[i] && got_tp == exp_tp[i], "GPU samples registered texture slot 0 across tex, tex_ubo and tex_pool");
    }
    mtl_reg_tex(0, NULL, NULL); mtl_reg_tex(256, NULL, NULL);
    mtl_pool_del(zmem);
    munmap(zmem, 32768);
}
int main(void) { @autoreleasepool {
    alarm(30); mtl_init();
    // Before any texture registration (g_ntreg == 0, g_treg == NULL), pool_gpu must not dereference g_treg[0].
    mtl_sub_begin(); __unsafe_unretained id<MTLBuffer> empty_pb = nil;
    os_unfair_lock_lock(&g_rmu);
    uint64_t empty_a = pool_gpu(0, MTLTextureType2D, NVN_DPF_FLOAT, 0, &empty_pb);
    os_unfair_lock_unlock(&g_rmu);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error() && empty_a && empty_pb, "pool_gpu handles an empty texture registry (g_ntreg == 0)");
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
    texture_zero(tp, sp, (uint32_t *)(images + 36864));
    mtl_pool_del(images); h_nvnCommandBufferFinalize(&cb); printf("t_descriptor_pools: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
