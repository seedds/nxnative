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
int main(int argc, const char **argv) { @autoreleasepool {
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
    // Persisted table slot whose allocation has retired; this shader only uses slot 0.
    __weak id<MTLBuffer> retired;
    @autoreleasepool {
        id<MTLBuffer> old=[device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        retired=old; [table setAddress:old.gpuAddress atIndex:7];
    }
    assert(retired == nil);
    // A genuinely used invalid address must still fail, not silently bind nil.
    if (argc > 1 && strcmp(argv[1], "--used-missing") == 0) [table setAddress:table->addresses[7] atIndex:0];
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
    // Stage-in buffers are not ordinary buffer arguments in all reflection.
    // Switch from a vertex-fetch pipeline to a vertex-id-only helper while an
    // unused table slot points at the retired allocation from the previous scene.
    NSString *renderSource = @"#include <metal_stdlib>\nusing namespace metal; struct V { float2 p [[attribute(0)]]; }; vertex float4 fetched(V v [[stage_in]]) { return float4(v.p,0,1); } vertex float4 helper(uint i [[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0,1); } fragment float4 red() { return float4(1,0,0,1); }";
    id<MTLLibrary> renderLib = [device.native newLibraryWithSource:renderSource options:nil error:&error]; assert(renderLib);
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [renderLib newFunctionWithName:@"fetched"]; pd.fragmentFunction = [renderLib newFunctionWithName:@"red"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    pd.vertexDescriptor = [MTLVertexDescriptor vertexDescriptor];
    pd.vertexDescriptor.attributes[0].format = MTLVertexFormatFloat2; pd.vertexDescriptor.attributes[0].bufferIndex = 3;
    pd.vertexDescriptor.layouts[3].stride = 8;
    id<MTLRenderPipelineState> fetched = [device newRenderPipelineStateWithDescriptor:pd error:&error]; assert(fetched);
    pd.vertexFunction = [renderLib newFunctionWithName:@"helper"]; pd.vertexDescriptor = nil;
    id<MTLRenderPipelineState> helper = [device newRenderPipelineStateWithDescriptor:pd error:&error]; assert(helper);
    float vertices[] = {-1,-1,3,-1,-1,3};
    id<MTLBuffer> vertexBuffer = [device newBufferWithBytes:vertices length:sizeof vertices options:MTLResourceStorageModeShared];
    [table setAddress:vertexBuffer.gpuAddress atIndex:3];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    [cb beginCommandBufferWithAllocator:nil];
    enc = (id)[cb renderCommandEncoderWithDescriptor:rp];
    [enc setArgumentTable:table atStages:MTLRenderStageVertex];
    [enc setRenderPipelineState:fetched]; [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc setRenderPipelineState:helper]; [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc setRenderPipelineState:fetched]; [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding]; [cb endCommandBuffer]; [q commit:buffers count:1]; [cb.native waitUntilCompleted];
    assert(!cb.native.error);
    uint8_t pixels[64]; [target getBytes:pixels bytesPerRow:16 fromRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:0];
    for (int i=0;i<16;i++) assert(pixels[i*4]==255 && pixels[i*4+1]==0 && pixels[i*4+2]==0 && pixels[i*4+3]==255);
    puts("PASS: stage-in vertex fetch and pipeline changes render red with a retired unused table slot");
    puts("PASS: retired unused binding ignored; real GPU compute/blit/compute preserved pipeline, bindings, threadgroup allocation and ordering (42/41)");
} return 0; }
