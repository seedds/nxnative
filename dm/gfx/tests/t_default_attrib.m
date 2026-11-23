// An absent attribute keeps its implicit W=1.0 component (as float bits) in native fetch,
// while unknown non-zero formats and inactive shadow declarations read zeros.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

int main(void) { @autoreleasepool {
    mtl_init();
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct F { float4 a [[attribute(12)]]; };\n"
        "struct U { uint4 a [[attribute(12)]]; };\n"
        "struct I { int4 a [[attribute(12)]]; };\n"
        "struct Sh { uint4 a [[attribute(0)]]; uint4 b [[attribute(12)]];\n"
        "    float4 a_vf [[attribute(16)]]; float4 b_vf [[attribute(17)]];\n"
        "    int4 a_vo [[attribute(18)]]; int4 b_vo [[attribute(19)]]; };\n"
        "vertex void vf(F in [[stage_in]], device uint4 *o [[buffer(1)]]) { *o = as_type<uint4>(in.a); }\n"
        "vertex void vu(U in [[stage_in]], device uint4 *o [[buffer(1)]]) { *o = in.a; }\n"
        "vertex void vi(I in [[stage_in]], device uint4 *o [[buffer(1)]]) { *o = as_type<uint4>(in.a); }\n"
        "vertex void vsh(Sh in [[stage_in]], device uint4 *o [[buffer(1)]]) {\n"
        "    o[0] = in.a | as_type<uint4>(in.a_vf) | as_type<uint4>(in.a_vo);\n"
        "    o[1] = in.b | as_type<uint4>(in.b_vf) | as_type<uint4>(in.b_vo);\n"
        "}\n";
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:source options:nil error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
    id<MTLCommandQueue> queue = [g_dev newCommandQueue];
    id<MTLBuffer> out = [g_dev newBufferWithLength:32 options:MTLResourceStorageModeShared];
    int failed = 0, checks = 0;
    NSString *names[] = {@"vf", @"vu", @"vi"};
    for (int type = 0; type < 3; type++) for (int mode = 0; mode < 3; mode++) {
        // mode 0: beyond S.nattr, mode 1: explicitly absent (fmt 0), mode 2: unknown non-zero format (reads zeros)
        NvShader *vs = [NvShader new];
        vs->fn = [library newFunctionWithName:names[type]];
        vs->rf.nv = 1;
        vs->rf.v[0].loc = vs->rf.v[0].nvn = 12;
        vs->rf.v[0].ct = type;
        memset(vs->rf.v[0].sh, 0xff, sizeof vs->rf.v[0].sh);
        state_reset(); S.nattr = mode == 0 ? 10 : 13;
        if (mode == 2) S.attr[12].fmt = 0xff;
        pso_key key; uint32_t formats[8] = {0};
        pso_key_fill(&key, vs, nil, formats, 0, 0, 1, 0);
        MTLRenderPipelineDescriptor *descriptor = pso_desc(&key, vs, nil, nil, 0);
        descriptor.rasterizationEnabled = NO;
        id<MTLRenderPipelineState> pipeline = [g_dev newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.renderTargetWidth = pass.renderTargetHeight = pass.defaultRasterSampleCount = 1;
        memset(out.contents, 0xEE, 16);
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline];
        [encoder setVertexBuffer:out offset:0 atIndex:1];
        [encoder setVertexBuffer:vertex_constants() offset:0 atIndex:30];
        [encoder drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:1];
        [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
        if (command.error) { fprintf(stderr, "%s\n", command.error.localizedDescription.UTF8String); return 2; }
        uint32_t expected[] = {0, 0, 0, mode == 2 ? 0u : 0x3f800000u};
        checks++;
        if (memcmp(out.contents, expected, sizeof expected)) {
            failed++; fprintf(stderr, "FAIL type %d, mode %d: W=%#x expected %#x\n", type, mode, ((uint32_t *)out.contents)[3], expected[3]);
        }
    }
    // Shadow-input pipeline (on == 1): attribute 0 takes Half4 with W=0 through its float shadow (inactive shadows must stay zero, not OR 1.0 into W),
    // while absent attribute 12 on the same shader keeps its default W=1.0.
    {
        _Float16 h4[4] = {1.0f, 2.0f, 3.0f, 0.0f};
        id<MTLBuffer> vb = [g_dev newBufferWithBytes:h4 length:sizeof h4 options:MTLResourceStorageModeShared];
        NvShader *vs = [NvShader new];
        vs->fn = [library newFunctionWithName:@"vu"];
        vs->vfn = [library newFunctionWithName:@"vsh"];
        vs->vfn_state = 1;
        vs->rf.has_vin = 1;
        vs->rf.nv = 2;
        vs->rf.v[0].loc = vs->rf.v[0].nvn = 0; vs->rf.v[0].ct = NVN_CT_UINT;
        memset(vs->rf.v[0].sh, 0xff, sizeof vs->rf.v[0].sh); vs->rf.v[0].sh[NVN_CT_FLOAT] = 16; vs->rf.v[0].sh[NVN_CT_INT] = 18;
        vs->rf.v[1].loc = vs->rf.v[1].nvn = 12; vs->rf.v[1].ct = NVN_CT_UINT;
        memset(vs->rf.v[1].sh, 0xff, sizeof vs->rf.v[1].sh); vs->rf.v[1].sh[NVN_CT_FLOAT] = 17; vs->rf.v[1].sh[NVN_CT_INT] = 19;
        state_reset(); S.nattr = 10; S.attr[0].fmt = 0x29; S.attr[0].stream = 0; S.stream[0].stride = 8;
        pso_key key; uint32_t formats[8] = {0};
        pso_key_fill(&key, vs, nil, formats, 0, 0, 1, 0);
        MTLRenderPipelineDescriptor *descriptor = pso_desc(&key, vs, nil, nil, 0);
        descriptor.rasterizationEnabled = NO;
        checks++;
        if (descriptor.vertexFunction != vs->vfn) { failed++; fprintf(stderr, "FAIL shadow-input pipeline not selected\n"); }
        id<MTLRenderPipelineState> pipeline = [g_dev newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.renderTargetWidth = pass.renderTargetHeight = pass.defaultRasterSampleCount = 1;
        memset(out.contents, 0xEE, 32);
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline];
        [encoder setVertexBuffer:out offset:0 atIndex:1];
        [encoder setVertexBuffer:vb offset:0 atIndex:VB_SLOT(0)];
        [encoder setVertexBuffer:vertex_constants() offset:0 atIndex:30];
        [encoder drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:1];
        [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
        if (command.error) { fprintf(stderr, "%s\n", command.error.localizedDescription.UTF8String); return 2; }
        uint32_t want0[4] = {0x3f800000u, 0x40000000u, 0x40400000u, 0u};
        uint32_t want1[4] = {0u, 0u, 0u, 0x3f800000u};
        const uint32_t *got = out.contents;
        checks++;
        if (memcmp(got, want0, sizeof want0)) { failed++; fprintf(stderr, "FAIL shadowed active attribute corrupted: W=%#x\n", got[3]); }
        checks++;
        if (memcmp(got + 4, want1, sizeof want1)) { failed++; fprintf(stderr, "FAIL shadowed absent attribute: W=%#x\n", got[7]); }
    }
    printf("default attributes: %d checks, %d failures\n", checks, failed);
    return failed != 0;
} }
