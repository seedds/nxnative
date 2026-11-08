#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(int argc, const char **argv) { @autoreleasepool {
    assert(argc == 2);
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
    assert(library);
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"main0"] error:&error];
    assert(pipeline);
    id<MTLBuffer> values = [device newBufferWithLength:8 options:MTLResourceStorageModeShared];
    id<MTLBuffer> args = [device newBufferWithLength:8 options:MTLResourceStorageModeShared];
    *(uint64_t *)args.contents = values.gpuAddress;
    id<MTLCommandQueue> queue = [device newCommandQueue];
    const uint32_t inputs[] = {0, 0x80000000, 1, 0x80000001, 0x3f800000, 0xbf800000,
                              0x7f800000, 0xff800000, 0x7fc12345, 0xffc12345, 0x7f800001, 0x7f7fffff};
    unsigned checks = 0, bad = 0;
    for (unsigned n = 0; n < sizeof(inputs) / sizeof(inputs[0]); n++) {
        uint32_t *words = values.contents;
        words[0] = inputs[n]; words[1] = 0x55555555;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [cb computeCommandEncoder];
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:args offset:0 atIndex:0];
        [encoder useResource:values usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding]; [cb commit]; [cb waitUntilCompleted];
        assert(!cb.error);
        checks++;
        if (words[1] != inputs[n]) {
            bad++;
            printf("old n=%u got=%08x want=%08x\n", n, words[1], inputs[n]);
        }
        float old, actual;
        memcpy(&old, &inputs[n], 4); memcpy(&actual, words, 4);
        float expected = old + 1.5f;
        checks++;
        if (!(isnan(expected) && isnan(actual)) && expected != actual) bad++;
    }
    printf("ATOMG_return_checks=%u failures=%u inputs=12 old_bits=exact sum=checked\n", checks, bad);
    return bad ? 1 : 0;
} }
