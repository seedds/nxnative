// Execute the synthetic ATOM.CAS fixture: check old value, replacement and all surrounding words.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <assert.h>

int main(int argc, const char **argv) { @autoreleasepool {
    assert(argc == 7);
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); assert(dev);
    NSError *error = nil;
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error]; assert(lib && !error);
    id<MTLFunction> fn = [lib newFunctionWithName:@"main0"]; assert(fn);
    id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:fn error:&error]; assert(pso && !error);
    id<MTLBuffer> data = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    uint32_t *words = data.contents; for (int i = 0; i < 1024; i++) words[i] = 0xabcdef01;
    words[0] = 7; words[1] = 9;
    id<MTLBuffer> c0 = [dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
    // Guest virtual address, not a Metal pointer: the translator resolves the c0 descriptor to s0.
    uint32_t *cb = c0.contents; cb[0x140 / 4] = 0x10000; cb[0x144 / 4] = 0; cb[0x148 / 4] = 4096;
    cb[0x150 / 4] = 1; // Runtime predicate input.
    id<MTLArgumentEncoder> args = [fn newArgumentEncoderWithBufferIndex:0];
    id<MTLBuffer> ab = [dev newBufferWithLength:args.encodedLength options:MTLResourceStorageModeShared];
    [args setArgumentBuffer:ab offset:0];
    if (atoi(argv[2]) >= 0) [args setBuffer:c0 offset:0 atIndex:atoi(argv[2])];
    [args setBuffer:data offset:0 atIndex:atoi(argv[3])];
    id<MTLCommandQueue> queue = [dev newCommandQueue]; id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [command computeCommandEncoder];
    [enc setComputePipelineState:pso]; [enc setBuffer:ab offset:0 atIndex:0];
    [enc useResource:data usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    [enc useResource:c0 usage:MTLResourceUsageRead];
    [enc dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [enc endEncoding]; [command commit]; [command waitUntilCompleted];
    assert(command.status == MTLCommandBufferStatusCompleted);
    int at = atoi(argv[4]); uint32_t value = (uint32_t)strtoul(argv[5], NULL, 0), old = (uint32_t)strtoul(argv[6], NULL, 0);
    for (int i = 0; i < 1024; i++) {
        uint32_t expected = i == at ? value : i == 8 ? old : i == 0 ? 7 : i == 1 ? 9 : 0xabcdef01;
        if (words[i] != expected) { fprintf(stderr, "word %d: got %#x, expected %#x\n", i, words[i], expected); return 1; }
    }
    puts("global CAS GPU: old value, replacement and sentinels passed"); return 0;
}}
