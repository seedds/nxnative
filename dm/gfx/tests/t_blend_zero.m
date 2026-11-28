// NVIDIA's "zero times anything is zero" float blending through the replay: a pixel shader that writes NaN, +Inf, -Inf and 2 at alpha 0 and at alpha 1 (dmsh_rewrite.py blend_zero's
// guard in it, from test_dmsh_blend_zero.py --fixture), drawn into an RGBA16F target under blend states with and without a zero source weight, and the target read back. Where the
// weight is zero by the alpha the target keeps its colour; everywhere else the non-finite colour is stored, as NVIDIA stores it. With DM_NVN_BLEND_ZERO=0 the same draws show Apple's blender as
// it is: the NaN is stored under the zero weight too (what the guard is for). bz_word's cases are checked on the state alone. A merged pass's draw (output 0 drawn to attachment 1) is
// drawn and read back too, with the words bz_bind bound.
// make GAME=<id> check_t_blend_zero (./t_blend_zero <dir>)
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static float half(uint16_t h) {
    int e = h >> 10 & 31, m = h & 1023; float s = h >> 15 ? -1 : 1;
    return e == 31 ? (m ? NAN : s * INFINITY) : s * (e ? ldexpf(1024 + m, e - 25) : ldexpf(m, -24));
}
enum { Z = NVN_BLEND_FUNC_ZERO, O = NVN_BLEND_FUNC_ONE, SC = NVN_BLEND_FUNC_SRC_COLOR, SA = NVN_BLEND_FUNC_SRC_ALPHA, ISA = NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, ADD = NVN_BLEND_EQUATION_ADD };
// each row's blend state: on, source and destination colour factor, equation; then what the row stores for a colour v at alpha 0 and at alpha 1 over the target's d:
// 'd' the target's own, 's' the shader's (non-finite as it is), '+' s + d, 'n' NaN when s is not finite and d * s when it is
static const struct { int on, src, dst, eq; char a0, a1; uint32_t word; } rows[] = {
    {0, O, Z, ADD, 's', 's', 0},        // blending off: the colour is stored
    {1, SA, ISA, ADD, 'd', 's', 1},     // the mist's state: nothing added at alpha 0
    {1, O, ISA, ADD, '+', 's', 0},      // premultiplied: the weight is 1
    {1, ISA, SA, ADD, 's', 'd', 2},     // zero at alpha 1
    {1, Z, O, ADD, 'd', 'd', 0},        // a factor of ZERO: Apple's blender leaves the term out by itself, NaN or not
    {1, SA, SC, ADD, 'n', '+', 0},      // the destination weight reads the source colour: left to the blender (NaN stays NaN; an infinite s is not checked, nor alpha 1's s + d * s for a finite s)
    {1, SA, ISA, NVN_BLEND_EQUATION_MAX, 0, 0, 0},   // MIN / MAX take no weights: the word alone
};
enum { NROWS = sizeof rows / sizeof *rows };
static blend_st state(int r) { blend_st b = {.target = 0, .src_rgb = (uint64_t)rows[r].src, .dst_rgb = (uint64_t)rows[r].dst, .src_a = O, .dst_a = Z, .eq_rgb = (uint64_t)rows[r].eq, .eq_a = ADD}; return b; }

