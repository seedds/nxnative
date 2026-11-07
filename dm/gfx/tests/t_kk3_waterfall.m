// t_kk3_waterfall <lib.metallib>: runs t_kk3_waterfall.metal's kernel `wf` (PRAGMATA 965043fe's ballot waterfall, capped at 4096 iterations) on lane patterns, 2048 threads each.
// One line per pattern: "<name>: capped <lanes that reached the cap> bad <lanes whose item count is not the number of distinct keys in their SIMD group>". Bounded: at most
// 4096 iterations a lane, well under 100 ms of GPU time in all. test_dmsh_metalwa.py (DMSH_TEST_METAL=1) builds and runs it.
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) { @autoreleasepool {
    if (argc < 2) { fprintf(stderr, "usage: t_kk3_waterfall <lib.metallib>\n"); return 2; }
    id<MTLDevice> d = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q = [d newCommandQueue]; NSError *e = nil;
    id<MTLLibrary> lib = [d newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&e];
    if (!lib) { fprintf(stderr, "%s\n", e.description.UTF8String); return 1; }
    id<MTLComputePipelineState> p = [d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"wf"] error:&e];
    if (!p) { fprintf(stderr, "%s\n", e.description.UTF8String); return 1; }
    const char *names[] = {"uniform", "2 keys", "3 keys", "32 keys", "lane 0 inactive, 2 keys", "half inactive, 4 keys", "random 1-4 keys, random inactive"};
    srand(7);
    enum { G = 64 * 32 };
    for (int pat = 0; pat < 7; pat++) {
        uint32_t v[G], a[G];
        for (int i = 0; i < G; i++) {
            int l = i & 31; a[i] = 1;
            switch (pat) {
            case 0: v[i] = 0x40000005; break;
            case 1: v[i] = l < 7 ? 0x11 : 0x22; break;
            case 2: v[i] = 0x100 + (l % 3); break;
            case 3: v[i] = 0x1000 + l; break;
            case 4: v[i] = l & 1 ? 0x11 : 0x22; a[i] = l != 0; break;
            case 5: v[i] = l % 4; a[i] = l & 1; break;
            default: v[i] = rand() % (1 + (i / 32) % 4); a[i] = (rand() % 8) != 0; break;
            }
        }
        id<MTLBuffer> vb = [d newBufferWithBytes:v length:sizeof v options:0], ab = [d newBufferWithBytes:a length:sizeof a options:0], ob = [d newBufferWithLength:G * 16 options:0];
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:p]; [ce setBuffer:vb offset:0 atIndex:0]; [ce setBuffer:ob offset:0 atIndex:1]; [ce setBuffer:ab offset:0 atIndex:2];
        [ce dispatchThreads:MTLSizeMake(G, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)]; [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
        if (cb.error) { fprintf(stderr, "%s\n", cb.error.description.UTF8String); return 1; }
        const uint32_t *r = ob.contents; int capped = 0, bad = 0;
        for (int g = 0; g < G / 32; g++) {
            uint32_t seen[32]; int distinct = 0;
            for (int l = 0; l < 32; l++) {
                int i = g * 32 + l, k; if (!a[i]) continue;
                for (k = 0; k < distinct; k++) if (seen[k] == v[i]) break;
                if (k == distinct) seen[distinct++] = v[i];
            }
            for (int l = 0; l < 32; l++) {
                int i = g * 32 + l; if (!a[i]) continue;
                if (r[i * 4 + 2] >= 4096) capped++;
                if (r[i * 4 + 0] != (uint32_t)distinct) bad++;
            }
        }
        printf("%s: capped %d bad %d\n", names[pat], capped, bad);
    }
} return 0; }
