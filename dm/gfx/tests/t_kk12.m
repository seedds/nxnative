// KK_WORKAROUND_12 (nvn_mtl.m kk12_len): 16-bit indices drawn as points, one column each of a 64x1 target, through the replay's two Metal 4 indexed draw wrappers
// (enc_r_draw_indexed, and enc_r_draw_indexed_indirect, which needs no guard). macOS 26 drops the last index when the index buffer starts and ends off a 4-byte boundary; with the guard every index is drawn.
// On macOS 26 the non-indirect "start and end off" cases fail with DM_NVN_KK12=0 (the guard off); the other cases, the indirect one included, are controls that draw every
// index either way. macOS 27 has the fix and passes either way. A few small draws, bounded
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

int main(void) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); NSError *e = nil;
    if (!dev) { fprintf(stderr, "no Metal device\n"); return 1; }
    id<MTLLibrary> l = [dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 p [[position]]; float s [[point_size]]; };\n"
        "vertex V vs(uint vid [[vertex_id]]) { V o; o.p = float4((float(vid) + 0.5) / 32.0 - 1.0, 0.0, 0.5, 1.0); o.s = 1.0; return o; }\n"   // column vid of a 64x1 target
        "fragment float4 fs(V v [[stage_in]]) { return float4(1.0); }\n" options:nil error:&e];
    CHECK(l != nil, "the point shaders compile"); if (!l) return 1;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [l newFunctionWithName:@"vs"]; pd.fragmentFunction = [l newFunctionWithName:@"fs"]; pd.colorAttachments[0].pixelFormat = MTLPixelFormatR8Unorm; pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassPoint;
    id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
    CHECK(p != nil, "the point pipeline builds"); if (!p) return 1;
    id<MTL4CommandQueue> q4 = [dev newMTL4CommandQueue]; id<MTL4CommandAllocator> al = [dev newCommandAllocator];
    id<MTLSharedEvent> ev = [dev newSharedEvent]; uint64_t evv = 0;
    id<MTLResidencySet> rs = [dev newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&e];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:64 height:1 mipmapped:NO]; td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    id<MTLBuffer> ib = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared], ia = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    CHECK(q4 && al && ev && rs && ib && ia, "the Metal 4 queue, residency set and buffers"); if (!q4 || !al || !ev || !rs || !ib || !ia) return 1;
    [rs addAllocation:ib]; [rs addAllocation:ia]; [rs commit]; [q4 addResidencySet:rs];
    printf("t_kk12: macOS %ld, guard %s\n", (long)NSProcessInfo.processInfo.operatingSystemVersion.majorVersion, kk12_on() ? "on" : "off");
    const struct { const char *what; int off, n, indirect; } C[] = {
        {"6 indices, start and end aligned (control)", 0, 6, 0},
        {"5 indices, end off a 4-byte boundary", 0, 5, 0},
        {"3 indices at offset 2, end aligned", 2, 3, 0},
        {"4 indices at offset 2, start and end off a 4-byte boundary", 2, 4, 0},
        {"2 indices at offset 2, start and end off a 4-byte boundary", 2, 2, 0},
        {"4 indices at offset 2, start and end off, indirect (a control: the indirect form draws them all)", 2, 4, 1},
    };
    for (unsigned c = 0; c < sizeof C / sizeof *C; c++) {
        uint16_t *h = ib.contents; for (int j = 0; j < 2048; j++) h[j] = 63;   // past the indices: column 63
        for (int j = 0; j < C[c].n; j++) h[C[c].off / 2 + j] = (uint16_t)(1 + j);   // columns 1..n
        id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
        CHECK(rt != nil, "the target"); if (!rt) break;
        [rs addAllocation:rt]; [rs commit];   // Metal 4: the attachment too must be resident
        MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new]; rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTL4CommandBuffer> cb = [dev newCommandBuffer]; [al reset]; [cb beginCommandBufferWithAllocator:al];
        id<MTL4RenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp]; [re setRenderPipelineState:p];
        uint64_t gpu = ib.gpuAddress + (uint64_t)C[c].off; NSUInteger len = (NSUInteger)C[c].n * 2;
        if (C[c].indirect) { *(MTLDrawIndexedPrimitivesIndirectArguments *)ia.contents = (MTLDrawIndexedPrimitivesIndirectArguments){(uint32_t)C[c].n, 1, 0, 0, 0};
            enc_r_draw_indexed_indirect(re, MTLPrimitiveTypePoint, MTLIndexTypeUInt16, gpu, len, ia.gpuAddress); }
        else enc_r_draw_indexed(re, MTLPrimitiveTypePoint, (NSUInteger)C[c].n, MTLIndexTypeUInt16, gpu, len, 1, 0, 0);
        [re endEncoding]; [cb endCommandBuffer];
        [q4 commit:&cb count:1]; [q4 signalEvent:ev value:++evv];
        int done = [ev waitUntilSignaledValue:evv timeoutMS:5000];
        CHECK(done, "the draw completes"); if (!done) break;
        uint8_t px[64]; [rt getBytes:px bytesPerRow:64 fromRegion:MTLRegionMake2D(0, 0, 64, 1) mipmapLevel:0];
        int got = 0; for (int j = 1; j <= C[c].n; j++) got += px[j] != 0;
        char label[200]; snprintf(label, sizeof label, "%s: %d of %d indices drawn", C[c].what, got, C[c].n);
        CHECK(got == C[c].n, label);
        snprintf(label, sizeof label, "%s: nothing past the indices is drawn", C[c].what); CHECK(px[63] == 0, label);
        [rs removeAllocation:rt]; [rs commit];
    }
    printf("t_kk12: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
