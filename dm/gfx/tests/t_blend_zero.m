// NVIDIA's "zero times anything is zero" float blending through the replay: a pixel shader that writes NaN, +Inf, -Inf and 2 at alpha 0 and at alpha 1 (dmsh_rewrite.py blend_zero's
// guard in it, from test_dmsh_blend_zero.py --fixture), drawn into an RGBA16F target under blend states with and without a zero source weight, and the target read back. Where the
// weight is zero by the alpha the target keeps its colour; everywhere else the non-finite colour is stored, as NVIDIA stores it. With DM_NVN_BLEND_ZERO=0 the same draws show Apple's blender as
// it is: the NaN is stored under the zero weight too (what the guard is for). bz_word's cases are checked on the state alone, and so is the rule that compiles the guard out
// (DMSH_BLEND_FC, pso_key.bz_off, fn_off): the pipelines the replay built are looked up by key, and each is specialized without the guard exactly where its word is 0.
// make GAME=<id> check_t_blend_zero (./t_blend_zero <dir>)
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)
// stderr into a file for a while (the host's red lines are what the checks read), then back, the text echoed
#include <fcntl.h>
static int g_save_fd = -1; static char g_cap_path[1024];
static void cap_begin(void) {
    fflush(stderr); g_save_fd = dup(2); int fd = open(g_cap_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (g_save_fd < 0 || fd < 0) { fprintf(stderr, "cannot capture stderr into %s\n", g_cap_path); abort(); }
    dup2(fd, 2); close(fd);
}
static NSString *cap_end(int echo) {
    fflush(stderr); dup2(g_save_fd, 2); close(g_save_fd); g_save_fd = -1;
    NSString *s = [NSString stringWithContentsOfFile:@(g_cap_path) encoding:NSUTF8StringEncoding error:nil] ?: @"";
    if (echo) fputs(s.UTF8String, stderr);
    return s;
}
static int count_of(NSString *s, NSString *what) { return (int)[[s componentsSeparatedByString:what] count] - 1; }
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
// A merged pass's draw (g_pso_lmap, the map from the physical attachments to the shader's outputs; g_pso_fsfn, the fragment that writes each output to its attachment): the word it binds is the
// state's under the map, as the pipeline's is. The state: the shader's output 0 (SRC_ALPHA / ONE_MINUS_SRC_ALPHA) goes to physical attachment 1, an RGBA16F target, beside an RGBA8 one at attachment 0
// that no output reaches; output 1's blending is off. Read without the map, the word is 0 (attachment 0 is no float target, output 1 is not blending) and the NaN of alpha 0 is stored. The merged
// fragment is the fixture's body with its colour written to color(1)
static void merged_case(NSString *source, prog_t *program, int on) {
    NSString *merged = [source stringByAppendingString:@"\nstruct dm_merged_out { float4 m_1 [[color(1)]]; };\n"
        "fragment dm_merged_out main_m(float4 gl_FragCoord [[position]], constant dm_blend_state& dm_blend [[buffer(28)]])\n"
        "{\n    main0_out out = {};\n    uint x = uint(gl_FragCoord.x);\n    uint k = x & 3u;\n"
        "    float v = as_type<float>(k == 0u ? 0x7fc00000u : (k == 1u ? 0x7f800000u : (k == 2u ? 0xff800000u : 0x40000000u)));\n"
        "    out.m_0 = float4(v, v, v, x < 4u ? 0.0 : 1.0);\n    dm_blend0(out, dm_blend);\n    dm_merged_out r = {};\n    r.m_1 = out.m_0;\n    return r;\n}\n"];
    NSError *error = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:merged options:nil error:&error];
    CHECK(lib != nil); if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return; }
    id<MTLFunction> fn = fn_spec(lib, @"main_m", [MTLFunctionConstantValues new], &error);
    CHECK(fn != nil); if (!fn) return;
    tex_t unused = {.magic = MAGIC('T', 'E', 'X', 0), .serial = 98202}, target = {.magic = MAGIC('T', 'E', 'X', 0), .serial = 98203};
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
    g_pso_lmap = lmap; g_pso_fsfn = fn;   // as draw has them for a merged pass; it clears them after its pipeline
    cap_begin();
    mtl_sub_begin(); uint64_t handle = (uint64_t)cmds; mtl_sub_index(&handle, 1); mtl_exec(cmds);
    id<MTLBuffer> pixels = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    CHECK(pixels != nil); if (!pixels) { (void)cap_end(1); return; }
    [blit() copyFromTexture:tex_mtl(&target) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(8, 1, 1) toBuffer:pixels destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256];
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull)); CHECK(!mtl_last_commit_error());
    NSString *log = cap_end(1);
    // the word the draw bound, from bz_bind's line: the state's under the map (output 0's bit), none without the rule
    CHECK([[log componentsSeparatedByString:@"(outputs 0x1)"] count] - 1 == (on ? 1u : 0u));
    const float src[4] = {NAN, INFINITY, -INFINITY, 2}; const uint16_t *px = pixels.contents;
    for (int x = 0; x < 8; x++) {
        float s = src[x & 3]; int fin = isfinite(s);
        char how = x < 4 ? 'd' : 's';   // the mist's state: nothing added at alpha 0, the colour at alpha 1
        if (!on && x < 4 && !fin) how = 'N';   // Apple's blender: 0 x NaN = 0 x Inf = NaN
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
// fn_off sets each constant its entry declares and no other: a probe kernel per declaration reads both back on the GPU
static void constant_cases(void) {
    NSString *src = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "constant bool dm_border_off [[function_constant(%u)]];\nconstant bool dm_blend_off [[function_constant(%u)]];\n"
        "constant bool bo = !is_function_constant_defined(dm_border_off) || !dm_border_off;\n"
        "constant bool bl = !is_function_constant_defined(dm_blend_off) || !dm_blend_off;\n"
        "kernel void both(device uint *out [[buffer(0)]]) { out[0] = uint(bo) | uint(bl) << 1; }\n"
        "kernel void border_only(device uint *out [[buffer(0)]]) { out[0] = uint(bo); }\n"
        "kernel void blend_only(device uint *out [[buffer(0)]]) { out[0] = uint(bl); }\n", (unsigned)DMSH_BORDER_FC, (unsigned)DMSH_BLEND_FC];
    NSError *error = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&error];
    CHECK(lib != nil); if (!lib) return;
    // by mask 0..3: what each kernel reads (a constant left live reads 1), and whether the mask has a variant of the entry
    static const struct { NSString *name; int want[4], variant[4]; } k[] = {
        {@"both",        {3, 2, 1, 0}, {0, 1, 1, 1}},
        {@"border_only", {1, 0, 1, 0}, {0, 1, 0, 1}},   // the blend bit finds nothing to set: no variant, the function as it is
        {@"blend_only",  {1, 1, 0, 0}, {0, 0, 1, 1}},
    };
    for (int i = 0; i < 3; i++) {
        id<MTLFunction> f = fn_spec(lib, k[i].name, [MTLFunctionConstantValues new], &error);
        CHECK(f != nil); if (!f) continue;
        for (int m = 0; m < 4; m++) {
            CHECK((fn4_of_b(f, m) != fn4_of(f)) == k[i].variant[m]);
            CHECK(fn4_of_b(f, m) == fn4_of_b(f, m));   // cached
            id<MTLComputePipelineState> p = [g_dev newComputePipelineStateWithFunction:fn_dev_b(f, m) error:&error];
            CHECK(p != nil); if (!p) continue;
            id<MTLBuffer> out = [g_dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = q_cb(); id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
            [e setComputePipelineState:p]; [e setBuffer:out offset:0 atIndex:0];
            [e dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
            CHECK(cb.status == MTLCommandBufferStatusCompleted && *(uint32_t *)out.contents == (uint32_t)k[i].want[m]);
        }
        MTL4SpecializedFunctionDescriptor *pre = (MTL4SpecializedFunctionDescriptor *)pre_fn(lib, k[i].name);   // the prebuild takes every declared constant off
        CHECK(fn_lib_spec(lib, k[i].name, pre.constantValues, &error) != nil);
    }
}
// bz_bind's check of the invariant (bz_audit) through real draws: one pipeline made with bz_off (row calm: its word is 0), drawn, then the blend state changed to row needy's (a float target
// with SRC_ALPHA: word 1) behind the pipeline's back, as a missed pipeline-dirty mark would leave it, and drawn again from the same pipeline. The second draw writes the red line, the first none
static void audit_forced(prog_t *program, tex_t *target, int calm, int needy) {
    uint8_t *cmds; if (posix_memalign((void **)&cmds, getpagesize(), 65536)) abort(); memset(cmds, 0, 65536); mtl_pool_add(cmds, 65536);
    size_t used = 0, second = 0;
    #define EMIT2(OP, X) do { size_t z = (8 + sizeof(X) + 7) & ~7ull; op_hdr h = {OP, (uint32_t)z}; memcpy(cmds + used, &h, 8); memcpy(cmds + used + 8, &(X), sizeof(X)); used += z; } while (0)
    p_set_rt rt = {.n = 1}; rt.color[0] = target; EMIT2(OP_SET_RT, rt);
    p_viewports vp = {.n = 1}; vp.v[0][2] = 8; vp.v[0][3] = NROWS; EMIT2(OP_VIEWPORTS, vp);
    p_bind_program bp = {program, 3}; EMIT2(OP_BIND_PROGRAM, bp);
    p_bind_state bs = {.kind = 1}; color_st cs = {1, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; memcpy(bs.bytes, &cs, sizeof cs); EMIT2(OP_BIND_STATE, bs);
    p_bind_state bb = {.kind = 0}; blend_st b = state(calm); memcpy(bb.bytes, &b, sizeof b); EMIT2(OP_BIND_STATE, bb);
    p_scissor sc = {0, 0, 8, 1}; EMIT2(OP_SCISSOR, sc);
    p_draw dr = {.prim = 4, .count = 3, .instances = 1}; EMIT2(OP_DRAW, dr);
    op_hdr end = {OP_END, 8}; memcpy(cmds + used, &end, 8); used += 8; second = used;
    EMIT2(OP_DRAW, dr);
    memcpy(cmds + used, &end, 8);
    #undef EMIT2
    state_reset(); memset(&R, 0, sizeof R); g_draw_bzoff = 0;
    cap_begin();
    mtl_sub_begin(); uint64_t hs[2] = {(uint64_t)cmds, (uint64_t)(cmds + second)}; mtl_sub_index(hs, 2);
    mtl_exec(cmds);
    NSString *before = cap_end(1);
    CHECK(g_draw_bzoff == 1 && count_of(before, @"blend-zero guard compiled out") == 0);   // the calm state's pipeline is the one without the guard, and its word is 0
    cap_begin();
    S.blend[0] = state(needy);   // no g_pso_dirty: the next draw takes the pipeline it has
    mtl_exec(cmds + second);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull)); CHECK(!mtl_last_commit_error());
    NSString *log = cap_end(0);
    int want = bz_on() ? 1 : 0;   // with DM_NVN_BLEND_ZERO=0 the word is 0 whatever the state: nothing is wrong
    CHECK(count_of(log, @"[RED] blend-zero guard compiled out but this draw needs it: fragment pblend_b, word 0x1, blend_en 0x1, formats 115 0 0 0 0 0 0 0, out0 src 5 dst 6 eq 1 (1 draws so far)") == want);
    CHECK(count_of(log, @"blend-zero guard compiled out") == want);
    mtl_pool_del(cmds); mtl_drain(); free(cmds);
}
int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) return 2;
    snprintf(g_cap_path, sizeof g_cap_path, "%s/stderr.txt", argv[1]);
    setenv("DM_MSL_DIR", argv[1], 1);
    mtl_init(); g_msl_dir = @(argv[1]);
    int on = bz_on();
    NSError *error = nil; NSString *source = [NSString stringWithContentsOfFile:[g_msl_dir stringByAppendingString:@"/blend.metal"] encoding:NSUTF8StringEncoding error:&error];
    if (!source) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:source options:nil error:&error];
    if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    NvProg *pr = [NvProg new];
    pr->sh[0] = [NvShader new]; pr->sh[0]->fn = fn4_note([lib newFunctionWithName:@"vertex_main"], lib, @"vertex_main", nil); pr->sh[0]->stage = 0; snprintf(pr->sh[0]->key, sizeof pr->sh[0]->key, "vblend_b");
    NvShader *ps = [NvShader new]; pr->sh[1] = ps;
    ps->fn = fn_spec(lib, @"main0", [MTLFunctionConstantValues new], &error); ps->stage = 1; ps->has_bz = 1; snprintf(ps->key, sizeof ps->key, "pblend_b");
    CHECK(ps->fn != nil);
    static prog_t program; program.serial = 98200; serial_put(program.serial, pr);
    merged_case(source, &program, on);
    const uint32_t merged_bases = g_pbase.n;   // the merged pass's own base is not one of the rows'
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
    cap_begin();
    memset(&R, 0, sizeof R); mtl_sub_begin(); uint64_t handle = (uint64_t)commands; mtl_sub_index(&handle, 1); mtl_exec(commands);
    id<MTLBuffer> pixels = [g_dev newBufferWithLength:256 * NROWS options:MTLResourceStorageModeShared];
    [blit() copyFromTexture:tex_mtl(&target) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(8, NROWS, 1) toBuffer:pixels destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256 * NROWS];
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull)); CHECK(!mtl_last_commit_error());
    CHECK(count_of(cap_end(1), @"blend-zero guard compiled out") == 0);   // every draw above had the word its pipeline was made for: the check bz_bind makes at each is silent
    // the word, on the state alone: a floating-point target 0, then the same state on a normalized target and through a merged pass's mapping
    const float src[4] = {NAN, INFINITY, -INFINITY, 2};
    for (int r = 0; r < NROWS; r++) {
        memset(&S.color, 0, sizeof S.color); S.color.blend_en = (uint32_t)rows[r].on; S.blend[0] = state(r);
        memset(g_pass.cfmt, 0, sizeof g_pass.cfmt); g_pass.cfmt[0] = MTLPixelFormatRGBA16Float;
        CHECK(bz_word(NULL) == (on ? rows[r].word : 0));
        g_pass.cfmt[0] = MTLPixelFormatRGBA8Unorm; CHECK(bz_word(NULL) == 0);   // not a float target: the option is the float blender's
        g_pass.cfmt[0] = MTLPixelFormatRG11B10Float; g_pass.cfmt[1] = MTLPixelFormatRGBA16Float;
        static const uint8_t lmap[8] = {0xff, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}; // output 0 is physical target 1
        CHECK(bz_word(lmap) == (on ? rows[r].word : 0));
        S.color.blend_en = rows[r].on ? 2 : 0; S.blend[1] = state(r); memset(&S.blend[0], 0, sizeof S.blend[0]);
        CHECK(bz_word(NULL) == (on ? rows[r].word << DMSH_BLEND_ZERO_BITS : 0));   // output 1's bits
    }
    // the key's flag on the state alone (pso_key_fill, bz_off), and each pipeline the replay drew with is in the caches by it, its base specialized by pso_off's mask
    for (int r = 0; r < NROWS; r++) {
        memset(&S.color, 0, sizeof S.color); S.color.blend_en = (uint32_t)rows[r].on; S.blend[0] = state(r); g_pso_lmap = NULL;
        const uint32_t cf[8] = {MTLPixelFormatRGBA16Float}, cn[8] = {MTLPixelFormatRGBA8Unorm};
        uint32_t word = on ? rows[r].word : 0;
        pso_key k; pso_key_fill(&k, pr->sh[0], ps, cf, 0, 0, 1, 0);
        CHECK(k.bz_off == !word);
        pso_key kn; pso_key_fill(&kn, pr->sh[0], ps, cn, 0, 0, 1, 0);
        CHECK(kn.bz_off == 1);   // a normalized target: the guard goes whatever the blend state
        if (!rows[r].a0) continue;
        CHECK(hm_get_id(&g_pso, &k) != nil);   // the draw's pipeline was built from this key
        if (g_comp4) {
            pbase_key bk; pbase_key_of(pso_desc(&k, pr->sh[0], ps, nil, 0), pso_off(&k), &bk);
            CHECK(bk.off == (FN_OFF_BORDER | (word ? 0 : FN_OFF_BLEND)) && hm_get_id(&g_pbase, &bk) != nil);
        }
        ps->has_bz = 0; pso_key kx; pso_key_fill(&kx, pr->sh[0], ps, cf, 0, 0, 1, 0); ps->has_bz = 1;
        CHECK(kx.bz_off == 0);   // a shader without the guard declares no constant: nothing to specialize
        static const uint8_t lmap[8] = {0xff, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};   // a merged pass: output 0 is physical target 1
        const uint32_t cm[8] = {MTLPixelFormatRG11B10Float, MTLPixelFormatRGBA16Float};
        pso_key km; pso_key_fill(&km, pr->sh[0], ps, cm, 0, 0, 1, 1);
        CHECK(km.bz_off == 0);   // until pso_get fills in lmap, the guard stays
        memcpy(km.lmap, lmap, 8); km.bz_off = pso_key_bz_off(&km, ps, 1);
        CHECK(km.bz_off == !word);
    }
    if (g_comp4) CHECK(g_pbase.n - merged_bases == (on ? 2u : 1u));   // the guard live and specialized away: two bases with the rule, one without it (the word is always 0)
    memset(&S.color, 0, sizeof S.color);
    constant_cases();
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
    audit_forced(&program, &target, 2, 1);
    mtl_pool_del(commands); mtl_drain(); free(commands);
    printf("blend zero replay (%s): %d checks, %d failed\n", on ? "NVIDIA's rule" : "DM_NVN_BLEND_ZERO=0, Apple's blender", checks, failed);
    return failed != 0;
} }