static void replace_float_case(void) {
    pso_key key = {.blend_en = 1, .chmask = 0xf};
    key.cfmt[0] = MTLPixelFormatRG11B10Float;
    key.blend[0] = state(0);
    CHECK(!pso_desc(&key, nil, nil, nil, 0).colorAttachments[0].blendingEnabled);
    key.cfmt[0] = MTLPixelFormatRGBA16Float;
    CHECK(!pso_desc(&key, nil, nil, nil, 0).colorAttachments[0].blendingEnabled);
    key.blend[0].dst_a = O;
    CHECK(pso_desc(&key, nil, nil, nil, 0).colorAttachments[0].blendingEnabled);
    key.blend[0] = state(2);
    CHECK(pso_desc(&key, nil, nil, nil, 0).colorAttachments[0].blendingEnabled);
    key.cfmt[0] = MTLPixelFormatRGBA8Unorm;
    key.blend[0] = state(0);
    CHECK(pso_desc(&key, nil, nil, nil, 0).colorAttachments[0].blendingEnabled);
}
// A merged pass's draw (g_pso_lmap, the map from the physical attachments to the shader's outputs; g_pso_fsfn, the fragment that writes each output to its attachment), as draw has it
// for a pass that reads its targets from the tile: the words it binds are the state's under the map, as its pipeline's blending is. The shader's output 0 (the mist's SRC_ALPHA /
// ONE_MINUS_SRC_ALPHA) goes to physical attachment 1, an RGBA16F target, beside an RGBA8 one at attachment 0 that no output reaches. Read without the map, the word is 0 (attachment 0
// is no float target) and the NaN of alpha 0 is stored. The merged fragment is the fixture's body with its colour written to color(1)
static void merged_case(NSString *source, prog_t *program, int on) {
    NSString *merged = [source stringByAppendingString:@"\nstruct dm_merged_out { float4 m_1 [[color(1)]]; };\n"
        "fragment dm_merged_out main_m(float4 gl_FragCoord [[position]], constant dm_blend_state& dm_blend [[buffer(28)]])\n"
        "{\n    main0_out out = {};\n    uint x = uint(gl_FragCoord.x);\n    uint k = x & 3u;\n"
        "    float v = as_type<float>(k == 0u ? 0x7fc00000u : (k == 1u ? 0x7f800000u : (k == 2u ? 0xff800000u : 0x40000000u)));\n"
        "    out.m_0 = float4(v, v, v, x < 4u ? 0.0 : 1.0);\n    dm_blend0(out, dm_blend);\n    dm_merged_out r = {};\n    r.m_1 = out.m_0;\n    return r;\n}\n"];
    NSError *error = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:merged options:nil error:&error];
    CHECK(lib != nil); if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return; }
    MTLFunctionConstantValues *mcv = [MTLFunctionConstantValues new]; BOOL no = NO;
    [mcv setConstantValue:&no type:MTLDataTypeBool atIndex:DMSH_BLEND_OFF_FC];
    id<MTLFunction> fn = fn_spec(lib, @"main_m", mcv, nil);   // as shader_load: a function with dm_blend_off is specialized with false
    CHECK(fn != nil); if (!fn) return;
    static tex_t unused = {.magic = MAGIC('T', 'E', 'X', 0), .serial = 98202}, target = {.magic = MAGIC('T', 'E', 'X', 0), .serial = 98203};
    unused.d = (nvn_texdesc){.width = 8, .height = 1, .depth = 1, .samples = 1, .levels = 1, .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8};
    target.d = (nvn_texdesc){.width = 8, .height = 1, .depth = 1, .samples = 1, .levels = 1, .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA16F};
    uint8_t *cmds; if (posix_memalign((void **)&cmds, getpagesize(), 65536)) abort(); memset(cmds, 0, 65536); mtl_pool_add(cmds, 65536);
    size_t used = 0;
    #define EMIT_M(OP, X) do { size_t z = (8 + sizeof(X) + 7) & ~7ull; op_hdr h = {OP, (uint32_t)z}; memcpy(cmds + used, &h, 8); memcpy(cmds + used + 8, &(X), sizeof(X)); used += z; } while (0)
    const float d[3] = {0.25f, 0.5f, 0.75f};
    p_set_rt rt = {.n = 2}; rt.color[0] = &unused; rt.color[1] = &target; EMIT_M(OP_SET_RT, rt);
    p_viewports vp = {.n = 1}; vp.v[0][2] = 8; vp.v[0][3] = 1; EMIT_M(OP_VIEWPORTS, vp);
    p_clear_color cl = {.index = 1, .mask = 15, .rgba = {d[0], d[1], d[2], 1}}; EMIT_M(OP_CLEAR_COLOR, cl);
    p_bind_program bp = {program, 3}; EMIT_M(OP_BIND_PROGRAM, bp);
    p_bind_state bs = {.kind = 1}; color_st cs = {1, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; memcpy(bs.bytes, &cs, sizeof cs); EMIT_M(OP_BIND_STATE, bs);
    p_bind_state bb = {.kind = 0}; blend_st b = state(1); memcpy(bb.bytes, &b, sizeof b); EMIT_M(OP_BIND_STATE, bb);   // output 0: the mist's state
    p_scissor sc = {0, 0, 8, 1}; EMIT_M(OP_SCISSOR, sc); p_draw dr = {.prim = 4, .count = 3, .instances = 1}; EMIT_M(OP_DRAW, dr);
    op_hdr end = {OP_END, 8}; memcpy(cmds + used, &end, 8);
    #undef EMIT_M
    state_reset(); memset(&R, 0, sizeof R);
    static const uint8_t lmap[8] = {0xff, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    g_pso_lmap = lmap; g_pso_fsfn = fn; memset(g_bz_last, 0xee, sizeof g_bz_last);   // as draw has them for a merged pass; it clears them once it has the pipeline
    mtl_sub_begin(); mtl_exec(cmds);
    CHECK(g_bz_last[0] == (on ? 1u : 0u)); CHECK(g_bz_last[1] == 0);   // the words the draw bound: output 0's zero bit under the map; no rounding (blended sum)
    CHECK(!g_pso_lmap);   // the map is the draw's alone
    id<MTLBuffer> pixels = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    CHECK(pixels != nil); if (!pixels) { mtl_sub_commit(); mtl_pool_del(cmds); free(cmds); return; }
    [blit() copyFromTexture:tex_mtl(&target) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(8, 1, 1) toBuffer:pixels destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256];
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull)); CHECK(!mtl_last_commit_error());
    const float src[4] = {NAN, INFINITY, -INFINITY, 2}; const uint16_t *px = pixels.contents;
    for (int x = 0; x < 8; x++) {
        float s = src[x & 3]; char how = x < 4 ? 'd' : 's';   // the mist's state: nothing added at alpha 0, the colour at alpha 1
        if (!on && x < 4 && !isfinite(s)) how = 'N';   // Apple's blender: 0 x NaN = 0 x Inf = NaN
        for (int c = 0; c < 3; c++) {
            float got = half(px[x * 4 + c]), want = how == 'd' ? d[c] : how == 's' ? s : NAN;
            int ok = isnan(want) ? isnan(got) : got == want;
            if (!ok) fprintf(stderr, "merged pass, x %d channel %d: got %g, want %g\n", x, c, got, want);
            CHECK(ok);
        }
    }
    mtl_pool_del(cmds); mtl_drain(); free(cmds);
    memset(&S.color, 0, sizeof S.color);
}
int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) return 2;
    setenv("DM_MSL_DIR", argv[1], 1);
    mtl_init(); g_msl_dir = @(argv[1]);
    replace_float_case();
    int on = bz_on();
    NSError *error = nil; NSString *source = [NSString stringWithContentsOfFile:[g_msl_dir stringByAppendingString:@"/blend.metal"] encoding:NSUTF8StringEncoding error:&error];
    if (!source) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:source options:nil error:&error];
    if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    NvProg *pr = [NvProg new];
    pr->sh[0] = [NvShader new]; pr->sh[0]->fn = [lib newFunctionWithName:@"vertex_main"]; pr->sh[0]->stage = 0; snprintf(pr->sh[0]->key, sizeof pr->sh[0]->key, "vblend_b");
    NvShader *ps = [NvShader new]; pr->sh[1] = ps;
    MTLFunctionConstantValues *pcv = [MTLFunctionConstantValues new]; BOOL pno = NO;
    [pcv setConstantValue:&pno type:MTLDataTypeBool atIndex:DMSH_BLEND_OFF_FC];
    ps->fn = fn_spec(lib, @"main0", pcv, nil); ps->stage = 1; ps->has_bz = 1; snprintf(ps->key, sizeof ps->key, "pblend_b");   // as shader_load
    CHECK(ps->fn != nil);
    static prog_t program; program.serial = 98200; serial_put(program.serial, pr);
    merged_case(source, &program, on);
    tex_t target = {.magic = MAGIC('T', 'E', 'X', 0), .serial = 98201};
    target.d = (nvn_texdesc){.width = 8, .height = NROWS, .depth = 1, .samples = 1, .levels = 1, .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA16F};
    uint8_t *commands; if (posix_memalign((void **)&commands, getpagesize(), 65536)) abort(); memset(commands, 0, 65536); mtl_pool_add(commands, 65536);
    size_t used = 0;
    #define EMIT(OP, X) do { size_t z = (8 + sizeof(X) + 7) & ~7ull; op_hdr h = {OP, (uint32_t)z}; memcpy(commands + used, &h, 8); memcpy(commands + used + 8, &(X), sizeof(X)); used += z; } while (0)
    const float d[3] = {0.25f, 0.5f, 0.75f};
    p_set_rt rt = {.n = 1}; rt.color[0] = &target; EMIT(OP_SET_RT, rt);
    p_viewports vp = {.n = 1}; vp.v[0][2] = 8; vp.v[0][3] = NROWS; EMIT(OP_VIEWPORTS, vp);
    p_clear_color cl = {.index = 0, .mask = 15, .rgba = {d[0], d[1], d[2], 1}}; EMIT(OP_CLEAR_COLOR, cl);
    p_bind_program bp = {&program, 3}; EMIT(OP_BIND_PROGRAM, bp);
    for (int r = 0; r < NROWS; r++) {
        if (!rows[r].a0) continue;
        p_bind_state bs = {.kind = 1}; color_st cs = {(uint32_t)rows[r].on, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; memcpy(bs.bytes, &cs, sizeof cs); EMIT(OP_BIND_STATE, bs);
        p_bind_state bb = {.kind = 0}; blend_st b = state(r); memcpy(bb.bytes, &b, sizeof b); EMIT(OP_BIND_STATE, bb);
        p_scissor sc = {0, r, 8, 1}; EMIT(OP_SCISSOR, sc);
        p_draw dr = {.prim = 4, .count = 3, .instances = 1}; EMIT(OP_DRAW, dr);
    }
    op_hdr end = {OP_END, 8}; memcpy(commands + used, &end, 8);
    #undef EMIT
    state_reset();
    memset(&R, 0, sizeof R); mtl_sub_begin(); mtl_exec(commands);
    id<MTLBuffer> pixels = [g_dev newBufferWithLength:256 * NROWS options:MTLResourceStorageModeShared];
    [blit() copyFromTexture:tex_mtl(&target) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(8, NROWS, 1) toBuffer:pixels destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256 * NROWS];
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull)); CHECK(!mtl_last_commit_error());
    // the word, on the state alone: a floating-point target 0, then the same state on a normalized target and through a merged pass's mapping
    const float src[4] = {NAN, INFINITY, -INFINITY, 2};
    for (int r = 0; r < NROWS; r++) {
        memset(&S.color, 0, sizeof S.color); S.color.blend_en = (uint32_t)rows[r].on; S.blend[0] = state(r);
        memset(g_pass.cfmt, 0, sizeof g_pass.cfmt); g_pass.cfmt[0] = MTLPixelFormatRGBA16Float;
        CHECK(bz_word(NULL) == (on ? rows[r].word : 0));
        g_pass.cfmt[0] = MTLPixelFormatRGBA8Unorm; CHECK(bz_word(NULL) == 0);   // not a float target: the option is the float blender's
        g_pass.cfmt[0] = MTLPixelFormatRG11B10Float; g_pass.cfmt[1] = MTLPixelFormatRGBA16Float;
        static const uint8_t lmap[8] = {0xff, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};   // output 0 is physical target 1
        CHECK(bz_word(lmap) == (on ? rows[r].word : 0));
        S.color.blend_en = rows[r].on ? 2 : 0; S.blend[1] = state(r); memset(&S.blend[0], 0, sizeof S.blend[0]);
        CHECK(bz_word(NULL) == (on ? rows[r].word << DMSH_BLEND_ZERO_BITS : 0));   // output 1's bits
    }
    // pso_blend_live / pso_off must read k->cfmt (not global g_pass.cfmt), so fsp_plan, fb_tile_ready, warm_task and pso_predict_rest_ never use another pass's formats
    {   memset(g_pass.cfmt, 0, sizeof g_pass.cfmt); g_pass.cfmt[0] = MTLPixelFormatRGBA8Unorm;
        pso_key k = {.cfmt = {MTLPixelFormatRGBA32Float}, .blend_en = 0, .chmask = 0xf};
        CHECK(pso_blend_live(&k, ps, 0) == 0 && pso_off(&k, ps, 0) == (FN_OFF_BORDER | FN_OFF_BLEND));
        memset(g_pass.cfmt, 0, sizeof g_pass.cfmt);
        k.cfmt[0] = MTLPixelFormatRGBA16Float; k.blend_en = 1; k.blend[0] = state(1);
        CHECK(pso_blend_live(&k, ps, 0) == (on ? 1 : 0));
        CHECK(pso_off(&k, ps, 0) == (on ? FN_OFF_BORDER : (FN_OFF_BORDER | FN_OFF_BLEND)));
        // Force FN_OFF_BLEND on the mist state with bz_word = 1 bound: dm_blend0 is compiled out, so Apple's blender stores NaN at alpha 0
        MTLRenderPipelineDescriptor *d = pso_desc(&k, pr->sh[0], ps, nil, 0);
        NSError *be = nil; id<MTLRenderPipelineState> off_ps = pso_build(d, FN_OFF_BORDER | FN_OFF_BLEND, &be);
        CHECK(off_ps != nil);
        if (off_ps) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:8 height:1 mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
            id<MTLTexture> rt = [g_dev newTextureWithDescriptor:td];
            uint32_t bz[2] = {1u, 0u}; id<MTLBuffer> bb = [g_dev newBufferWithBytes:bz length:16 options:MTLResourceStorageModeShared];
            CHECK(rt != nil && bb != nil);
            if (rt && bb) {
                id<MTLCommandBuffer> cb = q_cb(); MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new];
                rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1.0);
                id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
                [re setRenderPipelineState:off_ps]; [re setFragmentBuffer:bb offset:0 atIndex:DMSH_BLEND_BUFFER];
                [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [re endEncoding];
                [cb commit]; [cb waitUntilCompleted];
                CHECK(cb.status == MTLCommandBufferStatusCompleted);
                uint16_t raw[32] = {0}; [rt getBytes:raw bytesPerRow:64 fromRegion:MTLRegionMake2D(0, 0, 8, 1) mipmapLevel:0];
                CHECK(isnan(half(raw[0])) && isnan(half(raw[4])) && isnan(half(raw[8])));   // columns 0-2 (NaN, +Inf, -Inf at alpha 0) bypass dm_blend0 and become NaN
            }
        }
    }
    const uint16_t *px = pixels.contents;
    if (getenv("DM_T_DUMP")) for (int r = 0; r < NROWS; r++) { printf("row %d:", r); for (int x = 0; x < 8; x++) printf(" %g", half(px[r * 128 + x * 4])); printf("\n"); }   // the red channel as stored
    for (int r = 0; r < NROWS; r++) for (int x = 0; x < 8 && rows[r].a0; x++) {
        float s = src[x & 3]; char how = x < 4 ? rows[r].a0 : rows[r].a1; int fin = isfinite(s);
        if (!on && rows[r].word && how == 'd' && !fin) how = 'N';   // Apple's blender: 0 x NaN = 0 x Inf = NaN
        if (r == 5) { if (isinf(s) || (how == '+' && fin)) continue; if (how == '+') how = 's'; }
        for (int c = 0; c < 3; c++) {
            float got = half(px[r * 128 + x * 4 + c]), want = how == 'd' ? d[c] : how == 's' ? s : how == '+' ? s + d[c] : how == 'n' ? (fin ? d[c] * s : NAN) : NAN;
            int ok = isnan(want) ? isnan(got) : got == want;
            if (!ok) fprintf(stderr, "row %d x %d channel %d: got %g, want %g\n", r, x, c, got, want);
            CHECK(ok);
        }
    }
    mtl_pool_del(commands); mtl_drain(); free(commands);
    printf("blend zero replay (%s): %d checks, %d failed\n", on ? "NVIDIA's rule" : "DM_NVN_BLEND_ZERO=0, Apple's blender", checks, failed);
    return failed != 0;
} }
