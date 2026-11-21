// Render-target size census for Ocarina's dynamic resolution: what Metal allocates for each step of a 16:9 target between 0.333 and 0.75 of a
// 2048x1152 final image (the docked tier-1 picture the host presents), per format. No game: only heapTextureSizeAndAlign and a few real allocations.
// Build: clang -fobjc-arc -framework Metal -framework Foundation rt_sizes.m -o rt_sizes
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <stdio.h>

typedef struct { const char *name; MTLPixelFormat pf; } fmt;
static fmt F[] = {{"RGBA8", MTLPixelFormatRGBA8Unorm}, {"RGBA16F", MTLPixelFormatRGBA16Float}, {"RG11B10F", MTLPixelFormatRG11B10Float}, {"RGB9E5", MTLPixelFormatRGB9E5Float},
                  {"Depth32F", MTLPixelFormatDepth32Float}, {"R8", MTLPixelFormatR8Unorm}, {"R16F", MTLPixelFormatR16Float}, {"RG8", MTLPixelFormatRG8Unorm}, {"R32F", MTLPixelFormatR32Float}};
static const double PAL[] = {0.75, 0.71112, 0.66667, 0.62223, 0.58334, 0.53334, 0.5, 0.4445, 0.37223, 0.33334};

static MTLTextureDescriptor *td(MTLPixelFormat pf, NSUInteger w, NSUInteger h, int rt) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageShaderRead | (rt ? MTLTextureUsageRenderTarget : 0);
    return d;
}

int main(int argc, char **argv) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    NSUInteger BW = argc > 1 ? (NSUInteger)atoi(argv[1]) : 2048, BH = argc > 2 ? (NSUInteger)atoi(argv[2]) : 1152;
    printf("device %s, final image %lux%lu, working set %llu MB\n", dev.name.UTF8String, (unsigned long)BW, (unsigned long)BH, (unsigned long long)(dev.recommendedMaxWorkingSetSize >> 20));
    printf("%-9s %10s %10s %10s %10s %10s %8s %10s\n", "format", "largest", "pal10 sum", "all steps", "distinct", "steps", "n", "alloc/px");
    for (size_t f = 0; f < sizeof F / sizeof *F; f++) {
        NSUInteger lo = (NSUInteger)(BW * 0.33334), hi = (NSUInteger)(BW * 0.75);
        unsigned long long all = 0, largest = 0, pal = 0; NSMutableSet *distinct = [NSMutableSet set]; unsigned n = 0, changes = 0; unsigned long long last = 0;
        for (NSUInteger w = lo; w <= hi; w++) {
            NSUInteger h = (w * BH + BW / 2) / BW;
            MTLSizeAndAlign s = [dev heapTextureSizeAndAlignWithDescriptor:td(F[f].pf, w, h, 1)];
            all += s.size; n++; if (s.size > largest) largest = s.size; [distinct addObject:@(s.size)]; if (s.size != last) changes++; last = s.size;
        }
        for (size_t p = 0; p < 10; p++) { NSUInteger w = (NSUInteger)(BW * PAL[p]), h = (w * BH + BW / 2) / BW; pal += [dev heapTextureSizeAndAlignWithDescriptor:td(F[f].pf, w, h, 1)].size; }
        id<MTLTexture> t = [dev newTextureWithDescriptor:td(F[f].pf, hi, (hi * BH + BW / 2) / BW, 1)];
        printf("%-9s %7.1f MB %7.1f MB %7.1f MB %10lu %10u %8u %10.2f\n", F[f].name, largest / 1048576.0, pal / 1048576.0, all / 1048576.0, (unsigned long)distinct.count, changes, n,
               (double)t.allocatedSize / (double)(t.width * t.height));
    }
    // Allocation granularity: consecutive widths and what one pixel step costs
    printf("\nRGBA8 steps around 0.75: ");
    for (NSUInteger w = (NSUInteger)(BW * 0.75) - 6; w <= (NSUInteger)(BW * 0.75); w++) { NSUInteger h = (w * BH + BW / 2) / BW; printf("%lux%lu=%llu ", (unsigned long)w, (unsigned long)h, (unsigned long long)[dev heapTextureSizeAndAlignWithDescriptor:td(MTLPixelFormatRGBA8Unorm, w, h, 1)].size); }
    printf("\n");
    // Compressible vs not, render target vs sampled only
    for (int rt = 0; rt < 2; rt++) for (int opt = 0; opt < 2; opt++) {
        MTLTextureDescriptor *d = td(MTLPixelFormatRGBA8Unorm, 1536, 864, rt); d.allowGPUOptimizedContents = opt;
        printf("RGBA8 1536x864 rt=%d gpuOptimized=%d: heap size %llu align %llu\n", rt, opt, (unsigned long long)[dev heapTextureSizeAndAlignWithDescriptor:d].size, (unsigned long long)[dev heapTextureSizeAndAlignWithDescriptor:d].align);
    }
    return 0;
} }
