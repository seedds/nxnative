// Pipelines of a fragment shader whose float outputs carry integer register bits (mask_m, the outputs its mixed entry point returns as integers) into non-integer colour
// targets are built whole, not specialized from a base (pso_intbits_whole, Orbitals' player ship e97d65cf4844ecdb: its GBufferB blue channel is as_type<float>(int), and the
// specialized pipeline left GBufferB-D zero). Checks the rule's decision for plain and merged targets, integer targets (no rule: those already build whole) and an empty mask,
// and that a pipeline the rule names compiles whole on the Metal 4 path and writes the bits' unorm value with its other outputs intact
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
int main(void) { @autoreleasepool {
    mtl_init();
    pso_key k; memset(&k, 0, sizeof k);
    k.cfmt[0] = MTLPixelFormatRGBA16Float; k.cfmt[1] = MTLPixelFormatRGB10A2Unorm; k.cfmt[2] = MTLPixelFormatBGRA8Unorm; k.cfmt[7] = MTLPixelFormatBGRA8Unorm;
    CHECK(pso_intbits_whole(&k, 0x84, 0), "integer bits in outputs 2 and 7 into BGRA8Unorm targets: whole");
    CHECK(!pso_intbits_whole(&k, 0, 0), "no integer-bit outputs: specialized from a base");
    CHECK(!pso_intbits_whole(&k, 0x08, 0), "integer bits in an output with no target: specialized");
    pso_key u = k; u.cfmt[2] = MTLPixelFormatR32Uint; u.cfmt[7] = MTLPixelFormatRGBA8Uint;
    CHECK(!pso_intbits_whole(&u, 0x84, 0), "integer-bit outputs into integer targets: not this rule (integer targets build whole already)");
    pso_key m; memset(&m, 0, sizeof m); m.cfmt[0] = MTLPixelFormatBGRA8Unorm; m.cfmt[1] = MTLPixelFormatRGBA16Float; for (int i = 0; i < 8; i++) m.lmap[i] = (uint8_t)(i == 0 ? 2 : i == 1 ? 0 : 7);
    CHECK(pso_intbits_whole(&m, 0x04, 1), "merged pass: output 2 lands in attachment 0 (BGRA8Unorm): whole");
    CHECK(!pso_intbits_whole(&m, 0x02, 1), "merged pass: output 1 has no attachment: specialized");
    // A pipeline the rule names, through the host's own build (pso_build_ex): whole on the Metal 4 path, and the bits' unorm value (0) beside intact outputs
    NSError *error = nil;
    static const char *src = "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct VO { float4 p [[position, invariant]]; };\n"
        "vertex VO v(uint n [[vertex_id]]) { VO o; float2 q = float2((n << 1) & 2, n & 2); o.p = float4(q * 2 - 1, 0, 1); return o; }\n"
        "struct O { float4 c0 [[color(0)]]; float4 c2 [[color(2)]]; };\n"
        "fragment O f(float4 p [[position]]) { int bits = int(p.x) & 1; O o; o.c0 = float4(0.25, 0.5, 0.75, 1); o.c2 = float4(0.2, 0.4, as_type<float>(bits), 0.5529); return o; }\n";
    char dir[] = "/tmp/t_pso_intbits.XXXXXX"; if (!mkdtemp(dir)) return 1;
    NSString *ms = [NSString stringWithFormat:@"%s/p.metal", dir], *air = [NSString stringWithFormat:@"%s/p.air", dir], *ml = [NSString stringWithFormat:@"%s/p.metallib", dir];
    [@(src) writeToFile:ms atomically:NO encoding:NSUTF8StringEncoding error:nil];
    for (NSArray *args in @[@[@"-sdk", @"macosx", @"metal", @"-c", ms, @"-o", air], @[@"-sdk", @"macosx", @"metallib", air, @"-o", ml]]) {
        NSTask *t = [NSTask new]; t.executableURL = [NSURL fileURLWithPath:@"/usr/bin/xcrun"]; t.arguments = args;
        if (![t launchAndReturnError:&error]) { fprintf(stderr, "metal compile: %s\n", error.localizedDescription.UTF8String); return 1; }
        [t waitUntilExit]; if (t.terminationStatus) { fprintf(stderr, "metal compile failed\n"); return 1; }
    }
    id<MTLLibrary> lib = lib_from_file(ml, &error); if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    NvShader *vs = [NvShader new]; vs->fn = fn_spec(lib, @"v", nil, &error);
    NvShader *fs = [NvShader new]; fs->stage = 1; fs->fn = fn_spec(lib, @"f", nil, &error); fs->mask_m = 0x04; strlcpy(fs->key, "t_pso_intbits", sizeof fs->key);
    pso_key pk; memset(&pk, 0, sizeof pk); pk.cfmt[0] = MTLPixelFormatRGBA8Unorm; pk.cfmt[2] = MTLPixelFormatRGBA8Unorm; pk.samples = 1;
    pk.chmask = 0xffffffffu;   // every channel written
    MTLRenderPipelineDescriptor *d = pso_desc(&pk, vs, fs, nil, 0);
    CHECK(d && [d.label isEqualToString:PSO_WHOLE], "pso_desc labels the pipeline to be built whole");
    id<MTLRenderPipelineState> ps = d ? pso_build(d, !pk.border, &error) : nil;
    CHECK(ps != nil, "the pipeline builds");
    if (ps) {
        id<MTLTexture> t[2]; MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        for (int i = 0; i < 2; i++) { MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared; t[i] = [g_dev newTextureWithDescriptor:td];
            int a = i ? 2 : 0; rp.colorAttachments[a].texture = t[i]; rp.colorAttachments[a].loadAction = MTLLoadActionClear; rp.colorAttachments[a].storeAction = MTLStoreActionStore; }
        id<MTLCommandBuffer> cb = [[g_dev newCommandQueue] commandBuffer]; id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
        [en setRenderPipelineState:ps]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
        uint8_t p0[4], p2[4]; [t[0] getBytes:p0 bytesPerRow:16 fromRegion:MTLRegionMake2D(1, 1, 1, 1) mipmapLevel:0]; [t[1] getBytes:p2 bytesPerRow:16 fromRegion:MTLRegionMake2D(1, 1, 1, 1) mipmapLevel:0];
        CHECK(p0[0] == 64 && p0[1] == 128 && p0[2] == 191, "colour 0 intact");
        CHECK(p2[0] == 51 && p2[1] == 102 && p2[2] == 0 && p2[3] == 141, "colour 2: the integer bits' unorm value is 0, the other channels intact");
    }
    printf("t_pso_intbits: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
