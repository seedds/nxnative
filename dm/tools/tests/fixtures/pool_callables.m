#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdint.h>
#include <string.h>

// Synthetic texture/sampler resource-ID pools; no captured game input.
int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) return 2;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device || device.argumentBuffersSupport != MTLArgumentBuffersTier2) return 77;
    NSError *error = nil;
    NSString *source = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:&error];
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion4_0;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
    id<MTLFunction> function = [library newFunctionWithName:@"main0"];
    id<MTLComputePipelineState> pipeline = function ? [device newComputePipelineStateWithFunction:function error:&error] : nil;
    if (!pipeline) { fprintf(stderr, "%s\n", error.description.UTF8String); return 2; }
    id<MTLArgumentEncoder> arguments = [function newArgumentEncoderWithBufferIndex:0];
    id<MTLBuffer> argumentBuffer = [device newBufferWithLength:arguments.encodedLength options:MTLResourceStorageModeShared];
    id<MTLBuffer> output = [device newBufferWithLength:64 options:MTLResourceStorageModeShared];
    id<MTLBuffer> seen = [device newBufferWithLength:64 options:MTLResourceStorageModeShared];
    id<MTLBuffer> textures = [device newBufferWithLength:32 options:MTLResourceStorageModeShared];
    id<MTLBuffer> samplers = [device newBufferWithLength:24 options:MTLResourceStorageModeShared];
    for (unsigned i = 0; i < 16; i++) {
        ((uint32_t *)output.contents)[i] = 0xabcd0000u + i;
        ((uint32_t *)seen.contents)[i] = 0xdef00000u + i;
    }
    ((uint32_t *)seen.contents)[4] = 0;
    memset(textures.contents, 0, textures.length);
    memset(samplers.contents, 0, samplers.length);
    ((uint32_t *)textures.contents)[0] = 1;
    ((uint64_t *)textures.contents)[1] = seen.gpuAddress + 16;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:1 height:1 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> first = [device newTextureWithDescriptor:descriptor], second = [device newTextureWithDescriptor:descriptor];
    const float colors[2][4] = {{1,2,3,4},{5,6,7,8}};
    [first replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:colors[0] bytesPerRow:16];
    [second replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:colors[1] bytesPerRow:16];
    ((uint64_t *)textures.contents)[2] = first.gpuResourceID._impl;
    ((uint64_t *)textures.contents)[3] = second.gpuResourceID._impl;
    MTLSamplerDescriptor *samplerDescriptor = [MTLSamplerDescriptor new];
    samplerDescriptor.supportArgumentBuffers = YES;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:samplerDescriptor];
    ((uint64_t *)samplers.contents)[2] = sampler.gpuResourceID._impl;
    [arguments setArgumentBuffer:argumentBuffer offset:0];
    [arguments setBuffer:output offset:16 atIndex:0];
    [arguments setBuffer:textures offset:0 atIndex:1];
    [arguments setBuffer:samplers offset:0 atIndex:2];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:argumentBuffer offset:0 atIndex:0];
    for (id<MTLResource> resource in @[output,seen,textures,samplers,first,second])
        [encoder useResource:resource usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(2,1,1)];
    [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
    if (command.error) { fprintf(stderr, "%s\n", command.error.description.UTF8String); return 1; }
    if (memcmp((uint8_t *)output.contents + 16, colors, sizeof colors)) return 1;
    for (unsigned i = 0; i < 16; i++) {
        if ((i < 4 || i >= 12) && ((uint32_t *)output.contents)[i] != 0xabcd0000u + i) return 1;
        if (((uint32_t *)seen.contents)[i] != (i == 4 ? 3 : 0xdef00000u + i)) return 1;
    }
    puts("callable pool GPU: two colors, nested forwarding, read bitmap and guards passed");
    return 0;
} }
