// A register-heavy fixed-size kernel must execute every lane of its group.
#import <Foundation/Foundation.h>
#include "../nvn/nvn_compute.h"
#include <math.h>
#include <stdio.h>

int main(void) { @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) { puts("no Metal device"); return 2; }
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal; kernel void check(device const float *in [[buffer(0)]], device float *out [[buffer(1)]], uint3 local [[thread_position_in_threadgroup]], uint3 group [[threadgroup_position_in_grid]], uint3 size [[threads_per_threadgroup]]) { uint lane=local.x+size.x*(local.y+size.y*local.z), gid=group.x*(size.x*size.y*size.z)+lane; float a[32]; for(uint j=0;j<32;j++) a[j]=in[gid]+float(j)*0.001; for(uint k=0;k<8;k++) for(uint j=0;j<32;j++) a[j]=fma(a[j],1.001,a[(j+lane+1)&31]*0.001); float sum=0; for(uint j=0;j<32;j++) sum+=a[j]; out[gid]=sum; }";
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (!library) { puts(error.localizedDescription.UTF8String); return 1; }
    id<MTLFunction> function = [library newFunctionWithName:@"check"];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    const uint16_t shapes[][3] = {{1024, 1, 1}, {32, 8, 2}, {64, 1, 1}};
    for (unsigned test = 0; test < 3; test++) {
        const uint16_t *tg = shapes[test]; NSUInteger count = (NSUInteger)tg[0] * tg[1] * tg[2];
        id<MTLComputePipelineState> pipeline = nvn_compute_pipeline(device, function, tg, &error);
        if (!pipeline || pipeline.maxTotalThreadsPerThreadgroup < count) { puts(error ? error.localizedDescription.UTF8String : "fixed group does not fit"); return 1; }
        id<MTLBuffer> input = [device newBufferWithLength:2 * count * sizeof(float) options:MTLResourceStorageModeShared];
        id<MTLBuffer> output = [device newBufferWithLength:input.length options:MTLResourceStorageModeShared];
        float *in = input.contents, *out = output.contents;
        for (NSUInteger i = 0; i < 2 * count; i++) { in[i] = (float)(i + 1); out[i] = NAN; }
        id<MTLCommandBuffer> cb = [queue commandBuffer]; id<MTLComputeCommandEncoder> encoder = [cb computeCommandEncoder];
        [encoder setComputePipelineState:pipeline]; [encoder setBuffer:input offset:0 atIndex:0]; [encoder setBuffer:output offset:0 atIndex:1];
        [encoder dispatchThreadgroups:MTLSizeMake(2, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg[0], tg[1], tg[2])];
        [encoder endEncoding]; [cb commit]; [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) { puts(cb.error.localizedDescription.UTF8String); return 1; }
        for (NSUInteger i = 0; i < 2 * count; i++) if (!isfinite(out[i]) || out[i] < in[i] * 32 || out[i] > (in[i] + 1) * 34) { printf("missing or incorrect invocation %lu\n", (unsigned long)i); return 1; }
        printf("%ux%ux%u: %lu invocations PASS\n", tg[0], tg[1], tg[2], (unsigned long)(2 * count));
    }
    return 0;
} }
