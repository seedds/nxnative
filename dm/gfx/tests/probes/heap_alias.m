// Texture aliasing on a Metal placement heap: which pairs of textures placed at the same offset read each other's bytes back as plain reinterpretation.
// A is filled through a blit from a known byte pattern, B is placed at A's offset with another descriptor and read back with a blit, and the bytes are compared with A's.
// "identical" = B's bytes equal the pattern (the two share one layout); otherwise the fraction of matching 4-byte words. Then the reverse: write B, read A.
// Build: clang -fobjc-arc -framework Metal -framework Foundation heap_alias.m -o heap_alias
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <stdio.h>

static id<MTLDevice> dev; static id<MTLCommandQueue> q;

typedef struct { const char *name; MTLPixelFormat pf; NSUInteger w, h, bpp; MTLTextureUsage usage; BOOL opt; } tdesc;

static MTLTextureDescriptor *td(tdesc t) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.pf width:t.w height:t.h mipmapped:NO];
    d.storageMode = MTLStorageModePrivate; d.usage = t.usage; d.allowGPUOptimizedContents = t.opt;
    return d;
}

static void fill(id<MTLTexture> t, NSUInteger bpp, uint32_t seed) {   // a pattern that differs in every word
    NSUInteger n = t.width * t.height * bpp; id<MTLBuffer> b = [dev newBufferWithLength:n options:MTLResourceStorageModeShared];
    uint32_t *w = b.contents; for (NSUInteger i = 0; i < n / 4; i++) w[i] = (uint32_t)(i * 2654435761u) ^ seed;
    if (t.pixelFormat == MTLPixelFormatDepth32Float) for (NSUInteger i = 0; i < n / 4; i++) { float f = (float)(i % 1000) / 1000.0f; memcpy(&w[i], &f, 4); }
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromBuffer:b sourceOffset:0 sourceBytesPerRow:t.width * bpp sourceBytesPerImage:n sourceSize:MTLSizeMake(t.width, t.height, 1) toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
}

static id<MTLBuffer> readback(id<MTLTexture> t, NSUInteger bpp) {
    NSUInteger n = t.width * t.height * bpp; id<MTLBuffer> b = [dev newBufferWithLength:n options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(t.width, t.height, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:t.width * bpp destinationBytesPerImage:n];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    return b;
}

// fraction of B's words that equal the pattern A was filled with, word for word (the plain reinterpretation of the same bytes)
static double match(id<MTLBuffer> got, NSUInteger words, NSUInteger awords, uint32_t seed, BOOL adepth) {
    const uint32_t *w = got.contents; NSUInteger ok = 0, n = MIN(words, awords);
    for (NSUInteger i = 0; i < n; i++) { uint32_t want = (uint32_t)(i * 2654435761u) ^ seed; if (adepth) { float f = (float)(i % 1000) / 1000.0f; memcpy(&want, &f, 4); } if (w[i] == want) ok++; }
    return n ? (double)ok / (double)n : 0;
}

// A written by the GPU as a render target (a clear in a render pass, as a game's pass writes it), with compression allowed or not; B placed at its offset reads it back
static void run_rp(tdesc a, tdesc b) {
    MTLSizeAndAlign sa = [dev heapTextureSizeAndAlignWithDescriptor:td(a)], sb = [dev heapTextureSizeAndAlignWithDescriptor:td(b)];
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new]; hd.type = MTLHeapTypePlacement; hd.storageMode = MTLStorageModePrivate; hd.size = MAX(sa.size, sb.size) + MAX(sa.align, sb.align);
    id<MTLHeap> heap = [dev newHeapWithDescriptor:hd];
    id<MTLTexture> ta = [heap newTextureWithDescriptor:td(a) offset:0];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = ta; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1.0);
    id<MTLCommandBuffer> cb = [q commandBuffer]; [[cb renderCommandEncoderWithDescriptor:rp] endEncoding]; [cb commit]; [cb waitUntilCompleted];
    id<MTLBuffer> ra = readback(ta, a.bpp);   // A's own view of its pixels
    id<MTLTexture> tb = [heap newTextureWithDescriptor:td(b) offset:0];
    id<MTLBuffer> rb = readback(tb, b.bpp);
    NSUInteger n = MIN(ra.length, rb.length) / 4, ok = 0; const uint32_t *x = ra.contents, *y = rb.contents; for (NSUInteger i = 0; i < n; i++) ok += x[i] == y[i];
    printf("%-52s render-pass clear of A | B reads A's pixels: %6.2f%%  (A's first word %08x, B's %08x)\n", [NSString stringWithFormat:@"%s -> %s", a.name, b.name].UTF8String, 100.0 * ok / n, x[0], y[0]);
}

