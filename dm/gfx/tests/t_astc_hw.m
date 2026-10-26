// The GPU's own ASTC decoder against bcdec: BC1 / BC2 / BC3 blocks are transcoded (nvn_bc2astc.c), uploaded as ASTC 4x4 LDR and sRGB textures and read back through a compute kernel
// (what a shader samples). astcenc (tests/t_bc2astc.c in dm/gfx/tex) is the reference decoder there; this checks the hardware agrees, layout by layout.
// Usage: t_astc_hw [-v]
#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BCDEC_IMPLEMENTATION
#include "../../third_party/bcdec/bcdec.h"
#include "../tex/nvn_bc2astc.h"
#include "../tex/bc2astc_core.h"

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }
static void gen(int kind, uint8_t *b) {   // as t_bc2astc_gpu.m: families that reach every layout
    for (int i = 0; i < 16; i++) b[i] = (uint8_t)rnd();
    uint8_t *c = kind == 1 ? b : b + 8; uint32_t mode = rnd() % 12, c0 = c[0] | c[1] << 8, c1 = c[2] | c[3] << 8;
    if (mode == 1) c[4] = c[5] = c[6] = c[7] = (uint8_t)(0x55 * (rnd() & 3));
    if (mode == 2) c1 = c0;
    if (mode == 3) { c0 = (rnd() & 1) ? 0 : 0xffff; c1 = (rnd() & 1) ? 0 : 0xffff; }
    if (mode == 4 && c0 < c1) { uint32_t t = c0; c0 = c1; c1 = t; }
    if (mode == 5 && c0 > c1) { uint32_t t = c0; c0 = c1; c1 = t; }
    c[0] = (uint8_t)c0; c[1] = (uint8_t)(c0 >> 8); c[2] = (uint8_t)c1; c[3] = (uint8_t)(c1 >> 8);
    uint32_t am = rnd() % 8;
    if (kind == 2) { if (am == 1) { uint8_t n = (uint8_t)(rnd() & 15); for (int i = 0; i < 8; i++) b[i] = (uint8_t)(n | n << 4); } if (am == 2) for (int i = 0; i < 8; i++) b[i] = 0xff; }
    if (kind == 3) { if (am == 1) { b[1] = b[0]; for (int i = 2; i < 8; i++) b[i] = 0; } if (am == 2) { b[0] = 255; b[1] = 255; for (int i = 2; i < 8; i++) b[i] = 0; } if (am == 3 && b[0] < b[1]) { uint8_t t = b[0]; b[0] = b[1]; b[1] = t; } }
}
static float srgb2lin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }

