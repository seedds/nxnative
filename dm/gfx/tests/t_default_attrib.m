// An absent float attribute keeps its implicit W component for alpha-tested depth passes.
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
        "vertex void vf(F in [[stage_in]], device uint4 *o [[buffer(1)]]) { *o=as_type<uint4>(in.a); }\n"
        "vertex void vu(U in [[stage_in]], device uint4 *o [[buffer(1)]]) { *o=in.a; }\n"
        "vertex void vi(I in [[stage_in]], device uint4 *o [[buffer(1)]]) { *o=as_type<uint4>(in.a); }\n";
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:source options:nil error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
    id<MTLCommandQueue> queue = [g_dev newCommandQueue];
    id<MTLBuffer> out = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
    int failed = 0, checks = 0;
    NSString *names[] = {@"vf", @"vu", @"vi"};
    for (int type = 0; type < 3; type++) for (int declared = 0; declared < 2; declared++) {
        NvShader *vs = [NvShader new];
        vs->fn = [library newFunctionWithName:names[type]];
        vs->rf.nv = 1;
        vs->rf.v[0].loc = vs->rf.v[0].nvn = 12;
        vs->rf.v[0].ct = type;
        memset(vs->rf.v[0].sh, 0xff, sizeof vs->rf.v[0].sh);
        state_reset(); S.nattr = declared ? 13 : 10;
        pso_key key; uint32_t formats[8] = {0};
        pso_key_fill(&key, vs, nil, formats, 0, 0, 1, 0);
        MTLRenderPipelineDescriptor *descriptor = pso_desc(&key, vs, nil, nil, 0);
        descriptor.rasterizationEnabled = NO;
        id<MTLRenderPipelineState> pipeline = [g_dev newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.renderTargetWidth = pass.renderTargetHeight = pass.defaultRasterSampleCount = 1;
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline];
        [encoder setVertexBuffer:out offset:0 atIndex:1];
        [encoder setVertexBuffer:vertex_constants() offset:0 atIndex:30];
        [encoder drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:1];
        [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
        if (command.error) { fprintf(stderr, "%s\n", command.error.localizedDescription.UTF8String); return 2; }
        uint32_t expected[] = {0, 0, 0, 0x3f800000};
        checks++;
        if (memcmp(out.contents, expected, sizeof expected)) {
            failed++; fprintf(stderr, "FAIL type %d, declared %d: default W=%#x\n", type, declared, ((uint32_t *)out.contents)[3]);
        }
    }
    printf("default attributes: %d checks, %d failures\n", checks, failed);
    return failed != 0;
} }
