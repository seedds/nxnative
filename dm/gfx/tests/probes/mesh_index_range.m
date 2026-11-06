// Does an index past the mesh's vertex count hang Apple's GPU? A mesh with 64 declared vertices and 126 primitives sets every vertex, then emits triangles whose
// indices are (a) all in range, (b) one index of 200 (past the 64), (c) every index 255. Usage: mesh_index_range <a|b|c> [groups]
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#include <stdio.h>
static NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; };\n"
 "using M = metal::mesh<V, void, 64, 126, metal::topology::triangle>;\n"
 "[[mesh]] void m(M out, uint lid [[thread_position_in_threadgroup]], constant uint& mode [[buffer(0)]]) {\n"
 "  if (lid == 0) out.set_primitive_count(126);\n"
 "  for (uint n = lid; n < 64; n += 32) { V v; v.p = float4(float(n % 8) / 4.0 - 1.0, float(n / 8) / 4.0 - 1.0, 0.5, 1.0); out.set_vertex(n, v); }\n"
 "  for (uint n = lid; n < 378; n += 32) { uint i = n % 64; if (mode == 1 && n == 7) i = 200; if (mode == 2) i = 255; out.set_index(n, uchar(i)); }\n"
 "}\nfragment float4 f() { return float4(1); }\n";
int main(int argc, char **argv) { @autoreleasepool {
    uint32_t mode = argc > 1 ? (uint32_t)(argv[1][0] - 'a') : 0; int groups = argc > 2 ? atoi(argv[2]) : 1;
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q = [dev newCommandQueue]; NSError *e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&e]; if (!lib) { printf("compile: %s\n", e.localizedDescription.UTF8String); return 1; }
    MTLMeshRenderPipelineDescriptor *d = [MTLMeshRenderPipelineDescriptor new]; d.meshFunction = [lib newFunctionWithName:@"m"]; d.fragmentFunction = [lib newFunctionWithName:@"f"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm; d.maxTotalThreadsPerMeshThreadgroup = 32;
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithMeshDescriptor:d options:0 reflection:nil error:&e]; if (!ps) { printf("pipeline: %s\n", e.localizedDescription.UTF8String); return 1; }
    id<MTLTexture> rt = [dev newTextureWithDescriptor:({ MTLTextureDescriptor *t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:256 height:256 mipmapped:NO]; t.usage = MTLTextureUsageRenderTarget; t.storageMode = MTLStorageModePrivate; t; })];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor]; rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    [en setRenderPipelineState:ps]; [en setMeshBytes:&mode length:4 atIndex:0];
    [en drawMeshThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1) threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
    [en endEncoding]; double t0 = CACurrentMediaTime(); [cb commit]; [cb waitUntilCompleted];
    printf("mode %c, %d groups: %s (%.3f s)\n", 'a' + mode, groups, cb.error ? cb.error.localizedDescription.UTF8String : "completed", CACurrentMediaTime() - t0);
    return 0;
} }
