// Metal 4 pipelines (pso_build): one base per pipeline with its colour state unspecialized, made into pipelines by specialization, kept in the pipeline archive.
// Checks: pipelines that differ only in colour state share one base; a specialized pipeline draws the same pixels as one built through the MTLDevice; a flushed archive
// file serves the base again without compiling; a function with no Metal 4 descriptor keeps the MTLDevice path; pso_key_norm drops what no pipeline depends on; compaction
// merges the files the prewarm took bases from into one
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define EXPECT(name, want, got) do { uint64_t w = (want), g = (got); checks++; if (w != g) { fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)w, (unsigned long long)g); failed++; } } while (0)

static id<MTLLibrary> g_lib, g_lib2;   // g_lib2: the same source again, for functions with no Metal 4 descriptor (a library hands out one function object per name)
static id<MTLCommandQueue> g_q;
// a full-screen triangle; colour 0 = (0.5, 0.25, 1, 0.5), colour 1 = (0.2, 0.4, 0.6, 0.8); attribute 0 (a float4) scales nothing, so a vertex descriptor is part of the pipeline
static MTLRenderPipelineDescriptor *desc(MTLPixelFormat f0, int blend, MTLPixelFormat f1, MTLColorWriteMask m1, int registered) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    id<MTLFunction> v = [g_lib2 newFunctionWithName:@"vs"], f = [g_lib2 newFunctionWithName:@"fs"];
    static id<MTLFunction> rv, rf;   // the registered ones are made once, as shader_load makes a shader's functions once
    if (registered) { if (!rv) { rv = fn4_note([g_lib newFunctionWithName:@"vs"], g_lib, @"vs", nil); rf = fn4_note([g_lib newFunctionWithName:@"fs"], g_lib, @"fs", nil); } v = rv; f = rf; }
    d.vertexFunction = v; d.fragmentFunction = f;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat4; vd.attributes[0].bufferIndex = 16; vd.layouts[16].stride = 16; vd.layouts[16].stepFunction = MTLVertexStepFunctionConstant; vd.layouts[16].stepRate = 0;
    d.vertexDescriptor = vd;
    d.colorAttachments[0].pixelFormat = f0;
    if (blend) { d.colorAttachments[0].blendingEnabled = YES; d.colorAttachments[0].sourceRGBBlendFactor = d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
                 d.colorAttachments[0].destinationRGBBlendFactor = d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha; }
    d.colorAttachments[1].pixelFormat = f1; d.colorAttachments[1].writeMask = m1;
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;   // a Metal 4 pipeline names no depth format; drawn into a pass with this depth attachment
    return d;
}
// draws with ps into fresh targets cleared to (1, 0, 0, 1) and returns texel (1, 1) of both as floats
static void draw_with(id<MTLRenderPipelineState> ps, MTLPixelFormat f0, MTLPixelFormat f1, float out[2][4]) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new]; id<MTLTexture> t[2];
    MTLPixelFormat fm[2] = { f0, f1 };
    for (int i = 0; i < 2; i++) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fm[i] width:4 height:4 mipmapped:NO]; td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
        t[i] = [g_dev newTextureWithDescriptor:td];
        rp.colorAttachments[i].texture = t[i]; rp.colorAttachments[i].loadAction = MTLLoadActionClear; rp.colorAttachments[i].clearColor = MTLClearColorMake(1, 0, 0, 1); rp.colorAttachments[i].storeAction = MTLStoreActionStore;
    }
    MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:4 height:4 mipmapped:NO]; dd.usage = MTLTextureUsageRenderTarget; dd.storageMode = MTLStorageModePrivate;
    rp.depthAttachment.texture = [g_dev newTextureWithDescriptor:dd]; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.storeAction = MTLStoreActionDontCare;
    static id<MTLBuffer> one; if (!one) { float v[4] = { 1, 1, 1, 1 }; one = [g_dev newBufferWithBytes:v length:16 options:MTLResourceStorageModeShared]; }
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:ps]; [e setVertexBuffer:one offset:0 atIndex:16]; [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [e endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    for (int i = 0; i < 2; i++) {
        if (fm[i] == MTLPixelFormatRGBA8Unorm) { uint8_t p[4]; [t[i] getBytes:p bytesPerRow:16 fromRegion:MTLRegionMake2D(1, 1, 1, 1) mipmapLevel:0]; for (int c = 0; c < 4; c++) out[i][c] = p[c] / 255.0f; }
        else { __fp16 p[4]; [t[i] getBytes:p bytesPerRow:32 fromRegion:MTLRegionMake2D(1, 1, 1, 1) mipmapLevel:0]; for (int c = 0; c < 4; c++) out[i][c] = p[c]; }
    }
}
static void same_pixels(const char *name, MTLPixelFormat f0, int blend, MTLPixelFormat f1, MTLColorWriteMask m1) {
    NSError *e = nil;
    id<MTLRenderPipelineState> a = pso_build(desc(f0, blend, f1, m1, 1), &e), b = [g_dev newRenderPipelineStateWithDescriptor:desc(f0, blend, f1, m1, 0) error:&e];
    checks++; if (!a || !b) { fprintf(stderr, "%s: no pipeline (%s)\n", name, e.localizedDescription.UTF8String); failed++; return; }
    float pa[2][4], pb[2][4]; draw_with(a, f0, f1, pa); draw_with(b, f0, f1, pb);
    checks++; if (memcmp(pa, pb, sizeof pa)) { fprintf(stderr, "%s: the specialized pipeline drew (%g %g %g %g | %g %g %g %g), the MTLDevice one (%g %g %g %g | %g %g %g %g)\n", name,
        pa[0][0], pa[0][1], pa[0][2], pa[0][3], pa[1][0], pa[1][1], pa[1][2], pa[1][3], pb[0][0], pb[0][1], pb[0][2], pb[0][3], pb[1][0], pb[1][1], pb[1][2], pb[1][3]); failed++; }
}
int main(void) { @autoreleasepool {
    char dir[] = "/tmp/t_pso_archive.XXXXXX"; if (!mkdtemp(dir)) abort();
    setenv("DM_PSO_ARCHIVE", dir, 1);
    mtl_init(); g_q = [g_dev newCommandQueue]; NSError *err = nil;
    NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; };\nstruct I { float4 a [[attribute(0)]]; };\n"
        "vertex V vs(I in [[stage_in]], uint id [[vertex_id]]) { V o; float2 uv = float2((id << 1) & 2, id & 2); o.p = float4(uv * 2 - 1, 0, in.a.w); return o; }\n"
        "struct O { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; };\nfragment O fs() { return { float4(0.5, 0.25, 1.0, 0.5), float4(0.2, 0.4, 0.6, 0.8) }; }\n";
    g_lib = [g_dev newLibraryWithSource:src options:nil error:&err];
    if (!g_lib) { fprintf(stderr, "%s\n", err.localizedDescription.UTF8String); return 1; }
    g_lib2 = [g_dev newLibraryWithSource:[src stringByAppendingString:@"// another library: the same source would come back as the same library\n"] options:nil error:&err];
    // pipelines differing only in colour state share one base
    same_pixels("RGBA8 blended, RGBA16F all channels", MTLPixelFormatRGBA8Unorm, 1, MTLPixelFormatRGBA16Float, MTLColorWriteMaskAll);
    EXPECT("first pipeline: its base compiled", 1, g_pb_built);
    same_pixels("RGBA16F, RGBA8 red and green", MTLPixelFormatRGBA16Float, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskRed | MTLColorWriteMaskGreen);
    same_pixels("RGBA8, RGBA8 no channel", MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskNone);
    EXPECT("other colour states: the same base", 1, g_pb_built);
    EXPECT("other colour states: base reused", 2, g_pb_reused);
    EXPECT("one specialization per pipeline", 3, g_pb_spec);
    // the archive: flushed to a file, a later start takes the base from it without compiling
    EXPECT("one base waits for the archive", 1, g_arch_new);
    arch_flush();
    NSString *file = [@(dir) stringByAppendingPathComponent:@"00000.mtl4a"];
    EXPECT("archive file written", 1, [NSFileManager.defaultManager fileExistsAtPath:file]);
    EXPECT("nothing left to write", 0, g_arch_new);
    nvn_hm_clear(&g_pbase, hm_rel);   // as a new start: no base in memory, the archive opened
    g_arch4 = @[[g_dev newArchiveWithURL:[NSURL fileURLWithPath:file] error:&err]];
    uint64_t built = g_pb_built;
    same_pixels("from the archive: RGBA8 blended, RGBA16F", MTLPixelFormatRGBA8Unorm, 1, MTLPixelFormatRGBA16Float, MTLColorWriteMaskAll);
    EXPECT("base from the archive", 1, g_pb_archived);
    EXPECT("no base compiled", built, g_pb_built);
    // compaction: with more than PSO_ARCH_COMPACT files, the bases the prewarm takes from them are captured again; after the flush that follows, one file holds them and the old ones are gone
    {
        char cdir[] = "/tmp/t_pso_compact.XXXXXX"; if (!mkdtemp(cdir)) abort();
        NSString *saved = g_arch_dir; g_arch_dir = @(cdir); nvn_hm_clear(&g_pbase, hm_rel); g_arch4 = @[];
        NSMutableArray<MTLRenderPipelineDescriptor *> *descs = [NSMutableArray array];
        for (int i = 0; i <= PSO_ARCH_COMPACT; i++) {   // one base per file: the stride of the layout tells them apart
            MTLRenderPipelineDescriptor *cd = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
            cd.vertexDescriptor.layouts[16].stride = 16 * (i + 2); [descs addObject:cd];
            if (!pso_build(cd, &err)) { fprintf(stderr, "compaction: no pipeline %d\n", i); failed++; }
            arch_flush();
        }
        NSArray<NSString *> *names = [[NSFileManager.defaultManager contentsOfDirectoryAtPath:@(cdir) error:nil] sortedArrayUsingSelector:@selector(compare:)];
        EXPECT("compaction: one file per flush", PSO_ARCH_COMPACT + 1, names.count);
        NSMutableArray<id<MTL4Archive>> *open = [NSMutableArray array]; NSMutableArray<NSString *> *paths = [NSMutableArray array];
        for (NSString *n in names.reverseObjectEnumerator) { NSString *f = [@(cdir) stringByAppendingPathComponent:n]; [paths addObject:f]; [open addObject:[g_dev newArchiveWithURL:[NSURL fileURLWithPath:f] error:&err]]; }
        nvn_hm_clear(&g_pbase, hm_rel); g_arch4 = open;   // as a new start with that many files
        g_compact_opts = [MTL4CompilerTaskOptions new]; g_compact_opts.lookupArchives = open; g_compact_files = paths; g_compacted = 0;
        uint64_t arch0 = g_pb_archived, built0 = g_pb_built;
        atomic_store(&g_warm_running, 1);
        for (MTLRenderPipelineDescriptor *cd in descs) pso_build(cd, &err);
        atomic_store(&g_warm_running, 0);
        EXPECT("compaction: every base from the archive", arch0 + descs.count, g_pb_archived);
        EXPECT("compaction: none compiled", built0, g_pb_built);
        EXPECT("compaction: every base captured again", descs.count, g_compacted);
        arch_flush(); arch_compact_finish();
        names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:@(cdir) error:nil];
        EXPECT("compaction: one file left", 1, names.count);
        nvn_hm_clear(&g_pbase, hm_rel); g_compact_opts = nil; g_compacted = 0;
        g_arch4 = @[[g_dev newArchiveWithURL:[NSURL fileURLWithPath:[@(cdir) stringByAppendingPathComponent:names.firstObject]] error:&err]];
        arch0 = g_pb_archived; built0 = g_pb_built;
        for (MTLRenderPipelineDescriptor *cd in descs) pso_build(cd, &err);
        EXPECT("compaction: the new file holds every base", arch0 + descs.count, g_pb_archived);
        EXPECT("compaction: none compiled from the new file", built0, g_pb_built);
        [NSFileManager.defaultManager removeItemAtPath:@(cdir) error:nil]; g_arch_dir = saved; g_arch4 = @[];
    }
    // a function with no Metal 4 descriptor: the MTLDevice path
    uint64_t legacy = g_pb_legacy;
    id<MTLRenderPipelineState> l = pso_build(desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 0), &err);
    EXPECT("unregistered function: a pipeline", 1, l != nil);
    EXPECT("unregistered function: MTLDevice build", legacy + 1, g_pb_legacy);
    // key normalization
    pso_key k; memset(&k, 0, sizeof k); k.blend_en = 0x5; k.dfmt = MTLPixelFormatDepth32Float; k.sfmt = MTLPixelFormatStencil8;
    for (int i = 0; i < 8; i++) { k.blend[i].src_rgb = 5; k.blend[i].dst_rgb = 6; }
    pso_key_norm(&k);
    EXPECT("blend of an enabled target kept", 5, k.blend[0].src_rgb);
    EXPECT("blend of an enabled target kept (2)", 6, k.blend[2].dst_rgb);
    EXPECT("blend of a disabled target dropped", 0, k.blend[1].src_rgb);
    EXPECT("blend of a disabled target dropped (7)", 0, k.blend[7].dst_rgb);
    EXPECT("depth format kept (an MTLDevice pipeline names it)", MTLPixelFormatDepth32Float, k.dfmt);
    EXPECT("stencil format kept", MTLPixelFormatStencil8, k.sfmt);
    [NSFileManager.defaultManager removeItemAtPath:@(dir) error:nil];
    printf("t_pso_archive: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
