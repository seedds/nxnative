// Compare offline-compiled original and merged pixel shaders on identical GPU inputs.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

static id<MTLFunction> function(id<MTLLibrary> lib, NSString *name) {
    id<MTLFunction> f = [lib newFunctionWithName:name];
    if (f.functionConstantsDictionary.count) f = [lib newFunctionWithName:name constantValues:[MTLFunctionConstantValues new] error:nil];
    return f;
}
int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) return 2;
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> queue = [dev newCommandQueue];
    const int width = 32, height = 32;
    id<MTLBuffer> inputs = [dev newBufferWithLength:1024 * sizeof(float) options:MTLResourceStorageModeShared];
    float *v = inputs.contents; uint32_t rng = 7;
    for (int i = 0; i < 1024; i++) { rng = rng * 1664525u + 1013904223u; v[i] = ((int)(rng >> 8) - 8388608) / 1048576.f; }
    float edge[] = {0, -0.f, 1, -1, 1e-12f, 1e12f, INFINITY, -INFINITY, NAN};
    for (int i = 0; i < 9; i++) v[i] = edge[i];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:16 height:16 mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
    [tex replaceRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0 withBytes:v bytesPerRow:16 * 16];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    id<MTLSamplerState> sampler = [dev newSamplerStateWithDescriptor:sd];
    NSError *vertex_error = nil;
    id<MTLLibrary> vertex_lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithFormat:@"%s/vertex.metallib", argv[1]]] error:&vertex_error];
    if (!vertex_lib) { fprintf(stderr, "%s\n", vertex_error.localizedDescription.UTF8String); return 2; }
    uint64_t compared = 0, exact = 0, failed = 0; double max_error = 0;
    for (int fixture = 0; fixture < 9; fixture++) {
        int integer = fixture == 8;
        MTLPixelFormat format = integer ? MTLPixelFormatRGBA32Uint : MTLPixelFormatRGBA32Float;
        id<MTLBuffer> pixels[3];
        for (int variant = 0; variant < 3; variant++) {
            NSError *error = nil;
            NSString *path = [NSString stringWithFormat:@"%s/%d_%d.metallib", argv[1], fixture, variant];
            id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:path] error:&error];
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = function(vertex_lib, @"vertex_main"); pd.fragmentFunction = function(lib, @"main0");
            pd.colorAttachments[0].pixelFormat = format;
            id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&error];
            if (!pso) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
            td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
            td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageRenderTarget;
            id<MTLTexture> target = [dev newTextureWithDescriptor:td];
            pixels[variant] = [dev newBufferWithLength:width * height * 16 options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = [queue commandBuffer]; MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new];
            rp.colorAttachments[0].texture = target; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp]; [enc setRenderPipelineState:pso];
            [enc setFragmentBuffer:inputs offset:0 atIndex:0]; [enc setFragmentTexture:tex atIndex:0]; [enc setFragmentSamplerState:sampler atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [enc endEncoding];
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(width, height, 1) toBuffer:pixels[variant] destinationOffset:0 destinationBytesPerRow:width * 16 destinationBytesPerImage:width * height * 16];
            [blit endEncoding]; [cb commit]; [cb waitUntilCompleted];
            if (cb.status == MTLCommandBufferStatusError) { fprintf(stderr, "%s\n", cb.error.localizedDescription.UTF8String); return 2; }
        }
        for (int variant = 1; variant < 3; variant++) {
        uint32_t *a = pixels[0].contents, *b = pixels[variant].contents;
        for (int i = 0; i < width * height * 4; i++) {
            compared++; if (a[i] == b[i]) { exact++; continue; }
            float x, y; memcpy(&x, a + i, 4); memcpy(&y, b + i, 4);
            double error = fabs((double)x - y), limit = 1e-6 + 1e-5 * fmax(fabs(x), fabs(y));
            int same = !integer && ((isnan(x) && isnan(y)) || (isfinite(x) && isfinite(y) && error <= limit));
            if (isfinite(error)) max_error = fmax(max_error, error);
            if (!same && ++failed < 8) fprintf(stderr, "fixture %d word %d: %08x vs %08x (%g vs %g)\n", fixture, i, a[i], b[i], x, y);
        }
    }
    }
    printf("merged shader numerical comparison: %llu components, %llu bit-identical, %llu outside tolerance; max absolute error %.9g\n", (unsigned long long)compared, (unsigned long long)exact, (unsigned long long)failed, max_error);
    return failed ? 1 : 0;
} }
