// Draw passes in the replay's command buffer around a MetalFX scaler, through the host's own draw-pass encoder (enc_r_begin_pass / enc_r_end_pass, as a game's draws are
// encoded): a slow pass that writes the scaler's input just before it, and a pass that copies the scaler's output just after it (t_dlss_mfx, t_upscaler_mfx; mfx_order in
// nvn_mtl.m). Include after nvn_mtl.m.
static id<MTLRenderPipelineState> pass_pso(NSString *fs, MTLPixelFormat pf) {
    NSError *err = nil;
    NSString *src = [@"#include <metal_stdlib>\nusing namespace metal;\nstruct A { float4 c; uint iters; };\n"
                     "vertex float4 vs(uint v [[vertex_id]]) { return float4(v == 1 ? 3.0f : -1.0f, v == 2 ? 3.0f : -1.0f, 0, 1); }\n" stringByAppendingString:fs];
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&err];
    id<MTL4Compiler> comp = [g_dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&err];
    MTL4LibraryFunctionDescriptor *vf = [MTL4LibraryFunctionDescriptor new], *ff = [MTL4LibraryFunctionDescriptor new]; vf.name = @"vs"; ff.name = @"fs"; vf.library = ff.library = lib;
    MTL4RenderPipelineDescriptor *pd = [MTL4RenderPipelineDescriptor new]; pd.vertexFunctionDescriptor = vf; pd.fragmentFunctionDescriptor = ff; pd.colorAttachments[0].pixelFormat = pf;
    id<MTLRenderPipelineState> ps = lib && comp ? [comp newRenderPipelineStateWithDescriptor:pd compilerTaskOptions:nil error:&err] : nil;
    if (!ps) fprintf(stderr, "pass pipeline: %s\n", err.description.UTF8String);
    return ps;
}
static id<MTL4ArgumentTable> pass_table(id<MTLBuffer> b, id<MTLTexture> t) {
    MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 1; ad.maxTextureBindCount = 1;
    NSError *err = nil; id<MTL4ArgumentTable> at = [g_dev newArgumentTableWithDescriptor:ad error:&err];
    static NSMutableArray *bufs; if (!bufs) bufs = [NSMutableArray new];   // until the test ends (the table holds only the address)
    if (b) { [bufs addObject:b]; [at setAddress:b.gpuAddress atIndex:0]; res_add(b); res_commit(); }
    if (t) [at setTexture:t.gpuResourceID atIndex:0];
    return at;
}
// One full-screen triangle into dst with ps and the fragment argument table at. bare: the pass ends with no barrier, as a MetalFX scaler's own encoders end; else enc_r_end_pass
// ends it (the relaxed end with DM_NVN_BARRIER=game, the full one otherwise)
static int pass_draw(id<MTLRenderPipelineState> ps, id<MTL4ArgumentTable> at, id<MTLTexture> dst, int bare) {
    static NSMutableArray *keep;   // until the test ends: the encoding thread may encode the draw after this returns
    if (!ps || !at || !(dst.usage & MTLTextureUsageRenderTarget)) return 0;
    if (!keep) keep = [NSMutableArray new];
    [keep addObject:at];
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
    rp.colorAttachments[0].texture = dst; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTL4RenderCommandEncoder> e = enc_r_begin_pass(rp);
    [e setRenderPipelineState:ps]; [e setArgumentTable:at atStages:MTLRenderStageFragment];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    if (bare) { gp_end(e); [e endEncoding]; } else enc_r_end_pass(e);
    args_dirty_all();
    return 1;
}
// A slow pass (iters steps a fragment) that fills t with colour c
static int slow_draw(id<MTLTexture> t, const float c[4], uint32_t iters, int bare) {
    static id<MTLRenderPipelineState> ps; static MTLPixelFormat pf;
    if (!ps || pf != t.pixelFormat) { pf = t.pixelFormat;
        ps = pass_pso(@"fragment float4 fs(constant A &a [[buffer(0)]], float4 p [[position]]) { float x = p.x; for (uint k = 0; k < a.iters; k++) x = fma(x, 1.0000001f, 0.5f);"
                       " return x > -1.0f ? a.c : float4(0); }\n", pf); }
    struct { float c[4]; uint32_t iters, pad[3]; } a = {{c[0], c[1], c[2], c[3]}, iters, {0}};
    return pass_draw(ps, pass_table([g_dev newBufferWithBytes:&a length:sizeof a options:MTLResourceStorageModeShared], nil), t, bare);
}
// A pass that copies src into dst texel for texel (the same size)
static int copy_draw(id<MTLTexture> src, id<MTLTexture> dst) {
    static id<MTLRenderPipelineState> ps; static MTLPixelFormat pf;
    if (!ps || pf != dst.pixelFormat) { pf = dst.pixelFormat; ps = pass_pso(@"fragment float4 fs(texture2d<float> t [[texture(0)]], float4 p [[position]]) { return t.read(uint2(p.xy)); }\n", pf); }
    return pass_draw(ps, pass_table(nil, src), dst, 0);
}
