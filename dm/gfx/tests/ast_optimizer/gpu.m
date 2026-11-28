// Run the ordinary-IR fixture and compare every data/guard word in both modes.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdint.h>
#include <string.h>

int main(int argc, const char **argv) { @autoreleasepool {
    if (argc != 2) return 2;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return 77;
    id<MTLCommandQueue> queue = [device newCommandQueue];
    enum { GUARD = 16, WORDS = 64 };
    uint32_t initial[WORDS], expected[WORDS];
    for (unsigned i = 0; i < WORDS; i++) initial[i] = 0xc1000000u + i * 0x103u;
    memcpy(expected, initial, sizeof expected);
    // Independent arithmetic/memory model of Program.cs, not generated-source matching.
    expected[GUARD] = 6 * (3 + 4);
    expected[GUARD + 1] = 6 + (3 + 4);
    expected[GUARD + 2] = 77;
    expected[GUARD + 3] = 99;
    expected[GUARD + 4] = initial[GUARD + 3] + 7;
    expected[GUARD + 5] = 13;
    expected[GUARD + 6] = 10 + 11;
    expected[GUARD + 7] = initial[GUARD + 7] + 5;

    unsigned failures = 0;
    for (NSString *mode in @[@"optimized", @"debug"]) {
        NSError *error = nil;
        NSString *path = [@(argv[1]) stringByAppendingPathComponent:[mode stringByAppendingString:@".metal"]];
        NSString *source = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&error];
        MTLCompileOptions *options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_4;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
        id<MTLFunction> function = [library newFunctionWithName:@"main0"];
        id<MTLComputePipelineState> pipeline = function ? [device newComputePipelineStateWithFunction:function error:&error] : nil;
        if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
        id<MTLBuffer> data = [device newBufferWithBytes:initial length:sizeof initial options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:data offset:GUARD * sizeof(uint32_t) atIndex:1];
        [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
        if (command.error) { fprintf(stderr, "%s\n", command.error.description.UTF8String); return 2; }
        const uint32_t *got = data.contents;
        unsigned bad = 0;
        for (unsigned i = 0; i < WORDS; i++) if (got[i] != expected[i]) {
            fprintf(stderr, "%s word %u: got %#x, expected %#x\n", mode.UTF8String, i, got[i], expected[i]); bad++;
        }
        failures += bad;
        printf("%s: %u words, %u mismatches\n", mode.UTF8String, WORDS, bad);
    }
    printf("device: %s\n", device.name.UTF8String);
    return failures != 0;
} }
