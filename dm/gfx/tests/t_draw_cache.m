// Per-draw replay caches (Slice 4): cbsize_table_u epoch/sgen cache, encode_args_patch_cb zero-alloc reuse and dirty-slot patching,
// g_args_geo_only patching (preserving writable SSBO sbw_note), g_ed_dirty dynamic-state tracking and early exits,
// per-program PSO cache (NvProg.pso_c_* with packed border / early_read_only / pm_off flags), g_vbuf_dirty with enc_r_vbuf
// slot-14..29 collision tracking, border_need memo invalidation, vbuf_prepare / vconv_bind / udc_note_draw fast paths, and state_restore_caches.
#include "../nvn/nvn_mtl.m"
#include <sys/mman.h>

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL (line %d): %s\n", __LINE__, label); } } while (0)

static id<MTLTexture> make_tex(tex_t *t, uint64_t serial, int fmt) {
    *t = (tex_t){.magic = MAGIC('T', 'E', 'X', 0), .serial = serial};
    t->d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = fmt, .width = 4, .height = 4, .depth = 1, .levels = 1, .samples = 1};
    MTLTextureDescriptor *d = tex_desc(t, 0); d.storageMode = MTLStorageModeShared;
    id<MTLTexture> mt = [g_dev newTextureWithDescriptor:d];
    CHECK(mt != nil, "created Metal texture");
    res_add(mt); mtl_tex_init(t); serial_put(serial, mt);
    return mt;
}

static void bind_ubo(int stage, int idx, uint64_t addr, uint64_t size, int small) {
    if (small) {
        p_bind_ubo_small b = {.stage = (uint16_t)stage, .index = (uint16_t)idx, .size = (uint32_t)size, .addr = addr};
        state_op(OP_BIND_UBO_SMALL, &b);
    } else {
        p_bind_buf b = {.stage = stage, .index = idx, .addr = addr, .size = size};
        state_op(OP_BIND_UBO, &b);
    }
}

static void bind_ssbo(int stage, int idx, uint64_t addr, uint64_t size) {
    p_bind_buf b = {.stage = stage, .index = idx, .addr = addr, .size = size};
    state_op(OP_BIND_SSBO, &b);
}