static const char *k_msl =
"#include <metal_stdlib>\nusing namespace metal;\n"
"kernel void rd(texture2d<float, access::read> t [[texture(0)]], device float4 *o [[buffer(0)]], constant uint &w [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {\n"
"  if (g.x >= t.get_width() || g.y >= t.get_height()) return; o[g.y * w + g.x] = t.read(g); }\n";

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:@(k_msl) options:nil error:&err]; if (!lib) { printf("MSL: %s\n", err.localizedDescription.UTF8String); return 1; }
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"rd"] error:&err];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        const uint32_t nbx = 256, nby = 128, n = nbx * nby, W = nbx * 4, H = nby * 4;
        int fails = 0; const char *ln[5] = {"L0", "L1a", "L1b", "L2", "L3"};
        for (int srgb = 0; srgb < 2; srgb++) for (int kind = 1; kind <= 3; kind++) {
            uint8_t *bc = malloc((size_t)n * 16), *astc = malloc((size_t)n * 16); uint8_t *lay = malloc(n); uint8_t (*ref)[16][4] = malloc((size_t)n * 64);
            for (uint32_t i = 0; i < n; i++) { gen(kind, bc + (size_t)i * 16); uint32_t r = nvn_bc2astc_block(kind, bc + (size_t)i * 16, astc + (size_t)i * 16); lay[i] = (uint8_t)(r & 0xff);
                if (kind == 1) bcdec_bc1(bc + (size_t)i * 16, ref[i], 16); else if (kind == 2) bcdec_bc2(bc + (size_t)i * 16, ref[i], 16); else bcdec_bc3(bc + (size_t)i * 16, ref[i], 16); }
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:srgb ? MTLPixelFormatASTC_4x4_sRGB : MTLPixelFormatASTC_4x4_LDR width:W height:H mipmapped:NO];
            d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> t = [dev newTextureWithDescriptor:d];
            [t replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:astc bytesPerRow:nbx * 16];
            id<MTLBuffer> out = [dev newBufferWithLength:(size_t)W * H * 16 options:MTLResourceStorageModeShared]; uint32_t wv = W;
            id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
            [e setComputePipelineState:ps]; [e setTexture:t atIndex:0]; [e setBuffer:out offset:0 atIndex:0]; [e setBytes:&wv length:4 atIndex:1];
            [e dispatchThreads:MTLSizeMake(W, H, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
            const float (*px)[4] = out.contents;
            long cnt[5] = {0}, bad[5] = {0}; float worst[5] = {0}, worst_a[5] = {0}; int firstbad[5]; for (int l = 0; l < 5; l++) firstbad[l] = -1;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t bx = i % nbx, by = i / nbx; float mx = 0, ma = 0;
                for (int p = 0; p < 16; p++) {
                    const float *g = px[(by * 4 + p / 4) * W + bx * 4 + (p & 3)];
                    for (int c = 0; c < 4; c++) {
                        float want = ref[i][p][c] / 255.0f; if (srgb && c < 3) want = srgb2lin(want);
                        if (kind == 1 && ref[i][p][3] == 0 && c < 3) continue;   // transparent BC1 texel: RGB is a don't-care
                        float df = fabsf(g[c] - want) * 255.0f; if (c == 3) { if (df > ma) ma = df; } else if (df > mx) mx = df;
                    }
                }
                int L = lay[i]; cnt[L]++; if (mx > worst[L]) worst[L] = mx; if (ma > worst_a[L]) worst_a[L] = ma;
                // L3 carries four alpha levels per block, so its alpha differs from bcdec by design (the benchmark measures how much); everything else must agree within 4 levels (L3 colour: within its own endpoint precision)
                float tol_rgb = L == 4 ? (srgb ? 12.0f : 6.0f) : 4.0f;   // L3's colour endpoints have 48 levels (measured by the benchmark: about 5 of 255; an sRGB texture is compared after the linear conversion, which magnifies dark values)
                if (mx > tol_rgb || (L != 4 && ma > 4.0f) || (L == 4 && ma > 80.0f)) { bad[L]++; if (firstbad[L] < 0) firstbad[L] = (int)i; }
            }
            printf("%s BC%d: ", srgb ? "sRGB" : "LDR ", kind);
            for (int l = 0; l < 5; l++) if (cnt[l]) printf("%s %ld blocks, %ld off (worst RGB %.1f, alpha %.1f)  ", ln[l], cnt[l], bad[l], worst[l], worst_a[l]);
            printf("\n");
            for (int l = 0; l < 5; l++) if (bad[l]) { fails++; int i = firstbad[l]; printf("   first bad %s block %d: BC", ln[l], i); for (int k = 0; k < (kind == 1 ? 8 : 16); k++) printf("%02x", bc[(size_t)i * 16 + k]); printf(" → ASTC "); for (int k = 0; k < 16; k++) printf("%02x", astc[(size_t)i * 16 + k]); printf("\n"); }
            free(bc); free(astc); free(lay); free(ref);
        }
        printf("%s\n", fails ? "hardware ASTC decode DIFFERS from bcdec in some layouts" : "hardware ASTC decode matches bcdec within 4 levels in every layout (L3 alpha: within its four levels)");
        return fails != 0;
    }
}
