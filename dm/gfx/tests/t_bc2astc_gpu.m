// GPU BC1/BC2/BC3 → ASTC 4x4 transcoder (kernel `bca`, text from bc2astc_msl.h) against the C reference (nvn_bc2astc.c): the output must be byte-identical.
// Millions of blocks per format, mixed so that every layout and both swap directions are reached, through a pitched and offset source / destination; the bytes between the rows must stay untouched.
// Usage: t_bc2astc_gpu
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../tex/nvn_bc2astc.h"
#include "../tex/bc2astc_msl.h"

static uint64_t rng = 0x243F6A8885A308D3ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }

// A BC block of this kind (1/2/3), drawn from families that reach every layout: raw random, solid colour, uniform alpha, equal endpoints, extreme endpoints, c0 > c1 / c0 <= c1 forced
static void gen(int kind, uint8_t *b) {
    for (int i = 0; i < 16; i++) b[i] = (uint8_t)rnd();
    uint8_t *c = kind == 1 ? b : b + 8; uint32_t mode = rnd() % 12;
    uint32_t c0 = c[0] | c[1] << 8, c1 = c[2] | c[3] << 8;
    if (mode == 1) { c[4] = c[5] = c[6] = c[7] = (uint8_t)(0x55 * (rnd() & 3)); }                       // every colour index the same
    if (mode == 2) { c1 = c0; }                                                                          // equal endpoints
    if (mode == 3) { c0 = (rnd() & 1) ? 0 : 0xffff; c1 = (rnd() & 1) ? 0 : 0xffff; }                    // black / white endpoints
    if (mode == 4 && c0 < c1) { uint32_t t = c0; c0 = c1; c1 = t; }                                      // c0 > c1
    if (mode == 5 && c0 > c1) { uint32_t t = c0; c0 = c1; c1 = t; }                                      // c0 <= c1
    c[0] = (uint8_t)c0; c[1] = (uint8_t)(c0 >> 8); c[2] = (uint8_t)c1; c[3] = (uint8_t)(c1 >> 8);
    uint32_t am = rnd() % 8;
    if (kind == 2) {
        if (am == 1) { uint8_t n = (uint8_t)(rnd() & 15); for (int i = 0; i < 8; i++) b[i] = (uint8_t)(n | n << 4); }   // uniform alpha (including 15 = 255)
        if (am == 2) { for (int i = 0; i < 8; i++) b[i] = 0xff; }
    }
    if (kind == 3) {
        if (am == 1) { b[1] = b[0]; for (int i = 2; i < 8; i++) b[i] = 0; }                               // a0 == a1, every index 0
        if (am == 2) { b[0] = 255; b[1] = 255; for (int i = 2; i < 8; i++) b[i] = 0; }                    // opaque
        if (am == 3 && b[0] < b[1]) { uint8_t t = b[0]; b[0] = b[1]; b[1] = t; }                          // 8-level mode
    }
}

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); if (!dev) { printf("no Metal device\n"); return 1; }
        NSError *err = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:@(nvn_bca_msl) options:nil error:&err];
        if (!lib) { printf("MSL compile failed: %s\n", err.localizedDescription.UTF8String); return 1; }
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"bca"] error:&err];
        if (!ps) { printf("pipeline failed: %s\n", err.localizedDescription.UTF8String); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        int fails = 0, checks = 0;
        for (int kind = 1; kind <= 3; kind++) {
            const uint32_t nbx = 1000, nby = 600, n = nbx * nby, bb = kind == 1 ? 8 : 16;
            const uint32_t off_in = 48, pitch_in = nbx * bb + 16, off_out = 64, pitch_out = nbx * 16 + 32;   // pitched and offset on both sides
            size_t in_size = off_in + (size_t)pitch_in * nby + 32, out_size = off_out + (size_t)pitch_out * nby + 32;
            uint8_t *in = calloc(1, in_size), *out_ref = malloc(out_size);
            memset(out_ref, 0xCD, out_size);
            memset(in, 0xEE, in_size);
            long layouts[5] = {0}, swapped = 0, swap_by[5] = {0};
            for (uint32_t y = 0; y < nby; y++) for (uint32_t x = 0; x < nbx; x++) {
                uint8_t blk[16]; gen(kind, blk);
                memcpy(in + off_in + (size_t)y * pitch_in + (size_t)x * bb, blk, bb);
                uint32_t r = nvn_bc2astc_block(kind, blk, out_ref + off_out + (size_t)y * pitch_out + (size_t)x * 16);
                layouts[r & 0xff]++; if (r & 0x100) { swapped++; swap_by[r & 0xff]++; }
            }
            id<MTLBuffer> bin = [dev newBufferWithBytes:in length:in_size options:MTLResourceStorageModeShared];
            id<MTLBuffer> bout = [dev newBufferWithLength:out_size options:MTLResourceStorageModeShared]; memset(bout.contents, 0xCD, out_size);
            uint32_t p[7] = {off_in, pitch_in, off_out, pitch_out, nbx, nby, (uint32_t)kind};
            uint64_t t0 = now_ns();
            id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
            [e setComputePipelineState:ps]; [e setBuffer:bin offset:0 atIndex:0]; [e setBytes:p length:sizeof p atIndex:1]; [e setBuffer:bout offset:0 atIndex:2];
            [e dispatchThreads:MTLSizeMake(nbx, nby, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
            double ms = (now_ns() - t0) / 1e6;
            if (cb.error) { printf("BC%d: GPU error %s\n", kind, cb.error.localizedDescription.UTF8String); return 1; }
            const uint8_t *got = bout.contents; size_t bad = 0, first = (size_t)-1;
            for (size_t i = 0; i < out_size; i++) if (got[i] != out_ref[i]) { if (first == (size_t)-1) first = i; bad++; }
            checks++; if (bad) { fails++; printf("  ✗ BC%d: %zu of %zu bytes differ (first at %zu)\n", kind, bad, out_size, first); }
            // coverage: every layout the format can produce, and swaps in each layout that has endpoints to swap
            int want[5] = {1, 1, kind != 1, kind == 1, kind != 1};
            for (int l = 0; l < 5; l++) { if (!want[l]) continue; checks++; if (!layouts[l]) { fails++; printf("  ✗ BC%d: layout %d never reached\n", kind, l); } }
            for (int l = 1; l < 5; l++) { if (!want[l]) continue; checks++; if (!swap_by[l] || swap_by[l] == layouts[l]) { fails++; printf("  ✗ BC%d: layout %d swapped %ld of %ld blocks (need both)\n", kind, l, swap_by[l], layouts[l]); } }
            printf("BC%d: %u blocks, L0 %ld L1a %ld L1b %ld L2 %ld L3 %ld, %ld swapped, %s, %.1f ms (%.0f Mblocks/s)\n", kind, n, layouts[0], layouts[1], layouts[2], layouts[3], layouts[4], swapped, bad ? "DIFFERENT" : "byte-identical", ms, n / ms / 1e3);
            free(in); free(out_ref);
        }
        printf("%d checks, %d failed\n", checks, fails);
        return fails != 0;
    }
}