static void test_cbsize_and_arg_patch(uint8_t *pool) {
    uint32_t *out = (uint32_t *)(pool + 0x0000);
    uint32_t *u0a = (uint32_t *)(pool + 0x1000);
    uint32_t *u0b = (uint32_t *)(pool + 0x2000);
    uint32_t *u1a = (uint32_t *)(pool + 0x3000);
    uint32_t *u5a = (uint32_t *)(pool + 0x4000);
    out[0] = 0; out[1] = 0;
    u0a[0] = 10; u0b[0] = 40; u1a[0] = 7; u5a[0] = 999;

    NSError *err = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct Args { device uint *o [[id(0)]]; constant uint *c0 [[id(1)]]; constant uint *c1 [[id(2)]]; constant uint *cbs [[id(3)]]; };\n"
        "kernel void k_cb(constant Args &a [[buffer(0)]]) { a.o[0] = a.c0[0] + a.c1[0]; a.o[1] = a.cbs[1]; }\n"
        options:nil error:&err];
    CHECK(lib != nil, "compiled cbsize/patch compute shader");

    NvShader *sh = [NvShader new];
    sh->stage = 0; sh->fn = [lib newFunctionWithName:@"k_cb"]; sh->rf.nb = 4;
    snprintf(sh->key, sizeof sh->key, "8800000000000001_c");
    sh->ae = [sh->fn newArgumentEncoderWithBufferIndex:0];
    sh->arg_len = sh->ae.encodedLength;
    sh->rf.b[0] = (nvn_rbind){.kind = NVN_RB_SBUFP, .rw = 1, .reg = 0, .aid = 0};
    sh->rf.b[1] = (nvn_rbind){.kind = NVN_RB_CBUF, .reg = 0, .aid = 1};
    sh->rf.b[2] = (nvn_rbind){.kind = NVN_RB_CBUF, .reg = 1, .aid = 2};
    sh->rf.b[3] = (nvn_rbind){.kind = NVN_RB_CBSIZE, .reg = 0, .aid = 3};
    id<MTLComputePipelineState> pso = [g_dev newComputePipelineStateWithFunction:sh->fn error:&err];
    CHECK(pso != nil, "created cbsize/patch compute pipeline");

    // 1. cbsize_table_u epoch + sgen cache checks on stage 0.
    mtl_sub_begin(); state_reset();
    bind_ubo(0, 0, (uint64_t)(uintptr_t)u0a, 256, 0);
    bind_ubo(0, 1, (uint64_t)(uintptr_t)u1a, 256, 0);
    __unsafe_unretained id<MTLBuffer> b1 = nil, b2 = nil, b3 = nil, b4 = nil;
    uint64_t g1 = cbsize_table_u(0, &b1);
    size_t used_after_1 = g_cur_arena->used;
    // Same size, different address: g_cbsize_gen[0] does not bump, exact same GPU address returned with no arena alloc.
    uint64_t cgen0 = g_cbsize_gen[0];
    bind_ubo(0, 0, (uint64_t)(uintptr_t)u0b, 256, 1);
    CHECK(g_cbsize_gen[0] == cgen0, "same-size OP_BIND_UBO_SMALL keeps g_cbsize_gen unchanged");
    uint64_t g2 = cbsize_table_u(0, &b2);
    CHECK(g2 == g1 && b2 == b1 && g_cur_arena->used == used_after_1, "cbsize_table_u reuses cached table without allocating");
    // Size change bumps g_cbsize_gen and writes a new table.
    bind_ubo(0, 1, (uint64_t)(uintptr_t)u1a, 512, 0);
    CHECK(g_cbsize_gen[0] > cgen0, "size-changing OP_BIND_UBO bumps g_cbsize_gen");
    uint64_t g3 = cbsize_table_u(0, &b3);
    CHECK(g3 != g1 && g_cur_arena->used > used_after_1, "cbsize_table_u allocates fresh table when bound size changes");
    // Unbind (addr == 0) bumps g_cbsize_gen.
    uint64_t cgen1 = g_cbsize_gen[0];
    bind_ubo(0, 1, 0, 512, 0);
    CHECK(g_cbsize_gen[0] > cgen1, "unbinding UBO slot bumps g_cbsize_gen");
    bind_ubo(0, 1, (uint64_t)(uintptr_t)u1a, 256, 0);
    // Arena rollover / new epoch invalidates g_cbsc.
    uint64_t ep0 = g_arena_epoch;
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "cbsize sub 1 completed");
    CHECK(g_arena_epoch > ep0, "arena_commit bumps g_arena_epoch");
    mtl_sub_begin();
    uint64_t g4 = cbsize_table_u(0, &b4);
    CHECK(g4 != 0 && g_cbsc[0].epoch == g_arena_epoch, "cbsize_table_u rebuilds for new arena epoch");

    // 2. encode_args_patch_cb zero-alloc reuse vs dirty-slot-only patch on stage 2 (compute table, patchable).
    sh->stage = 2;
    bind_ssbo(2, 0, (uint64_t)(uintptr_t)out, 64);
    bind_ubo(2, 0, (uint64_t)(uintptr_t)u0a, 256, 0);
    bind_ubo(2, 1, (uint64_t)(uintptr_t)u1a, 256, 0);
    bind_ubo(2, 5, (uint64_t)(uintptr_t)u5a, 256, 0);   // pre-bind unused slot 5 so same-size rebind keeps cbsize_gen unchanged
    id<MTL4ComputeCommandEncoder> ce = enc_c_begin();
    args_dirty_all();
    uint64_t enc0 = g_stat_args_enc, reuse0 = g_stat_args_reuse, patch0 = g_stat_args_patch;
    CHECK(encode_args((id<MTLCommandEncoder>)ce, sh, 2, 0), "initial full encode");
    CHECK(g_stat_args_enc == enc0 + 1, "first call did full encode");
    uint64_t ab_gpu0 = sh->ab_gpu;
    // Same-size address change on unused UBO slot 5 triggers patch check but zero-alloc reuses sh->ab_buf (dirty_cb == 0 && !cbs_dirty).
    bind_ubo(2, 5, (uint64_t)(uintptr_t)(pool + 0x4100), 256, 1);
    size_t arena_before_reuse = g_cur_arena->used;
    CHECK(encode_args((id<MTLCommandEncoder>)ce, sh, 2, 0), "unused UBO slot same-size encode");
    CHECK(g_stat_args_reuse == reuse0 + 1 && g_stat_args_patch == patch0, "unused UBO slot hit zero-alloc reuse in encode_args_patch_cb");
    CHECK(sh->ab_gpu == ab_gpu0 && g_cur_arena->used == arena_before_reuse, "zero-alloc reuse kept ab_gpu and arena cursor");
    // Size change on unused UBO slot 5 bumps g_cbsize_gen -> MUST patch cbsize slot even though dirty_cb == 0.
    bind_ubo(2, 5, (uint64_t)(uintptr_t)(pool + 0x4100), 512, 0);
    CHECK(encode_args((id<MTLCommandEncoder>)ce, sh, 2, 0), "unused UBO slot size change patched cbsize");
    CHECK(g_stat_args_patch == patch0 + 1, "unused UBO slot size change refreshed cbsize via patch path");
    uint64_t ab_gpu1 = sh->ab_gpu;
    // Changing slot 0 (same size) patches only slot 0 and keeps cbsize cached.
    uint64_t cbs_gpu_before = g_cbsc[2].gpu;
    bind_ubo(2, 0, (uint64_t)(uintptr_t)u0b, 256, 1);
    CHECK(encode_args((id<MTLCommandEncoder>)ce, sh, 2, 0), "dirty UBO slot 0 patched");
    CHECK(g_stat_args_patch == patch0 + 2 && sh->ab_gpu != ab_gpu1, "dirty bound UBO slot took patch path");
    CHECK(g_cbsc[2].gpu == cbs_gpu_before, "same-size patch reused cached cbsize table");
    // Changing slot 1's size updates cbsize in the patched block.
    bind_ubo(2, 1, (uint64_t)(uintptr_t)u1a, 768, 0);
    CHECK(encode_args((id<MTLCommandEncoder>)ce, sh, 2, 0), "dirty UBO slot 1 size patched");
    CHECK(g_stat_args_patch == patch0 + 3 && g_cbsc[2].gpu != cbs_gpu_before, "size change refreshed cbsize slot during patch");
    enc_c_pso(ce, pso);
    enc_c_dispatch(ce, MTLSizeMake(1, 1, 1), MTLSizeMake(1, 1, 1));
    enc_c_end(ce);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "arg patch GPU submission completed");
    CHECK(out[0] == 47 && out[1] == 768, "GPU observed patched u0b (40) + u1a (7) and updated cbsize[1] (768)");

    // 3. g_args_geo_only path preserves writable SSBO sbw_note even when SSBO binding is unchanged.
    mtl_sub_begin();
    NvShader *gsh = [NvShader new];
    gsh->stage = 2; gsh->fn = sh->fn; gsh->ae = sh->ae; gsh->arg_len = sh->arg_len; gsh->rf.nb = 3;
    snprintf(gsh->key, sizeof gsh->key, "8800000000000002_g");
    gsh->rf.b[0] = (nvn_rbind){.kind = NVN_RB_SBUFP, .rw = 1, .reg = 0, .aid = 0};
    gsh->rf.b[1] = (nvn_rbind){.kind = NVN_RB_CBUF, .reg = 0, .aid = 1};
    gsh->rf.b[2] = (nvn_rbind){.kind = NVN_RB_GEOMETRY, .reg = 3, .aid = 2};
    bind_ssbo(2, 0, (uint64_t)(uintptr_t)out, 64);
    bind_ubo(2, 0, (uint64_t)(uintptr_t)u0a, 256, 0);
    g_geo_bind[3] = g_cur_arena ? g_cur_arena->gpu : (uint64_t)(uintptr_t)u0a;
    id<MTL4ComputeCommandEncoder> ce2 = enc_c_begin();
    args_dirty_all();
    CHECK(encode_args((id<MTLCommandEncoder>)ce2, gsh, 2, 0), "geo shader full encode");
    // Simulate a new submission clearing sbw state; a subsequent g_args_geo_only patch MUST still note the writable SSBO.
    g_pend_gen++; g_nsbw = 0; g_nsbw_w = 0; g_sbw_over = 0;
    CHECK(!sbw_hits((uint64_t)(uintptr_t)out, (uint64_t)(uintptr_t)out + 64), "sbw cleared before geo patch");
    uint64_t patch_g0 = g_stat_args_patch;
    g_geo_bind[3] = g_cur_arena->gpu + 256;
    args_dirty(2);   // as geo_mesh_encode does before g_args_geo_only = 1
    g_args_geo_only = 1;
    CHECK(encode_args((id<MTLCommandEncoder>)ce2, gsh, 2, 0), "geo shader patched encode");
    g_args_geo_only = 0;
    CHECK(g_stat_args_patch == patch_g0 + 1, "g_args_geo_only took patch path");
    CHECK(sbw_hits((uint64_t)(uintptr_t)out, (uint64_t)(uintptr_t)out + 64), "g_args_geo_only still recorded writable SSBO in sbw_note");
    enc_c_end(ce2);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "geo patch sub completed");
}

