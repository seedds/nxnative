// NVIDIA's float to normalized conversion through the replay: a pixel shader that writes 256 colours (half steps of 8, 16, 10 and 2 bits, where NVIDIA's colour unit and Apple's
// differ, then the edges; dmsh_rewrite.py blend_zero's guard in it, from test_dmsh_blend_zero.py --fixture-rop) drawn into RGBA8, RGBA16 and RGB10A2 normalized targets with blending
// off, with MAX, and with an ADD of ONE and ZERO (a blended sum: rounded after the blend, out of the shader's reach), and the targets read back. Where the stored value is the
// colour's conversion, it is NVIDIA's: the float32 product rounded to nearest even. The ADD row, and every row with DM_NVN_ROP_ROUND=0, show Apple's conversion as it is: the
// exact product rounded to nearest (Sonic Frontiers' flags 66.5 / 255: 66 against 67). rop_word's cases are checked on the state alone.
// make GAME=<id> check_t_rop_round (./t_rop_round <dir>)
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static unsigned nvidia_unorm(float x, unsigned s) { if (x != x) return 0; x = fminf(fmaxf(x, 0.f), 1.f); volatile float p = x * (float)s; return (unsigned)rintf(p); }   // float32 product, nearest even
static unsigned apple_unorm(float x, unsigned s) { if (x != x) return 0; x = fminf(fmaxf(x, 0.f), 1.f); return (unsigned)rint((double)x * s); }   // exact in a double (24 + 17 bits)
enum { O = NVN_BLEND_FUNC_ONE, Z = NVN_BLEND_FUNC_ZERO, ADD = NVN_BLEND_EQUATION_ADD, MAX = NVN_BLEND_EQUATION_MAX, MIN = NVN_BLEND_EQUATION_MIN };
// rows of every target: blending off, MAX on colour and alpha, a sum of ONE and ZERO
static const struct { int on, eq; int rounds; } rows[] = {{0, ADD, 1}, {1, MAX, 1}, {1, ADD, 0}};
enum { NROWS = sizeof rows / sizeof *rows, W = 256 };
static const struct { int nvn; MTLPixelFormat mtl; unsigned bpp; uint32_t cls; } fmts[] = {
    {NVN_FORMAT_RGBA8, MTLPixelFormatRGBA8Unorm, 4, DMSH_ROUND_8},
    {NVN_FORMAT_RGBA16, MTLPixelFormatRGBA16Unorm, 8, DMSH_ROUND_16},
    {NVN_FORMAT_RGB10A2, MTLPixelFormatRGB10A2Unorm, 4, DMSH_ROUND_10_2},
};
enum { NFMT = sizeof fmts / sizeof *fmts };
static void stored(int f, const uint8_t *px, unsigned out[4], unsigned sc[4]) {
    if (fmts[f].cls == DMSH_ROUND_8) { for (int c = 0; c < 4; c++) { out[c] = px[c]; sc[c] = 255; } }
    else if (fmts[f].cls == DMSH_ROUND_16) { const uint16_t *p = (const uint16_t *)px; for (int c = 0; c < 4; c++) { out[c] = p[c]; sc[c] = 65535; } }
    else { uint32_t v; memcpy(&v, px, 4); for (int c = 0; c < 3; c++) { out[c] = v >> (10 * c) & 1023; sc[c] = 1023; } out[3] = v >> 30; sc[3] = 3; }
}
static blend_st state(int r) { blend_st b = {.target = 0, .src_rgb = O, .dst_rgb = Z, .src_a = O, .dst_a = Z, .eq_rgb = (uint64_t)rows[r].eq, .eq_a = (uint64_t)rows[r].eq}; return b; }
int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) return 2;
    setenv("DM_MSL_DIR", argv[1], 1);
    mtl_init(); g_msl_dir = @(argv[1]);
    int on = rop_on();
    NSError *error = nil; NSString *source = [NSString stringWithContentsOfFile:[g_msl_dir stringByAppendingString:@"/rop.metal"] encoding:NSUTF8StringEncoding error:&error];
    if (!source) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    NSData *vals = [NSData dataWithContentsOfFile:[g_msl_dir stringByAppendingString:@"/rop.bin"]];
    if (vals.length != W * 4) { fprintf(stderr, "rop.bin: %lu bytes\n", (unsigned long)vals.length); abort(); }
    const float *v = vals.bytes;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:source options:nil error:&error];
    if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); abort(); }
    NvProg *pr = [NvProg new];
    pr->sh[0] = [NvShader new]; pr->sh[0]->fn = [lib newFunctionWithName:@"vertex_main"]; pr->sh[0]->stage = 0; snprintf(pr->sh[0]->key, sizeof pr->sh[0]->key, "vrop_b");
    NvShader *ps = [NvShader new]; pr->sh[1] = ps;
    ps->fn = fn4_note([lib newFunctionWithName:@"main0"], lib, @"main0", nil); ps->stage = 1; ps->has_bz = 1; snprintf(ps->key, sizeof ps->key, "prop_b");
    CHECK(ps->fn != nil);
    static prog_t program; program.serial = 98300; serial_put(program.serial, pr);
    static tex_t target[NFMT];
    for (int f = 0; f < NFMT; f++) {
        target[f] = (tex_t){.magic = MAGIC('T', 'E', 'X', 0), .serial = 98301 + f};
        target[f].d = (nvn_texdesc){.width = W, .height = NROWS, .depth = 1, .samples = 1, .levels = 1, .target = NVN_TEXTURE_TARGET_2D, .format = fmts[f].nvn};
    }
    uint8_t *commands; if (posix_memalign((void **)&commands, getpagesize(), 65536)) abort(); memset(commands, 0, 65536); mtl_pool_add(commands, 65536);
    size_t used = 0;
    #define EMIT(OP, X) do { size_t z = (8 + sizeof(X) + 7) & ~7ull; op_hdr h = {OP, (uint32_t)z}; memcpy(commands + used, &h, 8); memcpy(commands + used + 8, &(X), sizeof(X)); used += z; } while (0)
    p_bind_program bp = {&program, 3}; EMIT(OP_BIND_PROGRAM, bp);
    for (int f = 0; f < NFMT; f++) {
        p_set_rt rt = {.n = 1}; rt.color[0] = &target[f]; EMIT(OP_SET_RT, rt);
        p_viewports vp = {.n = 1}; vp.v[0][2] = W; vp.v[0][3] = NROWS; EMIT(OP_VIEWPORTS, vp);
        p_clear_color cl = {.index = 0, .mask = 15, .rgba = {0, 0, 0, 0}}; EMIT(OP_CLEAR_COLOR, cl);   // MAX over 0 keeps the colour
        for (int r = 0; r < NROWS; r++) {
            p_bind_state bs = {.kind = 1}; color_st cs = {(uint32_t)rows[r].on, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; memcpy(bs.bytes, &cs, sizeof cs); EMIT(OP_BIND_STATE, bs);
            p_bind_state bb = {.kind = 0}; blend_st b = state(r); memcpy(bb.bytes, &b, sizeof b); EMIT(OP_BIND_STATE, bb);
            p_scissor sc = {0, r, W, 1}; EMIT(OP_SCISSOR, sc);
            p_draw dr = {.prim = 4, .count = 3, .instances = 1}; EMIT(OP_DRAW, dr);
        }
    }
    op_hdr end = {OP_END, 8}; memcpy(commands + used, &end, 8);
    #undef EMIT
    state_reset();
    memset(&R, 0, sizeof R); mtl_sub_begin(); uint64_t handle = (uint64_t)commands; mtl_sub_index(&handle, 1); mtl_exec(commands);
    id<MTLBuffer> pixels[NFMT];
    for (int f = 0; f < NFMT; f++) {
        pixels[f] = [g_dev newBufferWithLength:W * 8 * NROWS options:MTLResourceStorageModeShared];
        [blit() copyFromTexture:tex_mtl(&target[f]) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, NROWS, 1) toBuffer:pixels[f] destinationOffset:0 destinationBytesPerRow:W * fmts[f].bpp destinationBytesPerImage:W * fmts[f].bpp * NROWS];
    }
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull)); CHECK(!mtl_last_commit_error());
    for (int f = 0; f < NFMT; f++) {
        int differ = 0, flags_seen = 0;
        for (int r = 0; r < NROWS; r++) for (int x = 0; x < W; x++) {
            unsigned got[4], sc[4]; stored(f, (const uint8_t *)pixels[f].contents + (r * W + x) * fmts[f].bpp, got, sc);
            for (int c = 0; c < 4; c++) {
                unsigned nv = nvidia_unorm(v[x], sc[c]), ap = apple_unorm(v[x], sc[c]), want = on && rows[r].rounds ? nv : ap;
                if (r == 0) differ += nv != ap;
                if (got[c] != want) fprintf(stderr, "%s row %d x %d (%.9g) channel %d: got %u, want %u (NVIDIA %u, Apple %u)\n",
                                            f == 0 ? "RGBA8" : f == 1 ? "RGBA16" : "RGB10A2", r, x, v[x], c, got[c], want, nv, ap);
                CHECK(got[c] == want);
                if (f == 0 && c == 3 && v[x] == 0.260784327983856201171875f) { flags_seen = 1; CHECK(got[c] == (on && rows[r].rounds ? 66u : 67u)); }
            }
        }
        CHECK(differ > 20);   // the colours reach the cases where the two differ
        if (f == 0) CHECK(flags_seen);
    }
    // the word on the state alone: by target format, blend state, logic operation and a merged pass's mapping
    memset(g_pass.cfmt, 0, sizeof g_pass.cfmt); g_pso_lmap = NULL; memset(&S.color, 0, sizeof S.color); S.color.logic_op = NVN_LOGIC_OP_COPY;
    for (int i = 0; i < 8; i++) S.blend[i] = state(0);
    static const struct { MTLPixelFormat f; uint32_t cls; } cases[] = {
        {MTLPixelFormatRGBA8Unorm, DMSH_ROUND_8}, {MTLPixelFormatBGRA8Unorm, DMSH_ROUND_8}, {MTLPixelFormatR8Unorm, DMSH_ROUND_8}, {MTLPixelFormatRG8Unorm, DMSH_ROUND_8},
        {MTLPixelFormatRGBA16Unorm, DMSH_ROUND_16}, {MTLPixelFormatR16Unorm, DMSH_ROUND_16}, {MTLPixelFormatRGB10A2Unorm, DMSH_ROUND_10_2}, {MTLPixelFormatBGR10A2Unorm, DMSH_ROUND_10_2},
        {MTLPixelFormatRGBA8Unorm_sRGB, 0}, {MTLPixelFormatRGBA8Snorm, 0}, {MTLPixelFormatRGBA16Float, 0}, {MTLPixelFormatRGBA8Uint, 0}, {MTLPixelFormatB5G6R5Unorm, 0},
    };
    for (size_t k = 0; k < sizeof cases / sizeof *cases; k++) {
        g_pass.cfmt[0] = cases[k].f; g_pass.cfmt[3] = cases[k].f;
        CHECK(rop_word() == (on ? cases[k].cls | cases[k].cls << (3 * DMSH_BLEND_ROUND_BITS) : 0));
    }
    g_pass.cfmt[3] = 0; g_pass.cfmt[0] = MTLPixelFormatRGBA8Unorm;
    S.color.blend_en = 1; S.blend[0] = state(2); CHECK(rop_word() == 0);   // a blended sum
    S.blend[0].eq_rgb = MIN; S.blend[0].eq_a = MAX; CHECK(rop_word() == (on ? DMSH_ROUND_8 : 0u));   // MIN / MAX on both
    S.blend[0].eq_a = ADD; CHECK(rop_word() == 0);   // the alpha blended
    S.color.logic_op = NVN_LOGIC_OP_XOR; CHECK(rop_word() == (on ? DMSH_ROUND_8 : 0u));   // a logic operation takes the place of blending
    S.color.logic_op = NVN_LOGIC_OP_COPY; S.color.blend_en = 0;
    static const uint8_t lmap[8] = {0xff, 2, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}; g_pso_lmap = lmap;   // physical target 1 is output 2
    g_pass.cfmt[0] = 0; g_pass.cfmt[1] = MTLPixelFormatRGBA16Unorm; CHECK(rop_word() == (on ? (uint32_t)DMSH_ROUND_16 << (2 * DMSH_BLEND_ROUND_BITS) : 0u));
    g_pso_lmap = NULL;
    mtl_pool_del(commands); mtl_drain(); free(commands);
    printf("normalized colour rounding replay (%s): %d checks, %d failed\n", on ? "NVIDIA's rule" : "DM_NVN_ROP_ROUND=0, Apple's conversion", checks, failed);
    return failed != 0;
} }
