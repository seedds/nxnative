// The typed-output entry point (dmsh_entry.py fs_uint) through the host's pipeline path (pso_desc): every mix of float, unsigned and signed targets for colours 0 to 2, the pipeline
// built as the draws build it (pso_build: Metal 4 bases, or the MTLDevice with DM_PSO_MTL4=0), drawn, and every target's bits read back: each output's register bits land as they
// are, whatever the target's class (Kirby and the Forgotten Land's R8Uint colour 5, GitHub issue #2760; Mortal Kombat 11's signed targets, PR #2807). Then the alpha test of a float
// colour 0 beside integer targets (main0_u with its alpha test on), passing and failing, and with float targets only (main0's alpha test entry point main0_a, no specialization).
// Colour 3 carries bits no float target keeps (NaN payloads, denormals: what a BC block encoder writes) into an unsigned or a signed target, or is left unattached. The libraries come from test_dmsh_typed.py --fixture <dir>
// make GAME=<id> check_t_typed_outputs
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static const uint32_t want[4][4] = {{0x3e800000, 0xbf000000, 0x3f800000, 0x3e800000}, {0x3e800000, 0xbf000000, 0x3f800000, 0x40400000}, {0xc0400000, 0, 0, 0}, {0x7fc00001, 0xffffffff, 0x00000001, 0x807fffff}};
static MTLPixelFormat fmt(int target, int kind) {   // kind 0 float, 1 unsigned, 2 signed; 32 bits a channel, so every bit pattern is kept
    static const MTLPixelFormat wide[3] = {MTLPixelFormatRGBA32Float, MTLPixelFormatRGBA32Uint, MTLPixelFormatRGBA32Sint}, one[3] = {MTLPixelFormatR32Float, MTLPixelFormatR32Uint, MTLPixelFormatR32Sint};
    return target == 2 ? one[kind] : wide[kind];
}
// One draw of a pipeline with targets of these classes (kind[3] < 0: colour 3 unattached; alpha: 0 none, else the NVN alpha function compared with 0.5); returns 1 when the fragment was kept, 0 when dropped
static int typed_draw(NvShader *vs, NvShader *fs, const int kind[4], int alpha, id<MTLCommandQueue> q) {
    pso_key k; memset(&k, 0, sizeof k); k.samples = 1; k.chmask = 0xffff; k.atest = alpha != 0;
    for (int i = 0; i < 4; i++) if (kind[i] >= 0) k.cfmt[i] = (uint32_t)fmt(i, kind[i]);
    MTLRenderPipelineDescriptor *d = pso_desc(&k, vs, fs, nil, 0);
    uint32_t cls; int need = pso_cls(&k, fs, &cls);
    CHECK((d.fragmentFunction != fs->fn) == (need || alpha));   // main0 itself when every target takes the class it declares and there is no test
    NSError *e = nil; id<MTLRenderPipelineState> ps = pso_build(d, !k.border, &e);
    if (!ps) { fprintf(stderr, "classes %d %d %d %d alpha %d: %s\n", kind[0], kind[1], kind[2], kind[3], alpha, e.localizedDescription.UTF8String); CHECK(ps); return -1; }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor]; id<MTLTexture> tex[4] = {nil};
    for (int i = 0; i < 4; i++) { if (kind[i] < 0) continue;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt(i, kind[i]) width:2 height:2 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared; tex[i] = [g_dev newTextureWithDescriptor:td];
        rp.colorAttachments[i].texture = tex[i]; rp.colorAttachments[i].loadAction = MTLLoadActionClear; rp.colorAttachments[i].storeAction = MTLStoreActionStore; rp.colorAttachments[i].clearColor = MTLClearColorMake(0, 0, 0, 0);
    }
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    [en setRenderPipelineState:ps];
    struct { uint32_t func; float ref; } st = {(uint32_t)alpha, 0.5f}; if (alpha) [en setFragmentBytes:&st length:sizeof st atIndex:DMSH_ALPHA_BUFFER];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CHECK(cb.status == MTLCommandBufferStatusCompleted);
    int kept = -1;
    for (int i = 0; i < 4; i++) { if (!tex[i]) continue;
        uint32_t px[16] = {0}; int nch = i == 2 ? 1 : 4;
        [tex[i] getBytes:px bytesPerRow:8 * nch fromRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0];
        int here = px[0] != 0;
        for (int j = 0; j < 4 * nch; j++) if (px[j] != (here ? want[i][j % nch] : 0)) {
            fprintf(stderr, "classes %d %d %d %d alpha %d: target %d word %d %#x, want %#x\n", kind[0], kind[1], kind[2], kind[3], alpha, i, j, px[j], here ? want[i][j % nch] : 0); CHECK(0); break; }
        CHECK(kept < 0 || kept == here); kept = here;
    }
    return kept;
}
int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) return 2;
    mtl_init(); g_msl_dir = @(argv[1]); NSError *e = nil;
    NSString *key = @"2222222222222222";
    id<MTLLibrary> lib = lib_from_file([NSString stringWithFormat:@"%@/%@_b.metallib", g_msl_dir, key], &e);
    if (!lib) { fprintf(stderr, "%s\n", e.localizedDescription.UTF8String); return 1; }
    NvShader *vs = [NvShader new]; vs->fn = fn4_note([lib newFunctionWithName:@"vertex_main"], lib, @"vertex_main", nil); vs->stage = 0; snprintf(vs->key, sizeof vs->key, "vt_b");
    NvShader *fs = [NvShader new]; fs->fn = fn4_note([lib newFunctionWithName:@"main0"], lib, @"main0", nil); fs->stage = 1; snprintf(fs->key, sizeof fs->key, "%s_b", key.UTF8String);
    fs->u_name = @"main0_u"; fs->u_side = 1; fs->u_alpha = 1; fs->u_logic = 1; fs->has_alpha = 1;   // as the reflection of a shader the build cut says (entry_u, entry_u_side, entry_u_alpha, entry_u_logic, alpha)
    fs->rf.color_known = 1; fs->rf.color_mask = 15; fs->rf.fcols_known = 1; fs->rf.fcols = 15;   // main0 declares every colour float, as the translator does
    CHECK(!fs->xlib_state);   // the side library is not loaded before a pipeline needs it
    id<MTLCommandQueue> q = [g_dev newCommandQueue]; int draws = 0;
    for (int m = 0; m < 27; m++) {   // colour 3 unsigned or signed beside an unsigned or signed colour 1, absent beside a float one
        int kind[4] = {m % 3, m / 3 % 3, m / 9, m / 3 % 3 ? m / 3 % 3 : -1}; CHECK(typed_draw(vs, fs, kind, 0, q) == 1); draws++;
    }
    CHECK(fs->ufn.count == 26u);   // every mix but all float, one specialization each
    for (int m = 0; m < 9; m++) {   // the alpha test of colour 0 (a float target, alpha 0.25) beside integer targets: LESS than 0.5 keeps the fragment, GREATER drops it
        int kind[4] = {0, m % 3, m / 3, -1};
        CHECK(typed_draw(vs, fs, kind, NVN_ALPHA_FUNC_LESS, q) == 1); CHECK(typed_draw(vs, fs, kind, NVN_ALPHA_FUNC_GREATER, q) == 0); draws += 2;
    }
    CHECK(fs->ufn.count == 34u && fs->afn[0]);   // main0_u with its test per mix with an integer target; main0_a, with no constant, for all float
    printf("typed outputs: %d draws, %d checks, %d failed\n", draws, checks, failed);
    return failed != 0;
} }
