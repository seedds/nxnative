// Execute a generated IGN image atomic fixture on a native atomic texture (dmsh_atomic.py). First max(value, 1) at the fixture's
// coordinate, then add its returned old value to texel zero; invalid first coordinates add zero. Every other texel must stay 3.
// Usage: t_atomic_return <metallib> <expected texel 0> <buffer|2d|2darray|3d> [<bias> <table id> <image id>]; the image is 8 wide, 4 high, 3 layers or slices deep.
// A buffer image is bound as the host binds one (nvn_mtl.m tbias_view): a view from word 0 of its buffer, bias elements wider, its texel 0 at word bias, and bias in the
// shader's offset table (dmsh_abi.h DMSH_TBIAS_BANK, argument buffer member <table id>), so the words before the image are checked to stay 3 as well
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <assert.h>
#include <string.h>

int main(int argc, const char **argv) { @autoreleasepool {
    assert(argc == 4 || argc == 7);   // the image is argument buffer member 0 unless the shader has the offset table, whose member comes first
    const uint32_t bias = argc == 7 ? (uint32_t)atoi(argv[4]) : 0; const int table_id = argc == 7 ? atoi(argv[5]) : -1, image_id = argc == 7 ? atoi(argv[6]) : 0;
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); assert(dev);
    NSError *error = nil;
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
    assert(lib && !error);
    id<MTLFunction> fn = [lib newFunctionWithName:@"main0"]; assert(fn);
    id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:fn error:&error];
    assert(pso && !error);
    const char *shape = argv[3];
    MTLTextureUsage usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageShaderAtomic;
    enum { W = 8, H = 4, L = 3 };
    uint32_t init[W * H * L]; for (int i = 0; i < W * H * L; i++) init[i] = 3;
    id<MTLBuffer> data = nil; id<MTLTexture> tex; NSUInteger n;
    if (!strcmp(shape, "buffer")) {   // a view of a buffer, as the host makes for buffer images; texels outside it stay 3 too
        data = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        uint32_t *words = data.contents; for (int i = 0; i < 1024; i++) words[i] = 3;
        MTLTextureDescriptor *desc = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:W + bias resourceOptions:data.resourceOptions usage:usage];
        tex = [data newTextureWithDescriptor:desc offset:0 bytesPerRow:((W + bias) * 4 + 15) / 16 * 16]; n = 1024;
    } else {
        MTLTextureDescriptor *desc = [MTLTextureDescriptor new];
        desc.textureType = !strcmp(shape, "2d") ? MTLTextureType2D : !strcmp(shape, "2darray") ? MTLTextureType2DArray : MTLTextureType3D;
        assert(desc.textureType != MTLTextureType3D || !strcmp(shape, "3d"));
        desc.pixelFormat = MTLPixelFormatR32Uint; desc.width = W; desc.height = H;
        desc.depth = desc.textureType == MTLTextureType3D ? L : 1; desc.arrayLength = desc.textureType == MTLTextureType2DArray ? L : 1;
        desc.storageMode = MTLStorageModeShared; desc.usage = usage;
        tex = [dev newTextureWithDescriptor:desc]; assert(tex);
        n = desc.textureType == MTLTextureType2D ? W * H : W * H * L;
        for (NSUInteger s = 0; s < desc.arrayLength; s++)
            [tex replaceRegion:MTLRegionMake3D(0, 0, 0, W, H, desc.depth) mipmapLevel:0 slice:s withBytes:init + s * W * H bytesPerRow:W * 4 bytesPerImage:W * H * 4];
    }
    assert(tex);
    id<MTLArgumentEncoder> args = [fn newArgumentEncoderWithBufferIndex:0];
    id<MTLBuffer> ab = [dev newBufferWithLength:args.encodedLength options:MTLResourceStorageModeShared];
    [args setArgumentBuffer:ab offset:0];
    [args setTexture:tex atIndex:(NSUInteger)image_id];
    id<MTLBuffer> table = nil;
    if (table_id >= 0) {   // the offset table: four uint4, the image's element offset at index 0
        table = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared]; memset(table.contents, 0, 64); memcpy(table.contents, &bias, 4);
        [args setBuffer:table offset:0 atIndex:(NSUInteger)table_id];
    }
    id<MTLCommandQueue> queue = [dev newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [command computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBuffer:ab offset:0 atIndex:0];
    [enc useResource:tex usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    if (table) [enc useResource:table usage:MTLResourceUsageRead];
    [enc dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [enc endEncoding]; [command commit]; [command waitUntilCompleted];
    assert(command.status == MTLCommandBufferStatusCompleted);
    uint32_t out[1024];
    if (data) memcpy(out, data.contents, sizeof out);
    else if (tex.textureType == MTLTextureType2DArray)
        for (NSUInteger s = 0; s < L; s++) [tex getBytes:out + s * W * H bytesPerRow:W * 4 bytesPerImage:W * H * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 slice:s];
    else [tex getBytes:out bytesPerRow:W * 4 bytesPerImage:W * H * 4 fromRegion:MTLRegionMake3D(0, 0, 0, W, H, tex.depth) mipmapLevel:0 slice:0];
    assert(out[bias] == (uint32_t)atoi(argv[2]));
    for (NSUInteger i = 0; i < n; i++) assert(i == bias || out[i] == 3);
    printf("returning IGN atomic on a native %s texture (element offset %u): result and sentinel checks passed\n", shape, bias);
    return 0;
}}
