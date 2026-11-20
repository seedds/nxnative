// Border colours Metal has no sampler state for (nvn_border.h, dmsh_rewrite.py border_emu): nvn_border's choice of Metal colours and factors, the production sampler registry's twin
// (mtl_reg_samp, samp_by_id), and, given the path of a shader border_emu rewrote (test_dmsh_border.py writes it), the GPU: samples through the sampler, its twin and the block come
// out as Maxwell's do, bilinear between the texels and the border colour, and with the block's mask clear as Metal's nearest colour
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

enum { W = 4, H = 4, NUV = 9 };
static const float UV[NUV][2] = { {-0.1f, 0.5f}, {0.5f, 1.05f}, {1.2f, 1.2f}, {0.06f, 0.94f}, {0.5f, 0.5f}, {0.f, 0.f}, {1.f, 0.3f}, {0.97f, -0.02f}, {0.3f, 0.6f} };
static float texel(int x, int y, int c) { const float v[4] = { (float)x / W, (float)y / H, (float)(x + y) / 8, 1 - (float)x / 8 }; return v[c]; }
static void reference(const float B[4], float u, float v, float out[4]) {   // Maxwell's linear filter: texel centres at (i + 0.5) / size, texels outside read the border colour
    float x = u * W - 0.5f, y = v * H - 0.5f; int x0 = (int)floorf(x), y0 = (int)floorf(y); float fx = x - x0, fy = y - y0;
    for (int c = 0; c < 4; c++) {
        float t[2][2];
        for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) { int xi = x0 + i, yj = y0 + j; t[j][i] = xi < 0 || yj < 0 || xi >= W || yj >= H ? B[c] : texel(xi, yj, c); }
        out[c] = (t[0][0] * (1 - fx) + t[0][1] * fx) * (1 - fy) + (t[1][0] * (1 - fx) + t[1][1] * fx) * fy;
    }
}

static void choice(const float B[4], uint8_t wrap, int reduction, int want_twin, int want_bn) {
    const uint8_t w[3] = { wrap, wrap, 0 }; int bn = -1, bw = -1; float k[4] = {0};
    int twin = nvn_border(B, w, reduction, &bn, &bw, k);
    CHECK(twin == want_twin, "border %g,%g,%g,%g wrap %d reduction %d: twin %d, expected %d", B[0], B[1], B[2], B[3], wrap, reduction, twin, want_twin);
    if (want_bn >= 0) CHECK(bn == want_bn, "border %g,%g,%g,%g: Metal colour %d, expected %d", B[0], B[1], B[2], B[3], bn, want_bn);
    if (!twin) return;
    CHECK(bw != bn && bw >= 0 && bw < 3, "border %g,%g,%g,%g: twin colour %d beside %d", B[0], B[1], B[2], B[3], bw, bn);
    for (int c = 0; c < 4; c++) {   // the correction gives B back where only the border is read
        float got = nvn_mtl_border[bn][c] + (nvn_mtl_border[bw][c] - nvn_mtl_border[bn][c]) * k[c];
        CHECK(fabsf(got - B[c]) < 1e-6f, "border %g,%g,%g,%g channel %d: corrected %g", B[0], B[1], B[2], B[3], c, got);
    }
}