static void run(tdesc a, tdesc b) {
    MTLSizeAndAlign sa = [dev heapTextureSizeAndAlignWithDescriptor:td(a)], sb = [dev heapTextureSizeAndAlignWithDescriptor:td(b)];
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new]; hd.type = MTLHeapTypePlacement; hd.storageMode = MTLStorageModePrivate;
    hd.size = MAX(sa.size, sb.size) + MAX(sa.align, sb.align);
    id<MTLHeap> heap = [dev newHeapWithDescriptor:hd];
    id<MTLTexture> ta = [heap newTextureWithDescriptor:td(a) offset:0];
    fill(ta, a.bpp, 0x1234567);
    id<MTLTexture> tb = [heap newTextureWithDescriptor:td(b) offset:0];
    double fwd = match(readback(tb, b.bpp), b.w * b.h * b.bpp / 4, a.w * a.h * a.bpp / 4, 0x1234567, a.pf == MTLPixelFormatDepth32Float);
    fill(tb, b.bpp, 0x7654321);
    double rev = match(readback(ta, a.bpp), a.w * a.h * a.bpp / 4, b.w * b.h * b.bpp / 4, 0x7654321, b.pf == MTLPixelFormatDepth32Float);
    printf("%-52s A %8lu B %8lu bytes | B reads A: %6.2f%% | A reads B: %6.2f%%%s\n", [NSString stringWithFormat:@"%s -> %s", a.name, b.name].UTF8String,
           (unsigned long)sa.size, (unsigned long)sb.size, 100 * fwd, 100 * rev, fwd > 0.999 && rev > 0.999 ? "  identical layout" : "");
}

int main(void) { @autoreleasepool {
    dev = MTLCreateSystemDefaultDevice(); q = [dev newCommandQueue];
    printf("device %s, placement heaps %s\n", dev.name.UTF8String, [dev supportsFamily:MTLGPUFamilyApple7] ? "yes" : "?");
    const MTLTextureUsage RT = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, SR = MTLTextureUsageShaderRead;
    tdesc rgba = {"RGBA8 512x512 RT", MTLPixelFormatRGBA8Unorm, 512, 512, 4, RT, NO};
    tdesc rgbaOpt = {"RGBA8 512x512 RT opt", MTLPixelFormatRGBA8Unorm, 512, 512, 4, RT, YES};
    tdesc rgbaSR = {"RGBA8 512x512 read-only", MTLPixelFormatRGBA8Unorm, 512, 512, 4, SR, NO};
    tdesc r32u = {"R32Uint 512x512 RT", MTLPixelFormatR32Uint, 512, 512, 4, RT, NO};
    tdesc r32f = {"R32Float 512x512 RT", MTLPixelFormatR32Float, 512, 512, 4, RT, NO};
    tdesc d32 = {"Depth32Float 512x512 RT", MTLPixelFormatDepth32Float, 512, 512, 4, RT, NO};
    tdesc rgba256 = {"RGBA8 256x512 RT", MTLPixelFormatRGBA8Unorm, 256, 512, 4, RT, NO};
    tdesc rgba1536 = {"RGBA8 1536x864 RT", MTLPixelFormatRGBA8Unorm, 1536, 864, 4, RT, NO};
    tdesc rgba1535 = {"RGBA8 1535x863 RT", MTLPixelFormatRGBA8Unorm, 1535, 863, 4, RT, NO};
    tdesc rgba16f = {"RGBA16F 512x256 RT", MTLPixelFormatRGBA16Float, 512, 256, 8, RT, NO};
    run(rgba, rgba);          // the same descriptor again: the reuse the plan relies on
    run(rgbaOpt, rgbaOpt);    // the same, with GPU-optimized (compressible) contents
    run(rgba, rgbaSR);        // same format and size, other usage
    run(rgba, r32u);          // same size and bytes per pixel, other format
    run(rgba, r32f);
    run(r32f, d32);           // colour against depth
    run(d32, d32);            // depth reused as itself
    run(rgba, rgba256);       // other width
    run(rgba1536, rgba1535);  // a dynamic-resolution step of one pixel
    run(rgba, rgba16f);       // same byte count, other format and shape
    run(rgba, rgbaOpt);       // compression on one side only
    printf("-- A written by a render pass --\n");
    run_rp(rgba, rgba); run_rp(rgbaOpt, rgbaOpt); run_rp(rgbaOpt, rgba); run_rp(rgba, rgbaOpt); run_rp(rgbaOpt, r32u);
    tdesc big = {"RGBA8 1536x864 RT opt", MTLPixelFormatRGBA8Unorm, 1536, 864, 4, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, YES};
    run_rp(big, big);
} return 0; }
