// The task-shader hang, reproduced without the game: a task shader that reads both ends of a range through a pointer at byte 0 of storage slot 0 (f75708d176bb6591, 47f9b38490dcf9a2,
// 99280b7b8aae78df, 2b97acec27eb47ad) loops (min(end - start, 256) - 1) >> 5 times; when the raw pool table misses, both ends read 0 and the count is uint(-1) >> 5 = 134,217,727.
// The pointer points into pool M, whose words are 0, 1, 2, ..., so any two reads give a short range. Cases:
//   A  M in the pool table, no site entry            -> the table finds M: no hang
//   B  M left out of the table, no site entry        -> the loads read 0: hang (GPU timeout)
//   C  M left out of the table, site 0 entry = M     -> the bind-time entry finds M: no hang
// Usage: task_hang <dir of <key>_b.metal/json built with the gptr translator> <key> <A|B|C>
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#include <stdio.h>
enum { ENTRIES = 8192, EW = 8 };
static void entry(uint32_t *t, int k, uint64_t lo, uint64_t hi, uint64_t delta) { uint32_t *e = t + (size_t)k * EW; e[0] = (uint32_t)lo; e[1] = (uint32_t)(lo >> 32); e[2] = (uint32_t)hi; e[3] = (uint32_t)(hi >> 32); e[4] = (uint32_t)delta; e[5] = (uint32_t)(delta >> 32); e[6] = e[7] = 0; }
int main(int argc, char **argv) { @autoreleasepool {
    if (argc < 4) { fprintf(stderr, "usage: task_hang <dir> <key> <A|B|C>\n"); return 2; }
    NSString *dir = @(argv[1]), *key = @(argv[2]); char mode = argv[3][0];
    NSDictionary *j = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:[dir stringByAppendingFormat:@"/%@_b.json", key]] options:0 error:nil];
    NSString *src = [NSString stringWithContentsOfFile:[dir stringByAppendingFormat:@"/%@_b.metal", key] encoding:NSUTF8StringEncoding error:nil];
    src = [src stringByAppendingString:@"\nstruct dm_sweep_v { float4 p [[position]]; };\n[[mesh]] void dm_sweep_mesh(metal::mesh<dm_sweep_v, void, 3, 1, metal::topology::triangle> m, const object_data dm_payload_t& p [[payload]]) { }\n"];
    int threads = [j[@"mesh_stage"][@"threads"] intValue] ?: 32, payload = [j[@"mesh_stage"][@"payload"] intValue];
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q = [dev newCommandQueue]; NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:[MTLCompileOptions new] error:&err]; if (!lib) { printf("compile failed\n"); return 1; }
    MTLMeshRenderPipelineDescriptor *d = [MTLMeshRenderPipelineDescriptor new];
    d.objectFunction = [lib newFunctionWithName:j[@"entry"] ?: @"main0"]; d.maxTotalThreadsPerObjectThreadgroup = threads; d.payloadMemoryLength = (payload + 15) & ~15;
    d.meshFunction = [lib newFunctionWithName:@"dm_sweep_mesh"]; d.maxTotalThreadsPerMeshThreadgroup = 1;
    d.fragmentFunction = [[dev newLibraryWithSource:@"fragment void f() {}" options:nil error:nil] newFunctionWithName:@"f"]; d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithMeshDescriptor:d options:0 reflection:nil error:&err]; if (!ps) { printf("pipeline failed: %s\n", err.localizedDescription.UTF8String); return 1; }
    const uint64_t G = 0x200000000ull, MSZ = 1 << 20;
    id<MTLBuffer> zero = [dev newBufferWithLength:MSZ options:MTLResourceStorageModeShared]; memset(zero.contents, 0, MSZ);
    id<MTLBuffer> M = [dev newBufferWithLength:MSZ options:MTLResourceStorageModeShared]; for (uint32_t i = 0; i < MSZ / 4; i++) ((uint32_t *)M.contents)[i] = i;
    id<MTLBuffer> S0 = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared]; memset(S0.contents, 0, 4096); uint64_t ptr = G + 4096; memcpy(S0.contents, &ptr, 8);   // the pointer word at byte 0
    id<MTLBuffer> tab = [dev newBufferWithLength:(size_t)ENTRIES * EW * 4 options:MTLResourceStorageModeShared]; uint32_t *t = tab.contents; memset(t, 0, tab.length);
    for (int k = 1; k < ENTRIES; k++) { t[k * EW] = 0xffffffffu; t[k * EW + 1] = 0xffffffffu; }
    uint32_t count = ENTRIES; memcpy(t + 4, &count, 4);
    if (mode == 'A') entry(t, 1, G, G + MSZ, M.gpuAddress - G);
    id<MTLBuffer> sites = [dev newBufferWithLength:64 * EW * 4 options:MTLResourceStorageModeShared]; memset(sites.contents, 0, sites.length);
    if (mode == 'C') entry(sites.contents, 0, G, G + MSZ, M.gpuAddress - G);
    int maxid = 0; for (NSDictionary *b in j[@"bindings"]) maxid = MAX(maxid, [b[@"arg_id"] intValue] + 1);
    id<MTLBuffer> ab = [dev newBufferWithLength:(size_t)maxid * 8 + 64 options:MTLResourceStorageModeShared]; uint64_t *slots = ab.contents;
    for (NSDictionary *b in j[@"bindings"]) { int a = [b[@"arg_id"] intValue]; NSString *k = b[@"kind"];
        slots[a] = [k isEqualToString:@"gmem_map"] ? tab.gpuAddress : [k isEqualToString:@"gptr"] ? sites.gpuAddress : ([k isEqualToString:@"sbuf"] && [b[@"reg"] intValue] == 0) ? S0.gpuAddress : zero.gpuAddress; }
    id<MTLTexture> rt = [dev newTextureWithDescriptor:({ MTLTextureDescriptor *x = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO]; x.usage = MTLTextureUsageRenderTarget; x.storageMode = MTLStorageModePrivate; x; })];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor]; rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:ps]; [e setObjectBuffer:ab offset:0 atIndex:0];
    for (id<MTLBuffer> b in @[zero, M, S0, tab, sites]) [e useResource:b usage:MTLResourceUsageRead stages:MTLRenderStageObject];
    uint32_t first = 0; [e setObjectBytes:&first length:4 atIndex:30];
    [e drawMeshThreadgroups:MTLSizeMake(1, 1, 1) threadsPerObjectThreadgroup:MTLSizeMake(threads, 1, 1) threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
    [e endEncoding]; double t0 = CACurrentMediaTime(); [cb commit]; [cb waitUntilCompleted]; double dt = CACurrentMediaTime() - t0;
    printf("%s case %c: %s (%.2f s, GPU %.3f s)\n", argv[2], mode, cb.error ? cb.error.localizedDescription.UTF8String : "completed", dt, cb.GPUEndTime - cb.GPUStartTime);
    return 0;
} }