static id<MTLTexture> g_tex;
// Pipeline selection happens before lazy Metal sampler creation. Exercise the real registry and bindings without needing a Metal device.
static void binding_cases(void) {
    NvShader *sh = [NvShader new]; sh->rf.border = 1; sh->rf.nb = 1;
    nvn_rbind *b = &sh->rf.b[0]; b->kind = NVN_RB_SAMPT; b->reg = 7; b->bw = 1;
    samp_par sp = {0}; sp.ws = sp.wt = sp.wr = 5;
    memcpy(sp.border, (float[4]){0.5f, 0.5f, 0.5f, 0.5f}, 16);
    CHECK(!border_need(nil, 0) && !border_need(sh, 0), "no shader / no registered custom border needs no correction");
    mtl_reg_samp(31, &sp);
    CHECK(!g_sview[31] && !g_sview_w[31], "selection precedes lazy sampler creation");
    for (int stage = 0; stage < 6; stage++) {
        S.tex[stage][7] = (uint64_t)31 << 20;
        CHECK(border_need(sh, stage), "stage %d keeps a custom border on its first draw", stage);
        S.tex[stage][7] = 0;
        CHECK(!border_need(sh, stage), "stage %d stops correcting after the sampler is unbound", stage);
    }
    {   // what the draw compares with its last pipeline's (prog_border): it follows the pixel stage's bindings with the program unchanged, and no other stage's
        NvProg *pr = [NvProg new]; pr->sh[1] = sh;
        CHECK(!prog_border(pr), "a program whose samplers have no twin keeps its specialized pipeline");
        S.tex[1][7] = (uint64_t)31 << 20; CHECK(prog_border(pr), "binding a sampler with a twin changes the draw's pipeline, the program the same");
        S.tex[1][7] = 0; S.tex[0][7] = (uint64_t)31 << 20; CHECK(!prog_border(pr), "another stage's binding does not");
        S.tex[0][7] = 0;
    }
    b->kind = NVN_RB_SAMP; S.samp[3][7] = (uint64_t)31 << 20;
    CHECK(border_need(sh, 3) && !border_need(sh, 4), "separate sampler bindings use the task / mesh stage's own state");
    b->kind = NVN_RB_SAMPU; b->soff = DMSH_C2_SAMP + 7 * DMSH_C2_ROW + 1; b->sreg = DMSH_SREG_C2;
    CHECK(border_need(sh, 3) && !border_need(sh, 4), "a c2 separate sampler word selects the same border");
    b->soff = 0; b->kind = NVN_RB_SAMPI; b->reg = 31;
    CHECK(border_need(sh, 1), "constant sampler ID keeps its custom border");
    b->bw = 0; CHECK(!border_need(sh, 1), "a sample without an emulated twin does not specialize the pipeline"); b->bw = 1;
    sh->rf.border = 0; CHECK(!border_need(sh, 1), "a shader without border emulation needs no correction"); sh->rf.border = 1;
    memset(sp.border, 0, 16); mtl_reg_samp(31, &sp);
    CHECK(!border_need(sh, 1), "re-registering the same ID with a native Metal colour removes the need");
    memcpy(sp.border, (float[4]){0.5f, 0.5f, 0.5f, 0.5f}, 16); mtl_reg_samp(31, &sp);
    CHECK(border_need(sh, 1), "re-registering the ID with a custom colour restores the need");
    sp.ws = sp.wt = sp.wr = 0; mtl_reg_samp(31, &sp);
    CHECK(!border_need(sh, 1), "wrapping without a border needs no correction");
    sp.ws = sp.wt = sp.wr = 5; sp.reduction = 1; mtl_reg_samp(31, &sp);
    CHECK(!border_need(sh, 1), "min reduction keeps its existing nearest-colour behavior");
    b->reg = g_nsreg + 1; CHECK(!border_need(sh, 1), "an unregistered sampler needs no correction");
    S.samp[3][7] = 0;
}
// A synthetic kernel makes specialization observable as a word on the GPU. Production kernels do not take BORDER_FC; the same host specialization serves render functions.
// Each case has a kernel of its own content: Metal hands out one function object per content, so one shared kernel would carry the first case's notes into the next
static void specialization_cases(void) {
    for (int deferred = 0; deferred < 3; deferred++) {
        NSString *src = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n"
            "constant uint prior [[function_constant(0)]];\nconstant bool dm_border_off [[function_constant(%u)]];\n"
            "constant bool live = !is_function_constant_defined(dm_border_off) || !dm_border_off;\n"
            "constant uint kept = is_function_constant_defined(prior) ? prior : 13u;\n"
            "kernel void probe(device uint *out [[buffer(0)]]) { out[0] = kept + uint(live); out[1] = %du; }\n", (unsigned)DMSH_BORDER_FC, deferred];
        NSError *error = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&error];
        CHECK(lib != nil, "specialization fixture compiles: %s", error.localizedDescription.UTF8String);
        if (!lib) return;
        MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new]; uint32_t prior = deferred == 2 ? 13 : 23;
        [cv setConstantValue:&prior type:MTLDataTypeUInt atIndex:0];
        id<MTLFunction> f = deferred == 2 ? geo_mesh_entry(lib, @"probe") : deferred ? fn_spec(lib, @"probe", cv, &error) : fn_lib_spec(lib, @"probe", cv, &error);
        CHECK(f != nil, "original specialization exists (%d)", deferred); if (!f) continue;
        id<MTLFunction> off = fn_dev_b(f, 1);   // the variant is no function object of its own (fn_off): a descriptor and a device function beside f's
        CHECK(fn_off(f) && fn4_of_b(f, 1) != fn4_of(f) && fn4_of_b(f, 1) == fn4_of_b(f, 1) && fn4_of_b(f, 0) == fn4_of(f), "border specialization has a descriptor of its own, cached (%d)", deferred);
        CHECK(off && off != fn_dev(f) && off == fn_dev_b(f, 1) && fn_dev_b(f, 0) == fn_dev(f), "its device function is distinct and cached (%d)", deferred);
        MTL4SpecializedFunctionDescriptor *pre = (MTL4SpecializedFunctionDescriptor *)pre_fn(lib, @"probe");
        id<MTLFunction> warm = fn_lib_spec(lib, @"probe", pre.constantValues, &error);
        CHECK(warm != nil, "prewarm specialization exists (%d)", deferred);
        // Prewarm must choose border-off with other constants at their defaults. The original must survive copying its constants into off unchanged.
        for (int variant = 2; variant >= 0; variant--) {
            id<MTLFunction> fn = variant == 2 ? warm : variant == 1 ? off : f; if (!fn) continue;
            id<MTLComputePipelineState> p = [g_dev newComputePipelineStateWithFunction:variant == 1 ? fn : fn_dev(fn) error:&error];
            CHECK(p != nil, "specialized pipeline builds: %s", error.localizedDescription.UTF8String); if (!p) continue;
            id<MTLBuffer> out = [g_dev newBufferWithLength:8 options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = q_cb(); id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
            [e setComputePipelineState:p]; [e setBuffer:out offset:0 atIndex:0];
            [e dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
            CHECK(cb.status == MTLCommandBufferStatusCompleted, "specialization GPU work completed");
            CHECK(*(uint32_t *)out.contents == (variant == 2 ? 13u : prior + (variant == 0)), "prior constant and border match variant %d (%d)", variant, deferred);
        }
    }
}
// live: the pipeline keeps the corrections (cps_new_b off without them: Metal's nearest colour whatever the mask)
static void gpu_case(id<MTLComputePipelineState> p, int live, int sid, const float B[4], int mask_on) {
    samp_par sp = {0}; sp.magf = 1; sp.minf = 1; sp.ws = sp.wt = sp.wr = 5; sp.lod_max = 0; memcpy(sp.border, B, 16);
    mtl_reg_samp(sid, &sp);
    os_unfair_lock_lock(&g_rmu);
    id<MTLSamplerState> s = samp_by_id(sid); int twin = g_sview_w[sid] != NULL; uint64_t rid = g_srid[sid], rid_w = twin ? g_srid_w[sid] : rid; float k[4]; memcpy(k, twin ? g_sbk[sid] : (float[4]){0}, 16);
    os_unfair_lock_unlock(&g_rmu);
    CHECK(s != nil, "sampler %d created", sid);
    int bn, bw; float kk[4]; const uint8_t w[3] = { 5, 5, 5 }; int want = nvn_border(B, w, 0, &bn, &bw, kk);
    CHECK(twin == want, "sampler %d: twin %d, nvn_border says %d", sid, twin, want);
    id<MTLBuffer> block = [g_dev newBufferWithLength:DMSH_BORDER_K + 16 options:MTLResourceStorageModeShared];
    uint32_t mask = twin && mask_on; memcpy(block.contents, &mask, 4); memcpy((uint8_t *)block.contents + DMSH_BORDER_K, k, 16);
    id<MTLBuffer> args = [g_dev newBufferWithLength:32 options:MTLResourceStorageModeShared];   // the members as border_emu lays them out: texture, sampler, its twin, the block
    uint64_t a[4] = { g_tex.gpuResourceID._impl, rid, rid_w, block.gpuAddress }; memcpy(args.contents, a, 32);
    id<MTLBuffer> uv = [g_dev newBufferWithBytes:UV length:sizeof UV options:MTLResourceStorageModeShared];
    id<MTLBuffer> out = [g_dev newBufferWithLength:16 * NUV options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = q_cb(); id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:p]; [e setBuffer:args offset:0 atIndex:0]; [e setBuffer:out offset:0 atIndex:1]; [e setBuffer:uv offset:0 atIndex:2];
    [e useResource:g_tex usage:MTLResourceUsageRead]; [e useResource:block usage:MTLResourceUsageRead];
    [e dispatchThreads:MTLSizeMake(NUV, 1, 1) threadsPerThreadgroup:MTLSizeMake(NUV, 1, 1)]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK(cb.status == MTLCommandBufferStatusCompleted, "sampler %d: GPU work completed", sid);
    const float *got = out.contents; const float *Bref = mask && live ? B : nvn_mtl_border[bn];   // the mask clear, or the corrections compiled out: Metal's nearest colour
    for (int i = 0; i < NUV; i++) { float ref[4]; reference(Bref, UV[i][0], UV[i][1], ref);
        for (int c = 0; c < 4; c++) CHECK(fabsf(got[4 * i + c] - ref[c]) < 6e-3f, "border %g,%g,%g,%g mask %u live %d at (%g, %g) channel %d: %g, expected %g", B[0], B[1], B[2], B[3], mask, live, UV[i][0], UV[i][1], c, got[4 * i + c], ref[c]); }
}

