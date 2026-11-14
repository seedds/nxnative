// Classic Metal adapter: dispatch/copy/dispatch must preserve encoder state.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include "../nvn_metal3_compat.inc"
int main(void) { @autoreleasepool {
    NX3Device *device=(NX3Device *)nx3_create_device(); assert(device.native);
    id<MTLCommandQueue> executionQueue = [device newCommandQueue];
    id<MTLCommandQueue> diagnosticQueue = [device newCommandQueue];
    assert(executionQueue != diagnosticQueue);
    assert(device.queue == executionQueue);
    NSError *error=nil;
    NSString *source=@"#include <metal_stdlib>\nusing namespace metal; kernel void increment(device uint *v [[buffer(0)]], threadgroup uint *scratch [[threadgroup(0)]]) { scratch[0]=v[0]+1; threadgroup_barrier(mem_flags::mem_threadgroup); v[0]=scratch[0]; }";
    id<MTLLibrary> lib=[device.native newLibraryWithSource:source options:nil error:&error];
    if (!lib) { NSLog(@"%@",error); return 1; }
    id<MTLComputePipelineState> pso=[device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"increment"] error:&error]; assert(pso);
    uint32_t initial=40;
    id<MTLBuffer> value=[device newBufferWithBytes:&initial length:4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> copy=[device newBufferWithLength:4 options:MTLResourceStorageModeShared];
    NX3Table *table=[NX3Table new]; [table setAddress:value.gpuAddress atIndex:0];
    NX3CommandBuffer *cb=[device newCommandBuffer]; [cb beginCommandBufferWithAllocator:nil];
    NX3Encoder *enc=(id)[cb computeCommandEncoder];
    [enc setArgumentTable:table]; [enc setComputePipelineState:pso]; [enc setThreadgroupMemoryLength:16 atIndex:0];
    [enc dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
    [(id<MTLBlitCommandEncoder>)[enc blit] copyFromBuffer:value sourceOffset:0 toBuffer:copy destinationOffset:0 size:4];
    // No second PSO, table or threadgroup-memory bind: MTL4 preserves them.
    [enc dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
    [enc endEncoding]; [cb endCommandBuffer];
    NX3Queue *q=[device newMTL4CommandQueueWithDescriptor:[NX3QueueDescriptor new] error:&error];
    id<NX3CommandBuffer> buffers[]={cb}; [q commit:buffers count:1]; [cb.native waitUntilCompleted];
    if (cb.native.error) { NSLog(@"%@",cb.native.error); return 1; }
    uint32_t got=*(uint32_t *)value.contents, copied=*(uint32_t *)copy.contents;
    if (got!=42 || copied!=41) { fprintf(stderr,"FAIL: final=%u copied=%u\n",got,copied); return 1; }
    puts("PASS: real GPU compute/blit/compute preserved pipeline, bindings, threadgroup allocation and ordering (42/41)");
} return 0; }