static void test_pso_dyn_vbuf_border_udc(uint8_t *pool) {
    NSError *err = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct VOut { float4 pos [[position]]; };\n"
        "vertex VOut vs_a(uint vid [[vertex_id]]) {\n"
        "    float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)};\n"
        "    return {float4(p[vid % 3], 0.5, 1.0)};\n"
        "}\n"
        "vertex VOut vs_b(uint vid [[vertex_id]]) {\n"
        "    float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)};\n"
        "    return {float4(p[vid % 3], 1.0, 1.0)};\n"
        "}\n"
        "fragment float4 fs_a() { return float4(1, 0, 0, 1); }\n"
        "fragment float4 fs_b() { return float4(0, 1, 0, 1); }\n"
        "[[early_fragment_tests]] fragment float4 fs_b_early() { return float4(0, 1, 0, 1); }\n"
        options:nil error:&err];
    CHECK(lib != nil, "compiled render test shaders");

    NvShader *vsa = [NvShader new]; vsa->stage = 0; vsa->fn = [lib newFunctionWithName:@"vs_a"]; snprintf(vsa->key, sizeof vsa->key, "8800000000000010_b");
    NvShader *vsb = [NvShader new]; vsb->stage = 0; vsb->fn = [lib newFunctionWithName:@"vs_b"]; snprintf(vsb->key, sizeof vsb->key, "8800000000000011_b");
    vsb->rf.pos_far = 1;
    NvShader *fsa = [NvShader new]; fsa->stage = 1; fsa->fn = [lib newFunctionWithName:@"fs_a"]; snprintf(fsa->key, sizeof fsa->key, "8800000000000012_b");
    fsa->rf.color_known = 1; fsa->rf.color_mask = 1;
    NvShader *fsb = [NvShader new]; fsb->stage = 1; fsb->fn = [lib newFunctionWithName:@"fs_b"]; snprintf(fsb->key, sizeof fsb->key, "8800000000000013_b");
    fsb->early_fn = [lib newFunctionWithName:@"fs_b_early"]; fsb->early_read_only = 1;
    fsb->rf.color_known = 1; fsb->rf.color_mask = 1;
    fsb->rf.border = 2; fsb->rf.nb = 2;
    fsb->rf.b[0] = (nvn_rbind){.kind = NVN_RB_SAMP, .reg = 0, .bw = 1};
    fsb->rf.b[1] = (nvn_rbind){.kind = NVN_RB_BORDER, .aid = 0};

    NvProg *prA = [NvProg new]; prA->sh[0] = vsa; prA->sh[1] = fsa;
    NvProg *prB = [NvProg new]; prB->sh[0] = vsb; prB->sh[1] = fsb;
    prog_t pA = {.magic = MAGIC('P','R','O','G'), .serial = 93001}; serial_put(pA.serial, prA);
    prog_t pB = {.magic = MAGIC('P','R','O','G'), .serial = 93002}; serial_put(pB.serial, prB);

    tex_t ctex, dtex;
    make_tex(&ctex, 93010, NVN_FORMAT_RGBA8);
    make_tex(&dtex, 93011, NVN_FORMAT_DEPTH32F);

    // Register normal sampler (sid=1) and custom-border sampler (sid=2).
    samp_par sp_norm = {.ws = NVN_WRAP_MODE_CLAMP_TO_EDGE, .wt = NVN_WRAP_MODE_CLAMP_TO_EDGE, .wr = NVN_WRAP_MODE_CLAMP_TO_EDGE};
    samp_par sp_bord = {.ws = NVN_WRAP_MODE_CLAMP_TO_BORDER, .wt = NVN_WRAP_MODE_CLAMP_TO_BORDER, .wr = NVN_WRAP_MODE_CLAMP_TO_BORDER, .border = {0.25f, 0.5f, 0.75f, 1.0f}};
    mtl_reg_samp(1, &sp_norm);
    mtl_reg_samp(2, &sp_bord);

    mtl_sub_begin(); state_reset();
    memset(&R, 0, sizeof R);
    R.rt.n = 1; R.rt.color[0] = &ctex; R.rt.depth = &dtex; R.has_rt = 1; g_setrt_n++;
    R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(0, 0, 0, 1);
    R.clear_d = 1; R.cd = 1.0;

    p_bind_program bpA = {.prog = &pA, .stages = 3};
    p_bind_program bpB = {.prog = &pB, .stages = 3};
    p_draw tri = {.prim = 4, .first = 0, .count = 3, .instances = 1};
    p_bind_handles bs1 = {.stage = 1, .first = 0, .n = 1}; bs1.h[0] = (uint64_t)1 << 20;
    p_bind_handles bs2 = {.stage = 1, .first = 0, .n = 1}; bs2.h[0] = (uint64_t)2 << 20;
    state_op(OP_BIND_SAMP, &bs1);

    // 1. Per-program PSO cache + g_ed_dirty initialization.
    state_op(OP_BIND_PROGRAM, &bpA);
    draw_(&tri, NULL);
    CHECK(g_enc != nil, "render pass opened");
    CHECK(prA->pso_c_ps != NULL && prA->pso_c_pass == g_pass.serial && prA->pso_c_stgen == g_pso_st_sgen && prA->pso_c_flags == 0, "prA populated pso_c_* cache");
    CHECK(g_ed_dirty == 0 && (g_ed.ok & ED_ALL_DYN) == ED_ALL_DYN, "draw_ initialized all dynamic state and cleared g_ed_dirty");
    CHECK(g_ed.bias[0] == 0.0f && g_ed.clip == 1 && g_ed.pos_far == 0, "prA has no pos_far bias");

    // 2. Switch to prB (pos_far == 1): g_pso_st_sgen unchanged, pos_far updates bias/clip, prB caches its PSO.
    uint64_t stgen0 = g_pso_st_sgen, mgen0 = g_mpso_sgen;
    state_op(OP_BIND_PROGRAM, &bpB);
    CHECK(g_pso_st_sgen == stgen0 && g_mpso_sgen == mgen0, "OP_BIND_PROGRAM does not bump g_pso_st_sgen or g_mpso_sgen");
    draw_(&tri, NULL);
    CHECK(prB->pso_c_ps != NULL && prB->pso_c_ps != prA->pso_c_ps && prB->pso_c_stgen == stgen0, "prB cached distinct PSO at same stgen");
    CHECK(g_ed.pos_far == 1 && g_ed.bias[0] == 16.0f && g_ed.clip == 0, "pos_far mismatch applied 16-unit depth bias and disabled depth clip");

    // 3. Reversing depth range clears pos_far bias via ED_VP | ED_BIAS | ED_CLIP.
    p_depth_range dr_rev = {.n = 1.0f, .f = 0.0f};
    state_op(OP_DEPTH_RANGE, &dr_rev);
    CHECK((g_ed_dirty & (ED_VP | ED_BIAS | ED_CLIP)) == (ED_VP | ED_BIAS | ED_CLIP), "OP_DEPTH_RANGE dirties ED_VP | ED_BIAS | ED_CLIP");
    p_viewports vp0 = {.first = 0, .n = 1, .v = {{0, 0, 4, 4}}};
    state_op(OP_VIEWPORTS, &vp0);
    draw_(&tri, NULL);
    CHECK(g_ed.bias[0] == 0.0f && g_ed.clip == 1, "reversed depth range disables pos_far bias");
    p_depth_range dr_fwd = {.n = 0.0f, .f = 1.0f};
    state_op(OP_DEPTH_RANGE, &dr_fwd);
    draw_(&tri, NULL);
    CHECK(g_ed.bias[0] == 16.0f && g_ed.clip == 0, "forward depth range restores pos_far bias");

    // 4. Alternating back to prA hits per-program PSO cache and restores pos_far == 0.
    state_op(OP_BIND_PROGRAM, &bpA);
    uint64_t d0 = g_stat_draw;
    draw_(&tri, NULL);
    CHECK(g_stat_draw == d0 + 1 && g_ed.pos_far == 0 && g_ed.bias[0] == 0.0f && g_ed.clip == 1, "alternating back to prA hit cache and cleared pos_far bias");

    // 5. Empty scissor skips draw and keeps g_ed_dirty nonzero; restoring valid scissor updates g_ed.sc.
    p_scissor sc_empty = {.x = 0, .y = 0, .w = 0, .h = 0};
    state_op(OP_SCISSOR, &sc_empty);
    CHECK((g_ed_dirty & ED_SC) != 0, "OP_SCISSOR sets ED_SC");
    d0 = g_stat_draw;
    draw_(&tri, NULL);
    CHECK(g_stat_draw == d0 && (g_ed_dirty & ED_SC) != 0, "empty scissor skipped draw without clearing g_ed_dirty");
    p_scissor sc_ok = {.x = 1, .y = 1, .w = 2, .h = 2};
    state_op(OP_SCISSOR, &sc_ok);
    draw_(&tri, NULL);
    CHECK(g_stat_draw == d0 + 1 && g_ed.sc.x == 1 && g_ed.sc.y == 1 && g_ed.sc.width == 2 && g_ed.sc.height == 2 && g_ed_dirty == 0, "valid scissor updated g_ed.sc and drew");

    // 6. Front+back cull skips draw without clearing g_ed_dirty; restoring NONE draws.
    p_bind_state st_poly = {.kind = 4};
    poly_st ps_fb = {.cull = NVN_FACE_FRONT_AND_BACK, .front = NVN_FRONT_FACE_CCW, .mode = NVN_POLYGON_MODE_FILL};
    memcpy(st_poly.bytes, &ps_fb, sizeof ps_fb);
    state_op(OP_BIND_STATE, &st_poly);
    CHECK(g_pso_st_sgen > stgen0 && g_mpso_sgen > mgen0, "OP_BIND_STATE(poly) bumps g_pso_st_sgen and g_mpso_sgen");
    d0 = g_stat_draw;
    draw_(&tri, NULL);
    CHECK(g_stat_draw == d0 && g_ed_dirty != 0, "front+back cull skipped draw without clearing g_ed_dirty");
    poly_st ps_none = {.cull = NVN_FACE_NONE, .front = NVN_FRONT_FACE_CCW, .mode = NVN_POLYGON_MODE_FILL};
    memcpy(st_poly.bytes, &ps_none, sizeof ps_none);
    state_op(OP_BIND_STATE, &st_poly);
    draw_(&tri, NULL);
    CHECK(g_stat_draw == d0 + 1 && g_ed_dirty == 0, "restoring cull NONE resumed drawing and cleared g_ed_dirty");

    // 7. OP_STENCIL (ref) and OP_BLEND_COLOR update g_ed selectively.
    p_stencil sref = {.face = 3, .which = 2, .ref = 0x5a};
    state_op(OP_STENCIL, &sref);
    CHECK(g_ed_dirty == ED_SREF, "OP_STENCIL(ref) dirties only ED_SREF");
    p_blend_color bc = {.rgba = {0.25f, 0.5f, 0.75f, 1.0f}};
    state_op(OP_BLEND_COLOR, &bc);
    CHECK(g_ed_dirty == (ED_SREF | ED_BLEND), "OP_BLEND_COLOR adds ED_BLEND");
    draw_(&tri, NULL);
    CHECK(g_ed.sref == 0x5a && g_ed.sref_b == 0x5a && g_ed.blend[0] == 0.25f && g_ed.blend[2] == 0.75f && g_ed_dirty == 0, "stencil ref and blend color applied");

    // 8. Mid-pass clear triangle invalidates g_ed.ok (except ED_DBND) so next draw re-applies dynamic state even with g_ed_dirty == 0.
    p_clear_color cc_mid = {.index = 0, .mask = 0xf, .rgba = {0, 0, 1, 1}};
    clear_color_tri(&cc_mid);
    CHECK((g_ed.ok & ~ED_DBND) == 0 && g_ed_dirty == 0, "clear_color_tri cleared g_ed.ok dynamic bits");
    draw_(&tri, NULL);
    CHECK((g_ed.ok & ED_ALL_DYN) == ED_ALL_DYN && g_ed.sc.width == 2 && g_ed.sref == 0x5a, "draw after clear_color_tri re-applied dynamic state");

    // 9. early_read_only and border flip pso_c_flags and invalidate per-program PSO cache without bumping g_pso_st_sgen.
    state_op(OP_BIND_PROGRAM, &bpB);
    draw_(&tri, NULL);
    void *psB_base = prB->pso_c_ps;
    CHECK(prB->pso_c_flags == 0, "prB starts with border=0, early_read_only=0, pm_off=0");
    // Enable depth test (read-only: dt=1, dw=0, df=LESS, st=0) -> early_read_only becomes 1 (pso_c_flags bit 1).
    p_bind_state st_ds = {.kind = 5};
    ds_st ds_ro = {.dt = 1, .dw = 0, .df = NVN_DEPTH_FUNC_LESS, .st = 0, .sf_f = NVN_STENCIL_FUNC_ALWAYS, .sf_b = NVN_STENCIL_FUNC_ALWAYS, .fail_f = 1, .zfail_f = 1, .zpass_f = 1, .fail_b = 1, .zfail_b = 1, .zpass_b = 1};
    memcpy(st_ds.bytes, &ds_ro, sizeof ds_ro);
    uint64_t stgen1 = g_pso_st_sgen;
    state_op(OP_BIND_STATE, &st_ds);
    CHECK(g_pso_st_sgen == stgen1, "OP_BIND_STATE(ds) does not bump g_pso_st_sgen");
    draw_(&tri, NULL);
    CHECK(prB->pso_c_flags == 2 && prB->pso_c_ps != psB_base, "early_read_only flipped pso_c_flags bit 1 and selected early_fn PSO");
    // Bind custom-border sampler (sid=2) -> border becomes 1 (pso_c_flags bit 0) and border_need memo updates.
    CHECK(border_need(fsb, 1) == 0, "border_need cached 0 for clamp-to-edge sampler");
    state_op(OP_BIND_SAMP, &bs2);
    CHECK(border_need(fsb, 1) == 1, "OP_BIND_SAMP invalidated border_need memo and detected border twin");
    void *psB_early = prB->pso_c_ps;
    draw_(&tri, NULL);
    CHECK(prB->pso_c_flags == 3 && prB->pso_c_ps != psB_early, "border twin flipped pso_c_flags bit 0 and selected border PSO");
    // Re-registering sampler 2 in-place to clamp-to-edge invalidates border_need via g_reg_gen.
    mtl_reg_samp(2, &sp_norm);
    CHECK(border_need(fsb, 1) == 0, "mtl_reg_samp invalidated border_need memo via g_reg_gen");

    // 10. OP_BIND_VATTR / OP_BIND_VSTREAM bump g_pso_st_sgen (not g_mpso_sgen), g_vattr_gen / g_vstream_gen, and invalidate vs_streams & vconv_bind.
    uint64_t stgen2 = g_pso_st_sgen, mgen2 = g_mpso_sgen, vagen0 = g_vattr_gen, vsgen0 = g_vstream_gen;
    p_bind_vattr va = {.n = 2};
    va_st v0 = {.fmt = 0x22, .offset = 0, .stream = 0}, v1 = {.fmt = 0x22, .offset = 0, .stream = 14};
    memcpy(&va.st[0], &v0, 4); memcpy(&va.st[1], &v1, 4);
    state_op(OP_BIND_VATTR, &va);
    CHECK(g_pso_st_sgen == stgen2 + 1 && g_mpso_sgen == mgen2 && g_vattr_gen == vagen0 + 1, "OP_BIND_VATTR bumps g_pso_st_sgen and g_vattr_gen, not g_mpso_sgen");
    p_bind_vstream vs = {.n = 15};
    vs.st[0] = (vs_st){.stride = 12, .divisor = 0};
    vs.st[14] = (vs_st){.stride = 12, .divisor = 0};
    state_op(OP_BIND_VSTREAM, &vs);
    CHECK(g_pso_st_sgen == stgen2 + 2 && g_mpso_sgen == mgen2 && g_vstream_gen == vsgen0 + 1, "OP_BIND_VSTREAM bumps g_pso_st_sgen and g_vstream_gen, not g_mpso_sgen");

    // 11. g_vbuf_dirty + enc_r_vbuf slot-14..29 collision + vbuf_prepare + vconv_bind + udc_note_draw.
    nvn_refl rf_vb = {.nv = 2};
    rf_vb.v[0].nvn = 0; rf_vb.v[0].ct = NVN_CT_FLOAT;
    rf_vb.v[1].nvn = 1; rf_vb.v[1].ct = NVN_CT_FLOAT;
    CHECK(vs_streams(&rf_vb) == ((1u << 0) | (1u << 14)), "vs_streams returns streams 0 and 14");
    uint64_t vb0 = (uint64_t)(uintptr_t)(pool + 0x5000);
    uint64_t vb14 = (uint64_t)(uintptr_t)(pool + 0x6000);
    p_bind_vbuf1 bv0 = {.index = 0, .addr = vb0, .size = 256};
    p_bind_vbuf1 bv14 = {.index = 14, .addr = vb14, .size = 256};
    state_op(OP_BIND_VBUF1, &bv0);
    state_op(OP_BIND_VBUF1, &bv14);
    p_draw_idx di = {.prim = 4, .itype = 1, .count = 3, .instances = 1, .ibuf = (uint64_t)(uintptr_t)(pool + 0x7000)};
    vbuf_prepare(&rf_vb, NULL, &di);
    CHECK((g_vbuf_unaligned_mask & 0x10000u) == 0 && (g_vbuf_unaligned_mask & ((1u << 0) | (1u << 14))) == 0, "vbuf_prepare scanned aligned streams");
    // Bind streams 0 and 14 -> clears bits 0 and 14 in g_vbuf_dirty.
    CHECK(bind_vbufs(g_enc, &rf_vb, NULL, &di) == -1, "bind_vbufs succeeded for streams 0 and 14");
    CHECK((g_vbuf_dirty & ((1u << 0) | (1u << 14))) == 0, "bind_vbufs cleared dirty bits for bound streams 0 and 14");
    uint64_t set_before = g_vbuf_set;
    CHECK(bind_vbufs(g_enc, &rf_vb, NULL, &di) == -1 && g_vbuf_set == set_before, "second bind_vbufs with clean dirty mask performed zero table sets");
    // Simulate pull_bind writing PULL_SLOT (16 == VB_SLOT(0)) and tessellation writing slot 14 (== VB_SLOT(14)).
    enc_r_vbuf(g_enc, PULL_SLOT, g_cur_arena->gpu);
    CHECK((g_vbuf_dirty & (1u << 0)) != 0, "enc_r_vbuf(PULL_SLOT=16) re-dirtied stream 0");
    enc_r_vbuf(g_enc, 14, g_cur_arena->gpu);
    CHECK((g_vbuf_dirty & (1u << 14)) != 0, "enc_r_vbuf(14) re-dirtied stream 14");
    uint64_t set_after_collision = g_vbuf_set;
    CHECK(bind_vbufs(g_enc, &rf_vb, NULL, &di) == -1 && g_vbuf_set == set_after_collision + 2, "bind_vbufs rebound streams 0 and 14 after slot-14/16 overwrite");
    CHECK((g_vbuf_dirty & ((1u << 0) | (1u << 14))) == 0, "g_vbuf_dirty cleared again after rebind");
    // Unaligned stream update sets bit 0 in g_vbuf_unaligned_mask.
    p_bind_vbuf1 bv0_un = {.index = 0, .addr = vb0 + 2, .size = 64};
    state_op(OP_BIND_VBUF1, &bv0_un);
    CHECK((g_vbuf_unaligned_mask & 1u) != 0 && (g_vbuf_dirty & 1u) != 0, "unaligned OP_BIND_VBUF1 set unaligned and dirty bits");
    state_op(OP_BIND_VBUF1, &bv0);
    CHECK((g_vbuf_unaligned_mask & 1u) == 0, "re-aligning OP_BIND_VBUF1 cleared unaligned bit");

    // vconv_bind negative memo populates and invalidates on OP_BIND_VATTR.
    vsa->rf = rf_vb;
    vconv_bind(g_enc, vsa, 0, NULL, &di);
    CHECK(vsa->vc_vgen == g_vattr_gen && vsa->vc_any == 0, "vconv_bind populated negative memo");
    v0.offset = 4; memcpy(&va.st[0], &v0, 4);
    state_op(OP_BIND_VATTR, &va);
    CHECK(vsa->vc_vgen != g_vattr_gen, "OP_BIND_VATTR invalidated vconv_bind negative memo");

    // udc_note_draw cleared pending color bit once written, and sd_finish resets g_sd_pend.
    CHECK((g_sd_pend & 1u) == 0, "udc_note_draw cleared color-0 pending bit after raster draw");
    rp_end();
    CHECK(g_sd_pend == 0 && g_sd_dup_mask == 0, "sd_finish cleared g_sd_pend and g_sd_dup_mask");

    // 12. state_restore_caches invalidates every Slice 4 cache together.
    uint64_t r_stgen = g_pso_st_sgen, r_mgen = g_mpso_sgen, r_vagen = g_vattr_gen, r_vsgen = g_vstream_gen, r_cgen = g_cbsize_gen[0];
    g_ed_dirty = 0; g_vbuf_dirty = 0; g_vbuf_unaligned_mask = 0;
    state_restore_caches();
    CHECK(g_ed_dirty == ED_ALL_DYN && g_vbuf_dirty == 0xffffu && g_vbuf_unaligned_mask == 0x10000u, "state_restore_caches resets dirty masks");
    CHECK(g_pso_st_sgen > r_stgen && g_mpso_sgen > r_mgen && g_vattr_gen > r_vagen && g_vstream_gen > r_vsgen && g_cbsize_gen[0] > r_cgen,
          "state_restore_caches bumps all generation counters");

    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "render cache sub completed");
}

int main(void) {
    @autoreleasepool {
        mtl_init();
        mtl_pc_claim();   // exercise replay-thread fast paths (same thread identity as nq_worker)
        size_t plen = 65536;
        uint8_t *pool = mmap(NULL, plen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        CHECK(pool != MAP_FAILED, "allocated guest memory pool");
        if (pool != MAP_FAILED) {
            mtl_pool_add(pool, plen);
            test_cbsize_and_arg_patch(pool);
            test_pso_dyn_vbuf_border_udc(pool);
            mtl_drain();
            mtl_pool_del(pool);
            munmap(pool, plen);
        }
        printf("t_draw_cache: %d checks, %d failed\n", checks, failed);
        return failed ? 1 : 0;
    }
}