int main(int argc, char **argv) { @autoreleasepool {
    choice((float[4]){0.5f, 0.5f, 0.5f, 0.5f}, 5, 0, 1, 0);   // Tears of the Kingdom's grass bend map
    choice((float[4]){0.25f, 0.5f, 0.75f, 1}, 5, 0, 1, 1);     // opaque: kept with opaque black, corrected with opaque white
    choice((float[4]){0.8f, 0.9f, 0.7f, 0.6f}, 4, 0, 1, 2);
    choice((float[4]){1, 0, 0, 1}, 5, 0, 1, -1);
    choice((float[4]){0.2f, 0, 0, 1}, 5, 0, 1, 1);             // opaque black and opaque white differ in red only where it matters
    choice((float[4]){0, 0, 0, 0}, 5, 0, 0, 0); choice((float[4]){0, 0, 0, 1}, 5, 0, 0, 1); choice((float[4]){1, 1, 1, 1}, 5, 0, 0, 2);   // Metal's own
    choice((float[4]){0.5f, 0.5f, 0.5f, 0.5f}, 0, 0, 0, 0);   // the border is never read
    choice((float[4]){0.5f, 0.5f, 0.5f, 0.5f}, 5, 1, 0, 0);   // a min reduction is not linear in it
    binding_cases();
    if (argc > 1 && !strcmp(argv[1], "--cpu")) {
        printf("t_border (CPU only): %d checks, %d failed\n", checks, fails);
        return fails != 0;
    }
    mtl_init();
    specialization_cases();
    if (argc > 1) {   // the rewritten shader
        NSError *error = nil; NSString *src = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:&error];
        id<MTLLibrary> lib = src ? [g_dev newLibraryWithSource:src options:nil error:&error] : nil;
        // as shader_load and dispatch take a compute shader: specialized with the empty set (the corrections), and without them (cps_new_b), each through cps_new
        NvShader *cs = [NvShader new]; cs->fn = lib ? fn_spec(lib, @"main0", [MTLFunctionConstantValues new], &error) : nil;
        id<MTLComputePipelineState> p = cs->fn ? cps_new_b(cs->fn, 0, &error) : nil, p_off = cs->fn ? cps_new_b(cs->fn, 1, &error) : nil;
        CHECK(p != nil && p_off != nil, "the rewritten shader compiles: %s", error.localizedDescription.UTF8String);
        CHECK(p && p_off && p != p_off && fn4_of_b(cs->fn, 1) != fn4_of(cs->fn) && fn_dev_b(cs->fn, 1) == fn_dev_b(cs->fn, 1), "the kernel's border-off pipeline is its own, its functions cached");
        if (p && p_off) {
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:W height:H mipmapped:NO];
            d.storageMode = MTLStorageModeShared; g_tex = [g_dev newTextureWithDescriptor:d];
            float px[H][W][4]; for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) for (int c = 0; c < 4; c++) px[y][x][c] = texel(x, y, c);
            [g_tex replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:px bytesPerRow:16 * W];
            if (![g_dev supports32BitFloatFiltering]) { fprintf(stderr, "t_border: this GPU does not filter RGBA32Float, GPU cases skipped\n"); }
            else {
                gpu_case(p, 1, 11, (float[4]){0.5f, 0.5f, 0.5f, 0.5f}, 1);
                gpu_case(p, 1, 11, (float[4]){0.5f, 0.5f, 0.5f, 0.5f}, 0);
                gpu_case(p, 1, 12, (float[4]){0.25f, 0.5f, 0.75f, 1}, 1);
                gpu_case(p, 1, 12, (float[4]){0.25f, 0.5f, 0.75f, 1}, 0);
                gpu_case(p, 1, 13, (float[4]){0.8f, 0.9f, 0.7f, 0.6f}, 1);
                gpu_case(p, 1, 14, (float[4]){0, 0, 0, 1}, 1);   // Metal's own: no twin, the plain sample
                gpu_case(p_off, 0, 11, (float[4]){0.5f, 0.5f, 0.5f, 0.5f}, 1);   // the corrections compiled out: the mask set, still the nearest colour
                gpu_case(p_off, 0, 12, (float[4]){0.25f, 0.5f, 0.75f, 1}, 1);
            }
        }
    }
    printf("t_border: %d checks, %d failed\n", checks, fails);
    return fails != 0;
} }
